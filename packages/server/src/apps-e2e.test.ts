import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"

/**
 * 托管应用服务级端到端：真实 startServer（独立端口 0）+ 真实 HTTP。
 *
 * 覆盖的是「路由位置与鉴权姿态」这类只有整条链路才看得出的问题——
 * 单测能证明 store 读得到文件，但证明不了 `/apps/x` 没被 static 的通配中间件吃掉、
 * 也证明不了 `/api/apps/x/*` 真的走在 `/api/*` 的 CORS/凭证链上。
 */
const home = mkdtempSync(join(tmpdir(), "gebai-apps-e2e-"))
let handle: ServerHandle

/** 造一个磁盘应用（含后端）。 */
function makeApp(id: string, files: Record<string, string>): void {
  const dir = join(home, "data", "apps", id)
  for (const [rel, content] of Object.entries(files)) {
    const abs = join(dir, rel)
    mkdirSync(join(abs, ".."), { recursive: true })
    writeFileSync(abs, content)
  }
}

beforeAll(async () => {
  makeApp("demo", {
    "app.json": JSON.stringify({ title: "演示", description: "端到端", backend: "server.js", env: { GREETING: "嘿" } }),
    "index.html": "<!doctype html><title>demo-idx</title><div id=app>hello</div>",
    "assets/app.js": "console.log('app')",
    "server.js": [
      "export const routes = {",
      "  'GET /health': (ctx) => ({ ok: true, app: ctx.appId, user: ctx.user }),",
      "  'POST /echo': async (ctx) => ({ body: await ctx.json() }),",
      "}",
      "export default (ctx) => ctx.respond({ miss: ctx.path }, { status: 404 })",
    ].join("\n"),
  })
  // 无后端、SPA 关闭的多页应用
  makeApp("static-only", { "app.json": JSON.stringify({ spa: false }), "index.html": "<title>static</title>ok" })
  // 入口缺失的应用（如实 503）
  makeApp("broken", { "app.json": JSON.stringify({ entry: "missing.html" }) })
  handle = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
})

