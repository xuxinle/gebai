/**
 * 拟人运行时：统一事件驱动入口（**角色 → 多 driver → 多 worker**）。
 *
 * ## 架构
 *
 * ```
 * 事件源 ──广播──▶ 角色（订阅匹配）
 *                    │
 *                    ├─ driver「alerts」 ──派活──▶ worker…（跑完回报给该 driver）
 *                    ├─ driver「support」──派活──▶ worker…
 *                    └─ driver「report」（可运行时新增）
 * ```
 *
 * ## 三个关键语义
 *
 * ### 1. 两级订阅
 * 事件先匹配角色（`subscribers`），再在角色内匹配 driver（`drivers[].subscriptions`）。
 * 只有**关注该类型的 driver** 才收到——否则"盯告警"的分身会被支持群消息淹没。
 *
 * ### 2. 幂等按「事件 × driver」记账
 * 同一事件命中同一角色的两个 driver 是**两次不同的处理**，不是重复。
 * 按事件 id 或按 personaId 去重都会吞掉其中一路。
 *
 * ### 3. driver 可运行时创建（见 `DriverRegistry`）
 * 新 driver 立即参与订阅匹配——下一个符合它订阅的事件就会投给它。
 */

import { EventDedupe } from "./dedupe"
import type { PersonaDispatchResult, PersonaEvent } from "./event"
import type { DriverDef, PersonaDelivery, PersonaLogger, PersonaLookup, PersonaRunner, SessionResolver } from "./ports"
import { PersonaScheduler, type PersonaSchedulerState } from "./scheduler"

export interface PersonaRuntimeOptions {
  lookup: PersonaLookup
  runner: PersonaRunner
  sessions: SessionResolver
  /** 全局幂等环容量（默认 2048；按「事件×driver」计条目）。 */
  dedupeCapacity?: number
  /** 每 driver 待跑队列上限（默认 64）。 */
  queueLimit?: number
  /** 单次运行最大轮次（默认 8；达上限时剩余事件重新入队）。 */
  maxRounds?: number
  log?: PersonaLogger
  now?: () => number
  /** 广播并发度（同时向多少个 driver 投递，默认 6）。 */
  fanoutConcurrency?: number
}

/** 单 driver 运行时状态。 */
export interface DriverRuntimeState extends PersonaSchedulerState {
  personaId: string
  driverId: string
  accepted: number
  deduped: number
  overflowed: number
  failed: number
}

const DEFAULT_DEDUPE_CAPACITY = 2048
const DEFAULT_FANOUT_CONCURRENCY = 6

/** driver 的全局键（`personaId/driverId`）——调度器与幂等记账都按它区分。 */
export function driverKey(personaId: string, driverId: string): string {
  return `${personaId}/${driverId}`
}

export class PersonaRuntime {
  private readonly dedupe: EventDedupe
  /** driverKey → 调度器 */
  private readonly schedulers = new Map<string, PersonaScheduler>()
  private readonly counters = new Map<string, { accepted: number; deduped: number; overflowed: number; failed: number }>()
  /** driverKey → 会话解析结果（Promise 缓存：并发也只解析一次）。 */
  private readonly sessionCache = new Map<string, Promise<{ sessionId: string | null; error?: string }>>()
  /**
   * driverKey → 已解析到的 sessionId。
   *
   * 为何要单独存：调度器的表按 **sessionId** 建（`Map<sessionId, AbortController>`），
   * 而对外接口按 `(personaId, driverId)` 说话。少了这张映射，取消接口就无从把
   * driver 翻译成会话，只能永远查不到（形同死代码）。
   */
  private readonly driverSessions = new Map<string, string>()
  private readonly log?: PersonaLogger
  private readonly now: () => number
  private readonly fanoutConcurrency: number
  private started = false

  constructor(private readonly opts: PersonaRuntimeOptions) {
    this.dedupe = new EventDedupe(opts.dedupeCapacity ?? DEFAULT_DEDUPE_CAPACITY)
    this.log = opts.log
    this.now = opts.now ?? Date.now
    this.fanoutConcurrency = Math.max(1, opts.fanoutConcurrency ?? DEFAULT_FANOUT_CONCURRENCY)
  }

  start(): void {
    this.started = true
    for (const s of this.schedulers.values()) s.start()
  }

