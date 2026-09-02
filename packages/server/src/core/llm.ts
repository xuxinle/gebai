import type { LLMCapabilities, MessageLike } from "@gebai/sdk"
import { repairToolPairing } from "./store"

export interface LLMToolDef {
  name: string
  description: string
  parameters: Record<string, unknown>
}

/** 服务端 usage 真值（接口返回的 token 计数；服务端/网关不返回时为 undefined → 引擎走估算兜底）。 */
export interface LLMUsage {
  inputTokens?: number
  outputTokens?: number
  totalTokens?: number
  /** 本次 input 中命中提示词缓存的 tokens（统一口径为 inputTokens 的一部分）：OpenAI chat/responses 的
   *  *_tokens_details.cached_tokens 已含在 prompt/input_tokens 内；Anthropic 的 cache_read_input_tokens
   *  是 input_tokens 之外的额外量，pickUsage 折算并入 inputTokens。接口不返回缓存字段时 undefined。 */
  cachedTokens?: number
}

export interface LLMChunk {
  type: "text" | "reasoning" | "tool_call" | "done"
  text?: string
  toolCall?: { id: string; name: string; arguments: Record<string, unknown>; raw?: string }
  /** 工具参数不是合法 JSON 时携带原始片段（引擎据此回传模型让其修正，而非静默以 {} 执行）。 */
  toolArgsError?: string
  stopReason?: string
  /** usage 真值（挂在 done chunk 上，一次调用一个值）。 */
  usage?: LLMUsage
}

export interface ChatOptions {
  tools?: LLMToolDef[]
  signal?: AbortSignal
  maxTokens?: number
  /** 本次调用的额外请求体参数（如 reasoning_effort 推理强度），优先级高于 ProviderConfig.extraParams。 */
  extraParams?: Record<string, unknown>
}

export interface LLMProvider {
  readonly id: string
  chat(messages: MessageLike[], opts?: ChatOptions): AsyncIterable<LLMChunk>
  capabilities(): LLMCapabilities
}

export type ApiKind = "openai" | "responses" | "anthropic"

/** 模型接口未配置错误（apiBase 缺失）：配置类错误无需重试，引擎据此直接失败并给出配置入口指引。 */
export class LLMConfigError extends Error {}

/** 拼接接口请求地址：apiBase 剥尾部斜杠后追加 endpoint；apiBase 已以 endpoint 结尾（用户直接粘贴
 *  完整接口地址，如 https://open.bigmodel.cn/api/paas/v4/chat/completions）时原样使用，避免双拼 404。 */
export function endpointUrl(apiBase: string, endpoint: string): string {
  const base = apiBase.replace(/\/+$/, "")
  return base.endsWith(endpoint) ? base : `${base}${endpoint}`
}

function assertApiConfigured(config: ProviderConfig): void {
  if (!config.apiBase) {
    throw new LLMConfigError(
      "模型接口地址未配置：请在「设置 → 环境变量」配置 GEBAI_LLM_API_BASE（及 GEBAI_LLM_API_KEY / GEBAI_LLM_MODEL，随消息生效、不落盘），或配置服务端环境变量后重启",
    )
  }
}

export interface ProviderConfig {
  apiKind: ApiKind
  apiBase: string
  apiKey: string
  model: string
  maxContextTokens: number
  multimodal: boolean
  /** 单次响应输出 token 上限（GEBAI_LLM_MAX_OUTPUT_TOKENS）：大文件生成截断防护；未配置走接口缺省
   *  （Anthropic 强制要求 max_tokens，缺省 8192）。 */
  maxOutputTokens?: number
  /** 每次请求固定的额外请求体参数（如 reasoning_effort），可被 ChatOptions.extraParams 覆盖。 */
  extraParams?: Record<string, unknown>
}

/**
 * 解析额外模型接口参数（JSON 字符串，如 `{"reasoning_effort":"high"}`）：
 * 必须是 JSON 对象（非数组/标量），否则抛错提示检查配置。
 */
export function parseExtraParams(raw: string | undefined): Record<string, unknown> {
  if (!raw) return {}
  let parsed: unknown
  try {
    parsed = JSON.parse(raw)
  } catch {
    throw new Error(`GEBAI_LLM_EXTRA_PARAMS 不是合法 JSON: ${raw}`)
  }
  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
    throw new Error(`GEBAI_LLM_EXTRA_PARAMS 必须是 JSON 对象（如 {"reasoning_effort":"high"}）`)
  }
  return parsed as Record<string, unknown>
}

/** Parse an SSE byte stream into `data:` payload strings.
 *  事件分隔与行结束按 SSE 规范兼容 `\n`/`\r\n`/`\r` 三种形态（部分网关以 CRLF 行结尾，
 *  只按 `\n\n` 切分会永远切不出事件边界、整轮静默无输出）；多行 `data:` 按 `\n` join。 */
