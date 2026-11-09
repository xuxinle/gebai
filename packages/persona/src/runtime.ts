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
import { chatSessionKeyOf } from "./env-config"
import { matchSessionMap } from "./session-map"
import type { PersonaDispatchResult, PersonaEvent } from "./event"
import type { DriverDef, PersonaDelivery, PersonaLogger, PersonaLookup, PersonaRunner, SessionResolver } from "./ports"
import { PersonaScheduler, type PersonaSchedulerState } from "./scheduler"

/**
 * 该事件是否只进上下文、不触发执行。
 *
 * 判定依据是事件源打上的 `refs.extra.contextOnly`（字符串 `"1"`）——为什么不用
 * 事件上的新字段：`refs.extra` 本来就是「渠道自定义引用，原样透传给模型」的口子，
 * 这类标记正好是那个性质；加新字段会让所有源、所有序列化路径都得改一遍。
 */
function isContextOnly(event: PersonaEvent): boolean {
  return event.context.refs?.extra?.contextOnly === "1"
}

export interface PersonaRuntimeOptions {
  lookup: PersonaLookup
  runner: PersonaRunner
  sessions: SessionResolver
  /**
   * 收到「用户亲手发言」时的回调（可选）。
   *
   * 用来驱动**让位窗口**：用户在一个群里当场说话了，人格在短时间内不该抢话。
   * 只对 `refs.extra.by === "human"`（事件源打的标）的事件触发——
   * 人格自己发出去的回灌已经在事件源被拦下，不会走到这里。
   */
  onHumanSpeech?: (event: PersonaEvent) => void
  /**
   * 用户亲手发言时是否**硬打断**正在跑的轮次（缺省 false）。
   *
   * 缺省不打断：用户插一句话不等于“你现在这个活别干了”——硬打断会丢掉
   * 已经算到一半的成果（包括已发出的消息）。默认只把它当**高优先级插话**
   * （下一轮边界吸收），这已足够“立即响应”。
   *
   * 确实需要“一说话就抢方向盘”的场景（客服坐席类）显式开启：
   * `GEBAI_PERSONA_HUMAN_INTERRUPT=1`。
   */
  interruptOnHumanSpeech?: boolean
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
  /**
   * 会话关联规则。
   *
   * **缺省 `driver`**：一个分身一个常驻会话，**不同群的消息都注入这个上下文**。
   * 会话面向的是上下文而不是群——把群当会话身份会把“一个人”拆成几份互不知情的助手
   * （A 群排查一小时，B 群问同一件事时又从零开始）。
   *
   * `chat` 只是**可选**的群间隔离模式，需要部署方显式要求。
   */
  sessionRouting?: "driver" | "chat"
  /**
   * 会话解析缓存的有效期（毫秒，缺省 30 分钟）。
   *
   * 为何必须有：缓存把 `(persona, driver, sessionKey) → sessionId` 永久记住后，
   * 会话一旦被用户删除 / 被 GC 归档，事件就会持续投给一个死 id——引擎抛「会话不存在」
   * → 调度器重试 3 次后丢弃。表面症状是「这个分身从此再也不回消息」，日志里只有一条 warn。
   * 过期后重新走解析（解析器自己会校验存活并重建）。
   */
  sessionCacheTtlMs?: number
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
const DEFAULT_SESSION_CACHE_TTL_MS = 30 * 60_000

/** driver 的全局键（`personaId/driverId`）——调度器与幂等记账都按它区分。 */
 export function driverKey(personaId: string, driverId: string): string {
 return `${personaId}/${driverId}`
 }

/** 默认会话键：未声明 sessionKeyOf / 返回空时使用（保持单会话行为不变）。 */
export const DEFAULT_SESSION_KEY = ""

/** 带会话键的缓存键：默认键退回二元键形状（与历史缓存键兼容，热升级后 memo 不丢）。 */
export function sessionDriverKey(personaId: string, driverId: string, sessionKey: string): string {
 return sessionKey === DEFAULT_SESSION_KEY ? driverKey(personaId, driverId) : `${personaId}/${driverId}#${sessionKey}`
}

/** 会话键清洗：非法字符压成下划线、限长 128（防路由函数返回怪形/超长串污染会话名与盘上键）。 */
export function sanitizeSessionKey(raw: string): string {
 const cleaned = raw.trim().replace(/[^A-Za-z0-9_.:-]/g, "_").slice(0, 128)
 return cleaned || DEFAULT_SESSION_KEY
}

/** 二元 driver 键（取消接口用：多会话键形态下仍按 driver 整体定位）。 */
function baseDriverKey(personaId: string, driverId: string): string {
 return driverKey(personaId, driverId)
}


export class PersonaRuntime {
  private readonly dedupe: EventDedupe
  /** driverKey → 调度器 */
  private readonly schedulers = new Map<string, PersonaScheduler>()
  private readonly counters = new Map<string, { accepted: number; deduped: number; overflowed: number; failed: number }>()
  /** driverKey → 会话解析结果（Promise 缓存：并发也只解析一次）。 */
  private readonly sessionCache = new Map<string, { at: number; value: Promise<{ sessionId: string | null; error?: string }> }>()  /** 会话解析缓存有效期（见 `sessionCacheTtlMs`）。 */
  private readonly sessionCacheTtlMs: number
  /**
   * driverKey → 已解析到的 sessionId。
   *
   * 为何要单独存：调度器的表按 **sessionId** 建（`Map<sessionId, AbortController>`），
   * 而对外接口按 `(personaId, driverId)` 说话。少了这张映射，取消接口就无从把
   * driver 翻译成会话，只能永远查不到（形同死代码）。
   */
  private readonly driverSessions = new Map<string, string>()
  private readonly log?: PersonaLogger
  private readonly fanoutConcurrency: number
  private readonly sessionRouting: "driver" | "chat"
  private started = false

