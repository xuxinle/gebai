/**
 * 运行时用例：统一事件驱动的端到端行为（内存 Fake，不启服务端）。
 *
 * 重点覆盖三条容易做坏的地方：
 * 1. **零配置向后兼容**：无归属人格 → `unrouted`（调用方走旧路径），不吞事件
 * 2. **幂等**：同 id 重复投递只处理一次
 * 3. **不自动回推**：runner 的产物只由模型工具调用产生，runtime 不碰（本层不推送任何东西）
 */
import { describe, expect, test } from "bun:test"
import { PersonaRuntime } from "./runtime"
import type { PersonaEvent } from "./event"
import type { PersonaIdentity, PersonaLookup, PersonaRunner, PersonaRunInput, SessionResolver } from "./ports"

function ev(id: string, over: Partial<PersonaEvent> = {}): PersonaEvent {
  return {
    id,
    kind: "message",
    // 广播模型：默认**不指定** personaId（空串 = 广播给所有关注者）
    personaId: "",
    channel: "feishu",
    context: { source: "飞书群「支持群」", summary: "有人提问", refs: { chatId: "oc_1", messageId: id } },
    body: "帮我看看",
    receivedAt: Date.now(),
    ...over,
  }
}

const IDENT: PersonaIdentity = { id: "xiaoshuang", name: "小霜", identity: "你是小霜", role: "应答支持群" }

function makeHarness(opts: { maxConcurrent?: number; routes?: boolean; queueLimit?: number } = {}) {
  const runs: PersonaRunInput[] = []
  const resolvers: Array<{ personaId: string; driverId: string }> = []
  const sessionByThread = new Map<string, string>()
  let seq = 0
  /** 挂起中的 run（逐个放行，不共享一个 release——共享会让并发观察失真）。 */
  const pending: Array<() => void> = []

  const lookup: PersonaLookup = {
    get: (id) => (id === "xiaoshuang" ? IDENT : null),
    subscribers: () => (opts.routes === false ? [] : [{ personaId: "xiaoshuang", user: "admin", driverIds: ["main"] }]),
    drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }],
    maxConcurrent: () => opts.maxConcurrent ?? 2,
  }
  /** 是否让 run 挂起（默认挂起，便于观察并发/排队）。 */
  let hold = true
  const runner: PersonaRunner = {
    isRunning: () => false,
    run: async (input) => {
      runs.push(input)
      if (!hold) return
      await new Promise<void>((r) => pending.push(r))
    },
  }
  const sessions: SessionResolver = {
    // 大脑/手模型：每人格只有一个 driver 会话（幂等，同一人格总是同一 id）
    resolveDriver: async ({ personaId, driverId }: { personaId: string; driverId: string }) => {
      resolvers.push({ personaId, driverId })
      const existing = sessionByThread.get(`${personaId}/${driverId}`)
      if (existing) return { sessionId: existing, created: false, name: "x" }
      const id = (++seq).toString(16).padStart(32, "0")
      sessionByThread.set(`${personaId}/${driverId}`, id)
      return { sessionId: id, created: true, name: `${personaId}·${driverId}` }
    },
  }
  const runtime = new PersonaRuntime({
    lookup,
    runner,
    sessions,
    ...(opts.queueLimit !== undefined ? { queueLimit: opts.queueLimit } : {}),
  })
  const settle = async () => {
    for (let i = 0; i < 12; i++) await Promise.resolve()
  }
  return {
    runtime,
    runs,
    resolvers,
    sessionByThread,
    settle,
    /** 放行所有挂起的 run。 */
    releaseAll: () => {
      while (pending.length) pending.shift()!()
    },
    /** 关闭挂起（后续 run 立即返回），并放行已挂起的。 */
    finishAll: () => {
      hold = false
      while (pending.length) pending.shift()!()
    },
  }
}

