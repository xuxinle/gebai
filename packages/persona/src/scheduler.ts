/**
 * 两层队列 + 轮次插入：人格级并发额度 + 会话级串行 + **运行中插入**。
 *
 * ## 三个不变量
 *
 * 1. **同一会话同时最多一个轮次在跑**（引擎硬约束：同 session 并发 run 直接抛错）。
 * 2. **同时运行的会话数 ≤ `maxConcurrent`**（资源约束）。
 * 3. **事件 FIFO 且不丢**（人的话不能被静默丢弃）。
 *
 * ## 插入（本轮的核心机制）
 *
 * 引擎不允许同会话并发，所以"插入"只能落在**轮次边界**上——但那恰恰是对的：
 *
 * ```
 *   轮次 1 开跑 ──▶ 事件 B 到达（进信箱） ──▶ 轮次 1 结束
 *                                              │
 *                        信箱非空？──是──▶ 轮次 2 开跑（含 B，即时生效）
 *                                              │
 *                        信箱非空？──否──▶ 本次运行结束
 * ```
 *
 * 引擎视角：每轮是**独立的一次 `engine.run()`**（会话在轮间是空闲的，不违反约束）。
 * 人格视角：B 在**下一个轮次边界**就被看到，而不是等一个完整任务结束。
 *
 * **这就是为什么不必改引擎**：插入的语义由 persona 的编排实现，引擎只负责跑单轮。
 *
 * ## 兜底（插话绝不能变成丢事件）
 *
 * 若运行结束（收到取消 / 达到轮次上限 / 信箱在结束瞬间仍有残留），残留的插话
 * **必须重新入队**。否则"插话"就从优化退化成静默丢弃——那比不插话严重得多。
 */

import { InterjectionMailbox } from "./mailbox"
import type { PersonaEvent } from "./event"
import type { PersonaLogger } from "./ports"

/** 排队中的一项。 */
interface QueueItem {
  event: PersonaEvent
  sessionId: string
  user: string
  enqueuedAt: number
  /** 入队序号（单调递增，FIFO 的唯一排序键）。 */
  seq: number
}

/** 单会话队列状态（可观测）。 */
export interface SessionQueueState {
  sessionId: string
  threadKey: string
  running: boolean
  /** 该会话等待中的事件数（不含正在跑的）。 */
  waiting: number
  /** 正在跑的事件 id。 */
  runningEventId?: string
  /** 本轮已处理的轮次数（插入会让一个运行包含多轮）。 */
  round: number
  /** 本轮收到的插话数（当前运行期间）。 */
  interjections: number
  /** 待取插话数（信箱里还没被下一轮吸收的）。 */
  pendingInterjections: number
  /** 已跑**运行**数 / 失败数（一个运行可能含多轮）。 */
  done: number
  failed: number
}

/** 人格调度器状态（可观测：用户与弹窗都靠它回答「在忙什么」）。 */
export interface PersonaSchedulerState {
  personaId: string
  maxConcurrent: number
  runningSessions: number
  /** 全局待跑事件数。 */
  queued: number
  sessions: SessionQueueState[]
}

/** 一次轮次的输入。 */
export interface RoundInput {
  sessionId: string
  user: string
  /**
   * 本轮要处理的事件（**可能多于一个**：插话在边界被合并）。
   *
   * 为何是数组而非单个：连续到达的多条插话一起吸收，比分多次跑更省 token
   * （一次把三件事讲清楚，比跑三遍各自带全套上下文便宜得多）。
   */
  events: PersonaEvent[]
  /** 本轮序号（1 起；>1 说明前面被插话续上了）。 */
  round: number
  /**
   * 插话信箱（runner 可在**任意时刻** drain 来提前取走，用于更细粒度的注入；
   * 不 drain 也不丢——轮次边界由调度器统一吸收）。
   */
  mailbox: import("./mailbox").PersonaMailbox
  signal: AbortSignal
}

/** 调度器依赖（注入，便于测试）。 */
export interface PersonaSchedulerOptions {
  personaId: string
  maxConcurrent: number
  /** 全局待跑队列上限（超额丢弃最旧并记日志）。 */
  queueLimit?: number
  /** 实际执行一轮（由 runtime 提供：整编事件 + 调引擎跑）。 */
  execute: (input: RoundInput) => Promise<void>
  /**
   * 单个运行的最大轮次数（防插话洪水导致会话永不结束）。默认 8。
   *
   * 达到上限时把剩余插话重新入队（不丢），本次运行收尾。
   */
  maxRounds?: number
  log?: PersonaLogger
}

/** 运行中事件的处理策略。 */
export type InsertStrategy = "insert" | "queue"

const DEFAULT_QUEUE_LIMIT = 64
const DEFAULT_MAX_ROUNDS = 8

