/**
 * local_infer 的推理调用层：OpenAI 兼容端点的请求构造、响应解析、**结构化输出**
 * （JSON 抽取 + JSON Schema 轻量校验 + 失败重试）与批量调度（限流 worker pool、逐条回调、重试）。
 *
 * 设计要点：
 *   * 只依赖 fetch（node 内建 / Bun 内建），不引入任何第三方依赖——可在无 GPU 的 CI/开发机上用
 *     mock 端点完整验证（见 api.test.ts / mock-server.test.ts）。
 *   * 结构性约束优先交给服务端（llama.cpp 把 response_format.json_schema 转 GBNF），工具侧只做
 *     「解析 + 校验 + 重试」——不自行实现 schema→GBNF 转换（易错且无必要）；`grammar` 直传作后手。
 *   * 推理型模型的正文在 `content`、思维链在 `reasoning_content`，两者都返回（只取 content 会把
 *     正常的思维链输出误判为空响应）。
 */

// ── 请求/响应类型 ─────────────────────────────────────────────────────────

export interface ChatMessage {
  role: "system" | "user" | "assistant" | "tool"
  content: string
  name?: string
}

/** 结构化输出约束（三选一，优先级 schema > grammar > json_object）。 */
export interface StructuredOutput {
  /** JSON Schema：服务端转 GBNF 约束解码（推荐）。 */
  schema?: Record<string, unknown>
  schema_name?: string
  /** GBNF 语法文本直传（llama.cpp 原生字段）。注意与 `-bs` 后端采样不兼容——引擎会自动回退 CPU 采样。 */
  grammar?: string
  /** 仅要求「合法 JSON」，不限定结构（最弱约束，作为 schema 不可用时的手动降级）。 */
  json_object?: boolean
  /** 校验失败时把错误回灌给模型再试一次（默认 true）。 */
  retry_on_invalid?: boolean
}

/** 采样与模板调参（单条请求与批量条目共用）。 */
export interface InferTuning {
  temperature?: number
  top_p?: number
  max_tokens?: number
  seed?: number
  stop?: string[]
  /** 关闭思维链（本模型思维链可占输出 90%+；推理型任务才开）。三态：undefined=服务端默认。 */
  enable_thinking?: boolean
}

export interface InferRequest extends InferTuning {
  messages?: ChatMessage[]
  /** 单轮 prompt（与 messages 二选一；给出时作为 user 消息）。 */
  prompt?: string
  /** 系统提示（与 messages 二选一；给出时置前）。 */
  system?: string
  structured?: StructuredOutput
}

export interface InferUsage {
  prompt_tokens?: number
  completion_tokens?: number
  total_tokens?: number
}

export interface InferTimings {
  prompt_n?: number
  prompt_ms?: number
  predicted_n?: number
  predicted_ms?: number
  [k: string]: number | undefined
}

export interface InferResponse {
  ok: boolean
  content: string
  reasoning: string
  /** 结构化结果（结构化约束生效且解析校验通过时）。 */
  json?: unknown
  /** 被解析的原始 JSON 文本。 */
  json_raw?: string
  /** 结构化校验/解析错误（ok=true 但有错误 = 解析成功而 schema 校验未通过）。 */
  json_errors?: string[]
  /** 结构化来源；`none` = 未启用结构化输出。 */
  structured_via?: "schema" | "grammar" | "json_object" | "none"
  usage?: InferUsage
  timings?: InferTimings
  /** 实测解码速度（token/s）：服务端 timings 优先，缺失时按墙钟估算。 */
  decode_tps?: number
  http_status?: number
  error?: string
  /** 降级提示（如服务端拒绝结构化约束后自动改为 json_object）。 */
  warning?: string
  attempts: number
  elapsed_ms: number
}

