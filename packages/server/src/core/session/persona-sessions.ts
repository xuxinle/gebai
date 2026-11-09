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
  /** 只记上下文用（appendOnly）：读一个会话与追加一条消息。 */
  load?(sessionId: string, userId?: string): Promise<SessionData | null>
  appendMessage?(sessionId: string, msg: import("@gebai/sdk").Message, userId?: string): Promise<void>
}

export interface PersonaSessionResolverOptions {
  store: SessionStore | PersonaSessionStore
  log?: { info(msg: string): void; warn(msg: string): void }
  /**
   * 取该人格声明的子代理工具包（`PersonaDef.agents`）。
   *
   * 为何要传进来而不是在建会话后再装：`loadedSubAgents` 是**会话持久化字段**——
   * 建会话那一刻写进去，重启/恢复时引擎据此重新注册工具。事后补装就必须另有
   * 一条路径把它写盘，否则“重载后工具不见了”。写在这里等于用一次写盘拿到这两个保证。
   *
   * 缺省不装（返回空数组）：工具包是可选能力，默认不改动任一部署的工具面与 token 开销。
   */
  agentsFor?: (personaId: string) => string[]
}

/** 会话绑定键（`personaId\0driverId\0sessionKey`）——用 NUL 分隔避免 id 里出现冒号时的歧义。
 *  sessionKey 缺省空串（单会话形态，与历史绑定键形状兼容：旧盘上无第三段的记录按空键读取）。 */
function bindingKey(personaId: string, driverId: string, sessionKey = ""): string {
  return `${personaId}\u0000${driverId}\u0000${sessionKey}`
}

