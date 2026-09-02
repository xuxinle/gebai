/** 上下文压缩器（自 engine.ts 拆分）：compactSession（主动/自动压缩）+ 溢出硬护栏（受保护消息降级）
 *  + 摘要生成（历史/分支报告）+ 上下文腾挪与溢出恢复重试。engine 经构造注入 deps 委托调用，
 *  AgentEngine 公共 API 不变。 */
import type { Message, MessageLike } from "@gebai/sdk"
import type { LLMChunk, LLMProvider, LLMUsage } from "../llm/llm"
import type { SessionStore } from "../session/store"
import { isProtectedMessage } from "../session/store"
import type { EnvManager } from "../session/env"
import { VISION_MIME_SET } from "../tools/vision"

/** 压缩判定保留比：默认压缩最早部分，保留最近一半可压缩消息。 */
const COMPACT_KEEP_RATIO = 0.5
/** 摘要请求的输入裁剪长度与输出上限。 */
const SUMMARY_INPUT_LIMIT = 20000
const SUMMARY_OUTPUT_LIMIT = 2000

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
  loadHistory: (sessionId: string, user: string, inlineMultimodal?: boolean) => Promise<MessageLike[]>
  callModel: (provider: LLMProvider, messages: MessageLike[], schemas: CallModelSchemas, signal: AbortSignal, onChunk?: (chunk: LLMChunk) => void, extraParams?: Record<string, unknown>, sessionId?: string) => Promise<CallModelResult>
}

export class ContextCompressor {
  constructor(private deps: CompressorDeps) {}

  async compactSession(
    sessionId: string,
    user: string,
    scope?: "all" | { from: number; to: number },
    provider?: LLMProvider,
    opts: { internal?: boolean } = {},
  ): Promise<{ compacted: number; summary: string }> {
    // 手动压缩（UI/REST 入口）在任务运行中被拒：summarize 是秒级 LLM 调用，期间任务持续追加消息，
    // 陈旧压缩区间会套删未参与摘要的新落盘内容（自动压缩经 internal 标记在任务流程内自身协调，不受此限）
    if (!opts.internal && this.deps.isTaskRunning(sessionId)) throw new Error("会话有任务正在运行，暂不能手动压缩；请等待任务完成或先停止任务")
    const session = await this.deps.store.load(sessionId, user)
    if (!session) throw new Error(`会话不存在: ${sessionId}`)
    // Provider 未显式指定（UI/REST 主动压缩入口）时按合并后 env 解析：用户/会话级模型配置同样生效
    const llm = provider ?? (this.deps.resolveProvider ? this.deps.resolveProvider(await this.deps.env.resolve(sessionId, user)) : this.deps.getDefaultProvider())
    const messages = session.messages
    // 可压缩消息：排除受保护消息（isProtectedMessage：系统提示词含装载提示词/压缩摘要、用户输入、
    // 新会话执行存档）——用户输入与系统提示词不压缩不改变（不选进区间、不进摘要输入，
    // 区间夹带时由 compactMessages 原位保留）；重复压缩只压缩新增长的 assistant/tool 历史
    const compactable: number[] = []
    for (let i = 0; i < messages.length; i++) {
      if (!isProtectedMessage(messages[i])) compactable.push(i)
    }
    if (compactable.length < 2) return { compacted: 0, summary: "" }

    let from: number
    let to: number
    if (scope && typeof scope === "object") {
      from = Math.max(0, Math.min(scope.from, messages.length))
      to = Math.min(messages.length, scope.to)
    } else if (scope === "all") {
      from = compactable[0]
      to = compactable[compactable.length - 1] + 1
    } else {
      // 默认：压缩最早部分，保留最近一半（保证最新上下文完整）
      const keep = Math.ceil(compactable.length * COMPACT_KEEP_RATIO)
      const target = compactable.slice(0, Math.max(0, compactable.length - keep))
      if (target.length < 2) return { compacted: 0, summary: "" }
      from = target[0]
      to = target[target.length - 1] + 1
    }
    if (from >= to) return { compacted: 0, summary: "" }

    const slice = messages.slice(from, to)
    const kept = slice.filter(isProtectedMessage)
    const removed = slice.length - kept.length
    if (removed === 0) return { compacted: 0, summary: "" } // 区间内仅剩受保护消息：无可压缩内容
    // 摘要输入只含将被移除的消息（受保护消息原样保留，无需进摘要）
    const summary = await this.summarize(slice.filter((m) => !isProtectedMessage(m)), llm, this.deps.taskSignal(sessionId))
    await this.deps.store.compactMessages(sessionId, user, { from, to, summary })
    this.deps.publish(sessionId, "event.message.compact", { from, to, count: removed, summary, sessionId })
    return { compacted: removed, summary }
  }