  constructor(private readonly opts: PersonaRuntimeOptions) {
    this.dedupe = new EventDedupe(opts.dedupeCapacity ?? DEFAULT_DEDUPE_CAPACITY)
    this.log = opts.log
    this.fanoutConcurrency = Math.max(1, opts.fanoutConcurrency ?? DEFAULT_FANOUT_CONCURRENCY)
    this.sessionRouting = opts.sessionRouting ?? "driver"
    const ttl = opts.sessionCacheTtlMs
    this.sessionCacheTtlMs = typeof ttl === "number" && Number.isFinite(ttl) && ttl > 0 ? ttl : DEFAULT_SESSION_CACHE_TTL_MS
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
    // 先记一笔「用户亲手发言」：让位窗口（C）与优先级判断（B）都靠它。
    //
    // 放在路由**之前**：即使 unrouted（无 driver 关注），用户说过话这个事实
    // 仍然成立——人格可能正从别的分身往这个群发东西。
    this.opts.onHumanSpeech?.(event)
    const targets = this.resolveTargets(event)
    if (targets.length === 0) return { status: "unrouted" }
    // 只记上下文、不触发执行（`refs.extra.contextOnly`，由事件源按发送者判定后打标）。
    //
    // 为何在这分流而不在源里直接判：**路由仍要走完整**（哪个 driver 关注、归属哪个会话）——
    // 写入目标与执行目标必须是同一个会话，否则“它看过这条”与“它在哪想事”成了两个地方。
    if (isContextOnly(event)) {
      return this.appendContextOnly(event, targets)
    }
    const deliveries = await this.mapLimit(targets, this.fanoutConcurrency, (t) => this.deliverTo(event, t))
    return this.summarize(event, deliveries)
  }

