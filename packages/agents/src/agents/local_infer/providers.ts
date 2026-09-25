/**
 * local_infer 的**统一推理目标层**（providers）：把「这次推理打哪个端点」从调用层里抽出来。
 *
 * 三类目标（同一个 target 字符串就能切换，generate/batch/jobs 全部通用）：
 *   1. `local`（缺省）：本机受管 llama-server——端口取服务实例状态文件（`readServerStates`，多个实例时取
 *      端口最小的一个并把其余列进 note），没有状态文件时退回 `LOCAL_INFER_PORT` / 8080；
 *   2. 直连 URL：`target="http://192.168.1.20:8080"` 或 `https://...`——任意 OpenAI 兼容端点
 *      （局域网另一台机器、LM Studio/vLLM/Ollama 的兼容口、云端网关都算）；
 *   3. 命名目标：环境变量 `LOCAL_INFER_TARGETS`（JSON 数组，元素 `{ name, base_url, api_key?, model?, note? }`）
 *      里声明的目标，`target="<name>"` 选用。
 *
 * 设计要点：
 *   * 只做「解析 + 归一 + 探活」，不发推理请求（那是 api.ts 的事）、不做持久化（那是 tasks/paths 的事）；
 *   * 解析**永不抛异常**——工具层要把它转成可读输出（`{ error }` / errors 数组），一个手抖的环境变量
 *     不该让整个工具调用崩掉；
 *   * 密钥只在本层进出，任何**人读输出**都必须经 `targetSummary`（掩码，如 `sk-***abc`），
 *     job.json 也只落目标名与不含密钥的 base_url。
 */
import type { Tool, ToolContext, ToolSchema } from "@gebai/sdk"
import { probe, type ProbeResult } from "./api"
import { baseUrl, defaultPort, inferHome, readServerStates, type ServerState } from "./paths"

// ── 类型 ──────────────────────────────────────────────────────────────────

/** LOCAL_INFER_TARGETS 里声明的命名目标（环境变量形态：JSON 数组的一个元素）。 */
export interface TargetSpec {
  name: string
  base_url: string
  api_key?: string
  model?: string
  note?: string
}

/** 解析后的推理目标：调用层只认这个——一个基址 + 可选鉴权头，与「本机/远端」无关。 */
export interface InferTarget {
  /** 目标标识：`local`、命名目标的 name，或直连 URL 原文（会落进 job.json，便于追溯结果出自哪个端点）。 */
  name: string
  /** `local` = 本机受管服务（状态文件 / LOCAL_INFER_PORT）；`remote` = 直连 URL 或命名目标（不由本机管理）。 */
  kind: "local" | "remote"
  baseUrl: string
  apiKey?: string
  model?: string
  note?: string
}

/** 本机目标的名字（保留名：命名目标不得占用）。 */
const LOCAL_NAME = "local"

// ── 小工具 ────────────────────────────────────────────────────────────────

/** 非空字符串取数（空串/纯空白/非字符串 → undefined；环境变量恒为字符串，空串当未配置）。 */
function str(v: unknown): string | undefined {
  return typeof v === "string" && v.trim() !== "" ? v.trim() : undefined
}

/** 端点基址归一：去首尾空白与尾斜杠（chatOnce 会再拼 `/v1/chat/completions`，留着尾斜杠会拼出双斜杠）。 */
function normalizeBaseUrl(v: string): string {
  return v.trim().replace(/\/+$/, "")
}

const isHttpUrl = (v: string): boolean => /^https?:\/\//i.test(v)

/** 配置示例（错误信息里反复用到，抽出来避免各处文案漂移）。 */
const CONFIG_EXAMPLE = '{"name":"lan","base_url":"http://192.168.1.20:8080","api_key":"sk-..."}'

// ── 环境变量解析 ──────────────────────────────────────────────────────────

/**
 * 解析 `LOCAL_INFER_TARGETS`（容错：非 JSON / 非数组 / 缺字段 / 非法端点 / 重复名 / 占用保留名
 * 一律收集进 `errors` 而不抛）。返回的 `targets` 只含可用项，坏条目跳过并各记一条带序号的错误，
 * 调用方可原样展示（"哪一项坏了、为什么"比"配置无效"有用得多）。
 */
