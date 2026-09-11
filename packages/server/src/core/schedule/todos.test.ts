import { describe, expect, test } from "bun:test"
import { randomUUID } from "node:crypto"
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { SessionStore } from "../session/store"
import type { AgentEngine } from "../engine/engine"
import { IDLE_TODO_MAX_ATTEMPTS, UserTodoManager, type UserTodo } from "./todos"

/** 用户级待办与闲时任务（core/schedule/todos.ts）单测：CRUD/排序持久化、空闲调度、串行、失败放弃、超时。
 *  范式同 cron.test.ts：注入 now/tickIntervalMs 与 fake AgentEngine，直接 await tick() 而非等真实定时器。 */

interface Harness {
  home: string
  store: SessionStore
  todos: UserTodoManager
  /** 虚拟时钟（测试推进）。 */
  clock: { t: number }
  runCalls: Array<{ sid: string; user: string; prompt: string }>
  cancelCalls: string[]
  /** 服务端是否有运行中的会话（engine.busy() 返回值）。 */
  busy: boolean
  /** run 挂起（模拟长任务，配合超时用例）。 */
  runHang: boolean
  /** run 抛错（模拟执行失败）。 */
  runFail: string | null
  /** run 成功后是否写入一条 assistant 回复（结果摘要来源）。 */
  appendReply: boolean
  settle: Array<() => void>
}

