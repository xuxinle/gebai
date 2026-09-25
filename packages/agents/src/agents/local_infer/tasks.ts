/**
 * local_infer 的推理任务层：单条提交（generate）、批量提交（batch）、批次进度与结果（jobs）。
 *
 * 分工：`api.ts` 负责「怎么调」（请求构造 / 解析 / 调度），`paths.ts` 负责「存哪」（任务目录与进度文件），
 * `providers.ts` 负责「打哪个端点」（本机受管服务 / 直连 URL / 命名目标），本文件只做参数解析、产物落盘、
 * 人类可读摘要与结构化 data——保证无 GPU 环境可用 mock 端点全链路验证。
 *
 * 推理目标（target）：generate/batch 都接受 `target`（缺省 local = 本机受管服务）与 `api_key`，
 * 于是同一套工具既能跑本机 CPU 引擎，也能跑局域网另一台机器或云端 OpenAI 兼容端点；
 * 目标解析失败一律转成可读输出（列出可用目标与配置指引），不抛异常。
 */
import { appendFileSync, existsSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import type { Tool, ToolContext, ToolSchema } from "@gebai/sdk"
import {
  chatOnce,
  runBatch,
  type BatchItem,
  type BatchResult,
  type BatchSummary,
  type ChatMessage,
  type InferResponse,
  type InferTuning,
  type ProbeResult,
  type StructuredOutput,
} from "./api"
import { baseUrl, inferHome, isAbsolutePath, listBatchJobs, loadProfiles, profileModel, readBatchJob, readServerStates, runsDir, type BatchJobEntry } from "./paths"
import { parseTargetsEnv, probeTarget, resolveTarget, targetHeaders, targetSummary, type InferTarget } from "./providers"

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

const num = (v: unknown): number | undefined => (typeof v === "number" && Number.isFinite(v) ? v : undefined)

/** 环境变量取数（环境变量恒为字符串；空串与非法值当作未配置）。 */
const envNum = (v: string | undefined): number | undefined => {
  if (v == null || v === "") return undefined
  const n = Number(v)
  return Number.isFinite(n) ? n : undefined
}

/** 参数 → 结构化约束（schema > grammar > json_object；都未给则 undefined = 不约束）。 */
function structuredFrom(args: Record<string, unknown>): StructuredOutput | undefined {
  const out: StructuredOutput = {}
  if (args.schema && typeof args.schema === "object" && !Array.isArray(args.schema)) out.schema = args.schema as Record<string, unknown>
  if (typeof args.grammar === "string" && args.grammar.trim()) out.grammar = args.grammar
  if (args.json_object === true) out.json_object = true
  if (args.retry_on_invalid != null) out.retry_on_invalid = args.retry_on_invalid !== false
  return out.schema || out.grammar || out.json_object ? out : undefined
}

/** 参数 → 采样调参（未给的字段不写入，交给服务端默认/批量默认）。 */
function tuningFrom(args: Record<string, unknown>): InferTuning {
  const t: InferTuning = {}
  const temperature = num(args.temperature)
  if (temperature != null) t.temperature = temperature
  const topP = num(args.top_p)
  if (topP != null) t.top_p = topP
  const maxTokens = num(args.max_tokens)
  if (maxTokens != null) t.max_tokens = maxTokens
  const seed = num(args.seed)
  if (seed != null) t.seed = seed
  if (typeof args.enable_thinking === "boolean") t.enable_thinking = args.enable_thinking
  return t
}

const ROLES = new Set(["system", "user", "assistant", "tool"])

// ── 后台批次与取消标记 ────────────────────────────────────────────────────

/** api.runBatch 给「还没开始就被取消」的条目补的占位失败（attempts=0）：不落盘也不计数。 */
const CANCEL_FILLER = "任务已取消"

/** 本进程正在跑的批次（key = 任务目录绝对路径）：jobs 的 cancel 据此真中止，status 据此判「在跑」。 */
const runningBatches = new Map<string, { controller: AbortController }>()

/** 心跳过期阈值：状态仍为 running 但 job.json 超该时长未更新 → 提示可能卡住。 */
const HEARTBEAT_STALE_MS = 10 * 60 * 1000

/** 删文件（不存在不算错）。 */
function rmQuiet(file: string): void {
  try {
    rmSync(file, { force: true })
  } catch {
    /* 忽略 */
  }
}

/** 轮询等待条件成立（取消收尾用）。 */
async function waitFor(pred: () => boolean, timeoutMs: number, stepMs = 100): Promise<boolean> {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (pred()) return true
    await new Promise((r) => setTimeout(r, stepMs))
  }
  return pred()
}

/** 探活命令（平台作显式参数：Windows/POSIX 双轨可测，不随宿主平台漂移）。 */
export function pidAliveCmd(pid: number, platform: NodeJS.Platform = process.platform): string {
  return platform === "win32" ? `tasklist /FI "PID eq ${pid}" /FO CSV /NH` : `kill -0 ${pid} 2>/dev/null`
}

/** 探活结论：win32 看 tasklist 输出是否含该 PID（CSV 里引号包裹），POSIX 看 kill -0 的退出码。 */
export function pidAliveVerdict(pid: number, out: { stdout: string; code: number }, platform: NodeJS.Platform = process.platform): boolean {
  return platform === "win32" ? new RegExp(`"${pid}"`).test(out.stdout) : out.code === 0
}

/** 批次属主进程是否存活：本进程直接为真；探活命令失败时保守返回 true（不确定就不误判为中断）。 */
async function ownerAlive(pid: number, ctx: ToolContext): Promise<boolean> {
  if (pid === process.pid) return true
  try {
    return pidAliveVerdict(pid, await ctx.runCommand(pidAliveCmd(pid), { timeoutMs: 15000 }))
  } catch {
    return true
  }
}

/** 心跳是否过期（updated_at 缺失或不可解析时不判定）。 */
function heartbeatStale(updatedAt?: string): boolean {
  if (!updatedAt) return false
  const t = Date.parse(updatedAt)
  return Number.isFinite(t) && Date.now() - t > HEARTBEAT_STALE_MS
}

/** job.json 的扩展视图：后台执行字段（旧记录没有这些字段，按 undefined 处理——类型留在本文件，不侵入 paths 的存储契约）。 */
interface BatchJobView extends BatchJobEntry {
  background?: boolean
  owner_pid?: number
  /** 推理目标（providers 新增字段；旧记录没有 → 展示时按 local 处理）。 */
  target?: string
  /** 目标端点基址（不含密钥）。 */
  target_base_url?: string
}

/** 读改写 job.json（保留未知字段）；失败静默返回 null（状态文件损坏不阻断查询）。 */
function patchJobFile(dir: string, patch: Record<string, unknown>): BatchJobView | null {
  const jp = join(dir, "job.json")
  if (!existsSync(jp)) return null
  try {
    const cur = JSON.parse(readFileSync(jp, "utf-8")) as BatchJobView
    const next = { ...cur, ...patch, updated_at: new Date().toISOString() }
    writeFileSync(jp, `${JSON.stringify(next, null, 2)}\n`, "utf-8")
    return next
  } catch {
    return null
  }
}