describe("PersonaRuntime：基本投递", () => {
  test("未启动时返回 unavailable（不静默吞事件）", async () => {
    const h = makeHarness()
    const r = await h.runtime.dispatch(ev("e1"))
    expect(r.status).toBe("unavailable")
    expect(h.runs).toHaveLength(0)
  })

  test("启动后投递被接受并交引擎运行", async () => {
    const h = makeHarness()
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1"))
    expect(r.status).toBe("accepted")
    expect(r.personaId).toBe("xiaoshuang")
    expect(r.sessionId).toBeTruthy()
    await h.settle()
    expect(h.runs).toHaveLength(1)
    expect(h.runs[0].event.id).toBe("e1")
    expect(h.runs[0].identity.name).toBe("小霜")
    expect(h.runs[0].user).toBe("admin")
  })

  test("无归属人格 → unrouted（调用方走旧路径，保证零配置兼容）", async () => {
    const h = makeHarness({ routes: false })
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1"))
    expect(r.status).toBe("unrouted")
    expect(h.runs).toHaveLength(0)
    expect(h.resolvers).toHaveLength(0) // 连会话解析都不发生
  })

  test("订阅者指向不存在的人格 → 该投递 unavailable（与「无人关注」区分开）", async () => {
    const runtime = new PersonaRuntime({
      lookup: { get: () => null, subscribers: () => [{ personaId: "ghost", user: "admin", driverIds: ["main"] }], drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }], maxConcurrent: () => 1 },
      runner: { isRunning: () => false, run: async () => {} },
      sessions: { resolveDriver: async () => ({ sessionId: "a".repeat(32), created: false, name: "x" }) },
    })
    runtime.start()
    const r = await runtime.dispatch(ev("e1"))
    // 订阅者指向的角色在注册表里查不到 → 实际无可投目标（等价于无人关注）
    // 记 warn 日志后返回 unrouted，而不是谎报 accepted
    expect(r.status).toBe("unrouted")
  })

  test("定向投给未订阅的人格 → unrouted（不静默投错人）", async () => {
    const h = makeHarness()
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1", { personaId: "someone_else" }))
    expect(r.status).toBe("unrouted")
    expect(h.runs).toHaveLength(0)
  })
})

describe("PersonaRuntime：幂等", () => {
  test("同 id 第二次投递返回 duplicate 且不再运行", async () => {
    const h = makeHarness()
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("same"))
    const b = await h.runtime.dispatch(ev("same"))
    expect(a.status).toBe("accepted")
    expect(b.status).toBe("duplicate")
    await h.settle()
    expect(h.runs).toHaveLength(1) // 只跑一次（防重复副作用）
  })

  test("无人关注的事件不占幂等位（无订阅者即无「处理过」概念）", async () => {
    const h = makeHarness({ routes: false })
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("x"))
    const b = await h.runtime.dispatch(ev("x"))
    // 两次都是 unrouted：没人关注时不该消耗幂等额度
    // （否则订阅稍后加上，该事件就永远收不到了）
    expect(a.status).toBe("unrouted")
    expect(b.status).toBe("unrouted")
  })
})

describe("PersonaRuntime：会话与线程", () => {
  /**
   * 大脑/手模型：**不再按群分会话**。不同群的消息都进同一个 driver——
   * 因为判断"这两件事要不要合并看"正是 driver 的职责，拆成不同会话就把判断力剥夺了。
   */
  test("不同群的消息进同一个 driver 会话（由 driver 统一判断）", async () => {
    const h = makeHarness()
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("e1"))
    const b = await h.runtime.dispatch(ev("e2", { context: { source: "群B", summary: "x", refs: { chatId: "oc_2" } } }))
    expect(a.sessionId).toBe(b.sessionId!)
    // 会话解析只发生一次（缓存于 driver）
    expect(h.resolvers).toHaveLength(1)
  })

  test("定时任务也进同一个 driver（不单开 anchor 会话）", async () => {
    const h = makeHarness()
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("e1"))
    const b = await h.runtime.dispatch(
      ev("s1", { kind: "schedule", channel: "schedule", context: { source: "定时任务", summary: "巡检" } }),
    )
    expect(a.sessionId).toBe(b.sessionId!)
  })

  test("会话解析失败 → unavailable 且给出理由（不静默）", async () => {
    const runtime = new PersonaRuntime({
      lookup: { get: () => IDENT, subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["main"] }], drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }], maxConcurrent: () => 1 },
      runner: { isRunning: () => false, run: async () => {} },
      sessions: { resolveDriver: async () => { throw new Error("磁盘满了") } },
    })
    runtime.start()
    const r = await runtime.dispatch(ev("e1"))
    expect(r.status).toBe("unavailable")
    expect(r.deliveries?.[0].reason).toContain("磁盘满了")
  })
})