/** 从盘上读出绑定键（兼容旧记录：无 sessionKey 字段按空键）。 */
function bindingKeyOf(s: { persona: { personaId: string; driverId: string; sessionKey?: string } }): string {
  return bindingKey(s.persona.personaId, s.persona.driverId, s.persona.sessionKey ?? "")
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
  /** 该人格声明的子代理工具包（见 `PersonaSessionResolverOptions.agentsFor`）。 */
  private readonly agentsFor?: (personaId: string) => string[]
  /** 已解析的绑定（避免每次事件都扫全部会话）。 */
  private readonly memo = new Map<string, string>()
  /** 同名并发保护：同一 key 的并发解析只建一个会话。 */
  private readonly inflight = new Map<string, Promise<ResolvedSession>>()
  /** 人格名解析（会话命名用；缺省退回 id）。 */
  nameOf: (personaId: string) => string = (id) => id

  constructor(opts: PersonaSessionResolverOptions) {
    this.store = opts.store
    this.log = opts.log
    this.agentsFor = opts.agentsFor
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
        const key = bindingKeyOf(s as never)
        // 只记“首个”（最早绑定的）；计数按**盘上找到的绑定数**而不是“内存新增数”——
        // 已在 memo 里的 key 也应计入（否则重启后 warm 会报 0 条，看起来像没恢复）
        if (!this.memo.has(key)) {
          this.memo.set(key, s.id)
          found++
        }
      }
      // 已在 memo 中的也计入总数（语义：盘上可恢复的绑定总数）
      const total = new Set(bound.map((s) => bindingKeyOf(s as never))).size
      return total
    } catch (err) {
      this.log?.warn(`[persona:sessions] 预热失败（首次查找会走扫描）: ${String((err as Error)?.message || err)}`)
      return found
    }
  }

  async resolveDriver(input: { personaId: string; driverId: string; user: string; event: unknown; sessionKey?: string }): Promise<ResolvedSession> {
    const { personaId, driverId, user } = input
    const sessionKey = input.sessionKey ?? ""
    const key = bindingKey(personaId, driverId, sessionKey)

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

    const task = this.resolveOrCreate(personaId, driverId, user, key, sessionKey).finally(() => this.inflight.delete(key))
    this.inflight.set(key, task)
    return task
  }

  private async resolveOrCreate(personaId: string, driverId: string, user: string, key: string, sessionKey: string): Promise<ResolvedSession> {
    // ③ 扫盘：memo 未命中（首次/重启/被清）时找已有绑定
    try {
      const sessions = await this.store.listSessions(user)
      const bound = sessions
        .filter((s) => s.persona?.personaId === personaId && s.persona?.driverId === driverId && (s.persona.sessionKey ?? "") === sessionKey)
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

    // ④ 新建并绑定（多会话键形态下会话名带键后缀，侧栏可区分同分身的不同会话）
    const name = this.nameFor(personaId, driverId) + (sessionKey ? `·${sessionKey}` : "")
    const session = await this.store.createSession(user, name)
    session.persona = { personaId, driverId, ...(sessionKey ? { sessionKey } : {}), boundAt: Date.now() }
    // 工具包在建会话时一次写进持久化字段（`loadedSubAgents`）——引擎重载后据此重注册。
    // 不写的话：新会话当时能用（内存里有），重启后就“工具不见了”，且不报错。
    const agents = this.agentsFor?.(personaId) ?? []
    if (agents.length) session.loadedSubAgents = [...agents]
    await this.store.save(session, { touch: false })
    this.memo.set(key, session.id)
    this.log?.info(
      `[persona:sessions] 为 ${personaId}/${driverId} 新建会话 ${session.id}（${name}）` +
        (agents.length ? `，装载工具包 ${agents.join(",")}` : ""),
    )
    return { sessionId: session.id, created: true, name }
  }

  /**
   * 只把一条消息写进会话上下文，**不启动任何执行**。
   *
   * 用途：人格自己以用户名义发出的消息回灌进群时，必须记下（它得知道群里
   * 此刻有自己这段话），却绝不能拿它再跑一轮（会把自己刚说的话当新指令重答）。
   *
   * 为何不用 resolveDriver + runner：runner 的语义就是「跑一轮」，而引擎有
   * `tasks.has(sessionId)` 互斥——会话正在跑时塞进去只会排队等下一轮，
   * 还是会被执行。写入必须绕开调度器与引擎，直接落会话记录。
   *
   * 写成 `role: "user"` 而不是自造一个角色：它就是「群里进来了这句话」这个事实，
   * 与用户发言同形。是谁说的由正文里的身份注记说清楚（见 `speakerNote`），
   * 不靠角色字段——引擎对 user/assistant 的理解固定，加新角色会改动所有下游。
   */
  async appendOnly(input: {
    personaId: string
    driverId: string
    user: string
    event: import("@gebai/persona").PersonaEvent
    sessionKey?: string
    note?: string
    /** 发送者类别（落盘标记用：面板能区分“它看过没答”与“它没收到”）。 */
    kind?: "self" | "owner" | "other"
    reason?: string
  }): Promise<{ sessionId: string; created: boolean }> {
    const kind = input.kind ?? "other"
    // store 可能是精简实现（单测注入只实现了列表/建/存）：没有写入能力就如实报错——
    // 静默成功会让“上下文里少了一条”无处可查。
    if (typeof this.store.load !== "function" || typeof this.store.appendMessage !== "function") {
      throw new Error("会话存储未实现 load/appendMessage（只记上下文能力不可用）")
    }
    const resolved = await this.resolveDriver({
      personaId: input.personaId,
      driverId: input.driverId,
      user: input.user,
      event: input.event,
      ...(input.sessionKey !== undefined ? { sessionKey: input.sessionKey } : {}),
    })
    const session = await this.store.load(resolved.sessionId, input.user)
    if (!session) {
      // 刚 resolve 出来的会话随即不见了（被删/被 GC）：不抛错——这条消息不够重要到
      // 值得中断投递；但必须留痕，否则“上下文里怎么少了一条”无从查起。
      this.log?.warn(`[persona:sessions] 只记模式：会话 ${resolved.sessionId} 已不可读，本条不落盘`)
      return { sessionId: resolved.sessionId, created: resolved.created }
    }
    // 正文 = 事件摘要头 + 原正文 + 身份注记。注记放在**末尾**（与 renderEventBlock 一致：
    // 先事实后说明），且用括号包住以免被误读为消息内容。
    const body = input.event.body ?? ""
    const content = [
      `【${input.event.context?.source ?? input.event.channel}】${input.event.context?.summary ?? ""}`.trim(),
      body,
      ...(input.note ? [`（${input.note}）`] : []),
    ]
      .filter((s) => s && s.trim())
      .join("\n")
    await this.store.appendMessage(
      resolved.sessionId,
      {
        id: crypto.randomUUID(),
        role: "user",
        content,
        createdAt: Date.now(),
        // 标记「这条只进了上下文、没有触发执行」：面板/诊断能区分
        // “它看过但没答”与“它压根没收到”——这两种故障的现象完全不同。
        // 用独立字段而不是 engineNote：后者是系统合成提醒的标记，会被 UI 弱化。
        contextOnly: { kind, ...(input.reason ? { reason: input.reason } : {}) },
      },
      input.user,
    )
    return { sessionId: resolved.sessionId, created: resolved.created }
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
  bindings(): Array<{ personaId: string; driverId: string; sessionKey: string; sessionId: string }> {
    return [...this.memo.entries()].map(([k, sessionId]) => {
      const [personaId, driverId, sessionKey] = k.split("\u0000")
      return { personaId: personaId!, driverId: driverId!, sessionKey: sessionKey ?? "", sessionId }
    })
  }
}