/** 参数 messages → 契约消息列表（角色非法时按 user 处理，不因模型幻觉的 role 值报错）。 */
function parseMessages(v: unknown): ChatMessage[] | undefined {
  if (!Array.isArray(v) || !v.length) return undefined
  return v.map((m) => {
    const o = (m ?? {}) as Record<string, unknown>
    const role = String(o.role ?? "user")
    return {
      role: (ROLES.has(role) ? role : "user") as ChatMessage["role"],
      content: typeof o.content === "string" ? o.content : JSON.stringify(o.content ?? ""),
    }
  })
}

/** 单条请求超时：显式参数 > LOCAL_INFER_TIMEOUT_MS > undefined（chatOnce 默认 600000）。 */
function resolveTimeout(args: Record<string, unknown>, env: Record<string, string>): number | undefined {
  return num(args.timeout_ms) ?? envNum(env.LOCAL_INFER_TIMEOUT_MS)
}

// ── 推理目标（providers）接入 ─────────────────────────────────────────────

/** 目标端点的端口（沿用既有的 port 字段/展示；远端目标同样适用）。 */
function portOf(t: InferTarget): number {
  const m = /:(\d+)(?:\/|$)/.exec(t.baseUrl)
  if (m) return Number(m[1])
  return t.baseUrl.startsWith("https") ? 443 : 80
}

/** 显式 port 参数（历史用法）对 local 目标仍生效：给了就以该端口为准，覆盖状态文件推断。 */
function withPortOverride(t: InferTarget, args: Record<string, unknown>): InferTarget {
  const p = num(args.port)
  if (p == null || t.kind !== "local") return t
  const url = baseUrl(p)
  if (url === t.baseUrl) return t
  return { ...t, baseUrl: url, note: `${t.note ? `${t.note}；` : ""}端口参数覆盖为 ${p}` }
}

/** 目标解析失败的人读输出：解析原因 + 可用目标 + 怎么配 LOCAL_INFER_TARGETS（不抛异常）。 */
function targetErrorText(error: string, ctx: ToolContext): string {
  const { targets: named } = parseTargetsEnv(ctx.env ?? {})
  const lines = [`推理目标解析失败：${error}`, ""]
  lines.push(`可用目标：local（本机受管实例，缺省）${named.length ? `、${named.map((t) => t.name).join("、")}` : "（当前没有有效的命名目标）"}`)
  lines.push('也可直接给端点：target="http://192.168.1.20:8080"（任意 OpenAI 兼容端点，可再传 api_key 参数）。')
  lines.push("配置命名目标（环境变量 LOCAL_INFER_TARGETS，JSON 数组）：")
  lines.push('  LOCAL_INFER_TARGETS=\'[{"name":"lan","base_url":"http://192.168.1.20:8080","api_key":"sk-..."}]\'')
  lines.push("用 local_infer_targets 查看全部可用目标（可加 probe=true 探活）。")
  return lines.join("\n")
}

/** 解析本次调用的推理目标（target/api_key/port 三个参数 + 环境变量）；失败返回可读错误而不抛。 */
function pickTarget(args: Record<string, unknown>, ctx: ToolContext): { target: InferTarget } | { error: string } {
  const explicitKey = typeof args.api_key === "string" && args.api_key.trim() ? args.api_key.trim() : undefined
  const r = resolveTarget(typeof args.target === "string" ? args.target : undefined, ctx, explicitKey)
  if ("error" in r) return { error: targetErrorText(r.error, ctx) }
  return { target: withPortOverride(r, args) }
}

/** 服务端能力提示：n_ctx 窗口与单条 prompt 的长度预算（批量条目过长会被截断或直接报错）。 */
function capacityNote(p: ProbeResult, maxTokens?: number): string | undefined {
  if (!p.ok || !p.n_ctx) return undefined
  const reserve = maxTokens && maxTokens > 0 ? maxTokens : 1024
  const budget = Math.max(0, p.n_ctx - reserve)
  return (
    `服务端窗口 n_ctx=${p.n_ctx}${p.total_slots ? `／slots=${p.total_slots}` : ""}：单条 prompt+输出须落在该窗口内；` +
    `按输出预留 ${reserve} token 计，单条 prompt 约可用 ${budget} token（中文约 1 字/token）——批量条目过长会被截断或直接报错。`
  )
}

/** 探活失败的一行提示（不阻断本次调用：请求照样发，失败原因由 api 层如实报告）。 */
function probeWarn(p: ProbeResult, kind: InferTarget["kind"]): string {
  const why = kind === "local" ? "本机服务可能未启动" : "远端端点可能不可达或密钥不正确"
  return `注意：目标探活未通过（${p.error ?? "探测失败"}）——${why}；仍按本次提交执行。`
}

