import { describe, expect, test, afterAll } from "bun:test"
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { AppsStore } from "./sources"
import type { InlineApp } from "./inline"

const roots: string[] = []
function tmp(prefix: string): string {
  const d = mkdtempSync(join(tmpdir(), prefix))
  roots.push(d)
  return d
}
afterAll(() => {
  for (const d of roots) rmSync(d, { recursive: true, force: true })
})

/** 造一个磁盘应用目录。 */
function makeApp(home: string, id: string, files: Record<string, string>, where = "data/apps"): string {
  const dir = join(home, where, id)
  for (const [rel, content] of Object.entries(files)) {
    const abs = join(dir, rel)
    mkdirSync(join(abs, ".."), { recursive: true })
    writeFileSync(abs, content)
  }
  return dir
}

const text = (b: Uint8Array | undefined): string => new TextDecoder().decode(b ?? new Uint8Array())

describe("托管应用：三层来源合并", () => {
  test("最小应用：只放 index.html 即被识别（清单可省，入口默认 index.html）", async () => {
    const home = tmp("gebai-apps-min-")
    makeApp(home, "min", { "index.html": "<h1>hi</h1>" })
    const store = new AppsStore({ home, scanTtlMs: 0 })
    const apps = store.list(true)
    expect(apps.map((a) => a.manifest.id)).toEqual(["min"])
    const entry = store.get("min")!
    expect(entry.layer).toBe("disk")
    expect(entry.error).toBeUndefined()
    expect(text((await store.readFile(entry, "index.html"))?.body)).toBe("<h1>hi</h1>")
  })

  test("没有 index.html 也没有 app.json 的目录不算应用（不把杂目录当应用）", () => {
    const home = tmp("gebai-apps-junk-")
    mkdirSync(join(home, "data", "apps", "junk"), { recursive: true })
    writeFileSync(join(home, "data", "apps", "junk", "notes.txt"), "x")
    const store = new AppsStore({ home, scanTtlMs: 0 })
    expect(store.list(true)).toEqual([])
  })

  test("优先级：disk > inline > embedded（同 id 逐文件叠加）", async () => {
    const home = tmp("gebai-apps-prio-")
    makeApp(home, "x", { "index.html": "disk-index", "only-disk.txt": "d" })
    const inline: InlineApp[] = [{ id: "x", files: { "index.html": "inline-index", "only-inline.txt": "i" } }]
    const embedded = { x: { "index.html": Buffer.from("embedded-index").toString("base64"), "only-embedded.txt": Buffer.from("e").toString("base64") } }
    const store = new AppsStore({ home, inline, embedded, scanTtlMs: 0 })
    const entry = store.get("x")!
    expect(entry.layers).toEqual(["disk", "inline", "embedded"]) // 高→低
    expect(entry.layer).toBe("disk")
    expect(text((await store.readFile(entry, "index.html"))?.body)).toBe("disk-index")
    // 低层独有的文件仍然可读（叠加而不是覆盖整份）
    expect(text((await store.readFile(entry, "only-inline.txt"))?.body)).toBe("i")
    expect(text((await store.readFile(entry, "only-embedded.txt"))?.body)).toBe("e")
  })

  test("纯内联应用：文件是字符串、后端是函数（无需落盘）", async () => {
    const home = tmp("gebai-apps-inline-")
    const inline: InlineApp[] = [
      {
        id: "inl",
        title: "内联",
        files: { "index.html": "<h1>inl</h1>" },
        backend: (ctx) => ({ path: ctx.path }),
      },
    ]
    const store = new AppsStore({ home, inline, scanTtlMs: 0 })
    const entry = store.get("inl")!
    expect(entry.layer).toBe("inline")
    expect(entry.hasBackend).toBe(true)
    expect(typeof entry.inlineBackend).toBe("function")
    expect(await entry.backendDir()).toBeNull() // 函数型后端不需要目录
    expect(text((await store.readFile(entry, "index.html"))?.body)).toBe("<h1>inl</h1>")
  })

  test("纯内嵌应用（二进制形态）：文件可读；带 backend 声明时落地到缓存目录", async () => {
    const home = tmp("gebai-apps-embed-")
    const b64 = (s: string): string => Buffer.from(s).toString("base64")
    const embedded = {
      emb: { "index.html": b64("<h1>emb</h1>"), "app.json": b64(JSON.stringify({ title: "内嵌", backend: "server.js" })), "server.js": b64("export default () => ({ ok: 1 })") },
    }
    const store = new AppsStore({ home, embedded, scanTtlMs: 0 })
    const entry = store.get("emb")!
    expect(entry.layer).toBe("embedded")
    expect(entry.hasBackend).toBe(true)
    expect(text((await store.readFile(entry, "index.html"))?.body)).toBe("<h1>emb</h1>")
    // 二进制形态没有源码树：文件型后端必须先落地成文件才能 import
    const dir = await entry.backendDir()
    expect(dir).toBe(join(home, "data", "apps-cache", "emb"))
    expect(await Bun.file(join(dir!, "server.js")).text()).toBe("export default () => ({ ok: 1 })")
    // 再次调用不重写（幂等：内容相同则跳过）
    const before = Bun.file(join(dir!, "server.js")).lastModified
    expect(await entry.backendDir()).toBe(dir)
    expect(Bun.file(join(dir!, "server.js")).lastModified).toBe(before)
  })

  test("内嵌应用（二进制形态）的文件型后端：落地后真能跑起来", async () => {
    // 这条盖住的是「单文件二进制自带含后端的应用」这条链路：没有源码树 → 必须先落地成文件才能 import。
    // 在测试里模拟内嵌层（不去依赖构建产物：提交态的内嵌表是空态，构建产物才有内容）。
    const home = tmp("gebai-apps-emb-backend-")
    const b64 = (s: string): string => Buffer.from(s).toString("base64")
    const embedded = {
      emb2: {
        "index.html": b64("<h1>emb2</h1>"),
        "app.json": b64(JSON.stringify({ backend: "server.js" })),
        "server.js": b64("export default (ctx) => ({ ok: true, path: ctx.path })"),
      },
    }
    const { AppBackendLoader, testContext } = await import("./backend")
    const store = new AppsStore({ home, embedded, scanTtlMs: 0 })
    const entry = store.get("emb2")!
    const loader = new AppBackendLoader({ enabled: true })
    expect(await loader.invoke(entry, testContext("emb2", "GET", "ping"))).toEqual({ ok: true, path: "ping" })
    expect(await Bun.file(join(home, "data", "apps-cache", "emb2", "server.js")).exists()).toBe(true)
  })

  test("入口缺失如实标 error（列出但不假装可用）", () => {
    const home = tmp("gebai-apps-noentry-")
    makeApp(home, "broken", { "app.json": JSON.stringify({ entry: "main.html" }) })
    const store = new AppsStore({ home, scanTtlMs: 0 })
    const entry = store.get("broken")!
    expect(entry.error).toContain("入口文件缺失")
  })

  test("磁盘层目录变化：invalidate 后立即可见（运行时动态载入）", () => {
    const home = tmp("gebai-apps-dyn-")
    const store = new AppsStore({ home, scanTtlMs: 0 })
    expect(store.list(true)).toEqual([])
    makeApp(home, "later", { "index.html": "x" })
    expect(store.list(true).map((a) => a.manifest.id)).toEqual(["later"])
  })

  test("文件读不到即 null（越界、目录、不存在）", async () => {
    const home = tmp("gebai-apps-read-")
    makeApp(home, "r", { "index.html": "x", "sub/a.txt": "a" })
    const store = new AppsStore({ home, scanTtlMs: 0 })
    const entry = store.get("r")!
    expect(await store.readFile(entry, "missing.txt")).toBeNull()
    expect(await store.readFile(entry, "../escape.txt")).toBeNull()
    expect(await store.readFile(entry, "sub")).toBeNull() // 目录
    expect(await store.readFile(entry, "/etc/passwd")).toBeNull()
    expect(await store.readFile(entry, "sub/a.txt")).not.toBeNull()
  })

  test("超出单文件上限视为不存在", async () => {
    const home = tmp("gebai-apps-max-")
    makeApp(home, "big", { "index.html": "x" })
    writeFileSync(join(home, "data", "apps", "big", "huge.bin"), Buffer.alloc(2048))
    const store = new AppsStore({ home, maxFileBytes: 1024, scanTtlMs: 0 })
    const entry = store.get("big")!
    expect(await store.readFile(entry, "huge.bin")).toBeNull()
    expect(await store.readFile(entry, "index.html")).not.toBeNull()
  })

  test("GEBAI_APPS_DIRS 显式目录优先级最高；出厂根 {home}/apps 也在扫描面内", () => {
    const home = tmp("gebai-apps-dirs-")
    const custom = tmp("gebai-apps-custom-")
    mkdirSync(join(custom, "fromcustom"), { recursive: true })
    writeFileSync(join(custom, "fromcustom", "index.html"), "c")
    makeApp(home, "factory", { "index.html": "f" }, "apps")
    const store = new AppsStore({ home, dirs: [custom], scanTtlMs: 0 })
    expect(store.roots()[0]).toBe(custom)
    expect(store.roots()).toContain(join(home, "apps"))
    expect(store.list(true).map((a) => a.manifest.id).sort()).toEqual(["factory", "fromcustom"])
  })

  test("内联应用定义非法时跳过并记错误（不拖垮其余）", () => {
    const home = tmp("gebai-apps-badinline-")
    const inline = [
      { id: "Bad Id", files: {} } as unknown as InlineApp,
      { id: "good", files: { "index.html": "ok" } } as InlineApp,
    ]
    const store = new AppsStore({ home, inline, scanTtlMs: 0 })
    expect(store.list(true).map((a) => a.manifest.id)).toEqual(["good"])
    expect(store.inlineErrors().length).toBe(1)
  })
})

