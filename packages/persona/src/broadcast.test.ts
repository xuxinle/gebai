/**
 * 订阅广播 + 多 driver 用例。
 *
 * ## 两级模型要守住的五条
 *
 * 1. **角色级广播**：同一事件被多个角色各自处理
 * 2. **driver 级筛选**：同一角色内，只有关注该类型的 driver 收到
 * 3. **一模多 driver**：同角色的两个 driver 都关注时**各自处理**（不是二选一）
 * 4. **幂等按「事件×driver」**：不能因一个 driver 处理过就吞掉另一个
 * 5. **定向 driver**：worker 回报只回到派它的那个 driver
 */
import { describe, expect, test } from "bun:test"
import { PersonaRuntime } from "./runtime"
import { coordsOf, matchesAny, matchesSubscription, validateSubscription } from "./subscription"
import type { PersonaEvent } from "./event"
import type { DriverDef, PersonaIdentity, PersonaLookup, PersonaRunner, PersonaRunInput, SessionResolver } from "./ports"

const PEOPLE: Record<string, PersonaIdentity> = {
  xiaoshuang: { id: "xiaoshuang", name: "小霜", identity: "你是小霜", role: "技术响应" },
  laowang: { id: "laowang", name: "老王", identity: "你是老王", role: "运维值守" },
}

/** 场景配置：角色 → driver 列表（含各自订阅）。 */
type Scene = Record<string, DriverDef[]>

function ev(id: string, over: Partial<PersonaEvent> = {}): PersonaEvent {
  return {
    id,
    kind: "message",
    personaId: "",
    channel: "feishu",
    context: { source: "飞书群「支持群」", summary: "有人提问", refs: { chatId: "oc_1", messageId: id } },
    body: "帮我看看",
    receivedAt: Date.now(),
    ...over,
  }
}

function def(id: string, subscriptions: string[], extra: Partial<DriverDef> = {}): DriverDef {
  return { id, focus: `负责 ${id}`, subscriptions, ...extra }
}

function makeHarness(scene: Scene, opts: { maxConcurrent?: number; failResolve?: string[] } = {}) {
  const runs: PersonaRunInput[] = []
  const pending: Array<{ key: string; release: () => void }> = []
  let hold = true
  const resolved: string[] = []

  const lookup: PersonaLookup = {
    get: (id) => PEOPLE[id] ?? null,
    drivers: (personaId) => scene[personaId] ?? [],
    subscribers: (e) => {
      const coords = coordsOf(e)
      const out: Array<{ personaId: string; user: string; driverIds: string[] }> = []
      for (const [personaId, drivers] of Object.entries(scene)) {
        const hit = drivers.filter((d) => matchesAny(d.subscriptions, coords)).map((d) => d.id)
        if (hit.length) out.push({ personaId, user: "admin", driverIds: hit })
      }
      return out
    },
    maxConcurrent: () => opts.maxConcurrent ?? 4,
  }

  const runner: PersonaRunner = {
    isRunning: () => false,
    run: async (input) => {
      runs.push(input)
      if (!hold) return
      await new Promise<void>((r) => pending.push({ key: `${input.personaId}/${input.driver.id}`, release: r }))
    },
  }

  const sessions: SessionResolver = {
    resolveDriver: async ({ personaId, driverId }) => {
      if (opts.failResolve?.includes(`${personaId}/${driverId}`)) throw new Error(`会话存储不可用（${personaId}/${driverId}）`)
      resolved.push(`${personaId}/${driverId}`)
      // 会话按 driver 隔离：不同 driver 必须是不同会话
      const id = `${personaId}:${driverId}`.padEnd(32, "0").slice(0, 32)
      return { sessionId: id, created: true, name: `${personaId}·${driverId}` }
    },
  }

  const runtime = new PersonaRuntime({ lookup, runner, sessions })
  const settle = async () => {
    for (let i = 0; i < 20; i++) await Promise.resolve()
  }
  return {
    runtime,
    runs,
    resolved,
    settle,
    releaseAll: () => {
      hold = false
      while (pending.length) pending.shift()!.release()
    },
    /** 放行某个 driver 的挂起 run（验证"一人格忙不挡另一 driver"）。 */
    release: (key: string) => {
      const i = pending.findIndex((p) => p.key === key)
      if (i >= 0) pending.splice(i, 1)[0].release()
    },
    /** 当前在跑的 driver 列表。 */
    runningKeys: () => pending.map((p) => p.key),
  }
}

