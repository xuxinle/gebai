import { describe, expect, test } from "bun:test"
import { randomUUID } from "node:crypto"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { Task } from "@gebai/sdk"
import { EventBus } from "../base/event-bus"
import type { AgentEngine } from "../engine/engine"
import { Sandbox } from "../security/sandbox"
import { EnvManager } from "../session/env"
import { SessionStore } from "../session/store"
import { TaskManager, type TaskManagerDeps } from "./tasks"

/**
 * 闲时任务的**执行时间段**（DESIGN「统一任务管理」）：把"什么时候算闲时"
 * 从「队列空闲即可」细化到「同时落在 idle_window 内」。
 *
 * 为什么这些用例必须存在（都是会真实咬人的分界）：
 * - 时段外不能跑（否则"夜间跑"的设置形同虚设，白天照样抢额度和峰价 token）；
 * - 到了时段要能自己起来（不能在时段外被判"不可执行"后就再也不进候选了）；
 * - 手动执行不该被时段拦（用户点"执行"是明确意图，与"自动调度"不是一回事）；
 * - 队列视图要说清"为什么没跑"（否则用户只会看到"闲时任务没动静"）；
 * - 跨零点（22:00–06:00）是**最常见的用法**，边界必须准。
 */

interface Harness {
  home: string
  tasks: TaskManager
  clock: { t: number }
  runCalls: Array<{ sid: string; user: string; prompt: string }>
  results: string[]
  cleanup: () => Promise<void>
}

