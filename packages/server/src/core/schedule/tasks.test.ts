import { describe, expect, test } from "bun:test"
import { randomUUID } from "node:crypto"
import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { Task, TaskNotifyChannel } from "@gebai/sdk"
import { EventBus } from "../base/event-bus"
import type { AgentEngine } from "../engine/engine"
import { Sandbox } from "../security/sandbox"
import { EnvManager } from "../session/env"
import { SessionStore } from "../session/store"
import { isOneShotSchedule, parseSchedule } from "./expr"
import { TASK_FILE_MAX_BYTES, TASK_MAX_CONCURRENT_DEFAULT, TASK_RUNS_HISTORY, TaskManager, type TaskManagerDeps } from "./tasks"
import { TASK_RUNS_KEEP, trimTaskRuns } from "./task-runs"

/**
 * 统一任务管理（core/schedule/tasks.ts）单测：三类任务（定时/普通/闲时）共用一条队列的
 * 入队顺序、并发额度、闲时让路、会话目标解析、执行收尾与通知、资源文件、持久化与旧数据迁移。
 *
 * 范式沿用 cron.test.ts / todos.test.ts：注入 now（虚拟时钟）、tickIntervalMs 与 fake AgentEngine，
 * 直接 await tick()/drain() 而非等真实定时器。执行本身是「出队即异步跑（不阻塞队列推进）」，
 * 故需要等待执行完成的用例统一用 waitFor 轮询收尾状态。
 */

interface Harness {
  home: string
  store: SessionStore
  sandbox: Sandbox
  env: EnvManager
  events: EventBus
  tasks: TaskManager
  /** 虚拟时钟（测试推进）。 */
  clock: { t: number }
  execCalls: Array<{ cmd: string; cwd: string; env?: Record<string, string> }>
  runCalls: Array<{ sid: string; user: string; prompt: string; interactionMode?: string; autoApprove?: boolean; envOverride?: Record<string, string> }>
  cancelCalls: string[]
  windDownCalls: string[]
  /** 挂起中的 engine.run 解挂回调（runHang 时登记）。 */
  runResolvers: Array<() => void>
  runHang: boolean
  runFail: string | null
  appendReply: boolean
  /** engine.busyUser 为真的用户（闲时任务的「用户优先」判定）。 */
  busyUsers: Set<string>
  /** engine.isRunning 为真的会话（目标会话忙 → 入队等待）。 */
  busySessions: Set<string>
  published: string[]
  notifyPosts: Array<{ url: string; body: Record<string, unknown>; headers?: Record<string, string> }>
  feishuSent: Array<{ chatId: string; msgType: string; content: Record<string, unknown> }>
  agentNames: string[]
  /** 已注册事件 Webhook 模拟注册表（webhookId 引用解析）：id → {url,secret,owner}。 */
  webhookRegistry: Map<string, { url: string; secret?: string; owner?: string }>
  /** onTaskFinished 回调留痕（任务收尾联动的观测点）。 */
  finished: Array<{ taskId: string; status: string }>
  /** 已收尾任务的 id 序列（`event.task.result` 发布序，waitDone 的完成判据用）。 */
  results: string[]
  /** 构造用的依赖（重启类用例用它新建调度器实例）。 */
  deps: TaskManagerDeps
}