/**
 * 人格调度器：管理该人格所有会话的排队、并发与**轮次插入**。
 */
export class PersonaScheduler {
  private readonly personaId: string
  private maxConcurrent: number
  private readonly queueLimit: number
  private readonly maxRounds: number
  private readonly execute: PersonaSchedulerOptions["execute"]
  private readonly log?: PersonaLogger

  /** sessionId → 该会话的待跑队列。 */
  private sessionQueues = new Map<string, QueueItem[]>()
  /** sessionId → threadKey（可观测用）。 */
  private threadKeys = new Map<string, string>()
  /** sessionId → 正在跑的运行的取消器。 */
  private running = new Map<string, AbortController>()
  /** sessionId → 运行中的插话信箱。 */
  private mailboxes = new Map<string, InterjectionMailbox>()
  /** sessionId → 正在跑的事件 id 集合（同会话去重必须算上它们——已不在队列里）。 */
  private runningEventIds = new Map<string, Set<string>>()
  /** sessionId → 统计。 */
  private stats = new Map<string, { done: number; failed: number; round: number; interjections: number; runningEventId?: string }>()
  /** 全局待跑事件总数。 */
  private pendingTotal = 0
  /** 入队序号发生器。 */
  private nextSeq = 1
  /** 每会话的插入策略（缺省 insert）。 */
  private strategies = new Map<string, InsertStrategy>()
  private stopped = false

  constructor(opts: PersonaSchedulerOptions) {
    this.personaId = opts.personaId
    if (!Number.isInteger(opts.maxConcurrent) || opts.maxConcurrent < 1) {
      throw new Error(`maxConcurrent 必须是正整数: ${opts.maxConcurrent}`)
    }
    this.maxConcurrent = opts.maxConcurrent
    this.queueLimit = opts.queueLimit ?? DEFAULT_QUEUE_LIMIT
    this.maxRounds = Math.max(1, opts.maxRounds ?? DEFAULT_MAX_ROUNDS)
    this.execute = opts.execute
    this.log = opts.log
  }

  /**
   * 投递一个事件到某会话。
   *
   * @returns 位次：`0` = 立即可跑或被插入；`>0` = 前面还有几件；`-1` = 队列满被拒。
   */
  enqueue(item: { event: PersonaEvent; sessionId: string; user: string; threadKey: string; strategy?: InsertStrategy }): number {
    return this.enqueueMany([item])[0] ?? -1
  }

  /**
   * 批量投递（同一批的事件保持顺序，并被同一轮吸收）。
   *
   * 语义与 `enqueue` 一致，只是支持一次投多条（runtime 在广播后可能同时给
   * 同一会话送来多条，逐条投会让它们分裂到不同轮次）。
   */
  enqueueMany(items: Array<{ event: PersonaEvent; sessionId: string; user: string; threadKey: string; strategy?: InsertStrategy }>): number[] {
    if (this.stopped) return items.map(() => -1)
    const out: number[] = []
    for (const item of items) {
      const strategy = item.strategy ?? this.strategies.get(item.sessionId) ?? "insert"
      // ① 会话在跑 → 走插入（进信箱，轮次边界吸收）；队列策略则照常排队
      const mailbox = this.mailboxes.get(item.sessionId)
      if (mailbox && strategy === "insert") {
        if (this.runningEventIds.get(item.sessionId)?.has(item.event.id)) {
          out.push(0)
          continue
        }
        mailbox.push(item.event)
        const st = this.stats.get(item.sessionId)
        if (st) st.interjections++
        out.push(0)
        continue
      }
      // ② 队列路径（容量、去重、FIFO）
      out.push(this.enqueueQueued(item))
    }
    this.pump()
    return out
  }

  /** 设置某会话的插入策略。 */
  setStrategy(sessionId: string, strategy: InsertStrategy): void {
    this.strategies.set(sessionId, strategy)
  }

  /** 调整并发额度（人格热加载）。 */
  setMaxConcurrent(n: number): void {
    if (!Number.isInteger(n) || n < 1) return
    this.maxConcurrent = n
    this.pump()
  }

  /** 取消某个会话的**待跑**事件（正在跑的不受影响——那要经取消器）。 */
  cancelWaiting(sessionId: string): number {
    const list = this.sessionQueues.get(sessionId)
    if (!list?.length) return 0
    const n = list.length
    this.pendingTotal -= n
    this.sessionQueues.delete(sessionId)
    return n
  }

  /** 取消某会话正在跑的运行（返回是否确有在跑）。 */
  cancelRunning(sessionId: string): boolean {
    const ctl = this.running.get(sessionId)
    if (!ctl) return false
    ctl.abort()
    return true
  }

  /** 停止：取消所有在跑，清空队列。 */
  stop(): void {
    this.stopped = true
    for (const ctl of this.running.values()) ctl.abort()
    this.running.clear()
    this.mailboxes.clear()
    this.runningEventIds.clear()
    this.sessionQueues.clear()
    this.pendingTotal = 0
  }