/** 本地时间戳（任务 id 用，肉眼可读且可排序）。 */
function stamp(d = new Date()): string {
  const p = (n: number, w = 2) => String(n).padStart(w, "0")
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`
}

/** 结果概览的一行（生成/批量共用）。 */
function timingLine(r: { elapsed_ms: number; attempts: number; decode_tps?: number; usage?: { prompt_tokens?: number; completion_tokens?: number } }): string {
  const parts = [`耗时 ${(r.elapsed_ms / 1000).toFixed(2)} s`, `attempts ${r.attempts}`]
  if (r.decode_tps) parts.push(`解码 ${r.decode_tps.toFixed(1)} t/s（服务端内部计时）`)
  if (r.usage) parts.push(`tokens in ${r.usage.prompt_tokens ?? "?"} / out ${r.usage.completion_tokens ?? "?"}`)
  return parts.join(" ｜ ")
}

/** 正文与思维链的分段呈现（推理型模型必须两者都看）。 */
function textSections(content: string, reasoning: string, limit = 4000): string[] {
  const out: string[] = []
  const body = content ?? ""
  out.push(`正文（content，${body.length} 字符）：`)
  out.push(body.length > limit ? `${body.slice(0, limit)}\n…（已截断 ${body.length - limit} 字符）` : body || "（空）")
  if (reasoning) {
    const head = reasoning.length > 400 ? `${reasoning.slice(0, 400)}…` : reasoning
    out.push("", `思维链（reasoning_content，${reasoning.length} 字符，${reasoning.length > 400 ? "前 400 字" : "全文"}）：`)
    out.push(head)
  }
  return out
}

/** 结构化结果的呈现（成功缩进 JSON，失败给错误与原始输出片段）。 */
function structuredSections(r: InferResponse): string[] {
  if (!r.structured_via || r.structured_via === "none") return []
  const out: string[] = [""]
  if (r.json_errors?.length) {
    out.push(`结构化输出校验未通过（${r.json_errors.length} 处，约束来源 ${r.structured_via}）：`)
    for (const e of r.json_errors.slice(0, 10)) out.push(`  - ${e}`)
    out.push("原始输出片段：", (r.json_raw ?? r.content).slice(0, 800))
  } else {
    out.push(`结构化输出（约束来源 ${r.structured_via}）：`)
    out.push(JSON.stringify(r.json, null, 2).slice(0, 4000))
  }
  return out
}

// ── generate：单条推理 ────────────────────────────────────────────────────

const generate: Tool = {
  name: "generate",
  safeMode: false, // 安全模式下不提供：会向推理端点发起请求（占 GPU 且属外发请求）
  description:
    '向推理目标提交一条推理任务并取回结果（OpenAI 兼容 /v1/chat/completions）：缺省打本机受管服务，' +
    '也可用 target 指向局域网另一台机器或云端 OpenAI 兼容端点（target="http://host:port" 或 LOCAL_INFER_TARGETS 里声明的目标名）。支持 messages 或单轮 prompt；' +
    "传入 schema（JSON Schema，服务端转 GBNF 约束解码）、grammar（GBNF 语法）或 json_object 可得结构化输出，工具侧做解析与校验，" +
    "校验不过会把错误回灌给模型重试一次（retry_on_invalid=false 可关）。返回正文（content）、思维链（reasoning_content）、实测解码速度与结构化结果。" +
    "推理型模型（如 Qwen-AgentWorld）的思维链可占输出九成以上，批量/工具类任务建议 enable_thinking=false；单次调用会占用算力数十秒到数分钟。免审批（不改变服务状态）。",
  parameters: schema({
    prompt: { type: "string", description: "单轮输入（与 messages 二选一）" },
    messages: {
      type: "array",
      description: "多轮消息（与 prompt 二选一），元素 { role, content }",
      items: { type: "object", properties: { role: { type: "string" }, content: { type: "string" } }, required: ["role", "content"] },
    },
    system: { type: "string", description: "系统提示（与 prompt/messages 搭配时置前）" },
    temperature: { type: "number", description: "采样温度（缺省 0，确定性输出）" },
    top_p: { type: "number", description: "核采样" },
    max_tokens: { type: "number", description: "最大生成 token 数（= llama.cpp 的 n_predict）" },
    seed: { type: "number", description: "随机种子（可复现实验）" },
    enable_thinking: { type: "boolean", description: "是否启用思维链（false 可显著提速省 token；推理型任务才需要 true）" },
    schema: { type: "object", description: "JSON Schema：要求模型输出严格符合该结构（服务端 GBNF 约束解码，推荐的结构化手段）" },
    grammar: { type: "string", description: "GBNF 语法文本（llama.cpp 原生约束；注意与 -bs 后端采样不兼容，会回退 CPU 采样）" },
    json_object: { type: "boolean", description: "仅要求合法 JSON（不约束结构；schema 不可用时的降级手段）" },
    retry_on_invalid: { type: "boolean", description: "结构化校验失败时把错误回灌给模型重试一次（缺省 true）" },
    port: { type: "number", description: "本机服务端口（只对缺省的本机目标有意义；缺省按状态文件 / LOCAL_INFER_PORT / 8080）" },
    timeout_ms: { type: "number", description: "单条请求超时毫秒数（缺省 600000，本地模型很慢，勿设得过小）" },
    target: {
      type: "string",
      description:
        '推理目标：缺省 local（本机受管服务，端口取状态文件 / LOCAL_INFER_PORT）；也可给命名目标（LOCAL_INFER_TARGETS 里声明的 name）' +
        '或直连 URL（target="http://192.168.1.20:8080" / "https://..."）。用 local_infer_targets 查看可用目标。',
    },
    api_key: { type: "string", description: "端点鉴权密钥（仅对直连 URL / 命名目标有意义；不传时用命名目标的 api_key 字段或 LOCAL_INFER_REMOTE_API_KEY）" },
  }),
  async execute(args, ctx) {
    const home = inferHome(ctx.env)
    const messages = parseMessages(args.messages)
    const prompt = typeof args.prompt === "string" ? args.prompt : undefined
    if (!messages && prompt == null) {
      return { output: "需要 prompt 或 messages 之一：prompt 用于单轮问答，messages 用于多轮（元素 { role, content }）。" }
    }

    // 目标先解析：目标不对就不发请求（错误输出里直接给出可用目标与怎么配）
    const picked = pickTarget(args, ctx)
    if ("error" in picked) return { output: picked.error }
    const target = picked.target
    const port = portOf(target)

    const structured = structuredFrom(args)
    // 探活（/health + /props）只作能力提示：给 n_ctx 窗口预算与「服务在不在」的判断，失败不阻断提交
    const probe = await probeTarget(target, { timeoutMs: 3000 })
    const resp = await chatOnce(
      {
        messages,
        prompt,
        system: typeof args.system === "string" ? args.system : undefined,
        ...tuningFrom(args),
        structured,
      },
      { baseUrl: target.baseUrl, timeoutMs: resolveTimeout(args, ctx.env), signal: ctx.signal, headers: targetHeaders(target) },
    )

    const lines = [
      `推理${resp.ok ? "完成" : "失败"}（目标 ${targetSummary(target)}${existsSync(home) ? "" : "；未找到 infer 子项目，按直连端点处理"}）`,
      timingLine(resp),
    ]
    if (resp.http_status) lines.push(`HTTP ${resp.http_status}`)
    if (resp.warning) lines.push(`注意：${resp.warning}`)
    if (resp.error) lines.push(`错误：${resp.error}`)
    const budgetNote = capacityNote(probe, num(args.max_tokens))
    if (budgetNote) lines.push(budgetNote)
    if (!probe.ok) lines.push(probeWarn(probe, target.kind))
    lines.push("", ...textSections(resp.content, resp.reasoning), ...structuredSections(resp))
    // 思维链提示按实际输出给：非推理型模型（如小模型）本就不会返回 reasoning_content，不该报“本模型是推理型”
    lines.push(
      "",
      resp.reasoning
        ? "提示：已返回思维链（reasoning_content）——正文看 content、思考过程看 reasoning_content，两者都要看。"
        : "提示：本次未返回思维链（模型可能不是推理型，或已关闭思考模式）；推理型模型（如 Qwen-AgentWorld）的思考过程在 reasoning_content。",
    )

    return {
      output: lines.join("\n"),
      data: {
        ok: resp.ok,
        port,
        target: { name: target.name, kind: target.kind, base_url: target.baseUrl, model: target.model },
        target_summary: targetSummary(target),
        probe: { ok: probe.ok, n_ctx: probe.n_ctx, total_slots: probe.total_slots, model_path: probe.model_path, error: probe.error },
        content: resp.content,
        reasoning: resp.reasoning,
        json: resp.json,
        json_errors: resp.json_errors,
        structured_via: resp.structured_via,
        usage: resp.usage,
        timings: resp.timings,
        decode_tps: resp.decode_tps,
        elapsed_ms: resp.elapsed_ms,
        attempts: resp.attempts,
        error: resp.error,
        warning: resp.warning,
      },
    }
  },
}

// ── batch：批量提交 ───────────────────────────────────────────────────────

/** 读条目文件：.jsonl（每行一条）或 .json（数组）。 */
function readItemsFile(path: string): { items?: BatchItem[]; error?: string } {
  if (!existsSync(path)) return { error: `条目文件不存在：${path}` }
  let text: string
  try {
    text = readFileSync(path, "utf-8")
  } catch (e) {
    return { error: `条目文件读取失败：${(e as Error).message}` }
  }
  const trimmed = text.trim()
  if (!trimmed) return { items: [] }
  if (trimmed.startsWith("[")) {
    try {
      const arr = JSON.parse(trimmed) as unknown
      if (!Array.isArray(arr)) return { error: "条目文件不是 JSON 数组" }
      return { items: arr as BatchItem[] }
    } catch (e) {
      return { error: `条目文件 JSON 解析失败：${(e as Error).message}` }
    }
  }
  const items: BatchItem[] = []
  const bad: number[] = []
  trimmed.split(/\r?\n/).forEach((line, i) => {
    const s = line.trim()
    if (!s || s.startsWith("#") || s.startsWith("//")) return
    try {
      items.push(JSON.parse(s) as BatchItem)
    } catch {
      bad.push(i + 1)
    }
  })
  if (bad.length) return { error: `条目文件第 ${bad.slice(0, 5).join("/")}${bad.length > 5 ? " 等" : ""} 行不是合法 JSON（JSONL 要求每行一条）` }
  return { items }
}

/** 写 JSONL（每条一行；覆盖写）。 */
function writeJsonl(file: string, rows: unknown[]): void {
  writeFileSync(file, rows.map((r) => JSON.stringify(r)).join("\n") + (rows.length ? "\n" : ""), "utf-8")
}

/** 读结果 JSONL（半行/坏行忽略——并发追加时可能读到写了一半的尾部）。 */
function readResults(file: string): BatchResult[] {
  if (!existsSync(file)) return []
  const out: BatchResult[] = []
  for (const line of readFileSync(file, "utf-8").split(/\r?\n/)) {
    const s = line.trim()
    if (!s) continue
    try {
      out.push(JSON.parse(s) as BatchResult)
    } catch {
      /* 忽略坏行 */
    }
  }
  return out
}

/** 并发度与档位 parallel 比对：超出即夹取（并发只在短 prompt 批量场景有收益，绝不能无脑放大）。 */
function clampConcurrency(home: string, env: Record<string, string>, requested: number): { concurrency: number; note?: string } {
  const prof = loadProfiles(home)
  if (!prof) return { concurrency: requested }
  const active = env.LOCAL_INFER_PROFILE ?? prof.default_profile
  const parallel = prof.profiles[active]?.parallel ?? 1
  if (requested > parallel) {
    return {
      concurrency: parallel,
      note:
        `并发 ${requested} 超过档位 ${active} 的 parallel=${parallel}，已夹取到 ${parallel}。` +
        "并发仅在「短 prompt + slot 数匹配」的批量场景有效（实测 np=16 短 prompt 聚合 627.8 t/s）；" +
        "GEBAI 的 15K prompt 场景并发无增益（预填充占主导），应改用 concurrent 档，详见 infer/README.md。",
    }
  }
  return { concurrency: requested }
}

const batch: Tool = {
  name: "batch",
  safeMode: false, // 安全模式下不提供：会发起大量推理请求并写产物文件
  description:
    "批量提交推理任务（限流并发 + 逐条落盘 + 断点续跑），产物写入 infer/bench/runs/<job_id>/：items.jsonl（输入副本）、" +
    "results.jsonl（每行一条结果，可实时 tail）、job.json（进度）。条目来自 items 数组或 items_file（JSONL/JSON 数组，路径相对 infer 根）；" +
    "schema/grammar/json_object 对全批生效（结构化输出，条目自带则覆盖）。同一 job_id 再次调用按 id 跳过已完成项（resume，缺省开），" +
    "max_items 可分批推进（只处理前 N 条未完成项）。并发 concurrency 会与档位 parallel 比对并夹取——并发只在短 prompt 批量场景有收益。" +
    "target 指定推理目标（缺省 local = 本机受管服务，也可为命名目标或直连 URL）：job.json 会记下目标名与端点（不含密钥），" +
    "便于追溯每批结果出自哪台机器；同一 job_id 换目标续跑会给出提示但仍按本次目标执行。" +
    "缺省同步执行（本次调用等批次跑完，条目多时耗时很长）；background=true 则立即返回 job_id，批次随服务进程在后台逐条落盘，" +
    "用 local_infer_jobs 查进度/取消（服务重启会留下「已中断」状态与完整产物，可 resume 续跑）。免审批（不改变服务状态）。",
  parameters: schema({
    items: {
      type: "array",
      description: "条目数组，元素 { id?, messages?|prompt?, system?, meta?, 及可选 temperature/max_tokens/enable_thinking/structured }",
      items: { type: "object" },
    },
    items_file: { type: "string", description: "条目文件（.jsonl 每行一条，或 .json 数组；相对 infer 根或绝对路径）" },
    schema: { type: "object", description: "全批默认的 JSON Schema（条目可自带 structured 覆盖）" },
    grammar: { type: "string", description: "全批默认的 GBNF 语法" },
    json_object: { type: "boolean", description: "全批仅要求合法 JSON" },
    concurrency: { type: "number", description: "并发度（缺省 1；会与档位 parallel 比对并夹取）" },
    retries: { type: "number", description: "单条失败后的重试次数（缺省 0；结构化校验不过也算失败）" },
    timeout_ms: { type: "number", description: "单条请求超时毫秒数（缺省 600000）" },
    max_tokens: { type: "number", description: "全批默认最大生成 token 数" },
    temperature: { type: "number", description: "全批默认采样温度（缺省 0）" },
    enable_thinking: { type: "boolean", description: "全批默认是否启用思维链（批量工具类任务建议 false）" },
    job_id: { type: "string", description: "任务标识（缺省 batch-<时间戳>；给定同 id 即续跑该任务）" },
    resume: { type: "boolean", description: "跳过 results.jsonl 中已成功的条目（缺省 true）" },
    max_items: { type: "number", description: "本次最多处理的未完成条目数（缺省 0 = 全部；用于分片推进）" },
    background: {
      type: "boolean",
      description:
        "后台执行（缺省 false = 同步，本次调用等批次跑完）：true 时立即返回 job_id，批次随服务进程在后台跑，用 local_infer_jobs 查进度/取消；" +
        "不随本次工具调用与会话中断，但随服务进程存活（服务重启 → jobs 报「已中断」，可 resume 续跑）",
    },
    port: { type: "number", description: "本机服务端口（只对缺省的本机目标有意义；缺省按状态文件 / LOCAL_INFER_PORT / 8080）" },
    target: {
      type: "string",
      description:
        '推理目标：缺省 local（本机受管服务）；也可为命名目标（LOCAL_INFER_TARGETS 里声明的 name）或直连 URL（target="http://192.168.1.20:8080"）。' +
        "远端目标不受本机档位（profiles.json）约束，并发请勿超过服务端 slots（见 local_infer_targets probe=true）。",
    },
    api_key: { type: "string", description: "端点鉴权密钥（仅对直连 URL / 命名目标有意义；不传时用命名目标的 api_key 字段或 LOCAL_INFER_REMOTE_API_KEY）" },
  }),
  async execute(args, ctx) {
    const home = inferHome(ctx.env)
    if (!existsSync(home)) return { output: `未找到本地推理子项目：${home}\n可通过 LOCAL_INFER_HOME 指定子项目根。` }
    const picked = pickTarget(args, ctx)
    if ("error" in picked) return { output: picked.error }
    const target = picked.target
    const port = portOf(target)

    // ① 条目来源：items 数组 或 items_file
    let items: BatchItem[] | undefined
    const itemsFileArg = typeof args.items_file === "string" && args.items_file.trim() ? args.items_file.trim() : undefined
    if (Array.isArray(args.items) && args.items.length) {
      items = args.items as BatchItem[]
    } else if (itemsFileArg) {
      const p = isAbsolutePath(itemsFileArg) ? itemsFileArg : join(home, itemsFileArg)
      const r = readItemsFile(p)
      if (r.error) return { output: r.error }
      items = r.items
    }
    if (!items?.length) {
      return {
        output:
          "需要 items 数组或 items_file 之一。\n" +
          '  items: [{"id":"q1","prompt":"问题一"},{"id":"q2","prompt":"问题二"}]\n' +
          '  items_file: bench/runs/questions.jsonl（每行一条：{"id":"q1","prompt":"..."}）\n' +
          "提示：条目必须含 messages 或 prompt；schema/grammar/json_object 对全批生效。",
      }
    }

    // ② 任务目录与断点
    const jobId = typeof args.job_id === "string" && args.job_id.trim() ? args.job_id.trim() : `batch-${stamp()}`
    // 上一次的任务记录：续跑提示用（目标是否换过、是否曾被取消）
    const prevJob = readBatchJob(home, jobId) as BatchJobView | null
    const prevState = prevJob?.state
    const targetChanged = typeof prevJob?.target === "string" && prevJob.target !== target.name
    const jobDir = join(runsDir(home), jobId)
    mkdirSync(jobDir, { recursive: true })
    const itemsPath = join(jobDir, "items.jsonl")
    const resultsPath = join(jobDir, "results.jsonl")
    const jobPath = join(jobDir, "job.json")

    const normalized: BatchItem[] = items.map((it, i) => ({ ...it, id: it.id != null ? String(it.id) : `item-${i}` }))
    const newItemsText = normalized.map((r) => JSON.stringify(r)).join("\n")
    const resume = args.resume !== false && typeof args.job_id === "string" && args.job_id.trim() !== ""

    let skipped = 0
    const doneIds = new Set<string>()
    if (resume) {
      for (const r of readResults(resultsPath)) if (r.ok) doneIds.add(String(r.id))
      skipped = normalized.filter((it) => doneIds.has(String(it.id))).length
    }

    const pending = normalized.filter((it) => !doneIds.has(String(it.id)))
    // 分片上限：显式 max_items 优先；否则用 LOCAL_INFER_BATCH_MAX_ITEMS 兜底（防一次提交把会话占死）。
    // 上限为 0 或未配置时不分片。
    const cap =
      num(args.max_items) && num(args.max_items)! > 0
        ? num(args.max_items)!
        : Math.max(0, Math.floor(envNum(ctx.env.LOCAL_INFER_BATCH_MAX_ITEMS) ?? 0))
    const slice = cap > 0 ? pending.slice(0, cap) : pending
    const remaining = pending.length - slice.length

    if (!slice.length) {
      return {
        output:
          `任务 ${jobId}（目标 ${target.name}）：没有待处理条目（数据集 ${normalized.length} 条，已成功 ${doneIds.size} 条${resume ? "" : "，本次 resume=false 未跳过"}）。\n` +
          `产物：${resultsPath}\n用 local_infer_jobs action=results job_id=${jobId} 查看结果。`,
        data: { job_id: jobId, total: normalized.length, processed: 0, skipped, remaining: 0, results_file: resultsPath, job_file: jobPath, results: [] },
      }
    }

    // ③ 输入副本（全量；与既有副本比对用于提示数据集已变）
    // 先拦重复提交：同一 job_id 已有在跑的批次时，连 items.jsonl 都不该被改写（否则在跑任务会被换掉输入副本）。
    // （跨进程并发不在本层防护范围，靠 owner_pid 与续跑语义治理。）
    if (runningBatches.has(jobDir)) {
      return {
        output:
          `任务 ${jobId} 正在本进程执行中，已拒绝重复提交（同一 job_id 并发写会互相覆盖产物）。\n` +
          `用 local_infer_jobs(action="status", job_id="${jobId}") 看进度，action="cancel" 可中止；或换一个 job_id。`,
        data: { job_id: jobId, rejected: "already_running" },
      }
    }
    let datasetChanged = false
    if (existsSync(itemsPath)) {
      const old = readFileSync(itemsPath, "utf-8").trim()
      datasetChanged = old !== "" && old !== newItemsText.trim()
    }
    writeJsonl(itemsPath, normalized)

    // ④ 并发夹取与档位记录（档位只对本机受管服务有意义：远端目标的并发按服务端 slots 自行控制）
    const reqConcurrency = Math.max(1, num(args.concurrency) ?? 1)
    const { concurrency, note } = target.kind === "local" ? clampConcurrency(home, ctx.env, reqConcurrency) : { concurrency: reqConcurrency }
    const prof = loadProfiles(home)
    const activeProfile = target.kind === "local" ? (ctx.env.LOCAL_INFER_PROFILE ?? prof?.default_profile) : undefined
    // 服务端能力探测（n_ctx 窗口 / slots / 模型名）：失败不阻断，只作提示
    const probe = await probeTarget(target, { timeoutMs: 3000 })
    // 本机目标时模型名以**实际运行实例**为准（探测/状态文件优先，档位缺省值作最后回退）——
    // 否则用默认档位的模型会记错（如跑 cpu-small 却记为 35B 模型），产物与 jobs 输出就不可信了。
    const localInstance = target.kind === "local" ? readServerStates(home).find((s) => s.port === port) : undefined
    const probedModel = probe.model_path ? (probe.model_path.split(/[\\/]/).pop() ?? undefined) : undefined
    const model =
      target.kind === "local"
        ? (probedModel ?? localInstance?.model ?? (prof && activeProfile ? profileModel(prof, activeProfile) : undefined))
        : target.model

    const now = new Date().toISOString()
    const background = args.background === true
    const cancelPath = join(jobDir, "cancel")
    // 新一轮运行前清掉上次的取消标记（否则「取消后 resume 续跑」会被旧标记立即中止）
    rmQuiet(cancelPath)

    const state = {
      job_id: jobId,
      state: slice.length === pending.length ? "running" : "interrupted",
      phase: background ? "running（后台）" : "running",
      port,
      // 推理目标（追溯结果出自哪个端点）：只落目标名与不含密钥的 base_url
      target: target.name,
      target_base_url: target.baseUrl,
      model,
      concurrency,
      total: normalized.length,
      done: 0,
      failed: 0,
      skipped,
      structured: Boolean(args.schema || args.grammar || args.json_object),
      started_at: now,
      updated_at: now,
      results_file: resultsPath,
      items_file: itemsPath,
      pid: process.pid,
      // 后台批次的存活依据：owner_pid = 跑批次的 GEBAI 服务进程；服务重启后 jobs 据此判为「已中断」
      background,
      owner_pid: process.pid,
    } as Record<string, unknown>

    let lastFlush = 0
    const flushJob = (force = false): void => {
      const t = Date.now()
      if (!force && t - lastFlush < 2000) return
      lastFlush = t
      state.updated_at = new Date().toISOString()
      writeFileSync(jobPath, `${JSON.stringify(state, null, 2)}\n`, "utf-8")
    }
    flushJob(true)

    // ⑤ 执行体（同步与后台共用）：按 signal 中止——后台传自身 controller，同步传会话取消信号
    let processedCount = 0
    let failedCount = 0
    const runOnce = async (signal?: AbortSignal): Promise<BatchSummary> =>
      await runBatch(slice, {
        baseUrl: target.baseUrl,
        concurrency,
        retries: num(args.retries) ?? 0,
        timeoutMs: resolveTimeout(args, ctx.env),
        headers: targetHeaders(target),
        defaults: tuningFrom(args),
        structured: structuredFrom(args),
        signal,
        onResult: (r: BatchResult) => {
          // 取消占位项（未开始就被取消）不落盘、不计数：results.jsonl 只留真实产出，续跑时这些条目自然重跑
          if (r.attempts === 0 && r.error === CANCEL_FILLER) return
          appendFileSync(resultsPath, `${JSON.stringify(r)}\n`, "utf-8")
          processedCount++
          if (!r.ok) failedCount++
          state.done = processedCount
          state.failed = failedCount
          flushJob(processedCount % 5 === 0)
        },
      })

    /** 写终态（同步/后台共用；异常路径由调用方单独处理）。 */
    const finalize = (summary: BatchSummary, cancelled: boolean): void => {
      const unprocessed = Math.max(0, slice.length - processedCount)
      state.state = cancelled ? "cancelled" : "done"
      state.phase = cancelled
        ? `已取消（本次剩余 ${unprocessed} 条未处理）`
        : remaining > 0
          ? "分片完成（仍有未处理条目）"
          : "完成"
      state.done = processedCount
      state.failed = failedCount
      state.finished_at = new Date().toISOString()
      state.summary = {
        ok: summary.ok,
        failed: summary.failed,
        tokens_in: summary.tokens_in,
        tokens_out: summary.tokens_out,
        decode_tps: summary.decode_tps,
        avg_latency_ms: summary.avg_latency_ms,
        elapsed_ms: summary.elapsed_ms,
      }
      flushJob(true)
    }

    /** 两侧共用的备注行（同步额外带本次结构化失败数）。 */
    const commonNotes = (structuredFail?: number): string[] => {
      const out: string[] = []
      if (state.structured) {
        const via = args.schema ? "schema" : args.grammar ? "grammar" : "json_object"
        out.push(`结构化输出：已启用（约束 ${via}）${structuredFail != null ? `｜解析/校验失败 ${structuredFail} 条` : ""}`)
      }
      if (datasetChanged) out.push("注意：本次输入数据集与任务目录中留存的 items.jsonl 不同（仍按 id 续跑；如需全新任务请换 job_id）。")
      if (prevState === "cancelled") out.push(`注意：任务 ${jobId} 先前被标记为取消，本次调用继续处理了剩余条目（如已放弃请换 job_id）。`)
      if (note) out.push(`注意：${note}`)
      if (targetChanged) {
        out.push(
          `注意：任务 ${jobId} 记录的目标是 ${prevJob?.target}（${prevJob?.target_base_url ?? "未知端点"}），本次按 ${target.name}（${target.baseUrl}）执行——` +
            "同一任务的结果会混用不同端点的产出，如需分离请换 job_id。",
        )
      }
      const budgetNote = capacityNote(probe, num(args.max_tokens))
      if (budgetNote) out.push(budgetNote)
      if (!probe.ok) out.push(probeWarn(probe, target.kind))
      if (target.kind === "remote" && probe.ok && probe.total_slots && concurrency > probe.total_slots) {
        out.push(`注意：并发 ${concurrency} 超过服务端 slots=${probe.total_slots}（远端目标不受本机档位夹取），多余的请求会排队等待。`)
      }
      if (concurrency > 1) out.push("提示：并发首次运行含 CUDA 图捕获（预热），实测吞吐应以稳态为准（实测 225 → 327 t/s）。")
      if (remaining > 0) out.push(`仍有 ${remaining} 条未处理：可再次调用 batch（同一 job_id=${jobId}）续跑，或调大 max_items。`)
      return out
    }

    // ⑤-a 后台执行：不 await，立即返回；异常也要落终态，不留「running」假象
    if (background) {
      const controller = new AbortController()
      runningBatches.set(jobDir, { controller })
      // 取消标记轮询：jobs cancel 写 <jobDir>/cancel，命中即中止后续条目（在飞条目自然跑完）
      const timer = setInterval(() => {
        if (runningBatches.has(jobDir) && existsSync(cancelPath)) controller.abort()
      }, 400)
      ;(timer as unknown as { unref?: () => void }).unref?.()
      // 分离执行：不 await（工具立即返回）；故意**不传 ctx.signal**——会话取消/工具调用结束不应杀掉后台批次，
      // 中止只经取消标记（jobs cancel）或所属服务进程退出（jobs 会据此判为已中断）。
      // 显式 catch 兜底：异常也要落终态，且不向进程抛未处理拒绝。
      void (async () => {
        try {
          const summary = await runOnce(controller.signal)
          finalize(summary, controller.signal.aborted)
        } catch (e) {
          state.state = "failed"
          state.phase = "异常终止"
          state.error = (e as Error).message
          state.done = processedCount
          state.finished_at = new Date().toISOString()
          try {
            flushJob(true)
          } catch {
            /* 终态写入失败只能放弃（磁盘/权限） */
          }
        } finally {
          clearInterval(timer)
          runningBatches.delete(jobDir)
          rmQuiet(cancelPath)
        }
      })().catch(() => {
        /* 兵底：连终态写入都失败时不再冒泡（状态留在 running，jobs 会按属主存活/心跳提示异常） */
      })

      const bgLines = [
        `批量推理已在后台启动：任务 ${jobId}（目标 ${target.name} ｜ ${target.baseUrl}${activeProfile ? ` ｜ 档位 ${activeProfile}` : ""}）`,
        `本次排队 ${slice.length} 条 ｜ 并发 ${concurrency} ｜ 数据集合计 ${normalized.length} 条${skipped ? ` ｜ 跳过 ${skipped}（已成功项）` : ""}`,
        `产物：${resultsPath}（逐条追加，可实时查看）`,
        "",
        `查进度：local_infer_jobs(action="status", job_id="${jobId}") ｜ 看结果：action="results"`,
        `取消：local_infer_jobs(action="cancel", job_id="${jobId}")——批次在落到下一条边界时停止，已产出结果保留`,
        `说明：后台批次随本 GEBAI 服务进程存活（不随工具调用/会话中断）。服务重启后 jobs 会显示「已中断」，此时用 batch(job_id="${jobId}", resume=true) 续跑剩余条目。`,
        ...commonNotes(),
      ]
      return {
        output: bgLines.join("\n"),
        data: {
          job_id: jobId,
          background: true,
          total: normalized.length,
          queued: slice.length,
          skipped,
          remaining,
          concurrency,
          port,
          target: target.name,
          target_base_url: target.baseUrl,
          results_file: resultsPath,
          job_file: jobPath,
        },
      }
    }

    // ⑤-b 同步执行（既有行为）
    const summary = await runOnce(ctx.signal)
    finalize(summary, false)

    const structuredFail = summary.results.filter((r) => r.json_errors?.length).length
    const slowest = summary.results.reduce((a, b) => Math.max(a, b.elapsed_ms), 0)
    const lines = [
      `批量推理完成：任务 ${jobId}（目标 ${target.name} ｜ ${target.baseUrl}${activeProfile ? ` ｜ 档位 ${activeProfile}` : ""}）`,
      `数据集 ${normalized.length} 条 ｜ 本次处理 ${summary.total} 条 ｜ 成功 ${summary.ok} ｜ 失败 ${summary.failed} ｜ 跳过 ${skipped}（已成功项）`,
      `耗时 ${(summary.elapsed_ms / 1000).toFixed(1)} s ｜ tokens in ${summary.tokens_in} / out ${summary.tokens_out} ｜ 有效吞吐 ${summary.decode_tps.toFixed(1)} t/s ｜ 平均延迟 ${(summary.avg_latency_ms / 1000).toFixed(2)} s ｜ 最慢 ${(slowest / 1000).toFixed(2)} s`,
      `产物：${resultsPath}`,
    ]
    lines.push(...commonNotes(structuredFail))
    const fails = summary.results.filter((r) => !r.ok).slice(0, 5)
    if (fails.length) {
      lines.push("", "失败样例：")
      for (const f of fails) lines.push(`  - ${f.id}: ${String(f.error ?? "未知错误").slice(0, 200)}`)
    }

    return {
      output: lines.join("\n"),
      data: {
        job_id: jobId,
        total: normalized.length,
        processed: summary.total,
        ok: summary.ok,
        failed: summary.failed,
        skipped,
        remaining,
        tokens_in: summary.tokens_in,
        tokens_out: summary.tokens_out,
        decode_tps: summary.decode_tps,
        avg_latency_ms: summary.avg_latency_ms,
        elapsed_ms: summary.elapsed_ms,
        structured_failed: structuredFail,
        concurrency,
        port,
        target: target.name,
        target_base_url: target.baseUrl,
        results_file: resultsPath,
        job_file: jobPath,
        results: summary.results.slice(0, 50).map((r) => ({ id: r.id, ok: r.ok, elapsed_ms: r.elapsed_ms, json: r.json, error: r.error })),
      },
    }
  },
}

// ── jobs：批次进度与结果 ───────────────────────────────────────────────────

const jobs: Tool = {
  name: "jobs",
  safeMode: false, // 安全模式下不提供：cancel 会写任务状态文件
  description:
    "查询与管理批量推理任务：list（列出任务与规模、目标端点、后台/同步与存活标记）、status（实时进度——从 results.jsonl 逐条统计，" +
    "并判定属主进程存活：服务重启后会把死批次改判「已中断」并给出续跑指引）、results（逐条结果，可按 only=ok/failed 过滤）、" +
    "cancel（取消：本进程在跑的后台批次会真中止——写取消标记，当前在飞条目跑完后停止；同步执行/已完成的任务仅改状态标记）。" +
    "产物都在 infer/bench/runs/<job_id>/，可直接 tail results.jsonl 看逐条产出。",
  parameters: schema({
    action: { type: "string", enum: ["list", "status", "results", "cancel"], description: "操作：list 列出任务 / status 进度与存活 / results 逐条结果 / cancel 取消（在跑的后台批次会真中止；缺省 list）" },
    job_id: { type: "string", description: "任务标识（status/results/cancel 必填）" },
    limit: { type: "number", description: "list 的任务条数或 results 的结果条数（缺省 20）" },
    only: { type: "string", enum: ["ok", "failed"], description: "results 只取成功/失败条目" },
  }),
  async execute(args, ctx) {
    const home = inferHome(ctx.env)
    const action = typeof args.action === "string" ? args.action : "list"
    const limit = Math.max(1, num(args.limit) ?? 20)
    const jobId = typeof args.job_id === "string" && args.job_id.trim() ? args.job_id.trim() : undefined

    if (action === "list") {
      const list = listBatchJobs(home, limit)
      if (!list.length) {
        return { output: `暂无批量任务（目录：${runsDir(home)}）。用 local_infer_batch 提交（items 或 items_file）。`, data: { jobs: [] } }
      }
      const lines = [`批量任务（${list.length} 个，新→旧）：`, ""]
      for (const j of list) {
        const resultsFile = j.results_file ?? join(j.dir, "results.jsonl")
        const done = countLines(resultsFile)
        const elapsed = j.finished_at ? `已完成` : `进行中`
        const bgMark = (j as BatchJobView).background === true ? "后台" : "同步"
        lines.push(
          `${j.job_id}  [${j.state}]  ${done}/${j.total} 条${j.failed ? ` ｜ 失败 ${j.failed}` : ""}` +
            `${j.skipped ? ` ｜ 跳过 ${j.skipped}` : ""}${j.structured ? " ｜ 结构化" : ""} ｜ 并发 ${j.concurrency} ｜ 端口 ${j.port}` +
            ` ｜ 目标 ${(j as BatchJobView).target ?? "local"} ｜ ${elapsed} ｜ ${bgMark} ｜ 起于 ${j.started_at}`,
        )
        // 存活标记：只对「状态仍为 running」的任务探活（已完成的任务无需花销）
        if (j.state === "running") {
          const pid = (j as BatchJobView).owner_pid
          let mark: string
          if (runningBatches.has(j.dir)) mark = "在跑（本进程）"
          else if (typeof pid === "number") mark = (await ownerAlive(pid, ctx)) ? `在跑（PID ${pid}）` : "已中断（执行进程已退出，可 resume 续跑）"
          else mark = "无属主记录（旧版本写入，无法确认）"
          lines.push(`    └ 存活：${mark}`)
        }
      }
      lines.push("", `用 local_infer_jobs action=status job_id=<id> 看进度；action=results 看逐条结果。`)
      return { output: lines.join("\n"), data: { jobs: list.map((j) => ({ ...j })) } }
    }

    if (!jobId) return { output: `action=${action} 需要 job_id（先用 action=list 查看可用任务）。` }
    const job = readBatchJob(home, jobId)
    if (!job) return { output: `未找到任务 ${jobId}（目录：${join(runsDir(home), jobId)}）。用 local_infer_jobs action=list 查看现有任务。` }
    const resultsFile = job.results_file ?? join(job.dir, "results.jsonl")

    if (action === "cancel") {
      const jp = join(job.dir, "job.json")
      const pid = (job as BatchJobView).owner_pid
      const marker = join(job.dir, "cancel")
      const running = runningBatches.get(job.dir)

      // ① 本进程正在跑（后台批次）：写取消标记 → 执行体在下一轮轮询中止，等在飞条目收尾
      if (running) {
        writeFileSync(marker, `${new Date().toISOString()} 由 local_infer_jobs cancel 写入\n`, "utf-8")
        const stopped = await waitFor(() => !runningBatches.has(job.dir), 30000, 100)
        const after = readBatchJob(home, jobId) as BatchJobView | null
        const produced = countLines(after?.results_file ?? resultsFile)
        const remaining = Math.max(0, (after?.total ?? job.total) - produced - (after?.skipped ?? 0))
        return {
          output:
            `任务 ${jobId} 已请求取消（写取消标记 ${marker}）：${stopped ? "批次已停止" : "批次仍在收尾（当前在飞条目跑完后停止）"}。\n` +
            `已产出 ${produced} 条 ｜ 状态 ${after?.state ?? "cancelling"}${after?.phase ? `（${after.phase}）` : ""} ｜ 剩余约 ${remaining} 条未产出。\n` +
            `已产出的结果保留在 ${after?.results_file ?? resultsFile}；剩余条目可用 local_infer_batch(job_id="${jobId}", resume=true) 续跑（未成功条目不算已完成）。`,
          data: { job_id: jobId, state: after?.state ?? "cancelling", cancelled: true, stopped, produced, remaining },
        }
      }

      // ② 不在本进程：属主已退出（服务重启）→ 直接置 cancelled；届时是已完成/无属主记录的任务，保持「仅标记」语义
      const ownerGone = job.state === "running" && typeof pid === "number" && !(await ownerAlive(pid, ctx))
      writeFileSync(jp, `${JSON.stringify({ ...job, state: "cancelled", updated_at: new Date().toISOString() }, null, 2)}\n`, "utf-8")
      return {
        output:
          `任务 ${jobId} 已标记为 cancelled（写回 ${jp}）。\n` +
          (ownerGone
            ? `该任务未在本进程运行（属主 PID ${pid} 已退出）——已直接置为 cancelled；已产出结果保留，剩余条目用 batch(job_id="${jobId}", resume=true) 可续跑。\n`
            : "说明：本任务未在本进程运行（同步执行已结束，或由其他进程/服务重启前提交）：取消仅为状态标记，不影响已提交的在飞调用（同 job_id 再次调用仍会处理剩余条目——若要彻底放弃请换 job_id）。\n") +
          "正在本进程跑的后台批次（background=true）才会被 cancel 真中止；同步执行中的批次请用会话中断（工具会随会话取消中止）。",
        data: { job_id: jobId, state: "cancelled" },
      }
    }

    const results = readResults(resultsFile)

    if (action === "status") {
      const view = job as BatchJobView
      const counts = results.reduce(
        (a, r) => {
          a.done++
          if (r.ok) a.ok++
          else a.failed++
          if (r.json_errors?.length) a.structuredFailed++
          a.tps += r.decode_tps ?? 0
          return a
        },
        { done: 0, ok: 0, failed: 0, structuredFailed: 0, tps: 0 },
      )

      // 存活判定：状态仍为 running 时确认属主是否还在跑（服务重启后不能把死批次当在跑）
      let liveness = ""
      if (job.state === "running") {
        const pid = view.owner_pid
        if (runningBatches.has(job.dir)) {
          liveness = heartbeatStale(job.updated_at)
            ? "在跑（本进程）——但心跳已超过 10 分钟未更新，可能已卡住（查看产物/日志，或 action=cancel 中止）"
            : "在跑（本进程）"
        } else if (typeof pid !== "number") {
          liveness = "状态为进行中，但记录无属主进程（旧版本写入的 job.json）——无法确认是否仍在跑；可用 action=cancel 中止后 resume 续跑。"
        } else if (await ownerAlive(pid, ctx)) {
          liveness = heartbeatStale(job.updated_at)
            ? `在跑（PID ${pid}）——但心跳已超过 10 分钟未更新，可能已卡住（查看产物/日志，或 action=cancel 中止）`
            : `在跑（PID ${pid}）`
        } else {
          // 属主进程已退出（服务重启/崩溃）：改判中断（幂等：仅在 running 时改写），给出续跑指引
          patchJobFile(job.dir, { state: "interrupted", phase: "已中断（执行进程退出）", error: `执行进程 PID ${pid} 已退出，任务中断` })
          job.state = "interrupted"
          job.phase = "已中断（执行进程退出）"
          liveness = `已中断（执行进程 PID ${pid} 已退出）——已产出结果保留，剩余条目用 local_infer_batch(job_id="${jobId}", resume=true) 续跑。`
        }
      }

      const last = results[results.length - 1]
      const lines = [
        `任务 ${jobId}`,
        `状态 ${job.state}${job.phase ? `（${job.phase}）` : ""} ｜ 目标 ${view.target ?? "local"}${view.target_base_url ? `（${view.target_base_url}）` : ""} ｜ 端口 ${job.port} ｜ 并发 ${job.concurrency}${job.model ? ` ｜ 模型 ${job.model}` : ""}`,
        `数据集 ${job.total} 条 ｜ 已产出 ${counts.done} 条（成功 ${counts.ok} / 失败 ${counts.failed}）${job.skipped ? ` ｜ 跳过 ${job.skipped}` : ""}`,
        counts.done ? `平均解码 ${(counts.tps / counts.done).toFixed(1)} t/s ｜ 最近一条 ${last ? `${last.ok ? "成功" : "失败"} ${(last.elapsed_ms / 1000).toFixed(2)} s` : "-"}` : "尚无产出",
        `起于 ${job.started_at}${job.finished_at ? ` ｜ 结束于 ${job.finished_at}` : ""} ｜ ${view.background === true ? "后台执行" : "同步执行"}`,
        `结果文件：${resultsFile}`,
      ]
      if (liveness) lines.push(`存活：${liveness}`)
      if (view.error) lines.push(`错误：${String(view.error).slice(0, 400)}`)
      if (counts.structuredFailed) lines.push(`结构化解析/校验失败 ${counts.structuredFailed} 条（见 action=results only=failed）`)
      if (counts.done < job.total && job.state !== "cancelled" && job.state !== "interrupted") {
        lines.push(`提示：进度以 results.jsonl 实时统计；剩余 ${Math.max(0, job.total - counts.done - (job.skipped ?? 0))} 条未产出（同一 job_id 再次 batch 即续跑）。`)
      }
      return { output: lines.join("\n"), data: { job: { ...job }, counts, liveness, results_file: resultsFile } }
    }

    // action === results
    const only = args.only === "ok" || args.only === "failed" ? args.only : undefined
    const filtered = results.filter((r) => (only === "ok" ? r.ok : only === "failed" ? !r.ok : true))
    const shown = filtered.slice(-limit)
    if (!shown.length) {
      return {
        output: `任务 ${jobId}（目标 ${(job as BatchJobView).target ?? "local"}）暂无${only ? ` ${only} ` : ""}结果（结果文件：${resultsFile}；已产出 ${results.length} 条）。`,
        data: { job_id: jobId, total_results: results.length, results: [] },
      }
    }
    const lines = [
      `任务 ${jobId}（目标 ${(job as BatchJobView).target ?? "local"}）：${filtered.length} 条${only ? ` ${only} ` : ""}结果，显示最近 ${shown.length} 条`,
      "",
    ]
    for (const r of shown) {
      lines.push(`── ${r.id} ｜ ${r.ok ? "成功" : "失败"} ｜ ${(r.elapsed_ms / 1000).toFixed(2)} s ｜ attempts ${r.attempts}${r.decode_tps ? ` ｜ ${r.decode_tps.toFixed(1)} t/s` : ""}`)
      if (r.json !== undefined) lines.push(JSON.stringify(r.json, null, 2).split("\n").slice(0, 40).join("\n"))
      else if (r.content) lines.push(String(r.content).slice(0, 1200))
      if (r.json_errors?.length) lines.push(`结构化校验未通过：${r.json_errors.slice(0, 5).join("; ")}`)
      if (r.error) lines.push(`错误：${String(r.error).slice(0, 400)}`)
      lines.push("")
    }
    return {
      output: lines.join("\n"),
      data: {
        job_id: jobId,
        total_results: results.length,
        returned: shown.length,
        results: shown.map((r) => ({ id: r.id, ok: r.ok, elapsed_ms: r.elapsed_ms, json: r.json, json_errors: r.json_errors, error: r.error, meta: r.meta })),
      },
    }
  },
}

/** 结果文件行数（列表视图的实时进度；不解析内容，坏行也计入——它代表已落盘条目）。 */
function countLines(file: string): number {
  if (!existsSync(file)) return 0
  try {
    if (statSync(file).size === 0) return 0
    const text = readFileSync(file, "utf-8")
    return text.split(/\r?\n/).filter((l) => l.trim()).length
  } catch {
    return 0
  }
}

export const tools: Record<string, Tool> = { generate, batch, jobs }
/** 三者都不改变服务状态（只发请求 / 落盘产物），免审批；描述中已提示会占用 GPU。 */
export const requiresApproval: Record<string, boolean> = {}