  stop(): void {
    this.started = false
    for (const s of this.schedulers.values()) s.stop()
  }

  isStarted(): boolean {
    return this.started
  }

  /** 统一事件入口：广播给所有关注该事件的 driver（跨角色、跨 driver）。 */
  async dispatch(event: PersonaEvent): Promise<PersonaDispatchResult> {
    if (!this.started) return { status: "unavailable", reason: "人格运行时未启动" }
    const targets = this.resolveTargets(event)
    if (targets.length === 0) return { status: "unrouted" }
    const deliveries = await this.mapLimit(targets, this.fanoutConcurrency, (t) => this.deliverTo(event, t))
    return this.summarize(event, deliveries)
  }

  /**
   * 批量投递：同一 driver 的多条事件**合并进同一轮**。
   *
   * 连着三条群消息逐条投 = 三次引擎 run（各带全套提示词与上下文）；
   * 合并成一轮只需一次。这也是 driver 能判断"这是同一件事的三个侧面"的前提。
   */
  async dispatchBatch(events: PersonaEvent[]): Promise<PersonaDispatchResult> {
    if (events.length === 0) return { status: "unrouted" }
    if (events.length === 1) return this.dispatch(events[0])
    if (!this.started) return { status: "unavailable", reason: "人格运行时未启动" }

    // 聚到 driver 维度（同一事件可以同时命中同角色的多个 driver）
    // 记录每个 (event, driver) 的幂等键：入队失败时要逐条撤回（见下方 deliverTo 同一条纪律）。
    const perDriver = new Map<string, { target: Target; events: PersonaEvent[]; dedupeIds: string[] }>()
    for (const event of events) {
      for (const t of this.resolveTargets(event)) {
        const key = driverKey(t.personaId, t.driver.id)
        const dedupeId = `${event.id}\u0000${key}`
        if (!this.dedupe.admit(dedupeId)) {
          this.bump(key, "deduped")
          continue
        }
        const entry = perDriver.get(key) ?? { target: t, events: [], dedupeIds: [] }
        entry.events.push({ ...event, personaId: t.personaId })
        entry.dedupeIds.push(dedupeId)
        perDriver.set(key, entry)
      }
    }
    if (perDriver.size === 0) return { status: "duplicate", reason: "全部事件已被订阅者处理过" }

    const deliveries: PersonaDelivery[] = []
    for (const [key, entry] of perDriver) {
      const d = await this.enqueueRound(key, entry.target, entry.events)
      // 整批同轮入队：一个没接管就全批撤回（否则下次重投只回来一半事件，上下文不完整）
      if (d.status !== "accepted") for (const id of entry.dedupeIds) this.dedupe.revoke(id)
      deliveries.push(d)
    }
    return this.summarize(events[0], deliveries)
  }

  /** worker 回报：包装成事件注入指定 driver（见 `workerReportEvent`）。 */
  async reportWorkerDone(event: PersonaEvent): Promise<PersonaDispatchResult> {
    return this.dispatch(event)
  }

  /** 某 driver 的状态。 */
  state(personaId: string, driverId: string): DriverRuntimeState | null {
    const key = driverKey(personaId, driverId)
    const s = this.schedulers.get(key)
    if (!s) return null
    const c = this.counters.get(key) ?? { accepted: 0, deduped: 0, overflowed: 0, failed: 0 }
    return { ...s.state(), personaId, driverId, ...c }
  }

  /** 某角色下全部 driver 的状态。 */
  personaStates(personaId: string): DriverRuntimeState[] {
    const out: DriverRuntimeState[] = []
    for (const d of this.opts.lookup.drivers(personaId)) {
      const st = this.state(personaId, d.id)
      if (st) out.push(st)
    }
    return out
  }

  /** 全部 driver 状态。 */
  allStates(): DriverRuntimeState[] {
    const out: DriverRuntimeState[] = []
    for (const [key, s] of this.schedulers) {
      const [personaId, driverId] = key.split("/")
      const c = this.counters.get(key) ?? { accepted: 0, deduped: 0, overflowed: 0, failed: 0 }
      out.push({ ...s.state(), personaId, driverId, ...c })
    }
    return out
  }

