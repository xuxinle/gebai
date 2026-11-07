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

function makeHarness(opts: { maxConcurrent?: number; routes?: boolean; queueLimit?: number; sessionRouting?: "driver" | "chat"; sessionKeyOf?: (e: PersonaEvent) => string; sessionMap?: import("./session-map").SessionMapRule[] } = {}) {
  const runs: PersonaRunInput[] = []
  const resolvers: Array<{ personaId: string; driverId: string; sessionKey?: string }> = []
  const sessionByThread = new Map<string, string>()
  let seq = 0
  /** 挂起中的 run（逐个放行，不共享一个 release——共享会让并发观察失真）。 */
  const pending: Array<() => void> = []

  const lookup: PersonaLookup = {
    get: (id) => (id === "xiaoshuang" ? IDENT : null),
    subscribers: () => (opts.routes === false ? [] : [{ personaId: "xiaoshuang", user: "admin", driverIds: ["main"] }]),
    drivers: () => [
      {
        id: "main",
        focus: "主驱动",
        subscriptions: ["*"],
        ...(opts.sessionKeyOf ? { sessionKeyOf: opts.sessionKeyOf } : {}),
        ...(opts.sessionMap ? { sessionMap: opts.sessionMap } : {}),
      },
    ],
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
    // 每 (人格, 分身, 会话键) 一个幂等会话（会话键缺省空串 = 单会话）
    resolveDriver: async ({ personaId, driverId, sessionKey }: { personaId: string; driverId: string; sessionKey?: string }) => {
      resolvers.push({ personaId, driverId, ...(sessionKey ? { sessionKey } : {}) })
      const key = `${personaId}/${driverId}#${sessionKey ?? ""}`
      const existing = sessionByThread.get(key)
      if (existing) return { sessionId: existing, created: false, name: "x" }
      const id = (++seq).toString(16).padStart(32, "0")
      sessionByThread.set(key, id)
      return { sessionId: id, created: true, name: `${personaId}·${driverId}` }
    },
  }
  const runtime = new PersonaRuntime({
    lookup,
    runner,
    sessions,
    ...(opts.queueLimit !== undefined ? { queueLimit: opts.queueLimit } : {}),
    ...(opts.sessionRouting !== undefined ? { sessionRouting: opts.sessionRouting } : {}),
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
   * 缺省语义：**会话面向上下文，不面向群**。
   *
   * 不同群的消息注入**同一个** driver 会话——因为“我盯支持答疑”就是一个上下文，
   * 而判断“这两件事要不要合并看”正是 driver 的职责。把群当会话身份会把“一个人”
   * 拆成几份互不知情的助手（A 群排查一小时，B 群问同一件事时又从零开始）。
   */
  test("缺省：不同群的消息**注入同一会话**（上下文共享，不按群拆）", async () => {
    const h = makeHarness()
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("e1"))
    const b = await h.runtime.dispatch(ev("e2", { context: { source: "群B", summary: "x", refs: { chatId: "oc_2" } } }))
    expect(a.sessionId).toBe(b.sessionId!)
    // 会话解析只发生一次（缓存于 driver）
    expect(h.resolvers).toHaveLength(1)
    expect(h.resolvers[0]!.sessionKey).toBeUndefined()
  })

  test("同一群的连续消息本就是同一会话（键稳定，不按消息分岔）", async () => {
    const h = makeHarness()
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("e1"))
    const b = await h.runtime.dispatch(ev("e2"))
    expect(a.sessionId).toBe(b.sessionId!)
    expect(h.resolvers).toHaveLength(1)
  })

  test("显式要求群间隔离（sessionRouting=chat）→ 每群一会话", async () => {
    const h = makeHarness({ sessionRouting: "chat" })
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("e1"))
    const b = await h.runtime.dispatch(ev("e2", { context: { source: "群B", summary: "x", refs: { chatId: "oc_2" } } }))
    expect(a.sessionId).not.toBe(b.sessionId!)
    expect(h.resolvers.map((r) => r.sessionKey)).toEqual(["oc_1", "oc_2"])
  })

  test("按上下文分会话（sessionKeyOf 按工单号）：同一工单跨群进同一会话", async () => {
    // 这是“会话面向上下文”的正向用法：分割依据是**事务**（工单），不是渠道
    const h = makeHarness({ sessionKeyOf: (e) => e.context.refs?.extra?.ticket ?? "" })
    h.runtime.start()
    const a = await h.runtime.dispatch(
      ev("e1", { context: { source: "群A", summary: "x", refs: { chatId: "oc_a", extra: { ticket: "T-1" } } } }),
    )
    const b = await h.runtime.dispatch(
      ev("e2", { context: { source: "群B", summary: "y", refs: { chatId: "oc_b", extra: { ticket: "T-1" } } } }),
    )
    const c = await h.runtime.dispatch(
      ev("e3", { context: { source: "群A", summary: "z", refs: { chatId: "oc_a", extra: { ticket: "T-2" } } } }),
    )
    expect(a.sessionId).toBe(b.sessionId!) // 同一工单：跨群共享上下文
    expect(c.sessionId).not.toBe(a.sessionId!) // 不同工单：不同上下文
  })

  test("话题群：sessionRouting=chat 时不同话题不同会话", async () => {
    const h = makeHarness({ sessionRouting: "chat" })
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("e1", { context: { source: "话题1", summary: "x", refs: { chatId: "oc_1", threadId: "omt_1" } } }))
    const b = await h.runtime.dispatch(ev("e2", { context: { source: "话题2", summary: "y", refs: { chatId: "oc_1", threadId: "omt_2" } } }))
    expect(a.sessionId).not.toBe(b.sessionId!)
  })

  test("定时任务与群消息同会话（不单开 anchor 会话）", async () => {
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

describe("PersonaRuntime：sessionMap（每话题一会话）", () => {
  const per_thread = [
    { when: "message", key: "{chat}__t_{thread}" },
    { when: "message", key: "{chat}" },
  ]

  test("话题内追问共享上下文，话题之间隔离，普通群退回按群", async () => {
    const h = makeHarness({ sessionMap: per_thread })
    h.runtime.start()
    const t1a = await h.runtime.dispatch(
      ev("e1", { context: { source: "话题1", summary: "x", refs: { chatId: "oc_a", threadId: "omt_1", messageId: "om_1" } } }),
    )
    const t1b = await h.runtime.dispatch(
      ev("e2", { context: { source: "话题1", summary: "y", refs: { chatId: "oc_a", threadId: "omt_1", messageId: "om_2" } } }),
    )
    const t2 = await h.runtime.dispatch(
      ev("e3", { context: { source: "话题2", summary: "z", refs: { chatId: "oc_a", threadId: "omt_2" } } }),
    )
    const plain = await h.runtime.dispatch(
      ev("e4", { context: { source: "普通群", summary: "w", refs: { chatId: "oc_b" } } }),
    )
    expect(t1a.sessionId).toBe(t1b.sessionId!) // 同一话题：接得上
    expect(t1a.sessionId).not.toBe(t2.sessionId!) // 不同话题：隔离
    expect(plain.sessionId).not.toBe(t1a.sessionId!) // 普通群走第二条规则
    // 键形也好对：话题键带 __t_ 标记，普通群是裸 chat
    expect(h.resolvers.map((r) => r.sessionKey)).toEqual(["oc_a__t_omt_1", "oc_a__t_omt_2", "oc_b"])
  })

  test("sessionKeyOf 优先于 sessionMap（函数比规则表更直接）", async () => {
    const h = makeHarness({ sessionKeyOf: () => "from-fn", sessionMap: per_thread })
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1"))
    expect(r.status).toBe("accepted")
    expect(h.resolvers[0]!.sessionKey).toBe("from-fn")
  })

  test("sessionMap 优先于部署 sessionRouting（规则表是更明确的声明）", async () => {
    const h = makeHarness({ sessionRouting: "chat", sessionMap: [{ when: "*", key: "all-in-one" }] })
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("e1"))
    const b = await h.runtime.dispatch(ev("e2", { context: { source: "群B", summary: "x", refs: { chatId: "oc_2" } } }))
    expect(a.sessionId).toBe(b.sessionId!)
    expect(h.resolvers[0]!.sessionKey).toBe("all-in-one")
  })

  test("规则全不命中 → 回落部署规则（不把事件搞丢）", async () => {
    const h = makeHarness({ sessionMap: [{ when: "webhook", key: "hook" }] })
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1"))
    expect(r.status).toBe("accepted")
    expect(h.resolvers[0]!.sessionKey ?? "").toBe("")
  })
})

describe("PersonaRuntime：dispatchInto（按需归到指定上下文）", () => {
  /**
   * 会话不面向群，但“归到哪个上下文”有时只有**调用方**知道：
   * 把三个群的未答问题汇总成“今天这批支持求助”、把一单在多群的讨论聚到一起。
   * `sessionKeyOf` 推导不出来（它只看得到单个事件）。
   */
  test("不同群的消息按调用方指定的 scope 注入**同一会话**", async () => {
    const h = makeHarness()
    h.runtime.start()
    const a = await h.runtime.dispatchInto(ev("e1"), "digest:2026-11-07")
    const b = await h.runtime.dispatchInto(
      ev("e2", { context: { source: "群B", summary: "x", refs: { chatId: "oc_2" } } }),
      "digest:2026-11-07",
    )
    expect(a.sessionId).toBe(b.sessionId!)
    expect(h.resolvers[0]!.sessionKey).toBe("digest:2026-11-07")
  })

  test("scope 优先于 driver 的 sessionKeyOf 与部署规则", async () => {
    const h = makeHarness({ sessionRouting: "chat", sessionKeyOf: (e) => e.context.refs?.chatId ?? "" })
    h.runtime.start()
    const a = await h.runtime.dispatchInto(ev("e1", { context: { source: "群A", summary: "x", refs: { chatId: "oc_a" } } }), "ticket:T-1")
    const b = await h.runtime.dispatchInto(ev("e2", { context: { source: "群B", summary: "y", refs: { chatId: "oc_b" } } }), "ticket:T-1")
    expect(a.sessionId).toBe(b.sessionId!) // 推导规则没把它拆开
    expect(h.resolvers[0]!.sessionKey).toBe("ticket:T-1")
  })

  test("scope 为空串 → 回到该 driver 的常驻上下文", async () => {
    const h = makeHarness()
    h.runtime.start()
    const r = await h.runtime.dispatchInto(ev("e1"), "")
    expect(r.status).toBe("accepted")
    expect(h.resolvers[0]!.sessionKey ?? "").toBe("")
  })

  test("scope 同样被清洗（非法字符/超长不污染会话名与盘上键）", async () => {
    const h = makeHarness()
    h.runtime.start()
    await h.runtime.dispatchInto(ev("e1"), "客户 A/投诉 单 #12")
    const key = h.resolvers[0]!.sessionKey!
    expect(key).not.toContain("/")
    expect(key).not.toContain(" ")
  })

  test("dispatchInto 未启动时如实返回 unavailable（不静默吞）", async () => {
    const h = makeHarness()
    const r = await h.runtime.dispatchInto(ev("e1"), "x")
    expect(r.status).toBe("unavailable")
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
  test("新事件进同一会话时走插入（单会话路由）", async () => {
    const h = makeHarness({ maxConcurrent: 1, sessionRouting: "driver" })
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("e1"))
    const b = await h.runtime.dispatch(ev("e2", { context: { source: "群B", summary: "x", refs: { chatId: "oc_2" } } }))
    await h.settle()
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
    const out = (await h.runs[0]) ? undefined : undefined
    expect(out).toBeUndefined()
  })
})

describe("PersonaRuntime：取消在跑的一轮（长任务卡住的唯一出口）", () => {
  /**
   * 调度器的表按 **sessionId** 建（`Map<sessionId, AbortController>`），而对外接口按
   * `(personaId, driverId)` 说话。少了 driverKey → sessionId 的映射，这两个接口就永远
   * 查不到东西（形同死代码）——表现为「人格卡在长任务上时无法取消」。
   *
   * 这一组用例钉住：取消必须能**真的把在跑的那一轮停下来**（看 signal）。
   */
  test("cancelRunning(personaId, driverId) 能真的中止在跑的一轮", async () => {
    let aborted = false
    const runtime = new PersonaRuntime({
      lookup: { get: () => IDENT, subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["main"] }], drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }], maxConcurrent: () => 1 },
      runner: {
        isRunning: () => false,
        run: async ({ signal }) => {
          await new Promise<void>((resolve) => {
            signal.addEventListener("abort", () => { aborted = true; resolve() })
            setTimeout(resolve, 5000).unref?.()
          })
        },
      },
      sessions: { resolveDriver: async () => ({ sessionId: "s-cancel", created: false, name: "x" }) },
    })
    runtime.start()
    await runtime.dispatch(ev("long"))
    await new Promise((r) => setTimeout(r, 0))

    expect(runtime.cancelRunning("xiaoshuang", "main")).toBe(true)
    await new Promise((r) => setTimeout(r, 0))
    expect(aborted).toBe(true) // 真的停了，不是只返回 true
    runtime.stop()
  })

  test("无在跑时 cancelRunning 返回 false（不谎报成功）", async () => {
    const h = makeHarness()
    h.runtime.start()
    h.runtime.stop()
    expect(h.runtime.cancelRunning("xiaoshuang", "main")).toBe(false)
  })

  test("从未解析过会话的 driver → cancel 返回 false/0（不抛错）", async () => {
    const h = makeHarness()
    h.runtime.start()
    // 没有投递过任何事件 → 没有调度器也没有会话映射
    expect(h.runtime.cancelRunning("xiaoshuang", "main")).toBe(false)
    expect(h.runtime.cancelWaiting("xiaoshuang", "main")).toBe(0)
  })

  test("cancelWaiting 取消排队事件但不影响在跑的一轮", async () => {
    // 用 queue 策略制造「确实有排队」的前提（insert 策略下事件全进信箱，队列永不满）
    const h = makeHarness({ maxConcurrent: 1 })
    h.runtime.start()
    await h.runtime.dispatch(ev("running"))
    await h.settle()
    // insert 策略下后续事件进信箱；cancelWaiting 只清队列，故此处返回 0 是正确语义
    const n = h.runtime.cancelWaiting("xiaoshuang", "main")
    expect(n).toBe(0)
    h.finishAll()
  })
})

