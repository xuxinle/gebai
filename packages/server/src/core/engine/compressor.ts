/** 上下文压缩器（自 engine.ts 拆分）：compactSession（主动/自动压缩）+ 溢出硬护栏（受保护消息降级）
 *  + 摘要生成（历史/分支报告）+ 上下文腾挪与溢出恢复重试。engine 经构造注入 deps 委托调用，
 *  AgentEngine 公共 API 不变。 */
import type { Message, MessageLike } from "@gebai/sdk"
import type { LLMChunk, LLMProvider, LLMUsage } from "../llm/llm"
import type { SessionStore } from "../session/store"
import { isEngineNote, isCompressibleMessage, estimateCharsTokens } from "../session/store"
import type { EnvManager } from "../session/env"
import { VISION_MIME_SET } from "@gebai/agents"

export const COMPACT_OUTPUT_RESERVE_FALLBACK = 16384
/** 输出预留的最小值（窗口很小时不能让预留缩到无意义）。 */
const COMPACT_OUTPUT_RESERVE_MIN = 1024
/** 压缩目标剩余倍数：压缩后窗口剩余 = 输出预留 × 该倍数（>1 让后续若干轮不必立即再压，
 *  同时避免一次压太多——尽可能多保留信息）。 */
const COMPACT_TARGET_RESERVE_MULTIPLE = 2
/** 压缩下限水位：压缩不得把输入压到窗口的该比例以下（压太狠丢信息）。 */
const COMPACT_FLOOR_RATIO = 0.4
/** 已低于目标水位时的最小腾挪量（溢出恢复场景：接口已报溢出，至少腾出该比例的窗口空间）。 */
const COMPACT_MIN_ROOM_RATIO = 0.05
/** 近消息滑动窗口（条，任意角色）：最近这么多条消息永不进压缩区间（原样保留）。
 *  上限为历史一半——否则短会话永远压不动（保留下限反而让压缩失效）。环境变量 GEBAI_COMPACT_WINDOW 可调。 */
const COMPACT_WINDOW_MESSAGES = 12
/** 图片/附件在内联窗口内的 token 粗估（按张；真实值由接口计，此处仅用于压缩量规划）。 */
const IMAGE_TOKEN_ESTIMATE = 1000

/**
 * 一次回复的输出预留（压缩触发与目标的唯一基准）：模型单次响应输出上限（接口能力声明的
 * maxOutputTokens），未声明时用缺省预留，并夹在 [1k, 窗口一半] 内——预留不能超过窗口一半，
 * 否则小窗口模型会永远处于「剩余不足」状态而反复压缩。
 * 压缩触发 = 窗口剩余 < 本预留（剩余不足以支撑一次回复）。
 */
export function outputReserveTokens(cap: number, maxOutputTokens?: number): number {
  if (cap <= 0) return 0
  const raw = maxOutputTokens && maxOutputTokens > 0 ? maxOutputTokens : COMPACT_OUTPUT_RESERVE_FALLBACK
  return Math.max(COMPACT_OUTPUT_RESERVE_MIN, Math.min(raw, Math.floor(cap / 2)))
}
/** 摘要请求的单块输入预算与块数上限（总覆盖 = 两者之积 ≈ 12 万字符历史可完整进摘要）。 */
const SUMMARY_INPUT_LIMIT = 20000
const SUMMARY_MAX_CHUNKS = 6
/** 摘要输出上限（合并后摘要的硬上限）。 */
const SUMMARY_OUTPUT_LIMIT = 2000
/** 摘要输入中单条消息骨架的内容上限（工具参数只取开头，避免单条超长消息挤占块预算）。 */
const SUMMARY_ITEM_LIMIT = 600
/** 摘要失败时降级骨架的行数与单行字符上限（降级文本同样受 SUMMARY_OUTPUT_LIMIT 约束）。 */
const SUMMARY_FALLBACK_LINES = 15
const SUMMARY_FALLBACK_ITEM_LIMIT = 120

/** 摘要提示词（单块与分块共用；分块时由 chunkPrompt 补「第 i/n 段」上下文）。
 *  注：压缩后用户输入原文不再留在上下文中（只留摘要），因此摘要必须承载用户的要求与约束。 */
const SUMMARIZE_PROMPT = [
  "你是对话历史压缩器。把给定的对话历史（含用户输入、工具调用与结果）压缩为一段中文摘要，供同一会话后续继续使用：",
  "保留——用户提出的原始要求与约束（关键句可原样引用）、已确认的目标与偏好、已达成的结论与关键决定、涉及的文件路径与命令、用过的工具与产物、当前进度与未完成事项、需要延续的约定（命名/风格/参数）；",
  "舍弃——客套与重复尝试过程、已被推翻的中间结论。不要编造未出现的信息。",
  "直接输出摘要正文（可分条），不超过 800 字。",
].join("\n")
/** 分块摘要的合并提示词（map-reduce 的 reduce 步）。 */
const MERGE_SUMMARY_PROMPT = [
  "把以下按时间顺序排列的分段摘要合并为一段完整摘要（同一会话的历史，供后续继续使用）：",
  "去重、按时序组织，保留——任务目标与约束、结论与关键决定、文件路径与产物、当前进度与未完成事项；",
  "舍弃重复叙述。不要编造未出现的信息。直接输出摘要正文（可分条），不超过 800 字。",
].join("\n")

/** 摘要请求的预留空间（token）：摘要输出上限 + 工具 schema 段开销——摘要请求只需装下前缀 + 预留空间；
 *  不用「一次回复的输出预留」（那是主循环的尺子）：摘要输出短得多，用大预留会把本该命中的前缀误判为超预算。 */
const SUMMARY_OUTPUT_RESERVE_TOKENS = 8192