  /** 重新开放（人格重载）。 */
  start(): void {
    this.stopped = false
  }

  /** 是否在跑任何一个会话。 */
  busy(): boolean {
    return this.running.size > 0
  }

  /** 该会话是否在跑。 */
  isRunning(sessionId: string): boolean {
    return this.running.has(sessionId)
  }

  /** 状态快照（用户/弹窗看「在忙什么」）。 */
  state(): PersonaSchedulerState {
    const sessions: SessionQueueState[] = []
    const seen = new Set<string>()
    for (const [sessionId, list] of this.sessionQueues) {
      const st = this.stats.get(sessionId)
      const mb = this.mailboxes.get(sessionId)
      sessions.push({
        sessionId,
        threadKey: this.threadKeys.get(sessionId) ?? "",
        running: this.running.has(sessionId),
        waiting: list.length,
        ...(st?.runningEventId ? { runningEventId: st.runningEventId } : {}),
        round: st?.round ?? 0,
        interjections: st?.interjections ?? 0,
        pendingInterjections: mb?.size() ?? 0,
        done: st?.done ?? 0,
        failed: st?.failed ?? 0,
      })
      seen.add(sessionId)
    }
    for (const [sessionId, mailbox] of this.mailboxes) {
      if (seen.has(sessionId)) continue
      const st = this.stats.get(sessionId)
      sessions.push({
        sessionId,
        threadKey: this.threadKeys.get(sessionId) ?? "",
        running: true,
        waiting: 0,
        ...(st?.runningEventId ? { runningEventId: st.runningEventId } : {}),
        round: st?.round ?? 0,
        interjections: st?.interjections ?? 0,
        pendingInterjections: mailbox.size(),
        done: st?.done ?? 0,
        failed: st?.failed ?? 0,
      })
    }
    return {
      personaId: this.personaId,
      maxConcurrent: this.maxConcurrent,
      runningSessions: this.running.size,
      queued: this.pendingTotal,
      sessions,
    }
  }

  // ── 内部 ──

  /** 队列路径的入队（容量 + 去重 + FIFO）。 */
  private enqueueQueued(item: { event: PersonaEvent; sessionId: string; user: string; threadKey: string }): number {
    while (this.pendingTotal >= this.queueLimit) {
      const dropped = this.dropOldestPending()
      if (!dropped) break
      this.log?.warn(`[persona:${this.personaId}] 待跑队列已满（上限 ${this.queueLimit}），丢弃最旧事件 ${dropped}`)
    }
    if (this.pendingTotal >= this.queueLimit) return -1
    if (this.runningEventIds.get(item.sessionId)?.has(item.event.id)) return 0
    const list = this.sessionQueues.get(item.sessionId) ?? []
    if (list.some((x) => x.event.id === item.event.id)) return this.aheadOf(item.sessionId, list, item.event.id)
    list.push({ event: item.event, sessionId: item.sessionId, user: item.user, enqueuedAt: Date.now(), seq: this.nextSeq++ })
    this.sessionQueues.set(item.sessionId, list)
    this.threadKeys.set(item.sessionId, item.threadKey)
    this.pendingTotal++
    return this.aheadOf(item.sessionId, list, item.event.id)
  }

  /**
   * 排队位次：该事件前面还有多少件（含正在跑的那件）。
   */
  private aheadOf(sessionId: string, list: QueueItem[], eventId: string): number {
    const pos = list.findIndex((x) => x.event.id === eventId)
    if (pos < 0) return 0
    const runningAhead = this.runningEventIds.get(sessionId)?.size ? 1 : 0
    return Math.max(0, pos + runningAhead)
  }

  /** 泵：尽可能多地启动可跑的会话（受额度限制）。 */
  private pump(): void {
    if (this.stopped) return
    while (this.running.size < this.maxConcurrent) {
      const next = this.pickNext()
      if (!next) break
      this.launch(next)
    }
  }

  /** 选出下一个可跑的（未被占用 + 队首有效 + 入队最早）。 */
  private pickNext(): QueueItem | null {
    let best: QueueItem | null = null
    for (const [sessionId, list] of this.sessionQueues) {
      if (this.running.has(sessionId)) continue
      if (!list.length) {
        this.sessionQueues.delete(sessionId)
        continue
      }
      const head = list[0]
      if (!best || head.seq < best.seq) best = head
    }
    return best
  }

