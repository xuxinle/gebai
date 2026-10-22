/**
 * Driver 注册表用例：**静态定义 + 模型运行时创建**。
 *
 * 这是"角色能自我组织"的关键：常用分工写死在配置里，临时分工由角色现场开。
 *
 * 五条必须守住的：
 * 1. 静态 + 动态共存，静态优先（手写配置才是权威）
 * 2. 动态创建**立即参与订阅匹配**（下个符合的事件就投给它）
 * 3. 动态 driver **必须落盘**（重启不丢——分工是工作结构，不是一次性状态）
 * 4. 创建校验从严（订阅写错 = 永远收不到事件，而现象是"没反应"）
 * 5. 静态 driver **不能运行时下线**（改配置才是对的做法）
 */
import { describe, expect, test } from "bun:test"
import { DriverRegistry, validateCreate, type DriverPersistence, type DriverRecord } from "./driver"
import { PersonaRuntime } from "./runtime"
import { coordsOf, matchesAny } from "./subscription"
import type { DriverDef, PersonaIdentity, PersonaLookup, PersonaRunner, SessionResolver } from "./ports"

const IDENT: PersonaIdentity = { id: "xiaoshuang", name: "小霜", identity: "你是小霜", role: "技术响应" }

function def(id: string, subscriptions: string[], extra: Partial<DriverDef> = {}): DriverDef {
  return { id, focus: `负责 ${id}`, subscriptions, ...extra }
}

/** 内存持久化（模拟落盘，可断言写入）。 */
function makePersistence(init: Array<{ personaId: string; record: DriverRecord }> = []) {
  const saved: Array<{ personaId: string; record: DriverRecord }> = [...init]
  const removed: Array<{ personaId: string; driverId: string }> = []
  const p: DriverPersistence = {
    async load() {
      return saved
    },
    async save(personaId, record) {
      saved.push({ personaId, record })
    },
    async remove(personaId, driverId) {
      removed.push({ personaId, driverId })
    },
  }
  return { p, saved, removed }
}

describe("DriverRegistry：静态与动态共存", () => {
  test("静态定义可用", () => {
    const reg = new DriverRegistry({ statics: [{ personaId: "xiaoshuang", def: def("alerts", ["schedule"]) }] })
    expect(reg.drivers("xiaoshuang").map((d) => d.id)).toEqual(["alerts"])
    expect(reg.get("xiaoshuang", "alerts")!.origin).toBe("static")
  })

  test("动态创建后与静态共存，静态排在前面", async () => {
    const reg = new DriverRegistry({ statics: [{ personaId: "xiaoshuang", def: def("alerts", ["schedule"]) }] })
    const r = await reg.create({
      personaId: "xiaoshuang",
      id: "tsdb",
      focus: "盯 TSDB 慢查询",
      subscriptions: ["message"],
      reason: "临时专项排查",
    })
    expect(r.ok).toBe(true)
    expect(reg.drivers("xiaoshuang").map((d) => d.id)).toEqual(["alerts", "tsdb"])
    expect(reg.get("xiaoshuang", "tsdb")!.origin).toBe("dynamic")
    expect(reg.get("xiaoshuang", "tsdb")!.reason).toBe("临时专项排查")
  })

  test("盘上的同 id 记录不覆盖手写静态配置（手写才是权威）", async () => {
    const { p } = makePersistence([
      { personaId: "xiaoshuang", record: { def: def("alerts", ["*"]), origin: "dynamic", createdAt: 1, reason: "旧记录" } },
    ])
    const reg = new DriverRegistry({ statics: [{ personaId: "xiaoshuang", def: def("alerts", ["schedule"]) }], persistence: p })
    await reg.init()
    // 仍是静态那份（订阅 schedule，不是 *）
    expect(reg.get("xiaoshuang", "alerts")!.def.subscriptions).toEqual(["schedule"])
    expect(reg.get("xiaoshuang", "alerts")!.origin).toBe("static")
  })

  test("启动时加载盘上的动态 driver", async () => {
    const { p } = makePersistence([
      { personaId: "xiaoshuang", record: { def: def("tsdb", ["message"]), origin: "dynamic", createdAt: 1, reason: "上次建的" } },
    ])
    const reg = new DriverRegistry({ persistence: p })
    await reg.init()
    expect(reg.drivers("xiaoshuang").map((d) => d.id)).toEqual(["tsdb"])
  })
})

