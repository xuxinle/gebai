/**
 * Web UI 入口 HTML 的缓存失效测试。
 *
 * 背景（真实缺陷）：`/` 路由只在**启动时**读一次 index.html 并长期缓存（非 dev-reload 模式下）。
 * 前端重新构建后（vite 产出新 hash 资源、clean-dist 删掉旧资源）服务端仍返回旧 HTML，
 * 其引用的 `/assets/main-*.js|css` 已不存在 → 全 404 → 页面无样式、脚本不执行。
 * 这类现象极易被误判为「刚改的代码有 bug」，实际只是缓存假象；故在此锁住失效行为。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync, utimesSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { createApp, type AppDeps } from "../app"
import type { ServerConfig } from "../core/base/config"

function makeDeps(config: Partial<ServerConfig>): AppDeps {
  return {
    config: { auth: "local", gebaiHome: join(tmpdir(), "gebai-static-home"), ...config } as unknown as ServerConfig,
    auth: { defaultUser: () => "service" },
    sandbox: { enforcedFor: () => false, isExempt: () => true },
    engine: { workbenchProjects: () => [] },
    store: { getEnv: async () => ({}) },
  } as unknown as AppDeps
}

/** 写 index.html 并把 mtime 显式推后（同一毫秒内的两次写入不该被判为「已变」）。 */
function writeIndex(dir: string, body: string, mtimeOffsetMs = 0): void {
  const p = join(dir, "index.html")
  writeFileSync(p, `<html><head></head><body>${body}</body></html>`, "utf8")
  if (mtimeOffsetMs) {
    const t = new Date(Date.now() + mtimeOffsetMs)
    utimesSync(p, t, t)
  }
}

