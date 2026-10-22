/**
 * Driver 管理工具用例：角色**运行时自我组织**。
 *
 * 验收要点（每条对应一处真实风险）：
 * 1. 创建后**立即生效**——注册表变了但调度器不知道 = 「创建成功」是谎话
 * 2. 静态不能被删——手写配置是权威，否则状态来源不可推理
 * 3. 创建必须给 reason——否则一个月后无法判断哪些分工还有用
 * 4. 身份由宿主给定，不由模型声明——否则能越权操作别的角色
 * 5. 空订阅被拒——不关注事件的 driver 永远不醒，等于白建
 */
import { describe, expect, test } from "bun:test"
import { DriverRegistry } from "./driver"
import { createDriverListTool, createDriverTools, type DriverToolContext } from "./driver-tools"
import { PersonaRuntime } from "./runtime"
import { coordsOf, matchesAny } from "./subscription"
import type { DriverDef, PersonaIdentity, PersonaLookup, PersonaRunner, SessionResolver } from "./ports"

const IDENT: PersonaIdentity = { id: "xiaoshuang", name: "小霜", identity: "你是小霜", role: "技术响应" }

function def(id: string, subscriptions: string[]): DriverDef {
  return { id, focus: `负责 ${id}`, subscriptions }
}

/** 模拟宿主上下文（真实环境是 ToolContext）。 */
function makeResolveContext(personaId = "xiaoshuang") {
  return (raw: unknown): DriverToolContext | null => {
    const c = raw as { personaId?: string; sessionId?: string } | null
    if (!c) return null
    if (c.personaId !== personaId) return null
    return { personaId, ...(c.sessionId ? { sessionId: c.sessionId } : {}) }
  }
}

const HOST_CTX = { personaId: "xiaoshuang", sessionId: "s".repeat(32) }

function setup(opts: { statics?: Array<{ personaId: string; def: DriverDef }>; withRuntime?: boolean } = {}) {
  const registry = new DriverRegistry({ statics: opts.statics ?? [] })
  const changes: Array<{ action: string; personaId: string; driverId: string }> = []
  const tools = createDriverTools({
    registry,
    resolveContext: makeResolveContext(),
    onChange: (c) => changes.push(c),
  })
  const listTool = createDriverListTool({ registry, resolveContext: makeResolveContext() })
  const create = tools.find((t) => t.name === "driver_create")!
  const retire = tools.find((t) => t.name === "driver_retire")!
  return { registry, tools, create, retire, listTool, changes }
}

describe("driver_create：创建分工", () => {
  test("成功创建并返回可读说明", async () => {
    const { create, registry, changes } = setup()
    const r = await create.execute(
      { id: "tsdb_watch", focus: "盯 TSDB 慢查询", subscriptions: ["message"], reason: "专项排查" },
      HOST_CTX,
    )
    expect(r.output).toContain("已创建分身")
    expect(r.output).toContain("下一个符合的事件就会投给它")
    expect(registry.drivers("xiaoshuang").map((d) => d.id)).toEqual(["tsdb_watch"])
    // onChange 触发（宿主据此同步调度器）
    expect(changes).toEqual([{ action: "created", personaId: "xiaoshuang", driverId: "tsdb_watch" }])
  })

  test("subscriptions 支持数组与逗号串两种形态", async () => {
    const { create, registry } = setup()
    await create.execute({ id: "a", focus: "f", subscriptions: ["message", "schedule"], reason: "r" }, HOST_CTX)
    await create.execute({ id: "b", focus: "f", subscriptions: "message, schedule", reason: "r" }, HOST_CTX)
    expect(registry.get("xiaoshuang", "a")!.def.subscriptions).toEqual(["message", "schedule"])
    expect(registry.get("xiaoshuang", "b")!.def.subscriptions).toEqual(["message", "schedule"])
  })

  test("空订阅被拒（永不唤醒的分身）", async () => {
    const { create, registry } = setup()
    const r = await create.execute({ id: "x", focus: "f", subscriptions: [], reason: "r" }, HOST_CTX)
    expect(r.output).toContain("不能为空")
    expect(registry.drivers("xiaoshuang")).toHaveLength(0)
  })

  test("缺 reason 被拒（审计必需）", async () => {
    const { create, registry } = setup()
    const r = await create.execute({ id: "x", focus: "f", subscriptions: ["*"] }, HOST_CTX)
    expect(r.output).toContain("缺少 reason")
    expect(registry.drivers("xiaoshuang")).toHaveLength(0)
  })

  test("id 冲突被拒（防意外覆盖正在工作的分工）", async () => {
    const { create } = setup({ statics: [{ personaId: "xiaoshuang", def: def("alerts", ["schedule"]) }] })
    const r = await create.execute({ id: "alerts", focus: "f", subscriptions: ["*"], reason: "r" }, HOST_CTX)
    expect(r.output).toContain("已存在")
  })

  test("身份由宿主给定：无上下文时拒绝（不允许模型自己声明角色）", async () => {
    const { create } = setup()
    const r = await create.execute({ id: "x", focus: "f", subscriptions: ["*"], reason: "r" }, null)
    expect(r.output).toContain("无法确定所属角色")
  })

  test("上下文属于别的角色 → 拒绝（防越权）", async () => {
    const { create, registry } = setup()
    const r = await create.execute(
      { id: "x", focus: "f", subscriptions: ["*"], reason: "r" },
      { personaId: "laowang" },
    )
    expect(r.output).toContain("无法确定所属角色")
    expect(registry.drivers("laowang")).toHaveLength(0)
  })

  test("max_concurrent 透传", async () => {
    const { create, registry } = setup()
    await create.execute({ id: "x", focus: "f", subscriptions: ["*"], reason: "r", max_concurrent: 3 }, HOST_CTX)
    expect(registry.get("xiaoshuang", "x")!.def.maxConcurrent).toBe(3)
  })
})