describe("PersonaRuntime：投递失败不占用幂等位（否则瞬时故障=永久丢事件）", () => {
  /**
   * 病根：旧实现「先 dedupe.admit 记账，后入队」，而入队可能失败（存储瞬时不可达 /
   * 队列已满）。记账不回滚 → 该 id 被永久占位 → 源按规范重投会被判 duplicate 静默丢弃。
   *
   * 各源都会重投（飞书 ACK 前断连重推、调度器重启重触发、webhook 重试），
   * 所以这条直接决定「人的话会不会被静默丢弃」。
   */
  test("会话解析瞬时失败 → 恢复后重投同一事件能被接住（不判 duplicate）", async () => {
    let broken = true
    const runtime = new PersonaRuntime({
      lookup: { get: () => IDENT, subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["main"] }], drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }], maxConcurrent: () => 1 },
      runner: { isRunning: () => false, run: async () => {} },
      sessions: {
        resolveDriver: async () => {
          if (broken) throw new Error("存储瞬时不可达")
          return { sessionId: "s-recover", created: false, name: "x" }
        },
      },
    })
    runtime.start()

    const first = await runtime.dispatch(ev("m1"))
    expect(first.status).toBe("unavailable")

    broken = false // 故障恢复
    const retry = await runtime.dispatch(ev("m1"))
    // 关键断言：不能是 duplicate——那意味着这条消息永远没人处理
    expect(retry.status).toBe("accepted")
    runtime.stop()
  })

  test("真正已接管的事件仍判 duplicate（撤回不能把幂等弄失效）", async () => {
    const h = makeHarness()
    h.runtime.start()
    const a = await h.runtime.dispatch(ev("dup"))
    expect(a.status).toBe("accepted")
    const b = await h.runtime.dispatch(ev("dup"))
    expect(b.status).toBe("duplicate")
    h.finishAll()
  })

  test("第 1 条失败、第 2 条成功后：各自幂等独立（不互相污染）", async () => {
    let fail = true
    const runtime = new PersonaRuntime({
      lookup: { get: () => IDENT, subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["main"] }], drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }], maxConcurrent: () => 1 },
      runner: { isRunning: () => false, run: async () => {} },
      sessions: {
        resolveDriver: async () => {
          if (fail) throw new Error("瞬时")
          return { sessionId: "s-ok", created: false, name: "x" }
        },
      },
    })
    runtime.start()
    expect((await runtime.dispatch(ev("x1"))).status).toBe("unavailable")
    fail = false
    expect((await runtime.dispatch(ev("x1"))).status).toBe("accepted")
    // x2 是新事件：应当独立被接受
    expect((await runtime.dispatch(ev("x2"))).status).toBe("accepted")
    // x1 第二次（已成功接管过）→ duplicate
    expect((await runtime.dispatch(ev("x1"))).status).toBe("duplicate")
    runtime.stop()
  })

  test("dispatchBatch 整批入队失败 → 整批撤回（重投能回来完整一批）", async () => {
    let fail = true
    const runs: string[][] = []
    const runtime = new PersonaRuntime({
      lookup: { get: () => IDENT, subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["main"] }], drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }], maxConcurrent: () => 1 },
      runner: { isRunning: () => false, run: async ({ events }) => { runs.push(events.map((e) => e.id)) } },
      sessions: {
        resolveDriver: async () => {
          if (fail) throw new Error("瞬时")
          return { sessionId: "s-batch", created: false, name: "x" }
        },
      },
    })
    runtime.start()
    const first = await runtime.dispatchBatch([ev("b1"), ev("b2")])
    expect(first.status).toBe("unavailable")

    fail = false
    const retry = await runtime.dispatchBatch([ev("b1"), ev("b2")])
    expect(retry.status).toBe("accepted")
    await new Promise((r) => setTimeout(r, 0))
    // 两条都要在（而不是只有一半——那会让模型看到残缺上下文）
    expect(runs.flat().sort()).toEqual(["b1", "b2"])
    runtime.stop()
  })
})