export function parseTargetsEnv(env: Record<string, string>): { targets: TargetSpec[]; errors: string[] } {
  const errors: string[] = []
  const raw = env?.["LOCAL_INFER_TARGETS"]
  if (raw == null || raw.trim() === "") return { targets: [], errors }

  let parsed: unknown
  try {
    parsed = JSON.parse(raw)
  } catch (e) {
    return { targets: [], errors: [`LOCAL_INFER_TARGETS 不是合法 JSON：${(e as Error).message}`] }
  }
  if (!Array.isArray(parsed)) {
    const kind = parsed === null ? "null" : typeof parsed
    return { targets: [], errors: [`LOCAL_INFER_TARGETS 必须是 JSON 数组（形如 [${CONFIG_EXAMPLE}]），当前是 ${kind}`] }
  }

  const targets: TargetSpec[] = []
  const seen = new Set<string>()
  parsed.forEach((item, i) => {
    const idx = i + 1
    if (!item || typeof item !== "object" || Array.isArray(item)) {
      errors.push(`第 ${idx} 项不是对象（应为 ${CONFIG_EXAMPLE}），已跳过`)
      return
    }
    const o = item as Record<string, unknown>
    const name = str(o.name)
    const base = str(o.base_url)
    if (!name) {
      errors.push(`第 ${idx} 项缺少 name，已跳过`)
      return
    }
    if (!base) {
      errors.push(`目标 ${name} 缺少 base_url，已跳过`)
      return
    }
    if (!isHttpUrl(base)) {
      errors.push(`目标 ${name} 的 base_url 不是 http(s) 端点（${base}），已跳过`)
      return
    }
    if (name === LOCAL_NAME) {
      errors.push(`目标名 ${LOCAL_NAME} 是本机目标的保留名，已跳过（选本机用 target 缺省或 target="local"）`)
      return
    }
    if (seen.has(name)) {
      errors.push(`目标名 ${name} 重复（第 ${idx} 项），已保留先声明的一个`)
      return
    }
    seen.add(name)

    const spec: TargetSpec = { name, base_url: normalizeBaseUrl(base) }
    const apiKey = str(o.api_key)
    const model = str(o.model)
    const note = str(o.note)
    if (apiKey) spec.api_key = apiKey
    if (model) spec.model = model
    if (note) spec.note = note
    targets.push(spec)
  })
  return { targets, errors }
}

// ── 目标解析 ──────────────────────────────────────────────────────────────

/** 本机目标：状态文件里的实例优先（多实例取端口最小的一个），否则 LOCAL_INFER_PORT / 8080。 */
function resolveLocalTarget(env: Record<string, string>, apiKey?: string): InferTarget {
  let states: ServerState[] = []
  try {
    states = readServerStates(inferHome(env))
  } catch {
    states = [] // 状态目录不可读不该阻断调用：退回端口推断
  }

  const fallback = defaultPort(env)
  const head = states[0]
  const port = head?.port ?? fallback
  const target: InferTarget = { name: LOCAL_NAME, kind: "local", baseUrl: baseUrl(port) }

  const model = str(head?.model)
  if (head && model) target.model = model

  if (head) {
    const profile = str(head.profile)
    let note = `本机受管实例，端口 ${port}${profile ? `，档位 ${profile}` : ""}${model ? `，模型 ${model}` : ""}`
    if (states.length > 1) {
      const others = states.slice(1).map((s) => s.port).join("/")
      note += `；另有 ${states.length - 1} 个实例在跑（端口 ${others}），本次取端口最小的一个`
    }
    target.note = note
  } else {
    target.note = `无服务实例状态文件，按 LOCAL_INFER_PORT 或 8080 推断为端口 ${port}；服务可能尚未启动`
  }

  if (apiKey) target.apiKey = apiKey
  return target
}

/** 未知目标的可读报错：点名问题 + 列出可用目标 + 怎么配（含环境变量解析告警）。 */
function unknownTargetError(key: string, targets: TargetSpec[], errors: string[]): string {
  const names = targets.map((t) => t.name)
  const lines = [
    `未知推理目标 "${key}"：既不是 local、也不是 http(s) 端点、也不在命名目标里。`,
    names.length ? `可用命名目标：${names.join("、")}` : "当前没有命名目标（LOCAL_INFER_TARGETS 未配置或全部无效）。",
    '也可以直接给端点：target="http://192.168.1.20:8080"（任意 OpenAI 兼容端点）。',
    `配置命名目标（环境变量 LOCAL_INFER_TARGETS，JSON 数组）：LOCAL_INFER_TARGETS='[${CONFIG_EXAMPLE}]'`,
  ]
  if (errors.length) lines.push(`LOCAL_INFER_TARGETS 解析告警：${errors.slice(0, 5).join("；")}`)
  return lines.join("\n")
}

