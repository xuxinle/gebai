import { describe, expect, test } from "bun:test"
import { existsSync, mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { CronManager, parseCronSchedule, type CronTask, type CronTaskType } from "./cron"
import { SessionStore } from "./store"
import { Sandbox } from "./sandbox"
import { EnvManager } from "./env"
import { EventBus } from "./event-bus"
import type { AgentEngine } from "./engine"

function localDate(ms: number | string): Date {
  return new Date(ms)
}

describe("parseCronSchedule", () => {
  test("5-field cron computes next minute-level time", () => {
    const sched = parseCronSchedule("0 9 * * *")
    const from = localDate("2026-08-06T08:30:00").getTime()
    const next = sched.next(from)
    expect(localDate(next).getHours()).toBe(9)
    expect(localDate(next).getMinutes()).toBe(0)
    expect(next).toBeGreaterThan(from)
    expect(next).toBeLessThan(from + 24 * 3600 * 1000)
  })

  test("next occurrence after the scheduled time rolls to next day", () => {
    const sched = parseCronSchedule("0 9 * * *")
    const from = localDate("2026-08-06T10:00:00").getTime()
    const next = sched.next(from)
    expect(localDate(next).getDate()).toBe(7)
    expect(localDate(next).getHours()).toBe(9)
  })

  test("step and range fields", () => {
    const sched = parseCronSchedule("*/15 9-18 * * 1-5")
    const from = localDate("2026-08-06T00:00:00").getTime() // 周四
    const next = sched.next(from)
    const d = localDate(next)
    expect([0, 15, 30, 45]).toContain(d.getMinutes())
    expect(d.getHours()).toBeGreaterThanOrEqual(9)
    expect(d.getHours()).toBeLessThanOrEqual(18)
    expect(d.getDay()).toBeGreaterThanOrEqual(1)
    expect(d.getDay()).toBeLessThanOrEqual(5)
  })

  test("dow 0 and 7 both mean Sunday", () => {
    const sunday = localDate("2026-08-09T00:00:00").getTime() // 周日
    expect(parseCronSchedule("0 0 * * 0").next(sunday - 3600_000)).toBe(parseCronSchedule("0 0 * * 7").next(sunday - 3600_000))
  })

  test("dom/dow both restricted uses OR semantics", () => {
    const sched = parseCronSchedule("0 0 1 * 0")
    const from = localDate("2026-08-01T12:00:00").getTime() // 8 月 1 日（周六）当天，OR 语义应命中今天 0 点已过 → 下一命中
    const next = sched.next(from)
    // 2026-08-02 是周日（dow=0 命中）→ 下一次 0 点应为 8 月 2 日
    expect(localDate(next).getDate()).toBe(2)
  })

  test("@every interval", () => {
    const sched = parseCronSchedule("@every 30m")
    const from = localDate("2026-08-06T10:00:00").getTime()
    expect(sched.next(from)).toBe(from + 30 * 60 * 1000)
    const off = from + 10 * 60 * 1000
    expect(sched.next(off)).toBe(from + 30 * 60 * 1000)
  })

  test("aliases @daily/@hourly/@weekly/@monthly", () => {
    expect(parseCronSchedule("@daily").next(0)).toBe(parseCronSchedule("0 0 * * *").next(0))
    expect(parseCronSchedule("@hourly").next(0)).toBe(parseCronSchedule("0 * * * *").next(0))
    expect(parseCronSchedule("@weekly").next(0)).toBe(parseCronSchedule("0 0 * * 0").next(0))
    expect(parseCronSchedule("@monthly").next(0)).toBe(parseCronSchedule("0 0 1 * *").next(0))
  })

  test("invalid expressions throw", () => {
    expect(() => parseCronSchedule("")).toThrow()
    expect(() => parseCronSchedule("60 * * * *")).toThrow()
    expect(() => parseCronSchedule("0 9 * *")).toThrow()
    expect(() => parseCronSchedule("a b c d e")).toThrow()
    expect(() => parseCronSchedule("@every 5x")).toThrow()
    expect(() => parseCronSchedule("@every 0m")).toThrow()
    expect(() => parseCronSchedule("0 0 30 2 *").next(0)).toThrow() // 2 月 30 日永不触发
  })
})

interface Harness {
  home: string
  store: SessionStore
  sandbox: Sandbox
  env: EnvManager
  events: EventBus
  cron: CronManager
  executed: string[]
  runCalls: string[]
  running: boolean
}

function setup(now = 1_780_000_000_000, tickIntervalMs = 3600_000, safeMode = false) {
  const home = mkdtempSync(join(tmpdir(), "gebai-cron-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  const store = new SessionStore({ home })
  const sandbox = new Sandbox({ home, enabled: false })
  const env = new EnvManager(home, store)
  const events = new EventBus()
  const h: Harness = {
    home,
    store,
    sandbox,
    env,
    events,
    cron: null as unknown as CronManager,
    executed: [],
    runCalls: [],
    running: false,
  }
  const fakeEngine = {
    isRunning: () => h.running,
    run: async (_sid: string, _user: string, prompt: string) => {
      h.runCalls.push(prompt)
    },
  } as unknown as AgentEngine
  h.cron = new CronManager({ home, store, env, sandbox, events, engine: fakeEngine, now: () => now, tickIntervalMs, safeMode: safeMode })
  return h
}

function cleanup(h: Harness) {
  h.cron.stop()
  rmSync(h.home, { recursive: true, force: true })
}

async function createSession(h: Harness, name = "t"): Promise<{ id: string; user: string }> {
  const s = await h.store.createSession("default", name)
  return { id: s.id, user: "default" }
}

/** 访问调度器内部条目（add 返回克隆，测试触发用需改内部 nextRunAt）。 */
function internal(h: Harness, task: CronTask): CronTask {
  return h.cron["entries"].get(task.id)!
}

describe("CronManager", () => {
  test("add persists cron.json and computes nextRunAt", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      const task = await h.cron.add(id, user, { name: "daily", type: "script", schedule: "0 9 * * *", script: "echo hi" })
      expect(task.nextRunAt).toBeGreaterThan(0)
      expect(task.enabled).toBe(true)
      expect(internal(h, task).runCount).toBe(0)
      expect(existsSync(join(h.home, "users", "default", "sessions"))).toBe(true)
      const fromDisk = await h.cron.list(id, user)
      expect(fromDisk).toHaveLength(1)
      expect(fromDisk[0].script).toBe("echo hi")
      // cron.json 落盘在会话目录
      const dirs = (await h.store.listSessions("default")).map((s) => s.id)
      expect(dirs).toContain(id)
    } finally {
      cleanup(h)
    }
  })

  test("add validates type fields and schedule", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      await expect(h.cron.add(id, user, { type: "script", schedule: "0 9 * * *" })).rejects.toThrow(/script/)
      await expect(h.cron.add(id, user, { type: "prompt", schedule: "0 9 * * *" })).rejects.toThrow(/prompt/)
      await expect(h.cron.add(id, user, { type: "script", schedule: "bad", script: "echo" })).rejects.toThrow(/无效/)
      await expect(h.cron.add(id, user, { type: "nope" as CronTaskType, schedule: "0 9 * * *", script: "x" })).rejects.toThrow(/类型/)
      expect(await h.cron.list(id, user)).toHaveLength(0)
    } finally {
      cleanup(h)
    }
  })

  test("update and remove with session ownership", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      const s2 = await h.store.createSession("default", "t2")
      const task = await h.cron.add(id, user, { type: "script", schedule: "0 9 * * *", script: "echo a" })
      // 其他会话无权修改/删除
      expect(await h.cron.update(s2.id, user, task.id, { enabled: false })).toBeNull()
      expect(await h.cron.remove(s2.id, user, task.id)).toBe(false)
      // 本会话可更新
      const updated = await h.cron.update(id, user, task.id, { enabled: false, schedule: "@every 10m" })
      expect(updated?.enabled).toBe(false)
      // 停用后 nextRunAt 仍保留原值（不重算），重新启用时重算
      const re = await h.cron.update(id, user, task.id, { enabled: true })
      expect(re?.enabled).toBe(true)
      expect(re?.nextRunAt).toBeGreaterThan(0)
      // 本会话可删除
      expect(await h.cron.remove(id, user, task.id)).toBe(true)
      expect(await h.cron.list(id, user)).toHaveLength(0)
    } finally {
      cleanup(h)
    }
  })

  test("update validates content after type change", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      const task = await h.cron.add(id, user, { type: "script", schedule: "0 9 * * *", script: "echo a" })
      await expect(h.cron.update(id, user, task.id, { type: "prompt" })).rejects.toThrow(/prompt/)
    } finally {
      cleanup(h)
    }
  })

  test("tick fires script task: exec + message + events + nextRunAt advance", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      h.sandbox.exec = async (cmd) => {
        h.executed.push(cmd)
        return { stdout: "out-1\nout-2", stderr: "", code: 0 }
      }
      const published: string[] = []
      h.events.subscribe((ev) => published.push(ev.type))
      const task = await h.cron.add(id, user, { name: "sync", type: "script", schedule: "@every 30m", script: "echo ok" })
      // 把 nextRunAt 拨到过去触发
      internal(h, task).nextRunAt = h.cron["now"]() - 1000
      await h.cron.tick()
      expect(h.executed).toEqual(["echo ok"])
      expect(internal(h, task).runCount).toBe(1)
      expect(internal(h, task).lastStatus).toBe("success")
      expect(internal(h, task).lastRunAt).toBe(h.cron["now"]())
      expect(internal(h, task).nextRunAt).toBeGreaterThan(h.cron["now"]())
      const session = await h.store.load(id, user)
      const last = session!.messages.at(-1)!
      expect(last.content).toContain("[定时任务「sync」执行结果（成功）]")
      expect(last.content).toContain("out-2")
      expect(published).toContain("event.cron.run")
      expect(published).toContain("event.cron.result")
      // 任务记录落盘
      const fromDisk = await h.cron.list(id, user)
      expect(fromDisk[0].runCount).toBe(1)
    } finally {
      cleanup(h)
    }
  })

  test("safe mode skips script cron tasks (no exec, message persisted, status skipped)", async () => {
    const h = setup(1_780_000_000_000, 3600_000, true) // safeMode=true
    try {
      const { id, user } = await createSession(h)
      h.sandbox.exec = async (cmd) => {
        h.executed.push(cmd)
        return { stdout: "should-not-run", stderr: "", code: 0 }
      }
      const task = await h.cron.add(id, user, { name: "danger", type: "script", schedule: "@every 30m", script: "echo pwned" })
      internal(h, task).nextRunAt = h.cron["now"]() - 1000
      await h.cron.tick()
      // 脚本未执行、状态标记跳过、提示消息落盘
      expect(h.executed).toEqual([])
      expect(internal(h, task).lastStatus).toBe("skipped")
      // nextRunAt 已推进：高频任务不会每 tick 重复刷跳过提示
      expect(internal(h, task).nextRunAt).toBeGreaterThan(h.cron["now"]())
      const session = await h.store.load(id, user)
      const last = session!.messages.at(-1)!
      expect(last.content).toContain("已跳过")
      expect(last.content).toContain("安全模式")
    } finally {
      cleanup(h)
    }
  })

  test("tick fires prompt task through engine", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      const task = await h.cron.add(id, user, { name: "report", type: "prompt", schedule: "@daily", prompt: "生成日报" })
      internal(h, task).nextRunAt = h.cron["now"]() - 1000
      await h.cron.tick()
      expect(h.runCalls).toHaveLength(1)
      expect(h.runCalls[0]).toContain("生成日报")
      expect(h.runCalls[0]).toContain("「report」")
      expect(internal(h, task).runCount).toBe(1)
      expect(internal(h, task).lastStatus).toBe("success")
      expect(h.executed).toHaveLength(0)
    } finally {
      cleanup(h)
    }
  })

  test("prompt task skipped while session busy, then rescheduled", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      const task = await h.cron.add(id, user, { type: "prompt", schedule: "@daily", prompt: "hi" })
      internal(h, task).nextRunAt = h.cron["now"]() - 1000
      h.running = true
      await h.cron.tick()
      expect(h.runCalls).toHaveLength(0)
      expect(internal(h, task).lastStatus).toBe("skipped")
      expect(internal(h, task).runCount).toBe(0)
      expect(internal(h, task).nextRunAt).toBeGreaterThan(h.cron["now"]())
    } finally {
      cleanup(h)
    }
  })

  test("disabled task is not fired", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      h.sandbox.exec = async () => ({ stdout: "", stderr: "", code: 0 })
      const task = await h.cron.add(id, user, { type: "script", schedule: "@every 30m", script: "echo x", enabled: false })
      internal(h, task).nextRunAt = h.cron["now"]() - 1000
      await h.cron.tick()
      expect(h.executed).toHaveLength(0)
      expect(internal(h, task).runCount).toBe(0)
    } finally {
      cleanup(h)
    }
  })

  test("script failure records error and reports exit code", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      h.sandbox.exec = async () => ({ stdout: "partial", stderr: "boom", code: 2 })
      const task = await h.cron.add(id, user, { type: "script", schedule: "@every 30m", script: "false" })
      internal(h, task).nextRunAt = h.cron["now"]() - 1000
      await h.cron.tick()
      expect(internal(h, task).lastStatus).toBe("error")
      expect(internal(h, task).lastError).toContain("exit 2")
      const session = await h.store.load(id, user)
      expect(session!.messages.at(-1)!.content).toContain("（失败）")
    } finally {
      cleanup(h)
    }
  })

  test("start() reloads persisted tasks and skips missed runs while server down", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      const task = await h.cron.add(id, user, { type: "script", schedule: "@daily", script: "echo ok" })
      internal(h, task).nextRunAt = 1 // 停机前已过期
      await h.cron["saveSessionEntries"](id, user) // 把过期状态落盘
      // 模拟重启：新实例 start() 扫描加载，过期任务从当前时间重算（不补跑）
      const cron2 = new CronManager({ home: h.home, store: h.store, sandbox: h.sandbox, env: h.env, events: h.events, now: () => h.cron["now"]() + 60_000 })
      try {
        await cron2.start()
        const tasks = await cron2.list(id, user)
        expect(tasks).toHaveLength(1)
        expect(tasks[0].nextRunAt).toBeGreaterThan(h.cron["now"]())
        // 旧的 nextRunAt 已被修正
        expect(tasks[0].nextRunAt).not.toBe(1)
      } finally {
        cron2.stop()
      }
    } finally {
      cleanup(h)
    }
  })

  test("tick drops entry when session is deleted", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      const task = await h.cron.add(id, user, { type: "script", schedule: "@daily", script: "echo ok" })
      internal(h, task).nextRunAt = h.cron["now"]() - 1000
      await h.store.delete(id, user)
      await h.cron.tick()
      expect(await h.cron.list(id, user)).toHaveLength(0)
    } finally {
      cleanup(h)
    }
  })

  test("tick failure reschedules nextRunAt (no infinite retry loop)", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      // 引擎抛错（会话运行中竞态/引擎不可用）：fire 抛错进入 tick 的 catch
      const failingEngine = {
        isRunning: () => false,
        run: async () => {
          throw new Error("session busy")
        },
      }
      ;(h.cron as unknown as { engine: typeof failingEngine }).engine = failingEngine
      const task = await h.cron.add(id, user, { type: "prompt", schedule: "@every 30m", prompt: "hi" })
      internal(h, task).nextRunAt = h.cron["now"]() - 1000
      await h.cron.tick()
      // 失败后必须重算下次执行时间，否则下一个 tick 会立刻重试（30 秒热循环）
      expect(internal(h, task).lastStatus).toBe("error")
      expect(internal(h, task).nextRunAt).toBeGreaterThan(h.cron["now"]())
      // 再次 tick 不应重复触发（nextRunAt 已在未来）
      const before = h.runCalls.length
      await h.cron.tick()
      expect(h.runCalls.length).toBe(before)
    } finally {
      cleanup(h)
    }
  })
})
