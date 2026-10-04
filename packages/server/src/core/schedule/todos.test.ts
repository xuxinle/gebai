import { describe, expect, test } from "bun:test"
import { randomUUID } from "node:crypto"
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { Task, TaskRunRecord } from "@gebai/sdk"
import { EventBus } from "../base/event-bus"
import type { AgentEngine } from "../engine/engine"
import { Sandbox } from "../security/sandbox"
import { EnvManager } from "../session/env"
import { SessionStore } from "../session/store"
import { TaskManager } from "./tasks"
import { TODO_IDLE_TIMEOUT_MS, TODO_MAX_ITEMS, TODO_TEXT_MAX, UserTodoManager, type UserTodo } from "./todos"

/**
 * 用户级待办（core/schedule/todos.ts）单测：清单 CRUD 与排序持久化、闲时自动执行的绑定联动
 * （建/删/校正绑定的闲时任务）、手动执行入队（复用绑定任务或建一次性任务）、
 * 执行结果回写（自动勾选 / 失败关闭闲时运行并停用绑定任务）、启动加载与多实例 RMW。
 *
 * 范式同 tasks.test.ts：注入 now（虚拟时钟）与 fake AgentEngine，任务执行由真实 TaskManager
 * （maxConcurrent 限 1）驱动，断言执行收尾状态时用 waitFor 轮询而非等真实定时器。
 */

interface Harness {
  home: string
  store: SessionStore
  tasks: TaskManager
  todos: UserTodoManager
  /** 虚拟时钟（测试推进）。 */
  clock: { t: number }
  runCalls: Array<{ sid: string; user: string; prompt: string }>
  /** 挂起中的 engine.run 解挂回调（runHang 时登记）。 */
  runResolvers: Array<() => void>
  runHang: boolean
  runFail: string | null
  appendReply: boolean
  /** engine.busyUser 为真的用户（闲时调度的「用户优先」判定）。
   *  缺省视为该用户有运行中的交互会话——闲时任务让路（用例只关注绑定语义时不被自动执行干扰）；
   *  需要验证闲时自动执行的用例自行 busyUsers.delete("default")。 */
  busyUsers: Set<string>
}

function setup(opts: { now?: number; maxConcurrent?: number } = {}): Harness {
  const home = mkdtempSync(join(tmpdir(), "gebai-todos-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  const store = new SessionStore({ home })
  const sandbox = new Sandbox({ home, enabled: false })
  const env = new EnvManager(store)
  const events = new EventBus()
  const h: Harness = {
    home,
    store,
    tasks: null as unknown as TaskManager,
    todos: null as unknown as UserTodoManager,
    clock: { t: opts.now ?? 1_780_000_000_000 },
    runCalls: [],
    runResolvers: [],
    runHang: false,
    runFail: null,
    appendReply: true,
    busyUsers: new Set(["default"]),
  }
  const fakeEngine = {
    isRunning: () => false,
    busyUser: (user: string) => h.busyUsers.has(user),
    cancel: async (sid: string) => {
      h.runResolvers.splice(0).forEach((f) => f())
      void sid
    },
    windDown: async (sid: string) => {
      h.runResolvers.splice(0).forEach((f) => f())
      void sid
    },
    run: async (sid: string, user: string, prompt: string) => {
      h.runCalls.push({ sid, user, prompt })
      if (h.runHang) await new Promise<void>((resolve) => h.runResolvers.push(resolve))
      if (h.runFail) throw new Error(h.runFail)
      if (h.appendReply) {
        await store.appendMessage(sid, { id: randomUUID(), role: "assistant", content: "待办执行完成：示例结果", createdAt: h.clock.t }, user)
      }
    },
    setTasks: () => {},
  } as unknown as AgentEngine
  const tasks = new TaskManager({
    home,
    store,
    env,
    sandbox,
    events,
    engine: fakeEngine,
    now: () => h.clock.t,
    tickIntervalMs: 3600_000,
    maxConcurrent: opts.maxConcurrent ?? 1,
  })
  const todos = new UserTodoManager({ home, store, tasks, now: () => h.clock.t })
  // 执行结果回写链路（生产接线：任务调度器收尾 → 待办回写）
  tasks.onFinished((task: Task, run: TaskRunRecord) => todos.recordTaskResult(task, run))
  h.tasks = tasks
  h.todos = todos
  return h
}

async function cleanup(h: Harness): Promise<void> {
  h.tasks.stop()
  // 解挂起中的执行（避免遗留 pending 的 run 在临时目录已删除后写回）
  h.runResolvers.splice(0).forEach((f) => f())
  await new Promise((r) => setTimeout(r, 20))
  // 在途收尾写盘可能仍占着目录（Windows EBUSY）——重试等句柄释放
  rmSync(h.home, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 })
}

function todoFile(h: Harness, user = "default"): string {
  return join(h.home, "users", user, "todos.json")
}

function readTodos(h: Harness, user = "default"): UserTodo[] {
  return JSON.parse(readFileSync(todoFile(h, user), "utf8")) as UserTodo[]
}

/** 轮询等待断言成立（执行是异步发出，队列推进不阻塞返回）。 */
async function waitFor(fn: () => boolean | Promise<boolean>, timeoutMs = 2000): Promise<void> {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (await fn()) return
    await new Promise((r) => setTimeout(r, 10))
  }
}

