import { afterAll, beforeAll, describe, expect, test } from "bun:test"
import { mkdirSync, mkdtempSync, rmSync } from "node:fs"
import { rename } from "node:fs/promises"
import { tmpdir } from "node:os"
import { dirname, join } from "node:path"
import { startServer, type ServerHandle } from "./index"
import { sessionPath } from "./core/paths"

const home = mkdtempSync(join(tmpdir(), "gebai-restore-"))
let handle: ServerHandle

beforeAll(async () => {
  handle = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
})

afterAll(() => {
  handle.gc?.stop()
  handle.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

const base = () => `http://127.0.0.1:${handle.server.port}`

describe("会话回收站恢复", () => {
  test("归档到 trash 的会话可经 restore 接口恢复（保留期内的自助恢复通道）", async () => {
    const created = (await (
      await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ name: "待恢复" }) })
    ).json()) as { id: string }
    const dir = sessionPath(home, "admin", created.id)
    expect(created.id).toMatch(/^[0-9a-f]{32}$/)
    // 模拟 GC 归档：目录整体移入 trash/{date}/{id}
    const date = new Date().toISOString().slice(0, 10)
    const trashDir = join(home, "users", "admin", "trash", date, created.id)
    mkdirSync(dirname(trashDir), { recursive: true })
    await rename(dir, trashDir)
    handle.store.evict(created.id) // 生产流程 GC 归档时会失效缓存（onArchive 回调）
    // 归档后正常查询不可见
    expect((await fetch(`${base()}/api/v1/sessions/${created.id}`)).status).toBe(404)
    // 恢复
    const res = await fetch(`${base()}/api/v1/sessions/${created.id}/restore`, { method: "POST" })
    expect(res.status).toBe(200)
    const loaded = await (await fetch(`${base()}/api/v1/sessions/${created.id}`)).json() as { name: string }
    expect(loaded.name).toBe("待恢复")
    // 重复恢复：会话已存在 → 409
    const again = await fetch(`${base()}/api/v1/sessions/${created.id}/restore`, { method: "POST" })
    expect(again.status).toBe(404) // trash 中已无该会话（findInTrash 未命中 → 404）
  })

  test("不存在/未归档的会话恢复返回 404（不泄露他人会话存在性）", async () => {
    const res = await fetch(`${base()}/api/v1/sessions/ffffffffffffffffffffffffffffffff/restore`, { method: "POST" })
    expect(res.status).toBe(404)
  })
})