describe("托管应用：后端模块加载与热重载", () => {
  test("改 server.js 后下一次请求即拿到新代码（按 mtime 版本化 import）", async () => {
    const home = tmp("gebai-apps-hot-")
    const dir = makeApp(home, "hot", {
      "index.html": "x",
      "app.json": JSON.stringify({ backend: "server.js" }),
      "server.js": "export default () => ({ v: 1 })",
    })
    const { AppBackendLoader } = await import("./backend")
    const loader = new AppBackendLoader({ enabled: true })
    const store = new AppsStore({ home, scanTtlMs: 0 })
    const entry = store.get("hot")!
    const ctx = (await import("./backend")).testContext(entry.manifest.id, "GET", "")
    expect(await loader.invoke(entry, ctx)).toEqual({ v: 1 })
    // 改文件（mtime 必须变：同一毫秒内写入不能被判为同一版本）
    writeFileSync(join(dir, "server.js"), "export default () => ({ v: 2 })")
    const t = new Date(Date.now() + 2000)
    require("node:fs").utimesSync(join(dir, "server.js"), t, t)
    expect(await loader.invoke(entry, ctx)).toEqual({ v: 2 })
  })

  test("未启用时如实说明原因（不静默降级成 404）", async () => {
    const home = tmp("gebai-apps-off-")
    makeApp(home, "off", { "index.html": "x", "app.json": JSON.stringify({ backend: "server.js" }), "server.js": "export default () => ({})" })
    const { AppBackendLoader, AppBackendError, testContext } = await import("./backend")
    const loader = new AppBackendLoader({ enabled: false, reason: "服务模式默认不加载" })
    const entry = new AppsStore({ home, scanTtlMs: 0 }).get("off")!
    expect(loader.canRun(entry)).toBe(false)
    await expect(loader.invoke(entry, testContext("off", "GET", ""))).rejects.toThrow(AppBackendError)
    await expect(loader.invoke(entry, testContext("off", "GET", ""))).rejects.toThrow("服务模式默认不加载")
  })

  test("三种导出形态都认：default 函数 / fetch 函数 / routes 表（routes 未命中落 default 兜底）", async () => {
    const home = tmp("gebai-apps-forms-")
    makeApp(home, "forms", {
      "index.html": "x",
      "app.json": JSON.stringify({ backend: "server.js" }),
      "server.js": [
        "export const routes = { 'GET /ping': () => ({ via: 'routes' }) }",
        "export default (ctx) => ({ via: 'default', path: ctx.path })",
      ].join("\n"),
    })
    const { AppBackendLoader, testContext } = await import("./backend")
    const loader = new AppBackendLoader({ enabled: true })
    const entry = new AppsStore({ home, scanTtlMs: 0 }).get("forms")!
    expect(await loader.invoke(entry, testContext("forms", "GET", "ping"))).toEqual({ via: "routes" })
    // 路由表未命中 → default 兜底（不是“统一 404”：那是把两种写法互斥化了）
    expect(await loader.invoke(entry, testContext("forms", "POST", "other"))).toEqual({ via: "default", path: "other" })
  })

  test("后端抛错被包成 run 阶段错误；超时被包成 timeout", async () => {
    const home = tmp("gebai-apps-err-")
    makeApp(home, "err", {
      "index.html": "x",
      "app.json": JSON.stringify({ backend: "server.js", env: { GEBAI_APP_TIMEOUT_MS: "50" } }),
      "server.js": "export default (ctx) => { if (ctx.path === 'boom') throw new Error('炸了'); return new Promise(() => {}) }",
    })
    const { AppBackendLoader, AppBackendError, testContext } = await import("./backend")
    const loader = new AppBackendLoader({ enabled: true })
    const entry = new AppsStore({ home, scanTtlMs: 0 }).get("err")!
    const e1 = (await loader.invoke(entry, testContext("err", "GET", "boom")).catch((e) => e)) as InstanceType<typeof AppBackendError>
    expect(e1.stage).toBe("run")
    expect(e1.message).toContain("炸了")
    const e2 = (await loader.invoke(entry, testContext("err", "GET", "hang")).catch((e) => e)) as InstanceType<typeof AppBackendError>
    expect(e2.stage).toBe("timeout")
  })

  test("加载失败（语法错）如实报 load 阶段", async () => {
    const home = tmp("gebai-apps-syntax-")
    makeApp(home, "syn", { "index.html": "x", "app.json": JSON.stringify({ backend: "server.js" }), "server.js": "export default ((((" })
    const { AppBackendLoader, AppBackendError, testContext } = await import("./backend")
    const loader = new AppBackendLoader({ enabled: true })
    const entry = new AppsStore({ home, scanTtlMs: 0 }).get("syn")!
    const err = (await loader.invoke(entry, testContext("syn", "GET", "")).catch((e) => e)) as InstanceType<typeof AppBackendError>
    expect(err.stage).toBe("load")
  })
})