/** 分块摘要提示（标注段序，便于模型保留时序）。 */
function chunkPrompt(index: number, total: number): string {
  return `${SUMMARIZE_PROMPT}\n这是长对话历史的第 ${index}/${total} 段（按时间顺序），请压缩为要点，不超过 300 字。`
}

/**
 * 缓存友好摘要请求的尾部指令（追加在主循环同前缀之后）：用与主循环**逐字节相同的前缀**（同一 system
 * 提示词、同一段历史原文、同一批工具 schema）发起摘要调用，服务端前缀缓存可命中——已处理过的历史 token
 * 按缓存价计（多数服务商命中部分仅 10%~50% 价），且不需要把消息重写成骨架带来的额外开销；
 * 同时保留原文而非截断到 600 字符，摘要信息量更高。
 */
const CACHE_PREFIX_INSTRUCTION = [
  "【压缩指令】请把以上对话历史（含用户输入、工具调用与结果）压缩为一段中文摘要，供本会话后续继续使用：",
  "保留——用户提出的原始要求与约束（关键句可原样引用）、已确认的目标与偏好、已达成的结论与关键决定、涉及的文件路径与命令、用过的工具与产物、当前进度与未完成事项、需要延续的约定（命名/风格/参数）；",
  "舍弃——客套与重复尝试过程、已被推翻的中间结论。不要编造未出现的信息。",
  "不要调用任何工具，直接输出摘要正文（可分条），不超过 800 字。",
].join("\n")

/** MessageLike 的 token 粗估（含内容块：图片 base64 等按字符折算，宁可高估——超预算就退回骨架路径）。 */
function estimateMessageLikeTokens(m: MessageLike): number {
  const content = m.content
  const text = typeof content === "string" ? content : JSON.stringify(content ?? "")
  let t = estimateCharsTokens(text)
  for (const tc of (m as { toolCalls?: Array<{ name: string; arguments?: unknown }> }).toolCalls ?? []) {
    t += estimateCharsTokens(`${tc.name} ${JSON.stringify(tc.arguments ?? {})}`)
  }
  return t
}

/** 文本截断（保留「共多少字符」提示，模型据此知道有省略）。 */
function clipText(s: string, n: number): string {
  if (s.length <= n) return s
  return `${s.slice(0, n)}…（余 ${s.length - n} 字符）`
}

/**
 * 摘要输入的单条消息骨架行：assistant 工具调用轮的 content 常为空（正文在推理与工具参数里），
 * 只取 content 会让「调用过哪些工具、带了什么参数」在摘要里彻底消失——补上工具调用骨架（工具名 + 参数摘要）
 * 与工具结果的脉络（结果全文已按截断规则落盘，摘要只需知道「哪个工具做过什么」）。
 */
export function summarizeMessageLine(m: Message, limit = SUMMARY_ITEM_LIMIT): string {
  const body = typeof m.content === "string" ? m.content.trim() : ""
  if (m.role === "tool") return `[工具结果:${m.name ?? "tool"}] ${clipText(body, limit)}`
  if (m.role === "assistant" && m.toolCalls?.length) {
    const calls = m.toolCalls.map((tc) => `${tc.name}(${clipText(JSON.stringify(tc.arguments ?? {}), 160)})`).join("；")
    return `[assistant]${body ? ` ${clipText(body, limit)}` : ""} [调用工具] ${calls}`
  }
  return `[${m.role}] ${clipText(body, limit)}`
}

/** 单条消息的 token 估算（与 estimateCharsTokens 同口径）：正文 + 工具调用签名（名/参数）+ 图片与附件按张粗估。
 *  仅用于压缩量规划（真实占用以接口 usage 为准，见「上下文占用口径」）。 */
export function estimateMessageTokens(m: Message): number {
  const body = typeof m.content === "string" ? m.content : JSON.stringify(m.content ?? "")
  let t = estimateCharsTokens(body)
  for (const tc of m.toolCalls ?? []) t += estimateCharsTokens(`${tc.name} ${JSON.stringify(tc.arguments ?? {})}`)
  t += ((m.images?.length ?? 0) + (m.attachments?.length ?? 0)) * IMAGE_TOKEN_ESTIMATE
  return t
}

/** 压缩区间端点对齐 assistant(toolCalls)/tool 配对边界：区间切在配对中间时，落单的 tool 结果会被
 *  配对修复当孤儿丢弃（内容既没进摘要也没留在上下文）——末尾是发起消息时把其后的 tool 结果一并纳入
 *  （结果因此进摘要输入）；区间以 tool 结果开头时前推到它的发起消息。 */
export function alignPairingBounds(messages: Message[], from: number, to: number): { from: number; to: number } {
  let end = to
  while (end < messages.length && messages[end]?.role === "tool" && messages[end - 1]?.role === "assistant" && messages[end - 1].toolCalls?.length) end++
  let start = from
  while (start > 0 && messages[start]?.role === "tool") start--
  return { from: start, to: end }
}

/**
 * 压缩区间规划（水位区间 + 滑动窗口）：压缩四条基本原则的落点——
 * ① **近消息滑动窗口**：最近 `window`（默认 COMPACT_WINDOW_MESSAGES，上限为历史一半）条消息（任意角色）
 *    永不进压缩区间，随新消息自然向前滑动；
 * ② **远消息压缩后完全抛弃**：区间取最早的连续一段（远的先压），区间内可压缩消息由摘要替换并从上下文移除；
 * ③ **水位之间压缩**：上水位 = 窗口 - 输出预留（`reserve`，剩余不够一次回复即触发），下水位 = 窗口×40%
 *    与「窗口 - 预留×2」取高（压缩后仍能支撑若干次回复，且不压太狠）——需腾出量 = 基线 - 目标输入，
 *    按 estimateMessageTokens 逐条累计（压多少算多少，不一次压太多）；无真实占用基线时压到窗口边界；
 * ④ **系统提示词不压缩**：区间只取可压缩消息（系统提示词消息由 compactMessages 原位保留，见 store）。
 * 无真实占用基线（算不出需腾出量）时退保守口径：压掉窗口外可压缩消息的一半（不一次丢大量历史，靠不足时的迭代压缩与溢出重试逐步收敛）。
 * 返回压缩区间（已对齐配对边界）或 null（无可压缩区间）。
 */