afterAll(() => {
  handle.gc?.stop()
  handle.tasks?.stop()
  handle.todos?.stop()
  handle.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

const base = (): string => `http://127.0.0.1:${handle.server.port}`

describe("托管应用：静态分发（/apps）", () => {
  test("GET /apps 索引页：列出应用并标出来源层", async () => {
    const res = await fetch(`${base()}/apps`)
    expect(res.status).toBe(200)
    expect(res.headers.get("cache-control")).toBe("no-store")
    const html = await res.text()
    expect(html).toContain("托管应用")
    expect(html).toContain("demo")
    expect(html).toContain("运行时") // disk 层的中文标识
  })

  test("GET /apps/{id}/ 返回入口；/apps/{id} 不带尾斜杠同样可用", async () => {
    const a = await fetch(`${base()}/apps/demo/`)
    expect(a.status).toBe(200)
    expect(a.headers.get("content-type")).toContain("text/html")
    expect(await a.text()).toContain("demo-idx")

    const b = await fetch(`${base()}/apps/demo`)
    expect(b.status).toBe(200)
    expect(await b.text()).toContain("demo-idx")
  })

  test("子路径静态资源：MIME 按扩展名、no-cache、ETag 命中回 304", async () => {
    const res = await fetch(`${base()}/apps/demo/assets/app.js`)
    expect(res.status).toBe(200)
    expect(res.headers.get("content-type")).toContain("javascript")
    expect(res.headers.get("cache-control")).toBe("no-cache")
    const etag = res.headers.get("etag")!
    expect(etag).toBeTruthy()
    expect(await res.text()).toBe("console.log('app')")
    const second = await fetch(`${base()}/apps/demo/assets/app.js`, { headers: { "If-None-Match": etag } })
    expect(second.status).toBe(304)
  })

  test("SPA 回退：无扩展名的未知路径回落入口；有扩展名的不回落（404）", async () => {
    const page = await fetch(`${base()}/apps/demo/settings/profile`)
    expect(page.status).toBe(200)
    expect(await page.text()).toContain("demo-idx")
    // 有扩展名 → 不回落（否则缺失的 .js 会拿到 HTML，浏览器报 MIME 不符）
    const missingJs = await fetch(`${base()}/apps/demo/assets/missing.js`)
    expect(missingJs.status).toBe(404)
  })

  test("spa:false 的应用不做页面回退（多页应用按真实 404）", async () => {
    const res = await fetch(`${base()}/apps/static-only/nope/deep`)
    expect(res.status).toBe(404)
    expect((await fetch(`${base()}/apps/static-only/`)).status).toBe(200)
  })

  test("入口缺失的应用返回 503 说明页（不是空白/404）", async () => {
    const res = await fetch(`${base()}/apps/broken/`)
    expect(res.status).toBe(503)
    const html = await res.text()
    expect(html).toContain("不可用")
    expect(html).toContain("missing.html")
  })

  test("未知应用 404；非法 id 形态不会穿越到别的路径", async () => {
    expect((await fetch(`${base()}/apps/nope/`)).status).toBe(404)
    expect((await fetch(`${base()}/apps/..%2f..%2fetc/`)).status).toBe(404)
  })

  test("运行期新增应用：refresh 之后即可访问（无需重启服务）", async () => {
    makeApp("added-later", { "index.html": "<title>later</title>later" })
    const before = await fetch(`${base()}/apps/added-later/`)
    // 目录扫描有 1s TTL：这里以 refresh 显式失效，验证「动态载入」这条路本身是通的
    await fetch(`${base()}/api/v1/apps/refresh`, { method: "POST" })
    const after = await fetch(`${base()}/apps/added-later/`)
    expect(after.status).toBe(200)
    expect(await after.text()).toContain("later")
    expect(before.status).toBe(404)
  })
})

describe("托管应用：后端模块（/api/apps）", () => {
  test("GET /api/apps/demo/health 调通（本地模式后端默认启用）", async () => {
    const res = await fetch(`${base()}/api/apps/demo/health`)
    expect(res.status).toBe(200)
    const body = (await res.json()) as { ok: boolean; app: string; user: { id: string } }
    expect(body.ok).toBe(true)
    expect(body.app).toBe("demo")
    expect(body.user.id).toBe("admin") // 身份从凭证链来，不是从请求体来
  })

  test("POST + JSON 请求体（ctx.json）可用", async () => {
    const res = await fetch(`${base()}/api/apps/demo/echo`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ a: 1 }),
    })
    expect(res.status).toBe(200)
    expect(await res.json()).toEqual({ body: { a: 1 } })
  })

  test("路由表未命中由 default 兜底（未被包成统一 404）", async () => {
    const res = await fetch(`${base()}/api/apps/demo/unknown-path`)
    expect(res.status).toBe(404)
    expect(await res.json()).toEqual({ miss: "unknown-path" })
  })

  test("未声明后端的应用：后端端点 404 且说明原因", async () => {
    const res = await fetch(`${base()}/api/apps/static-only/anything`)
    expect(res.status).toBe(404)
    expect(String(((await res.json()) as { error: string }).error)).toContain("后端")
  })

  test("未知应用后端 404；非法 id 400", async () => {
    expect((await fetch(`${base()}/api/apps/nope/x`)).status).toBe(404)
    expect((await fetch(`${base()}/api/apps/Bad_Id/x`)).status).toBe(400)
  })

  test("应用清单接口：列出应用与后端状态（/api/v1/apps）", async () => {
    const res = await fetch(`${base()}/api/v1/apps`)
    expect(res.status).toBe(200)
    const body = (await res.json()) as {
      apps: Array<{ id: string; layer: string; backend: boolean; url: string; api: string | null; error?: string }>
      backend: { enabled: boolean }
      roots: string[]
    }
    const demo = body.apps.find((a) => a.id === "demo")!
    expect(demo).toMatchObject({ layer: "disk", backend: true, url: "/apps/demo/", api: "/api/apps/demo" })
    expect(body.apps.find((a) => a.id === "broken")?.error).toContain("入口文件缺失")
    expect(body.backend.enabled).toBe(true)
    expect(body.roots.length).toBeGreaterThan(0)
  })
})

describe("托管应用：关闭开关即整体下线", () => {
  test("GEBAI_APPS=false 时 /apps 与后端路由都不注册（404，不是 500）", async () => {
    const offHome = mkdtempSync(join(tmpdir(), "gebai-apps-off-"))
    mkdirSync(join(offHome, "data", "apps", "x"), { recursive: true })
    writeFileSync(join(offHome, "data", "apps", "x", "index.html"), "x")
    const h = await startServer({ gebaiHome: offHome, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0, appsEnabled: false })
    try {
      const b = `http://127.0.0.1:${h.server.port}`
      expect((await fetch(`${b}/apps`)).status).toBe(404)
      expect((await fetch(`${b}/apps/x/`)).status).toBe(404)
      expect((await fetch(`${b}/api/apps/x/y`)).status).toBe(404)
    } finally {
      h.gc?.stop()
      h.tasks?.stop()
      h.todos?.stop()
      h.server.stop(true)
      rmSync(offHome, { recursive: true, force: true })
    }
  })
})