  /**
   * 已解析的 `(人格, 分身) → 常驻会话` 绑定（诊断/运行态快照用）。
   *
   * 为何要暴露它：宿主侧快照要回答的是「**运行时实际投递到了哪个会话**」——
   * 那是本类内部的 `driverSessions`（调度器把 driver 翻译成会话时写下的）。
   * 没有这个方法时，快照（`persona-status.ts`）只能去调 `SessionResolver.bindings()`，
   * 而那是**另一个对象**、且不一定有该方法 → 快照里 `sessionId` 永远是 null：
   * 分身明明正在跑，面板却说「它还没接过事件」，把「在干活」误报成「没通电」——
   * 恰恰是这套视图最该避免的那类误判。
   */
  bindings(): Array<{ personaId: string; driverId: string; sessionId: string }> {
    return [...this.driverSessions.entries()].map(([key, sessionId]) => {
      const [personaId, driverId] = key.split("/")
      return { personaId: personaId!, driverId: driverId!, sessionId }
    })
  }

  /**
   * 取消某 driver 下**正在跑**的那一轮。
   *
   * 返回是否确有在跑。语义：长任务卡住（等审批/等接口/死循环）时的唯一出口——
   * 与 `cancelWaiting`（只清排队）、`stop()`（整角色停）都不同。
   */
  cancelRunning(personaId: string, driverId: string): boolean {
    const key = driverKey(personaId, driverId)
    const scheduler = this.schedulers.get(key)
    if (!scheduler) return false
    // 调度器的表按 sessionId 建：必须把 driver 先翻译成它的常驻会话。
    const sessionId = this.driverSessions.get(key)
    if (!sessionId) return false
    return scheduler.cancelRunning(sessionId)
  }

  /** 取消某 driver 下**待跑**的事件（不影响正在跑的那一轮）。 */
  cancelWaiting(personaId: string, driverId: string): number {
    const key = driverKey(personaId, driverId)
    const scheduler = this.schedulers.get(key)
    if (!scheduler) return 0
    const sessionId = this.driverSessions.get(key)
    if (!sessionId) return 0
    return scheduler.cancelWaiting(sessionId)
  }

  /** 角色/驱动配置热加载后同步额度。 */
  syncConcurrency(personaId: string): void {
    for (const d of this.opts.lookup.drivers(personaId)) {
      const s = this.schedulers.get(driverKey(personaId, d.id))
      if (s) s.setMaxConcurrent(d.maxConcurrent ?? this.opts.lookup.maxConcurrent(personaId))
    }
  }

  /**
   * driver 上线/下线时同步调度器（动态创建后调用）。
   *
   * 上线：注册调度器（下次事件即投）
   * 下线：取消在跑、清空队列、摘除调度器
   */
  syncDriver(personaId: string, driverId: string, action: "online" | "offline"): void {
    const key = driverKey(personaId, driverId)
    if (action === "offline") {
      const s = this.schedulers.get(key)
      if (s) s.stop()
      this.schedulers.delete(key)
      this.counters.delete(key)
      this.sessionCache.delete(key)
      this.driverSessions.delete(key)
      return
    }
    // 上线：确保调度器存在（schedulerFor 惰性创建即可，这里显式触发一次）
    const def = this.opts.lookup.drivers(personaId).find((d) => d.id === driverId)
    if (def) this.schedulerFor(personaId, def)
  }

  // ── 内部 ──

  /** 单条投递目标：角色 + 命中的 driver。 */
  private resolveTargets(event: PersonaEvent): Target[] {
    const subs = this.opts.lookup.subscribers(event)
    const out: Target[] = []
    for (const s of subs) {
      // 定向投递：事件显式指定 personaId 时只投该角色
      if (event.personaId && event.personaId !== s.personaId) continue
      const identity = this.opts.lookup.get(s.personaId)
      if (!identity) {
        this.log?.warn(`[persona] 事件 ${event.id} 命中不存在的角色 ${s.personaId}`)
        continue
      }
      const all = new Map(this.opts.lookup.drivers(s.personaId).map((d) => [d.id, d]))
      for (const driverId of s.driverIds) {
        // 定向 driver：事件显式指定 driverId 时只投该 driver
        // （worker 回报靠它回到派它的那个 driver，而不惊动同角色的其它分身）
        if (event.driverId && event.driverId !== driverId) continue
        const driver = all.get(driverId)
        if (!driver) {
          this.log?.warn(`[persona:${s.personaId}] 事件 ${event.id} 命中不存在的 driver ${driverId}`)
          continue
        }
        out.push({ personaId: s.personaId, identity, driver, user: s.user })
      }
    }
    return out
  }