  /**
   * 只写入上下文：给每个命中 driver 的会话落一条消息，**不起任何轮次**。
   *
   * 返回值里 status 仍用 `accepted`：从事件源视角看它确实被接管了（没有被丢弃），
   * 源不应重投。区分“记了”与“跑了”靠 reason 与计数（见下）。
   */
  private async appendContextOnly(event: PersonaEvent, targets: Target[]): Promise<PersonaDispatchResult> {
    const sessions = this.opts.sessions
    if (typeof sessions.appendOnly !== "function") {
      // 宿主未实现只记能力：**不能静默丢**（那就回到旧行为了）。如实报 unavailable，
      // 源会重投——总比“上下文里凭空少一条”好查。
      this.log?.warn(
        `[persona] 事件 ${event.id} 标记为只记上下文，但会话解析器未实现 appendOnly——本条未落盘` +
          `（宿主需实现该能力，或事件源不应打 contextOnly 标记）`,
      )
      return { status: "unavailable", reason: "会话解析器未实现 appendOnly（只记上下文能力缺失）" }
    }
    const deliveries: PersonaDelivery[] = []
    for (const t of targets) {
      const key = driverKey(t.personaId, t.driver.id)
      const dedupeId = `${event.id}\u0000${key}`
      if (!this.dedupe.admit(dedupeId)) {
        this.bump(key, "deduped")
        deliveries.push({
          personaId: t.personaId,
          driverId: t.driver.id,
          status: "duplicate",
          reason: `driver ${key} 已处理过事件 ${event.id}`,
        })
        continue
      }
      try {
        const sessionKey = this.sessionKeyFor(key, t.driver, event)
        const note = event.context.refs?.extra?.speakerNote
        const kind = (event.context.refs?.extra?.speakerKind ?? "self") as "self" | "owner" | "other"
        const r = await sessions.appendOnly({
          personaId: t.personaId,
          driverId: t.driver.id,
          user: t.user,
          event: { ...event, personaId: t.personaId },
          sessionKey,
          ...(note ? { note } : {}),
          kind,
          ...(event.context.refs?.extra?.contextOnlyReason ? { reason: event.context.refs.extra.contextOnlyReason } : {}),
        })
        deliveries.push({
          personaId: t.personaId,
          driverId: t.driver.id,
          status: "accepted",
          sessionId: r.sessionId,
          reason: "已记入上下文（未触发执行）",
        })
      } catch (err) {
        // 写失败必须撤回幂等位：否则源重投会被判 duplicate 而永久丢失这条上下文
        this.dedupe.revoke(dedupeId)
        this.bump(key, "failed")
        const reason = String((err as Error)?.message || err)
        this.log?.warn(`[persona:${key}] 只记上下文失败（已撤回幂等位，重投可被接住）: ${reason}`)
        deliveries.push({ personaId: t.personaId, driverId: t.driver.id, status: "unavailable", reason })
      }
    }
    return this.summarize(event, deliveries)
  }

