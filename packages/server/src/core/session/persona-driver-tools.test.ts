/**
 * 人格自组织工具的服务端适配：**身份从会话归属来**，不从模型自报来。
 *
 * 这组用例守住的是一条信任边界：`driver_create` 是写操作（会新增一个常驻分身 + 落盘 + 参与订阅匹配），
 * 若模型能自己声明「我代表谁」，它就能替别的人格造/删分工。所以本文件重点验：
 *
 * 1. 普通会话（无 `SessionData.persona`）→ 明确拒绝，不落任何写
 * 2. 人格会话 → 身份取自会话归属，模型传什么 personaId 都不影响
 * 3. `onChange` 里 `syncDriver` 的作用（新建的分身必须立刻能被事件投到）——用替身录制调用
 */
import { describe, expect, test } from "bun:test"
import type { SessionData } from "./store"
import type { DriverRecord } from "@gebai/persona"
import { DriverRegistry } from "@gebai/persona"
import { personaContextResolver, personaOwnerOf, toEngineTools } from "./persona-driver-tools"

/** 最小 store 替身：只实现 `load`（适配层也只依赖它）。 */
function fakeStore(sessions: Record<string, SessionData | null>) {
  return {
    load: async (id: string) => sessions[id] ?? null,
  } as unknown as Parameters<typeof personaOwnerOf>[0]
}

function boundSession(personaId: string, driverId: string): SessionData {
  return {
    id: "s1",
    name: "小霜·support",
    userId: "admin",
    messages: [],
    todos: [],
    createdAt: 0,
    updatedAt: 0,
    persona: { personaId, driverId, boundAt: 1 },
  }
}

function plainSession(): SessionData {
  return { id: "s2", name: "随便聊聊", userId: "admin", messages: [], todos: [], createdAt: 0, updatedAt: 0 }
}

function fakeTool(name: string, record: unknown[] = []) {
  return {
    name,
    description: `${name} 的说明`,
    parameters: { type: "object" as const, properties: {} },
    execute: async (args: Record<string, unknown>, ctx: unknown) => {
      record.push({ args, ctx })
      return { output: "ok", data: { echoed: true } }
    },
  }
}

describe("personaOwnerOf：身份只从会话归属读", () => {
  test("人格会话 → 返回 persona/driver", async () => {
    const r = await personaOwnerOf(fakeStore({ s1: boundSession("xiaoshuang", "support") }), "s1", "admin")
    expect(r).toEqual({ personaId: "xiaoshuang", driverId: "support" })
  })

  test("普通会话 → null（没有 persona 字段）", async () => {
    expect(await personaOwnerOf(fakeStore({ s2: plainSession() }), "s2", "admin")).toBeNull()
  })

  test("会话不存在 / 读取抛错 → null（宁可少给能力，也不给错身份）", async () => {
    expect(await personaOwnerOf(fakeStore({}), "gone", "admin")).toBeNull()
    const throwing = {
      load: async () => Promise.reject(new Error("磁盘故障")),
    } as unknown as Parameters<typeof personaOwnerOf>[0]
    expect(await personaOwnerOf(throwing, "s1", "admin")).toBeNull()
  })

  test("空 sessionId → null（不拿空串去撞存储）", async () => {
    let called = false
    const store = {
      load: async () => {
        called = true
        return null
      },
    } as unknown as Parameters<typeof personaOwnerOf>[0]
    expect(await personaOwnerOf(store, "", "admin")).toBeNull()
    expect(called).toBe(false)
  })
})

describe("toEngineTools：普通会话拒绝、人格会话放行", () => {
  const deps = () => ({
    registry: new DriverRegistry({}),
    store: fakeStore({ s1: boundSession("xiaoshuang", "support"), s2: plainSession() }),
    runtime: { syncDriver: () => {} },
  })

  test("普通会话：返回可读的拒绝原因，且**不调用内核工具**", async () => {
    const calls: unknown[] = []
    const [tool] = toEngineTools([fakeTool("driver_create", calls)], deps())
    const r = await tool!.execute({}, { sessionId: "s2", user: "admin" } as never)
    expect(r.output).toContain("不属于任何人格")
    expect(calls.length).toBe(0) // 关键：写操作一次都没发生
  })

  test("人格会话：把会话归属作为身份传给内核（模型参数无法覆盖）", async () => {
    const calls: unknown[] = []
    const [tool] = toEngineTools([fakeTool("driver_create", calls)], deps())
    await tool!.execute({ personaId: "另一个角色" }, { sessionId: "s1", user: "admin" } as never)
    expect(calls.length).toBe(1)
    const c = calls[0] as { ctx: { personaId: string; sessionId: string } }
    expect(c.ctx.personaId).toBe("xiaoshuang") // 会话归属胜出
    expect(c.ctx.sessionId).toBe("s1")
  })

  test("结构化输出（data）透传（js 编排依赖它）", async () => {
    const [tool] = toEngineTools([fakeTool("driver_list")], deps())
    const r = await tool!.execute({}, { sessionId: "s1", user: "admin" } as never)
    expect(r.data).toEqual({ echoed: true })
  })

  test("描述带「仅人格会话可用」前缀（全局工具，普通会话也看得见 → 必须自己说清楚）", () => {
    const [tool] = toEngineTools([fakeTool("driver_list")], deps())
    expect(tool!.description.startsWith("【仅人格会话可用】")).toBe(true)
  })

  test("requireApproval 透传（部署方收紧时三个工具都要审）", () => {
    const tools = toEngineTools([fakeTool("driver_create"), fakeTool("driver_retire")], deps(), { requireApproval: true })
    expect(tools.every((t) => t.requiresApproval === true)).toBe(true)
  })
})