function setup(opts: { now?: number; tickIntervalMs?: number; safeMode?: boolean; maxConcurrent?: number; defaultNotify?: TaskNotifyChannel[] } = {}): Harness {
  const home = mkdtempSync(join(tmpdir(), "gebai-tasks-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  const store = new SessionStore({ home })
  const sandbox = new Sandbox({ home, enabled: false })
  const env = new EnvManager(store)
  const events = new EventBus()
  const h: Harness = {
    home,
    store,
    deps: null as unknown as TaskManagerDeps,
    sandbox,
    env,
    events,
    tasks: null as unknown as TaskManager,
    clock: { t: opts.now ?? 1_780_000_000_000 },
    execCalls: [],
    runCalls: [],
    cancelCalls: [],
    windDownCalls: [],
    runResolvers: [],
    runHang: false,
    runFail: null,
    appendReply: true,
    busyUsers: new Set(),
    busySessions: new Set(),
    published: [],
    notifyPosts: [],
    feishuSent: [],
    agentNames: ["explore", "code"],
    webhookRegistry: new Map(),
    finished: [],
    results: [],
  }
  events.subscribe((ev) => {
    h.published.push(ev.type)
    if (ev.type === "event.task.result") h.results.push(String((ev.payload as { id?: unknown })?.id ?? ""))
  })
  // 脚本执行：默认记录调用并成功返回（用例可替换为失败/自定义输出）
  sandbox.exec = (async (cmd: string, o: { cwd?: string; env?: Record<string, string> }) => {
    h.execCalls.push({ cmd, cwd: o.cwd ?? "", env: o.env })
    return { stdout: `out:${cmd}`, stderr: "", code: 0 }
  }) as unknown as Sandbox["exec"]
  const fakeEngine = {
    isRunning: (sid: string) => h.busySessions.has(sid),
    busyUser: (user: string) => h.busyUsers.has(user),
    cancel: async (sid: string) => {
      h.cancelCalls.push(sid)
      h.runResolvers.splice(0).forEach((f) => f())
    },
    // 快速结束（windDown）：无运行中子会话时等价 cancel（子会话收尾路径由 subsessions 用例覆盖）
    windDown: async (sid: string) => {
      h.windDownCalls.push(sid)
      h.runResolvers.splice(0).forEach((f) => f())
    },
    run: async (sid: string, user: string, prompt: string, opts?: { interactionMode?: string; autoApprove?: boolean; envOverride?: Record<string, string> }) => {
      h.runCalls.push({ sid, user, prompt, interactionMode: opts?.interactionMode, autoApprove: opts?.autoApprove, envOverride: opts?.envOverride })
      if (h.runHang) await new Promise<void>((resolve) => h.runResolvers.push(resolve))
      if (h.runFail) throw new Error(h.runFail)
      if (h.appendReply) {
        await store.appendMessage(sid, { id: randomUUID(), role: "assistant", content: "任务执行完成：示例结果", createdAt: h.clock.t }, user)
      }
    },
    setTasks: () => {},
  } as unknown as AgentEngine
  const deps: TaskManagerDeps = {
    home,
    store,
    env,
    sandbox,
    events,
    engine: fakeEngine,
    now: () => h.clock.t,
    tickIntervalMs: opts.tickIntervalMs ?? 3600_000,
    ...(opts.safeMode === undefined ? {} : { safeMode: opts.safeMode }),
    ...(opts.maxConcurrent === undefined ? {} : { maxConcurrent: opts.maxConcurrent }),
    notify: {
      fetchImpl: async (url, init) => {
        h.notifyPosts.push({ url, body: JSON.parse(String(init.body)) as Record<string, unknown>, headers: init.headers as Record<string, string> })
        return { ok: true, status: 200 }
      },
      feishuSend: async (chatId, msgType, content) => void h.feishuSent.push({ chatId, msgType, content }),
    },
    agentExists: (name) => h.agentNames.includes(name),
    resolveWebhook: (id, user) => {
      const cfg = h.webhookRegistry.get(id)
      if (!cfg) return null
      if (cfg.owner && cfg.owner !== user) return null
      return { url: cfg.url, secret: cfg.secret }
    },
    ...(opts.defaultNotify ? { defaultNotify: opts.defaultNotify } : {}),
    onTaskFinished: (task, run) => void h.finished.push({ taskId: task.id, status: run.status }),
  }
  h.deps = deps
  h.tasks = new TaskManager(deps)
  return h
}

async function cleanup(h: Harness): Promise<void> {
  h.tasks.stop()
  // 解挂起中的执行（避免遗留 pending 的 run 在临时目录已删除后写回）
  h.runResolvers.splice(0).forEach((f) => f())
  // 等收尾链静默（无运行中任务、且事件流连续两轮无新增）再删目录：执行是「出队即异步跑」，
  // 在途落盘撞上已删目录会抛 ENOENT（虽已由 TaskManager 降级为告警，仍该避免半落盘状态）
  let seen = -1
  await waitFor(() => {
    const n = h.published.length
    const quiet = n === seen && h.tasks["running"].size === 0
    seen = n
    return quiet
  }, 2000).catch(() => {})
  await new Promise((r) => setTimeout(r, 20))
  // 在途收尾写盘可能仍占着目录（Windows EBUSY）——重试等句柄释放
  rmSync(h.home, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 })
}

function taskFile(h: Harness, user = "default"): string {
  return join(h.home, "users", user, "tasks.json")
}

function readTasks(h: Harness, user = "default"): Task[] {
  return JSON.parse(readFileSync(taskFile(h, user), "utf8")) as Task[]
}

/** 访问调度器内部条目（对外方法返回克隆，状态断言需读内部对象）。 */
function internal(h: Harness, id: string): Task {
  return h.tasks["entries"].get(id)!
}

async function waitFor(cond: () => boolean, timeoutMs = 5000): Promise<void> {
  const start = Date.now()
  while (!cond()) {
    if (Date.now() - start > timeoutMs) throw new Error("等待条件超时")
    await new Promise((r) => setTimeout(r, 5))
  }
}

/** 等待任务收尾**整条链**走完（可指定已运行次数）。
 *  单看 `state`/`runCount` 不够：两者在 finishRun **开头**就更新（此时落盘与结果事件尚未发生），
 *  紧随其后的断言会读到未完成的 runs/事件/落盘状态。故在两者之外再等一条**本次**收尾事件。
 *  调用时任务已收尾（本用例前面已等到）则直接返回，不再等新事件。 */
async function waitDone(h: Harness, id: string, runCount?: number): Promise<Task> {
  const done = (): boolean => {
    const e = h.tasks["entries"].get(id)
    return !!e && e.state !== "running" && (runCount === undefined || e.runCount >= runCount)
  }
  if (done()) return internal(h, id)
  const base = h.results.filter((x) => x === id).length
  await waitFor(() => done() && h.results.filter((x) => x === id).length > base)
  return internal(h, id)
}

async function createSession(h: Harness, name = "t", user = "default"): Promise<string> {
  const s = await h.store.createSession(user, name)
  return s.id
}

/** 把定时任务拨到立即可触发（写内部 nextRunAt，模拟时间流逝）。 */
function due(h: Harness, id: string, dir = -1000): void {
  internal(h, id).nextRunAt = h.clock.t + dir
}

describe("执行表达式解析（expr）", () => {
  test("5 段 cron 计算下一次分钟级时间", () => {
    const sched = parseSchedule("0 9 * * *")
    const from = new Date("2026-08-06T08:30:00").getTime()
    const next = sched.next(from)
    expect(new Date(next).getHours()).toBe(9)
    expect(new Date(next).getMinutes()).toBe(0)
    expect(next).toBeGreaterThan(from)
    expect(next).toBeLessThan(from + 24 * 3600 * 1000)
  })

  test("已过触发点滚动到下一天", () => {
    const from = new Date("2026-08-06T10:00:00").getTime()
    const next = parseSchedule("0 9 * * *").next(from)
    expect(new Date(next).getDate()).toBe(7)
    expect(new Date(next).getHours()).toBe(9)
  })

  test("步长与范围字段", () => {
    const from = new Date("2026-08-06T00:00:00").getTime() // 周四
    const d = new Date(parseSchedule("*/15 9-18 * * 1-5").next(from))
    expect([0, 15, 30, 45]).toContain(d.getMinutes())
    expect(d.getHours()).toBeGreaterThanOrEqual(9)
    expect(d.getHours()).toBeLessThanOrEqual(18)
    expect(d.getDay()).toBeGreaterThanOrEqual(1)
    expect(d.getDay()).toBeLessThanOrEqual(5)
  })

  test("周字段 0 与 7 同义（周日）", () => {
    const sunday = new Date("2026-08-09T00:00:00").getTime()
    expect(parseSchedule("0 0 * * 0").next(sunday - 3600_000)).toBe(parseSchedule("0 0 * * 7").next(sunday - 3600_000))
  })

  test("日与周均受限时取 OR 语义", () => {
    const from = new Date("2026-08-01T12:00:00").getTime() // 8/1（周六）当天，OR 命中次日周日 0 点
    expect(new Date(parseSchedule("0 0 1 * 0").next(from)).getDate()).toBe(2)
  })

  test("@every 间隔按固定节拍对齐", () => {
    const from = new Date("2026-08-06T10:00:00").getTime()
    const sched = parseSchedule("@every 30m")
    expect(sched.next(from)).toBe(from + 30 * 60 * 1000)
    expect(sched.next(from + 10 * 60 * 1000)).toBe(from + 30 * 60 * 1000)
  })

  test("别名 @daily/@hourly/@weekly/@monthly", () => {
    expect(parseSchedule("@daily").next(0)).toBe(parseSchedule("0 0 * * *").next(0))
    expect(parseSchedule("@hourly").next(0)).toBe(parseSchedule("0 * * * *").next(0))
    expect(parseSchedule("@weekly").next(0)).toBe(parseSchedule("0 0 * * 0").next(0))
    expect(parseSchedule("@monthly").next(0)).toBe(parseSchedule("0 0 1 * *").next(0))
  })

  test("@at 一次性表达式解析绝对时间（ISO 与空格分隔）", () => {
    expect(isOneShotSchedule("@at 2026-09-01T09:00")).toBe(true)
    expect(isOneShotSchedule("0 9 * * *")).toBe(false)
    const sched = parseSchedule("@at 2026-09-01T09:00")
    const from = new Date("2026-08-06T10:00:00").getTime()
    const target = new Date("2026-09-01T09:00:00").getTime()
    expect(sched.next(from)).toBe(target)
    expect(sched.next(0)).toBe(target) // 与 fromMs 无关（绝对时刻）
    expect(parseSchedule("@at 2026-09-01 09:00").next(from)).toBe(target)
    expect(() => parseSchedule("@at whenever")).toThrow(/@at/)
  })

  test("时区按目标时区墙上时钟计算", () => {
    const from = Date.UTC(2026, 7, 5, 22, 0) // UTC 8/5 22:00 = 上海 8/6 06:00
    const next = parseSchedule("0 9 * * *", "Asia/Shanghai").next(from)
    const wall = new Intl.DateTimeFormat("en-US", { timeZone: "Asia/Shanghai", hour: "2-digit", minute: "2-digit", hour12: false }).format(new Date(next))
    expect(wall).toBe("09:00")
    expect(next).toBe(Date.UTC(2026, 7, 6, 1, 0))
  })

  test("非法表达式/时区抛错", () => {
    expect(() => parseSchedule("0 9 * * *", "Mars/Olympus")).toThrow(/时区/)
    expect(() => parseSchedule("")).toThrow()
    expect(() => parseSchedule("60 * * * *")).toThrow()
    expect(() => parseSchedule("0 9 * *")).toThrow()
    expect(() => parseSchedule("a b c d e")).toThrow()
    expect(() => parseSchedule("@every 5x")).toThrow()
    expect(() => parseSchedule("@every 0m")).toThrow()
    expect(() => parseSchedule("0 0 30 2 *").next(0)).toThrow() // 2 月 30 日永不触发
  })
})

describe("任务增删改与校验", () => {
  test("add 落盘 tasks.json 并计算 nextRunAt（定时任务）", async () => {
    const h = setup()
    try {
      const sid = await createSession(h)
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "script", schedule: "0 9 * * *", script: "echo hi", name: "daily" }, sid)
      expect(task.enabled).toBe(true)
      expect(task.kind).toBe("scheduled")
      expect(task.runner).toBe("script")
      expect(task.nextRunAt).toBeGreaterThan(0)
      expect(task.originSessionId).toBe(sid)
      expect(task.state).toBe("idle")
      expect(existsSync(taskFile(h))).toBe(true)
      const onDisk = readTasks(h)
      expect(onDisk).toHaveLength(1)
      expect(onDisk[0].script).toBe("echo hi")
      expect((await h.tasks.get("default", task.id))?.name).toBe("daily")
      expect(await h.tasks.get("default", "0".repeat(32))).toBeNull()
    } finally {
      await cleanup(h)
    }
  })

  test("add 校验：执行体/内容/表达式/目标/子Agent/通知/超时", async () => {
    const h = setup()
    try {
      await expect(h.tasks.add("default", { runner: "nope" as never })).rejects.toThrow(/执行体/)
      await expect(h.tasks.add("default", { runner: "script" })).rejects.toThrow(/script 参数/)
      await expect(h.tasks.add("default", { runner: "prompt" })).rejects.toThrow(/prompt 参数/)
      await expect(h.tasks.add("default", { kind: "scheduled", runner: "script", script: "echo" })).rejects.toThrow(/schedule/)
      await expect(h.tasks.add("default", { runner: "script", script: "echo", schedule: "bad" })).rejects.toThrow(/无效的 cron 表达式/)
      await expect(h.tasks.add("default", { runner: "script", script: "echo", schedule: "@at 2020-01-01T00:00" })).rejects.toThrow(/已过去/)
      await expect(h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "hi", target: "nowhere" as never })).rejects.toThrow(/执行目标/)
      await expect(h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "hi", agents: ["ghost"] })).rejects.toThrow(/不存在/)
      await expect(
        h.tasks.add("default", { kind: "scheduled", runner: "script", script: "x", schedule: "0 9 * * *", notify: [{ type: "feishu", target: "https://evil.com/hook" }] }),
      ).rejects.toThrow(/飞书/)
      await expect(h.tasks.add("default", { kind: "scheduled", runner: "script", script: "x", schedule: "0 9 * * *", timeoutMs: 10 })).rejects.toThrow(/超时/)
      await expect(h.tasks.add("default", { kind: "scheduled", runner: "script", script: "x", schedule: "0 9 * * *", maxConsecutiveErrors: -1 })).rejects.toThrow(/0~1000/)
      expect(await h.tasks.list("default")).toHaveLength(0)
    } finally {
      await cleanup(h)
    }
  })

  test("kind 推断：给了 schedule 视为定时，否则普通；idle 显式指定", async () => {
    const h = setup()
    try {
      const a = await h.tasks.add("default", { runner: "script", script: "echo", schedule: "@every 1h", runNow: false })
      const b = await h.tasks.add("default", { runner: "script", script: "echo", runNow: false })
      const c = await h.tasks.add("default", { kind: "idle", runner: "script", script: "echo" })
      expect(a.kind).toBe("scheduled")
      expect(b.kind).toBe("manual")
      expect(c.kind).toBe("idle")
      // 普通任务 runNow 缺省即入队；显式 false 只创建
      expect(b.state).toBe("idle")
    } finally {
      await cleanup(h)
    }
  })

  test("update/remove 归属校验、停用退出队列、重新启用重算下次执行", async () => {
    const h = setup()
    try {
      await createSession(h, "t2", "other")
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "script", script: "echo a", schedule: "0 9 * * *" })
      expect(await h.tasks.update("other", task.id, { enabled: false })).toBeNull()
      expect(await h.tasks.remove("other", task.id)).toBe(false)
      const off = await h.tasks.update("default", task.id, { enabled: false, schedule: "@every 10m" })
      expect(off?.enabled).toBe(false)
      expect(off?.nextRunAt).toBe(task.nextRunAt) // 停用不重算调度时间
      const on = await h.tasks.update("default", task.id, { enabled: true })
      expect(on?.enabled).toBe(true)
      expect(on?.nextRunAt).toBeGreaterThan(0)
      expect(await h.tasks.remove("default", task.id)).toBe(true)
      expect(await h.tasks.list("default")).toHaveLength(0)
    } finally {
      await cleanup(h)
    }
  })

  test("update 改执行体后校验内容；普通任务不接受 schedule", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", { runner: "script", script: "echo a", runNow: false })
      await expect(h.tasks.update("default", task.id, { runner: "prompt" })).rejects.toThrow(/prompt 参数/)
      await expect(h.tasks.update("default", task.id, { schedule: "@every 5m" })).rejects.toThrow(/没有执行表达式/)
    } finally {
      await cleanup(h)
    }
  })

  test("list 对通知密钥脱敏（secret → ***）", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", {
        kind: "scheduled",
        runner: "script",
        script: "echo",
        schedule: "@every 1h",
        notify: [{ type: "webhook", target: "https://example.com/hook", secret: "s3cret" }],
      })
      const [view] = await h.tasks.list("default")
      expect(view.notify?.[0].secret).toBe("***")
      // 内部对象仍保留真值（投递时使用）
      expect(internal(h, task.id).notify?.[0].secret).toBe("s3cret")
      expect(readTasks(h)[0].notify?.[0].secret).toBe("s3cret")
    } finally {
      await cleanup(h)
    }
  })
})