export function planCompactRange(
  messages: Message[],
  compactable: number[],
  ctx: { baseline?: number; cap: number; reserve: number; window?: number },
): { from: number; to: number } | null {
  if (compactable.length < 2) return null
  const { baseline, cap, reserve } = ctx
  // ① 滑动窗口：最近 window 条消息（任意角色）原样保留；上限为历史一半（否则短会话永远压不动）
  const window = Math.max(1, Math.min(ctx.window ?? COMPACT_WINDOW_MESSAGES, Math.floor(messages.length / 2)))
  const maxTo = Math.max(0, messages.length - window)
  const inRange = compactable.filter((i) => i < maxTo)
  if (inRange.length < 2) return null
  let k: number
  if (cap > 0 && baseline !== undefined && baseline > 0) {
    // ③ 水位区间：目标输入 = max(下限水位, 窗口 - 输出预留 × 倍数)
    const floor = cap * COMPACT_FLOOR_RATIO
    const targetInput = Math.max(floor, cap - reserve * COMPACT_TARGET_RESERVE_MULTIPLE)
    let need = baseline - targetInput
    // 下限水位保护：不把输入压到 floor 以下
    if (need > 0) need = Math.min(need, Math.max(0, baseline - floor))
    if (need <= 0) need = cap * COMPACT_MIN_ROOM_RATIO // 已低于目标（如接口已报溢出）：至少腾出一点
    let acc = 0
    k = 0
    for (; k < inRange.length; k++) {
      acc += estimateMessageTokens(messages[inRange[k]])
      if (acc >= need) {
        k++
        break
      }
    }
  } else {
    // 无真实占用基线（老会话/接口不返回 usage/窗口未知）：**保守口径**——只压掉窗口外可压缩消息的一半。
    // 拿不到真实占用就算不出需腾出量，压满窗口外（=只留最近窗口）会一次丢掉大量历史；
    // 靠不足时的迭代压缩（run 前至多 4 轮）与溢出重试逐步收敛更稳（符合「每次不压太多」）
    const half = Math.max(1, Math.floor(inRange.length / 2))
    k = inRange.length - half
  }
  if (k < 2) return null
  const aligned = alignPairingBounds(messages, inRange[0], inRange[k - 1] + 1)
  if (aligned.from >= aligned.to) return null
  return aligned
}

/** 按顺序打包骨架行：每块 ≤ budget；单行超预算时独占一块并按预算截断（不让一条超长消息吃掉全部预算）。 */
function packLines(lines: string[], budget: number, maxChunks: number): string[] {
  const chunks: string[] = []
  let cur = ""
  for (const line of lines) {
    if (chunks.length >= maxChunks) break
    const piece = line.length > budget ? clipText(line, budget) : line
    if (cur && cur.length + piece.length + 1 > budget) {
      chunks.push(cur)
      cur = ""
      if (chunks.length >= maxChunks) break
    }
    cur = cur ? `${cur}\n${piece}` : piece
  }
  if (cur && chunks.length < maxChunks) chunks.push(cur)
  return chunks
}

/**
 * 骨架行 → 摘要分块。总量在预算内（budget × maxChunks）时顺序分块；超出时**头尾保留**
 * （与工具输出截断同语义）：头部 60% + 尾部 40%，中部以省略说明行代表——保证「最早的任务背景」与
 * 「最新进度」都进摘要输入，而不是只摘要最早一段、其余静默丢弃（按 20000 字符硬切时，一次压缩覆盖的
 * 15 万字符历史只有约 1/8 进入摘要，且无任何省略提示）。
 */
export function buildSummaryChunks(lines: string[], budget = SUMMARY_INPUT_LIMIT, maxChunks = SUMMARY_MAX_CHUNKS): string[] {
  const total = lines.reduce((a, l) => a + l.length + 1, 0)
  const cap = budget * maxChunks
  if (total <= cap) return packLines(lines, budget, maxChunks)
  const headBudget = Math.floor(cap * 0.6)
  const tailBudget = cap - headBudget
  const headLines: string[] = []
  let headChars = 0
  let i = 0
  for (; i < lines.length; i++) {
    if (headChars + lines[i].length + 1 > headBudget && headLines.length) break
    headLines.push(lines[i])
    headChars += lines[i].length + 1
  }
  const tailLines: string[] = []
  let tailChars = 0
  for (let j = lines.length - 1; j >= i; j--) {
    if (tailChars + lines[j].length + 1 > tailBudget && tailLines.length) break
    tailLines.unshift(lines[j])
    tailChars += lines[j].length + 1
  }
  const omitted = total - headChars - tailChars
  const note = `[省略] 此处约 ${omitted} 字符的历史内容因超出摘要输入预算未纳入（消息过密）：摘要仅覆盖最早与最近两端，其余细节已随压缩移除。`
  return [...packLines(headLines, budget, maxChunks), note, ...packLines(tailLines, budget, maxChunks)]
}

/**
 * 摘要失败（接口异常/读超时/取消）时的降级文本：保留**被裁剪内容的骨架行**而非一句空占位——
 * 模型仍能知道被裁剪的历史大致是什么、涉及哪些文件与工具，
 * 比「历史已丢弃」式占位信息量大得多。
 */