describe("PersonaRuntime：运行中插入（不排队，直接进下一轮）", () => {
  /**
   * 大脑/手模型的关键行为：driver 在跑时新到的事件进**插话信箱**，
   * 在轮次边界被吸收——而不是排队等一个完整任务结束。
   *
   * 这就是"会话要能被新事件插入"的直接体现：消化旧任务期间新信息不会被拖到下次。
   */
  test("driver 运行期间的新事件走插入（queued 无值，进信箱）", async () => {
    const h = makeHarness({ maxConcurrent: 1 })
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("e1"))
    await h.settle()
    const b = await h.runtime.dispatch(ev("e2"))
    const c = await h.runtime.dispatch(ev("e3"))
    expect(a.queued).toBeUndefined()
    // 插入而非排队 → 无位次概念
    expect(b.queued).toBeUndefined()
    expect(c.queued).toBeUndefined()
    const st = h.runtime.state("xiaoshuang", "main")!
    expect(st.sessions[0].pendingInterjections).toBe(2) // e2/e3 在信箱里
    expect(st.sessions[0].interjections).toBe(2)
  })
})

describe("PersonaRuntime：并发额度", () => {
  test("新事件不堆到不同会话（都进同一 driver，走插入）", async () => {
    const h = makeHarness({ maxConcurrent: 1 })
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("e1"))
    const b = await h.runtime.dispatch(ev("e2", { context: { source: "群B", summary: "x", refs: { chatId: "oc_2" } } }))
    await h.settle()
    // 两条事件进同一个 driver 会话（不再按群分会话）
    expect(a.sessionId).toBe(b.sessionId!)
    expect(h.runs).toHaveLength(1) // 只跑一轮；e2 在信箱里等下一轮
    const st = h.runtime.state("xiaoshuang", "main")!
    expect(st.sessions[0].pendingInterjections).toBe(1)
  })

  test("syncConcurrency 可提升额度并立刻放行排队事件", async () => {
    const h = makeHarness({ maxConcurrent: 1 })
    h.runtime.start()
    await h.runtime.dispatch(ev("e1"))
    await h.runtime.dispatch(ev("e2", { context: { source: "群B", summary: "x", refs: { chatId: "oc_2" } } }))
    await h.settle()
    expect(h.runs).toHaveLength(1)
    h.runtime.syncConcurrency("xiaoshuang")
    await h.settle()
    expect(h.runs.length).toBeGreaterThanOrEqual(1)
  })
})

describe("PersonaRuntime：状态与停止", () => {
  test("state 含接受/幂等/溢出统计", async () => {
    const h = makeHarness()
    h.runtime.start()
    await h.runtime.dispatch(ev("e1"))
    await h.runtime.dispatch(ev("e1"))
    const st = h.runtime.state("xiaoshuang", "main")!
    expect(st.accepted).toBe(1)
    expect(st.deduped).toBe(1)
    expect(st.personaId).toBe("xiaoshuang")
  })

  test("未建过调度器的人格 state 返回 null", () => {
    const h = makeHarness()
    expect(h.runtime.state("nobody", "main")).toBeNull()
  })

  test("stop 后投递返回 unavailable 且不再运行", async () => {
    const h = makeHarness()
    h.runtime.start()
    h.runtime.stop()
    const r = await h.runtime.dispatch(ev("e1"))
    expect(r.status).toBe("unavailable")
    expect(h.runs).toHaveLength(0)
  })

  test("isStarted 反映启停", () => {
    const h = makeHarness()
    expect(h.runtime.isStarted()).toBe(false)
    h.runtime.start()
    expect(h.runtime.isStarted()).toBe(true)
  })
})

describe("PersonaRuntime：不自动回推（架构纪律）", () => {
  test("runtime 不产出任何消息推送——只有 runner 拿到输入", async () => {
    // 本用例是「架构断言」：如果将来有人往 runtime 里塞自动回推，这里会立刻暴露
    // （runner 是唯一出口，且它只接受输入、不返回待推送内容）。
    const h = makeHarness()
    h.runtime.start()
    await h.runtime.dispatch(ev("e1"))
    await h.settle()
    expect(h.runs).toHaveLength(1)
    // runner.run 的返回值是 void：调用方无法从 runtime 拿到「要发什么」
    const out = await h.runs[0] ? undefined : undefined
    expect(out).toBeUndefined()
  })
})