describe("统一队列：顺序、优先级与并发额度", () => {
  test("普通任务创建即入队并按 FIFO 执行", async () => {
    const h = setup()
    try {
      const a = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "任务 A" })
      const b = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "任务 B" })
      await waitFor(() => h.runCalls.length === 2)
      expect(h.runCalls[0].prompt).toContain("任务 A")
      expect(h.runCalls[1].prompt).toContain("任务 B")
      expect((await waitDone(h, a.id)).lastStatus).toBe("success")
      expect((await waitDone(h, b.id)).lastStatus).toBe("success")
      expect((await h.tasks.get("default", a.id))?.state).toBe("idle")
    } finally {
      await cleanup(h)
    }
  })

  test("并发额度：maxConcurrent=2 时第三条排队等待，槽位释放后自动续跑", async () => {
    const h = setup({ maxConcurrent: 2 })
    try {
      h.runHang = true
      const a = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "A" })
      const b = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "B" })
      const c = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "C" })
      expect(h.runCalls).toHaveLength(2)
      expect(internal(h, a.id).state).toBe("running")
      expect(internal(h, b.id).state).toBe("running")
      expect(internal(h, c.id).state).toBe("queued")
      const view = h.tasks.queueView("default")
      expect(view.limit).toBe(2)
      expect(view.running.map((r) => r.taskId).sort()).toEqual([a.id, b.id].sort())
      expect(view.entries.map((e) => e.taskId)).toEqual([c.id])
      expect(view.entries[0].position).toBe(1)
      // 运行中的任务不因新任务入队被中断（额度不足只排队）
      expect(h.cancelCalls).toHaveLength(0)
      expect(h.windDownCalls).toHaveLength(0)
      // 槽位释放 → 队首自动起步
      h.runResolvers.shift()?.()
      await waitFor(() => h.runCalls.length === 3)
      expect(h.runCalls[2].prompt).toContain("C")
      expect(internal(h, c.id).state).toBe("running")
      expect(h.tasks.queueView("default").entries).toHaveLength(0)
    } finally {
      await cleanup(h)
    }
  })

  test("优先级：front 置顶优于普通任务，定时任务到期仍排在更前", async () => {
    const h = setup({ maxConcurrent: 1 })
    try {
      h.runHang = true
      const a = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "A" })
      const b = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "B" })
      const c = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "C", front: true })
      const d = await h.tasks.add("default", { kind: "scheduled", runner: "prompt", prompt: "D", schedule: "@every 30m" })
      expect(internal(h, a.id).state).toBe("running")
      expect(h.tasks.queueView("default").entries.map((e) => e.taskId)).toEqual([c.id, b.id])
      due(h, d.id)
      await h.tasks.tick()
      const entries = h.tasks.queueView("default").entries
      expect(entries.map((e) => e.taskId)).toEqual([d.id, c.id, b.id])
      expect(entries[0].source).toBe("schedule")
      expect(entries[1].front).toBe(true)
      // 额度未释放：仅 A 在跑
      expect(h.runCalls).toHaveLength(1)
    } finally {
      await cleanup(h)
    }
  })

  test("手动执行：重复入队幂等、置顶就地生效、出队与终止", async () => {
    const h = setup({ maxConcurrent: 1 })
    try {
      h.runHang = true
      const a = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "A" })
      const b = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "B", runNow: false })
      expect(internal(h, b.id).state).toBe("idle")
      const r1 = await h.tasks.run("default", b.id)
      expect(r1?.queued).toBe(true)
      expect(r1?.position).toBe(1)
      const r2 = await h.tasks.run("default", b.id)
      expect(r2?.queued).toBe(true)
      expect(h.tasks.queueView("default").entries).toHaveLength(1) // 幂等：不重复入队
      // 出队（排队中）
      expect(await h.tasks.cancel("default", b.id)).toBe(true)
      expect(internal(h, b.id).state).toBe("idle")
      expect(h.tasks.queueView("default").entries).toHaveLength(0)
      // 运行中的任务不能出队，只能终止
      expect(await h.tasks.cancel("default", a.id)).toBe(false)
      expect(await h.tasks.stopRun("default", a.id)).toBe(true)
      expect(h.cancelCalls).toContain(h.runCalls[0].sid)
      expect(await h.tasks.stopRun("default", b.id)).toBe(false) // 已不在运行
      expect(await h.tasks.run("default", "0".repeat(32))).toBeNull()
    } finally {
      await cleanup(h)
    }
  })

  test("停用的任务不入队（排队中被停用即退出队列）", async () => {
    const h = setup({ maxConcurrent: 1 })
    try {
      h.runHang = true
      await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "占用额度" })
      const b = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "B" })
      expect(internal(h, b.id).state).toBe("queued")
      await h.tasks.update("default", b.id, { enabled: false })
      expect(internal(h, b.id).state).toBe("idle")
      expect(internal(h, b.id).queue).toBeUndefined()
      expect(h.tasks.queueView("default").entries).toHaveLength(0)
      const r = await h.tasks.run("default", b.id)
      expect(r?.queued).toBe(false)
      expect(r?.reason).toContain("已停用")
    } finally {
      await cleanup(h)
    }
  })

  test("多用户互不挤占额度（每用户一条队列）", async () => {
    const h = setup({ maxConcurrent: 1 })
    try {
      h.runHang = true
      const a = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "A 用户任务" })
      const b = await h.tasks.add("other", { kind: "manual", runner: "prompt", prompt: "B 用户任务" })
      // 每用户额度 1：两个用户的任务都在跑
      expect(internal(h, a.id).state).toBe("running")
      expect(internal(h, b.id).state).toBe("running")
      expect(h.runCalls).toHaveLength(2)
      expect(h.tasks.queueView("default").limit).toBe(1)
      expect(h.tasks.queueView("other").running.map((r) => r.taskId)).toEqual([b.id])
    } finally {
      await cleanup(h)
    }
  })
})

/** 直接写 tasks.json（模拟既有/外部编辑的文件），返回条目 id 列表。 */
function makeTaskFile(h: Harness, entries: Array<Partial<Task>>, user = "default"): string[] {
  const ids: string[] = []
  const list = entries.map((e) => {
    const id = e.id ?? randomUUID().replace(/-/g, "")
    ids.push(id)
    return {
      id,
      user,
      kind: "scheduled",
      runner: "script",
      script: "echo x",
      schedule: "@every 1h",
      enabled: true,
      createdAt: h.clock.t,
      updatedAt: h.clock.t,
      state: "idle",
      runCount: 0,
      ...e,
    } as Task
  })
  mkdirSync(join(h.home, "users", user), { recursive: true })
  writeFileSync(taskFile(h, user), JSON.stringify(list, null, 2))
  return ids
}

