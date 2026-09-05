import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs"
import { join, dirname } from "node:path"
import type { ToolContext } from "../../core/base/types"
import { tmpdir } from "node:os"
import { createPlaywrightTools } from "./playwright_tools"
import type { BridgeLike } from "../../core/browser/bridge"
import { def as playwrightDef } from "./playwright"

function ctx(home: string, overrides: Partial<ToolContext> = {}): ToolContext {
  const sid = "0123456789abcdef0123456789abcdef" // 合法会话 id（32 位 hex）
  const tmp = join(home, "users", "default", "sessions", sid, "tmp")
  mkdirSync(tmp, { recursive: true })
  const base: ToolContext = {
    user: "default",
    sessionId: sid,
    workdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => join(tmp, p),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, content) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(dirname(p), { recursive: true })
      await writeFile(p, content)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (r) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => { throw new Error("未知预置项目") },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    runNewSession: async () => ({ output: "ok", archive: { runId: "r", agents: ["x"], input: "", output: "ok", messages: [] } }),
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
    waitForCapture: async () => null,
  }
  return { ...base, ...overrides }
}

/** 记录调用序列的 fake bridge，可按 op 定制返回。 */
function recordingBridge() {
  const calls: Array<{ op: string; args: Record<string, unknown> }> = []
  const bridge: BridgeLike = {
    request: async (op, args) => {
      calls.push({ op, args })
      switch (op) {
        case "open":
          return { url: "https://example.com/", title: "Example" }
        case "content":
          return { text: "hello world" }
        case "pages":
          return { pages: [{ index: 0, url: "https://example.com/", title: "Example", active: true }] }
        case "evaluate":
          return { value: { value: JSON.stringify({ a: 1 }) } }
        case "new_page":
          return { index: 1, url: "about:blank", title: "" }
        case "switch_page":
          return { index: 0, url: "https://example.com/", title: "Example" }
        default:
          return {}
      }
    },
  }
  return { bridge, calls }
}

