import { describe, expect, test } from "bun:test"
import { mkdtempSync, writeFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { createApp, SERVICE_USER, type AppDeps } from "./app"
import type { ServerConfig } from "./core/config"

/** 最小 deps：UI 路由只依赖 config 与 auth.defaultUser（auth=none），其余字段运行时不触碰。 */
function makeDeps(overrides: Partial<ServerConfig> = {}): AppDeps {
  const config = {
    auth: "local",
    binaryMode: false,
    devReload: false,
    basePath: "/",
    uiStyle: "matrix",
    ...overrides,
  } as unknown as ServerConfig
  return { config, auth: { defaultUser: () => SERVICE_USER } } as unknown as AppDeps
}

describe("Web UI 路由（dev-reload 首轮构建窗口期）", () => {
  test("index.html 暂缺时返回 503 占位页而非抛异常崩溃（窗口期）", async () => {
    // clean-dist 清空 dist 后、vite 尚未重建完成的真实状态：目录在、index.html 不在
    const dist = mkdtempSync(join(tmpdir(), "gebai-dist-empty-"))
    try {
      const app = createApp(makeDeps({ webDist: dist, devReload: true }))
      const res = await app.request("/")
      expect(res.status).toBe(503)
      const html = await res.text()
      expect(html).toContain("前端构建中")
      expect(html).toContain("__gebai_hot") // 复用热刷新通道，构建完成自动刷新
      expect(res.headers.get("cache-control")).toBe("no-cache")
    } finally {
      rmSync(dist, { recursive: true, force: true })
    }
  })

  test("dev-reload 下 dist 目录整体缺失时 UI 路由仍注册并返回占位页", async () => {
    const dist = mkdtempSync(join(tmpdir(), "gebai-dist-"))
    rmSync(dist, { recursive: true, force: true }) // 确保目录不存在
    const app = createApp(makeDeps({ webDist: dist, devReload: true }))
    const res = await app.request("/")
    expect(res.status).toBe(503)
    expect(await res.text()).toContain("前端构建中")
  })

  test("index.html 就绪后正常返回并注入 UI 风格", async () => {
    const dist = mkdtempSync(join(tmpdir(), "gebai-dist-ok-"))
    try {
      writeFileSync(join(dist, "index.html"), "<!doctype html><html><head></head><body>ok</body></html>")
      const app = createApp(makeDeps({ webDist: dist }))
      const res = await app.request("/")
      expect(res.status).toBe(200)
      const html = await res.text()
        expect(html).toContain('__GEBAI_UI_STYLE__="matrix"')
      expect(html).toContain("ok")
    } finally {
      rmSync(dist, { recursive: true, force: true })
    }
  })

  test("UI 风格白名单：aether 直通、非法值回落默认 acrylic", async () => {
    const dist = mkdtempSync(join(tmpdir(), "gebai-dist-style-"))
    try {
      writeFileSync(join(dist, "index.html"), "<!doctype html><html><head></head><body>ok</body></html>")
      const pass = createApp(makeDeps({ webDist: dist, uiStyle: "aether" }))
      const passHtml = await (await pass.request("/")).text()
      expect(passHtml).toContain('__GEBAI_UI_STYLE__="aether"')
      const fallback = createApp(makeDeps({ webDist: dist, uiStyle: "neon" }))
      const fallbackHtml = await (await fallback.request("/")).text()
      expect(fallbackHtml).toContain('__GEBAI_UI_STYLE__="acrylic"')
    } finally {
      rmSync(dist, { recursive: true, force: true })
    }
  })

  test("非 dev-reload 且 dist 缺失时 UI 路由不注册（404）", async () => {
    const dist = mkdtempSync(join(tmpdir(), "gebai-dist-"))
    rmSync(dist, { recursive: true, force: true })
    const app = createApp(makeDeps({ webDist: dist }))
    const res = await app.request("/")
    expect(res.status).toBe(404)
  })

  test("dev-reload 下每次请求重读 index.html（vite 重建后新 hash 资源生效）", async () => {
    const dist = mkdtempSync(join(tmpdir(), "gebai-dist-reload-"))
    try {
      writeFileSync(join(dist, "index.html"), '<!doctype html><html><head></head><body><script src="/assets/index-AAA.css"></script></body></html>')
      const app = createApp(makeDeps({ webDist: dist, devReload: true }))
      const first = await app.request("/")
      expect(await first.text()).toContain("index-AAA.css")
      // 模拟 vite build --watch 重建：index.html 更新为引用新 hash 资源
      writeFileSync(join(dist, "index.html"), '<!doctype html><html><head></head><body><script src="/assets/index-BBB.css"></script></body></html>')
      const second = await app.request("/")
      const secondHtml = await second.text()
      expect(secondHtml).toContain("index-BBB.css")
      expect(secondHtml).not.toContain("index-AAA.css")
    } finally {
      rmSync(dist, { recursive: true, force: true })
    }
  })

  test("非 dev-reload 模式 index.html 变更不影响已缓存 HTML（按原行为）", async () => {
    const dist = mkdtempSync(join(tmpdir(), "gebai-dist-cache-"))
    try {
      writeFileSync(join(dist, "index.html"), '<!doctype html><html><head></head><body><script src="/assets/index-AAA.css"></script></body></html>')
      const app = createApp(makeDeps({ webDist: dist }))
      await app.request("/")
      writeFileSync(join(dist, "index.html"), '<!doctype html><html><head></head><body><script src="/assets/index-BBB.css"></script></body></html>')
      const second = await app.request("/")
      expect(await second.text()).toContain("index-AAA.css")
    } finally {
      rmSync(dist, { recursive: true, force: true })
    }
  })
})