describe("subscription：订阅匹配语义", () => {
  test("* 关注全部", () => {
    expect(matchesSubscription("*", { kind: "message", channel: "feishu" })).toBe(true)
  })

  test("单段：kind 或 channel 任一命中", () => {
    expect(matchesSubscription("message", { kind: "message", channel: "feishu" })).toBe(true)
    expect(matchesSubscription("feishu", { kind: "message", channel: "feishu" })).toBe(true)
    expect(matchesSubscription("schedule", { kind: "message", channel: "feishu" })).toBe(false)
  })

  test("双段：且关系，顺序无关", () => {
    const c = { kind: "message", channel: "feishu" }
    expect(matchesSubscription("feishu:message", c)).toBe(true)
    expect(matchesSubscription("message:feishu", c)).toBe(true)
    expect(matchesSubscription("feishu:schedule", c)).toBe(false)
  })

  test("空订阅 = 不关注（不是关注全部）", () => {
    expect(matchesAny([], { kind: "message", channel: "feishu" })).toBe(false)
    expect(matchesAny(["", " "], { kind: "message", channel: "feishu" })).toBe(false)
  })

  test("不支持前缀通配", () => {
    expect(matchesSubscription("feishu*", { kind: "message", channel: "feishu" })).toBe(false)
  })

  test("validateSubscription 拒绝非法写法", () => {
    expect(validateSubscription("*")).toBeNull()
    expect(validateSubscription("feishu:message")).toBeNull()
    expect(validateSubscription("a:b:c")).toContain("段数")
    expect(validateSubscription("feishu*")).toContain("非法")
    expect(validateSubscription("")).toContain("不能为空")
  })
})

describe("driver 级筛选：只有关注的 driver 收到", () => {
  test("同角色两 driver 订阅不同类目 → 各收各的", async () => {
    const h = makeHarness({
      xiaoshuang: [def("support", ["message"]), def("alerts", ["schedule"])],
    })
    h.runtime.start()
    const r1 = await h.runtime.dispatch(ev("m1"))
    expect(r1.deliveries).toHaveLength(1)
    expect(r1.deliveries![0].driverId).toBe("support")

    const r2 = await h.runtime.dispatch(
      ev("s1", { kind: "schedule", channel: "schedule", context: { source: "定时", summary: "巡检" } }),
    )
    expect(r2.deliveries).toHaveLength(1)
    expect(r2.deliveries![0].driverId).toBe("alerts")
    await h.settle()
    expect(h.runs.map((x) => x.driver.id)).toEqual(["support", "alerts"])
    h.releaseAll()
  })

  /** 关键：同角色的两个 driver 都关注时，各自独立处理——不是"选一个"。 */
  test("同角色两 driver 订阅同一类目 → 都处理（各一次，不是二选一）", async () => {
    const h = makeHarness({
      xiaoshuang: [def("main", ["*"]), def("audit", ["message"])],
    })
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1"))
    expect(r.status).toBe("accepted")
    expect(r.deliveries).toHaveLength(2)
    expect(r.deliveries!.map((d) => d.driverId).sort()).toEqual(["audit", "main"])
    await h.settle()
    expect(h.runs.map((x) => `${x.driver.id}`).sort()).toEqual(["audit", "main"])
    h.releaseAll()
  })
})

describe("角色级广播：多角色各自处理", () => {
  test("两角色的 driver 都关注 → 都跑", async () => {
    const h = makeHarness({
      xiaoshuang: [def("support", ["feishu"])],
      laowang: [def("alerts", ["message"])],
    })
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1"))
    expect(r.deliveries).toHaveLength(2)
    expect(r.deliveries!.map((d) => d.personaId).sort()).toEqual(["laowang", "xiaoshuang"])
    await h.settle()
    expect(h.runs).toHaveLength(2)
    h.releaseAll()
  })

  test("无人关注 → unrouted（调用方走旧路径）", async () => {
    const h = makeHarness({ xiaoshuang: [def("support", ["schedule"])] })
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1"))
    expect(r.status).toBe("unrouted")
    expect(h.runs).toHaveLength(0)
  })
})

