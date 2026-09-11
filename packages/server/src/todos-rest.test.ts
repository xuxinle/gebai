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

  test("修改（文本/完成/闲时）与删除；非法 id 400、不存在 404", async () => {
    const created = (await (await fetch(`${base()}/api/v1/todos`, { method: "POST", ...json({ text: "写周报", idle: true }) })).json()) as { id: string; idleState?: string }
    expect(created.idleState).toBe("pending")

    const patched = (await (
      await fetch(`${base()}/api/v1/todos/${created.id}`, { method: "PATCH", ...json({ text: "写月报", done: true, idle: false }) })
    ).json()) as { text: string; done: boolean; idle: boolean; idleState?: string }
    expect([patched.text, patched.done, patched.idle]).toEqual(["写月报", true, false])
    expect(patched.idleState).toBeUndefined()

    expect((await fetch(`${base()}/api/v1/todos/not-hex`, { method: "PATCH", ...json({ done: true }) })).status).toBe(400)
    expect((await fetch(`${base()}/api/v1/todos/${"0".repeat(32)}`, { method: "PATCH", ...json({ done: true }) })).status).toBe(404)

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
})
