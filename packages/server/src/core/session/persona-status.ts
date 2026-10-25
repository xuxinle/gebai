/**
 * 拟人模式运行态快照：把**散在各处的事实**汇成一份可读视图。
 *
 * ## 为什么必须在服务端聚合，而不是让前端自己拼
 *
 * 「它到底在不在干活」这个问题横跨五个来源：
 *
 * | 事实 | 在哪 | 只看它会漏掉什么 |
 * |---|---|---|
 * | 有哪些人格/分身 | `PersonaCatalog`（定义 + 动态 driver） | 定义存在 ≠ 有会话 ↔ 跑过 |
 * | 在跑什么/排了多少 | `PersonaRuntime.allStates()`（调度器） | 调度器惰性创建——没事件就没条目 |
 * | 事件从哪儿来 | `EventSourceManager.state()` | 源未启动时计数器全是 0，看起来像「没收到」 |
 * | 渠道入站计数 | `ChannelInputBridge.allStats()` | unrouted 持续涨 = 订阅配错，而不是渠道坏了 |
 * | 出站凭据可用性 | `PersonaCredentials.status()` | 没凭据时人格「不回消息」而日志全绿 |
 *
 * 分散到前端拼，必然出现「一处判对、另一处漏判」，而且每加一个来源就要改前端。
 * 这里统一算好：**路由与界面都只读这一个形状**。
 *
 * ## 刻意不返回的东西
 *
 * - 令牌明文（只给「可用/不可用 + 到期时刻」）
 * - 事件正文（快照是运维视图；正文属会话记录，且有隐私面）
 */

import type { PersonaRuntime, PersonaCatalog, EventSourceManager, PersonaCredentials, DriverRegistry } from "@gebai/persona"
import type { InputCounters } from "@gebai/channels"

/** 单个分身的运行态（定义 + 调度 + 会话）。 */
export interface PersonaDriverView {
  id: string
  focus: string
  subscriptions: string[]
  /** static=写在人格定义里（权威）；dynamic=模型自己开的。 */
  origin: "static" | "dynamic"
  /** 动态创建的理由（审计线索）。 */
  reason?: string
  maxConcurrent?: number
  /** 是否已有调度器条目（= 至少发生过一次投递/被显式上线）。 */
  active: boolean
  running: number
  queued: number
  accepted: number
  deduped: number
  overflowed: number
  failed: number
  /** 该分身的常驻会话（未解析时为 null）。 */
  sessionId: string | null
  /** 该会话此刻是否真的在跑任务（引擎视角，比调度计数更可信）。 */
  sessionRunning: boolean
}

export interface PersonaView {
  id: string
  name: string
  role: string
  description?: string
  drivers: PersonaDriverView[]
}

export interface PersonaSourceView {
  name: string
  channel: string
  description: string
  enabled: boolean
  started: boolean
  error?: string
  stats: { emitted: number; accepted: number; unattended: number; duplicate: number; overflow: number; unavailable: number }
}

/** 运行态快照（`GET /api/v1/persona/state` 的形状）。 */
export interface PersonaStateSnapshot {
  enabled: boolean
  generatedAt: number
  personas: PersonaView[]
  sources: PersonaSourceView[]
  /** 事件源加载/启动失败（`{name: reason}`）。 */
  sourceErrors: Record<string, string>
  /** 渠道入站计数（按渠道名）。 */
  channelInput: Record<string, InputCounters>
  /** 出站凭据（不含量牌明文）。 */
  credentials: { ready: boolean; state: string; message: string; remainingMs?: number; needsAuth: boolean } | null
  /** 汇总计数（一眼看全貌）。 */
  totals: { drivers: number; activeDrivers: number; running: number; queued: number; failed: number }
}

export interface PersonaStatusDeps {
  catalog: PersonaCatalog
  runtime: PersonaRuntime
  drivers: DriverRegistry
  sources?: EventSourceManager | null
  credentials?: PersonaCredentials | null
  channelInput?: () => Record<string, InputCounters>
  /** 会话是否在跑（引擎 `isRunning`）：比调度器计数更早/更准地反映「正在干活」。 */
  sessionRunning: (sessionId: string) => boolean
}