export function summarizeFallback(slice: Message[]): string {
  const lines = slice.slice(0, SUMMARY_FALLBACK_LINES).map((m) => summarizeMessageLine(m, SUMMARY_FALLBACK_ITEM_LIMIT))
  const head = `[上下文已裁剪：摘要生成失败，已丢弃最早 ${slice.length} 条历史消息（原文不再保留）。以下为被裁剪内容的骨架记录]`
  const more = slice.length > SUMMARY_FALLBACK_LINES ? `\n…（另有 ${slice.length - SUMMARY_FALLBACK_LINES} 条更晚的消息同样已被裁剪）` : ""
  return `${head}\n${lines.join("\n")}${more}`.slice(0, SUMMARY_OUTPUT_LIMIT)
}

/** 模型单轮调用的 schema 形态（engine.callModel 同构）。 */
export type CallModelSchemas = Array<{ name: string; description: string; parameters: Record<string, unknown> }>

/** callModel 返回形态（文本/工具调用/usage/停止原因）。 */
export interface CallModelResult {
  text: string
  toolCalls: Array<{ id: string; name: string; arguments: Record<string, unknown>; argsError?: string; raw?: string }>
  usage?: LLMUsage
  stopReason?: string
}

export interface CompressorDeps {
  store: SessionStore
  env: EnvManager
  /** 启动缺省 Provider（活取——测试可运行期改写 engine.opts.provider）。 */
  getDefaultProvider: () => LLMProvider
  /** 任务级 Provider 解析（env 覆盖时重建；无覆盖返回 undefined；活取同上）。 */
  resolveProvider?: (env: Record<string, string>) => LLMProvider | undefined
  /** LLM 流式读空闲超时（毫秒，opts.llmIdleTimeoutMs ?? 引擎缺省值——由 engine 解析后注入）。 */
  idleTimeoutMs: number
  isTaskRunning: (sessionId: string) => boolean
  /** 任务取消信号（运行中任务的 controller.signal）。 */
  taskSignal: (sessionId: string) => AbortSignal | undefined
  publish: (sessionId: string, type: string, payload: Record<string, unknown>) => void
  loadHistory: (sessionId: string, user: string, inlineMultimodal?: boolean, upToIndex?: number) => Promise<MessageLike[]>
  callModel: (provider: LLMProvider, messages: MessageLike[], schemas: CallModelSchemas, signal: AbortSignal, onChunk?: (chunk: LLMChunk) => void, extraParams?: Record<string, unknown>, sessionId?: string) => Promise<CallModelResult>
  /** 配置开关读取（环境变量）：GEBAI_COMPACT_CACHE_PREFIX（前缀缓存策略：0/off 强制骨架行路径、
   *  1/on 强制原文前缀、缺省自适应——按上次是否命中缓存决定）与 GEBAI_COMPACT_WINDOW（滑动窗口条数）。
   *  测试可注入；不注入时读 process.env。 */
  readEnv?: (name: string) => string | undefined
}

/** 缓存友好摘要请求的前缀（engine 构造：与主循环请求同前缀 + 同批工具）。 */
export interface SummarizeCachePrefix {
  /** 与主循环完全一致的 system 提示词串。 */
  systemPrompt: string
  /** 主循环同批工具 schema（多数服务商的前缀缓存包含 tools 段，缺了会整体失配）。 */
  tools?: CallModelSchemas
}

export class ContextCompressor {
  /** 前缀缓存命中记忆（provider+model → 上次摘要调用是否命中）：未命中时改写骨架行路径（体积小得多）。 */
  private cacheHit = new Map<string, boolean>()

  constructor(private deps: CompressorDeps) {}