export interface ChatOptions {
  baseUrl: string
  timeoutMs?: number
  signal?: AbortSignal
  fetchImpl?: typeof fetch
  /** 额外请求头（远端 OpenAI 兼容端点鉴权用，如 `Authorization: Bearer <key>`）。 */
  headers?: Record<string, string>
}

// ── 请求体构造 ────────────────────────────────────────────────────────────

/** 归一化消息列表：messages 优先，其次 system + prompt。 */
export function normalizeMessages(req: InferRequest): ChatMessage[] {
  const out: ChatMessage[] = []
  if (req.messages?.length) {
    out.push(...req.messages.map((m) => ({ role: m.role, content: String(m.content ?? ""), ...(m.name ? { name: m.name } : {}) })))
    return out
  }
  if (req.system) out.push({ role: "system", content: req.system })
  if (req.prompt != null) out.push({ role: "user", content: String(req.prompt) })
  return out
}

/** 组装 OpenAI 兼容请求体（llama-server 语义）。 */
export function buildRequestBody(req: InferRequest): Record<string, unknown> {
  const body: Record<string, unknown> = { messages: normalizeMessages(req), temperature: req.temperature ?? 0 }
  if (req.top_p != null) body.top_p = req.top_p
  // llama.cpp 两种写法都认：max_tokens（OpenAI 兼容）与 n_predict（原生）
  if (req.max_tokens != null) {
    body.max_tokens = req.max_tokens
    body.n_predict = req.max_tokens
  }
  if (req.seed != null) body.seed = req.seed
  if (req.stop?.length) body.stop = req.stop
  if (req.enable_thinking != null) body.chat_template_kwargs = { enable_thinking: req.enable_thinking }

  const s = req.structured
  if (s) {
    if (s.schema) {
      body.response_format = {
        type: "json_schema",
        json_schema: { name: s.schema_name ?? "result", schema: s.schema, strict: true },
      }
    } else if (s.grammar) {
      body.grammar = s.grammar
    } else if (s.json_object) {
      body.response_format = { type: "json_object" }
    }
  }
  return body
}

/** 结构化约束来源（与请求体一致；无约束为 none）。 */
export function structuredVia(req: InferRequest): "schema" | "grammar" | "json_object" | "none" {
  const s = req.structured
  if (!s) return "none"
  if (s.schema) return "schema"
  if (s.grammar) return "grammar"
  if (s.json_object) return "json_object"
  return "none"
}

// ── 结构化解析 ────────────────────────────────────────────────────────────

/** 从模型输出中抽取 JSON：剥离 ``` 围栏、跳过前后噪声，取第一个括号配平的 JSON 值。 */
export function extractJson(text: string): { raw: string; value?: unknown; error?: string } {
  const src = String(text ?? "").trim()
  if (!src) return { raw: "", error: "输出为空" }

  const candidates: string[] = []
  const fence = src.match(/```(?:json|JSON)?\s*([\s\S]*?)```/)
  if (fence?.[1]) candidates.push(fence[1].trim())
  candidates.push(src)

  // 括号配平扫描（跳过字符串内部与转义）
  const scan = (s: string, openIdx: number): string | null => {
    const open = s[openIdx]
    const close = open === "{" ? "}" : "]"
    let depth = 0
    let inStr = false
    let esc = false
    for (let i = openIdx; i < s.length; i++) {
      const c = s[i]
      if (inStr) {
        if (esc) esc = false
        else if (c === "\\") esc = true
        else if (c === '"') inStr = false
        continue
      }
      if (c === '"') inStr = true
      else if (c === open) depth++
      else if (c === close) {
        depth--
        if (depth === 0) return s.slice(openIdx, i + 1)
      }
    }
    return null
  }

  for (const cand of candidates) {
    try {
      return { raw: cand, value: JSON.parse(cand) }
    } catch {
      /* 继续尝试抽取 */
    }
    for (let i = 0; i < cand.length; i++) {
      const c = cand[i]
      if (c !== "{" && c !== "[") continue
      const slice = scan(cand, i)
      if (!slice) break
      try {
        return { raw: slice, value: JSON.parse(slice) }
      } catch {
        /* 该起点的片段不合法，继续找下一个起点 */
      }
    }
  }
  return { raw: src, error: "未找到可解析的 JSON" }
}

