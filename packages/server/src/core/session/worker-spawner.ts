/**
 * `WorkerSpawner` 的宿主实现：**driver 派一个隔离会话去干活，跑完把结论回报给 driver**。
 *
 * ## 为什么 worker 是「独立会话」而不是 subsession
 *
 * 两者都能做到「独立上下文」，差别在**谁拥有它**：
 *
 * | | subsession | worker（本实现） |
 * |---|---|---|
 * | 归属 | 挂在**某个父任务的工具上下文**里（`ToolContext.subSessions`，随 run 生命周期） | 独立会话，`(personaId, driverId)` 派出的活 |
 * | 谁能启动 | 只有正在跑的会话（要 fork 源、父信号、注册表视角） | 任何时刻（driver 派活时 driver 自己在跑，但语义上这活**不依附于**它这一轮） |
 * | 退出后 | 存档留在父会话 | 会话删除（结论已进 driver 事件，追溯在 driver 那边） |
 *
 * persona 的 worker 是「**按需、跑完即走**」（见 `worker.ts` 文件头的架构图），不是
 * 「父任务的一次子分支」。绑到父 run 上会出现：driver 这一轮结束 → worker 被连带取消 →
 * 而它本该继续干。所以这里用独立会话。
 *
 * ## 为什么跑完就删会话
 *
 * worker 会话出现在侧栏会变成噪音：一个活跃人格一天能派几十个，用户看到的是
 * 「小霜·派活·查tsdb」×20 而不是他要的对话。而**结论并没有丢**——`workerReportEvent`
 * 把它变成 driver 的一个事件，落在 driver 会话里，从那里可追溯。
 *
 * 删失败只记日志：留一个僵尸会话比丢结论轻得多，不值得让派活失败。
 *
 * ## 取消
 *
 * `cancel(runId)` → `engine.cancel(workerSessionId)`：与用户点「停止」同一条路径
 * （abort 信号 + 解开挂起等待），因此不会出现「取消后任务仍卡着」。
 */

import type { WorkerOutcome, WorkerSpawnInput, WorkerSpawner } from "@gebai/persona"
import type { SessionStore, SessionData } from "./store"

/** 引擎面（只声明 worker 需要的部分，便于单测）。 */
export interface WorkerEnginePort {
  run(
    sessionId: string,
    user: string,
    prompt: string,
    opts?: {
      channelNote?: string
      autoApprove?: boolean
      /** 无交互通道（worker 天然无前端）：交互类工具直接不可用 */
      interactionMode?: "none" | "multi_turn" | "realtime"
    },
  ): Promise<void>
  cancel(sessionId: string): void
  isRunning(sessionId: string): boolean
}

/** worker 会话的最小存储面。 */
export interface WorkerStorePort {
  createSession(userId: string, name?: string): Promise<SessionData>
  load(id: string, user?: string): Promise<SessionData | null>
  delete(id: string, userId: string): Promise<void>
}

export interface SessionWorkerSpawnerOptions {
  engine: WorkerEnginePort
  store: SessionStore | WorkerStorePort
  /** worker 是否自动通过审批：无人值守派出的活没有人守审批卡（与 driver 同姿态）。 */
  autoApprove?: boolean
  /** 单个 worker 的运行时限（毫秒；缺省 20 分钟）——到点取消，防止一个死循环的活永远占着。 */
  timeoutMs?: number
  log?: { info(msg: string): void; warn(msg: string): void }
  now?: () => number
}

const DEFAULT_WORKER_TIMEOUT_MS = 20 * 60 * 1000

/**
 * worker 的提示词。
 *
 * 三件必须写进去的事：
 *
 * 1. **你是谁派出来的**：worker 是独立上下文，除了这段文本它一无所知
 * 2. **交付形态**：结论要「可直接引用」——driver 会拿它组织答复，中间过程没用
 * 3. **不许再派活**：worker 里没有 `worker_*` 工具（工具面本来就没有），但明确说一句
 *    能省掉它去尝试 `subsession_run` 的往返
 */
