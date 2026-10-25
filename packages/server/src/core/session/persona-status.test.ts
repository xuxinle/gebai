/**
 * 运行态快照：把散在五处的「事实」汇成一份可读视图。
 *
 * 这组用例的重点不是「字段能不能读出来」，而是**难排查的那几种组合能不能被看见**：
 *
 * | 现象 | 快照里应该长什么样 |
 * |---|---|
 * | 配了人格但一个事件源都没起 | `sources` 空 / 全 `started:false` |
 * | 事件源在收但没人订阅（channel 没被认领） | `stats.unattended` 增长 |
 * | 分身建了但从未接过事件 | `sessionId: null` + `active: false` |
 * | 调度器说在跑但引擎说没跑 | `sessionRunning` 以引擎为准（调度计数会滞后） |
 *
 * 另有一条刻意的取舍要在用例里钉住：**不返回令牌明文**。
 */
import { describe, expect, test } from "bun:test"
import { DriverRegistry, PersonaCatalog } from "@gebai/persona"
import type { PersonaDef } from "@gebai/persona"
import { collectPersonaState } from "./persona-status"

const DEF: PersonaDef = {
  id: "xiaoshuang",
  name: "小霜",
  identity: "你是小霜。",
  role: "值班工程师",
  description: "值班助手",
  drivers: [
    { id: "support", focus: "答支持群", subscriptions: ["message"] },
    { id: "alerts", focus: "盯告警", subscriptions: ["webhook"] },
  ],
}

function catalog(drivers?: DriverRegistry) {
  return new PersonaCatalog({ personas: new Map([["xiaoshuang", DEF]]), ...(drivers ? { drivers } : {}) })
}

/** 调度器状态替身（只实现 `personaStates`/`bindings`）。 */
function fakeRuntime(opts: {
  states?: Array<Record<string, unknown>>
  bindings?: Array<{ personaId: string; driverId: string; sessionId: string }>
}) {
  return {
    personaStates: () => opts.states ?? [],
    bindings: () => opts.bindings ?? [],
  } as never
}

describe("collectPersonaState：基本形状", () => {
  test("人格 / 分身 / 来源 / 订阅全部带出", async () => {
    const snap = await collectPersonaState({
      catalog: catalog(),
      runtime: fakeRuntime({}),
      drivers: new DriverRegistry({}),
      sessionRunning: () => false,
    })
    expect(snap.enabled).toBe(true)
    expect(snap.personas.length).toBe(1)
    const p = snap.personas[0]!
    expect(p.id).toBe("xiaoshuang")
    expect(p.name).toBe("小霜")
    expect(p.description).toBe("值班助手")
    expect(p.drivers.map((d) => d.id)).toEqual(["support", "alerts"])
    expect(p.drivers[0]!.subscriptions).toEqual(["message"])
    expect(p.drivers[0]!.origin).toBe("static")
    expect(snap.generatedAt).toBeGreaterThan(0)
  })

  test("从未接过事件的分身：active=false、sessionId=null、计数全 0（这本身就是可用信号）", async () => {
    const snap = await collectPersonaState({
      catalog: catalog(),
      runtime: fakeRuntime({}),
      drivers: new DriverRegistry({}),
      sessionRunning: () => false,
    })
    for (const d of snap.personas[0]!.drivers) {
      expect(d.active).toBe(false)
      expect(d.sessionId).toBeNull()
      expect(d.sessionRunning).toBe(false)
      expect(d.accepted).toBe(0)
      expect(d.queued).toBe(0)
    }
    expect(snap.totals).toEqual({ drivers: 2, activeDrivers: 0, running: 0, queued: 0, failed: 0, workersRunning: 0 })
  })
})

describe("collectPersonaState：调度与引擎的口径", () => {
  test("调度计数与会话归属合流；sessionRunning 取引擎判定（不是调度计数）", async () => {
    const snap = await collectPersonaState({
      catalog: catalog(),
      runtime: fakeRuntime({
        states: [
          { personaId: "xiaoshuang", driverId: "support", runningSessions: 1, queued: 3, accepted: 9, deduped: 1, overflowed: 0, failed: 2, sessions: [], maxConcurrent: 2 },
        ] as never,
        bindings: [{ personaId: "xiaoshuang", driverId: "support", sessionId: "s1" }],
      }),
      drivers: new DriverRegistry({}),
      // 引擎说这个会话此刻**没在跑**（例如任务刚结束、调度计数还没回落）
      sessionRunning: () => false,
    })
    const support = snap.personas[0]!.drivers.find((d) => d.id === "support")!
    expect(support.active).toBe(true)
    expect(support.sessionId).toBe("s1")
    expect(support.accepted).toBe(9)
    expect(support.queued).toBe(3)
    expect(support.failed).toBe(2)
    // 关键：两个来源给的是同一件事的两个视角，快照不把它们混成一个字段
    expect(support.running).toBe(1)
    expect(support.sessionRunning).toBe(false)
    expect(snap.totals).toEqual({ drivers: 2, activeDrivers: 1, running: 0, queued: 3, failed: 2, workersRunning: 0 })
  })

  test("引擎判定抛错 → 当 false（快照本身不能因为一个探测失败而整份挂掉）", async () => {
    const snap = await collectPersonaState({
      catalog: catalog(),
      runtime: fakeRuntime({ bindings: [{ personaId: "xiaoshuang", driverId: "support", sessionId: "s1" }] }),
      drivers: new DriverRegistry({}),
      sessionRunning: () => {
        throw new Error("引擎已销毁")
      },
    })
    expect(snap.personas[0]!.drivers.find((d) => d.id === "support")!.sessionRunning).toBe(false)
  })

  test("runtime 没有 bindings（替身/旧实现）→ 不抛错，sessionId 全 null", async () => {
    const snap = await collectPersonaState({
      catalog: catalog(),
      runtime: { personaStates: () => [] } as never,
      drivers: new DriverRegistry({}),
      sessionRunning: () => false,
    })
    expect(snap.personas[0]!.drivers.every((d) => d.sessionId === null)).toBe(true)
  })
})