describe("定时调度", () => {
  test("到期入队：推进下次执行时间不受执行时长阻塞，结果写回来源会话", async () => {
    const h = setup()
    try {
      const sid = await createSession(h)
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "script", script: "echo ok", schedule: "@every 30m", name: "sync" }, sid)
      // 时间推进到超过原定触发点 → 到期
      h.clock.t += 31 * 60_000
      expect(internal(h, task.id).nextRunAt).toBeLessThan(h.clock.t)
      await h.tasks.tick()
      // 入队即推进（尚未执行完成也已推进，避免任务拖住后续调度）
      expect(internal(h, task.id).nextRunAt).toBeGreaterThan(h.clock.t)
      // 入队即被队列拉起（不等下个 tick）：入队事件已发布
      expect(h.published).toContain("event.task.queued")
      const done = await waitDone(h, task.id, 1)
      expect(done.runCount).toBe(1)
      expect(done.lastStatus).toBe("success")
      expect(done.lastError).toBeUndefined()
      // 脚本在任务资源目录执行
      expect(h.execCalls.map((c) => c.cmd)).toEqual(["echo ok"])
      expect(h.execCalls[0].cwd).toBe(join(h.home, "users", "default", "tasks", task.id))
      // 结果消息写回来源会话：user 角色 + engineNote='task'（尾 assistant 会被思考类模型 400 拒绝）
      const session = await h.store.load(sid, "default")
      const last = session!.messages.at(-1)!
      expect(last.role).toBe("user")
      expect(last.engineNote).toBe("task")
      expect(String(last.content)).toContain("【智体·定时任务「sync」执行结果（成功）】")
      expect(String(last.content)).toContain("out:echo ok")
      // 运行历史（按文件落盘，经 runs 读取）与事件
      const runs = await h.tasks.runs("default", task.id)
      expect(runs[0].status).toBe("success")
      expect(runs[0].sessionId).toBeUndefined()
      expect(h.published).toContain("event.task.queued")
      expect(h.published).toContain("event.task.start")
      expect(h.published).toContain("event.task.result")
      expect(h.published).toContain("event.task.queue")
      expect(h.finished).toEqual([{ taskId: task.id, status: "success" }])
      // 落盘状态与内存一致
      expect(readTasks(h)[0].lastStatus).toBe("success")
      expect(readTasks(h)[0].runCount).toBe(1)
    } finally {
      await cleanup(h)
    }
  })

  test("上次执行未结束时再次到期 → 记 skipped，不并发叠加、调度时间照常推进", async () => {
    const h = setup()
    try {
      h.runHang = true
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "prompt", prompt: "巡检", schedule: "@every 30m" })
      due(h, task.id)
      await h.tasks.tick()
      expect(internal(h, task.id).state).toBe("running")
      expect(h.runCalls).toHaveLength(1)
      // 31 分钟后再次到期（上次仍在跑）
      h.clock.t += 31 * 60_000
      await h.tasks.tick()
      expect(h.runCalls).toHaveLength(1) // 未启动第二次
      const e = internal(h, task.id)
      expect(e.lastStatus).toBe("skipped")
      const runs = await h.tasks.runs("default", task.id)
      expect(runs[0].status).toBe("skipped")
      expect(String(runs[0].reason)).toContain("尚未结束")
      expect(e.nextRunAt).toBeGreaterThan(h.clock.t)
      expect(h.tasks.queueView("default").entries).toHaveLength(0)
    } finally {
      await cleanup(h)
    }
  })

  test("已在队列中排队时再次到期 → 记 skipped，不重复入队", async () => {
    const h = setup({ maxConcurrent: 1 })
    try {
      h.runHang = true
      await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "占用额度" })
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "prompt", prompt: "S", schedule: "@every 30m" })
      due(h, task.id)
      await h.tasks.tick()
      expect(internal(h, task.id).state).toBe("queued")
      h.clock.t += 31 * 60_000
      await h.tasks.tick()
      expect(h.tasks.queueView("default").entries.filter((e) => e.taskId === task.id)).toHaveLength(1)
      expect(internal(h, task.id).lastStatus).toBe("skipped")
      const runs = await h.tasks.runs("default", task.id)
      expect(String(runs[0].reason)).toContain("已在队列")
    } finally {
      await cleanup(h)
    }
  })

  test("@at 一次性任务：到期入队后停用，不再进入调度", async () => {
    const h = setup()
    try {
      const at = new Date(h.clock.t + 60 * 60 * 1000).toISOString().slice(0, 16)
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "script", script: "echo once", schedule: `@at ${at}` })
      expect(task.enabled).toBe(true)
      due(h, task.id)
      await h.tasks.tick()
      expect(internal(h, task.id).nextRunAt).toBeUndefined() // 无未来触发点（不再参与到期检查）
      const e = await waitDone(h, task.id, 1)
      expect(e.enabled).toBe(false) // 执行结束自动停用
      expect(h.execCalls.map((c) => c.cmd)).toEqual(["echo once"])
      // 已停用：时间再推进也不再触发
      h.clock.t += 24 * 3600_000
      await h.tasks.tick()
      expect(h.execCalls).toHaveLength(1)
    } finally {
      await cleanup(h)
    }
  })

  test("misfire=run：启动后保留过期触发点，首个 tick 立即补跑一次", async () => {
    const h = setup()
    try {
      const [id] = makeTaskFile(h, [{ script: "echo catchup", schedule: "@every 1h", misfire: "run", nextRunAt: h.clock.t - 3600_000 }])
      await h.tasks.start()
      expect(internal(h, id).nextRunAt).toBeLessThan(h.clock.t) // 补跑策略：过期触发点保留
      await h.tasks.tick()
      await waitDone(h, id, 1)
      expect(h.execCalls.map((c) => c.cmd)).toEqual(["echo catchup"])
      expect(internal(h, id).nextRunAt).toBeGreaterThan(h.clock.t)
    } finally {
      await cleanup(h)
    }
  })

  test("缺省 misfire=skip：启动时不补跑，直接从当前重算下次执行", async () => {
    const h = setup()
    try {
      const [id] = makeTaskFile(h, [{ script: "echo skip", schedule: "@every 1h", nextRunAt: h.clock.t - 3600_000 }])
      await h.tasks.start()
      expect(internal(h, id).nextRunAt).toBeGreaterThan(h.clock.t)
      await h.tasks.tick()
      expect(h.execCalls).toHaveLength(0)
    } finally {
      await cleanup(h)
    }
  })

  test("加载时非法表达式/时区直接禁用（防每 30s 热循环重试）", async () => {
    const h = setup()
    try {
      const [bad, badTz] = makeTaskFile(h, [
        { schedule: "bad" },
        { schedule: "0 9 * * *", timezone: "Mars/Olympus" },
      ])
      await h.tasks.start()
      expect(internal(h, bad).enabled).toBe(false)
      expect(String(internal(h, bad).lastError)).toContain("非法")
      expect(internal(h, badTz).enabled).toBe(false)
      await h.tasks.tick()
      expect(h.execCalls).toHaveLength(0)
    } finally {
      await cleanup(h)
    }
  })

  test("执行失败：记录错误与退出码，连续失败达阈值自动停用", async () => {
    const h = setup()
    try {
      h.sandbox.exec = (async (cmd: string, o: { cwd?: string }) => {
        h.execCalls.push({ cmd, cwd: o.cwd ?? "" })
        return { stdout: "", stderr: "boom", code: 3 }
      }) as unknown as Sandbox["exec"]
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "script", script: "false", schedule: "@every 30m", maxConsecutiveErrors: 2 })
      due(h, task.id)
      await h.tasks.tick()
      let e = await waitDone(h, task.id, 1)
      expect(e.lastStatus).toBe("error")
      expect(e.lastError).toBe("exit 3")
      expect(e.consecutiveErrors).toBe(1)
      expect(e.enabled).toBe(true)
      expect(e.nextRunAt).toBeGreaterThan(h.clock.t) // 失败也照常推进（不卡在下个 tick 热循环）
      due(h, task.id)
      await h.tasks.tick()
      e = await waitDone(h, task.id, 2)
      expect(e.enabled).toBe(false)
      expect(e.consecutiveErrors).toBe(2)
      expect(String(e.lastError)).toContain("已自动停用")
      // 停用后不再触发
      h.clock.t += 3600_000
      await h.tasks.tick()
      expect(h.execCalls).toHaveLength(2)
    } finally {
      await cleanup(h)
    }
  })

  test("安全模式：脚本型任务跳过执行并留痕", async () => {
    const h = setup({ safeMode: true })
    try {
      const sid = await createSession(h)
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "script", script: "echo pwned", schedule: "@every 30m", name: "danger" }, sid)
      due(h, task.id)
      await h.tasks.tick()
      const e = await waitDone(h, task.id, 1)
      expect(h.execCalls).toHaveLength(0)
      expect(e.lastStatus).toBe("skipped")
      expect(e.lastError).toBe("safe-mode")
      const session = await h.store.load(sid, "default")
      expect(String(session!.messages.at(-1)!.content)).toContain("安全模式")
    } finally {
      await cleanup(h)
    }
  })

  test("prompt 型超时：windDown 快速结束后按 timeout 记录", async () => {
    const h = setup()
    try {
      h.runHang = true
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "prompt", prompt: "长任务", schedule: "@every 30m", timeoutMs: 1000 })
      due(h, task.id)
      await h.tasks.tick()
      const e = await waitDone(h, task.id, 1)
      expect(e.lastStatus).toBe("timeout")
      expect(String(e.lastError)).toContain("超时")
      expect((await h.tasks.runs("default", task.id))[0].status).toBe("timeout")
      expect(h.windDownCalls).toEqual([h.runCalls[0].sid])
    } finally {
      await cleanup(h)
    }
  })

  test("执行记录按条落盘（不受环形截断所限），保留上限按时间删最旧", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo hist", runNow: false })
      const total = TASK_RUNS_HISTORY + 2
      for (let i = 1; i <= total; i++) {
        await h.tasks.run("default", task.id)
        await waitDone(h, task.id, i)
      }
      // 定义文件不再内联记录；记录以「时间为名」存于 task-runs/{taskId}/
      const dir = join(h.home, "users", "default", "task-runs", task.id)
      const files = () => readdirSync(dir).filter((n) => n.endsWith(".json"))
      expect(files()).toHaveLength(total)
      expect(internal(h, task.id).runCount).toBe(total)
      expect(internal(h, task.id)).not.toHaveProperty("runs")
      const runs = await h.tasks.runs("default", task.id)
      expect(runs).toHaveLength(total)
      expect(runs.every((r) => r.status === "success")).toBe(true)
      expect(await h.tasks.runs("default", task.id, 2)).toHaveLength(2)
      // 保留上限：超出部分按时间删最旧（上限缺省 TASK_RUNS_KEEP）
      expect(await trimTaskRuns(h.home, "default", task.id, 2)).toBe(total - 2)
      expect(files()).toHaveLength(2)
      expect(TASK_RUNS_KEEP).toBeGreaterThan(TASK_RUNS_HISTORY)
    } finally {
      await cleanup(h)
    }
  }, 30_000)

  test("执行记录：读取上限、任务归属校验、定义文件不含 runs 字段", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo a" })
      await waitDone(h, task.id, 1)
      expect(await h.tasks.runs("default", task.id, 0)).toEqual([])
      // 其他用户不可读（任务不属于他）
      await expect(h.tasks.runs("someone-else", task.id)).rejects.toThrow(/任务不存在/)
      await expect(h.tasks.runs("default", "0".repeat(32))).rejects.toThrow(/任务不存在/)
      // 落盘的任务定义不再带 runs（执行记录只在 task-runs/ 下）
      expect(readTasks(h)[0]).not.toHaveProperty("runs")
    } finally {
      await cleanup(h)
    }
  })

  test("旧数据迁移：定义文件里的 runs 一次性导为执行记录文件，随后从定义中移除", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo legacy", runNow: false })
      const legacy = [
        { id: "r1", at: 1_780_000_000_100, endedAt: 1_780_000_001_100, status: "success", durationMs: 1000, output: "legacy-1" },
        { id: "r2", at: 1_780_000_000_000, endedAt: 1_780_000_000_500, status: "error", durationMs: 500, error: "exit 1" },
      ]
      // 模拟旧版定义文件（内联 runs）
      const raw = JSON.parse(readFileSync(taskFile(h), "utf8")) as Array<Record<string, unknown>>
      raw[0].runs = legacy
      writeFileSync(taskFile(h), JSON.stringify(raw, null, 2))
      h.tasks.stop()
      const restarted = new TaskManager({ ...h.deps })
      await restarted.start()
      expect((await restarted.runs("default", task.id)).map((r) => r.id)).toEqual(["r1", "r2"]) // 新→旧
      restarted.stop()
      // 迁移幂等：再次加载不产生重复记录
      const again = new TaskManager({ ...h.deps })
      await again.start()
      expect(await again.runs("default", task.id)).toHaveLength(2)
      again.stop()
      // 定义文件已剥离 runs
      expect(readTasks(h)[0]).not.toHaveProperty("runs")
    } finally {
      await cleanup(h)
    }
  })
})