  /**
   * 溢出硬护栏（上下文压缩无法收敛时的最后防线）：受保护消息让路——
   * 1) 最旧带图片附件的用户消息：附件图片降级为文本说明（图片永久占窗口且不参与压缩）；
   * 2) 仍无图片可降级：最旧用户消息内容替换为裁剪占位（原文仍在 chat.json，UI 可查、不丢数据）。
   * 最新一条用户消息（本次任务的输入）永不裁剪——裁掉当前任务输入则任务失去意义。
   * 返回是否发生降级。
   */
  async degradeProtectedMessages(sessionId: string, user: string): Promise<boolean> {
    const session = await this.deps.store.load(sessionId, user)
    if (!session) return false
    let lastUserIdx = -1
    for (let i = session.messages.length - 1; i >= 0; i--) {
      if (session.messages[i].role === "user") {
        lastUserIdx = i
        break
      }
    }
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
      return true
    }
    // 2) 最旧用户消息裁剪占位（最新一条用户消息即本次任务输入，跳过）
    for (let i = 0; i < session.messages.length; i++) {
      if (i === lastUserIdx) continue
      const m = session.messages[i]
      if (m.role !== "user" || typeof m.content !== "string" || m.content.length <= 500) continue
      if (m.content.startsWith("[历史消息已裁剪")) continue
      const size = m.content.length
      m.content = `[历史消息已裁剪（原 ${size} 字符，原文仍在会话记录中可查看）] ${m.content.slice(0, 200)}`
      console.warn(`[engine] 会话 ${sessionId} 溢出护栏：最旧用户消息（${size} 字符）裁剪为占位`)
      await this.deps.store.save(session)
      return true
    }
    return false
  }

  /** 用 LLM 生成最早历史消息的摘要；失败返回降级占位文本（滚动裁剪语义）。
   *  默认用启动 Provider；自动压缩（任务内触发）可传入任务级 Provider（与任务同模型）。
   *  读超时与取消信号同主循环（chatWithIdleTimeout 同款防假死）——压缩在任务流程内同步等待，
   *  无超时防护时接口假死会把整个运行中任务永久挂死（isRunning 残留、后续 prompt 全被拒）。 */
  async summarize(slice: Array<Message>, provider: LLMProvider = this.deps.getDefaultProvider(), signal?: AbortSignal): Promise<string> {
    try {
      const text = slice
        .map((m) => `[${m.role}] ${m.content}`)
        .join("\n")
        .slice(0, SUMMARY_INPUT_LIMIT)
      const msgs: MessageLike[] = [
        {
          role: "system",
          content: "你是对话压缩器。将以下对话历史压缩为一段简洁中文摘要：保留关键决定、结论、任务进度、文件路径与待办，舍弃寒暄与过程细节。不超过 500 字，直接输出摘要正文。",
        },
        { role: "user", content: text },
      ]
      const summary = await this.completeText(msgs, provider, signal)
      if (!summary) throw new Error("摘要为空")
      return summary.slice(0, SUMMARY_OUTPUT_LIMIT)
    } catch {
      // 摘要失败（含读超时/取消）降级为滚动裁剪：仅丢弃，占位文本向模型说明历史已被裁剪
      return "[上下文已裁剪：历史消息过多，已丢弃最早部分。如需详情可查看会话文件。]"
    }
  }

  /** 单轮文本补全（压缩/分支报告摘要共用核心）：流式收集文本 chunk，读空闲超时防接口假死（与 callModel 同防护——
   *  无超时防护时接口假死会把调用方（任务内压缩/合入流程）永久挂死）。 */
  async completeText(msgs: MessageLike[], provider: LLMProvider, signal?: AbortSignal): Promise<string> {
    const idleMs = this.deps.idleTimeoutMs
    const iter = provider.chat(msgs, { signal })[Symbol.asyncIterator]()
    let text = ""
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
      }
    } finally {
      void iter.return?.().catch(() => {})
    }
    return text.trim()
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
   */
  async makeContextRoom(
    sessionId: string,
    user: string,
    provider: LLMProvider,
    messages: MessageLike[],
    systemPrompt: string,
    ctx: { ctxInputTokens?: number; ctxCachedTokens?: number; ctxCountedLen: number },
  ): Promise<boolean> {
    const { compacted } = await this.compactSession(sessionId, user, undefined, provider, { internal: true })
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
        if (!(await this.makeContextRoom(sessionId, user, provider, messages, systemPrompt, ctxUsage))) throw err
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