describe("personaContextResolver：内核工具的 resolveContext", () => {
  test("有 personaId → 返回身份（并带上 sessionId 供审计）", () => {
    expect(personaContextResolver({ personaId: "xiaoshuang", sessionId: "s1" })).toEqual({
      personaId: "xiaoshuang",
      sessionId: "s1",
    })
  })

  test("缺 personaId / 非对象 → null", () => {
    expect(personaContextResolver({ sessionId: "s1" })).toBeNull()
    expect(personaContextResolver(undefined)).toBeNull()
    expect(personaContextResolver({ personaId: "  " })).toBeNull()
  })
})

describe("driver 注册表与适配层协同", () => {
  test("真实内核 create + onChange：syncDriver 收到 online", async () => {
    const { createDriverTools } = await import("@gebai/persona")
    const registry = new DriverRegistry({})
    const synced: Array<{ personaId: string; driverId: string; action: string }> = []
    const tools = createDriverTools({
      registry,
      resolveContext: personaContextResolver,
      onChange: (c) => synced.push({ ...c }),
    })
    const adapted = toEngineTools(tools, {
      registry,
      store: fakeStore({ s1: boundSession("xiaoshuang", "support") }),
      runtime: { syncDriver: () => {} },
    })
    const create = adapted.find((t) => t.name === "driver_create")!
    const r = await create.execute(
      { id: "tsdb_watch", focus: "盯 TSDB 指标", subscriptions: ["webhook"], reason: "专项排查" },
      { sessionId: "s1", user: "admin" } as never,
    )
    expect(r.output).toContain("已创建分身")
    expect(registry.get("xiaoshuang", "tsdb_watch")?.origin).toBe("dynamic")
    expect(synced).toEqual([{ action: "created", personaId: "xiaoshuang", driverId: "tsdb_watch" }])
  })

  test("静态 driver 不能下线（权威是手写配置）→ 适配层如实转出内核的拒绝", async () => {
    const { createDriverTools } = await import("@gebai/persona")
    const statics: Array<{ personaId: string; def: DriverRecord["def"] }> = [
      { personaId: "xiaoshuang", def: { id: "support", focus: "答支持群", subscriptions: ["message"] } },
    ]
    const registry = new DriverRegistry({ statics })
    const tools = createDriverTools({ registry, resolveContext: personaContextResolver })
    const adapted = toEngineTools(tools, {
      registry,
      store: fakeStore({ s1: boundSession("xiaoshuang", "support") }),
      runtime: { syncDriver: () => {} },
    })
    const retire = adapted.find((t) => t.name === "driver_retire")!
    const r = await retire.execute({ id: "support" }, { sessionId: "s1", user: "admin" } as never)
    expect(r.output).toContain("静态定义")
    expect(registry.get("xiaoshuang", "support")).not.toBeNull()
  })

  test("driver_list：列出静态与动态并标出来源", async () => {
    const { createDriverListTool } = await import("@gebai/persona")
    const registry = new DriverRegistry({
      statics: [{ personaId: "xiaoshuang", def: { id: "support", focus: "答支持群", subscriptions: ["message"] } }],
    })
    const list = toEngineTools([createDriverListTool({ registry, resolveContext: personaContextResolver })], {
      registry,
      store: fakeStore({ s1: boundSession("xiaoshuang", "support") }),
      runtime: { syncDriver: () => {} },
    })[0]!
    await registry.create({ personaId: "xiaoshuang", id: "tsdb", focus: "盯 TSDB", subscriptions: ["webhook"], reason: "专项" })
    const r = await list.execute({}, { sessionId: "s1", user: "admin" } as never)
    expect(r.output).toContain("support")
    expect(r.output).toContain("tsdb")
    expect(r.output).toContain("动态") // 来源标注存在
  })
})