function typeOf(v: unknown): string {
  if (v === null) return "null"
  if (Array.isArray(v)) return "array"
  if (typeof v === "number") return Number.isInteger(v) ? "integer" : "number"
  return typeof v
}

function typeMatches(v: unknown, want: string): boolean {
  if (want === "number") return typeof v === "number"
  if (want === "integer") return typeof v === "number" && Number.isInteger(v)
  if (want === "array") return Array.isArray(v)
  if (want === "null") return v === null
  if (want === "object") return typeof v === "object" && v !== null && !Array.isArray(v)
  return typeof v === want
}

/**
 * 轻量 JSON Schema 校验（覆盖 type/required/properties/items/enum/const/anyOf/oneOf/数值与长度边界）。
 * 只做「模型输出是否满足约定」的实用校验，不是完整实现（additionalProperties、$ref、format 等忽略）。
 * 返回错误消息清单（空数组 = 通过）。
 */
export function validateAgainstSchema(value: unknown, schema: Record<string, unknown> | undefined, path = "$"): string[] {
  const errors: string[] = []
  if (!schema || typeof schema !== "object") return errors

  const anyOf = (schema.anyOf ?? schema.oneOf) as Array<Record<string, unknown>> | undefined
  if (Array.isArray(anyOf) && anyOf.length) {
    const passes = anyOf.filter((sub) => validateAgainstSchema(value, sub, path).length === 0)
    const min = schema.oneOf ? 1 : 1
    if (passes.length < min) errors.push(`${path}: 不满足 anyOf/oneOf 任一分支`)
    return errors
  }

  if (schema.const !== undefined && JSON.stringify(value) !== JSON.stringify(schema.const)) {
    errors.push(`${path}: 期望常量 ${JSON.stringify(schema.const)}`)
  }
  if (Array.isArray(schema.enum) && !schema.enum.some((e) => JSON.stringify(e) === JSON.stringify(value))) {
    errors.push(`${path}: 不在枚举 ${JSON.stringify(schema.enum)} 内`)
  }

  const want = schema.type as string | string[] | undefined
  if (want) {
    const list = Array.isArray(want) ? want : [want]
    const ok = list.some((t) => typeMatches(value, t))
    if (!ok) {
      errors.push(`${path}: 类型应为 ${list.join("|")}，实际 ${typeOf(value)}`)
      return errors
    }
  }

  if (typeof value === "number") {
    if (typeof schema.minimum === "number" && value < schema.minimum) errors.push(`${path}: 小于 minimum ${schema.minimum}`)
    if (typeof schema.maximum === "number" && value > schema.maximum) errors.push(`${path}: 大于 maximum ${schema.maximum}`)
  }
  if (typeof value === "string") {
    if (typeof schema.minLength === "number" && value.length < schema.minLength) errors.push(`${path}: 长度小于 minLength ${schema.minLength}`)
    if (typeof schema.maxLength === "number" && value.length > schema.maxLength) errors.push(`${path}: 长度大于 maxLength ${schema.maxLength}`)
    if (typeof schema.pattern === "string") {
      try {
        if (!new RegExp(schema.pattern).test(value)) errors.push(`${path}: 不匹配 pattern ${schema.pattern}`)
      } catch {
        /* 非法 pattern 忽略 */
      }
    }
  }

  if (Array.isArray(value)) {
    if (typeof schema.minItems === "number" && value.length < schema.minItems) errors.push(`${path}: 元素数少于 minItems ${schema.minItems}`)
    if (typeof schema.maxItems === "number" && value.length > schema.maxItems) errors.push(`${path}: 元素数多于 maxItems ${schema.maxItems}`)
    const items = schema.items as Record<string, unknown> | undefined
    if (items) value.forEach((v, i) => errors.push(...validateAgainstSchema(v, items, `${path}[${i}]`)))
  }

  if (typeof value === "object" && value !== null && !Array.isArray(value)) {
    const obj = value as Record<string, unknown>
    const required = schema.required as string[] | undefined
    if (Array.isArray(required)) {
      for (const k of required) if (!(k in obj)) errors.push(`${path}.${k}: 缺少必需字段`)
    }
    const props = schema.properties as Record<string, Record<string, unknown>> | undefined
    if (props) {
      for (const [k, sub] of Object.entries(props)) {
        if (k in obj) errors.push(...validateAgainstSchema(obj[k], sub, `${path}.${k}`))
      }
    }
  }
  return errors
}

