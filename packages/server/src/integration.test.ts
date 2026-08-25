import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"
import { GebaiClient } from "@gebai/sdk"
import { sessionPath } from "./core/paths"
import { saveMiniTool } from "./core/mini-tools"

const home = mkdtempSync(join(tmpdir(), "gebai-http-"))
let handle: ServerHandle

beforeAll(async () => {
  process.env.GEBAI_HOME = home
  process.env.GEBAI_MODE = "local"
  process.env.GEBAI_SANDBOX = "off"
  handle = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
})

afterAll(() => {
  handle.gc?.stop()
  handle.server.stop(true)
  delete process.env.GEBAI_HOME
  rmSync(home, { recursive: true, force: true })
})

function base() {
  return `http://127.0.0.1:${handle.server.port}`
}

describe("REST API", () => {
  test("health", async () => {
    const res = await fetch(`${base()}/api/health`)
    expect(res.status).toBe(200)
    const body = (await res.json()) as { ok: boolean }
    expect(body.ok).toBe(true)
  })

  test("tools list includes global tools; sub-agent tools are lazily loaded", async () => {
    const res = await fetch(`${base()}/api/v1/tools`)
    const tools = (await res.json()) as Array<{ name: string; group: string }>
    expect(tools.some((t) => t.name === "read")).toBe(true)
    // 按需装载：默认不预装载子 Agent，其命名空间工具不出现在工具列表中
    expect(tools.some((t) => t.name === "code_read")).toBe(false)
  })

  test("sub-agents listed", async () => {
    const res = await fetch(`${base()}/api/v1/sub-agents`)
    const agents = (await res.json()) as Array<{ name: string }>
    expect(agents.some((a) => a.name === "code")).toBe(true)
  })

  test("session create/list/delete", async () => {
    const created = await (await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ name: "smoke" }) })).json() as { id: string }
    expect(created.id).toBeTruthy()
    const list = await (await fetch(`${base()}/api/v1/sessions`)).json() as Array<{ id: string }>
    expect(list.some((s) => s.id === created.id)).toBe(true)
    const del = await fetch(`${base()}/api/v1/sessions/${created.id}`, { method: "DELETE" })
    expect(del.status).toBe(200)
  })

  test("files/content serves binary files intact (PNG bytes + image/png)", async () => {
    const created = await (await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ name: "img" }) })).json() as { id: string }
    // 1x1 红色 PNG：验证二进制不被 text() 解码损坏（曾导致前端 <img> 无法显示）
    const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==", "base64")
    const dir = join(sessionPath(home, "admin", created.id), "tmp")
    mkdirSync(dir, { recursive: true })
    writeFileSync(join(dir, "pw_test.png"), png)
    const res = await fetch(`${base()}/api/v1/sessions/${created.id}/files/content?path=tmp/pw_test.png`)
    expect(res.status).toBe(200)
    expect(res.headers.get("content-type")).toContain("image/png")
    const body = new Uint8Array(await res.arrayBuffer())
    expect(body.length).toBe(png.length)
    // PNG 魔数 89 50 4E 47 必须原样保留（text() 会将其替换为 EF BF BD）
    expect(Buffer.from(body.subarray(0, 8)).toString("hex")).toBe("89504e470d0a1a0a")
  })

  test("files/preview：会话相对路径以 tmp/ 为根，绝对路径（本地模式）放行，download=1 附件形式", async () => {
    const created = await (await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ name: "pv" }) })).json() as { id: string }
    const dir = join(sessionPath(home, "admin", created.id), "tmp")
    mkdirSync(dir, { recursive: true })
    writeFileSync(join(dir, "a.ts"), "export const a = 1\n")
    // 相对路径（tmp/ 前缀可省）
    const rel = await fetch(`${base()}/api/v1/sessions/${created.id}/files/preview?path=${encodeURIComponent("tmp/a.ts")}`)
    expect(rel.status).toBe(200)
    expect(await rel.text()).toContain("export const a = 1")
    // 绝对路径（本地模式操作者本人，与文件工具能力对齐——read 本地绝对路径场景）
    const abs = await fetch(`${base()}/api/v1/sessions/${created.id}/files/preview?path=${encodeURIComponent(join(dir, "a.ts"))}`)
    expect(abs.status).toBe(200)
    expect(await abs.text()).toContain("export const a = 1")
    // download=1 → Content-Disposition 附件（文件卡/chip 下载入口）
    const dl = await fetch(`${base()}/api/v1/sessions/${created.id}/files/preview?path=${encodeURIComponent("tmp/a.ts")}&download=1`)
    expect(dl.status).toBe(200)
    expect(dl.headers.get("content-disposition")).toContain("attachment")
    // 不存在的文件 → 404
    const miss = await fetch(`${base()}/api/v1/sessions/${created.id}/files/preview?path=tmp/nope.ts`)
    expect(miss.status).toBe(404)
  })

  test("serves built web UI at / when present", async () => {
    const res = await fetch(`${base()}/`)
    if (res.status === 200) {
      const html = await res.text()
      expect(html).toContain("<html")
      // 服务端注入全局默认 UI 风格（GEBAI_UI_STYLE → __GEBAI_UI_STYLE__，白名单校验）
      expect(html).toContain("__GEBAI_UI_STYLE__=")
      expect(html).toMatch(/__GEBAI_UI_STYLE__="(acrylic|classic|aether|dark|modern|minimal|cyberpunk|aurora|synthwave|matrix|tokyo-night|cny)"/)
    } else {
      // web build not present in this environment; acceptable
      expect([404, 200]).toContain(res.status)
    }
  })

  test("websocket connects and handles a request via SDK", async () => {
    const client = new GebaiClient({ baseUrl: base() })
    await client.connect()
    const sessions = await client.listSessions()
    expect(Array.isArray(sessions)).toBe(true)
  })

  test("mini-tools SDK methods wire to REST endpoints", async () => {
    const client = new GebaiClient({ baseUrl: base() })
    await saveMiniTool(home, "admin", { name: "sdk_tool", html: "<p>sdk</p>", scope: "public" })
    const list = await client.listMiniTools()
    expect(list.some((t) => t.name === "sdk_tool")).toBe(true)
    const got = await client.getMiniTool("sdk_tool")
    expect(got.html).toBe("<p>sdk</p>")
    await client.deleteMiniTool("sdk_tool", "public")
    await expect(client.getMiniTool("sdk_tool")).rejects.toThrow("404")
  })

  test("mini-tools REST: list/get/delete with private shadowing public", async () => {
    // 经 save_tool 落库（公用 + 私有同名）
    await saveMiniTool(home, "admin", { name: "clock", html: "<p>公共时钟</p>", scope: "public" })
    await saveMiniTool(home, "admin", { name: "clock", html: "<p>我的时钟</p>", scope: "private" })
    await saveMiniTool(home, "admin", { name: "calc", html: "<p>计算器</p>", scope: "public" })

    // 列表：公用全部 + 本人私有；同名私有覆盖公用（只出现一条 clock）
    const list = (await (await fetch(`${base()}/api/v1/mini-tools`)).json()) as Array<{ name: string; scope: string }>
    const clocks = list.filter((t) => t.name === "clock")
    expect(clocks.length).toBe(1)
    expect(clocks[0].scope).toBe("private")
    expect(list.some((t) => t.name === "calc" && t.scope === "public")).toBe(true)

    // 单条读取：私有优先，含 html
    const got = (await (await fetch(`${base()}/api/v1/mini-tools/clock`)).json()) as { name: string; html: string; scope: string }
    expect(got.html).toBe("<p>我的时钟</p>")
    expect(got.scope).toBe("private")

    // 404
    const missing = await fetch(`${base()}/api/v1/mini-tools/nope`)
    expect(missing.status).toBe(404)

    // 删除私有 → 同名公共恢复可见
    const del = await fetch(`${base()}/api/v1/mini-tools/clock?scope=private`, { method: "DELETE" })
    expect(del.status).toBe(200)
    const after = (await (await fetch(`${base()}/api/v1/mini-tools/clock`)).json()) as { html: string; scope: string }
    expect(after.html).toBe("<p>公共时钟</p>")

    // 删除公共
    const delPub = await fetch(`${base()}/api/v1/mini-tools/clock?scope=public`, { method: "DELETE" })
    expect(delPub.status).toBe(200)
    expect((await fetch(`${base()}/api/v1/mini-tools/clock`)).status).toBe(404)
  })
})
