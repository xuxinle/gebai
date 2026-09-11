import { describe, expect, test } from "bun:test"
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { brotliDecompressSync, gunzipSync } from "node:zlib"
import { createApp, SERVICE_USER, type AppDeps } from "./app"
import type { ServerConfig } from "./core/base/config"

/** 最小 deps：静态托管只依赖 config（webDist/devReload/binaryMode/basePath）。 */
function makeDeps(webDist: string, overrides: Partial<ServerConfig> = {}): AppDeps {
  const config = {
    auth: "local",
    binaryMode: false,
    devReload: false,
    basePath: "/",
    uiStyle: "acrylic",
    webDist,
    ...overrides,
  } as unknown as ServerConfig
  return { config, auth: { defaultUser: () => SERVICE_USER } } as unknown as AppDeps
}

/** 临时站点：root/web 为 webDist，root/secret.txt 在站点目录之外（目录穿越用）。 */
function makeSite(): { root: string; dist: string; js: string } {
  const root = mkdtempSync(join(tmpdir(), "gebai-static-"))
  const dist = join(root, "web")
  mkdirSync(join(dist, "assets"), { recursive: true })
  mkdirSync(join(dist, "vendor"), { recursive: true })
  const js = `console.log("${"x".repeat(4096)}")\n`
  writeFileSync(join(dist, "assets", "main-abc123.js"), js)
  writeFileSync(join(dist, "vendor", "engine.js"), js)
  writeFileSync(join(dist, "favicon.svg"), "<svg/>")
  writeFileSync(join(root, "secret.txt"), "TOP-SECRET")
  return { root, dist, js }
}

describe("Web 静态资源托管（压缩协商 + 缓存策略）", () => {
  test("指纹资源：immutable 强缓存 + br 优先压缩，解压后与原文一致", async () => {
    const { root, dist, js } = makeSite()
    try {
      const app = createApp(makeDeps(dist))
      const res = await app.request("/assets/main-abc123.js", { headers: { "accept-encoding": "gzip, deflate, br" } })
      expect(res.status).toBe(200)
      expect(res.headers.get("content-type")).toBe("text/javascript")
      expect(res.headers.get("cache-control")).toBe("public, max-age=31536000, immutable")
      expect(res.headers.get("vary")).toBe("Accept-Encoding")
      expect(res.headers.get("content-encoding")).toBe("br")
      const raw = Buffer.from(await res.arrayBuffer())
      expect(raw.byteLength).toBeLessThan(js.length) // 确实压缩了
      expect(brotliDecompressSync(raw).toString()).toBe(js)
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  test("仅声明 gzip 时按 gzip 编码返回", async () => {
    const { root, dist, js } = makeSite()
    try {
      const app = createApp(makeDeps(dist))
      const res = await app.request("/assets/main-abc123.js", { headers: { "accept-encoding": "gzip" } })
      expect(res.headers.get("content-encoding")).toBe("gzip")
      expect(gunzipSync(Buffer.from(await res.arrayBuffer())).toString()).toBe(js)
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  test("客户端不接受压缩时原样返回（无 Content-Encoding）", async () => {
    const { root, dist, js } = makeSite()
    try {
      const app = createApp(makeDeps(dist))
      const res = await app.request("/assets/main-abc123.js", { headers: { "accept-encoding": "identity" } })
      expect(res.headers.get("content-encoding")).toBeNull()
      expect(await res.text()).toBe(js)
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  test("稳定名资源（/vendor）短缓存；dev-reload 下一律 no-cache", async () => {
    const { root, dist } = makeSite()
    try {
      const prod = await createApp(makeDeps(dist)).request("/vendor/engine.js")
      expect(prod.headers.get("cache-control")).toBe("public, max-age=86400")
      const dev = await createApp(makeDeps(dist, { devReload: true })).request("/vendor/engine.js")
      expect(dev.headers.get("cache-control")).toBe("no-cache")
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  test("小文件不压缩（低于阈值原样返回）", async () => {
    const { root, dist } = makeSite()
    try {
      const app = createApp(makeDeps(dist))
      const res = await app.request("/assets/small.js", { headers: { "accept-encoding": "br" } })
      expect(res.status).toBe(404) // 该文件不存在 → 落到 serveStatic 兜底 404
      writeFileSync(join(dist, "assets", "small.js"), "console.log(1)\n")
      const hit = await app.request("/assets/small.js", { headers: { "accept-encoding": "br" } })
      expect(hit.status).toBe(200)
      expect(hit.headers.get("content-encoding")).toBeNull()
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  test("目录穿越被拒：站点目录之外的文件不外泄", async () => {
    const { root, dist } = makeSite()
    try {
      const app = createApp(makeDeps(dist))
      for (const p of ["/assets/../../secret.txt", "/assets/..%2f..%2fsecret.txt", "/vendor/%2e%2e/%2e%2e/secret.txt"]) {
        const res = await app.request(p)
        expect(res.status).not.toBe(200)
        expect(await res.text()).not.toContain("TOP-SECRET")
      }
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  test("非前缀根文件仍由 serveStatic 兜底提供", async () => {
    const { root, dist } = makeSite()
    try {
      const res = await createApp(makeDeps(dist)).request("/favicon.svg")
      expect(res.status).toBe(200)
      expect(await res.text()).toBe("<svg/>")
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })
})