describe("collectPersonaState：动态 driver 与审计线索", () => {
  test("模型自建的分身标 dynamic，并带出 reason（事后回答「这条分工怎么来的」）", async () => {
    const registry = new DriverRegistry({})
    await registry.create({
      personaId: "xiaoshuang",
      id: "tsdb_watch",
      focus: "盯 TSDB 指标",
      subscriptions: ["webhook"],
      reason: "这次专项排查我单开一个盯着",
    })
    const snap = await collectPersonaState({
      catalog: catalog(registry),
      runtime: fakeRuntime({}),
      drivers: registry,
      sessionRunning: () => false,
    })
    const d = snap.personas[0]!.drivers.find((x) => x.id === "tsdb_watch")!
    expect(d.origin).toBe("dynamic")
    expect(d.reason).toBe("这次专项排查我单开一个盯着")
    expect(snap.totals.drivers).toBe(3) // 2 静态 + 1 动态
  })
})

describe("collectPersonaState：事件源与渠道入站", () => {
  const sourceMgr = {
    state: () => [
      {
        name: "file_drop",
        channel: "filedrop",
        description: "文件投递",
        enabled: true,
        started: true,
        stats: { emitted: 5, accepted: 3, unattended: 2, duplicate: 0, overflow: 0, unavailable: 0 },
      },
      {
        name: "feishu",
        channel: "feishu",
        description: "飞书",
        enabled: true,
        started: false,
        error: "启动失败: 长连接握手超时",
        stats: { emitted: 0, accepted: 0, unattended: 0, duplicate: 0, overflow: 0, unavailable: 0 },
      },
    ],
    loadErrors: () => ({ broken: "[custom] 未导出 source" }),
  } as never

  test("源状态与失败清单并列给出（「配了但没反应」要靠这两项一起看）", async () => {
    const snap = await collectPersonaState({
      catalog: catalog(),
      runtime: fakeRuntime({}),
      drivers: new DriverRegistry({}),
      sources: sourceMgr,
      sessionRunning: () => false,
    })
    expect(snap.sources.map((s) => s.name)).toEqual(["file_drop", "feishu"])
    expect(snap.sources[1]!.error).toContain("握手超时")
    expect(snap.sources[0]!.stats.unattended).toBe(2) // 无人订阅的可见信号
    expect(snap.sourceErrors).toEqual({ broken: "[custom] 未导出 source" })
  })

  test("未装配事件源 → 空数组，不抛错（关闭态与「配了但没有源」要能区分）", async () => {
    const snap = await collectPersonaState({
      catalog: catalog(),
      runtime: fakeRuntime({}),
      drivers: new DriverRegistry({}),
      sessionRunning: () => false,
    })
    expect(snap.sources).toEqual([])
    expect(snap.sourceErrors).toEqual({})
  })

  test("渠道入站计数原样带出", async () => {
    const snap = await collectPersonaState({
      catalog: catalog(),
      runtime: fakeRuntime({}),
      drivers: new DriverRegistry({}),
      channelInput: () => ({
        feishu: { emitted: 10, accepted: 8, unrouted: 2, duplicate: 0, overflow: 0, unavailable: 0 },
      }),
      sessionRunning: () => false,
    })
    expect(snap.channelInput.feishu!.unrouted).toBe(2)
  })
})

describe("collectPersonaState：凭据（不吐明文）", () => {
  test("ready 状态只带可展示字段", async () => {
    const snap = await collectPersonaState({
      catalog: catalog(),
      runtime: fakeRuntime({}),
      drivers: new DriverRegistry({}),
      credentials: {
        status: async () => ({
          ready: true,
          state: "ok",
          message: "已授权（张三）",
          authorizedUser: "张三",
          remainingMs: 3_600_000,
          needsAuth: false,
          accessToken: "u-secret-token", // 内核若带了敏感字段，快照**不转出**
        }),
      } as never,
      sessionRunning: () => false,
    })
    expect(snap.credentials).toEqual({ ready: true, state: "ok", message: "已授权（张三）", remainingMs: 3_600_000, needsAuth: false })
    expect(JSON.stringify(snap)).not.toContain("u-secret-token")
  })

  test("取状态抛错 → 如实记为不可用（不显示成 ready）", async () => {
    const snap = await collectPersonaState({
      catalog: catalog(),
      runtime: fakeRuntime({}),
      drivers: new DriverRegistry({}),
      credentials: {
        status: async () => {
          throw new Error("令牌文件损坏")
        },
      } as never,
      sessionRunning: () => false,
    })
    expect(snap.credentials?.ready).toBe(false)
    expect(snap.credentials?.state).toBe("error")
    expect(snap.credentials?.message).toContain("令牌文件损坏")
  })

  test("未装配凭据 → null（区别于「有凭据但不可用」）", async () => {
    const snap = await collectPersonaState({
      catalog: catalog(),
      runtime: fakeRuntime({}),
      drivers: new DriverRegistry({}),
      sessionRunning: () => false,
    })
    expect(snap.credentials).toBeNull()
  })
})