export async function* parseSSE(stream: ReadableStream<Uint8Array>, signal?: AbortSignal): AsyncGenerator<string> {
  const reader = stream.getReader()
  const decoder = new TextDecoder()
  let buf = ""
  try {
    for (;;) {
      if (signal?.aborted) break
      const { done, value } = await reader.read()
      if (done) break
      buf += decoder.decode(value, { stream: true }).replace(/\r\n/g, "\n").replace(/\r/g, "\n")
      let idx: number
      while ((idx = buf.indexOf("\n\n")) >= 0) {
        const raw = buf.slice(0, idx)
        buf = buf.slice(idx + 2)
        let data = ""
        for (const line of raw.split("\n")) {
          if (!line.startsWith("data:")) continue
          const v = line.slice(5).replace(/^ /, "")
          data = data ? `${data}\n${v}` : v
        }
        if (data) yield data
      }
    }
    const rest = buf.trim()
    if (rest.startsWith("data:")) yield rest.slice(5).trim()
  } finally {
    // 消费方提前退出（空闲超时 iter.return/取消）：cancel 通知上游中止传输，防连接悬挂累积
    void reader.cancel().catch(() => {})
    reader.releaseLock()
  }
}

/**
 * fetch 带重试（模型接口健壮性）：仅网络错误、429、5xx 重试（指数退避，最多 retries 次）；
 * 其余 4xx 与 AbortError 立即返回/抛出，不重试。请求体为字符串可安全复用。
 */
async function fetchRetry(url: string, init: RequestInit, retries = 2): Promise<Response> {
  for (let attempt = 0; ; attempt++) {
    let res: Response
    try {
      res = await fetch(url, init)
    } catch (err) {
      if ((err as Error).name === "AbortError") throw err
      if (attempt >= retries) throw err
      await sleep(500 * 2 ** attempt, init.signal) // 退避期间可被取消（abort 立即中断）
      continue
    }
    if (res.ok || (res.status !== 429 && res.status < 500)) return res
    if (attempt >= retries) return res // 重试耗尽：返回响应，由调用方构造含状态码的错误
    await sleep(500 * 2 ** attempt, init.signal)
  }
}

function sleep(ms: number, signal?: AbortSignal | null): Promise<void> {
  if (signal?.aborted) return Promise.reject(new DOMException("Aborted", "AbortError"))
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      signal?.removeEventListener("abort", onAbort)
      resolve()
    }, ms)
    const onAbort = () => {
      clearTimeout(timer)
      reject(new DOMException("Aborted", "AbortError"))
    }
    signal?.addEventListener("abort", onAbort, { once: true })
  })
}

function openaiTool(t: LLMToolDef): Record<string, unknown> {
  return { type: "function", function: { name: t.name, description: t.description, parameters: t.parameters } }
}

function anthropicTool(t: LLMToolDef): Record<string, unknown> {
  return { name: t.name, description: t.description, input_schema: t.parameters }
}

/**
 * 统一内部多模态内容块 → OpenAI 兼容格式：
 * `{ type: "image", mime, data }`（base64 数据）→ `{ type: "image_url", image_url: { url: data URL } }`；
 * 其余块原样透传（text 等已为 OpenAI 原生结构）。
 */
function toOpenAIContentBlock(b: Record<string, unknown>): Record<string, unknown> {
  if (b.type === "image" && typeof b.mime === "string" && typeof b.data === "string") {
    return { type: "image_url", image_url: { url: `data:${b.mime};base64,${b.data}` } }
  }
  return b
}

function toOpenAIMessages(msgs: MessageLike[]): Array<Record<string, unknown>> {
  const out: Array<Record<string, unknown>> = []
  for (const m of repairToolPairing(msgs, { flushTail: true })) {
    if (m.role === "tool") {
      // 工具结果多模态（DESIGN「多模态支持」read 图片内联）：块数组映射为内容部件（text/image_url）；
      // 纯字符串保持原样（绝大多数工具结果零开销直传）
      const msg: Record<string, unknown> = {
        role: "tool",
        tool_call_id: m.toolCallId,
        content: typeof m.content === "string" ? m.content : (Array.isArray(m.content) ? m.content.map(toOpenAIContentBlock) : ""),
      }
      if (m.name) msg.name = m.name
      out.push(msg)
    } else if (m.role === "assistant" && m.toolCalls) {
      const calls = m.toolCalls.map((tc) => ({
        id: tc.id,
        type: "function",
        function: { name: tc.name, arguments: typeof tc.arguments === "string" ? tc.arguments : JSON.stringify(tc.arguments) },
      }))
      const msg: Record<string, unknown> = { role: "assistant", content: typeof m.content === "string" ? (m.content || null) : "", tool_calls: calls }
      if (m.toolCalls[0]?.name) msg.name = m.toolCalls[0].name
      out.push(msg)
    } else {
      const content =
        typeof m.content === "string" ? m.content : (Array.isArray(m.content) ? m.content.map(toOpenAIContentBlock) : "")
      out.push({ role: m.role, content })
    }
  }
  return out
}

/** 统一内部多模态内容块 → Anthropic 格式：`{ type: "image", mime, data }` → `{ type: "image", source: { base64 } }`。 */
function toAnthropicContentBlock(b: Record<string, unknown>): Record<string, unknown> {
  if (b.type === "image" && typeof b.mime === "string" && typeof b.data === "string") {
    return { type: "image", source: { type: "base64", media_type: b.mime, data: b.data } }
  }
  if (b.type === "text" && typeof b.text === "string") return { type: "text", text: b.text }
  return b
}