describe("闲时任务", () => {
  test("队列空闲时串行执行：同时只跑一条，前一条结束自动起步下一条", async () => {
    const h = setup()
    try {
      h.runHang = true
      const a = await h.tasks.add("default", { kind: "idle", runner: "prompt", prompt: "闲时 A" })
      const b = await h.tasks.add("default", { kind: "idle", runner: "prompt", prompt: "闲时 B" })
      // 队列空闲：A 立即被拉起；B 等待（同时只跑一条）
      expect(h.runCalls).toHaveLength(1)
      expect(h.runCalls[0].prompt).toContain("闲时 A")
      expect(internal(h, a.id).state).toBe("running")
      expect(internal(h, b.id).state).not.toBe("running")
      expect(h.tasks.queueView("default").running).toHaveLength(1)
      // A 结束 → drain 立刻把下一条闲时任务拉起（不等下一次 tick）
      h.runResolvers.shift()?.()
      await waitFor(() => h.runCalls.length === 2)
      expect(h.runCalls[1].prompt).toContain("闲时 B")
      expect(internal(h, a.id).state).toBe("idle")
      expect(internal(h, b.id).state).toBe("running")
      h.runResolvers.shift()?.()
      await waitDone(h, b.id, 1)
    } finally {
      await cleanup(h)
    }
  })

  test("有定时/普通任务排队或运行时闲时任务让路（不抢额度）", async () => {
    const h = setup({ maxConcurrent: 1 })
    try {
      h.runHang = true
      const manual = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "普通任务" })
      const idle = await h.tasks.add("default", { kind: "idle", runner: "prompt", prompt: "闲时任务" })
      expect(h.runCalls).toHaveLength(1)
      expect(internal(h, manual.id).state).toBe("running")
      expect(internal(h, idle.id).state).toBe("idle") // 未进场（队列非空闲）
      expect(h.tasks.queueView("default").entries).toHaveLength(0)
      // 普通任务结束 → 空闲后闲时才入场执行
      h.runResolvers.shift()?.()
      await waitFor(() => h.runCalls.length === 2)
      expect(h.runCalls[1].prompt).toContain("闲时任务")
      expect(internal(h, idle.id).state).toBe("running")
    } finally {
      await cleanup(h)
    }
  })

  test("有排队中的普通任务时闲时不插队", async () => {
    const h = setup({ maxConcurrent: 1 })
    try {
      h.runHang = true
      const running = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "运行中" })
      const queued = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "排队中" })
      const idle = await h.tasks.add("default", { kind: "idle", runner: "prompt", prompt: "闲时" })
      expect(internal(h, queued.id).state).toBe("queued")
      expect(internal(h, idle.id).state).toBe("idle")
      // 释放额度：先跑排队中的普通任务，闲时仍让路
      h.runResolvers.shift()?.()
      await waitFor(() => h.runCalls.length === 2)
      expect(h.runCalls[1].prompt).toContain("排队中")
      expect(internal(h, idle.id).state).not.toBe("running")
      expect(internal(h, running.id).state).toBe("idle")
      // 普通任务全部结束后闲时才执行
      h.runResolvers.shift()?.()
      await waitFor(() => h.runCalls.length === 3)
      expect(h.runCalls[2].prompt).toContain("闲时")
    } finally {
      await cleanup(h)
    }
  })

  test("用户有运行中的会话时闲时不启动（用户优先）", async () => {
    const h = setup()
    try {
      h.busyUsers.add("default")
      const task = await h.tasks.add("default", { kind: "idle", runner: "prompt", prompt: "闲时待办" })
      expect(h.runCalls).toHaveLength(0)
      expect(internal(h, task.id).state).toBe("idle")
      await h.tasks.tick()
      expect(h.runCalls).toHaveLength(0)
      // 用户空闲后由 drain 拉起
      h.busyUsers.delete("default")
      await h.tasks.drain()
      await waitFor(() => h.runCalls.length === 1)
      expect(h.runCalls[0].prompt).toContain("闲时待办")
    } finally {
      await cleanup(h)
    }
  })

  test("停用的闲时任务不入场；重新启用后恢复执行", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", { kind: "idle", runner: "prompt", prompt: "闲时", enabled: false })
      expect(h.runCalls).toHaveLength(0)
      expect(internal(h, task.id).state).toBe("idle")
      await h.tasks.update("default", task.id, { enabled: true })
      await waitFor(() => h.runCalls.length === 1)
      await waitDone(h, task.id, 1)
      expect(internal(h, task.id).lastStatus).toBe("success")
    } finally {
      await cleanup(h)
    }
  })

  test("每用户额度缺省为 5（常量与队列视图一致）", async () => {
    const h = setup()
    try {
      expect(h.tasks.queueView("default").limit).toBe(TASK_MAX_CONCURRENT_DEFAULT)
      expect(TASK_MAX_CONCURRENT_DEFAULT).toBe(5)
    } finally {
      await cleanup(h)
    }
  })
})