  /**
   * 投递给单个 driver。
   *
   * **记账时机**：先 admit（幂等）再入队，但入队没成功时必须 `revoke` 撤回。
   * 若只 admit 不撤回，一次瞬时故障（存储不可达、队列已满）就把该 id 永久占位，
   * 源按规范重投同一事件会被判 duplicate 静默丢弃——违反「FIFO 且不丢」。
   */
  private async deliverTo(event: PersonaEvent, t: Target): Promise<PersonaDelivery> {
    const key = driverKey(t.personaId, t.driver.id)
    const dedupeId = `${event.id}\u0000${key}`
    if (!this.dedupe.admit(dedupeId)) {
      this.bump(key, "deduped")
      return { personaId: t.personaId, driverId: t.driver.id, status: "duplicate", reason: `driver ${key} 已处理过事件 ${event.id}` }
    }
    const delivery = await this.enqueueRound(key, t, [{ ...event, personaId: t.personaId }])
    // 只有「确实已接管」才算处理过；其余（队列满 / 会话解析失败 / driver 已停）一律撤回，
    // 让源的重投能真正被接住。
    if (delivery.status !== "accepted") this.dedupe.revoke(dedupeId)
    return delivery
  }

  /** 把一批事件投进某个 driver（同一轮）。 */
  private async enqueueRound(key: string, t: Target, events: PersonaEvent[]): Promise<PersonaDelivery> {
    const { personaId, driver } = t
    const scheduler = this.schedulerFor(personaId, driver)
    if (!scheduler) {
      return { personaId, driverId: driver.id, status: "unavailable", reason: "driver 已停止" }
    }
    const resolved = await this.driverSession(personaId, driver.id, t.user, events[0])
    if (!resolved.sessionId) {
      this.bump(key, "failed")
      // 如实转出根因：只说“会话解析失败”会让排查无从下手
      // （底层可能是磁盘满/权限/存储不可达，处理方式完全不同）
      return {
        personaId,
        driverId: driver.id,
        status: "unavailable",
        reason: `driver 会话解析失败${resolved.error ? `：${resolved.error}` : ""}`,
      }
    }
    const sessionId = resolved.sessionId
    const aheads = scheduler.enqueueMany(
      events.map((e) => ({ event: e, sessionId, user: t.user, threadKey: driver.id })),
    )
    if (aheads.some((a) => a < 0)) {
      this.bump(key, "overflowed")
      return { personaId, driverId: driver.id, status: "overflow", sessionId, reason: "待处理队列已满" }
    }
    this.bump(key, "accepted")
    const ahead = aheads.length ? Math.max(...aheads) : 0
    return { personaId, driverId: driver.id, status: "accepted", sessionId, ...(ahead > 0 ? { queued: ahead } : {}) }
  }

  private async driverSession(
    personaId: string,
    driverId: string,
    user: string,
    event: PersonaEvent,
  ): Promise<{ sessionId: string | null; error?: string }> {
    // 缓存：同一 driver 的会话是稳定的（resolveDriver 幂等），没必要每条事件都问一次。
    // 缓存住的是 **Promise**（不是结果）：并发投递时也只会真正解析一次。
    const key = driverKey(personaId, driverId)
    const cached = this.sessionCache.get(key)
    if (cached) return cached
    const p = (async () => {
      try {
        const r = await this.opts.sessions.resolveDriver({ personaId, driverId, user, event })
        // 记下 driverKey → sessionId：取消接口靠它把 driver 翻译成调度器认的会话
        this.driverSessions.set(key, r.sessionId)
        return { sessionId: r.sessionId }
      } catch (err) {
        const error = String((err as Error)?.message || err)
        this.log?.warn(`[persona:${personaId}/${driverId}] driver 会话解析失败: ${error}`)
        // 失败不入缓存（下次重试；否则一次瞬时故障会永久失效）
        this.sessionCache.delete(key)
        return { sessionId: null, error }
      }
    })()
    this.sessionCache.set(key, p)
    return p
  }