function toAnthropicMessages(msgs: MessageLike[]): Array<Record<string, unknown>> {
  const out: Array<Record<string, unknown>> = []
  for (const m of repairToolPairing(msgs, { flushTail: true })) {
    if (m.role === "tool") {
      // 工具结果多模态：tool_result 内容支持块数组（text + image，Anthropic 官方形态——read 图片内联）
      out.push({ role: "user", content: [{ type: "tool_result", tool_use_id: m.toolCallId, content: typeof m.content === "string" ? m.content : (Array.isArray(m.content) ? m.content.map(toAnthropicContentBlock) : "") }] })
    } else if (m.role === "assistant" && m.toolCalls) {
      out.push({
        role: "assistant",
        content: [
          { type: "text", text: typeof m.content === "string" ? m.content : "" },
          ...m.toolCalls.map((tc) => ({
            type: "tool_use",
            id: tc.id,
            name: tc.name,
            input: typeof tc.arguments === "string" ? {} : tc.arguments,
          })),
        ],
      })
    } else if (m.role === "system") {
      // system messages folded by the caller; ignore here to keep ordering valid
    } else {
      // 文本内容统一为块数组（与 tool_result/tool_use 分支同构），使相邻同角色消息可合并
      const content: unknown =
        typeof m.content === "string"
          ? (m.content ? [{ type: "text", text: m.content }] : "")
          : (Array.isArray(m.content) ? m.content.map(toAnthropicContentBlock) : "")
      out.push({ role: m.role, content })
    }
  }
  // 相邻同角色合并（Anthropic 要求 user/assistant 交替）：配对修复在 tool_result 后可能紧跟 user 文本、
  // 历史装载的连续 user/assistant 消息同理——content 数组拼接为单条消息
  const merged: Array<Record<string, unknown>> = []
  for (const m of out) {
    const prev = merged[merged.length - 1]
    if (prev && prev.role === m.role && Array.isArray(prev.content) && Array.isArray(m.content)) {
      prev.content = [...(prev.content as unknown[]), ...(m.content as unknown[])]
    } else {
      merged.push(m)
    }
  }
  return merged
}

function capabilities(config: ProviderConfig): LLMCapabilities {
  return {
    streaming: true,
    toolCalling: true,
    multimodal: config.multimodal,
    maxContextTokens: config.maxContextTokens,
  }
}

/**
 * 统一内部消息 → OpenAI Responses API `input` 格式：
 * 普通消息原样透传；assistant 工具调用拆为独立 `function_call` item（后跟 `function_call_output` tool 消息），
 * 多模态图片块转 `image_url`（同 Chat Completions）。
 */
function toResponsesInput(msgs: MessageLike[]): Array<Record<string, unknown>> {
  const out: Array<Record<string, unknown>> = []
  for (const m of repairToolPairing(msgs, { flushTail: true })) {
    if (m.role === "tool") {
      // function_call_output 仅接受字符串：文本块拼为输出，图片块转紧随的 user 消息内容部件
      // （Responses 输入项顺序合法，模型同轮可见——工具结果多模态 read 图片内联）
      const output = typeof m.content === "string"
        ? m.content
        : (Array.isArray(m.content) ? m.content.filter((b) => b.type === "text").map((b) => String((b as { text?: unknown }).text ?? "")).join("\n") : "")
      out.push({ type: "function_call_output", call_id: m.toolCallId, output })
      if (Array.isArray(m.content)) {
        const imgs = m.content.filter((b) => b.type === "image")
        if (imgs.length) out.push({ role: "user", content: imgs.map(toOpenAIContentBlock) })
      }
    } else if (m.role === "assistant" && m.toolCalls) {
      const content =
        typeof m.content === "string" ? (m.content || "") : (Array.isArray(m.content) ? m.content.map(toOpenAIContentBlock) : "")
      out.push({ role: "assistant", content })
      for (const tc of m.toolCalls) {
        out.push({
          type: "function_call",
          call_id: tc.id,
          name: tc.name,
          arguments: typeof tc.arguments === "string" ? tc.arguments : JSON.stringify(tc.arguments),
        })
      }
    } else {
      const content =
        typeof m.content === "string" ? m.content : (Array.isArray(m.content) ? m.content.map(toOpenAIContentBlock) : "")
      out.push({ role: m.role, content })
    }
  }
  return out
}

function responsesTool(t: LLMToolDef): Record<string, unknown> {
  return { type: "function", name: t.name, description: t.description, parameters: t.parameters }
}

export function createProvider(config: ProviderConfig): LLMProvider {
  if (config.apiKind === "anthropic") return new AnthropicProvider(config)
  if (config.apiKind === "responses") return new ResponsesProvider(config)
  return new OpenAIProvider(config)
}

const API_KINDS: ApiKind[] = ["openai", "responses", "anthropic"]

/**
 * 任务级主模型配置覆盖：从会话/任务 env 提取 `GEBAI_LLM_*` 覆盖启动配置
 * （模型/接口地址/密钥/类型/上下文预算/多模态声明；`GEBAI_LLM_EXTRA_PARAMS` 由引擎单独经 ChatOptions 合并，不在此列）。
 * 无任何覆盖键时返回 null（调用方沿用启动 Provider 实例，避免无谓重建）。
 */
