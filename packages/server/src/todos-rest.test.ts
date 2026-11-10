import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { existsSync, mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"

let handle: ServerHandle
const home = mkdtempSync(join(tmpdir(), "gebai-todos-rest-"))

beforeAll(async () => {
  handle = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
})

afterAll(() => {
  handle.gc?.stop()
  handle.tasks?.stop()
  handle.todos?.stop()
  handle.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

function base() {
  return `http://127.0.0.1:${handle.server.port}`
}

function json(body: unknown): RequestInit {
  return { headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }
}

describe("todos REST（用户级待办管理面）", () => {
  test("初始为空；新增落盘用户级 todos.json", async () => {
    expect(await (await fetch(`${base()}/api/v1/todos`)).json()).toEqual([])
    const res = await fetch(`${base()}/api/v1/todos`, { method: "POST", ...json({ text: "买牛奶" }) })
    expect(res.status).toBe(201)
    const created = (await res.json()) as { id: string; done: boolean; idle: boolean }
    expect(created.id).toMatch(/^[a-f0-9]{32}$/)
    expect([created.done, created.idle]).toEqual([false, false])
    expect(existsSync(join(home, "users", "admin", "todos.json")) || existsSync(join(home, "users", "default", "todos.json"))).toBe(true)
    const listed = (await (await fetch(`${base()}/api/v1/todos`)).json()) as Array<{ text: string }>
    expect(listed.map((t) => t.text)).toEqual(["买牛奶"])
  })

  test("新增校验：空文本/超长文本 400", async () => {
    const empty = await fetch(`${base()}/api/v1/todos`, { method: "POST", ...json({ text: "   " }) })
    expect(empty.status).toBe(400)
    expect(String(((await empty.json()) as { error: string }).error)).toContain("不能为空")
    const long = await fetch(`${base()}/api/v1/todos`, { method: "POST", ...json({ text: "x".repeat(2100) }) })
    expect(long.status).toBe(400)
  })

    test("修改（文本/完成/闲时）与删除；闲时自动执行绑定/解绑闲时任务；非法 id 400、不存在 404", async () => {
    const created = (await (await fetch(`${base()}/api/v1/todos`, { method: "POST", ...json({ text: "写周报", idle: true }) })).json()) as { id: string; idleState?: string; idleTaskId?: string }
    expect(created.idleState).toBe("pending")
    // 开启闲时自动执行 → 绑定一个闲时任务（任务清单里可见，todoId 反向关联）
    expect(String(created.idleTaskId)).toMatch(/^[a-f0-9]{32}$/)
    const bound = (await (await fetch(`${base()}/api/v1/tasks?kind=idle`)).json()) as Array<{ id: string; todoId?: string; enabled: boolean; prompt?: string }>
    const mine = bound.find((t) => t.id === created.idleTaskId)
    // 只断言**绑定关系**（todoId 反向关联 + 内容 + 启用）：不断言整份任务对象的其余字段——
    // 子集匹配以外的字段（runCount/state/lastStatus 等）取决于闲时调度器是否已把它跑过，
    // 而那是**另一个异步子系统**的时序（并行满载下实例待办清单空闲即会被拉起）。
    expect(mine).toBeDefined()
    expect(mine).toMatchObject({ todoId: created.id, enabled: true, prompt: "写周报" })

    const patched = (await (
      await fetch(`${base()}/api/v1/todos/${created.id}`, { method: "PATCH", ...json({ text: "写月报", done: true, idle: false }) })
    ).json()) as { text: string; done: boolean; idle: boolean; idleState?: string; idleTaskId?: string }
    expect([patched.text, patched.done, patched.idle]).toEqual(["写月报", true, false])
    expect(patched.idleState).toBeUndefined()
    // 关闭闲时自动执行 → 绑定任务随之删除（不残留无人认领的闲时任务）
    expect(patched.idleTaskId).toBeUndefined()
    expect((await fetch(`${base()}/api/v1/tasks/${created.idleTaskId}`)).status).toBe(404)

    expect((await fetch(`${base()}/api/v1/todos/not-hex`, { method: "PATCH", ...json({ done: true }) })).status).toBe(400)
    expect((await fetch(`${base()}/api/v1/todos/${ "0".repeat(32) }`, { method: "PATCH", ...json({ done: true }) })).status).toBe(404)

    expect((await fetch(`${base()}/api/v1/todos/${created.id}`, { method: "DELETE" })).status).toBe(200)
    expect((await fetch(`${base()}/api/v1/todos/${created.id}`, { method: "DELETE" })).status).toBe(404)
  })

  test("拖动排序落库（PATCH /api/v1/todos { ids }）", async () => {
    // 清空既有待办（同一服务实例跨用例共享用户清单），保证顺序断言简单
    for (const t of (await (await fetch(`${base()}/api/v1/todos`)).json()) as Array<{ id: string }>) {
      await fetch(`${base()}/api/v1/todos/${t.id}`, { method: "DELETE" })
    }
    const ids: string[] = []
    for (const text of ["一", "二", "三"]) {
      const r = (await (await fetch(`${base()}/api/v1/todos`, { method: "POST", ...json({ text }) })).json()) as { id: string }
      ids.push(r.id)
    }
    const reordered = (await (
      await fetch(`${base()}/api/v1/todos`, { method: "PATCH", ...json({ ids: [ids[2], ids[0], ids[1]] }) })
    ).json()) as Array<{ id: string }>
    expect(reordered.map((t) => t.id)).toEqual([ids[2], ids[0], ids[1]])
    // 重新读取：顺序持久化
    const listed = (await (await fetch(`${base()}/api/v1/todos`)).json()) as Array<{ id: string }>
    expect(listed.map((t) => t.id)).toEqual([ids[2], ids[0], ids[1]])
    // 清理：后续用例断言更简单
    for (const id of ids) await fetch(`${base()}/api/v1/todos/${id}`, { method: "DELETE" })
  })

  test("能力关闭（GEBAI_IDLE_TODO_ENABLED=false）返回 503", async () => {
    const home2 = mkdtempSync(join(tmpdir(), "gebai-todos-rest-off-"))
    const h2 = await startServer({ gebaiHome: home2, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], idleTodoEnabled: false, port: 0 })
    try {
      const res = await fetch(`http://127.0.0.1:${h2.server.port}/api/v1/todos`)
      expect(res.status).toBe(503)
      expect(h2.todos).toBeNull()
    } finally {
      h2.gc?.stop()
      h2.server.stop(true)
      rmSync(home2, { recursive: true, force: true })
    }
  })

    test("立即执行（POST /api/v1/todos/:id/run）：入队跑一次（统一队列，不占任务清单）", async () => {
    const created = (await (await fetch(`${base()}/api/v1/todos`, { method: "POST", ...json({ text: "【REST】执行：写一份周报" }) })).json()) as { id: string }
    const ran = await fetch(`${base()}/api/v1/todos/${created.id}/run`, { method: "POST" })
    expect(ran.status).toBe(200)
    const body = (await ran.json()) as { todo: { id: string }; queued: boolean; position?: number; taskId: string; ephemeral: boolean; reason?: string }
    expect(body.todo.id).toBe(created.id)
    expect(body.queued).toBe(true)
    expect(body.taskId).toMatch(/^[a-f0-9]{32}$/)
    // 无绑定任务：建一次性任务入队执行（执行结束自动删除，任务清单不被一次性执行塞满）
    expect(body.ephemeral).toBe(true)
    expect(body.position === undefined || body.position >= 1).toBe(true)
    // 非法 id 400 / 不存在 404
    expect((await fetch(`${base()}/api/v1/todos/not-hex/run`, { method: "POST" })).status).toBe(400)
    expect((await fetch(`${base()}/api/v1/todos/${ "0".repeat(32) }/run`, { method: "POST" })).status).toBe(404)
    await fetch(`${base()}/api/v1/todos/${created.id}`, { method: "DELETE" })
  })

  test("任务能力关闭（GEBAI_TASKS_ENABLED=false）：待办仍可管理，手动执行 503、闲时不绑定任务", async () => {
    const home3 = mkdtempSync(join(tmpdir(), "gebai-todos-rest-notasks-"))
    const h3 = await startServer({ gebaiHome: home3, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], tasksEnabled: false, port: 0 })
    try {
      const b = `http://127.0.0.1:${h3.server.port}`
      const created = (await (await fetch(`${b}/api/v1/todos`, { method: "POST", ...json({ text: "无任务能力也能记" }) })).json()) as { id: string }
      expect((await fetch(`${b}/api/v1/todos/${created.id}/run`, { method: "POST" })).status).toBe(503)
      const idle = (await (await fetch(`${b}/api/v1/todos`, { method: "POST", ...json({ text: "闲时项", idle: true }) })).json()) as { idleState?: string; idleTaskId?: string }
      expect(idle.idleState).toBe("pending")
      expect(idle.idleTaskId).toBeUndefined()
    } finally {
      h3.gc?.stop()
      h3.server.stop(true)
      rmSync(home3, { recursive: true, force: true })
    }
  })
})