describe("Web UI 入口 HTML 缓存（按 index.html 的 mtime 失效）", () => {
  test("前端重建后再次请求即拿到新 HTML，并保留 UI 风格注入", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-static-html-"))
    try {
      writeIndex(dir, "v1")
      const app = createApp(makeDeps({ webDist: dir, devReload: false }))

      const first = await (await app.request("/")).text()
      expect(first).toContain("v1")
      expect(first).toContain("__GEBAI_UI_STYLE__")
      // 重新构建：内容与 mtime 都变
      writeIndex(dir, "v2", 5_000)
      const second = await (await app.request("/")).text()
      expect(second).toContain("v2")
      expect(second).not.toContain("v1")
      expect(second).toContain("__GEBAI_UI_STYLE__")

      // 文件未再变时命中缓存（同一份注入结果）
      const third = await (await app.request("/")).text()
      expect(third).toBe(second)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("index.html 暂缺时返回占位页（503）而不是抛异常；补齐后恢复", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-static-missing-"))
    try {
      writeIndex(dir, "v1")
      const app = createApp(makeDeps({ webDist: dir, devReload: false }))
      expect(await (await app.request("/")).text()).toContain("v1")

      // 构建窗口期：clean-dist 删掉 index.html
      rmSync(join(dir, "index.html"))
      const missing = await app.request("/")
      expect(missing.status).toBe(503)

      // 构建完成：新 HTML 上线
      writeIndex(dir, "v3", 5_000)
      const back = await app.request("/")
      expect(back.status).toBe(200)
      expect(await back.text()).toContain("v3")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("入口 HTML 一律 no-store：移动端浏览器/中间缓存不得存住旧 HTML", async () => {
    // 为什么必须是 no-store 而不是 no-cache：后者只要求「用前校验」，但移动端浏览器
    // （微信/UC/系统浏览器）与部分反向代理会忽略它而强缓存 HTML——而入口 HTML 引用的是
    // 内容 hash 命名的 /assets/*，缓存住旧 HTML 就等于引用已被删除的旧资源（全 404），
    // 页面无样式且脚本不执行，表现为「看起来像代码改坏了」（无痕模式却正常）。
    const dir = mkdtempSync(join(tmpdir(), "gebai-static-nostore-"))
    try {
      writeIndex(dir, "v1")
      const app = createApp(makeDeps({ webDist: dir, devReload: false }))
      const res = await app.request("/")
      expect(res.headers.get("cache-control")).toBe("no-store")

      // 构建窗口期的占位页同样不得被缓存（否则构建完成后仍会看到占位页）
      rmSync(join(dir, "index.html"))
      const placeholder = await app.request("/")
      expect(placeholder.status).toBe(503)
      expect(placeholder.headers.get("cache-control")).toBe("no-store")

      // 指纹资源仍应长期强缓存（与 HTML 策略相反，两者不可相互渗透）
      const { mkdirSync } = await import("node:fs")
      mkdirSync(join(dir, "assets"), { recursive: true })
      writeFileSync(join(dir, "assets", "main-abc123.js"), "console.log(1)")
      const asset = await app.request("/assets/main-abc123.js")
      expect(asset.headers.get("cache-control")).toContain("immutable")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })
})

describe("页面标题注入（GEBAI_TITLE）", () => {
  /** 写入指定 <title> 的入口 HTML。 */
  function writeIndexWithTitle(dir: string, titleTag: string): void {
    writeFileSync(join(dir, "index.html"), `<html><head>${titleTag}</head><body>page</body></html>`, "utf8")
  }

  test("设置后替换 <title>（含 files 页前缀保留）并注入 window.__GEBAI_TITLE__", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-static-title-"))
    try {
      writeIndexWithTitle(dir, "<title>歌白</title>")
      writeFileSync(join(dir, "files.html"), `<html><head><title>文件工作台 · 歌白</title></head><body>f</body></html>`, "utf8")
      const app = createApp(makeDeps({ webDist: dir, devReload: false, title: "我的工作台" }))

      const html = await (await app.request("/")).text()
      expect(html).toContain("<title>我的工作台</title>")
      expect(html).toContain('window.__GEBAI_TITLE__="我的工作台"')
      expect(html).not.toContain("歌白")

      // files 页：前缀「文件工作台」保留，分隔符不重复，名字替换为定制值
      const files = await (await app.request("/files")).text()
      expect(files).toContain("<title>文件工作台 · 我的工作台</title>")
      expect(files).toContain('window.__GEBAI_TITLE__="我的工作台"')
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("含 HTML 敏感字符的标题正确转义（<title> 实体 / <script> JSON 转义）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-static-title-esc-"))
    try {
      writeIndexWithTitle(dir, "<title>歌白</title>")
      const app = createApp(makeDeps({ webDist: dir, devReload: false, title: `a<b>&"c</script>` }))

      const html = await (await app.request("/")).text()
      // <title> 上下文：实体转义
      expect(html).toContain("<title>a&lt;b&gt;&amp;&quot;c&lt;/script&gt;</title>")
      // <script> 上下文：JSON 序列化 + `<` 转义为 \u003c，不产生真实的 </script> 截断点
      expect(html).toContain('window.__GEBAI_TITLE__="a\\u003cb>&\\"c\\u003c/script>"')
      expect(html).not.toContain("c</script>")
      // 注入脚本可完整解析回原字符串（转义往返无损，脚本元素不会被截断）
      const m = /<script>window\.__GEBAI_TITLE__=(.*?)<\/script>/.exec(html)
      expect(m && JSON.parse(m[1]!)).toBe(`a<b>&"c</script>`)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("未设置时不注入（前端内置默认「歌白」保持不动）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-static-title-off-"))
    try {
      writeIndexWithTitle(dir, "<title>歌白</title>")
      const app = createApp(makeDeps({ webDist: dir, devReload: false }))

      const html = await (await app.request("/")).text()
      expect(html).toContain("<title>歌白</title>")
      expect(html).not.toContain("__GEBAI_TITLE__")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })
})

describe("语法 wasm 静态回源（/vendor/tree-sitter/lang）", () => {
  /** 空 webDist（这些字节不来自 web 产物，而是服务端内嵌语法集）。 */
  const app = () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-static-grammar-"))
    writeIndex(dir, "v1")
    return { app: createApp(makeDeps({ webDist: dir, devReload: false })), dir }
  }

  test("白名单内的语法以 wasm 类型回源，并支持 gzip 协商（体积比原始字节小得多）", async () => {
    const { app: a, dir } = app()
    try {
      // 不声明 Accept-Encoding：回原始 wasm 字节（客户不一定会解压，不能无条件压缩）
      const plain = await a.request("/vendor/tree-sitter/lang/tree-sitter-python.wasm")
      expect(plain.status).toBe(200)
      expect(plain.headers.get("content-type")).toBe("application/wasm")
      expect(plain.headers.get("content-encoding")).toBeNull()
      expect(plain.headers.get("vary")).toContain("Accept-Encoding")
      expect(plain.headers.get("cache-control")).toContain("max-age")
      const raw = new Uint8Array(await plain.arrayBuffer())
      expect(raw.byteLength).toBeGreaterThan(1000)
      // wasm 魔数（\0asm）——确保拿到的是真文件而不是错误页
      expect([raw[0], raw[1], raw[2], raw[3]]).toEqual([0, 97, 115, 109])

      const gz = await a.request("/vendor/tree-sitter/lang/tree-sitter-python.wasm", { headers: { "accept-encoding": "gzip" } })
      expect(gz.status).toBe(200)
      expect(gz.headers.get("content-encoding")).toBe("gzip")
      const gzBytes = new Uint8Array(await gz.arrayBuffer())
      expect(gzBytes.byteLength).toBeLessThan(raw.byteLength)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("白名单外的文件名一律 404（不碰文件系统）", async () => {
    const { app: a, dir } = app()
    try {
      for (const p of [
        "/vendor/tree-sitter/lang/tree-sitter-nonexistent.wasm",
        "/vendor/tree-sitter/lang/..%2F..%2Findex.html",
        "/vendor/tree-sitter/lang/",
      ]) {
        expect((await a.request(p)).status).toBe(404)
      }
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("SDK 语言表里的每个语法都能取到（前后端表漂移会在这里变红）", async () => {
    const { TREE_SITTER_GRAMMAR } = await import("@gebai/sdk")
    const { app: a, dir } = app()
    try {
      for (const file of Object.values(TREE_SITTER_GRAMMAR)) {
        const res = await a.request(`/vendor/tree-sitter/lang/${file}`)
        expect({ file, status: res.status }).toEqual({ file, status: 200 })
      }
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })
})