export function modelEnvOverrides(env: Record<string, string> | undefined): Partial<ProviderConfig> | null {
  if (!env) return null
  const out: Partial<ProviderConfig> = {}
  let any = false
  if (env.GEBAI_LLM_MODEL) {
    out.model = env.GEBAI_LLM_MODEL
    any = true
  }
  if (env.GEBAI_LLM_API_BASE) {
    out.apiBase = env.GEBAI_LLM_API_BASE
    any = true
  }
  if (env.GEBAI_LLM_API_KEY) {
    out.apiKey = env.GEBAI_LLM_API_KEY
    any = true
  }
  if (env.GEBAI_LLM_API_KIND && API_KINDS.includes(env.GEBAI_LLM_API_KIND as ApiKind)) {
    out.apiKind = env.GEBAI_LLM_API_KIND as ApiKind
    any = true
  }
  const maxCtx = Number(env.GEBAI_LLM_MAX_CONTEXT)
  if (env.GEBAI_LLM_MAX_CONTEXT !== undefined && Number.isFinite(maxCtx) && maxCtx > 0) {
    out.maxContextTokens = maxCtx
    any = true
  }
  const maxOut = Number(env.GEBAI_LLM_MAX_OUTPUT_TOKENS)
  if (env.GEBAI_LLM_MAX_OUTPUT_TOKENS !== undefined && Number.isFinite(maxOut) && maxOut > 0) {
    out.maxOutputTokens = maxOut
    any = true
  }
  if (env.GEBAI_LLM_MULTIMODAL !== undefined) {
    out.multimodal = env.GEBAI_LLM_MULTIMODAL === "true"
    any = true
  }
  return any ? out : null
}

/** 启动配置 + 任务级覆盖合并：无覆盖时返回原配置引用（调用方可据此直接复用启动 Provider）。 */
export function applyModelEnvOverrides(base: ProviderConfig, env: Record<string, string> | undefined): ProviderConfig {
  const o = modelEnvOverrides(env)
  return o ? { ...base, ...o } : base
}

/**
 * 命名模型路由（`GEBAI_LLM_ROUTES`，DESIGN「会话分支运行与合并」多路接口）：JSON 对象
 * `{ "<路由名>": { "model": "...", "api_base"?: "...", "api_key"?: "...", "api_kind"?: "openai|responses|anthropic", "max_context"?: 400000 } }`。
 * 分支运行按路由名解析各自 Provider——多端点/多模型并行分摊单路限流，摆脱单轮串行速度限制。
 * 非法 JSON/非对象/字段缺失的条目静默忽略；未配置返回空表。
 */
export function parseModelRoutes(env: Record<string, string> | undefined): Record<string, { model: string; apiBase?: string; apiKey?: string; apiKind?: ApiKind; maxContextTokens?: number }> {
  const raw = env?.GEBAI_LLM_ROUTES
  if (!raw) return {}
  let parsed: unknown
  try {
    parsed = JSON.parse(raw)
  } catch {
    return {}
  }
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return {}
  const out: Record<string, { model: string; apiBase?: string; apiKey?: string; apiKind?: ApiKind; maxContextTokens?: number }> = {}
  for (const [name, v] of Object.entries(parsed as Record<string, unknown>)) {
    if (!/^[A-Za-z0-9_-]{1,32}$/.test(name) || !v || typeof v !== "object") continue
    const r = v as Record<string, unknown>
    if (typeof r.model !== "string" || !r.model.trim()) continue
    const maxCtx = Number(r.max_context)
    out[name] = {
      model: r.model.trim(),
      ...(typeof r.api_base === "string" && r.api_base.trim() ? { apiBase: r.api_base.trim() } : {}),
      ...(typeof r.api_key === "string" && r.api_key ? { apiKey: r.api_key } : {}),
      ...(typeof r.api_kind === "string" && API_KINDS.includes(r.api_kind as ApiKind) ? { apiKind: r.api_kind as ApiKind } : {}),
      ...(r.max_context !== undefined && Number.isFinite(maxCtx) && maxCtx > 0 ? { maxContextTokens: maxCtx } : {}),
    }
  }
  return out
}

/**
 * 按名解析分支运行 Provider（branch_run 的 model 参数）：命中 `GEBAI_LLM_ROUTES` 路由名 →
 * 路由配置合并启动配置构建独立 Provider（未指定的项沿用任务级合并基准）；未命中路由名 →
 * 视为字面模型名覆盖；name 为空返回 undefined（沿用任务级 Provider）。
 */
export function resolveModelRouteProvider(base: ProviderConfig, env: Record<string, string> | undefined, name: string): LLMProvider | undefined {
  if (!name.trim()) return undefined
  const mergedBase = applyModelEnvOverrides(base, env)
  const route = parseModelRoutes(env)[name]
  if (route) {
    return createProvider({
      ...mergedBase,
      model: route.model,
      ...(route.apiBase !== undefined ? { apiBase: route.apiBase } : {}),
      ...(route.apiKey !== undefined ? { apiKey: route.apiKey } : {}),
      ...(route.apiKind !== undefined ? { apiKind: route.apiKind } : {}),
      ...(route.maxContextTokens !== undefined ? { maxContextTokens: route.maxContextTokens } : {}),
    })
  }
  return createProvider({ ...mergedBase, model: name })
}

/**
 * 任务级视觉（多模态）Provider 解析：`GEBAI_VISION_*` env 覆盖启动视觉配置；
 * 未配置视觉模型时回落到声明多模态能力的主模型（含任务级 `GEBAI_LLM_MULTIMODAL` 覆盖）。
 * 返回 null 表示视觉不可用。
 */