describe("DriverRegistry：动态创建必须落盘", () => {
  test("创建后写入持久化", async () => {
    const { p, saved } = makePersistence()
    const reg = new DriverRegistry({ persistence: p })
    await reg.create({ personaId: "xiaoshuang", id: "tsdb", focus: "盯 TSDB", subscriptions: ["message"], reason: "专项" })
    expect(saved).toHaveLength(1)
    expect(saved[0].record.def.id).toBe("tsdb")
    expect(saved[0].record.reason).toBe("专项")
  })

  test("持久化失败不回滚内存（但记日志）——重启后以盘为准", async () => {
    const logs: string[] = []
    const reg = new DriverRegistry({
      persistence: {
        async load() {
          return []
        },
        async save() {
          throw new Error("磁盘满")
        },
        async remove() {},
      },
      log: { info: () => {}, warn: (m) => logs.push(m) },
    })
    const r = await reg.create({ personaId: "p", id: "x", focus: "y", subscriptions: ["*"], reason: "z" })
    expect(r.ok).toBe(true) // 内存已生效
    expect(reg.drivers("p").map((d) => d.id)).toEqual(["x"])
    expect(logs.some((l) => l.includes("磁盘满"))).toBe(true)
  })

  test("下线动态 driver 同时移除持久化", async () => {
    const { p, removed } = makePersistence()
    const reg = new DriverRegistry({ persistence: p })
    await reg.create({ personaId: "p", id: "tmp", focus: "f", subscriptions: ["*"], reason: "r" })
    expect((await reg.remove("p", "tmp")).ok).toBe(true)
    expect(reg.drivers("p")).toHaveLength(0)
    expect(removed).toEqual([{ personaId: "p", driverId: "tmp" }])
  })

  test("静态 driver 不能运行时下线（改配置才是对的做法）", async () => {
    const reg = new DriverRegistry({ statics: [{ personaId: "p", def: def("alerts", ["schedule"]) }] })
    const r = await reg.remove("p", "alerts")
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toContain("静态定义")
    expect(reg.drivers("p")).toHaveLength(1)
  })
})

describe("validateCreate：校验从严", () => {
  const base = { personaId: "p", id: "x", focus: "f", subscriptions: ["*"], reason: "r" }

  test("合法入参返回 null", () => {
    expect(validateCreate(base, undefined)).toBeNull()
  })

  test("缺 reason 被拒（审计必需）", () => {
    expect(validateCreate({ ...base, reason: "" }, undefined)).toContain("reason")
  })

  test("空 subscriptions 被拒（不关注任何事件 = 永远不被唤醒）", () => {
    expect(validateCreate({ ...base, subscriptions: [] }, undefined)).toContain("subscriptions 不能为空")
  })

  test("订阅语法非法被拒（写错 = 永远收不到事件）", () => {
    expect(validateCreate({ ...base, subscriptions: ["feishu*"] }, undefined)).toContain("非法")
    expect(validateCreate({ ...base, subscriptions: ["a:b:c"] }, undefined)).toContain("段数")
  })

  test("id 与已有 driver 冲突被拒（防意外覆盖正在工作的分工）", () => {
    const existing = new Map([["x", { def: def("x", ["*"]), origin: "static" as const, createdAt: 1 }]])
    expect(validateCreate(base, existing)).toContain("已存在")
  })

  test("id 格式非法被拒", () => {
    expect(validateCreate({ ...base, id: "Bad-Id" }, undefined)).toContain("[a-z0-9_]+")
  })

  test("数量上限", () => {
    const existing = new Map<string, DriverRecord>()
    for (let i = 0; i < 16; i++) {
      existing.set(`d${i}`, { def: def(`d${i}`, ["*"]), origin: "dynamic", createdAt: i })
    }
    expect(validateCreate(base, existing)).toContain("上限")
  })

  test("maxConcurrent 越界被拒", () => {
    expect(validateCreate({ ...base, maxConcurrent: 0 }, undefined)).toContain("maxConcurrent")
    expect(validateCreate({ ...base, maxConcurrent: 99 }, undefined)).toContain("maxConcurrent")
  })
})

