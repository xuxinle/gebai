/**
 * 会话解析：`(personaId, driverId)` → 一个**稳定**的常驻会话。
 *
 * ## 设计要求（"稳定又可灵活扩展"）
 *
 * | 要求 | 做法 |
 * |---|---|
 * | **稳定** | 绑定写在 `SessionData.persona`，会话自身即真相源：重启、迁移、导入导出都不丢；删除会话映射随之消失，不会留下指向空会话的幽灵索引 |
 * | **不重复** | 幂等：同一个 (personaId, driverId) 永远返回同一 sessionId（并发调用也只建一个） |
 * | **可扩展** | 绑定额外字段放 `SessionData.persona` 上即可；「一个 driver 多会话」只需改查找侧（取最新/按标签），存储不动 |
 * | **可观测** | 会话名带人格与分身标识（`小霜·支持`），在会话列表里一眼能认出来 |
 *
 * ## 为什么不用外部映射表
 *
 * 外部表最难的从来不是写入，而是**一致性**：会话被删/被 GC 归档/被用户重命名后，
 * 表里的记录就成了悬空指针，且没有任何机制能保证它被清理。而映射挂在会话上，
 * 「会话没了 → 映射自然没了」是存储层的免费保证。
 *
 * ## 反向查找的开销
 *
 * `store.listSessions(user)` 会扫描该用户的全部会话（已在列表接口使用，内部有索引缓存）。
 * 首次查找后由 `memo` 记住结果，后续同 key 直接命中——正常运行时每个 driver 只付一次扫描成本。
 */

import type { ResolvedSession, SessionResolver } from "@gebai/persona"
import type { SessionData, SessionStore } from "./store"

/** 解析器依赖的最小 store 面（便于单测注入）。 */
export interface PersonaSessionStore {
  listSessions(userId: string): Promise<SessionData[]>
  createSession(userId: string, name?: string): Promise<SessionData>
  save(session: SessionData, opts?: { touch?: boolean }): Promise<void>
}

export interface PersonaSessionResolverOptions {
  store: SessionStore | PersonaSessionStore
  log?: { info(msg: string): void; warn(msg: string): void }
}

/** 会话绑定键（`personaId\0driverId`）——用 NUL 分隔避免 id 里出现冒号时的歧义。 */
function bindingKey(personaId: string, driverId: string): string {
  return `${personaId}\u0000${driverId}`
}

/**
 * 会话名的默认构造：`{人格名}·{分身 id}`。
 *
 * 为何不用 focus（那是给人看的描述，可能很长）：会话名在侧栏展示，
 * 需要短且**稳定**——focus 改了不该导致会话改名（改名会让用户找不到历史会话）。
 */
function defaultName(personaName: string, driverId: string): string {
  return `${personaName}·${driverId}`
}

export class PersonaSessionResolver implements SessionResolver {
  private readonly store: PersonaSessionStore
  private readonly log?: PersonaSessionResolverOptions["log"]
  /** 已解析的绑定（避免每次事件都扫全部会话）。 */
  private readonly memo = new Map<string, string>()
  /** 同名并发保护：同一 key 的并发解析只建一个会话。 */
  private readonly inflight = new Map<string, Promise<ResolvedSession>>()
  /** 人格名解析（会话命名用；缺省退回 id）。 */
  nameOf: (personaId: string) => string = (id) => id

  constructor(opts: PersonaSessionResolverOptions) {
    this.store = opts.store
    this.log = opts.log
  }

  /**
   * 启动时预热：扫一遍该用户的会话，把已有绑定装进 memo。
   *
   * 不预热也能工作（首次查找走扫描），但预热让**重启后的第一条事件**不必等扫描，
   * 也便于启动期就发现「绑定字段异常」。
   */
  async warm(user: string): Promise<number> {
    let found = 0
    try {
      const sessions = await this.store.listSessions(user)
      // 按 boundAt 升序：多会话绑同一 key 时取最早的
      const bound = sessions.filter((s) => s.persona?.personaId && s.persona?.driverId)
      bound.sort((a, b) => (a.persona!.boundAt ?? 0) - (b.persona!.boundAt ?? 0))
      for (const s of bound) {
        const key = bindingKey(s.persona!.personaId, s.persona!.driverId)
        // 只记“首个”（最早绑定的）；计数按**盘上找到的绑定数**而不是“内存新增数”——
        // 已在 memo 里的 key 也应计入（否则重启后 warm 会报 0 条，看起来像没恢复）
        if (!this.memo.has(key)) {
          this.memo.set(key, s.id)
          found++
        }
      }
      // 已在 memo 中的也计入总数（语义：盘上可恢复的绑定总数）
      const total = new Set(bound.map((s) => bindingKey(s.persona!.personaId, s.persona!.driverId))).size
      return total
    } catch (err) {
      this.log?.warn(`[persona:sessions] 预热失败（首次查找会走扫描）: ${String((err as Error)?.message || err)}`)
      return found
    }
  }