export function resolveVisionProvider(mainCfg: ProviderConfig, visionCfg: ProviderConfig | null, env: Record<string, string> | undefined): LLMProvider | null {
  const main = applyModelEnvOverrides(mainCfg, env)
  const vModel = env?.GEBAI_VISION_MODEL || visionCfg?.model
  if (!vModel) return main.multimodal ? createProvider(main) : null
  const maxCtx = Number(env?.GEBAI_VISION_MAX_CONTEXT)
  const extra = parseExtraParamsSafe(env?.GEBAI_VISION_EXTRA_PARAMS)
  const merged: ProviderConfig = {
    apiKind: (env?.GEBAI_VISION_API_KIND && API_KINDS.includes(env.GEBAI_VISION_API_KIND as ApiKind) ? env.GEBAI_VISION_API_KIND : (visionCfg?.apiKind || main.apiKind)) as ApiKind,
    apiBase: env?.GEBAI_VISION_API_BASE || visionCfg?.apiBase || main.apiBase,
    apiKey: env?.GEBAI_VISION_API_KEY || visionCfg?.apiKey || main.apiKey,
    model: vModel,
    maxContextTokens: (env?.GEBAI_VISION_MAX_CONTEXT !== undefined && Number.isFinite(maxCtx) && maxCtx > 0 ? maxCtx : visionCfg?.maxContextTokens) || main.maxContextTokens,
    multimodal: true,
    extraParams: extra || visionCfg?.extraParams,
  }
  return createProvider(merged)
}

/** 任务级额外模型接口参数解析（GEBAI_LLM_EXTRA_PARAMS/GEBAI_VISION_EXTRA_PARAMS）：非法 JSON 静默忽略，不阻塞任务。 */
function parseExtraParamsSafe(raw: string | undefined): Record<string, unknown> | undefined {
  if (raw === undefined) return undefined
  try {
    return parseExtraParams(raw)
  } catch {
    return undefined
  }
}

class OpenAIProvider implements LLMProvider {
  readonly id = "openai"
  constructor(private config: ProviderConfig) {}

  capabilities(): LLMCapabilities {
    return capabilities(this.config)
  }

  async *chat(msgs: MessageLike[], opts: ChatOptions = {}): AsyncIterable<LLMChunk> {
    assertApiConfigured(this.config)
    const body: Record<string, unknown> = {
      model: this.config.model,
      messages: toOpenAIMessages(msgs),
      stream: true,
      // 末 chunk 返回 usage 真值（prompt/completion/total tokens；服务端不支持时忽略 → 估算兜底）
      stream_options: { include_usage: true },
    }
    if (opts.tools?.length) body.tools = opts.tools.map(openaiTool)
    const maxOut = opts.maxTokens ?? this.config.maxOutputTokens
    if (maxOut) body.max_tokens = maxOut
    // 额外模型接口参数：Provider 级固定参数 + 本次调用参数（后者优先），顶层合并进请求体
    Object.assign(body, this.config.extraParams, opts.extraParams)
    const res = await fetchRetry(endpointUrl(this.config.apiBase, "/chat/completions"), {
      method: "POST",
      headers: { "Content-Type": "application/json", Authorization: `Bearer ${this.config.apiKey}` },
      body: JSON.stringify(body),
      signal: opts.signal,
    })
    if (!res.ok || !res.body) {
      const detail = (await res.text()).slice(0, 200)
      throw new Error(`模型接口错误（HTTP ${res.status}）: ${detail || res.statusText}`)
    }
    // Aggregate tool-call fragments by index: id/name/arguments arrive across chunks.
    const toolCallsByIndex = new Map<number, { id: string; name: string; args: string }>()
    // usage 真值在 finish_reason 之后的末 chunk（include_usage）到达：记录 finish_reason 并延迟 done 产出，把 usage 一并挂到 done
    let pendingStop: string | undefined
    let usage: LLMUsage | undefined
    for await (const data of parseSSE(res.body, opts.signal)) {
      if (data === "[DONE]") break
      let parsed: Record<string, unknown>
      try {
        parsed = JSON.parse(data)
      } catch {
        continue
      }
      const u = pickUsage(parsed.usage as Record<string, unknown> | undefined)
      if (u) usage = u
      const choice = (parsed.choices as Array<Record<string, unknown>>)?.[0]
      if (!choice) continue
      const delta = (choice.delta as Record<string, unknown>) || {}
      const content = delta.content as string | undefined
      if (content) {
        // DeepSeek 思考标记泄漏（自部署接口未分离）：整轮作废，抛错由引擎层重试（不净化保留）
        yield { type: "text", text: content }
      }
      // 推理内容（如 DeepSeek 的 reasoning_content）：泄漏标记同样作废整轮（用户实测两字段均可能出现）
      const reasoning = delta.reasoning_content as string | undefined
      if (reasoning) {
        yield { type: "reasoning", text: reasoning }
      }
      const tcs = delta.tool_calls as Array<Record<string, unknown>> | undefined
      if (tcs) {
        for (const tc of tcs) {
          const idx = Number(tc.index ?? 0)
          const cur = toolCallsByIndex.get(idx) || { id: "", name: "", args: "" }
          if (tc.id) cur.id = tc.id as string
          const fn = (tc.function as Record<string, unknown>) || {}
          if (fn.name) cur.name = fn.name as string
          if (typeof fn.arguments === "string") cur.args += fn.arguments
          toolCallsByIndex.set(idx, cur)
        }
      }
      if (typeof choice.finish_reason === "string" && choice.finish_reason) {
        pendingStop = choice.finish_reason
      }
    }
    if (pendingStop !== undefined) {
      yield { type: "done", stopReason: pendingStop, ...(usage ? { usage } : {}) }
    } else {
      // 异常断流（无 finish_reason 即连接结束）：补 done 保持三家语义一致（下游终止信号不缺位）
      yield { type: "done", stopReason: "stop" }
    }
    for (const tc of toolCallsByIndex.values()) {
      if (!tc.name) continue
      const pa = parseArgs(tc.args)
      yield { type: "tool_call", toolCall: { id: tc.id, name: tc.name, arguments: pa.args, ...(pa.error ? { raw: tc.args } : {}) }, ...(pa.error ? { toolArgsError: pa.error } : {}) }
    }
  }
}