describe("driver_retire：撤销分工", () => {
  test("撤销动态 driver", async () => {
    const { create, retire, registry, changes } = setup()
    await create.execute({ id: "tmp", focus: "临时", subscriptions: ["*"], reason: "试试" }, HOST_CTX)
    const r = await retire.execute({ id: "tmp", reason: "不需要了" }, HOST_CTX)
    expect(r.output).toContain("已撤销")
    expect(r.output).toContain("不需要了")
    expect(registry.drivers("xiaoshuang")).toHaveLength(0)
    expect(changes.at(-1)).toMatchObject({ action: "removed", driverId: "tmp" })
  })

  test("静态 driver 不能撤销（改配置才是对的做法）", async () => {
    const { retire, registry } = setup({ statics: [{ personaId: "xiaoshuang", def: def("alerts", ["schedule"]) }] })
    const r = await retire.execute({ id: "alerts" }, HOST_CTX)
    expect(r.output).toContain("静态定义")
    expect(registry.drivers("xiaoshuang")).toHaveLength(1)
  })

  test("撤销不存在的 driver 给出明确提示", async () => {
    const { retire } = setup()
    const r = await retire.execute({ id: "nope" }, HOST_CTX)
    expect(r.output).toContain("没有找到")
  })
})

describe("driver_list：先看见再决定", () => {
  test("列出静态与动态，标出来源与原因", async () => {
    const { create, listTool } = setup({ statics: [{ personaId: "xiaoshuang", def: def("alerts", ["schedule"]) }] })
    await create.execute({ id: "tsdb", focus: "盯 TSDB", subscriptions: ["message"], reason: "专项排查" }, HOST_CTX)
    const r = await listTool.execute({}, HOST_CTX)
    expect(r.output).toContain("共 2 个工作分身")
    expect(r.output).toContain("静态（部署配置）")
    expect(r.output).toContain("动态（自己创建）")
    expect(r.output).toContain("专项排查")
  })

  test("空列表给出可执行的下一步", async () => {
    const { listTool } = setup()
    const r = await listTool.execute({}, HOST_CTX)
    expect(r.output).toContain("driver_create")
  })
})