describe("prompt 型执行目标与会话解析", () => {
  test("ephemeral（缺省）：每次执行新建会话，预载子Agent 写入装载名单", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "prompt", prompt: "总结今日", schedule: "@every 30m", name: "日报", agents: ["explore"] })
      due(h, task.id)
      await h.tasks.tick()
      await waitDone(h, task.id, 1)
      expect(h.runCalls[0].prompt).toContain("【智体·定时任务「日报」触发】")
      // 无人值守执行按无交互通道运行（本地模式需审批工具自动通过，不空等 5 分钟超时后跳过）
      expect(h.runCalls[0].interactionMode).toBe("none")
      expect(h.runCalls[0].prompt).toContain("无人值守执行")
      const sessions = await h.store.listSessions("default")
      const created = sessions.find((s) => s.name === "定时任务「日报」")!
      expect(created.loadedSubAgents).toEqual(["explore"])
      expect((await h.tasks.runs("default", task.id))[0].sessionId).toBe(created.id)
      // 再触发一次：另建新会话（ephemeral 不复用）
      due(h, task.id)
      await h.tasks.tick()
      await waitDone(h, task.id, 2)
      expect((await h.store.listSessions("default")).filter((s) => s.name === "定时任务「日报」")).toHaveLength(2)
    } finally {
      await cleanup(h)
    }
  })

  test("无人值守执行免审批（含服务模式）：prompt 任务以 autoApprove 运行、脚本任务注入任务级环境变量", async () => {
    const h = setup()
    try {
      // prompt 型无人值守：interactionMode=none + autoApprove=true（服务模式下同样自动通过需审批工具）
      const t = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "跑一次", name: "免审", runNow: false })
      await h.tasks.run("default", t.id)
      await waitDone(h, t.id, 1)
      expect(h.runCalls[0].interactionMode).toBe("none")
      expect(h.runCalls[0].autoApprove).toBe(true)
      expect(h.runCalls[0].prompt).toContain("需审批工具自动通过")

      // 脚本型：任务级 env 合并进子进程环境（高于会话/进程环境），两类执行体通用
      const s = await h.tasks.add("default", {
        kind: "manual",
        runner: "script",
        script: "echo $REPORT_DIR",
        env: { REPORT_DIR: "data/reports", GEBAI_LLM_MODEL: "task-model" },
      })
      await waitDone(h, s.id, 1)
      const env = h.execCalls.at(-1)!.env!
      expect(env.REPORT_DIR).toBe("data/reports")
      expect(env.GEBAI_LLM_MODEL).toBe("task-model")

      // prompt 型：任务级 env 作为执行会话的 envOverride 注入（模型/子Agent 一并生效）
      const p = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "带环境执行", env: { REPORT_DIR: "x" }, runNow: false })
      await h.tasks.run("default", p.id)
      await waitDone(h, p.id, 1)
      expect(h.runCalls.at(-1)!.envOverride).toEqual({ REPORT_DIR: "x" })

      // 绑定会话（可能有人在场）保持实时交互与实时审批，不叠加 autoApprove
      const sid = await createSession(h, "有人会话")
      const b = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "x", target: "session", sessionId: sid, runNow: false })
      await h.tasks.run("default", b.id)
      await waitDone(h, b.id, 1)
      expect(h.runCalls.at(-1)!.interactionMode).toBe("realtime")
      expect(h.runCalls.at(-1)!.autoApprove).toBeUndefined()
    } finally {
      await cleanup(h)
    }
  })

  test("任务级环境变量：敏感值加密落盘、非敏感明文、回显掩码、掩码回传保留原值", async () => {
    const h = setup()
    try {
      await expect(h.tasks.add("default", { kind: "manual", runner: "script", script: "echo", runNow: false, env: { "1BAD": "x" } })).rejects.toThrow(/无效的环境变量名/)
      await expect(h.tasks.add("default", { kind: "manual", runner: "script", script: "echo", runNow: false, env: { A: 1 as unknown as string } })).rejects.toThrow(/必须是字符串/)

      const t = await h.tasks.add("default", {
        kind: "manual",
        runner: "script",
        script: "echo",
        runNow: false,
        env: { API_TOKEN: "secret-value", PLAIN: "ok" },
      })
      // 磁盘：敏感键为密文（enc:v1: 前缀），非敏感键保持明文（便于直接查看任务配置）
      const onDisk = readTasks(h).find((x) => x.id === t.id)!.env!
      expect(onDisk.API_TOKEN).toMatch(/^enc:v1:/)
      expect(onDisk.API_TOKEN).not.toContain("secret-value")
      expect(onDisk.PLAIN).toBe("ok")
      // 内存态为明文（执行时脚本/Provider 需要真值）
      expect(internal(h, t.id).env).toEqual({ API_TOKEN: "secret-value", PLAIN: "ok" })
      const view = await h.tasks.get("default", t.id)
      // 回显：敏感键固定占位（与通知 secret 同惯例），非敏感键原样
      expect(view!.env!.PLAIN).toBe("ok")
      expect(view!.env!.API_TOKEN).toBe("***")
      // 掩码回传（前端表单不改动即原样回传）→ 保留原值，不被占位覆盖
      await h.tasks.update("default", t.id, { env: { API_TOKEN: "***", PLAIN: "changed" } })
      expect(internal(h, t.id).env).toEqual({ API_TOKEN: "secret-value", PLAIN: "changed" })
      // 显式改新值照常生效
      await h.tasks.update("default", t.id, { env: { API_TOKEN: "rotated" } })
      expect(internal(h, t.id).env).toEqual({ API_TOKEN: "rotated" })

      await h.tasks.update("default", t.id, { env: { SECOND: "2" } })
      expect(internal(h, t.id).env).toEqual({ SECOND: "2" })
      await h.tasks.update("default", t.id, { env: {} })
      expect(internal(h, t.id).env).toBeUndefined()
      // 重启加载：密文解密回明文；外部编辑损坏的条目整条丢弃（不阻断任务加载）
      await h.tasks.update("default", t.id, { env: { KEEP_TOKEN: "keep-secret" } })
      const file = taskFile(h)
      const bad = { ...internal(h, t.id), id: "b".repeat(32), env: { "1BAD": "x" } }
      writeFileSync(file, JSON.stringify([...JSON.parse(readFileSync(file, "utf8")), bad]))
      h.tasks.stop()
      const mgr = new TaskManager({ ...h.deps, home: h.home })
      await mgr.start()
      try {
        expect(mgr["entries"].get(t.id)!.env).toEqual({ KEEP_TOKEN: "keep-secret" })
        expect(mgr["entries"].get("b".repeat(32))!.env).toBeUndefined()
      } finally {
        mgr.stop()
      }
    } finally {
      await cleanup(h)
    }
  })

  test("sticky：专用会话惰性创建并跨次复用", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "延续上下文", target: "sticky", name: "长任务" })
      await waitDone(h, task.id, 1)
      const stickyId = internal(h, task.id).stickySessionId!
      expect(stickyId).toMatch(/^[0-9a-f]{32}$/)
      await h.tasks.run("default", task.id)
      await waitDone(h, task.id, 2)
      expect(internal(h, task.id).stickySessionId).toBe(stickyId)
      expect(h.runCalls.map((c) => c.sid)).toEqual([stickyId, stickyId])
      // sticky 同为无人值守形态（专用会话不面向人）
      expect(h.runCalls.every((c) => c.interactionMode === "none")).toBe(true)
      expect((await h.store.listSessions("default")).filter((s) => s.name === "普通任务「长任务」")).toHaveLength(1)
    } finally {
      await cleanup(h)
    }
  })

  test("session：绑定既有会话执行；绑定会话被删除则自愈降级为独立会话", async () => {
    const h = setup()
    try {
      const sid = await createSession(h, "目标会话")
      const task = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "在绑定会话执行", target: "session", sessionId: sid, runNow: false })
      await h.tasks.run("default", task.id)
      await waitDone(h, task.id, 1)
      expect(h.runCalls[0].sid).toBe(sid)
      // 绑定用户会话可能有人在场（可当场批准需审批工具），保持实时交互姿态
      expect(h.runCalls[0].interactionMode).toBe("realtime")
      expect(h.runCalls[0].prompt).not.toContain("无人值守执行")
      // 绑定会话被删除：下一次执行自愈降级为新建会话
      await h.store.delete(sid, "default")
      await h.tasks.run("default", task.id)
      await waitDone(h, task.id, 2)
      expect(h.runCalls[1].sid).not.toBe(sid)
      expect(h.runCalls[1].interactionMode).toBe("none")
      expect(internal(h, task.id).target).toBe("ephemeral")
      const session = await h.store.load(h.runCalls[1].sid, "default")
      expect(session?.name).toContain("普通任务")
    } finally {
      await cleanup(h)
    }
  })

  test("目标会话忙：条目留在队列等待（不占额度、不打断），空闲后自动起步", async () => {
    const h = setup()
    try {
      const sid = await createSession(h, "目标会话")
      const task = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "等待会话", target: "session", sessionId: sid, runNow: false })
      h.busySessions.add(sid)
      await h.tasks.run("default", task.id)
      // 会话忙 → 留队等待，不启动
      expect(h.runCalls).toHaveLength(0)
      expect(internal(h, task.id).state).toBe("queued")
      const view = h.tasks.queueView("default")
      expect(view.entries[0].waiting).toContain("正在运行")
      expect(view.running).toHaveLength(0)
      // 会话空闲 → 下一轮 drain 起步
      h.busySessions.delete(sid)
      await h.tasks.drain()
      await waitFor(() => h.runCalls.length === 1)
      expect(h.runCalls[0].sid).toBe(sid)
      await waitDone(h, task.id, 1)
    } finally {
      await cleanup(h)
    }
  })

  test("脚本型任务无引擎也可执行（任务不依赖会话存活）", async () => {
    const h = setup()
    try {
      // 引擎未就绪（构造期只给 shell）：脚本型任务照常执行
      h.tasks["engine"] = undefined
      const task = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo standalone" })
      const e = await waitDone(h, task.id, 1)
      expect(e.lastStatus).toBe("success")
      expect(h.execCalls.map((c) => c.cmd)).toEqual(["echo standalone"])
      // 提示词型任务则记错误（不静默假成功）
      const p = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "无引擎" })
      const pe = await waitDone(h, p.id, 1)
      expect(pe.lastStatus).toBe("error")
      expect(String(pe.lastError)).toContain("引擎未就绪")
    } finally {
      await cleanup(h)
    }
  })
})