function setup(opts: { now?: number; tickIntervalMs?: number; timeoutMs?: number; maxAttempts?: number } = {}): Harness {
  const home = mkdtempSync(join(tmpdir(), "gebai-todos-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  const store = new SessionStore({ home })
  const h: Harness = {
    home,
    store,
    todos: null as unknown as UserTodoManager,
    clock: { t: opts.now ?? 1_780_000_000_000 },
    runCalls: [],
    cancelCalls: [],
    busy: false,
    runHang: false,
    runFail: null,
    appendReply: true,
    settle: [],
  }
  const fakeEngine = {
    busy: () => h.busy,
    cancel: (sid: string) => {
      h.cancelCalls.push(sid)
      // 模拟真实引擎：cancel 让挂起中的 run settle（abort 传播）
      h.settle.splice(0).forEach((r) => r())
    },
    run: async (sid: string, user: string, prompt: string) => {
      h.runCalls.push({ sid, user, prompt })
      if (h.runHang) await new Promise<void>((resolve) => h.settle.push(resolve))
      if (h.runFail) throw new Error(h.runFail)
      if (h.appendReply) {
        await store.appendMessage(sid, { id: randomUUID(), role: "assistant", content: "闲时任务已完成：示例结果", createdAt: h.clock.t }, user)
      }
    },
  } as unknown as AgentEngine
  h.todos = new UserTodoManager({
    home,
    store,
    engine: fakeEngine,
    now: () => h.clock.t,
    tickIntervalMs: opts.tickIntervalMs ?? 3600_000,
    timeoutMs: opts.timeoutMs ?? 30 * 60 * 1000,
    maxAttempts: opts.maxAttempts ?? IDLE_TODO_MAX_ATTEMPTS,
  })
  return h
}

function cleanup(h: Harness) {
  h.todos.stop()
  rmSync(h.home, { recursive: true, force: true })
}

function todoFile(h: Harness, user = "default"): string {
  return join(h.home, "users", user, "todos.json")
}

function readFile(h: Harness, user = "default"): UserTodo[] {
  return JSON.parse(readFileSync(todoFile(h, user), "utf8")) as UserTodo[]
}

describe("用户待办 CRUD 与持久化", () => {
  test("新增/修改/删除/排序落盘，顺序即清单顺序", async () => {
    const h = setup()
    try {
      const a = await h.todos.add("default", { text: "写周报" })
      const b = await h.todos.add("default", { text: "整理截图", idle: true })
      const c = await h.todos.add("default", { text: "更新文档" })
      expect((await h.todos.list("default")).map((t) => t.text)).toEqual(["写周报", "整理截图", "更新文档"])
      expect([a.done, b.idle, c.idle]).toEqual([false, true, false])
      expect(b.idleState).toBe("pending")

      const done = await h.todos.update("default", a.id, { done: true })
      expect(done?.done).toBe(true)
      const renamed = await h.todos.update("default", c.id, { text: "更新设计文档" })
      expect(renamed?.text).toBe("更新设计文档")

      const reordered = await h.todos.reorder("default", [c.id, b.id, a.id])
      expect(reordered.map((t) => t.id)).toEqual([c.id, b.id, a.id])
      expect(readFile(h).map((t) => t.id)).toEqual([c.id, b.id, a.id])

      expect(await h.todos.remove("default", b.id)).toBe(true)
      expect(await h.todos.remove("default", b.id)).toBe(false)
      expect(readFile(h).map((t) => t.text)).toEqual(["更新设计文档", "写周报"])
    } finally {
      cleanup(h)
    }
  })

  test("空文本与超长文本拒绝；待办按用户隔离", async () => {
    const h = setup()
    try {
      await expect(h.todos.add("default", { text: "   " })).rejects.toThrow("待办内容不能为空")
      await expect(h.todos.add("default", { text: "x".repeat(2100) })).rejects.toThrow("过长")
      await h.todos.add("default", { text: "甲的待办" })
      await h.todos.add("bob", { text: "乙的待办" })
      expect((await h.todos.list("default")).map((t) => t.text)).toEqual(["甲的待办"])
      expect((await h.todos.list("bob")).map((t) => t.text)).toEqual(["乙的待办"])
      // 跨用户不可见/不可改
      const mine = (await h.todos.list("default"))[0]
      expect(await h.todos.update("bob", mine.id, { done: true })).toBeNull()
      expect(await h.todos.remove("bob", mine.id)).toBe(false)
    } finally {
      cleanup(h)
    }
  })

  test("启动加载外部编辑的 todos.json，损坏条目跳过", async () => {
    const h = setup()
    try {
      await h.todos.add("default", { text: "保留项" })
      h.todos.stop()
      const file = todoFile(h)
      const raw = JSON.parse(readFileSync(file, "utf8")) as unknown[]
      raw.push({ id: "not-a-hex-id", user: "default", text: "损坏项" })
      raw.push({ id: randomUUID().replace(/-/g, ""), user: "default", text: "外部新增" })
      writeFileSync(file, JSON.stringify(raw))
      await h.todos.start()
      const list = await h.todos.list("default")
      expect(list.map((t) => t.text)).toEqual(["保留项", "外部新增"])
    } finally {
      cleanup(h)
    }
  })
})

describe("闲时任务调度", () => {
  test("服务端有运行中会话（busy）时不执行；空闲后按顺序执行并自动完成", async () => {
    const h = setup()
    try {
      const first = await h.todos.add("default", { text: "闲时一", idle: true })
      await h.todos.add("default", { text: "闲时二", idle: true })
      await h.todos.add("default", { text: "普通待办" })

      h.busy = true
      await h.todos.tick()
      expect(h.runCalls).toHaveLength(0)

      h.busy = false
      await h.todos.tick()
      // 一次 tick 只启动一条（串行），且只跑标记为闲时的条目
      expect(h.runCalls).toHaveLength(1)
      expect(h.runCalls[0].prompt).toContain("[闲时待办任务]")
      expect(h.runCalls[0].prompt).toContain("闲时一")
      expect(h.runCalls[0].user).toBe("default")

      const after = (await h.todos.list("default")).find((t) => t.id === first.id)!
      expect(after.done).toBe(true)
      expect(after.idleState).toBe("done")
      expect(after.idleAttempts).toBe(1)
      expect(after.idleResult).toBe("闲时任务已完成：示例结果")
      expect(after.idleSessionId).toBe(h.runCalls[0].sid)
      // 执行会话已建立，且标题带待办摘要
      const session = await h.store.load(h.runCalls[0].sid, "default")
      expect(session?.name).toContain("闲时待办")
      expect(session?.name).toContain("闲时一")

      // 下一条（第二位）在下次 tick 执行
      await h.todos.tick()
      expect(h.runCalls).toHaveLength(2)
      expect(h.runCalls[1].prompt).toContain("闲时二")
      const list = await h.todos.list("default")
      expect(list.every((t) => (t.idle ? t.done : !t.done))).toBe(true)
      // 仅执行了两条闲时待办（普通待办不动）
      expect(list.find((t) => t.text === "普通待办")!.done).toBe(false)
    } finally {
      cleanup(h)
    }
  })

  test("执行失败累计次数，达上限后停止自动执行；重新开启闲时任务可重试", async () => {
    const h = setup({ maxAttempts: 2 })
    try {
      const t = await h.todos.add("default", { text: "总是失败", idle: true })
      h.runFail = "模型不可用"
      await h.todos.tick()
      let cur = (await h.todos.list("default")).find((x) => x.id === t.id)!
      expect(cur.idleState).toBe("pending")
      expect(cur.idleAttempts).toBe(1)
      expect(cur.idleError).toContain("模型不可用")

      await h.todos.tick()
      cur = (await h.todos.list("default")).find((x) => x.id === t.id)!
      expect(cur.idleState).toBe("failed")
      expect(cur.idleAttempts).toBe(2)

      // 已放弃：后续 tick 不再执行
      const before = h.runCalls.length
      await h.todos.tick()
      expect(h.runCalls).toHaveLength(before)

      // 重新开启（关闭再开启）重置计数并可再次排队
      await h.todos.update("default", t.id, { idle: false })
      await h.todos.update("default", t.id, { idle: true })
      cur = (await h.todos.list("default")).find((x) => x.id === t.id)!
      expect(cur.idleState).toBe("pending")
      expect(cur.idleAttempts).toBe(0)
      await h.todos.tick()
      expect(h.runCalls).toHaveLength(before + 1)
    } finally {
      cleanup(h)
    }
  })

  test("执行超时：取消执行会话并按失败计次", async () => {
    const h = setup({ timeoutMs: 20 })
    try {
      const t = await h.todos.add("default", { text: "长任务", idle: true })
      h.runHang = true
      const running = h.todos.tick()
      await new Promise((r) => setTimeout(r, 60))
      await running
      const cur = (await h.todos.list("default")).find((x) => x.id === t.id)!
      expect(h.cancelCalls).toHaveLength(1)
      expect(cur.idleState).toBe("pending")
      expect(cur.idleError).toContain("超时")
    } finally {
      cleanup(h)
    }
  })

  test("无闲时待办或未注入引擎时 tick 不产生任何执行", async () => {
    const h = setup()
    try {
      await h.todos.add("default", { text: "普通" })
      await h.todos.tick()
      expect(h.runCalls).toHaveLength(0)
      expect(existsSync(todoFile(h))).toBe(true)
    } finally {
      cleanup(h)
    }
  })
})