// ── 单条推理 ──────────────────────────────────────────────────────────────

interface RawChoice {
  message?: { content?: string; reasoning_content?: string; role?: string }
  text?: string
  finish_reason?: string
}

interface RawResponse {
  choices?: RawChoice[]
  content?: string
  usage?: InferUsage
  timings?: InferTimings
  error?: { message?: string } | string
}

/** 组合超时与外部取消信号：任一触发即中止请求（本地推理很慢，超时按外呼数倍给）。 */
function withTimeout(timeoutMs: number, outer?: AbortSignal): { signal: AbortSignal; cleanup: () => void } {
  const ac = new AbortController()
  const onAbort = () => ac.abort(outer?.reason)
  if (outer) {
    if (outer.aborted) ac.abort(outer.reason)
    else outer.addEventListener("abort", onAbort, { once: true })
  }
  const timer = setTimeout(() => ac.abort(new Error(`请求超时（${timeoutMs} ms）`)), Math.max(1, timeoutMs))
  return {
    signal: ac.signal,
    cleanup: () => {
      clearTimeout(timer)
      outer?.removeEventListener("abort", onAbort)
    },
  }
}

/** 从错误响应体中提取可读信息（llama-server 用 {error:{message}}）。 */
function errorText(status: number, body: string): string {
  try {
    const j = JSON.parse(body) as { error?: { message?: string } | string; message?: string }
    if (typeof j.error === "string") return j.error
    if (j.error?.message) return j.error.message
    if (j.message) return j.message
  } catch {
    /* 非 JSON 错误体 */
  }
  return `HTTP ${status}: ${body.slice(0, 300)}`
}

/** 解码速度：服务端 timings 优先（剔除 HTTP 与排队开销），缺失时按墙钟估算。 */
function decodeTps(timings: InferTimings | undefined, usage: InferUsage | undefined, elapsedMs: number): number | undefined {
  const n = timings?.predicted_n ?? usage?.completion_tokens
  if (!n) return undefined
  const ms = timings?.predicted_ms
  if (ms && ms > 0) return n / (ms / 1000)
  return elapsedMs > 0 ? n / (elapsedMs / 1000) : undefined
}