  async compactSession(
    sessionId: string,
    user: string,
    scope?: "all" | { from: number; to: number },
    provider?: LLMProvider,
    opts: { internal?: boolean; cachePrefix?: SummarizeCachePrefix } = {},
  ): Promise<{ compacted: number; summary: string }> {
    // 手动压缩（UI/REST 入口）在任务运行中被拒：summarize 是秒级 LLM 调用，期间任务持续追加消息，
    // 陈旧压缩区间会套删未参与摘要的新落盘内容（自动压缩经 internal 标记在任务流程内自身协调，不受此限）
    if (!opts.internal && this.deps.isTaskRunning(sessionId)) throw new Error("会话有任务正在运行，暂不能手动压缩；请等待任务完成或先停止任务")
    const session = await this.deps.store.load(sessionId, user)
    if (!session) throw new Error(`会话不存在: ${sessionId}`)
    // Provider 未显式指定（UI/REST 主动压缩入口）时按合并后 env 解析：用户/会话级模型配置同样生效
    const llm = provider ?? this.deps.resolveProvider?.(await this.deps.env.resolve(sessionId, user)) ?? this.deps.getDefaultProvider()
    const messages = session.messages
    // 可压缩消息（isCompressibleMessage）：受保护消息（系统提示词含装载提示词/压缩摘要、用户输入、
    // 新会话执行存档）不压缩不改变（不选进区间、不进摘要输入，区间夹带时由 compactMessages 原位保留）；
    // 引擎注入的消息（软性提醒/裁剪提示，同为 user 角色）是引擎可再生的派生内容，可被摘要吸收
    const compactable: number[] = []
    for (let i = 0; i < messages.length; i++) {
      if (isCompressibleMessage(messages[i])) compactable.push(i)
    }
    if (compactable.length < 2) return { compacted: 0, summary: "" }

    let from: number
    let to: number
    if (scope && typeof scope === "object") {
      from = Math.max(0, Math.min(scope.from, messages.length))
      to = Math.min(messages.length, scope.to)
      ;({ from, to } = alignPairingBounds(messages, from, to))
    } else if (scope === "all") {
      const aligned = alignPairingBounds(messages, compactable[0], compactable[compactable.length - 1] + 1)
      from = aligned.from
      to = aligned.to
    } else {
      // 默认：水位区间 + 滑动窗口（近消息原样保留、远消息压缩后完全抛弃、每次只压到目标水位）
      // cap 从 provider 能力声明读取；测试桩/不完整的 Provider 未声明能力时退回兜底口径（保留最近一半）
      const caps = typeof llm?.capabilities === "function" ? llm.capabilities() : undefined
      const cap = caps?.maxContextTokens ?? 0
      const reserve = outputReserveTokens(cap, caps?.maxOutputTokens)
      const plan = planCompactRange(messages, compactable, { baseline: session.ctxInputTokens, cap, reserve, window: this.windowMessages() })
      if (!plan) return { compacted: 0, summary: "" }
      from = plan.from
      to = plan.to
    }
    if (from >= to) return { compacted: 0, summary: "" }

    const slice = messages.slice(from, to)
    const removable = slice.filter((m) => isCompressibleMessage(m))
    const removed = removable.length
    if (removed === 0) return { compacted: 0, summary: "" } // 区间内仅剩受保护消息：无可压缩内容
    // 滚动摘要合并：既有摘要（compacted）内容并入新摘要，由 compactMessages 一并替换——
    // 长会话反复压缩时摘要恒为一条，不再逐条累积（各条摘要相互重叠，纯粋白占窗口）
    const priorSummaries = messages
      .filter((m) => m.compacted && typeof m.content === "string" && m.content.trim())
      .map((m) => m.content as string)
    // 缓存友好的摘要请求：直接把**与主循环逐字节同前缀**的历史原文发给模型（同一 system 提示词、
    // 同一段历史、同一批工具 schema）——服务端前缀缓存可命中，已处理过的 token 按缓存价计；
    // 仅在既有摘要消息全部位于区间之前时启用（否则旧摘要内容不在前缀里，吸收后会丢信息）
    const priorSummaryBefore = messages.every((m, i) => !m.compacted || i < from)
    const cached = await this.cachedPrefix(llm, sessionId, user, to, opts.cachePrefix, priorSummaryBefore)
    const summary = await this.summarize(removable, llm, this.deps.taskSignal(sessionId), priorSummaries, cached)
    await this.deps.store.compactMessages(sessionId, user, { from, to, summary })
    this.deps.publish(sessionId, "event.message.compact", { from, to, count: removed, summary, sessionId })
    return { compacted: removed, summary }
  }

  /** 构造（并判断是否可用）缓存友好摘要请求的前缀：engine 提供 system 提示词与工具 schema，
   *  历史前缀经 deps.loadHistory(upToIndex) 渲染（与主循环同渲染路径）。
   *  不可用时返回 undefined，summarize 退回骨架行 + 分块路径。 */
  private async cachedPrefix(
    llm: LLMProvider,
    sessionId: string,
    user: string,
    upTo: number,
    cachePrefix: SummarizeCachePrefix | undefined,
    priorSummaryBefore: boolean,
  ): Promise<{ messages: MessageLike[]; tools?: CallModelSchemas } | undefined> {
    if (!cachePrefix?.systemPrompt || !priorSummaryBefore) return undefined
    if (!this.cachePreferred(llm)) return undefined // 上次同模型调用未命中缓存（服务商无前缀缓存）：不再白付全价
    const caps = typeof llm?.capabilities === "function" ? llm.capabilities() : undefined
    try {
      const history = await this.deps.loadHistory(sessionId, user, caps?.multimodal ?? false, upTo)
      const messages: MessageLike[] = [{ role: "system", content: cachePrefix.systemPrompt }, ...history]
      return { messages, tools: cachePrefix.tools }
    } catch (err) {
      // 历史渲染失败（附件读文件异常等）不影响压缩本身：退回骨架路径
      if (this.deps.taskSignal(sessionId)?.aborted) throw err
      return undefined
    }
  }

  /** 前缀缓存可用性（按 provider+模型记忆）：上次同模型摘要调用命中过缓存 → 继续用原文前缀；
   *  未命中（服务商无前缀缓存/前缀失配）→ 改写骨架行路径（体积小得多，全价也划算）。
   *  环境变量 GEBAI_COMPACT_CACHE_PREFIX=0/off 强制关闭，=1/on 强制开启（不做自适应）。 */
  private cachePreferred(llm: LLMProvider): boolean {
    const flag = (this.deps.readEnv?.("GEBAI_COMPACT_CACHE_PREFIX") ?? process.env.GEBAI_COMPACT_CACHE_PREFIX ?? "").toLowerCase()
    if (flag === "0" || flag === "off" || flag === "false") return false
    if (flag === "1" || flag === "on" || flag === "true") return true
    return this.cacheHit.get(this.providerKey(llm)) !== false
  }

  private providerKey(llm: LLMProvider): string {
    const caps = typeof llm?.capabilities === "function" ? llm.capabilities() : undefined
    return `${llm?.id ?? "unknown"}:${caps?.model ?? ""}`
  }

  /** 近消息滑动窗口条数（GEBAI_COMPACT_WINDOW 可调，非法值忽略取缺省）。 */
  private windowMessages(): number | undefined {
    const raw = this.deps.readEnv?.("GEBAI_COMPACT_WINDOW") ?? process.env.GEBAI_COMPACT_WINDOW
    if (!raw) return undefined
    const n = Number(raw)
    return Number.isFinite(n) && n >= 2 ? Math.floor(n) : undefined
  }