/**
 * 解析本次调用要用的目标：
 *   * `undefined` / `"local"` → 本机（状态文件 > LOCAL_INFER_PORT > 8080）；
 *   * `http(s)://…` → 直连该端点；
 *   * 其它 → 按命名目标查 LOCAL_INFER_TARGETS（未知名返回 `{ error }`，**不抛异常**）。
 *
 * api_key 优先级：显式参数 > 命名目标字段 > 环境变量 `LOCAL_INFER_REMOTE_API_KEY`。
 */
export function resolveTarget(
  name: string | undefined,
  ctx: ToolContext,
  explicitKey?: string,
): InferTarget | { error: string } {
  const env = ctx?.env ?? {}
  const key = str(name) ?? LOCAL_NAME
  const explicit = str(explicitKey)
  const envKey = str(env["LOCAL_INFER_REMOTE_API_KEY"])

  if (key === LOCAL_NAME) return resolveLocalTarget(env, explicit ?? envKey)

  if (isHttpUrl(key)) {
    const url = normalizeBaseUrl(key)
    const target: InferTarget = {
      name: url,
      kind: "remote",
      baseUrl: url,
      note: "由 target 参数直接给出的端点，不由本机服务管理",
    }
    const apiKey = explicit ?? envKey
    if (apiKey) target.apiKey = apiKey
    return target
  }

  const { targets, errors } = parseTargetsEnv(env)
  const hit = targets.find((t) => t.name === key)
  if (!hit) return { error: unknownTargetError(key, targets, errors) }

  const target: InferTarget = { name: hit.name, kind: "remote", baseUrl: hit.base_url }
  const apiKey = explicit ?? hit.api_key ?? envKey
  if (apiKey) target.apiKey = apiKey
  if (hit.model) target.model = hit.model
  if (hit.note) target.note = hit.note
  return target
}

// ── 鉴权头 / 探活 / 人读呈现 ──────────────────────────────────────────────

/** 目标的鉴权请求头（无密钥时为空对象——本机端点不需要任何头）。 */
export function targetHeaders(t: InferTarget): Record<string, string> {
  return t.apiKey ? { authorization: `Bearer ${t.apiKey}` } : {}
}

/** 探活 /props + /health（远端端点带上鉴权头，否则只会拿到 401）。 */
export async function probeTarget(
  t: InferTarget,
  opts: { fetchImpl?: typeof fetch; timeoutMs?: number } = {},
): Promise<ProbeResult> {
  try {
    return await probe(t.baseUrl, { fetchImpl: opts.fetchImpl, timeoutMs: opts.timeoutMs, headers: targetHeaders(t) })
  } catch (e) {
    return { ok: false, error: (e as Error).message ?? String(e) }
  }
}

/** 密钥掩码：只留头 3 尾 3（短密钥全掩）——人读输出里**任何**位置都不得出现原文。 */
function maskSecret(key: string): string {
  const k = key.trim()
  if (k.length <= 8) return "***"
  return `${k.slice(0, 3)}***${k.slice(-3)}`
}

/** 人读单行（掩码密钥）：`local ｜ 本机 ｜ http://127.0.0.1:8080 ｜ 本机受管实例，端口 8080`。 */
export function targetSummary(t: InferTarget): string {
  const parts = [t.name, t.kind === "local" ? "本机" : "远端", t.baseUrl]
  if (t.model) parts.push(`模型 ${t.model}`)
  if (t.apiKey) parts.push(`key ${maskSecret(t.apiKey)}`)
  const head = parts.join(" ｜ ")
  return t.note ? `${head} ｜ ${t.note}` : head
}

/** 结构判定（js 编排里透传的 target 对象可据此验形）。 */
export function isInferTarget(v: unknown): v is InferTarget {
  if (!v || typeof v !== "object") return false
  const o = v as Record<string, unknown>
  if (typeof o.name !== "string" || typeof o.baseUrl !== "string") return false
  if (o.kind !== "local" && o.kind !== "remote") return false
  const optOk = (x: unknown): boolean => x === undefined || typeof x === "string"
  return optOk(o.apiKey) && optOk(o.model) && optOk(o.note)
}

// ── 工具：targets（只读） ─────────────────────────────────────────────────

const schema = (properties: Record<string, unknown>, required: string[] = []): ToolSchema => ({ type: "object", properties, required })