function entryOf(list: UserTodo[], id: string): UserTodo {
  const e = list.find((x) => x.id === id)
  if (!e) throw new Error(`待办不存在: ${id}`)
  return e
}

describe("待办清单 CRUD 与持久化", () => {
  test("新增/修改/删除/排序落盘，数组顺序即清单顺序", async () => {
    const h = setup()
    try {
      const a = await h.todos.add("default", { text: "写周报" })
      const b = await h.todos.add("default", { text: "整理截图", idle: true })
      const c = await h.todos.add("default", { text: "更新文档" })
      expect((await h.todos.list("default")).map((t) => t.text)).toEqual(["写周报", "整理截图", "更新文档"])
      expect(readTodos(h).map((t) => t.id)).toEqual([a.id, b.id, c.id])

      await h.todos.update("default", c.id, { text: "更新设计文档" })
      expect(entryOf(await h.todos.list("default"), c.id).text).toBe("更新设计文档")

      const reordered = await h.todos.reorder("default", [c.id, a.id, b.id])
      expect(reordered.map((t) => t.id)).toEqual([c.id, a.id, b.id])
      // 只列出一条：未列出的按当前磁盘原序（c,a）追加在后，不丢
      await h.todos.reorder("default", [b.id])
      expect((await h.todos.list("default")).map((t) => t.id)).toEqual([b.id, c.id, a.id])

      expect(await h.todos.remove("default", a.id)).toBe(true)
      expect(readTodos(h).map((t) => t.id)).toEqual([b.id, c.id])
      expect(await h.todos.remove("default", a.id)).toBe(false)
    } finally {
      await cleanup(h)
    }
  })

  test("内容校验：空内容与超长拒绝；跨用户与不存在条目不可见", async () => {
    const h = setup()
    try {
      await expect(h.todos.add("default", { text: "   " })).rejects.toThrow(/不能为空/)
      await expect(h.todos.add("default", { text: "x".repeat(TODO_TEXT_MAX + 1) })).rejects.toThrow(/过长/)
      const t = await h.todos.add("default", { text: "隔离检查" })
      expect(await h.todos.update("other", t.id, { done: true })).toBeNull()
      expect(await h.todos.remove("other", t.id)).toBe(false)
      expect(await h.todos.list("other")).toEqual([])
    } finally {
      await cleanup(h)
    }
  })

  test("条数上限以磁盘真值判定（外部塞满后新增被拒绝）", async () => {
    const h = setup()
    try {
      const full: UserTodo[] = Array.from({ length: TODO_MAX_ITEMS }, (_, i) => ({
        id: randomUUID().replace(/-/g, ""),
        user: "default",
        text: `item-${i}`,
        done: false,
        idle: false,
        createdAt: h.clock.t,
        updatedAt: h.clock.t,
      }))
      writeFileSync(todoFile(h), JSON.stringify(full, null, 2), "utf8")
      await expect(h.todos.add("default", { text: "溢出" })).rejects.toThrow(/上限/)
    } finally {
      await cleanup(h)
    }
  })

  test("多实例共库：各自新增互不覆盖（写入以磁盘真值为基准合并）", async () => {
    const h = setup()
    try {
      const other = new UserTodoManager({ home: h.home, store: h.store, tasks: h.tasks, now: () => h.clock.t })
      const a = await h.todos.add("default", { text: "实例 A" })
      const b = await other.add("default", { text: "实例 B" })
      expect(readTodos(h).map((t) => t.id)).toEqual([a.id, b.id])
      // A 镜像虽未感知 B（list 读内存镜像），但下一次写入必须保留 B 的条目而非整体覆盖
      const c = await h.todos.add("default", { text: "实例 A2" })
      expect(readTodos(h).map((t) => t.id)).toEqual([a.id, b.id, c.id])
      expect((await h.todos.list("default")).map((t) => t.id)).toEqual([a.id, b.id, c.id])
    } finally {
      await cleanup(h)
    }
  })
})