describe("持久化、重启恢复与旧数据迁移", () => {
  test("旧 cron.json 一次性迁移：字段同义搬移 + 文件改名 .migrated.bak + 工作目录并入", async () => {
    const h = setup()
    try {
      const legacyId = randomUUID().replace(/-/g, "")
      const legacy = [
        {
          id: legacyId,
          user: "default",
          name: "旧定时任务",
          type: "script",
          schedule: "0 9 * * *",
          script: "bash run.sh",
          timeoutMs: 60_000,
          enabled: true,
          createdAt: h.clock.t - 1000,
          updatedAt: h.clock.t - 1000,
          nextRunAt: h.clock.t + 3600_000,
          runCount: 3,
          lastStatus: "success",
          runs: [],
        },
      ]
      mkdirSync(join(h.home, "users", "default"), { recursive: true })
      writeFileSync(join(h.home, "users", "default", "cron.json"), JSON.stringify(legacy, null, 2))
      const oldWorkspace = join(h.home, "users", "default", "cron-workspace", legacyId)
      mkdirSync(oldWorkspace, { recursive: true })
      writeFileSync(join(oldWorkspace, "run.sh"), "echo hi\n")

      await h.tasks.start()
      const tasks = await h.tasks.list("default")
      expect(tasks).toHaveLength(1)
      const t = tasks[0]
      expect(t.id).toBe(legacyId)
      expect(t.kind).toBe("scheduled")
      expect(t.runner).toBe("script")
      expect(t.name).toBe("旧定时任务")
      expect(t.script).toBe("bash run.sh")
      expect(t.timeoutMs).toBe(60_000)
      expect(t.runCount).toBe(3)
      expect(t.state).toBe("idle")
      // 旧文件改名保留，任务落到 tasks.json
      expect(existsSync(join(h.home, "users", "default", "cron.json"))).toBe(false)
      expect(existsSync(join(h.home, "users", "default", "cron.json.migrated.bak"))).toBe(true)
      expect(existsSync(taskFile(h))).toBe(true)
      // 脚本工作目录并入任务资源目录（既有产物不丢）
      expect(readFileSync(join(h.home, "users", "default", "tasks", legacyId, "run.sh"), "utf8")).toBe("echo hi\n")
      // 迁移后任务可执行（脚本在资源目录内）
      h.sandbox.exec = (async (cmd: string, o: { cwd?: string }) => {
        h.execCalls.push({ cmd, cwd: o.cwd ?? "" })
        return { stdout: "ok", stderr: "", code: 0 }
      }) as unknown as Sandbox["exec"]
      await h.tasks.run("default", legacyId)
      await waitDone(h, legacyId, 4)
      expect(h.execCalls[0].cwd).toBe(join(h.home, "users", "default", "tasks", legacyId))
    } finally {
      await cleanup(h)
    }
  })

  test("迁移幂等：已有 tasks.json 时不再读取 cron.json", async () => {
    const h = setup()
    try {
      mkdirSync(join(h.home, "users", "default"), { recursive: true })
      writeFileSync(join(h.home, "users", "default", "cron.json"), JSON.stringify([{ id: "a".repeat(32), user: "default", type: "script", schedule: "@every 1h", script: "echo legacy", enabled: true, createdAt: 1, updatedAt: 1, nextRunAt: h.clock.t + 3600_000, runCount: 0 }]))
      writeFileSync(taskFile(h), JSON.stringify([]))
      await h.tasks.start()
      expect(await h.tasks.list("default")).toHaveLength(0)
      expect(existsSync(join(h.home, "users", "default", "cron.json"))).toBe(true) // 未触碰旧文件
    } finally {
      await cleanup(h)
    }
  })

  test("重启恢复：queued 条目重新入队执行，running 标记中断不自动重跑", async () => {
    const h = setup()
    try {
      const [queued, running] = makeTaskFile(h, [
        { kind: "manual", runner: "prompt", prompt: "排队中的任务", state: "queued", queue: { source: "manual", enqueuedAt: h.clock.t - 1000 } },
        { kind: "manual", runner: "prompt", prompt: "运行中的任务", state: "running", startedAt: h.clock.t - 1000 },
      ])
      await h.tasks.start()
      // running → 标记中断（不自动重跑）
      const r = internal(h, running)
      expect(r.state).toBe("idle")
      expect(r.startedAt).toBeUndefined()
      expect(r.lastStatus).toBe("error")
      expect(String(r.lastError)).toContain("已中断")
      // queued → 恢复入队并执行
      expect(internal(h, queued).state === "queued" || internal(h, queued).state === "running").toBe(true)
      await h.tasks.drain()
      await waitFor(() => h.runCalls.length === 1)
      expect(h.runCalls[0].prompt).toContain("排队中的任务")
      await waitDone(h, queued, 1)
      // 中断那条不会被自动执行
      expect(h.runCalls.map((c) => c.prompt).join()).not.toContain("运行中的任务")
    } finally {
      await cleanup(h)
    }
  })

  test("多实例共库：不同管理器实例的写入不互相抹掉；删除只影响目标条目", async () => {
    const h = setup()
    try {
      const second = new TaskManager({
        home: h.home,
        store: new SessionStore({ home: h.home }),
        env: new EnvManager(h.store),
        sandbox: h.sandbox,
        events: new EventBus(),
        now: () => h.clock.t,
        tickIntervalMs: 3600_000,
      })
      await h.tasks.start()
      await second.start()
      const a = await h.tasks.add("default", { kind: "scheduled", runner: "script", script: "echo A", schedule: "@every 1h", name: "A任务", runNow: false })
      const b = await second.add("default", { kind: "scheduled", runner: "script", script: "echo B", schedule: "@every 1h", name: "B任务" })
      expect(readTasks(h).map((t) => t.name)).toEqual(["A任务", "B任务"])
      expect(await second.remove("default", a.id)).toBe(true)
      expect(readTasks(h).map((t) => t.name)).toEqual(["B任务"])
      expect(await second.get("default", b.id)).not.toBeNull()
      second.stop()
    } finally {
      await cleanup(h)
    }
  })

  test("文件落盘为完整 JSON（原子写）", async () => {
    const h = setup()
    try {
      await h.tasks.add("default", { kind: "scheduled", runner: "script", script: "echo x", schedule: "@every 1h" })
      expect(() => JSON.parse(readFileSync(taskFile(h), "utf8"))).not.toThrow()
    } finally {
      await cleanup(h)
    }
  })
})

describe("任务资源文件", () => {
  test("list/read/write/delete：嵌套路径、目录项与越界拒绝", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "script", script: "bash run.sh", schedule: "@every 1h" })
      const id = task.id
      const written = await h.tasks.writeFile("default", id, "scripts/run.sh", "echo run\n")
      expect(written.path).toBe("scripts/run.sh")
      expect(written.size).toBe(9)
      await h.tasks.writeFile("default", id, "notes.md", "# 说明\n")
      const list = await h.tasks.files("default", id)
      expect(list.map((f) => `${f.dir ? "d" : "f"}:${f.path}`)).toEqual(["f:notes.md", "d:scripts", "f:scripts/run.sh"])
      expect(await h.tasks.readFile("default", id, "scripts/run.sh")).toBe("echo run\n")
      expect(await h.tasks.deleteFile("default", id, "notes.md")).toBe(true)
      expect(await h.tasks.deleteFile("default", id, "notes.md")).toBe(false)
      expect(await h.tasks.deleteFile("default", id, "scripts")).toBe(true) // 目录递归删除
      expect(await h.tasks.files("default", id)).toEqual([])
      // 越界与非法操作
      await expect(h.tasks.writeFile("default", id, "../escape.txt", "x")).rejects.toThrow(/越界/)
      await expect(h.tasks.readFile("default", id, "../../etc/passwd")).rejects.toThrow(/越界/)
      await expect(h.tasks.deleteFile("default", id, "..")).rejects.toThrow(/越界/)
      await expect(h.tasks.writeFile("default", id, "", "x")).rejects.toThrow(/缺少文件路径/)
      await expect(h.tasks.deleteFile("default", id, "")).rejects.toThrow(/不能删除任务资源目录本身/)
      await expect(h.tasks.readFile("default", id, "nope.txt")).rejects.toThrow(/不存在/)
      await expect(h.tasks.files("default", "0".repeat(32))).rejects.toThrow(/任务不存在/)
      // 资源目录不因任务删除而清理（产物保留）
      const ws = join(h.home, "users", "default", "tasks", id)
      await h.tasks.writeFile("default", id, "keep.txt", "keep")
      await h.tasks.remove("default", id)
      expect(existsSync(join(ws, "keep.txt"))).toBe(true)
    } finally {
      await cleanup(h)
    }
  })

  test("写入超过上限拒绝", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", { kind: "scheduled", runner: "script", script: "echo", schedule: "@every 1h" })
      await expect(h.tasks.writeFile("default", task.id, "big.txt", "x".repeat(TASK_FILE_MAX_BYTES + 1))).rejects.toThrow(/过大/)
    } finally {
      await cleanup(h)
    }
  })
})