describe("playwright tools", () => {
  test("open sends url/wait_until/timeout and reports title", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const { bridge, calls } = recordingBridge()
    const tools = createPlaywrightTools({ bridge })
    const r = await tools.open.execute({ url: "https://example.com", wait_until: "domcontentloaded", timeout: 5000 }, ctx(home))
    expect(r.output).toContain("https://example.com/")
    expect(r.output).toContain("Example")
    expect(calls[0].op).toBe("open")
    expect(calls[0].args).toMatchObject({ url: "https://example.com", waitUntil: "domcontentloaded", timeout: 5000 }) // 桥协议键驼峰（driver 契约）
    // 会话 id 自动附带
    expect(calls[0].args.sessionId).toBe("0123456789abcdef0123456789abcdef")
  })

  test("content validates mode before calling bridge", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    let called = false
    const tools = createPlaywrightTools({
      bridge: { request: async () => { called = true; return {} } },
    })
    const r = await tools.content.execute({ mode: "xml" }, ctx(home))
    expect(called).toBe(false)
    expect(r.output).toContain("mode")
  })

  test("content passes selector/index and returns text", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const { bridge, calls } = recordingBridge()
    const tools = createPlaywrightTools({ bridge })
    const r = await tools.content.execute({ mode: "text", selector: "main", index: 1 }, ctx(home))
    expect(r.output).toContain("hello world")
    expect(calls[0].args).toMatchObject({ mode: "text", selector: "main", index: 1 })
  })

  test("content truncates oversized output to file", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const big = "x".repeat(20_000)
    const tools = createPlaywrightTools({
      bridge: { request: async () => ({ text: big }) },
    })
    const r = await tools.content.execute({ mode: "text" }, ctx(home))
    expect(r.output).toContain("已截断")
    expect(r.truncated).toBe(true)
  })

  test("screenshot resolves path into session tmp and returns image block", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const { bridge, calls } = recordingBridge()
    const tools = createPlaywrightTools({ bridge })
    const r = await tools.screenshot.execute({ full_page: true }, ctx(home))
    expect(r.output).toMatch(/已截图: tmp\/playwright_\d+\.png/)
    expect(r.output).toContain("绝对路径")
    expect(r.blocks?.[0]?.type).toBe("image")
    expect(calls[0].args).toMatchObject({ fullPage: true }) // 桥协议键驼峰（driver 契约）
    expect(String(calls[0].args.path)).toContain(join("users", "default", "sessions", "0123456789abcdef0123456789abcdef", "tmp"))
  })

  test("interactive ops map failures to output text", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    let fails = 1 // 第一次调用失败，之后成功
    const tools = createPlaywrightTools({
      bridge: {
        request: async () => {
          if (fails-- > 0) throw new Error("element not found")
          return {}
        },
      },
    })
    const r = await tools.click.execute({ selector: "#btn" }, ctx(home))
    expect(r.output).toContain("element not found")
    const ok = await tools.close_page.execute({}, ctx(home))
    expect(ok.output).toBe("标签页已关闭")
  })

  test("hover/dblclick/drag forward selectors to bridge", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const { bridge, calls } = recordingBridge()
    const tools = createPlaywrightTools({ bridge })
    await tools.hover.execute({ selector: "#menu", timeout: 5000 }, ctx(home))
    await tools.dblclick.execute({ selector: "td.name" }, ctx(home))
    await tools.drag.execute({ source: "#item", target: "#drop" }, ctx(home))
    expect(calls[0]).toMatchObject({ op: "hover", args: { selector: "#menu", timeout: 5000 } })
    expect(calls[1]).toMatchObject({ op: "dblclick", args: { selector: "td.name" } })
    expect(calls[2]).toMatchObject({ op: "drag", args: { source: "#item", target: "#drop" } })
  })

  test("upload resolves file paths via ctx and forwards list", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const { bridge, calls } = recordingBridge()
    const tools = createPlaywrightTools({ bridge })
    const r = await tools.upload.execute({ selector: "input[type=file]", files: ["a.png", "b.png"] }, ctx(home))
    expect(r.output).toContain("已设置上传文件（2 个）")
    expect(calls[0].op).toBe("upload")
    expect((calls[0].args.paths as string[]).every((p) => p.includes(join(home, "users", "default", "sessions")))).toBe(true)
    const empty = await tools.upload.execute({ selector: "input", files: [] }, ctx(home))
    expect(empty.output).toContain("缺少 files")
  })

  test("evaluate returns serialized JSON", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const { bridge, calls } = recordingBridge()
    const tools = createPlaywrightTools({ bridge })
    const r = await tools.evaluate.execute({ expression: "document.title" }, ctx(home))
    expect(r.output).toBe(JSON.stringify({ a: 1 }))
    expect(calls[0].args.expression).toBe("document.title")
  })

  test("evaluate rejects empty expression", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    let called = false
    const tools = createPlaywrightTools({ bridge: { request: async () => { called = true; return {} } } })
    const r = await tools.evaluate.execute({ expression: "  " }, ctx(home))
    expect(called).toBe(false)
    expect(r.output).toContain("expression")
  })

  test("evaluate failure suggests content fallback (B3 降级提示)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const tools = createPlaywrightTools({
      bridge: { request: async () => { throw new Error("boom") } },
    })
    const r = await tools.evaluate.execute({ expression: "x.y" }, ctx(home))
    expect(r.output).toContain("boom")
    expect(r.output).toContain("content")
  })

  test("serve_dir starts static server, serves files, and reuses existing (B1 静态服务器)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const root = join(home, "site")
    mkdirSync(root, { recursive: true })
    writeFileSync(join(root, "index.html"), "<h1>hi gebai</h1>")
    const tools = createPlaywrightTools({ bridge: recordingBridge().bridge })
    const c = ctx(home, { resolveProjectPath: () => root })
    const r = await tools.serve_dir.execute({ path: "site" }, c)
    expect(r.output).toContain("已启动静态服务器")
    const url = r.output.match(/http:\/\/127\.0\.0\.1:\d+/)?.[0]
    expect(url).toBeTruthy()
    const res = await fetch(`${url}/`)
    expect(res.status).toBe(200)
    expect(await res.text()).toContain("hi gebai")
    // 目录穿越防护
    const evil = await fetch(`${url}/..%2F..%2Fpackage.json`)
    expect([400, 403, 404]).toContain(evil.status)
    // 重复调用复用
    const r2 = await tools.serve_dir.execute({ path: "site" }, c)
    expect(r2.output).toContain("已在服务中")
  })

  test("serve_dir sandboxed mode only serves preset projects (absolute path rejected)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const tools = createPlaywrightTools({ bridge: recordingBridge().bridge })
    const r = await tools.serve_dir.execute({ path: home }, ctx(home, { sandboxed: true }))
    expect(r.output).toContain("服务端部署")
  })

  test("serve_dir reports missing directory", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const tools = createPlaywrightTools({ bridge: recordingBridge().bridge })
    const r = await tools.serve_dir.execute({ path: "nope" }, ctx(home, { resolveProjectPath: () => join(home, "nope") }))
    expect(r.output).toContain("目录不存在")
  })

  test("pages lists tabs with active marker", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const { bridge } = recordingBridge()
    const tools = createPlaywrightTools({ bridge })
    const r = await tools.pages.execute({}, ctx(home))
    expect(r.output).toContain("[0]")
    expect(r.output).toContain("◀当前")
    expect(r.output).toContain("Example")
  })

  test("same-session requests are serialized", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const order: string[] = []
    let release!: () => void
    const gate = new Promise<void>((r) => (release = r))
    const bridge: BridgeLike = {
      request: async (op) => {
        order.push(op)
        if (op === "open") await gate // 第一个请求挂起，验证第二个不会并发进入
        return {}
      },
    }
    const tools = createPlaywrightTools({ bridge })
    const p1 = tools.open.execute({ url: "https://a.com" }, ctx(home))
    const p2 = tools.click.execute({ selector: "#x" }, ctx(home))
    await new Promise((r) => setTimeout(r, 50))
    expect(order).toEqual(["open"]) // click 被锁住，未并发执行
    release()
    await Promise.all([p1, p2])
    expect(order).toEqual(["open", "click"])
  })

  test("different sessions run concurrently (no cross-session lock)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-"))
    const seen: string[] = []
    let release!: () => void
    const gate = new Promise<void>((r) => (release = r))
    const bridge: BridgeLike = {
      request: async (_op, args) => {
        seen.push(String(args.sessionId))
        await gate
        return {}
      },
    }
    const tools = createPlaywrightTools({ bridge })
    const c1 = ctx(home)
    const c2 = { ...ctx(home), sessionId: "22222222222222222222222222222222" }
    const p1 = tools.open.execute({ url: "https://a.com" }, c1)
    const p2 = tools.open.execute({ url: "https://b.com" }, c2)
    await new Promise((r) => setTimeout(r, 50))
    expect(seen.sort()).toEqual(["0123456789abcdef0123456789abcdef", "22222222222222222222222222222222"]) // 两个会话都进入执行，未被互相阻塞
    release()
    await Promise.all([p1, p2])
  })

  test("requiresApproval covers navigation/interaction/script/credential ops only", () => {
    for (const op of ["open", "click", "fill", "press", "select", "check", "hover", "dblclick", "drag", "upload", "evaluate", "new_page", "serve_dir", "emulate", "cookies", "local_storage", "storage_state"]) {
      expect(playwrightDef.requiresApproval?.[op]).toBe(true)
    }
    for (const op of ["content", "screenshot", "pdf", "downloads", "dialogs", "pages", "wait_for", "switch_page", "close_page", "close", "ocr", "locate", "locate_image"]) {
      expect(playwrightDef.requiresApproval?.[op]).toBeFalsy()
    }
  })

  test("def exposes full toolset with namespaced-safe names", () => {
    expect(playwrightDef.name).toBe("playwright")
    expect(playwrightDef.description.length).toBeGreaterThan(20)
    expect(playwrightDef.systemPrompt).toContain("浏览器自动化")
    expect(playwrightDef.preload).toBe(false)
    const expected = [
      "open", "content", "screenshot", "click", "fill", "press", "select", "check", "hover", "dblclick", "drag", "upload",
      "wait_for", "evaluate", "pages", "new_page", "switch_page", "close_page", "close", "serve_dir",
      "pdf", "downloads", "dialogs", "emulate", "cookies", "local_storage", "storage_state",
      "ocr", "locate", "locate_image",
    ]
    for (const t of expected) {
      expect(playwrightDef.tools?.[t]).toBeDefined()
      expect(playwrightDef.tools?.[t].name).toBe(t)
    }
    expect(Object.keys(playwrightDef.tools ?? {})).toHaveLength(expected.length)
  })
})