  /**
   * 按**指定的上下文**投递：把"这批消息属于同一个上下文"这个判断由调用方给定。
   *
   * ## 为何需要它（“不同群可以按需要注入一个会话”的落地口）
   *
   * 会话不面向群——但“归到哪个上下文”有时只有**调用方**知道：
   * 定时任务把三个群的未答问题汇总成一个上下文（“今天这批支持求助”）、
   * 运营把一个工单在多个群的讨论聚到一起（“这单的全部讨论”）。
   * 靠 `sessionKeyOf` 推导不出来——它只看得到单个事件。
   *
   * ## 语义
   *
   * `scope` 就是会话键（与 `sessionKeyOf` 返回值同一个空间）。它**优先于**
   * driver 声明的 `sessionKeyOf` 与部署的 `sessionRouting`：调用方显式指定了，
   * 就不该被推导规则覆盖。
   *
   * `scope` 为空串 = 回到该 driver 的常驻会话（“归到默认上下文”）。
   *
   * 同样受 `sanitizeSessionKey` 清洗（非法字符压下划线、128 限长）——
   * 调用方给的键同样会拼进会话名与盘上绑定键。
   */
  async dispatchInto(event: PersonaEvent, scope: string): Promise<PersonaDispatchResult> {
    if (!this.started) return { status: "unavailable", reason: "人格运行时未启动" }
    const targets = this.resolveTargets(event)
    if (targets.length === 0) return { status: "unrouted" }
    const key = sanitizeSessionKey(scope)
    const deliveries = await this.mapLimit(targets, this.fanoutConcurrency, (t) => this.deliverTo(event, t, key))
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
      let d: PersonaDelivery
      try {
        d = await this.enqueueRound(key, entry.target, entry.events)
      } catch (err) {
        // 单 driver 异常不得让整批中断（后续 driver 的已 admit 幂等位会泄漏、
        // 已完成的投递结果也全部丢失）：记为该 driver 的失败投递，其余照常。
        const reason = String((err as Error)?.message || err)
        this.bump(key, "failed")
        this.log?.warn(`[persona:${key}] 批量投递异常: ${reason}`)
        d = { personaId: entry.target.personaId, driverId: entry.target.driver.id, status: "unavailable", reason }
      }
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
      // 会话缓存与绑定也要按**前缀**清：多会话键形态下键是
      // `personaId/driverId#sessionKey`（不止一条），只删二元键会留下泄漏条目，
      // 且已下线分身的死会话仍会被复用。
      this.invalidateSession(personaId, driverId)
      for (const k of [...this.driverSessions.keys()]) {
        if (k === key) this.driverSessions.delete(k)
      }
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
  private async deliverTo(event: PersonaEvent, t: Target, scopeKey?: string): Promise<PersonaDelivery> {
    const key = driverKey(t.personaId, t.driver.id)
    const dedupeId = `${event.id}\u0000${key}`
    if (!this.dedupe.admit(dedupeId)) {
      this.bump(key, "deduped")
      return { personaId: t.personaId, driverId: t.driver.id, status: "duplicate", reason: `driver ${key} 已处理过事件 ${event.id}` }
    }
    try {
      const delivery = await this.enqueueRound(key, t, [{ ...event, personaId: t.personaId }], scopeKey)
      // 只有「确实已接管」才算处理过；其余（队列满 / 会话解析失败 / driver 已停）一律撤回，
      // 让源的重投能真正被接住。
      if (delivery.status !== "accepted") this.dedupe.revoke(dedupeId)
      return delivery
    } catch (err) {
      // **抛异常分支同样必须撤回**：schedulerFor / enqueueMany 同步抛错（如非法额度）
      // 时若不 revoke，该 id 被永久占位，源重投被判 duplicate 静默丢弃——
      // 与「返回非 accepted」同一条纪律，此分支原先缺失。
      const reason = String((err as Error)?.message || err)
      this.dedupe.revoke(dedupeId)
      this.bump(key, "failed")
      this.log?.warn(`[persona:${key}] 投递异常（已撤回幂等位，重投可被接住）: ${reason}`)
      return { personaId: t.personaId, driverId: t.driver.id, status: "unavailable", reason }
    }
  }

  /** 把一批事件投进某个 driver（同一轮）。`scopeKey` 非 undefined 时优先于推导。 */
  private async enqueueRound(key: string, t: Target, events: PersonaEvent[], scopeKey?: string): Promise<PersonaDelivery> {
    const { personaId, driver } = t
    const scheduler = this.schedulerFor(personaId, driver)
    if (!scheduler) {
      return { personaId, driverId: driver.id, status: "unavailable", reason: "driver 已停止" }
    }
    // 会话路由：先按事件推导键，再按**键分组**入队。
    //
    // 为何必须分组：`dispatchBatch` 会把同一批事件合成一轮，而一批里完全可能是
    // “工单 A 一条 + 工单 B 一条”——全投进同一个上下文就等于把两个事务串在一起。
    //
    // `scopeKey` 非 undefined = 调用方显式指定了“归到哪个上下文”（`dispatchInto`）：
    // 那种情况下不再推导（调用方比推导规则知道得更多）。
    const groups = new Map<string, PersonaEvent[]>()
    for (const e of events) {
      const sessionKey = scopeKey !== undefined ? scopeKey : this.sessionKeyFor(key, driver, e)
      const list = groups.get(sessionKey) ?? []
      list.push(e)
      groups.set(sessionKey, list)
    }

    let firstSessionId: string | undefined
    let maxAhead = 0
    for (const [sessionKey, groupEvents] of groups) {
      const resolved = await this.driverSession(personaId, driver.id, t.user, groupEvents[0]!, sessionKey)
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
      firstSessionId ??= sessionId
      // 用户亲手打的 = 高优先级（B）：走 `insert` 插话（会话在跑则被下一轮边界吸收，
      // 而不是排在几十个待办后面），而不是默认排队。
      //
      // 为何不无条件 `cancelRunning`：硬打断会丢掉正在做的活（包括已发出的部分成果），
      // 而“用户发言”大多数时候并不需要那么急（他说完一句，人格下一轮就能读到）。
      // 真是急切场景用 `GEBAI_PERSONA_HUMAN_INTERRUPT=1` 显式开启硬打断。
      const human = groupEvents.some((e) => e.context.refs?.extra?.by === "human")
      const strategy: "insert" | "queue" | undefined = human ? "insert" : undefined
      const aheads = scheduler.enqueueMany(
        groupEvents.map((e) => ({ event: e, sessionId, user: t.user, threadKey: driver.id, ...(strategy ? { strategy } : {}) })),
      )
      if (aheads.some((a) => a < 0)) {
        this.bump(key, "overflowed")
        return { personaId, driverId: driver.id, status: "overflow", sessionId, reason: "待处理队列已满" }
      }
      // 硬打断（可选）：只对人亲手发言、且显式开启时生效。
      if (human && this.opts.interruptOnHumanSpeech && scheduler.isRunning(sessionId)) {
        this.log?.info(`[persona:${key}] 用户亲手发言 → 打断正在跑的轮次（${sessionId}）`)
        scheduler.cancelRunning(sessionId)
      }
      maxAhead = Math.max(maxAhead, ...aheads)
    }
    this.bump(key, "accepted")
    return {
      personaId,
      driverId: driver.id,
      status: "accepted",
      ...(firstSessionId ? { sessionId: firstSessionId } : {}),
      ...(maxAhead > 0 ? { queued: maxAhead } : {}),
    }
  }

  /**
 * 单个事件的会话路由键。
 *
 * 优先级（高 → 低）：`dispatchInto` 显式指定 > `sessionKeyOf` 函数 > `sessionMap` 规则表 >
 * 部署规则 `sessionRouting`。
 *
 * 缺省（什么都没声明）返回空串 = 该分身的**单一常驻上下文**：会话面向上下文，不面向群。
 *
 * 任一环节抛错都不能杀事件：降级到下一级并记日志（与「读历史失败不阻断投递」同纪律）。
 */
  private sessionKeyFor(driverKeyStr: string, driver: DriverDef, event: PersonaEvent): string {
    if (typeof driver.sessionKeyOf === "function") {
      try {
        const derived = driver.sessionKeyOf(event)
        if (derived && derived.trim()) return sanitizeSessionKey(derived)
      } catch (err) {
        this.log?.warn(`[persona:${driverKeyStr}] sessionKeyOf 抛错（降级到规则表）: ${String((err as Error)?.message || err)}`)
      }
    }
    if (driver.sessionMap?.length) {
      const mapped = matchSessionMap(driver.sessionMap, {
        channel: event.channel,
        kind: event.kind,
        ...(event.context.refs ? { refs: event.context.refs } : {}),
      })
      // undefined = 没有规则命中（→ 继续走部署规则）；"" = 命中且要求常驻上下文
      if (mapped !== undefined) return mapped ? sanitizeSessionKey(mapped) : DEFAULT_SESSION_KEY
    }
    // 缺省（driver 模式）：不分→所有事件进同一常驻会话，不同群的消息共享上下文
    if (this.sessionRouting === "driver") return DEFAULT_SESSION_KEY
    const derived = chatSessionKeyOf(event)
    return derived ? sanitizeSessionKey(derived) : DEFAULT_SESSION_KEY
  }

  private async driverSession(
    personaId: string,
    driverId: string,
    user: string,
    event: PersonaEvent,
    sessionKey: string = DEFAULT_SESSION_KEY,
  ): Promise<{ sessionId: string | null; error?: string }> {
    // 缓存：同一 driver（同一会话键）的会话是稳定的（resolveDriver 幂等），没必要每条事件都问一次。
    // 缓存住的是 **Promise**（不是结果）：并发投递时也只会真正解析一次。
    //
    // 有 TTL（而不是永久）：会话会被用户删除 / 被 GC 归档，而解析器内部的 memo
    // 知道自己校验存活，本层不知道——永久缓存会把「已死的 sessionId」钉死在这里。
    const key = sessionDriverKey(personaId, driverId, sessionKey)
    const cached = this.sessionCache.get(key)
    if (cached && this.now() - cached.at < this.sessionCacheTtlMs) return cached.value
    const p = (async () => {
      try {
        const r = await this.opts.sessions.resolveDriver({ personaId, driverId, user, event, sessionKey })
        // 记下 driverKey → sessionId：取消接口靠它把 driver 翻译成调度器认的会话。
        // 多会话键形态下同一 driver 有多条记录（取消接口取最新一条，其余会话由各自的调度器桶管理）。
        this.driverSessions.set(baseDriverKey(personaId, driverId), r.sessionId)
        return { sessionId: r.sessionId }
      } catch (err) {
        const error = String((err as Error)?.message || err)
        this.log?.warn(`[persona:${personaId}/${driverId}] driver 会话解析失败: ${error}`)
        // 失败不入缓存（下次重试；否则一次瞬时故障会永久失效）
        this.sessionCache.delete(key)
        return { sessionId: null, error }
      }
    })()
    this.sessionCache.set(key, { at: this.now(), value: p })
    return p
  }

  /** 当前时间（可注入，便于测试 TTL）。 */
  private now(): number {
    return this.opts.now ? this.opts.now() : Date.now()
  }

  /**
   * 使某个 driver 的会话解析缓存失效（下一事件重新解析）。
   *
   * 为何需要它：会话被删除 / 重建后，缓存里的 sessionId 就是死 id。引擎会抛
   * 「会话不存在」，而调度器重试 3 次后丢弃——表面症状是「这个分身从此哑了」。
   * 宿主（或引擎错误分类）可以调它立即恢复，而不必等 TTL 过期。
   */
  invalidateSession(personaId: string, driverId?: string): number {
    // 缓存键形状：`personaId/driverId`（默认键）或 `personaId/driverId#sessionKey`（多会话）
    const exact = driverId ? `${personaId}/${driverId}` : null
    const prefix = exact ? `${exact}#` : `${personaId}/`
    let n = 0
    for (const key of [...this.sessionCache.keys()]) {
      if (exact !== null ? key === exact || key.startsWith(prefix) : key.startsWith(prefix)) {
        this.sessionCache.delete(key)
        n++
      }
    }
    if (driverId) this.driverSessions.delete(driverKey(personaId, driverId))
    if (n) this.log?.info(`[persona:${personaId}${driverId ? `/${driverId}` : ""}] 会话解析缓存已失效 ${n} 条（下次事件重新解析）`)
    return n
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
      // 外部占用预检（K2）：用户手动对 driver 常驻会话发起任务期间，事件留在队列
      // 而不是 launch 后被引擎拒掉、首批丢失。isRunning 是引擎的权威回答。
      ...(typeof this.opts.runner.isRunning === "function"
        ? {
            isSessionBusy: (sessionId: string) => {
              try {
                return this.opts.runner.isRunning(sessionId)
              } catch {
                return false
              }
            },
          }
        : {}),
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
        // 本轮的会话键：同一轮的 events 已按会话键分组入队，所以取首条的推导结果即可。
        const sessionKey = this.sessionKeyFor(key, driver, first)
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
          ...(sessionKey ? { sessionKey } : {}),
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
  }
}

/** 投递目标（内部）。 */
interface Target {
  personaId: string
  identity: import("./ports").PersonaIdentity
  driver: DriverDef
  user: string
}
