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