/** 发一次 chat 请求（含结构化解析与「无效则回灌重试」；服务端不支持约束时自动降级 json_object）。 */
export async function chatOnce(req: InferRequest, opts: ChatOptions): Promise<InferResponse> {
  const fetchImpl = opts.fetchImpl ?? fetch
  const timeoutMs = opts.timeoutMs ?? 600000
  const url = `${opts.baseUrl.replace(/\/+$/, "")}/v1/chat/completions`
  const t0 = Date.now()
  const retryInvalid = req.structured?.retry_on_invalid !== false

  let via = structuredVia(req)
  let messages = normalizeMessages(req)
  let effective: InferRequest = { ...req, messages, prompt: undefined, system: undefined }
  let attempts = 0
  let degraded: string | undefined

  for (let attempt = 0; attempt < 2; attempt++) {
    attempts++
    const { signal, cleanup } = withTimeout(timeoutMs, opts.signal)
    let res: Response
    try {
      res = await fetchImpl(url, {
        method: "POST",
        headers: { "content-type": "application/json", ...(opts.headers ?? {}) },
        body: JSON.stringify(buildRequestBody(effective)),
        signal,
      })
    } catch (e) {
      cleanup()
      const msg = (e as Error).message ?? String(e)
      return {
        ok: false,
        content: "",
        reasoning: "",
        attempts,
        elapsed_ms: Date.now() - t0,
        structured_via: via,
        error: /abort/i.test(msg) ? `请求中止或超时（${timeoutMs} ms）：${msg}` : `请求失败：${msg}`,
      }
    }
    cleanup()
    const text = await res.text()

    if (!res.ok) {
      const err = errorText(res.status, text)
      // 服务端不认识 response_format/grammar（旧引擎或参数名差异）→ 降级为最弱的 json_object 再试一次
      if (attempt === 0 && via !== "none" && via !== "json_object" && /response_format|json_schema|grammar|schema|unsupported|not support|unknown/i.test(err)) {
        degraded = `服务端拒绝了 ${via} 约束（${err}）→ 已降级为 json_object 重试`
        via = "json_object"
        effective = { ...effective, structured: { json_object: true, retry_on_invalid: retryInvalid } }
        continue
      }
      return { ok: false, content: "", reasoning: "", attempts, elapsed_ms: Date.now() - t0, http_status: res.status, structured_via: via, error: err }
    }

    let data: RawResponse
    try {
      data = JSON.parse(text) as RawResponse
    } catch {
      return {
        ok: false,
        content: "",
        reasoning: "",
        attempts,
        elapsed_ms: Date.now() - t0,
        http_status: res.status,
        structured_via: via,
        error: `响应不是合法 JSON：${text.slice(0, 200)}`,
      }
    }

    const choice = data.choices?.[0]
    const content = choice?.message?.content ?? choice?.text ?? data.content ?? ""
    const reasoning = choice?.message?.reasoning_content ?? ""
    const elapsed = Date.now() - t0

    let json: unknown
    let jsonRaw: string | undefined
    let jsonErrors: string[] | undefined
    if (via !== "none") {
      const ex = extractJson(content)
      jsonRaw = ex.raw
      if (ex.error) {
        jsonErrors = [ex.error]
      } else {
        json = ex.value
        const schema = effective.structured?.schema
        if (schema) {
          const errs = validateAgainstSchema(ex.value, schema)
          jsonErrors = errs.length ? errs.slice(0, 20) : undefined
        }
      }
      if (jsonErrors?.length && attempt === 0 && retryInvalid) {
        messages = [
          ...messages,
          { role: "assistant", content: content.slice(0, 2000) },
          {
            role: "user",
            content: `上一次输出无法作为符合要求的 JSON 使用：${jsonErrors.join("; ")}。请只输出一个满足要求的 JSON，不要解释、不要代码块。`,
          },
        ]
        effective = { ...effective, messages, prompt: undefined, system: undefined }
        continue
      }
    }

    return {
      ok: true,
      content,
      reasoning,
      json,
      json_raw: jsonRaw,
      json_errors: jsonErrors,
      structured_via: via,
      usage: data.usage,
      timings: data.timings,
      decode_tps: decodeTps(data.timings, data.usage, elapsed),
      http_status: res.status,
      attempts,
      elapsed_ms: elapsed,
      ...(degraded ? { warning: degraded } : {}),
    }
  }

  return { ok: false, content: "", reasoning: "", attempts, elapsed_ms: Date.now() - t0, structured_via: via, error: "重试后仍未取得可用响应" }
}

/** 探活与能力探测：/health（可用性）、/props（n_ctx、slot 数、模型路径——并发与批量的档位依据）。 */
export interface ProbeResult {
  ok: boolean
  health_status?: number
  n_ctx?: number
  total_slots?: number
  model_path?: string
  raw_props?: Record<string, unknown>
  error?: string
}