class ResponsesProvider implements LLMProvider {
  readonly id = "responses"
  constructor(private config: ProviderConfig) {}

  capabilities(): LLMCapabilities {
    return capabilities(this.config)
  }

  async *chat(msgs: MessageLike[], opts: ChatOptions = {}): AsyncIterable<LLMChunk> {
    assertApiConfigured(this.config)
    const body: Record<string, unknown> = {
      model: this.config.model,
      input: toResponsesInput(msgs),
      stream: true,
    }
    if (opts.tools?.length) body.tools = opts.tools.map(responsesTool)
    const maxOut = opts.maxTokens ?? this.config.maxOutputTokens
    if (maxOut) body.max_output_tokens = maxOut
    // 额外模型接口参数：Provider 级固定参数 + 本次调用参数（后者优先），顶层合并进请求体
    Object.assign(body, this.config.extraParams, opts.extraParams)
    const res = await fetchRetry(endpointUrl(this.config.apiBase, "/responses"), {
      method: "POST",
      headers: { "Content-Type": "application/json", Authorization: `Bearer ${this.config.apiKey}` },
      body: JSON.stringify(body),
      signal: opts.signal,
    })
    if (!res.ok || !res.body) {
      const detail = (await res.text()).slice(0, 200)
      throw new Error(`模型接口错误（HTTP ${res.status}）: ${detail || res.statusText}`)
    }
    // 函数调用按 item_id 聚合：call_id/name 由 output_item.added 提供，参数由 arguments delta 累积
    const calls = new Map<string, { callId: string; name: string; args: string }>()
    let done = false
    for await (const data of parseSSE(res.body, opts.signal)) {
      let evt: Record<string, unknown>
      try {
        evt = JSON.parse(data)
      } catch {
        continue
      }
      const type = evt.type as string
      if (type === "response.output_item.added") {
        const item = evt.item as Record<string, unknown> | undefined
        if (item?.type === "function_call" && typeof item.id === "string") {
          calls.set(item.id, { callId: String(item.call_id ?? ""), name: String(item.name ?? ""), args: "" })
        }
      } else if (type === "response.function_call_arguments.delta") {
        const cur = calls.get(String(evt.item_id))
        if (cur && typeof evt.delta === "string") cur.args += evt.delta
      } else if (type === "response.function_call_arguments.done") {
        const cur = calls.get(String(evt.item_id))
        if (cur) {
          if (typeof evt.arguments === "string") cur.args = evt.arguments
          const pa = parseArgs(cur.args)
          yield { type: "tool_call", toolCall: { id: cur.callId, name: cur.name, arguments: pa.args, ...(pa.error ? { raw: cur.args } : {}) }, ...(pa.error ? { toolArgsError: pa.error } : {}) }
          calls.delete(String(evt.item_id))
        }
      } else if (type === "response.output_text.delta") {
        if (typeof evt.delta === "string") {
          // DeepSeek 思考标记泄漏（自部署接口未分离）：整轮作废，抛错由引擎层重试（不净化保留）
          yield { type: "text", text: evt.delta }
        }
      } else if (type === "response.reasoning_summary_text.delta" || type === "response.reasoning_text.delta") {
        if (typeof evt.delta === "string") {
          yield { type: "reasoning", text: evt.delta }
        }
      } else if (type === "response.completed") {
        const resp = evt.response as Record<string, unknown> | undefined
        const output = ((resp?.output as Array<Record<string, unknown>> | undefined) || [])
        let stopReason = "stop"
        for (let i = output.length - 1; i >= 0; i--) {
          const fr = output[i]?.finish_reason as string | undefined
          if (fr) {
            stopReason = fr
            break
          }
        }
        done = true
        // usage 真值（input/output/total tokens）：与 stopReason 同挂 done chunk
        const usage = pickUsage(resp?.usage as Record<string, unknown> | undefined)
        yield { type: "done", stopReason, ...(usage ? { usage } : {}) }
      } else if (type === "response.incomplete") {
        // 输出上限截断（incomplete_details.reason=max_output_tokens 等）：与截断检测口径对齐（length）
        const reason = (evt.response as Record<string, unknown> | undefined)?.incomplete_details as Record<string, unknown> | undefined
        done = true
        yield { type: "done", stopReason: reason?.reason === "max_output_tokens" ? "length" : String(reason?.reason ?? "length") }
      } else if (type === "response.failed" || type === "error") {
        const err = (evt.error as Record<string, unknown>) || evt
        throw new Error(`模型接口错误: ${String(err?.message ?? "流式响应失败")}`)
      }
    }
    // 兜底：completed 未触发（异常断流）时清掉尚未产出的函数调用并补 done
    for (const c of calls.values()) {
      if (!c.name) continue
      const pa = parseArgs(c.args)
      yield { type: "tool_call", toolCall: { id: c.callId, name: c.name, arguments: pa.args, ...(pa.error ? { raw: c.args } : {}) }, ...(pa.error ? { toolArgsError: pa.error } : {}) }
    }
    if (!done) yield { type: "done", stopReason: "stop" }
  }
}