describe("闲时自动执行的绑定联动", () => {
  test("开启闲时自动执行 → 创建绑定闲时任务（todoId/prompt 对应）；关闭 → 任务删除", async () => {
    const h = setup()
    try {
      const t = await h.todos.add("default", { text: "闲时跑一遍", idle: true })
      const boundId = entryOf(await h.todos.list("default"), t.id).idleTaskId
      expect(boundId).toMatch(/^[0-9a-f]{32}$/)
      const task = await h.tasks.get("default", boundId!)
      expect(task?.kind).toBe("idle")
      expect(task?.todoId).toBe(t.id)
      expect(task?.prompt).toBe("闲时跑一遍")
      expect(task?.runner).toBe("prompt")
      expect(task?.timeoutMs).toBe(TODO_IDLE_TIMEOUT_MS) // 待办闲时缺省 3 小时超时
      expect(task?.enabled).toBe(true)
      expect(h.todos.boundIdleTaskIds("default")).toEqual([boundId!])

      await h.todos.update("default", t.id, { idle: false })
      expect(entryOf(await h.todos.list("default"), t.id).idleTaskId).toBeUndefined()
      expect(await h.tasks.get("default", boundId!)).toBeNull()
      expect(h.todos.boundIdleTaskIds("default")).toEqual([])
    } finally {
      await cleanup(h)
    }
  })

  test("文本变更同步提示词；勾选完成停用绑定任务；重新开启重置失败计数", async () => {
    const h = setup()
    try {
      const t = await h.todos.add("default", { text: "初版内容", idle: true })
      const boundId = entryOf(await h.todos.list("default"), t.id).idleTaskId!

      await h.todos.update("default", t.id, { text: "改版内容" })
      expect((await h.tasks.get("default", boundId))?.prompt).toBe("改版内容")

      await h.todos.update("default", t.id, { done: true })
      expect((await h.tasks.get("default", boundId))?.enabled).toBe(false)

      // 失败计次后重开闲时：计数与状态清零
      await h.todos.update("default", t.id, { done: false })
      const seeded = await h.todos.update("default", t.id, { idle: false })
      expect(seeded).not.toBeNull()
      await h.todos.update("default", t.id, { idle: true })
      const cur = entryOf(await h.todos.list("default"), t.id)
      expect(cur.idleState).toBe("pending")
      expect(cur.idleAttempts ?? 0).toBe(0)
      expect(cur.idleError).toBeUndefined()
    } finally {
      await cleanup(h)
    }
  })

  test("用户空闲时闲时任务自动执行：成功回写待办并停用绑定任务", async () => {
    const h = setup()
    try {
      h.busyUsers.delete("default") // 用户无运行中的会话 → 闲时任务可执行
      const t = await h.todos.add("default", { text: "空闲时跑一遍", idle: true })
      const boundId = entryOf(await h.todos.list("default"), t.id).idleTaskId!
      await waitFor(async () => (entryOf(await h.todos.list("default"), t.id).done === true))
      expect(h.runCalls).toHaveLength(1)
      expect(h.runCalls[0].prompt).toContain("空闲时跑一遍")
      const cur = entryOf(await h.todos.list("default"), t.id)
      expect(cur.idleState).toBe("done")
      expect(cur.idleResult).toBe("待办执行完成：示例结果")
      expect((await h.tasks.get("default", boundId))?.enabled).toBe(false)
    } finally {
      await cleanup(h)
    }
  })

  test("删除待办连带删除绑定任务；绑定任务排队顺序按清单顺序", async () => {
    const h = setup()
    try {
      const a = await h.todos.add("default", { text: "第一件", idle: true })
      const b = await h.todos.add("default", { text: "第二件", idle: true })
      const list = await h.todos.list("default")
      const idA = entryOf(list, a.id).idleTaskId!
      const idB = entryOf(list, b.id).idleTaskId!
      expect(h.todos.boundIdleTaskIds("default")).toEqual([idA, idB])

      // 排序后绑定顺序随之变化（闲时执行顺序 = 清单顺序）
      await h.todos.reorder("default", [b.id, a.id])
      expect(h.todos.boundIdleTaskIds("default")).toEqual([idB, idA])

      await h.todos.remove("default", a.id)
      expect(await h.tasks.get("default", idA)).toBeNull()
      expect(await h.tasks.get("default", idB)).not.toBeNull()
    } finally {
      await cleanup(h)
    }
  })
})
describe("手动执行（入队）", () => {
  test("无绑定任务的待办：建一次性普通任务入队（执行完自动清理）", async () => {
    const h = setup()
    try {
      const t = await h.todos.add("default", { text: "立刻跑的任务\n含多行说明" })
      const res = await h.todos.run("default", t.id)
      expect(res).not.toBeNull()
      expect(res!.queued).toBe(true)
      expect(res!.ephemeral).toBe(true)
      expect(res!.taskId).toMatch(/^[0-9a-f]{32}$/)
      const task = await h.tasks.get("default", res!.taskId)
      expect(task?.kind).toBe("manual")
      expect(task?.runner).toBe("prompt")
      expect(task?.prompt).toBe("立刻跑的任务\n含多行说明")
      expect(task?.todoId).toBe(t.id)
      expect(task?.ephemeral).toBe(true)

      // 执行（maxConcurrent=1 下入队即启动）→ 回写待办并清理一次性任务
      await waitFor(async () => (entryOf(await h.todos.list("default"), t.id).done === true))
      expect(h.runCalls).toHaveLength(1)
      expect(h.runCalls[0].prompt).toContain("立刻跑的任务")
      const cur = entryOf(await h.todos.list("default"), t.id)
      expect(cur.idleResult).toBe("待办执行完成：示例结果")
      expect(cur.idleSessionId).toMatch(/^[0-9a-f]{32}$/)
      // 未开启闲时自动执行的待办不携带闲时状态
      expect(cur.idleState).toBeUndefined()
      expect(await h.tasks.get("default", res!.taskId)).toBeNull()
    } finally {
      await cleanup(h)
    }
  })

  test("已绑定闲时任务的待办：复用该任务入队（不新建），停用后执行自动恢复启用", async () => {
    const h = setup()
    try {
      const t = await h.todos.add("default", { text: "闲时条目", idle: true })
      const boundId = entryOf(await h.todos.list("default"), t.id).idleTaskId!
      // 勾选完成 → 绑定任务停用
      await h.todos.update("default", t.id, { done: true })
      expect((await h.tasks.get("default", boundId))?.enabled).toBe(false)

      // 用户空闲后手动执行（闲时类任务在用户有运行中会话时让路，故此处放开）
      h.busyUsers.delete("default")
      const res = await h.todos.run("default", t.id)
      expect(res!.taskId).toBe(boundId)
      expect(res!.ephemeral).toBe(false)
      expect((await h.tasks.get("default", boundId))?.enabled).toBe(true)

      await waitFor(() => h.runCalls.length === 1)
      // 等**收尾链走完**再断言：任务停用（enabled=false）发生在前段，待办回写（idleState）在其后——
      // 只等前者会读到回写前状态（与其他用例同款按目标状态轮询）
      await waitFor(async () => entryOf(await h.todos.list("default"), t.id).idleState === "done")
      const cur = entryOf(await h.todos.list("default"), t.id)
      expect(cur.done).toBe(true)
      expect(cur.idleState).toBe("done")
      expect(cur.idleResult).toBe("待办执行完成：示例结果")
      expect(h.runCalls[0].prompt).toContain("闲时条目")
    } finally {
      await cleanup(h)
    }
  })

  test("待办不存在返回 null；任务能力未启用时抛错", async () => {
    const h = setup()
    try {
      expect(await h.todos.run("default", "0".repeat(32))).toBeNull()
      const bare = new UserTodoManager({ home: h.home, store: h.store, now: () => h.clock.t })
      const t = await bare.add("default", { text: "无任务能力", idle: false })
      await expect(bare.run("default", t.id)).rejects.toThrow(/任务能力未启用/)
    } finally {
      await cleanup(h)
    }
  })

  test("执行失败回写待办（不静默吞掉）：闲时条目关闭 idle，普通条目只记因", async () => {
    const h = setup()
    try {
      const t = await h.todos.add("default", { text: "会失败的待办" })
      h.runFail = "模型不可用"
      await h.todos.run("default", t.id)
      await waitFor(async () => ((entryOf(await h.todos.list("default"), t.id).idleAttempts ?? 0) > 0))
      const cur = entryOf(await h.todos.list("default"), t.id)
      expect(cur.done).toBe(false)
      expect(cur.idleAttempts).toBe(1)
      expect(cur.idleError).toContain("模型不可用")
      // 未开启闲时自动执行的条目：失败只记因，不涉及 idle 开关
      expect(cur.idle).toBe(false)
    } finally {
      await cleanup(h)
    }
  })
})