  /** 溢出护栏降级通知（UI 可见性）：护栏会改动上下文（历史图片降级/最旧用户消息裁剪），
   *  此前只写 console.warn——用户无从得知上下文为何变化；经 event.message.compact 同通道通知
   *  （payload.degraded 标识降级类型，count=0 表示不是压缩替换而是护栏降级）。 */
  private publishDegrade(sessionId: string, summary: string, kind: string): void {
    this.deps.publish(sessionId, "event.message.compact", { sessionId, count: 0, summary, degraded: kind })
  }

  /**
   * 溢出硬护栏（上下文压缩无法收敛时的最后防线）：受保护消息让路——
   * 1) 最旧带图片附件的用户消息：附件图片降级为文本说明（图片永久占窗口且不参与压缩，图片文件本身仍在会话 tmp/ 中）；
   * 2) 仍无图片可降级：最旧用户消息内容替换为裁剪占位（占位后原文不再保留，仅保留头部 200 字符）。
   * 最新一条用户消息（本次任务的输入）永不裁剪——裁掉当前任务输入则任务失去意义。
   * 返回是否发生降级。
   */
  async degradeProtectedMessages(sessionId: string, user: string): Promise<boolean> {
    const session = await this.deps.store.load(sessionId, user)
    if (!session) return false
    let lastUserIdx = -1
    for (let i = session.messages.length - 1; i >= 0; i--) {
      // 引擎提示（提醒/定时任务写回/分支合入，同为 user 角色 + engineNote）**不是**用户输入：不参与
      // 「本次任务输入」定位——否则任务末尾的提示会顶替真输入，真输入反被当作可裁剪历史
      if (session.messages[i].role === "user" && !isEngineNote(session.messages[i])) {
        lastUserIdx = i
        break
      }
    }
    // 注：引擎提示本身**可**被本护栏裁剪（不排除 isEngineNote）——它们都是引擎可再生的历史内容（分支报告全文
    // 在过程存档/bg_task、提醒为一次性提示），体积可能不小（分支报告可达数千字符），不该永占窗口
    // 1) 图片降级（从最旧开始，一次降级一条消息的全部图片）：用户消息的图片附件与工具消息的
    //    图片引用（read 读取的图片）同规则让路
    for (let i = 0; i < session.messages.length; i++) {
      if (i === lastUserIdx) continue
      const m = session.messages[i]
      if (m.role === "tool" && m.images?.length) {
        const imgCount = m.images.length
        const note = m.images.map((img) => `[历史图片已降级为路径说明: ${img.display ?? img.path}，可用 vision/read 工具按需查看]`).join(" ")
        m.content = `${note}\n${m.content}`
        delete m.images
        console.warn(`[engine] 会话 ${sessionId} 溢出护栏：最旧工具消息的 ${imgCount} 张图片降级为文本说明`)
        await this.deps.store.save(session)
        this.publishDegrade(sessionId, `上下文溢出护栏：最旧工具消息的 ${imgCount} 张历史图片已降级为路径说明（可用 vision/read 按需查看）`, "tool-images")
        return true
      }
      if (m.role !== "user" || !m.attachments?.length) continue
      const images = m.attachments.filter((a) => VISION_MIME_SET.has(a.mime))
      if (!images.length) continue
      m.attachments = m.attachments.filter((a) => !VISION_MIME_SET.has(a.mime))
      const note = images.map((a) => `[历史图片已降级为路径说明: ${a.path}（${a.name}），可用 vision/read 工具按需查看]`).join(" ")
      m.content = `${note}\n${m.content}`
      console.warn(`[engine] 会话 ${sessionId} 溢出护栏：最旧用户消息的 ${images.length} 张图片降级为文本说明`)
      await this.deps.store.save(session)
      this.publishDegrade(sessionId, `上下文溢出护栏：最旧用户消息的 ${images.length} 张历史图片已降级为路径说明（可用 vision/read 按需查看）`, "user-images")
      return true
    }
    // 2) 最旧用户消息裁剪占位（最新一条用户消息即本次任务输入，跳过；引擎提示可裁——见上注）
    for (let i = 0; i < session.messages.length; i++) {
      if (i === lastUserIdx) continue
      const m = session.messages[i]
      if (m.role !== "user" || typeof m.content !== "string" || m.content.length <= 500) continue
      if (m.content.startsWith("[历史消息已裁剪")) continue
      const size = m.content.length
      m.content = `[历史消息已裁剪（原 ${size} 字符，原文不再保留）] ${m.content.slice(0, 200)}`
      console.warn(`[engine] 会话 ${sessionId} 溢出护栏：最旧用户消息（${size} 字符）裁剪为占位`)
      await this.deps.store.save(session)
      this.publishDegrade(sessionId, `上下文溢出护栏：最旧用户消息（${size} 字符）已裁剪为占位`, "user-message")
      return true
    }
    return false
  }