describe("幂等按「事件 × driver」记账", () => {
  /** 若按事件 id 全局去重，同角色的第二个 driver 将永远收不到。 */
  test("同一事件给同角色两 driver：各收到（不被第一个吞掉）", async () => {
    const h = makeHarness({ xiaoshuang: [def("a", ["*"]), def("b", ["*"])] })
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1"))
    expect(r.deliveries).toHaveLength(2)
    expect(r.deliveries!.every((d) => d.status === "accepted")).toBe(true)
    await h.settle()
    expect(h.runs).toHaveLength(2)
    h.releaseAll()
  })

  test("同一事件重复投给同一 driver → 第二次 duplicate", async () => {
    const h = makeHarness({ xiaoshuang: [def("a", ["*"])] })
    h.runtime.start()
    const first = await h.runtime.dispatch(ev("e1"))
    const second = await h.runtime.dispatch(ev("e1"))
    expect(first.status).toBe("accepted")
    expect(second.status).toBe("duplicate")
    await h.settle()
    expect(h.runs).toHaveLength(1)
    h.releaseAll()
  })

  test("全部 driver 都处理过 → 整体 duplicate", async () => {
    const h = makeHarness({ xiaoshuang: [def("a", ["*"]), def("b", ["*"])] })
    h.runtime.start()
    await h.runtime.dispatch(ev("e1"))
    await h.settle()
    h.releaseAll()
    const again = await h.runtime.dispatch(ev("e1"))
    expect(again.status).toBe("duplicate")
    expect(again.deliveries!.every((d) => d.status === "duplicate")).toBe(true)
  })
})

describe("会话按 driver 隔离", () => {
  test("同角色不同 driver → 不同会话", async () => {
    const h = makeHarness({ xiaoshuang: [def("a", ["*"]), def("b", ["*"])] })
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1"))
    const ids = r.deliveries!.map((d) => d.sessionId)
    expect(new Set(ids).size).toBe(2)
    h.releaseAll()
  })

  test("同一 driver 的连续事件 → 同一会话", async () => {
    const h = makeHarness({ xiaoshuang: [def("a", ["*"])] })
    h.runtime.start()
    const r1 = await h.runtime.dispatch(ev("e1"))
    const r2 = await h.runtime.dispatch(ev("e2"))
    expect(r1.deliveries![0].sessionId).toBe(r2.deliveries![0].sessionId)
    h.releaseAll()
  })
})

describe("部分失败是正常形态", () => {
  test("一个 driver 会话解析失败，另一个照常接管", async () => {
    const h = makeHarness({ xiaoshuang: [def("ok", ["*"]), def("bad", ["*"])] }, { failResolve: ["xiaoshuang/bad"] })
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1"))
    expect(r.status).toBe("accepted")
    const byDriver = Object.fromEntries(r.deliveries!.map((d) => [d.driverId, d.status]))
    expect(byDriver.ok).toBe("accepted")
    expect(byDriver.bad).toBe("unavailable")
    expect(r.deliveries!.find((d) => d.driverId === "bad")!.reason).toContain("会话存储不可用")
    h.releaseAll()
  })
})

describe("定向投递", () => {
  test("事件指定 driverId → 只投该 driver", async () => {
    const h = makeHarness({ xiaoshuang: [def("a", ["*"]), def("b", ["*"])] })
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1", { personaId: "xiaoshuang", driverId: "b" }))
    expect(r.deliveries).toHaveLength(1)
    expect(r.deliveries![0].driverId).toBe("b")
    h.releaseAll()
  })

  test("worker 回报定向到派它的 driver（不惊动同角色的其它 driver）", async () => {
    const h = makeHarness({ xiaoshuang: [def("a", ["*"]), def("b", ["*"])] })
    h.runtime.start()
    const r = await h.runtime.dispatch(
      ev("w1", {
        personaId: "xiaoshuang",
        driverId: "a",
        channel: "worker",
        context: { source: "工作会话", summary: "回报" },
      }),
    )
    expect(r.deliveries).toHaveLength(1)
    expect(r.deliveries![0].driverId).toBe("a")
    h.releaseAll()
  })
})

describe("driver 间互不阻塞", () => {
  test("一个 driver 卡住，另一个照常处理（这正是多 driver 的意义）", async () => {
    const h = makeHarness({
      xiaoshuang: [def("slow", ["message"]), def("fast", ["message"])],
    })
    h.runtime.start()
    await h.runtime.dispatch(ev("e1"))
    await h.settle()
    // 两个 driver 都在跑（各占自己的会话，引擎允许不同会话并发）
    expect(h.runningKeys().sort()).toEqual(["xiaoshuang/fast", "xiaoshuang/slow"])
    // 放行 fast，slow 仍在跑
    h.release("xiaoshuang/fast")
    await h.settle()
    expect(h.runningKeys()).toEqual(["xiaoshuang/slow"])
    h.releaseAll()
  })

  test("每 driver 状态独立可查", async () => {
    const h = makeHarness({ xiaoshuang: [def("a", ["*"]), def("b", ["*"])] })
    h.runtime.start()
    await h.runtime.dispatch(ev("e1"))
    await h.settle()
    expect(h.runtime.state("xiaoshuang", "a")!.accepted).toBe(1)
    expect(h.runtime.state("xiaoshuang", "b")!.accepted).toBe(1)
    expect(h.runtime.state("xiaoshuang", "a")!.driverId).toBe("a")
    h.releaseAll()
  })
})