describe("执行结果回写（recordTaskResult）", () => {
  test("成功：自动勾选、记结果与会话、停用绑定任务", async () => {
    const h = setup()
    try {
      const t = await h.todos.add("default", { text: "回写成功", idle: true })
      const boundId = entryOf(await h.todos.list("default"), t.id).idleTaskId!
      const run: TaskRunRecord = {
        id: randomUUID(),
        at: h.clock.t,
        endedAt: h.clock.t + 1000,
        status: "success",
        durationMs: 1000,
        output: "执行摘要",
        sessionId: "a".repeat(32),
      }
      await h.todos.recordTaskResult((await h.tasks.get("default", boundId))!, run)
      const cur = entryOf(await h.todos.list("default"), t.id)
      expect(cur.done).toBe(true)
      expect(cur.idleState).toBe("done")
      expect(cur.idleResult).toBe("执行摘要")
      expect(cur.idleSessionId).toBe("a".repeat(32))
      expect(cur.idleAttempts).toBe(1)
      expect((await h.tasks.get("default", boundId))?.enabled).toBe(false)
    } finally {
      await cleanup(h)
    }
  })

  test("失败：关闭闲时运行（idle=false）并停用绑定任务；重新开启后继续执行", async () => {
    const h = setup()
    try {
      const t = await h.todos.add("default", { text: "回写失败", idle: true })
      const boundId = entryOf(await h.todos.list("default"), t.id).idleTaskId!
      const fail: TaskRunRecord = {
        id: randomUUID(),
        at: h.clock.t,
        endedAt: h.clock.t + 500,
        status: "error",
        durationMs: 500,
        error: "脚本退出码 1",
      }
      await h.todos.recordTaskResult((await h.tasks.get("default", boundId))!, fail)
      let cur = entryOf(await h.todos.list("default"), t.id)
      expect(cur.idleAttempts).toBe(1)
      expect(cur.idle).toBe(false) // 失败即关闭闲时运行（不重试）
      expect(cur.idleState).toBe("failed")
      expect(cur.idleError).toContain("脚本退出码 1")
      expect(cur.idleError).toContain("重新开启")
      expect((await h.tasks.get("default", boundId))?.enabled).toBe(false)

      // 用户重新开启 ⚡：状态与计数重置、绑定任务复用并恢复启用，下次队列空闲继续执行
      h.runFail = null
      h.busyUsers.delete("default")
      cur = (await h.todos.update("default", t.id, { idle: true }))!
      expect(cur.idle).toBe(true)
      expect(cur.idleState).toBe("pending")
      expect(cur.idleAttempts ?? 0).toBe(0)
      expect(cur.idleTaskId).toBe(boundId) // 复用原绑定任务
      await waitFor(async () => entryOf(await h.todos.list("default"), t.id).done === true)
      expect(h.runCalls.at(-1)?.prompt).toContain("回写失败")
    } finally {
      await cleanup(h)
    }
  })

  test("非待办任务（无 todoId）与跨用户回写不产生副作用", async () => {
    const h = setup()
    try {
      const standalone = await h.tasks.add("default", { kind: "manual", runner: "prompt", prompt: "独立任务", runNow: false })
      await h.todos.recordTaskResult(standalone, { id: randomUUID(), at: h.clock.t, endedAt: h.clock.t, status: "error", durationMs: 0 })
      const t = await h.todos.add("default", { text: "归属校验", idle: true })
      const boundId = entryOf(await h.todos.list("default"), t.id).idleTaskId!
      const foreign = { ...(await h.tasks.get("default", boundId))!, user: "other" }
      await h.todos.recordTaskResult(foreign, { id: randomUUID(), at: h.clock.t, endedAt: h.clock.t, status: "success", durationMs: 0 })
      const cur = entryOf(await h.todos.list("default"), t.id)
      expect(cur.done).toBe(false)
      expect(cur.idleAttempts ?? 0).toBe(0)
    } finally {
      await cleanup(h)
    }
  })
})