export async function probe(
  baseUrl: string,
  opts: { fetchImpl?: typeof fetch; timeoutMs?: number; headers?: Record<string, string> } = {},
): Promise<ProbeResult> {
  const fetchImpl = opts.fetchImpl ?? fetch
  const base = baseUrl.replace(/\/+$/, "")
  const timeout = opts.timeoutMs ?? 5000
  const headers = opts.headers ?? {}
  try {
    const health = await fetchImpl(`${base}/health`, { signal: AbortSignal.timeout(timeout), headers })
    if (!health.ok) return { ok: false, health_status: health.status, error: `/health → HTTP ${health.status}` }
    const props = await fetchImpl(`${base}/props`, { signal: AbortSignal.timeout(timeout), headers })
    if (!props.ok) return { ok: true, health_status: health.status, error: `/props → HTTP ${props.status}（n_ctx/并发数未知）` }
    const j = (await props.json()) as Record<string, unknown>
    const gen = (j.default_generation_settings ?? {}) as Record<string, unknown>
    return {
      ok: true,
      health_status: health.status,
      n_ctx: typeof gen.n_ctx === "number" ? gen.n_ctx : undefined,
      total_slots: typeof j.total_slots === "number" ? j.total_slots : undefined,
      model_path: typeof j.model_path === "string" ? j.model_path : undefined,
      raw_props: j,
    }
  } catch (e) {
    return { ok: false, error: (e as Error).message }
  }
}

// ── 批量提交 ──────────────────────────────────────────────────────────────

export interface BatchItem extends InferTuning {
  /** 任务标识（缺省为序号）；断点续跑与结果归档都以它为准。 */
  id?: string
  messages?: ChatMessage[]
  prompt?: string
  system?: string
  /** 条目级结构化约束（覆盖批量默认）。 */
  structured?: StructuredOutput
  /** 业务侧透传字段（原样回到结果里，便于结果表关联输入）。 */
  meta?: Record<string, unknown>
}

export interface BatchResult extends InferTuning {
  id: string
  index: number
  ok: boolean
  attempts: number
  elapsed_ms: number
  content?: string
  reasoning?: string
  json?: unknown
  json_errors?: string[]
  usage?: InferUsage
  decode_tps?: number
  error?: string
  warning?: string
  meta?: Record<string, unknown>
}

export interface BatchProgress {
  done: number
  failed: number
  total: number
}

export interface BatchOptions {
  baseUrl: string
  /** 并发度（缺省 1）。仅在 slot 数匹配且 prompt 较短的批量场景才有收益——并发上限按档位 parallel。 */
  concurrency?: number
  timeoutMs?: number
  /** 额外请求头（远端端点鉴权）。 */
  headers?: Record<string, string>
  /** 单条失败后的重试次数（缺省 0；含结构化校验失败）。 */
  retries?: number
  /** 批量默认调参（可被条目覆盖）。 */
  defaults?: InferTuning
  /** 批量默认结构化约束（可被条目覆盖）。 */
  structured?: StructuredOutput
  signal?: AbortSignal
  /** 每条完成即回调（用于逐条落盘/进度输出）。 */
  onResult?: (r: BatchResult) => void | Promise<void>
  onProgress?: (p: BatchProgress) => void | Promise<void>
  fetchImpl?: typeof fetch
}

export interface BatchSummary extends BatchProgress {
  ok: number
  tokens_in: number
  tokens_out: number
  decode_tps: number
  avg_latency_ms: number
  elapsed_ms: number
  results: BatchResult[]
}

/**
 * 批量提交：限流 worker pool（并发 = concurrency，缺省串行）逐条推理，
 * 逐条回调落盘，失败按 retries 重试（结构化校验不过也算失败）。
 * 不在此层做持久化——调用方（tasks.ts）负责把每条结果写 JSONL 与进度文件。
 */