describe("动态 driver 立即参与订阅匹配", () => {
  /** 造一个接 DriverRegistry 的 lookup。 */
  function makeLookup(reg: DriverRegistry) {
    const lookup: PersonaLookup = {
      get: (id) => (id === "xiaoshuang" ? IDENT : null),
      drivers: (personaId) => reg.drivers(personaId),
      subscribers: (e) => {
        const coords = coordsOf(e)
        const hit = reg.drivers("xiaoshuang").filter((d) => matchesAny(d.subscriptions, coords)).map((d) => d.id)
        return hit.length ? [{ personaId: "xiaoshuang", user: "admin", driverIds: hit }] : []
      },
      maxConcurrent: () => 4,
    }
    return lookup
  }

  function makeRuntime(reg: DriverRegistry) {
    const runs: string[] = []
    const runner: PersonaRunner = {
      isRunning: () => false,
      run: async (input) => {
        runs.push(`${input.driver.id}:${input.events.map((e) => e.id).join(",")}`)
      },
    }
    const sessions: SessionResolver = {
      resolveDriver: async ({ personaId, driverId }) => ({
        sessionId: `${personaId}:${driverId}`.padEnd(32, "0").slice(0, 32),
        created: true,
        name: `${personaId}·${driverId}`,
      }),
    }
    const rt = new PersonaRuntime({ lookup: makeLookup(reg), runner, sessions })
    rt.start()
    return { rt, runs }
  }

  const ev = (id: string, kind: "message" | "schedule" = "message") => ({
    id,
    kind,
    personaId: "",
    channel: kind === "message" ? "feishu" : "schedule",
    context: { source: "s", summary: "m" },
    body: "b",
    receivedAt: Date.now(),
  })

  test("新建的 driver 立刻能收到符合订阅的事件", async () => {
    const reg = new DriverRegistry({ statics: [{ personaId: "xiaoshuang", def: def("alerts", ["schedule"]) }] })
    const { rt, runs } = makeRuntime(reg)

    // 此时 message 事件无人关注
    expect((await rt.dispatch(ev("m1"))).status).toBe("unrouted")

    // 建一个关注 message 的 driver
    await reg.create({ personaId: "xiaoshuang", id: "support", focus: "答支持群", subscriptions: ["message"], reason: "支持群问题堆积" })
    rt.syncDriver("xiaoshuang", "support", "online")

    // 下一条 message 就投给它
    const r = await rt.dispatch(ev("m2"))
    expect(r.status).toBe("accepted")
    expect(r.deliveries!.map((d) => d.driverId)).toEqual(["support"])
    await new Promise((r) => setTimeout(r, 5))
    expect(runs).toContain("support:m2")
  })

  test("下线后不再收到事件", async () => {
    const reg = new DriverRegistry()
    const { rt, runs } = makeRuntime(reg)
    await reg.create({ personaId: "xiaoshuang", id: "tmp", focus: "临时", subscriptions: ["message"], reason: "试试" })
    rt.syncDriver("xiaoshuang", "tmp", "online")
    expect((await rt.dispatch(ev("m1"))).status).toBe("accepted")

    await reg.remove("xiaoshuang", "tmp")
    rt.syncDriver("xiaoshuang", "tmp", "offline")
    expect((await rt.dispatch(ev("m2"))).status).toBe("unrouted")
    await new Promise((r) => setTimeout(r, 5))
    expect(runs).toContain("tmp:m1")
    expect(runs).not.toContain("tmp:m2")
  })

  test("静态与动态 driver 订阅同一类目时都收到", async () => {
    const reg = new DriverRegistry({ statics: [{ personaId: "xiaoshuang", def: def("main", ["message"]) }] })
    const { rt } = makeRuntime(reg)
    await reg.create({ personaId: "xiaoshuang", id: "audit", focus: "审计", subscriptions: ["message"], reason: "合规要求" })
    rt.syncDriver("xiaoshuang", "audit", "online")
    const r = await rt.dispatch(ev("m1"))
    expect(r.deliveries!.map((d) => d.driverId).sort()).toEqual(["audit", "main"])
  })
})