  private summarize(event: PersonaEvent, deliveries: PersonaDelivery[]): PersonaDispatchResult {
    const accepted = deliveries.filter((d) => d.status === "accepted")
    const first = accepted[0] ?? deliveries[0]
    const status =
      accepted.length > 0
        ? "accepted"
        : deliveries.every((d) => d.status === "duplicate")
          ? "duplicate"
          : deliveries.every((d) => d.status === "duplicate" || d.status === "unavailable")
            ? "unavailable"
            : "overflow"
    return {
      status,
      ...(first?.personaId ? { personaId: first.personaId } : {}),
      ...(first?.driverId ? { driverId: first.driverId } : {}),
      ...(first?.sessionId ? { sessionId: first.sessionId } : {}),
      ...(first?.queued !== undefined ? { queued: first.queued } : {}),
      ...(status === "duplicate" ? { reason: `事件 ${event.id} 已被全部订阅者处理过` } : {}),
      deliveries,
    }
  }

  /** 取（或建）driver 调度器。 */
  private schedulerFor(personaId: string, driver: DriverDef): PersonaScheduler | null {
    const key = driverKey(personaId, driver.id)
    const existing = this.schedulers.get(key)
    if (existing) return existing
    const maxConcurrent = driver.maxConcurrent ?? this.opts.lookup.maxConcurrent(personaId)
    const scheduler = new PersonaScheduler({
      // personaId 用全局 key，保证日志/状态里能区分同一角色的不同 driver
      personaId: key,
      maxConcurrent,
      ...(this.opts.queueLimit !== undefined ? { queueLimit: this.opts.queueLimit } : {}),
      ...(this.opts.maxRounds !== undefined ? { maxRounds: this.opts.maxRounds } : {}),
      ...(this.log ? { log: this.log } : {}),
      execute: async ({ events, sessionId, user, signal, round, mailbox }) => {
        const identity = this.opts.lookup.get(personaId)
        if (!identity) throw new Error(`角色 ${personaId} 已不存在`)
        const first = events[0]
        if (!first) return
        const attachments = events.find((e) => e.attachments?.length)?.attachments
        // 兄弟分身（除自己）：runner 据此告知「同一个我」还有哪些分工在跑。
        // 为何在快照时算而不是在 runner 里取：runner 只认得自己这一位，拿不到全角色视图。
        const siblings = this.opts.lookup.drivers(personaId).filter((d) => d.id !== driver.id)
        // 该人格声明的子代理工具包：**同一个来源**同时驱动"会话装工具"与"提示词写纪律"。
        // 两半必须同源（见 `DriverTarget.agents` 的注释）：一分离就会出现
        // “装了工具不告诉它”或“提示它有却没装”两种错配，而且都不报错。
        const agents = this.opts.lookup.agents?.(personaId) ?? []
        await this.opts.runner.run({
          personaId,
          identity,
          driver,
          events,
          event: first,
          round,
          sessionId,
          user,
          signal,
          mailbox,
          ...(siblings.length ? { siblings } : {}),
          ...(agents.length ? { agents } : {}),
          ...(attachments ? { attachments } : {}),
        })
      },
    })
    if (this.started) scheduler.start()
    this.schedulers.set(key, scheduler)
    return scheduler
  }

  private async mapLimit<T, R>(items: T[], limit: number, fn: (item: T) => Promise<R>): Promise<R[]> {
    if (items.length === 0) return []
    const out = new Array<R>(items.length)
    let next = 0
    const workers = Array.from({ length: Math.min(limit, items.length) }, async () => {
      for (;;) {
        const i = next++
        if (i >= items.length) return
        out[i] = await fn(items[i])
      }
    })
    await Promise.all(workers)
    return out
  }

  private bump(key: string, k: "accepted" | "deduped" | "overflowed" | "failed"): void {
    const c = this.counters.get(key) ?? { accepted: 0, deduped: 0, overflowed: 0, failed: 0 }
    c[k]++
    this.counters.set(key, c)
    void this.now
  }
}

/** 投递目标（内部）。 */
interface Target {
  personaId: string
  identity: import("./ports").PersonaIdentity
  driver: DriverDef
  user: string
}