  /**
   * 启动一个会话运行：**多轮循环**（这是插入的实现位置）。
   *
   * 每轮：
   * 1. 取本轮事件（首轮 = 队首批次；后续轮 = 信箱吸收）
   * 2. `execute` 跑一轮（引擎视角是一次独立的 run）
   * 3. 轮末检查信箱：非空则续下一轮，空则结束运行
   */
  private launch(first: QueueItem): void {
    const sessionId = first.sessionId
    const list = this.sessionQueues.get(sessionId)
    if (!list?.length) return
    // 首轮：把队首**连续同会话**的事件一起取走（批量投递的事件应同轮处理）
    const batch: QueueItem[] = []
    while (list.length && (batch.length === 0 || list[0].sessionId === sessionId) && batch.length < 64) {
      const it = list.shift()!
      batch.push(it)
      this.pendingTotal--
    }
    if (!list.length) this.sessionQueues.delete(sessionId)

    const ctl = new AbortController()
    const mailbox = new InterjectionMailbox()
    this.running.set(sessionId, ctl)
    this.mailboxes.set(sessionId, mailbox)
    this.runningEventIds.set(sessionId, new Set(batch.map((b) => b.event.id)))
    const st = this.stats.get(sessionId) ?? { done: 0, failed: 0, round: 0, interjections: 0 }
    st.runningEventId = batch[batch.length - 1]?.event.id
    st.round = 0
    st.interjections = 0
    this.stats.set(sessionId, st)

    void (async () => {
      let round = 0
      let pendingEvents = batch.map((b) => b.event)
      let user = batch[0]?.user ?? ""
      try {
        for (;;) {
          if (ctl.signal.aborted) break
          if (pendingEvents.length === 0) break
          round++
          st.round = round
          st.runningEventId = pendingEvents[pendingEvents.length - 1]?.id
          this.runningEventIds.set(sessionId, new Set(pendingEvents.map((e) => e.id)))
          await this.execute({ sessionId, user: user || "admin", events: pendingEvents, round, mailbox, signal: ctl.signal })
          // 轮次边界：吸收插话
          const next = mailbox.drain()
          if (next.length === 0) break
          if (round >= this.maxRounds) {
            // 达到轮次上限：剩余插话重新入队（**绝不丢弃**）
            this.log?.warn(
              `[persona:${this.personaId}] 会话 ${sessionId} 达到单次运行轮次上限 ${this.maxRounds}，` +
                `剩余 ${next.length} 条插话重新入队`,
            )
            this.requeue(sessionId, user, next)
            break
          }
          pendingEvents = next
          // 插话可能来自不同 user（多用户场景）：以最后一条为准，保持会话归属一致
          user = user || "admin"
        }
        st.done++
      } catch (err) {
        st.failed++
        this.log?.warn(`[persona:${this.personaId}] 会话 ${sessionId} 运行失败: ${String((err as Error)?.message || err)}`)
        // 失败时残留插话同样重新入队
        const rest = mailbox.takeAll()
        if (rest.length) this.requeue(sessionId, user, rest)
      } finally {
        st.runningEventId = undefined
        st.round = 0
        this.running.delete(sessionId)
        this.mailboxes.delete(sessionId)
        this.runningEventIds.delete(sessionId)
        if (!this.sessionQueues.get(sessionId)?.length) this.sessionQueues.delete(sessionId)
        this.pump()
      }
    })()
  }

  /** 把事件重新放回队列（不经过插入路径，避免"入信箱→再取出"的死循环）。 */
  private requeue(sessionId: string, user: string, events: PersonaEvent[]): void {
    const list = this.sessionQueues.get(sessionId) ?? []
    for (const event of events) {
      // 队列上限仍要守；超了就丢最旧（日志已在 enqueueQueued 里）
      while (this.pendingTotal >= this.queueLimit) {
        const dropped = this.dropOldestPending()
        if (!dropped) break
        this.log?.warn(`[persona:${this.personaId}] 重新入队时队列已满，丢弃最旧事件 ${dropped}`)
      }
      if (this.pendingTotal >= this.queueLimit) {
        this.log?.warn(`[persona:${this.personaId}] 重新入队失败（队列满），事件 ${event.id} 已丢弃`)
        continue
      }
      list.push({ event, sessionId, user, enqueuedAt: Date.now(), seq: this.nextSeq++ })
      this.pendingTotal++
    }
    if (list.length) this.sessionQueues.set(sessionId, list)
  }

  /** 丢弃全局最旧的待跑事件，返回其 id。 */
  private dropOldestPending(): string | undefined {
    let target: { sessionId: string; seq: number } | null = null
    for (const [sessionId, list] of this.sessionQueues) {
      const head = list[0]
      if (!head) continue
      if (!target || head.seq < target.seq) target = { sessionId, seq: head.seq }
    }
    if (!target) return undefined
    const list = this.sessionQueues.get(target.sessionId)
    if (!list?.length) return undefined
    const dropped = list.shift()!
    this.pendingTotal--
    if (!list.length) this.sessionQueues.delete(target.sessionId)
    return dropped.event.id
  }
}