class AnthropicProvider implements LLMProvider {
  readonly id = "anthropic"
  constructor(private config: ProviderConfig) {}

  capabilities(): LLMCapabilities {
    return capabilities(this.config)
  }

  async *chat(msgs: MessageLike[], opts: ChatOptions = {}): AsyncIterable<LLMChunk> {
    assertApiConfigured(this.config)
    const system = msgs.filter((m) => m.role === "system").map((m) => String(m.content)).join("\n")
    const body: Record<string, unknown> = {
      model: this.config.model,
      messages: toAnthropicMessages(msgs),
      // Anthropic 强制要求 max_tokens：大文件生成截断防护——配置上限优先，缺省 8192（现代 Claude 3.5+ 均支持）
      max_tokens: opts.maxTokens || this.config.maxOutputTokens || 8192,
      stream: true,
    }
    if (system) body.system = system
    if (opts.tools?.length) body.tools = opts.tools.map(anthropicTool)
    // 额外模型接口参数：Provider 级固定参数 + 本次调用参数（后者优先），顶层合并进请求体
    Object.assign(body, this.config.extraParams, opts.extraParams)
    const res = await fetchRetry(endpointUrl(this.config.apiBase, "/v1/messages"), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "x-api-key": this.config.apiKey,
        "anthropic-version": "2023-06-01",
      },
      body: JSON.stringify(body),
      signal: opts.signal,
    })
    if (!res.ok || !res.body) {
      const detail = (await res.text()).slice(0, 200)
      throw new Error(`模型接口错误（HTTP ${res.status}）: ${detail || res.statusText}`)
    }
    let currentTool: { id: string; name: string; args: string } | null = null
    let doneYielded = false
    // usage 真值：message_start 的 input_tokens（含 system 与工具 schema，即「真上下文」）+ message_delta 的 output_tokens；
    // cache_read 折算后的 cachedTokens 一并自 message_start 携带（done 时并入最终 usage）
    let inputTokens: number | undefined
    let cachedTokens: number | undefined
    for await (const data of parseSSE(res.body, opts.signal)) {
      let evt: Record<string, unknown>
      try {
        evt = JSON.parse(data)
      } catch {
        continue
      }
      const type = evt.type as string
      if (type === "message_start") {
        const u = pickUsage((evt.message as Record<string, unknown> | undefined)?.usage as Record<string, unknown> | undefined)
        if (u?.inputTokens !== undefined) inputTokens = u.inputTokens
        if (u?.cachedTokens !== undefined) cachedTokens = u.cachedTokens
      } else if (type === "content_block_start") {
        const block = evt.content_block as Record<string, unknown> | undefined
        if (block?.type === "tool_use") {
          currentTool = { id: block.id as string, name: block.name as string, args: "" }
        }
      } else if (type === "content_block_delta") {
        const delta = evt.delta as Record<string, unknown> | undefined
        if (delta?.type === "text_delta" && delta.text) {
          // DeepSeek 思考标记泄漏：整轮作废，抛错由引擎层重试（不净化保留）
          yield { type: "text", text: delta.text as string }
        } else if (delta?.type === "thinking_delta" && delta.thinking) {
          yield { type: "reasoning", text: delta.thinking as string }
        } else if (delta?.type === "input_json_delta" && currentTool) currentTool.args += delta.partial_json as string
      } else if (type === "content_block_stop") {
        if (currentTool) {
          const pa = parseArgs(currentTool.args)
          yield { type: "tool_call", toolCall: { id: currentTool.id, name: currentTool.name, arguments: pa.args, ...(pa.error ? { raw: currentTool.args } : {}) }, ...(pa.error ? { toolArgsError: pa.error } : {}) }
          currentTool = null
        }
      } else if (type === "message_delta") {
        const delta = evt.delta as Record<string, unknown> | undefined
        const out = pickUsage(evt.usage as Record<string, unknown> | undefined)?.outputTokens
        const usage = inputTokens !== undefined || out !== undefined ? { inputTokens, outputTokens: out, cachedTokens } : undefined
        if (delta?.stop_reason) {
          doneYielded = true
          yield { type: "done", stopReason: delta.stop_reason as string, ...(usage ? { usage } : {}) }
        }
      }
    }
    // 兜底 done（恰好一次）：正常流 message_delta 已产出则跳过；异常断流补终止信号
    if (!currentTool && !doneYielded) yield { type: "done", stopReason: "end_turn" }
  }
}

function parseArgs(raw: string): { args: Record<string, unknown>; error?: string } {
  if (!raw) return { args: {} }
  try {
    return { args: JSON.parse(raw) }
  } catch {
    return { args: {}, error: raw.slice(0, 500) }
  }
}

function skipWs(s: string, i: number): number {
  while (i < s.length && (s[i] === " " || s[i] === "\n" || s[i] === "\t" || s[i] === "\r")) i++
  return i
}

/** 容错扫描 JSON 字符串字面量：返回解码值与扫描终点；未遇闭合引号（截断）closed=false、
 *  value 为已解码前缀（尾部不完整转义序列丢弃）；遇 JSON 不允许的裸控制字符视为字符串到此截断；
 *  非法转义序列返回 null（整体放弃，防脏数据落盘）。 */
