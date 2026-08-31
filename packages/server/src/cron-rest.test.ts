import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { existsSync, mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"

let handle: ServerHandle
const home = mkdtempSync(join(tmpdir(), "gebai-cron-rest-"))

beforeAll(async () => {
  handle = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
})

afterAll(() => {
  handle.gc?.stop()
  handle.cron?.stop()
  handle.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

function base() {
  return `http://127.0.0.1:${handle.server.port}`
}

describe("cron REST（用户级任务管理面）", () => {
  test("list initially empty, create persists user-level store", async () => {
    expect(await (await fetch(`${base()}/api/v1/cron`)).json()).toEqual([])
    const res = await fetch(`${base()}/api/v1/cron`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name: "rest-task", type: "script", schedule: "@every 10m", script: "echo rest" }),
    })
    expect(res.status).toBe(201)
    const task = (await res.json()) as { id: string; nextRunAt: number }
    expect(task.id).toMatch(/^[a-f0-9]{32}$/)
    expect(task.nextRunAt).toBeGreaterThan(0)
    expect(existsSync(join(home, "users", "admin", "cron.json")) || existsSync(join(home, "users", "default", "cron.json"))).toBe(true)
    const listed = (await (await fetch(`${base()}/api/v1/cron`)).json()) as Array<{ id: string; name: string }>
    expect(listed).toHaveLength(1)
    expect(listed[0].name).toBe("rest-task")
  })

  test("create validates payload with 400", async () => {
    const res = await fetch(`${base()}/api/v1/cron`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ type: "script", schedule: "bad expr", script: "x" }),
    })
    expect(res.status).toBe(400)
    expect(String(((await res.json()) as { error: string }).error)).toContain("无效")
  })

  test("patch update, manual run, delete; invalid id rejected", async () => {
    const created = (await (
      await fetch(`${base()}/api/v1/cron`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ type: "script", schedule: "@every 10m", script: "echo run" }),
      })
    ).json()) as { id: string; nextRunAt: number }
    // 更新
    const patched = (await (
      await fetch(`${base()}/api/v1/cron/${created.id}`, {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ enabled: false, notify: [{ type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/x" }] }),
      })
    ).json()) as { enabled: boolean; notify?: Array<{ secret?: string }> }
    expect(patched.enabled).toBe(false)
    expect(patched.notify![0].secret).toBeUndefined() // 无密钥不脱敏占位
    // 手动触发：停用状态也可立即执行（验证配置），nextRunAt 不变
    const ran = (await (await fetch(`${base()}/api/v1/cron/${created.id}/run`, { method: "POST" })).json()) as { runCount: number; nextRunAt: number; runs?: Array<{ manual?: boolean; status: string }> }
    expect(ran.runCount).toBe(1)
    expect(ran.nextRunAt).toBe(created.nextRunAt)
    expect(ran.runs![0].manual).toBe(true)
    expect(ran.runs![0].status).toBe("success")
    // 删除
    expect((await fetch(`${base()}/api/v1/cron/${created.id}`, { method: "DELETE" })).status).toBe(200)
    expect((await fetch(`${base()}/api/v1/cron/${created.id}`, { method: "DELETE" })).status).toBe(404)
    // 非法 id：格式白名单拒绝（32 位 hex 之外的畸形形态）
    expect((await fetch(`${base()}/api/v1/cron/not-hex`, { method: "DELETE" })).status).toBe(400)
  })

  test("disabled capability returns 503", async () => {
    const home2 = mkdtempSync(join(tmpdir(), "gebai-cron-rest-off-"))
    const h2 = await startServer({ gebaiHome: home2, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], cronEnabled: false, port: 0 })
    try {
      const res = await fetch(`http://127.0.0.1:${h2.server.port}/api/v1/cron`)
      expect(res.status).toBe(503)
      expect(h2.cron).toBeNull()
    } finally {
      h2.gc?.stop()
      h2.server.stop(true)
      rmSync(home2, { recursive: true, force: true })
    }
  })
})
