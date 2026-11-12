/**
 * 两层队列 + 轮次插入：driver 级并发额度 + 会话级串行 + **运行中插入**。
 *
 * ## 三个不变量
 *
 * 1. **同一会话同时最多一个轮次在跑**（引擎硬约束：同 session 并发 run 直接抛错）。
 * 2. **同时运行的会话数 ≤ `maxConcurrent`**（资源约束）。
 *    注：本调度器实例是 **per-driver** 的，而一个 driver 只对应一个常驻会话——额度大于 1
 *    在日常路由下不会生效（会话级串行先把它锁死）。
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
import { toSessionLostError } from "./session-lost"
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
  /** 放回重试次数（launch 失败后按原 seq 放回队首时递增；达上限放弃，防毒事件死循环）。 */
  attempts?: number
}

/** 单次运行里允许的会话换锚次数：换一次就该好；超过说明是“重建了但仍然报不存在”的循环，按真失败处理。 */
const MAX_REANCHORS = 3

/** 单个事件（含首次）的执行尝试上限：确定性失败到次数后放弃，防 restore→pump 紧循环。 */
const MAX_LAUNCH_ATTEMPTS = 3

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
  /**
   * 单会话插话信箱上限（默认与 `queueLimit` 同值，即 64）。
   *
   * 满了之后新插话**降级到队列**（而不是丢弃）：插话是尽力而为的高优先路径，
   * 可靠投递仍由队列保证。
   */
  mailboxCapacity?: number
  /** 实际执行一轮（由 runtime 提供：整编事件 + 调引擎跑）。 */
  execute: (input: RoundInput) => Promise<void>
  /**
   * 单个运行的最大轮次数（防插话洪水导致会话永不结束）。默认 8。
   *
   * 达到上限时把剩余插话重新入队（不丢），本次运行收尾。
   */
  maxRounds?: number
  /**
   * 该会话是否被**外部任务**占用（引擎对「有任务在跑」的权威回答）。
   *
   * driver 的常驻会话在 UI 里可见可打开——用户手动对其发起任务期间，若照常
   * launch，engine.run 会抛「已有任务在运行」，而那时首批事件已被 shift 出队。
   * 预检为 true 时不 launch、不 shift，事件留在队列等下次 pump（占用结束后
   * 任何新事件入队或其它运行收尾都会触发）。
   */
  isSessionBusy?: (sessionId: string) => boolean
  /**
   * 会话**已不在存储中**时的换锚：把死 `sessionId` 换成当前有效的那个。
   *
   * 为什么需要它：调度器的桶按 `sessionId` 建，而入队时那个 id 来自上一层的解析
   * 缓存（可能已经在 TTL 窗口内过期于真实存储——会话被删/被 GC 归档/chat.json 丢失）。
   * 没有换锚时，引擎抛「会话不存在」只会落进通用 catch：事件按**原 id** 放回队首、
   * 烧掉重试次数，三次之后整批丢弃——缓存刷新前的每一条事件都在同一个死 id 上死一遍。
   *
   * 返回新 sessionId 时，本会话的在跑批次与待跑队列里的**事件**（`QueueItem.event`，
   * 不是 sessionId 字段）由调度器改投新 id；返回 `null` 表示换不到（宿主持久化层也
   * 认不出这个 driver → 真错误，走原失败路径）。
   *
   * 不传 = 无自愈（旧行为）。
   */
  resolveSession?: (deadSessionId: string, event: PersonaEvent, user: string) => Promise<string | null>
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
  private readonly mailboxCapacity: number
  private readonly execute: PersonaSchedulerOptions["execute"]
  private readonly isSessionBusy?: (sessionId: string) => boolean
  private readonly resolveSession?: (deadSessionId: string, event: PersonaEvent, user: string) => Promise<string | null>
  private readonly log?: PersonaLogger

  /** sessionId → 该会话的待跑队列。 */
  private sessionQueues = new Map<string, QueueItem[]>()
  /** sessionId → threadKey（可观测用）。 */
  private threadKeys = new Map<string, string>()
  /** sessionId → 正在跑的运行的取消器。 */
  private running = new Map<string, AbortController>()
  /** sessionId → 运行世代号（stop→start 复用实例时，旧 finally 不得误删新运行的表项）。 */
  private generations = new Map<string, number>()
  private nextGeneration = 1
  /** sessionId → 运行中的插话信箱。 */
  private mailboxes = new Map<string, InterjectionMailbox>()
  /** sessionId → 正在跑的事件 id 集合（同会话去重必须算上它们——已不在队列里）。 */
  private runningEventIds = new Map<string, Set<string>>()
  /** sessionId → 统计。 */
  private stats = new Map<string, { done: number; failed: number; round: number; interjections: number; runningEventId?: string }>()
  /** 刚被 cancelRunning/stop 打断、尚未走完 finally-pump 的会话（见 pump 的 cancelledNow）。 */
  private pendingCancelSkip = new Set<string>()
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
    this.mailboxCapacity = opts.mailboxCapacity ?? this.queueLimit
    this.maxRounds = Math.max(1, opts.maxRounds ?? DEFAULT_MAX_ROUNDS)
    this.execute = opts.execute
    this.isSessionBusy = opts.isSessionBusy
    this.resolveSession = opts.resolveSession
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
        // 信箱有界：满了就降级到队列（事件仍会被处理，只是晚一轮——绝不静默丢）
        if (mailbox.push(item.event)) {
          const st = this.stats.get(item.sessionId)
          if (st) st.interjections++
          out.push(0)
          continue
        }
        this.log?.warn(
          `[persona:${this.personaId}] 会话 ${item.sessionId} 插话信箱已满，事件 ${item.event.id} 降级到队列`,
        )
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

  /** 调整并发额度（driver 配置热加载）。 */
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
    // 记入本轮取消集：被取消运行的 requeue 事件不在同一次 pump 里捞起（见 pump 注释）。
    // 用可空 Set 而非实例字段：cancelRunning 与 finally-pump 之间没有 await，
    // 同步标记同步消费即可；实例字段反而要考虑清理时机。
    this.pendingCancelSkip.add(sessionId)
    // 微任务尾满空：finally 的 pump 发生在本轮事件循环尾，届时标记已就位；
    // pump 结束后清掉，不影响下一次自然触发的 pump。
    void Promise.resolve().then(() => {
      queueMicrotask(() => this.pendingCancelSkip.delete(sessionId))
    })
    return true
  }

  /** 停止：取消所有在跑，清空队列。 */
  stop(): void {
    this.stopped = true
    for (const ctl of this.running.values()) ctl.abort()
    this.running.clear()
    this.generations.clear()
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
    /** 本轮 pump 已确认「被外部占用」的会话（只诊断一次，避免重复扫与重复日志）。 */
    const externallyBusy = new Set<string>()
    /** 本轮 pump 内刚被 cancelRunning/stop 打断的会话：其 requeue 的事件是「人叫停的」，
     * 不能在同一次 pump 里立刻捞起重跑——否则取消形同虚设（取消了又马上重跑）。
     * 留在队列里，等下一个自然触发点（新事件入队/其它运行收尾/下一次 pump）。 */
    const cancelledNow = this.pendingCancelSkip
    while (this.running.size < this.maxConcurrent) {
      const next = this.pickNext(externallyBusy, cancelledNow)
      if (!next) break
      // 外部占用预检：不 launch、不 shift 首批，事件留在队列。没有这一步，
      // 冲突只能靠 engine.run 抛错暴露——而那时首批已被 shift，事件就丢了。
      if (this.isSessionBusy?.(next.sessionId)) {
        externallyBusy.add(next.sessionId)
        this.log?.info(`[persona:${this.personaId}] 会话 ${next.sessionId} 被外部任务占用，事件暂留队列`)
        continue
      }
      this.launch(next)
    }
  }

  /** 选出下一个可跑的（未被占用 + 队首有效 + 入队最早）。 */
  private pickNext(skip?: Set<string>, skip2?: Set<string>): QueueItem | null {
    let best: QueueItem | null = null
    for (const [sessionId, list] of this.sessionQueues) {
      if (this.running.has(sessionId)) continue
      if (skip?.has(sessionId)) continue
      if (skip2?.has(sessionId)) continue
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
    const mailbox = new InterjectionMailbox(this.mailboxCapacity)
    // 世代号：stop→start 复用同一实例时，旧运行的 finally 可能在新运行建立之后才
    // 执行；不比对就 delete 会把**新运行**的表项误删（isRunning 失真 → 同会话双
    // launch → 引擎抛「已有任务」→ 事件丢失）。
    const generation = ++this.nextGeneration
    this.running.set(sessionId, ctl)
    this.mailboxes.set(sessionId, mailbox)
    this.generations.set(sessionId, generation)
    this.runningEventIds.set(sessionId, new Set(batch.map((b) => b.event.id)))
    const st = this.stats.get(sessionId) ?? { done: 0, failed: 0, round: 0, interjections: 0 }
    st.runningEventId = batch[batch.length - 1]?.event.id
    st.round = 0
    st.interjections = 0
    this.stats.set(sessionId, st)

    void (async () => {
      let round = 0
      /**
       * 本运行实际的会话 id。
       *
       * 底座是 `first.sessionId`；会话丢失并被 `resolveSession` 换锚后改指新 id——
       * 后续轮次、轮末 requeue、异常回填都用它，不再用入队时的快照。
       */
      let runSessionId = sessionId
      /** 本运行已发生的换锚次数（兑底上限，防“重建了但依旧报不存在”的死循环）。 */
      let reanchors = 0
      let pendingEvents = batch.map((b) => b.event)
      let user = batch[0]?.user ?? ""
      try {
        for (;;) {
          if (ctl.signal.aborted) {
            // 取消路径与异常路径同款纪律：已 drain 未执行的 + 信箱残留都必须回到队列。
            // 它们没被任何一轮处理过，而 runtime 层的幂等位已 admit 占位——丢了这里，
            // 源按规范重投还会被判 duplicate 静默丢弃（双保险全灭）。
            const rest = [...pendingEvents, ...mailbox.takeAll()]
            if (rest.length) this.requeue(runSessionId, user, rest)
            break
          }
          if (pendingEvents.length === 0) break
          round++
          st.round = round
          st.runningEventId = pendingEvents[pendingEvents.length - 1]?.id
          this.runningEventIds.set(runSessionId, new Set(pendingEvents.map((e) => e.id)))
          try {
            await this.execute({ sessionId: runSessionId, user: user || "admin", events: pendingEvents, round, mailbox, signal: ctl.signal })
          } catch (err) {
            // 会话丢失 → 换锚重跑本轮（不消耗重试、不计失败）：这是缓存陈旧，
            // 不是毒事件。换不到锚时原错上抛，走通用失败路径。
            const lost = toSessionLostError(runSessionId, err)
            if (!lost || !this.resolveSession) throw err
            if (reanchors >= MAX_REANCHORS) {
              this.log?.warn(
                `[persona:${this.personaId}] 会话 ${runSessionId} 已换锚 ${reanchors} 次仍报不存在（疑似存储异常）——本次运行按失败处理`,
              )
              throw err
            }
            const fresh = await this.reanchor(runSessionId, pendingEvents[0]!, user, ctl.signal)
            if (!fresh) throw err
            reanchors++
            runSessionId = fresh
            round-- // 本轮未做成：换锚后同样条件重跑，轮次语义不因自愈而提前耗尽
            // 换锚后重新指向新会话的观测表（旧 id 的表项已在 reanchor 里清掉）
            st.runningEventId = pendingEvents[pendingEvents.length - 1]?.id
            this.stats.set(fresh, st)
            continue
          }
          // 轮次边界：吸收插话
          const next = mailbox.drain()
          if (next.length === 0) break
          if (round >= this.maxRounds) {
            // 达到轮次上限：剩余插话重新入队（**绝不丢弃**）
            this.log?.warn(
              `[persona:${this.personaId}] 会话 ${runSessionId} 达到单次运行轮次上限 ${this.maxRounds}，` +
                `剩余 ${next.length} 条插话重新入队`,
            )
            this.requeue(runSessionId, user, next)
            break
          }
          pendingEvents = next
          // 插话可能来自不同 user（多用户场景）：以最后一条为准，保持会话归属一致
          user = user || "admin"
        }
        st.done++
      } catch (err) {
        st.failed++
        this.log?.warn(`[persona:${this.personaId}] 会话 ${runSessionId} 运行失败: ${String((err as Error)?.message || err)}`)
        // 首批已被 shift 出队：引擎拒绝（如会话被外部任务占用的竞态）或执行异常时
        // 不能直接丢——按原 seq 放回队首重试（毒事件由尝试上限兑底，防死循环）。
        this.restoreBatch(batch, runSessionId)
        // 失败时残留插话同样重新入队
        const rest = mailbox.takeAll()
        if (rest.length) this.requeue(runSessionId, user, rest)
      } finally {
        // 世代比对：表里还是自己那一代才清理（否则会把新运行的表项误删，见 launch 头注释）
        if (this.generations.get(runSessionId) === generation) {
          this.generations.delete(runSessionId)
          this.running.delete(runSessionId)
          this.mailboxes.delete(runSessionId)
          this.runningEventIds.delete(runSessionId)
          st.runningEventId = undefined
          st.round = 0
          if (!this.sessionQueues.get(runSessionId)?.length) {
            this.sessionQueues.delete(runSessionId)
            // 观测表随会话终结清理：否则已死会话的 threadKeys/stats 永远增长
            // （state() 本就不展示完全空闲的会话，这里只影响下次的累计计数归零）
            this.threadKeys.delete(runSessionId)
            this.stats.delete(runSessionId)
          }
        }
        this.pump()
      }
    })()
  }

  /**
   * 把一次 launch 取走的首批按原 seq 放回队首（保命路径：引擎拒绝/执行异常时不丢事件）。
   *
   * 重试上限：毒事件（确定性失败）在尝试次数用尽后放弃并记日志——无上限的 restore
   * 会在 catch→finally pump→launch 之间形成紧循环。而真正的「会话被占用」冲突
   * 在下一次 pump 时会被 isSessionBusy 预检拦住（不烧尝试次数），空出后自然接起。
   */
  private restoreBatch(batch: QueueItem[], sessionId?: string): void {
    const bySession = new Map<string, QueueItem[]>()
    for (const it of batch) {
      const attempts = (it.attempts ?? 0) + 1
      if (attempts >= MAX_LAUNCH_ATTEMPTS) {
        this.log?.warn(
          `[persona:${this.personaId}] 事件 ${it.event.id} 连续失败 ${attempts} 次后放弃（已从队列移除；下次重投不受影响）`,
        )
        continue
      }
      // 桶键以调用方给定的为准（会话换锚后要回新桶，否则重试还打在死会话上）
      const key = sessionId ?? it.sessionId
      bySession.set(key, [...(bySession.get(key) ?? []), { ...it, sessionId: key, attempts }])
    }
    for (const [sessionId, items] of bySession) {
      while (this.pendingTotal + items.length > this.queueLimit) {
        const dropped = this.dropOldestPending()
        if (!dropped) break
        this.log?.warn(`[persona:${this.personaId}] 放回批次时队列已满，丢弃最旧事件 ${dropped}`)
      }
      if (this.pendingTotal + items.length > this.queueLimit) {
        for (const it of items) this.log?.warn(`[persona:${this.personaId}] 事件 ${it.event.id} 放回失败（队列容量不足），已丢弃`)
        continue
      }
      // 放在队首：原 seq 早于现存队列项，全局 FIFO 不受影响
      const existing = this.sessionQueues.get(sessionId) ?? []
      this.sessionQueues.set(sessionId, [...items, ...existing])
      this.pendingTotal += items.length
    }
  }

  /**
   * 会话丢失后的换锚：请宿主重新解析出有效会话 id，并把本运行/本队列的桶整体搬过去。
   *
   * 三件必须一起做（缺一件就会出现“换了 id 但事件还在旧桶里”的断层）：
   * 1. 调 `resolveSession` 拿新 id（宿主侧同时清自己的解析缓存，否则拿到的还是同一个死 id）
   * 2. 本会话在跑的表项（running/mailboxes/generations/runningEventIds/stats/threadKeys）搬到新 id
   * 3. **待跑队列**合并到新 id：`QueueItem.sessionId` 是桶键（launch 取的是 `event`），
   *    下一轮 pump 按桶键辨认会话——不合并，旧桶的事件会继续用死 id 重试。
   *
   * 返回 null = 宿主也换不到（driver 已下线/存储不可达），由调用方按真错误处理。
   */
  private async reanchor(deadSessionId: string, event: PersonaEvent, user: string, signal: AbortSignal): Promise<string | null> {
    if (!this.resolveSession) return null
    if (signal.aborted) return null
    let fresh: string | null = null
    try {
      fresh = await this.resolveSession(deadSessionId, event, user)
    } catch (err) {
      this.log?.warn(`[persona:${this.personaId}] 会话 ${deadSessionId} 换锚失败（解析报错）: ${String((err as Error)?.message || err)}`)
      return null
    }
    if (!fresh || fresh === deadSessionId) {
      this.log?.warn(`[persona:${this.personaId}] 会话 ${deadSessionId} 已丢失且无法换锚（宿主持久化层未给出新会话）`)
      return null
    }
    // ① 在跑的表项搬过去（cancelRunning / state 靠它们定位）
    const move = <T>(m: Map<string, T>) => {
      const v = m.get(deadSessionId)
      if (v === undefined) return
      m.delete(deadSessionId)
      m.set(fresh as string, v)
    }
    move(this.running)
    move(this.mailboxes)
    move(this.generations)
    move(this.runningEventIds)
    move(this.stats)
    move(this.threadKeys)
    // ② 待跑队列合并（只改桶键，不动事件与 seq——FIFO 不变）
    const dead = this.sessionQueues.get(deadSessionId)
    if (dead?.length) {
      this.sessionQueues.delete(deadSessionId)
      const merged = [...(this.sessionQueues.get(fresh) ?? []), ...dead.map((it) => ({ ...it, sessionId: fresh as string }))]
      merged.sort((a, b) => a.seq - b.seq)
      this.sessionQueues.set(fresh, merged)
    }
    this.log?.warn(
      `[persona:${this.personaId}] 会话 ${deadSessionId} 已不存在（删除/归档）→ 换锚到 ${fresh}，在途与待跑事件改投新会话`,
    )
    return fresh
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