  /**
   * 用 LLM 生成最早历史消息的摘要；失败返回降级骨架（滚动裁剪语义）。
   * 输入构造：每条消息压成骨架行（含工具调用名/参数与工具结果脉络），按块预算分块——历史长于单块预算时
   * **逐块摘要再合并**（map-reduce）：原实现把拼接文本一刀切 slice(0, 20000)，越出部分既不进摘要也无提示，
   * 等于静默丢弃——而一次压缩常覆盖 15 万字符以上的历史（52 万 token 窗口的 80% 阈值下），摘要只剭开头一小段。
   * 超出总预算时头尾保留（最早的任务背景与最新进度都在），中部以省略说明行代表。
   * priorSummaries 为既有摘要（滚动合并：旧摘要内容并入新摘要，保证长会话摘要恒为一条且信息不丢）。
   * 默认用启动 Provider；自动压缩（任务内触发）可传入任务级 Provider（与任务同模型）。
   * 读超时与取消信号同主循环（chatWithIdleTimeout 同款防假死）——压缩在任务流程内同步等待，
   * 无超时防护时接口假死会把整个运行中任务永久挂死（isRunning 残留、后续 prompt 全被拒）。
   */
  async summarize(
    slice: Array<Message>,
    provider: LLMProvider = this.deps.getDefaultProvider(),
    signal?: AbortSignal,
    priorSummaries: string[] = [],
    cached?: { messages: MessageLike[]; tools?: CallModelSchemas },
  ): Promise<string> {
    try {
      // 缓存友好前缀路径（首选）：直接把与主循环逐字节同前缀的历史原文 + 尾部压缩指令发出——
      // 服务端前缀缓存命中（已处理过的 token 按缓存价计），且保留原文而非截断到 600 字符
      if (cached && this.cachePreferred(provider)) {
        // 指令统一在 summarize 追加（前缀本身止于主循环历史的最后一条，保证与主循环逐字节同前缀）；
        // 前缀总量必须装得下一次请求（超预算退回骨架路径——骨架路径才是为超大历史设计的）
        const req: MessageLike[] = [...cached.messages, { role: "user", content: CACHE_PREFIX_INSTRUCTION }]
        const caps = typeof provider?.capabilities === "function" ? provider.capabilities() : undefined
        const cap = caps?.maxContextTokens ?? 0
        let fits = true
        if (cap > 0) {
          const budget = Math.max(0, cap - SUMMARY_OUTPUT_RESERVE_TOKENS)
          let est = 0
          for (const m of req) est += estimateMessageLikeTokens(m)
          fits = est <= budget
        }
        if (fits) {
          const { text, usage } = await this.completeTextWithUsage(req, provider, signal, { tools: cached.tools })
          // 缓存命中记忆：服务商无前缀缓存/前缀失配时不白付全价，下次改走骨架路径
          if (usage) this.cacheHit.set(this.providerKey(provider), (usage.cachedTokens ?? 0) > 0)
          if (text) return text.slice(0, SUMMARY_OUTPUT_LIMIT)
          // 空文本（如模型改成调用工具）→ 退回骨架路径
        }
      }
      const lines = [...priorSummaries.map((s) => `[此前摘要] ${clipText(s.trim(), SUMMARY_ITEM_LIMIT)}`), ...slice.map((m) => summarizeMessageLine(m))]
      const chunks = buildSummaryChunks(lines)
      if (!chunks.length) throw new Error("摘要输入为空")
      let summary: string
      if (chunks.length === 1) {
        summary = await this.completeText([{ role: "system", content: SUMMARIZE_PROMPT }, { role: "user", content: chunks[0] }], provider, signal)
      } else {
        // 逐块摘要（单块失败跳过——其余块仍能产出摘要，不因一块异常整体降级为空占位）
        const partials: string[] = []
        for (let i = 0; i < chunks.length; i++) {
          try {
            const part = await this.completeText(
              [{ role: "system", content: chunkPrompt(i + 1, chunks.length) }, { role: "user", content: chunks[i] }],
              provider,
              signal,
            )
            if (part) partials.push(part)
          } catch (err) {
            if (signal?.aborted) throw err
          }
        }
        if (!partials.length) throw new Error("分块摘要均为空")
        summary = partials.length === 1 ? partials[0] : await this.mergeSummaries(partials, provider, signal)
      }
      if (!summary) throw new Error("摘要为空")
      return summary.slice(0, SUMMARY_OUTPUT_LIMIT)
    } catch {
      // 摘要失败（含读超时/取消）降级为滚动裁剪：被裁剪内容保留骨架行，占位文本向模型说明历史已被裁剪
      return summarizeFallback(slice)
    }
  }

  /** 分块摘要合并（map-reduce 的 reduce 步）：合并失败时退化为分块摘要拼接（仍有摘要，不降级为空占位）。 */
  private async mergeSummaries(partials: string[], provider: LLMProvider, signal?: AbortSignal): Promise<string> {
    try {
      const merged = await this.completeText(
        [{ role: "system", content: MERGE_SUMMARY_PROMPT }, { role: "user", content: partials.join("\n\n") }],
        provider,
        signal,
      )
      return merged || partials.join("\n\n")
    } catch (err) {
      if (signal?.aborted) throw err
      return partials.join("\n\n")
    }
  }

  /** 单轮文本补全（压缩/分支报告摘要共用核心）：流式收集文本 chunk，读空闲超时防接口假死（与 callModel 同防护——
   *  无超时防护时接口假死会把调用方（任务内压缩/合入流程）永久挂死）。 */
  async completeText(msgs: MessageLike[], provider: LLMProvider, signal?: AbortSignal): Promise<string> {
    return (await this.completeTextWithUsage(msgs, provider, signal)).text
  }

  /** 同 completeText，另返回 usage（缓存友好摘要路径据此判定前缀缓存是否命中）并支持传同批工具 schema
   *  （前缀缓存多数包含 tools 段：缺了会整体失配，所以摘要请求带上与主循环一致的工具）。
   *  注：工具只作为前缀的一部分——指令明确要求不调用工具，真返回了工具调用则文本为空，调用方退回骨架路径。 */
  async completeTextWithUsage(
    msgs: MessageLike[],
    provider: LLMProvider,
    signal?: AbortSignal,
    opts: { tools?: CallModelSchemas } = {},
  ): Promise<{ text: string; usage?: LLMUsage }> {
    const idleMs = this.deps.idleTimeoutMs
    const iter = provider.chat(msgs, { signal, tools: opts.tools as never })[Symbol.asyncIterator]()
    let text = ""
    let usage: LLMUsage | undefined
    try {
      for (;;) {
        let timer: ReturnType<typeof setTimeout> | undefined
        const timedOut = new Promise<never>((_, reject) => {
          timer = setTimeout(() => reject(new Error(`模型接口读超时（${Math.round(idleMs / 1000)} 秒无数据），判定接口假死`)), idleMs)
        })
        const next = await Promise.race([iter.next(), timedOut])
        if (timer) clearTimeout(timer)
        if (next.done) break
        if (next.value.type === "text") text += next.value.text
        else if (next.value.usage) usage = next.value.usage
      }
    } finally {
      void iter.return?.().catch(() => {})
    }
    return { text: text.trim(), usage }
  }