/**
 * 组装快照。
 *
 * 会话 id 取自 `runtime.bindings()`（driverKey → sessionId 的已知映射）：
 * 未解析过会话的 driver 显示 `null`——那正是「它还没接过任何事件」的可见信号，
 * 不该编一个 id 出来。
 */
export async function collectPersonaState(deps: PersonaStatusDeps): Promise<PersonaStateSnapshot> {
  const sessions = new Map<string, string>()
  for (const b of driverSessions(deps.runtime)) sessions.set(`${b.personaId}/${b.driverId}`, b.sessionId)

  const personas: PersonaView[] = deps.catalog.all().map((p) => {
    const st = new Map(deps.runtime.personaStates(p.id).map((s) => [s.driverId, s]))
    const drivers: PersonaDriverView[] = deps.catalog.drivers(p.id).map((d) => {
      const rec = deps.drivers.get(p.id, d.id)
      const s = st.get(d.id)
      const sessionId = sessions.get(`${p.id}/${d.id}`) ?? null
      return {
        id: d.id,
        focus: d.focus,
        subscriptions: [...d.subscriptions],
        origin: rec?.origin ?? "static",
        ...(rec?.reason ? { reason: rec.reason } : {}),
        ...(d.maxConcurrent !== undefined ? { maxConcurrent: d.maxConcurrent } : {}),
        active: s !== undefined,
        running: s?.runningSessions ?? 0,
        queued: s?.queued ?? 0,
        accepted: s?.accepted ?? 0,
        deduped: s?.deduped ?? 0,
        overflowed: s?.overflowed ?? 0,
        failed: s?.failed ?? 0,
        sessionId,
        sessionRunning: sessionId ? safeCall(() => deps.sessionRunning(sessionId), false) : false,
      }
    })
    return {
      id: p.id,
      name: p.name,
      role: p.role,
      ...(p.description ? { description: p.description } : {}),
      drivers,
    }
  })

  const sources: PersonaSourceView[] = (deps.sources?.state() ?? []).map((s) => ({
    name: s.name,
    channel: s.channel,
    description: s.description,
    enabled: s.enabled,
    started: s.started,
    ...(s.error ? { error: s.error } : {}),
    stats: s.stats,
  }))

  let credentials: PersonaStateSnapshot["credentials"] = null
  if (deps.credentials) {
    try {
      const c = await deps.credentials.status()
      credentials = {
        ready: c.ready,
        state: c.state,
        message: c.message,
        ...(c.remainingMs !== undefined ? { remainingMs: c.remainingMs } : {}),
        needsAuth: c.needsAuth,
      }
    } catch (err) {
      // 拿不到状态本身也是信息：如实说「不可用」，不要显示成 ready
      credentials = { ready: false, state: "error", message: String((err as Error)?.message || err), needsAuth: false }
    }
  }

  const allDrivers = personas.flatMap((p) => p.drivers)
  return {
    enabled: true,
    generatedAt: Date.now(),
    personas,
    sources,
    sourceErrors: deps.sources?.loadErrors() ?? {},
    channelInput: deps.channelInput?.() ?? {},
    credentials,
    totals: {
      drivers: allDrivers.length,
      activeDrivers: allDrivers.filter((d) => d.active).length,
      running: allDrivers.filter((d) => d.sessionRunning).length,
      queued: allDrivers.reduce((n, d) => n + d.queued, 0),
      failed: allDrivers.reduce((n, d) => n + d.failed, 0),
    },
  }
}

/** `runtime.bindings()` 防呆（测试注入的替身可能没有这个方法）。 */
function driverSessions(runtime: PersonaRuntime): Array<{ personaId: string; driverId: string; sessionId: string }> {
  const fn = (runtime as { bindings?: () => Array<{ personaId: string; driverId: string; sessionId: string }> }).bindings
  if (typeof fn !== "function") return []
  try {
    return fn.call(runtime)
  } catch {
    return []
  }
}

function safeCall<T>(fn: () => T, fallback: T): T {
  try {
    return fn()
  } catch {
    return fallback
  }
}