describe("通知投递", () => {
  test("webhook 通道：载荷含任务信息与 at 名单，配 secret 时带签名", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", {
        kind: "manual",
        runner: "script",
        script: "echo notify",
        notify: [{ type: "webhook", target: "https://example.com/hook", secret: "s3cret", at: [{ id: "ou_abc", name: "张三" }] }],
      })
      await waitDone(h, task.id, 1)
      await waitFor(() => h.notifyPosts.length === 1)
      const post = h.notifyPosts[0]
      expect(post.url).toBe("https://example.com/hook")
      expect(post.body.event).toBe("task.result")
      expect(post.body.ok).toBe(true)
      expect(post.body.status).toBe("success")
      expect((post.body.task as { name?: string }).name).toBe("")
      expect(post.body.output).toContain("out:echo notify")
      expect(post.headers?.["X-Gebai-Signature"]).toMatch(/^sha256=[0-9a-f]{64}$/)
      expect(post.body.at).toEqual([{ id: "ou_abc", name: "张三" }])
    } finally {
      await cleanup(h)
    }
  })

  test("feishu_chat 通道走应用消息（at 含 all 时降级 text）", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", {
        kind: "manual",
        runner: "script",
        script: "echo feishu",
        notify: [{ type: "feishu_chat", target: "oc_abcdef1234567890abcdef", at: [{ id: "all" }] }],
      })
      await waitDone(h, task.id, 1)
      await waitFor(() => h.feishuSent.length === 1)
      expect(h.feishuSent[0].chatId).toBe("oc_abcdef1234567890abcdef")
      expect(h.feishuSent[0].msgType).toBe("text")
      expect(String((h.feishuSent[0].content as { text: string }).text)).toContain('<at user_id="all">所有人</at>')
    } finally {
      await cleanup(h)
    }
  })

  test("notifyOn=auto（缺省）：成功与失败都自动投递；未知值按缺省 auto 处理", async () => {
    const h = setup()
    try {
      const task = await h.tasks.add("default", {
        kind: "manual",
        runner: "script",
        script: "echo ok",
        notify: [{ type: "webhook", target: "https://example.com/hook" }],
      })
      await waitDone(h, task.id, 1)
      await waitFor(() => h.notifyPosts.length === 1)
      expect(h.notifyPosts[0].body.ok).toBe(true)
      // 失败同样自动投递（不再有「仅失败」这一档）
      h.sandbox.exec = (async () => ({ stdout: "", stderr: "boom", code: 1 })) as unknown as Sandbox["exec"]
      await h.tasks.run("default", task.id)
      await waitDone(h, task.id, 2)
      await waitFor(() => h.notifyPosts.length === 2)
      expect(h.notifyPosts[1].body.ok).toBe(false)
      expect(h.notifyPosts[1].body.error).toBe("exit 1")
      // 非法/陈构值不入库（读取归一为缺省 auto）
      const bogus = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo x", notifyOn: "error" as never, runNow: false })
      expect(bogus.notifyOn).toBeUndefined()
    } finally {
      await cleanup(h)
    }
  })

  test("全局默认通道回落：任务未配 notify 时使用；任务自配则不叠加", async () => {
    const h = setup({ defaultNotify: [{ type: "webhook", target: "https://default.example.com/hook" }] })
    try {
      const a = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo a" })
      await waitDone(h, a.id, 1)
      await waitFor(() => h.notifyPosts.length === 1)
      expect(h.notifyPosts[0].url).toBe("https://default.example.com/hook")
      const b = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo b", notify: [{ type: "webhook", target: "https://own.example.com/hook" }] })
      await waitDone(h, b.id, 1)
      await waitFor(() => h.notifyPosts.length === 2)
      expect(h.notifyPosts[1].url).toBe("https://own.example.com/hook")
    } finally {
      await cleanup(h)
    }
  })

  test("webhookId 引用：投递时解析注册 Webhook，失效则记 lastNotifyError 不影响执行", async () => {
    const h = setup()
    try {
      const wid = "b".repeat(32)
      h.webhookRegistry.set(wid, { url: "https://registered.example.com/hook", secret: "reg-secret" })
      const task = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo ref", notify: [{ type: "webhook", webhookId: wid }] })
      await waitDone(h, task.id, 1)
      await waitFor(() => h.notifyPosts.length === 1)
      expect(h.notifyPosts[0].url).toBe("https://registered.example.com/hook")
      expect(h.notifyPosts[0].headers?.["X-Gebai-Signature"]).toMatch(/^sha256=/)
      // 引用失效（被删除）：任务仍成功，记通知错误
      h.webhookRegistry.delete(wid)
      await h.tasks.run("default", task.id)
      await waitDone(h, task.id, 2)
      expect(internal(h, task.id).lastStatus).toBe("success")
      await waitFor(() => String(internal(h, task.id).lastNotifyError ?? "").includes("webhook 引用不可用"))
    } finally {
      await cleanup(h)
    }
  })

  test("创建时拒绝无权引用的 webhookId", async () => {
    const h = setup()
    try {
      h.webhookRegistry.set("c".repeat(32), { url: "https://x.example.com/h", owner: "someone-else" })
      await expect(h.tasks.add("default", { kind: "manual", runner: "script", script: "echo", notify: [{ type: "webhook", webhookId: "c".repeat(32) }], runNow: false })).rejects.toThrow(/无权引用/)
      await expect(h.tasks.add("default", { kind: "manual", runner: "script", script: "echo", notify: [{ type: "webhook", webhookId: "d".repeat(32) }], runNow: false })).rejects.toThrow(/不存在/)
    } finally {
      await cleanup(h)
    }
  })

  test("安全模式：通知不投递并留痕", async () => {
    const h = setup({ safeMode: true })
    try {
      const task = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo x", notify: [{ type: "webhook", target: "https://example.com/hook" }] })
      const e = await waitDone(h, task.id, 1)
      expect(h.notifyPosts).toHaveLength(0)
      await waitFor(() => String(internal(h, task.id).lastNotifyError ?? "").includes("安全模式"))
      expect(String(e.lastNotifyError)).toContain("安全模式")
    } finally {
      await cleanup(h)
    }
  })
})

describe("主动通知（task_notify）", () => {
  test("未指定任务 id：按执行会话推断运行中的任务并投递自撰正文；notifyOn=model 不自动投递结果", async () => {
    const h = setup()
    try {
      h.agentNames.push("task")
      h.runHang = true
      const task = await h.tasks.add("default", {
        kind: "manual",
        runner: "prompt",
        prompt: "巡检磁盘",
        notify: [{ type: "webhook", target: "https://example.com/hook" }],
        notifyOn: "model",
      })
      await waitFor(() => h.runCalls.length === 1)
      const sid = h.runCalls[0].sid
      // 有通知通道 → 执行会话自动预载 task（否则执行中的模型拿不到 task_notify），触发消息带任务 ID 与用法提示
      expect((await h.store.load(sid, "default"))?.loadedSubAgents).toContain("task")
      expect(h.runCalls[0].prompt).toContain(task.id)
      expect(h.runCalls[0].prompt).toContain("task_notify")

      const res = await h.tasks.notify("default", undefined, { text: "磁盘占用 92%", title: "巡检告警" }, { sessionId: sid })
      expect(res).toEqual({ taskId: task.id, delivered: 1, errors: [] })
      await waitFor(() => h.notifyPosts.length === 1)
      expect(h.notifyPosts[0].body.event).toBe("task.message")
      expect(h.notifyPosts[0].body.text).toBe("磁盘占用 92%")
      expect(h.notifyPosts[0].body.title).toBe("巡检告警")
      expect((h.notifyPosts[0].body.task as { id: string }).id).toBe(task.id)

      // 收尾：model 模式不自动投递结果通知（消息条数保持 1）
      h.runResolvers.splice(0).forEach((f) => f())
      await waitDone(h, task.id, 1)
      await new Promise((r) => setTimeout(r, 20))
      expect(h.notifyPosts).toHaveLength(1)
    } finally {
      await cleanup(h)
    }
  })

  test("无可用通道 / 未指定任务 / 空正文 / 非法 at：明确报错", async () => {
    const h = setup()
    try {
      const bare = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo", notifyOn: "model" })
      await waitDone(h, bare.id, 1)
      await expect(h.tasks.notify("default", bare.id, { text: "hi" })).rejects.toThrow(/未配置通知通道/)
      await expect(h.tasks.notify("default", undefined, { text: "hi" }, { sessionId: "s-none" })).rejects.toThrow(/未指定任务 ID/)
      await expect(h.tasks.notify("default", "0".repeat(32), { text: "hi" })).rejects.toThrow(/任务不存在/)
      const withCh = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo", notify: [{ type: "webhook", target: "https://example.com/hook" }] })
      await waitDone(h, withCh.id, 1)
      await expect(h.tasks.notify("default", withCh.id, { text: "   " })).rejects.toThrow(/正文不能为空/)
      await expect(h.tasks.notify("default", withCh.id, { text: "x", at: ["bad-id"] })).rejects.toThrow(/无效的 @ 对象 id/)
    } finally {
      await cleanup(h)
    }
  })

  test("全局默认通道回落、at 覆盖、webhookId 解析与投递失败留痕", async () => {
    const h = setup({ defaultNotify: [{ type: "webhook", target: "https://default.example.com/hook" }] })
    try {
      const task = await h.tasks.add("default", { kind: "manual", runner: "script", script: "echo", notifyOn: "model" })
      await waitDone(h, task.id, 1)
      const res = await h.tasks.notify("default", task.id, { text: "报表已生成", at: ["ou_abc"] })
      expect(res.delivered).toBe(1)
      expect(h.notifyPosts[0].url).toBe("https://default.example.com/hook")
      expect(h.notifyPosts[0].body.at).toEqual([{ id: "ou_abc" }])

      // webhookId 引用：投递时解析为注册 URL 并带签名
      const wid = "e".repeat(32)
      h.webhookRegistry.set(wid, { url: "https://registered.example.com/hook", secret: "reg" })
      const ref = await h.tasks.add("default", {
        kind: "manual",
        runner: "script",
        script: "echo",
        notifyOn: "model",
        notify: [{ type: "webhook", webhookId: wid }],
      })
      await waitDone(h, ref.id, 1)
      h.notifyPosts.length = 0
      const r2 = await h.tasks.notify("default", ref.id, { text: "x" })
      expect(r2.delivered).toBe(1)
      expect(h.notifyPosts[0].url).toBe("https://registered.example.com/hook")
      expect(h.notifyPosts[0].headers?.["X-Gebai-Signature"]).toMatch(/^sha256=/)

      // 引用失效：尽力而为返回 errors 并留痕，不影响任务与其他通道
      h.webhookRegistry.delete(wid)
      const r3 = await h.tasks.notify("default", ref.id, { text: "y" })
      expect(r3.delivered).toBe(0)
      expect(r3.errors.join("；")).toContain("webhook 引用不可用")
      expect(String(internal(h, ref.id).lastNotifyError)).toContain("webhook 引用不可用")
    } finally {
      await cleanup(h)
    }
  })

  test("安全模式：主动通知拒绝", async () => {
    const h = setup({ safeMode: true })
    try {
      const task = await h.tasks.add("default", {
        kind: "manual",
        runner: "script",
        script: "echo",
        notifyOn: "model",
        notify: [{ type: "webhook", target: "https://example.com/hook" }],
      })
      await waitDone(h, task.id, 1)
      await expect(h.tasks.notify("default", task.id, { text: "hi" })).rejects.toThrow(/安全模式/)
      expect(h.notifyPosts).toHaveLength(0)
    } finally {
      await cleanup(h)
    }
  })
})