export function workerPrompt(input: WorkerSpawnInput): string {
  const lines: string[] = []
  lines.push(`# 你被派出去干一件具体的活`)
  lines.push("")
  lines.push(`【派出者】${input.identity.name}（${input.identity.role}）的分身 \`${input.driverId}\``)
  lines.push(`【任务】`)
  lines.push(input.task.trim())
  if (input.context?.trim()) {
    lines.push("")
    lines.push(`【已知背景】（派出者已查到的，不必重复查）`)
    lines.push(input.context.trim())
  }
  if (input.verify?.trim()) {
    lines.push("")
    lines.push(`【验收标准】`)
    lines.push(input.verify.trim())
  }
  lines.push("")
  lines.push("## 交付要求")
  lines.push("")
  lines.push("- 你的最终回复会**原样交给派出者**，由它组织对外答复。所以要给**结论**，不要给过程叙述。")
  lines.push("- 有具体位置就写具体位置（文件:行号 / 命令 / 链接 / 具体数值）；说不确定就说不确定。")
  lines.push("- 没查到的、做不到的，如实说明并给出下一步建议——**不要把没做成写成做成了**。")
  lines.push("- 你用**独立上下文**：派出者对话里的东西你看不到，需要什么自己查。")
  return lines.join("\n")
}

/** worker 会话名（诊断用；跑完即删，正常不会出现在侧栏）。 */
function workerName(input: WorkerSpawnInput): string {
  const label = (input.label?.trim() || input.task.trim().split("\n")[0] || "活").slice(0, 24)
  return `⚙ ${input.identity.name}·派活·${label}`
}

/**
 * 从 worker 会话里取最终结论。
 *
 * 取**最后一条 assistant 文本消息**：`engine.run` 结束后会话里已有完整记录，
 * 最后一次助手回复就是模型自己的收口（工具调用的中间轮 assistant 消息内容为空或有
 * toolCalls，跳过即可）。
 */
export function finalAnswerOf(session: SessionData | null): string {
  if (!session) return ""
  for (let i = session.messages.length - 1; i >= 0; i--) {
    const m = session.messages[i]!
    if (m.role !== "assistant") continue
    const text = typeof m.content === "string" ? m.content : Array.isArray(m.content) ? extractText(m.content) : ""
    if (text.trim()) return text.trim()
  }
  return ""
}

function extractText(blocks: unknown[]): string {
  const out: string[] = []
  for (const b of blocks) {
    const o = b as { type?: string; text?: unknown }
    if (o?.type === "text" && typeof o.text === "string") out.push(o.text)
  }
  return out.join("\n")
}

export class SessionWorkerSpawner implements WorkerSpawner {
  private readonly engine: WorkerEnginePort
  private readonly store: WorkerStorePort
  private readonly autoApprove: boolean
  private readonly timeoutMs: number
  private readonly log?: SessionWorkerSpawnerOptions["log"]
  private readonly now: () => number
  /** runId → 会话 id（取消用）。跑完即删条目。 */
  private readonly live = new Map<string, string>()

  constructor(opts: SessionWorkerSpawnerOptions) {
    this.engine = opts.engine
    this.store = opts.store as WorkerStorePort
    this.autoApprove = opts.autoApprove !== false
    this.timeoutMs = opts.timeoutMs && opts.timeoutMs > 0 ? opts.timeoutMs : DEFAULT_WORKER_TIMEOUT_MS
    this.log = opts.log
    this.now = opts.now ?? Date.now
  }