  async resolveDriver(input: { personaId: string; driverId: string; user: string; event: unknown }): Promise<ResolvedSession> {
    const { personaId, driverId, user } = input
    const key = bindingKey(personaId, driverId)

    // ① memo 命中：确认会话仍存在（被删/被归档后要重建，不能返回死 id）
    const known = this.memo.get(key)
    if (known) {
      const alive = await this.exists(known, user)
      if (alive) return { sessionId: known, created: false, name: this.nameFor(personaId, driverId) }
      this.memo.delete(key) // 会话已不在：清除后走重建
      this.log?.info(`[persona:sessions] 绑定的会话 ${known} 已不存在，将为 ${personaId}/${driverId} 重建`)
    }

    // ② 同名并发合并：两个事件同时到达时只建一个会话
    const pending = this.inflight.get(key)
    if (pending) return pending

    const task = this.resolveOrCreate(personaId, driverId, user, key).finally(() => this.inflight.delete(key))
    this.inflight.set(key, task)
    return task
  }

  private async resolveOrCreate(personaId: string, driverId: string, user: string, key: string): Promise<ResolvedSession> {
    // ③ 扫盘：memo 未命中（首次/重启/被清）时找已有绑定
    try {
      const sessions = await this.store.listSessions(user)
      const bound = sessions
        .filter((s) => s.persona?.personaId === personaId && s.persona?.driverId === driverId)
        // 取最早绑定的：多会话时行为稳定（不随新建漂移）
        .sort((a, b) => (a.persona!.boundAt ?? 0) - (b.persona!.boundAt ?? 0))
      if (bound.length > 0) {
        const hit = bound[0]!
        this.memo.set(key, hit.id)
        return { sessionId: hit.id, created: false, name: hit.name }
      }
    } catch (err) {
      // 扫盘失败不该让事件丢失：降级到「新建」（会在建后立刻记住）
      this.log?.warn(`[persona:sessions] 扫描会话失败（改为新建）: ${String((err as Error)?.message || err)}`)
    }

    // ④ 新建并绑定
    const name = this.nameFor(personaId, driverId)
    const session = await this.store.createSession(user, name)
    session.persona = { personaId, driverId, boundAt: Date.now() }
    await this.store.save(session, { touch: false })
    this.memo.set(key, session.id)
    this.log?.info(`[persona:sessions] 为 ${personaId}/${driverId} 新建会话 ${session.id}（${name}）`)
    return { sessionId: session.id, created: true, name }
  }

  /** 会话是否仍存在（且归属正确）。 */
  private async exists(sessionId: string, user: string): Promise<boolean> {
    try {
      const all = await this.store.listSessions(user)
      return all.some((s) => s.id === sessionId)
    } catch {
      // 查不到即当不存在（宁可重建也不返回可能已死的 id：死 id 上跑任务会静默丢消息）
      return false
    }
  }

  private nameFor(personaId: string, driverId: string): string {
    return defaultName(this.nameOf(personaId), driverId)
  }

  /** 显式解绑（人格下线/分身删除时清理；不删会话本身，历史仍可查）。 */
  async unbind(personaId: string, driverId: string, user: string): Promise<boolean> {
    const key = bindingKey(personaId, driverId)
    const sessionId = this.memo.get(key)
    this.memo.delete(key)
    try {
      const all = await this.store.listSessions(user)
      const target = sessionId ? all.find((s) => s.id === sessionId) : all.find((s) => s.persona?.personaId === personaId && s.persona?.driverId === driverId)
      if (!target) return false
      delete target.persona
      await this.store.save(target, { touch: false })
      return true
    } catch (err) {
      this.log?.warn(`[persona:sessions] 解绑失败: ${String((err as Error)?.message || err)}`)
      return false
    }
  }

  /** 当前已知绑定（诊断/面板）。 */
  bindings(): Array<{ personaId: string; driverId: string; sessionId: string }> {
    return [...this.memo.entries()].map(([k, sessionId]) => {
      const [personaId, driverId] = k.split("\u0000")
      return { personaId: personaId!, driverId: driverId!, sessionId }
    })
  }
}