  /** 分支报告摘要（merge=summary 合入粒度，DESIGN「会话分支运行与合并」）：任务级模型压缩为「结论+要点」，
   *  失败/空结果返回 undefined——调用方全文兜底（合入不因摘要失败而丢失）。 */
  async summarizeBranchReport(content: string, env: Record<string, string>): Promise<string | undefined> {
    try {
      const provider = this.deps.resolveProvider?.(env) ?? this.deps.getDefaultProvider()
      const out = await this.completeText(
        [
          { role: "system", content: "你是并行分支报告压缩器。把分支执行报告压缩为给主线决策用的要点：保留结论、关键发现、产物文件路径、给主线的建议与未尽事项；舍弃过程叙述与客套话。直接输出要点正文（可分条），不超过 400 字。" },
          { role: "user", content: content.slice(0, SUMMARY_INPUT_LIMIT) },
        ],
        provider,
      )
      return out ? out.slice(0, SUMMARY_OUTPUT_LIMIT) : undefined
    } catch {
      return undefined
    }
  }

  /** 上下文溢出（模型服务 4xx 拒绝）判定：真实窗口大小的权威信号——压缩判定不靠估算，
   *  接口报「上下文长度/token 超限」即确凿的溢出证据。匹配 OpenAI/Anthropic/中文网关常见表述。 */
  isContextOverflowError(err: unknown): boolean {
    if (!(err instanceof Error)) return false
    if (!/模型接口错误（HTTP 4\d\d）/.test(err.message)) return false
    return /(context|上下文|prompt|输入|请求).{0,30}(length|长度|limit|exceed|超|过长|太大|过大|too|long)/i.test(err.message) ||
      /too many tokens|token.{0,20}(limit|exceed|超)/i.test(err.message)
  }

  /**
   * 上下文腾挪（真实 usage 驱动的压缩判定落点）：依次尝试
   * 1) 压缩最早可压缩历史为摘要（保留最近一半，受保护消息原位保留）；
   * 2) 无可压缩内容（历史几乎全为受保护消息）时硬护栏降级受保护消息——历史图片降级为
   *    文本说明、最旧用户消息裁剪为占位（原文仍在会话存储中，不丢数据）。
   * 每次改动后重建主循环消息（真实 usage 基线锚点失效，标记清除）。返回是否发生改动。
   * tools 为本次主循环同批工具 schema：传给摘要请求以保持与主循环同前缀（前缀缓存命中）。 */
  async makeContextRoom(
    sessionId: string,
    user: string,
    provider: LLMProvider,
    messages: MessageLike[],
    systemPrompt: string,
    ctx: { ctxInputTokens?: number; ctxCachedTokens?: number; ctxCountedLen: number },
    tools?: CallModelSchemas,
  ): Promise<boolean> {
    const { compacted } = await this.compactSession(sessionId, user, undefined, provider, { internal: true, cachePrefix: { systemPrompt, tools } })
    if (compacted === 0) {
      if (!(await this.degradeProtectedMessages(sessionId, user))) return false
    }
    const fresh = await this.deps.loadHistory(sessionId, user, provider.capabilities().multimodal)
    messages.length = 0
    messages.push({ role: "system", content: systemPrompt }, ...fresh)
    ctx.ctxInputTokens = undefined
    ctx.ctxCachedTokens = undefined
    ctx.ctxCountedLen = 0
    return true
  }

  /**
   * 单轮模型调用（含上下文溢出恢复）：模型服务返回上下文长度 4xx = 真实窗口大小的权威信号——
   * 压缩判定不靠估算，接口拒绝即确凿证据。压缩最早历史后重试（至多 3 次）；恢复期间清除
   * 真实 usage 基线锚点（消息被摘要替换后失效，由下一次成功调用重建）。无可压缩内容或
   * 压缩 3 次仍溢出时上抛原错误（任务失败由调用方呈现）。
   */
  async callModelWithOverflowRecovery(
    sessionId: string,
    user: string,
    provider: LLMProvider,
    messages: MessageLike[],
    schemas: CallModelSchemas,
    systemPrompt: string,
    signal: AbortSignal,
    extraParams: Record<string, unknown> | undefined,
    ctxUsage: { ctxInputTokens?: number; ctxCachedTokens?: number; ctxCountedLen: number },
    onChunk?: (chunk: LLMChunk) => void,
  ): Promise<CallModelResult> {
    try {
      return await this.deps.callModel(provider, messages, schemas, signal, onChunk, extraParams, sessionId)
    } catch (err) {
      if (!this.isContextOverflowError(err)) throw err
      for (let i = 0; i < 3; i++) {
            // 压缩/护栏腾挪均无效（上下文确实无处可让）时恢复失败，上抛原错误
    if (!(await this.makeContextRoom(sessionId, user, provider, messages, systemPrompt, ctxUsage, schemas))) throw err
        try {
          return await this.deps.callModel(provider, messages, schemas, signal, onChunk, extraParams, sessionId)
        } catch (err2) {
          if (!this.isContextOverflowError(err2)) throw err2
          if (i === 2) throw err2 // 压缩 3 次仍溢出：无可收敛空间，上抛
        }
      }
      throw err
    }
  }
}