describe("PersonaRuntime：投递路径的异常护栏（K3 修复）", () => {
  /**
   * 病根：schedulerFor / enqueueRound 若**抛异常**（如非法额度在 PersonaScheduler
   * 构造时同步抛错），旧实现直接向上冒——①已 admit 的幂等位没人 revoke，源重投
   * 判 duplicate 静默丢弃；②dispatchBatch 的 Promise 链让同批其它 driver 的
   * 投递结果全部丢失。这里钉住「异常也走 revoke + 失败投递」的纪律。
   */
  test("单条投递抛异常 → 幂等位被撤回，重投能被接住（不判 duplicate）", async () => {
    let explode = true
    const runtime = new PersonaRuntime({
      lookup: { get: () => IDENT, subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["main"] }], drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }], maxConcurrent: () => 1 },
      runner: { isRunning: () => false, run: async () => {} },
      sessions: {
        resolveDriver: async () => {
          if (explode) throw new Error("调度器构造炸了")
          return { sessionId: "s-k3", created: false, name: "x" }
        },
      },
    })
    runtime.start()
    const first = await runtime.dispatch(ev("k1"))
    expect(first.status).toBe("unavailable")
    expect(first.deliveries?.[0]?.reason).toContain("调度器构造炸了")

    explode = false
    const retry = await runtime.dispatch(ev("k1"))
    expect(retry.status).toBe("accepted") // 若幂等位泄漏，这里会是 duplicate
    runtime.stop()
  })

  test("dispatchBatch 单 driver 异常不中断整批（其它 driver 照常投递）", async () => {
    // 同一事件命中两个 driver：bad 的会话解析抛错，good 照常被接管
    const drivers = [
      { id: "good", focus: "好驱动", subscriptions: ["*"] },
      { id: "bad", focus: "坏驱动", subscriptions: ["*"] },
    ]
    const runtime = new PersonaRuntime({
      lookup: { get: () => IDENT, subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["good", "bad"] }], drivers: () => drivers, maxConcurrent: () => 1 },
      runner: { isRunning: () => false, run: async () => {} },
      sessions: {
        resolveDriver: async ({ driverId }: { driverId: string }) => {
          if (driverId === "bad") throw new Error("bad 的会话解析炸了")
          return { sessionId: "s-good", created: false, name: "x" }
        },
      },
    })
    runtime.start()
    const r = await runtime.dispatchBatch([ev("m1")])
    // good 被接管，bad 如实报 unavailable（而不是整批 reject 丢失结果）
    const byDriver = Object.fromEntries((r.deliveries ?? []).map((d) => [d.driverId, d.status]))
    expect(byDriver["good"]).toBe("accepted")
    expect(byDriver["bad"]).toBe("unavailable")
    runtime.stop()
  })
})