describe("创建后立即生效（端到端）", () => {
  /** 这是最关键的验收：注册表变了但调度器不知道 = 「创建成功」是谎话。 */
  test("新 driver 立刻能收到符合订阅的事件", async () => {
    const registry = new DriverRegistry()
    const runs: string[] = []
    const lookup: PersonaLookup = {
      get: (id) => (id === "xiaoshuang" ? IDENT : null),
      drivers: (p) => registry.drivers(p),
      subscribers: (e) => {
        const coords = coordsOf(e)
        const hit = registry.drivers("xiaoshuang").filter((d) => matchesAny(d.subscriptions, coords)).map((d) => d.id)
        return hit.length ? [{ personaId: "xiaoshuang", user: "admin", driverIds: hit }] : []
      },
      maxConcurrent: () => 4,
    }
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
    const rt = new PersonaRuntime({ lookup, runner, sessions })
    rt.start()

    // 工具接上运行时同步（真实装配形态）
    const tools = createDriverTools({
      registry,
      resolveContext: makeResolveContext(),
      onChange: (c) => rt.syncDriver(c.personaId, c.driverId, c.action === "created" ? "online" : "offline"),
    })
    const create = tools.find((t) => t.name === "driver_create")!

    const mkEvent = (id: string) => ({
      id,
      kind: "message" as const,
      personaId: "",
      channel: "feishu",
      context: { source: "飞书群", summary: "提问", refs: { chatId: "oc_1" } },
      body: "帮我看看",
      receivedAt: Date.now(),
    })

    // 创建前：无人关注 → unrouted
    expect((await rt.dispatch(mkEvent("m1"))).status).toBe("unrouted")

    // 创建关注 message 的分身
    const r = await create.execute({ id: "support", focus: "答支持群", subscriptions: ["message"], reason: "问题堆积" }, HOST_CTX)
    expect(r.output).toContain("已创建")

    // 立即生效：下一个事件就投给它
    const res = await rt.dispatch(mkEvent("m2"))
    expect(res.status).toBe("accepted")
    expect(res.deliveries!.map((d) => d.driverId)).toEqual(["support"])
    await new Promise((r) => setTimeout(r, 5))
    expect(runs).toContain("support:m2")
  })

  test("撤销后立刻停止接收", async () => {
    const registry = new DriverRegistry()
    const runs: string[] = []
    const lookup: PersonaLookup = {
      get: (id) => (id === "xiaoshuang" ? IDENT : null),
      drivers: (p) => registry.drivers(p),
      subscribers: (e) => {
        const coords = coordsOf(e)
        const hit = registry.drivers("xiaoshuang").filter((d) => matchesAny(d.subscriptions, coords)).map((d) => d.id)
        return hit.length ? [{ personaId: "xiaoshuang", user: "admin", driverIds: hit }] : []
      },
      maxConcurrent: () => 4,
    }
    const rt = new PersonaRuntime({
      lookup,
      runner: {
        isRunning: () => false,
        run: async (input) => {
          runs.push(`${input.driver.id}:${input.events.map((e) => e.id).join(",")}`)
        },
      },
      sessions: {
        resolveDriver: async ({ personaId, driverId }) => ({
          sessionId: `${personaId}:${driverId}`.padEnd(32, "0").slice(0, 32),
          created: true,
          name: "x",
        }),
      },
    })
    rt.start()
    const tools = createDriverTools({
      registry,
      resolveContext: makeResolveContext(),
      onChange: (c) => rt.syncDriver(c.personaId, c.driverId, c.action === "created" ? "online" : "offline"),
    })
    const create = tools.find((t) => t.name === "driver_create")!
    const retire = tools.find((t) => t.name === "driver_retire")!
    const mkEvent = (id: string) => ({
      id,
      kind: "message" as const,
      personaId: "",
      channel: "feishu",
      context: { source: "s", summary: "m" },
      body: "b",
      receivedAt: Date.now(),
    })

    await create.execute({ id: "tmp", focus: "f", subscriptions: ["message"], reason: "r" }, HOST_CTX)
    expect((await rt.dispatch(mkEvent("m1"))).status).toBe("accepted")

    await retire.execute({ id: "tmp", reason: "不再需要" }, HOST_CTX)
    expect((await rt.dispatch(mkEvent("m2"))).status).toBe("unrouted")
    await new Promise((r) => setTimeout(r, 5))
    expect(runs).toContain("tmp:m1")
    expect(runs).not.toContain("tmp:m2")
  })
})

describe("工具元数据", () => {
  test("三个工具名符合 snake_case 规范", () => {
    const { tools, listTool } = setup()
    for (const t of [...tools, listTool]) {
      expect(t.name).toMatch(/^[a-z][a-z0-9_]*$/)
      expect(t.parameters.type).toBe("object")
    }
  })

  test("默认不审批（否则自我组织退化成每步问人）", () => {
    const { create } = setup()
    expect(create.requiresApproval).toBeUndefined()
  })

  test("可配置为需审批", () => {
    const registry = new DriverRegistry()
    const tools = createDriverTools({ registry, resolveContext: makeResolveContext(), requireApproval: true })
    expect(tools.every((t) => t.requiresApproval === true)).toBe(true)
  })

  test("driver_create 的必填参数齐全", () => {
    const { create } = setup()
    expect(create.parameters.required).toEqual(["id", "focus", "subscriptions", "reason"])
  })
})