async function setup(opts: { now?: number; defaultIdleWindow?: (user: string) => Promise<{ start: string; end: string; days?: number[] } | undefined> } = {}): Promise<Harness> {
  const home = mkdtempSync(join(tmpdir(), "gebai-idlewin-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  const store = new SessionStore({ home })
  const sandbox = new Sandbox({ home, enabled: false })
  const env = new EnvManager(store)
  const events = new EventBus()
  const h: Harness = {
    home,
    tasks: null as unknown as TaskManager,
    clock: { t: opts.now ?? new Date(2026, 2, 4, 23, 0).getTime() }, // 周三 23:00（夜里）
    runCalls: [],
    results: [],
    cleanup: async () => rmSync(home, { recursive: true, force: true }),
  }
  events.subscribe((ev) => {
    if (ev.type === "event.task.result") h.results.push(String((ev.payload as { id?: unknown })?.id ?? ""))
  })
  sandbox.exec = (async () => ({ stdout: "", stderr: "", code: 0 })) as unknown as Sandbox["exec"]
  const engine = {
    isRunning: () => false,
    busyUser: () => false,
    cancel: async () => {},
    windDown: async () => {},
    run: async (sid: string, user: string, prompt: string) => {
      h.runCalls.push({ sid, user, prompt })
      await store.appendMessage(sid, { id: randomUUID(), role: "assistant", content: "完成", createdAt: h.clock.t }, user)
    },
    setTasks: () => {},
  } as unknown as AgentEngine
  const deps: TaskManagerDeps = {
    home,
    store,
    env,
    sandbox,
    events,
    engine,
    now: () => h.clock.t,
    tickIntervalMs: 3600_000,
  }
  h.tasks = new TaskManager(deps)
  if (opts.defaultIdleWindow) h.tasks.setDefaultIdleWindow(opts.defaultIdleWindow)
  await h.tasks.start()
  return h
}

const within = async (cond: () => boolean, timeoutMs = 3000): Promise<void> => {
  const t0 = Date.now()
  while (!cond()) {
    if (Date.now() - t0 > timeoutMs) throw new Error("等待超时")
    await new Promise((r) => setTimeout(r, 5))
  }
}

const entry = (h: Harness, id: string): Task => h.tasks["entries"].get(id)!

describe("闲时任务执行时间段", () => {
  test("时段内：照常执行", async () => {
    const h = await setup() // 周三 23:00
    try {
      const t = await h.tasks.add("default", {
        kind: "idle",
        runner: "prompt",
        prompt: "夜间活",
        idleWindow: { start: "22:00", end: "06:00" },
      })
      await within(() => h.runCalls.length >= 1)
      expect(h.runCalls[0].prompt).toContain("夜间活")
      // runCount 在 finishRun 开头更新，等收尾再断言
      await h.tasks.drain()
      await within(() => entry(h, t.id).runCount >= 1)
    } finally {
      await h.cleanup()
    }
  })

  test("时段外：不执行，且队列视图说明「为什么没跑」与「下次何时」", async () => {
    const h = await setup({ now: new Date(2026, 2, 4, 14, 0).getTime() }) // 周三 14:00（白天）
    try {
      const t = await h.tasks.add("default", {
        kind: "idle",
        runner: "prompt",
        prompt: "夜间活",
        idleWindow: { start: "22:00", end: "06:00" },
      })
      // 推进调度：仍不该跑
      await h.tasks.drain()
      expect(h.runCalls).toHaveLength(0)
      expect(entry(h, t.id).state).toBe("idle")
      const q = h.tasks.queueView("default")
      expect(q.idle.enabled).toBe(true)
      // 队列视图由后端算好：被时段拦下 + 下次进入时刻 = 当天 22:00
      expect(q.idle.blocked).toHaveLength(1)
      expect(q.idle.blocked[0].taskId).toBe(t.id)
      expect(q.idle.blocked[0].reason).toContain("不在闲时执行时间段")
      expect(q.idle.blocked[0].reason).toContain("22:00–06:00")
      expect(q.idle.nextOpenAt).toBe(new Date(2026, 2, 4, 22, 0).getTime())
    } finally {
      await h.cleanup()
    }
  })

  test("时间推进到时段内：下一轮调度自动起来（不会因一次被拦就再也不进候选）", async () => {
    const h = await setup({ now: new Date(2026, 2, 4, 14, 0).getTime() })
    try {
      await h.tasks.add("default", { kind: "idle", runner: "prompt", prompt: "夜间活", idleWindow: { start: "22:00", end: "06:00" } })
      await h.tasks.drain()
      expect(h.runCalls).toHaveLength(0)
      // 拨到 22:00
      h.clock.t = new Date(2026, 2, 4, 22, 0).getTime()
      await h.tasks.drain()
      await within(() => h.runCalls.length >= 1)
      expect(h.runCalls[0].prompt).toContain("夜间活")
    } finally {
      await h.cleanup()
    }
  })

  test("跨零点：22:00 与次日 03:00 都在窗内，12:00 在窗外", async () => {
    for (const [hour, expected] of [[22, 1], [23, 1], [3, 1], [12, 0], [21, 0]] as const) {
      const day = hour === 3 ? 5 : 4
      const h = await setup({ now: new Date(2026, 2, day, hour, 0).getTime() })
      try {
        await h.tasks.add("default", { kind: "idle", runner: "prompt", prompt: "夜间活", idleWindow: { start: "22:00", end: "06:00" } })
        await h.tasks.drain()
        expect(h.runCalls.length).toBe(expected)
      } finally {
        await h.cleanup()
      }
    }
  })

  test("指定星期：只在周一执行的时段，周三不跑", async () => {
    const h = await setup() // 周三 23:00
    try {
      const t = await h.tasks.add("default", {
        kind: "idle",
        runner: "prompt",
        prompt: "周一的活",
        idleWindow: { days: [1], start: "22:00", end: "06:00" },
      })
      await h.tasks.drain()
      expect(h.runCalls).toHaveLength(0)
      expect(h.tasks.queueView("default").idle.blocked[0].reason).toContain("周一")
      expect(entry(h, t.id).state).toBe("idle")
    } finally {
      await h.cleanup()
    }
  })

  test("未设时段 = 不限（既有行为不变）", async () => {
    const h = await setup({ now: new Date(2026, 2, 4, 12, 0).getTime() }) // 大中午
    try {
      await h.tasks.add("default", { kind: "idle", runner: "prompt", prompt: "随便跑" })
      await within(() => h.runCalls.length >= 1)
      expect(h.tasks.queueView("default").idle.blocked).toHaveLength(0)
    } finally {
      await h.cleanup()
    }
  })

  test("手动执行不被时段拦（用户明确点执行 ≠ 自动调度）", async () => {
    const h = await setup({ now: new Date(2026, 2, 4, 14, 0).getTime() }) // 白天
    try {
      const t = await h.tasks.add("default", {
        kind: "idle",
        runner: "prompt",
        prompt: "手动跑一次",
        idleWindow: { start: "22:00", end: "06:00" },
      })
      // 时段外：自动调度不该拉起它
      await h.tasks.drain()
      expect(h.runCalls).toHaveLength(0)
      // 手加入队（source=manual）→ 应直接执行，不被时段拦
      const res = await h.tasks.run("default", t.id, { front: true })
      expect(res?.queued).toBe(true)
      await within(() => h.runCalls.length >= 1)
      expect(h.runCalls[0].prompt).toContain("手动跑一次")
    } finally {
      await h.cleanup()
    }
  })

  test("非闲时任务不能设时段（语义只有闲时有）", async () => {
    const h = await setup()
    try {
      await expect(
        h.tasks.add("default", { kind: "manual", runner: "script", script: "echo", idleWindow: { start: "22:00", end: "06:00" } }),
      ).rejects.toThrow(/只有闲时任务/)
      await expect(
        h.tasks.add("default", { kind: "scheduled", runner: "script", script: "echo", schedule: "@daily", idleWindow: { start: "22:00", end: "06:00" } }),
      ).rejects.toThrow(/只有闲时任务/)
    } finally {
      await h.cleanup()
    }
  })

  test("新建闲时任务继承用户默认时段（偏好设一次覆盖后续所有）", async () => {
    const h = await setup({ defaultIdleWindow: async () => ({ start: "22:00", end: "06:00" }) })
    try {
      const t = await h.tasks.add("default", { kind: "idle", runner: "prompt", prompt: "继承默认" })
      expect(entry(h, t.id).idleWindow).toEqual({ start: "22:00", end: "06:00" })
      // 显式指定则覆盖默认
      const t2 = await h.tasks.add("default", {
        kind: "idle",
        runner: "prompt",
        prompt: "显式覆盖",
        idleWindow: { start: "09:00", end: "18:00" },
      })
      expect(entry(h, t2.id).idleWindow).toEqual({ start: "09:00", end: "18:00" })
    } finally {
      await h.cleanup()
    }
  })

  test("update 改/清时段：null 清除；停用后不再受时段影响", async () => {
    const h = await setup({ now: new Date(2026, 2, 4, 14, 0).getTime() })
    try {
      const t = await h.tasks.add("default", { kind: "idle", runner: "prompt", prompt: "x" })
      await h.tasks.update("default", t.id, { idleWindow: { start: "22:00", end: "06:00" } })
      expect(entry(h, t.id).idleWindow).toEqual({ start: "22:00", end: "06:00" })
      // 清除 → 回到不限时段（白天也能跑）
      await h.tasks.update("default", t.id, { idleWindow: null })
      expect(entry(h, t.id).idleWindow).toBeUndefined()
      await within(() => h.runCalls.length >= 1)
    } finally {
      await h.cleanup()
    }
  })

  test("外部编辑写坏时段：加载时清除（回到不限时段），不拒载任务", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-idlewin-bad-"))
    mkdirSync(join(home, "users", "default"), { recursive: true })
    const id = "a".repeat(32)
    writeBadTask(home, id)
    const store = new SessionStore({ home })
    const tasks = new TaskManager({
      home,
      store,
      env: new EnvManager(store),
      sandbox: new Sandbox({ home, enabled: false }),
      events: new EventBus(),
      now: () => new Date(2026, 2, 4, 12, 0).getTime(),
    })
    try {
      await tasks.start()
      const t = await tasks.get("default", id)
      expect(t).not.toBeNull()
      expect(t!.idleWindow).toBeUndefined()
      expect(String(t!.lastError ?? "")).toContain("idleWindow 非法")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

/** 写一条 idleWindow 非法的任务定义（模拟外部手改坏磁盘文件）。 */
function writeBadTask(home: string, id: string): void {
  const task = {
    id,
    user: "default",
    kind: "idle",
    runner: "prompt",
    prompt: "坏时段",
    enabled: true,
    createdAt: 1,
    updatedAt: 1,
    state: "idle",
    runCount: 0,
    idleWindow: { start: "99:99", end: "06:00" },
  }
  writeFileSync(join(home, "users", "default", "tasks.json"), JSON.stringify([task], null, 2), "utf8")
}