  async spawn(input: WorkerSpawnInput): Promise<WorkerOutcome> {
    const startedAt = this.now()
    const runId = input.runId?.trim() || `worker_${startedAt.toString(36)}_${Math.random().toString(36).slice(2, 8)}`
    const label = input.label ?? input.task.trim().split("\n")[0]?.slice(0, 40)

    let sessionId: string | null = null
    let timer: ReturnType<typeof setTimeout> | undefined
    try {
      if (input.signal.aborted) {
        return { runId, status: "cancelled", error: "派活时已被取消", ...(label ? { label } : {}), elapsedMs: 0 }
      }
      const session = await this.store.createSession(input.user, workerName(input))
      sessionId = session.id
      this.live.set(runId, sessionId)
      // 取消传播：driver 那一轮被停（用户点停止 / 人格下线）不该把已经派出的活连带杀掉，
      // 但**显式取消**（worker_cancel / 人格停止）要立即生效。这里只接租户级停止：
      // signal 在 worker 生命周期内通常不会 abort，接了也不会误伤。
      const onAbort = () => {
        if (sessionId) this.engine.cancel(sessionId)
      }
      input.signal.addEventListener("abort", onAbort, { once: true })

      // 运行时限：到点取消（防止死循环的活永远占着；结论按「已掌握的信息」由模型自己收口
      // 做不到——engine.cancel 是硬中止，所以这里如实报 timeout 而不是假装成功）
      timer = setTimeout(() => {
        this.log?.warn(`[persona:worker] ${runId} 超过 ${Math.round(this.timeoutMs / 60000)} 分钟，强制取消`)
        if (sessionId) this.engine.cancel(sessionId)
      }, this.timeoutMs)
      timer.unref?.()

      this.log?.info(`[persona:worker] ${input.personaId}/${input.driverId} 派出 ${runId}（会话 ${sessionId}）：${label ?? ""}`)
      try {
        await this.engine.run(sessionId, input.user, workerPrompt(input), {
          channelNote: workerNote(input),
          ...(this.autoApprove ? { autoApprove: true } : {}),
          // 无交互通道：ask/show 等交互类工具对 worker 无意义（没人作答），直接不提供
          interactionMode: "none",
        })
      } finally {
        input.signal.removeEventListener("abort", onAbort)
      }

      const after = await this.loadQuietly(sessionId, input.user)
      const report = finalAnswerOf(after)
      const elapsedMs = this.now() - startedAt
      const cancelled = input.signal.aborted
      if (cancelled) {
        return { runId, status: "cancelled", error: "派活被取消", ...(label ? { label } : {}), elapsedMs }
      }
      if (!report) {
        // 跑完了但没有文本结论：这是真实失败形态（模型只调了工具就结束），要如实说，
        // 不能当成成功把空结论回报给 driver（driver 会以为"查完了，没发现问题"）
        return {
          runId,
          status: "failed",
          error: "worker 结束了但没有给出任何文本结论（可能只做了工具调用就停止）",
          ...(label ? { label } : {}),
          elapsedMs,
        }
      }
      return { runId, status: "done", report, ...(label ? { label } : {}), elapsedMs }
    } catch (err) {
      const elapsedMs = this.now() - startedAt
      const message = String((err as Error)?.message || err)
      // 取消与超时都表现为 abort 抛错：区分开，driver 需要知道「这活是被叫停的」还是「它自己炸了」
      const cancelled = input.signal.aborted || /cancel|abort|终止|中断/i.test(message)
      return {
        runId,
        status: cancelled ? "cancelled" : "failed",
        error: message,
        ...(label ? { label } : {}),
        elapsedMs,
      }
    } finally {
      if (timer) clearTimeout(timer)
      this.live.delete(runId)
      // 跑完即走：删掉 worker 会话（结论已通过 WorkerOutcome 交回，追溯在 driver 会话里）
      if (sessionId) {
        await this.store.delete(sessionId, input.user).catch((err) => {
          this.log?.warn(`[persona:worker] 清理 worker 会话 ${sessionId} 失败（不影响结论）: ${String((err as Error)?.message || err)}`)
        })
      }
    }
  }

  async cancel(runId: string): Promise<void> {
    const sessionId = this.live.get(runId)
    if (!sessionId) {
      this.log?.warn(`[persona:worker] 取消 ${runId} 失败：没有这个在跑的活（可能已结束）`)
      return
    }
    this.engine.cancel(sessionId)
  }

  /** 当前在跑的 worker（诊断/面板）。 */
  running(): Array<{ runId: string; sessionId: string }> {
    return [...this.live.entries()].map(([runId, sessionId]) => ({ runId, sessionId }))
  }

  private async loadQuietly(sessionId: string, user: string): Promise<SessionData | null> {
    try {
      return await this.store.load(sessionId, user)
    } catch {
      return null
    }
  }
}

/**
 * worker 的通道注记（进系统提示词）。
 *
 * 与 driver 的 `personaNote` 不同：worker **不对外发言**——它把结论交给 driver，
 * 由 driver 决定怎么组织答复。这条差别必须写明，否则 worker 会自己去 `act` 发消息，
 * 于是同一个问题在群里被回答两遍（一遍 driver 的、一遍 worker 的）。
 */
export function workerNote(input: WorkerSpawnInput): string {
  return [
    "## 你的位置",
    `你是「${input.identity.name}」（${input.identity.role}）临时派出的工作会话，只负责本次任务。`,
    "",
    "## 硬约束",
    "",
    "1. **不要对外发消息**：不要调用 act / observe 向任何人发言。你的结论会交给派出者，由它组织答复。",
    "   你自己发一遍 = 对方收到两遍（派出者也会回一次）。",
    "2. **不要派活**：你已经是执行末端，需要什么自己动手做。",
    "3. 只做这一件事；发现它牵扯到别的方向，在结论里点出来，由派出者决定要不要另开一个活。",
  ].join("\n")
}