/** 探活结论的一行（人读）。 */
function probeLine(p: ProbeResult): string {
  if (!p.ok) return `不可用（${p.error ?? "探测失败"}）`
  const bits = [`可用 HTTP ${p.health_status ?? "?"}`]
  if (p.n_ctx) bits.push(`n_ctx=${p.n_ctx}`)
  if (p.total_slots) bits.push(`slots=${p.total_slots}`)
  if (p.model_path) bits.push(`模型 ${p.model_path.split(/[\\/]/).pop()}`)
  const line = bits.join(" ｜ ")
  return p.error ? `${line}（${p.error}）` : line
}

/** 目标 → 结构化视图（密钥只给掩码，原文不出本层）。 */
function targetData(t: InferTarget): Record<string, unknown> {
  const out: Record<string, unknown> = { name: t.name, kind: t.kind, base_url: t.baseUrl, has_api_key: Boolean(t.apiKey) }
  if (t.apiKey) out.api_key_masked = maskSecret(t.apiKey)
  if (t.model) out.model = t.model
  if (t.note) out.note = t.note
  return out
}

const targetsTool: Tool = {
  name: "targets",
  safeMode: true, // 只读：列目标 + GET 探活（/health、/props），不发写操作
  description:
    "列出可用的推理目标——推理不限于本机：local（本机受管 llama-server，端口取服务实例状态文件或 LOCAL_INFER_PORT）" +
    "与 LOCAL_INFER_TARGETS 环境变量声明的命名目标（局域网另一台机器 / 云端 OpenAI 兼容端点），密钥一律掩码显示。" +
    "probe=true 时逐个探活（GET /health + /props），给出可用性、n_ctx、slots（并发上限）与模型名——" +
    "批量条目长度预算与并发上限都以它为依据。只读、免审批。",
  parameters: schema({
    probe: { type: "boolean", description: "是否逐个探活（缺省 false；探测只发 GET，不发写操作）" },
  }),
  async execute(args, ctx) {
    const env = ctx?.env ?? {}
    const { targets: named, errors } = parseTargetsEnv(env)

    const list: InferTarget[] = []
    const local = resolveTarget(LOCAL_NAME, ctx)
    if (!("error" in local)) list.push(local) // local 恒可解析（防御性判断：解析失败也要能列出命名目标）
    for (const s of named) {
      const t: InferTarget = { name: s.name, kind: "remote", baseUrl: s.base_url }
      if (s.api_key) t.apiKey = s.api_key
      if (s.model) t.model = s.model
      if (s.note) t.note = s.note
      list.push(t)
    }

    const doProbe = args.probe === true
    const probes = doProbe ? await Promise.all(list.map((t) => probeTarget(t, { timeoutMs: 3000 }))) : []

    const localCount = list.filter((t) => t.kind === "local").length
    const lines = [`推理目标（${list.length} 个：${localCount} 本机 + ${list.length - localCount} 远端）`, ""]
    list.forEach((t, i) => {
      lines.push(`· ${targetSummary(t)}`)
      if (probes[i]) lines.push(`    └ 探活：${probeLine(probes[i])}`)
    })

    if (errors.length) {
      lines.push("", `LOCAL_INFER_TARGETS 解析告警（${errors.length} 条）：`)
      for (const e of errors.slice(0, 10)) lines.push(`  - ${e}`)
    }
    lines.push("")
    if (!named.length) {
      lines.push(`尚未配置命名目标。配一个即可跑在局域网/云端：LOCAL_INFER_TARGETS='[${CONFIG_EXAMPLE}]'`)
    }
    lines.push(
      '选用：generate / batch 传 target="<名字>" 或 target="http://host:port"（缺省 local，即本机受管实例）。',
      "密钥：命名目标配 api_key 字段，或本次调用传 api_key 参数，或设 LOCAL_INFER_REMOTE_API_KEY（优先级同此顺序）。",
      "注意：远端目标不受本机档位（profiles.json）约束——并发按服务端 slots 自行控制。",
    )

    return {
      output: lines.join("\n"),
      data: {
        count: list.length,
        local: list[0] && list[0].kind === "local" ? targetData(list[0]) : undefined,
        targets: list.map(targetData),
        parse_errors: errors,
        ...(doProbe
          ? {
              probes: list.map((t, i) => ({
                name: t.name,
                ok: probes[i]?.ok ?? false,
                n_ctx: probes[i]?.n_ctx,
                total_slots: probes[i]?.total_slots,
                model_path: probes[i]?.model_path,
                error: probes[i]?.error,
              })),
            }
          : {}),
      },
    }
  },
}

export const tools: Record<string, Tool> = { targets: targetsTool }
/** 免审批表为空：targets 只读（不含任何写操作），其余工具在 tasks.ts 里声明。 */
export const requiresApproval: Record<string, boolean> = {}