export async function runBatch(items: BatchItem[], opts: BatchOptions): Promise<BatchSummary> {
  const total = items.length
  const concurrency = Math.max(1, Math.min(opts.concurrency ?? 1, Math.max(1, total)))
  const retries = Math.max(0, opts.retries ?? 0)
  const results: BatchResult[] = new Array(total)

  const t0 = Date.now()
  let next = 0
  let done = 0
  let failed = 0
  let tokensIn = 0
  let tokensOut = 0

  const worker = async (): Promise<void> => {
    for (;;) {
      const index = next++
      if (index >= total) return
      const item = items[index]
      const id = item.id ?? String(index)
      const itemStart = Date.now()

      if (opts.signal?.aborted) {
        const aborted: BatchResult = { id, index, ok: false, attempts: 0, elapsed_ms: 0, error: "任务已取消", meta: item.meta }
        results[index] = aborted
        failed++
        done++
        await opts.onResult?.(aborted)
        await opts.onProgress?.({ done, failed, total })
        continue
      }

      const req: InferRequest = {
        messages: item.messages,
        prompt: item.prompt,
        system: item.system,
        temperature: item.temperature ?? opts.defaults?.temperature,
        top_p: item.top_p ?? opts.defaults?.top_p,
        max_tokens: item.max_tokens ?? opts.defaults?.max_tokens,
        seed: item.seed ?? opts.defaults?.seed,
        stop: item.stop ?? opts.defaults?.stop,
        enable_thinking: item.enable_thinking ?? opts.defaults?.enable_thinking,
        structured: item.structured ?? opts.structured,
      }

      let resp: InferResponse | undefined
      let error: string | undefined
      let attempts = 0
      for (let attempt = 0; attempt <= retries; attempt++) {
        attempts++
        resp = await chatOnce(req, { baseUrl: opts.baseUrl, timeoutMs: opts.timeoutMs, signal: opts.signal, fetchImpl: opts.fetchImpl, headers: opts.headers })
        if (resp.ok && !(resp.json_errors?.length)) {
          error = undefined
          break
        }
        error = resp.error ?? `结构化校验未通过：${(resp.json_errors ?? []).join("; ")}`
      }

      const ok = Boolean(resp?.ok) && !(resp?.json_errors?.length)
      const result: BatchResult = {
        id,
        index,
        ok,
        attempts,
        elapsed_ms: Date.now() - itemStart,
        content: resp?.content,
        reasoning: resp?.reasoning,
        json: resp?.json,
        json_errors: resp?.json_errors,
        usage: resp?.usage,
        decode_tps: resp?.decode_tps,
        error: ok ? undefined : error,
        warning: resp?.warning,
        meta: item.meta,
      }

      if (ok) {
        done++
        tokensIn += resp?.usage?.prompt_tokens ?? 0
        tokensOut += resp?.usage?.completion_tokens ?? 0
      } else {
        failed++
        done++
      }
      results[index] = result
      await opts.onResult?.(result)
      await opts.onProgress?.({ done, failed, total })
    }
  }

  await Promise.all(Array.from({ length: concurrency }, () => worker()))

  const elapsed = Date.now() - t0
  const elapsedForRate = Math.max(1, elapsed)
  const okCount = total - failed
  const latencies = results.filter(Boolean).map((r) => r.elapsed_ms)
  const avgLatency = latencies.length ? latencies.reduce((a, b) => a + b, 0) / latencies.length : 0
  return {
    total,
    done,
    failed,
    ok: okCount,
    tokens_in: tokensIn,
    tokens_out: tokensOut,
    decode_tps: tokensOut > 0 ? tokensOut / (elapsedForRate / 1000) : 0,
    avg_latency_ms: Math.round(avgLatency),
    elapsed_ms: elapsed,
    results,
  }
}