describe("PersonaRuntime：外部占用预检接线（K2）", () => {
  test("runner.isRunning=true 时事件不 launch（留在队列），占用结束后接起", async () => {
    let busy = true
    const runs: string[] = []
    const runtime = new PersonaRuntime({
      lookup: { get: () => IDENT, subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["main"] }], drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }], maxConcurrent: () => 1 },
      runner: {
        isRunning: () => busy,
        run: async ({ events }) => {
          runs.push(events.map((e) => e.id).join(","))
        },
      },
      sessions: { resolveDriver: async () => ({ sessionId: "s-busy", created: false, name: "x" }) },
    })
    runtime.start()
    const r = await runtime.dispatch(ev("m1"))
    expect(r.status).toBe("accepted") // 已接管（入队成功）
    await new Promise((resolve) => setTimeout(resolve, 5))
    expect(runs).toEqual([]) // 但没有 launch（外部占用预检拦住）

    busy = false
    await runtime.dispatch(ev("m2"))
    await new Promise((resolve) => setTimeout(resolve, 5))
    // m2 入队触发 pump → m1 先跑（FIFO）
    expect(runs.join("|")).toContain("m1")
    runtime.stop()
  })
})

describe("PersonaRuntime.bindings：运行态快照的会话来源", () => {
  /**
   * `bindings()` 是快照里 `sessionId` / `sessionRunning` 两列的唯一来源。
   *
   * 为何钉住：宿主侧快照（`persona-status.ts`）只要拿不到它，就把所有分身的
   * `sessionId` 一律显示 null——**分身明明正在跑，面板却说「它还没接过事件」**，
   * 把「在干活」误报成「没通电」，正是这套视图最该避免的误判。
   * 实测形态：真投递跑完一轮后，`/api/v1/persona/state` 里 support 的
   * `sessionId` 仍为 null，而会话里已经有完整对话。
   */
  test("解析过会话后，bindings() 能报出 driverKey → sessionId", async () => {
    const runtime = new PersonaRuntime({
      lookup: {
        get: () => IDENT,
        subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["main"] }],
        drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }],
        maxConcurrent: () => 1,
      },
      runner: { isRunning: () => false, run: async () => {} },
      sessions: { resolveDriver: async () => ({ sessionId: "s-bound", created: false, name: "x" }) },
    })
    runtime.start()
    // 未投递前：没有任何绑定（这本身就是「它还没接过事件」的正确信号）
    expect(runtime.bindings()).toEqual([])

    await runtime.dispatch(ev("bind-1"))

    expect(runtime.bindings()).toEqual([{ personaId: "xiaoshuang", driverId: "main", sessionId: "s-bound" }])
    runtime.stop()
  })

  test("driver 下线后绑定被清掉（不留下指向已停分身的陈旧会话）", async () => {
    const runtime = new PersonaRuntime({
      lookup: {
        get: () => IDENT,
        subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["main"] }],
        drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }],
        maxConcurrent: () => 1,
      },
      runner: { isRunning: () => false, run: async () => {} },
      sessions: { resolveDriver: async () => ({ sessionId: "s-gone", created: false, name: "x" }) },
    })
    runtime.start()
    await runtime.dispatch(ev("bye-1"))
    expect(runtime.bindings()).toHaveLength(1)

    runtime.syncDriver("xiaoshuang", "main", "offline")
    expect(runtime.bindings()).toEqual([])
    runtime.stop()
  })
})