function scanString(s: string, i: number): { value: string; end: number; closed: boolean } | null {
  i++ // 跳过开头引号（调用方保证）
  let out = ""
  for (;;) {
    if (i >= s.length) return { value: out, end: i, closed: false }
    const ch = s[i]
    if (ch === '"') return { value: out, end: i + 1, closed: true }
    if (ch === "\\") {
      if (i + 1 >= s.length) return { value: out, end: i, closed: false } // 尾部孤立反斜杠：丢弃
      const e = s[i + 1]
      if (e === "u") {
        if (i + 6 > s.length) return { value: out, end: i, closed: false } // \u 转义不完整：丢弃
        const hex = s.slice(i + 2, i + 6)
        if (!/^[0-9a-fA-F]{4}$/.test(hex)) return null
        out += String.fromCharCode(parseInt(hex, 16))
        i += 6
        continue
      }
      const dec: Record<string, string> = { '"': '"', "\\": "\\", "/": "/", b: "\b", f: "\f", n: "\n", r: "\r", t: "\t" }
      if (!Object.prototype.hasOwnProperty.call(dec, e)) return null
      out += dec[e]
      i += 2
      continue
    }
    if (ch.charCodeAt(0) < 0x20) return { value: out, end: i, closed: false }
    out += ch
    i++
  }
}

/**
 * 抢救解析被截断的 write 参数（大文件生成防护）：模型输出达到上限被截断时，工具参数 JSON
 * 不完整（如 `{"path":"a.txt","content":"前半…`）——容错扫描前缀，提取完整生成的 path 与
 * 已生成部分的 content（未闭合字符串取已解码前缀），引擎据此先把已生成内容落盘，模型下一轮
 * append 续写即可（无需重新生成全量、避免再次截断）。path/content 缺失、前缀结构对不上或
 * 含非法转义时返回 null（走普通错误引导）。
 */
export function salvageWriteArgs(raw: string): { path: string; content: string } | null {
  let i = skipWs(raw, 0)
  if (raw[i] !== "{") return null
  i++
  let path: string | undefined
  let content: string | undefined
  for (;;) {
    i = skipWs(raw, i)
    if (i >= raw.length || raw[i] === "}") break
    if (raw[i] !== '"') return null
    const key = scanString(raw, i)
    if (!key || !key.closed) return null
    i = skipWs(raw, key.end)
    if (raw[i] !== ":") return null
    i = skipWs(raw, i + 1)
    if (raw[i] === '"') {
      const val = scanString(raw, i)
      if (!val) return null
      if (key.value === "path") path = val.value
      else if (key.value === "content") content = val.value
      i = val.end
    } else {
      // 非字符串标量值（append:true 等）：跳过至 , 或 }（数值/布尔/null 不含这两字符）
      while (i < raw.length && raw[i] !== "," && raw[i] !== "}") i++
    }
    i = skipWs(raw, i)
    if (raw[i] === ",") i++
    else break // } 或截断
  }
  if (!path || !content) return null
  return { path, content }
}

/** 从接口返回的 usage 对象提取统一结构（OpenAI prompt_tokens/completion_tokens 与 Responses/Anthropic input_tokens/output_tokens 两种命名）。 */
function pickUsage(u: Record<string, unknown> | undefined): LLMUsage | undefined {
  if (!u) return undefined
  const num = (v: unknown) => (typeof v === "number" && Number.isFinite(v) ? v : undefined)
  // 提示词缓存命中：OpenAI chat（prompt_tokens_details）与 Responses（input_tokens_details）的 cached_tokens
  // 已含在 prompt/input_tokens 内；Anthropic 的 cache_read_input_tokens 在 input_tokens 之外，折算并入统一「cached ⊆ input」
  const details = (u.prompt_tokens_details ?? u.input_tokens_details) as Record<string, unknown> | undefined
  const cacheRead = num(u.cache_read_input_tokens)
  const cachedTokens = num(details?.cached_tokens) ?? cacheRead
  let inputTokens = num(u.input_tokens) ?? num(u.prompt_tokens)
  if (cacheRead !== undefined && inputTokens !== undefined) inputTokens += cacheRead
  const outputTokens = num(u.output_tokens) ?? num(u.completion_tokens)
  const totalTokens = num(u.total_tokens)
  if (inputTokens === undefined && outputTokens === undefined && totalTokens === undefined && cachedTokens === undefined) return undefined
  return { inputTokens, outputTokens, totalTokens, cachedTokens }
}

/** 图片扩展名 → MIME（视觉/多模态图片支持白名单，Anthropic 仅接受 png/jpeg/gif/webp）。 */
export const VISION_IMAGE_MIME: Record<string, string> = {
  png: "image/png",
  jpg: "image/jpeg",
  jpeg: "image/jpeg",
  gif: "image/gif",
  webp: "image/webp",
}

/**
 * 构造多模态用户消息内容块（统一内部格式：文本 + base64 图片），
 * `provider.chat()` 按接口规范转换（OpenAI `image_url` / Anthropic `image`）。
 * 供视觉工具（vision）等直接携带图片调用模型。
 */
export function imageMessageBlocks(text: string, mime: string, base64: string): Array<Record<string, unknown>> {
  return [{ type: "text", text }, { type: "image", mime, data: base64 }]
}