describe("启动加载", () => {
  test("加载后复位执行中状态、为开启闲时自动执行的条目补齐绑定任务", async () => {
    const h = setup()
    try {
      const idleItem: UserTodo = {
        id: randomUUID().replace(/-/g, ""),
        user: "default",
        text: "需要补绑定",
        done: false,
        idle: true,
        createdAt: h.clock.t,
        updatedAt: h.clock.t,
        idleState: "running",
        idleAttempts: 2,
      }
      const plain: UserTodo = {
        id: randomUUID().replace(/-/g, ""),
        user: "default",
        text: "普通条目",
        done: false,
        idle: false,
        createdAt: h.clock.t,
        updatedAt: h.clock.t,
      }
      writeFileSync(todoFile(h), JSON.stringify([idleItem, plain], null, 2), "utf8")
      // 新管理器实例从磁盘加载（同一 tasks 实例避免重复调度器）
      const loaded = new UserTodoManager({ home: h.home, store: h.store, tasks: h.tasks, now: () => h.clock.t })
      await loaded.start()
      const list = await loaded.list("default")
      expect(list.map((t) => t.id)).toEqual([idleItem.id, plain.id])
      const cur = entryOf(list, idleItem.id)
      expect(cur.idleState).toBe("pending") // 上次进程遗留的「执行中」复位
      expect(cur.idleAttempts).toBe(2)
      expect(cur.idleTaskId).toMatch(/^[0-9a-f]{32}$/)
      expect(await h.tasks.get("default", cur.idleTaskId!)).not.toBeNull()
      expect(await h.tasks.get("default", plain.id)).toBeNull() // 未开启闲时：不建任务
    } finally {
      await cleanup(h)
    }
  })

  test("损坏条目在加载时丢弃，不影响其余清单", async () => {
    const h = setup()
    try {
      writeFileSync(
        todoFile(h),
        JSON.stringify(
          [
            { id: "not-a-valid-id", user: "default", text: "坏 id" },
            { id: randomUUID().replace(/-/g, ""), user: "default", text: "", done: false, idle: false },
            { id: randomUUID().replace(/-/g, ""), user: "default", text: "正常条目", done: false, idle: false },
          ],
          null,
          2,
        ),
        "utf8",
      )
      const loaded = new UserTodoManager({ home: h.home, store: h.store, tasks: h.tasks, now: () => h.clock.t })
      await loaded.start()
      expect((await loaded.list("default")).map((t) => t.text)).toEqual(["正常条目"])
    } finally {
      await cleanup(h)
    }
  })
})
