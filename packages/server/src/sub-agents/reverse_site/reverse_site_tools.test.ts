import { describe, expect, test } from "bun:test"
import { mkdirSync } from "node:fs"
import { join } from "node:path"
import type { ToolContext } from "../../core/types"
import { createCaptureTools, createHttpRequestTool, formatHttpResult, parseJsonObject, redactHeaders, type CapturedRequest, type FetchLike } from "./reverse_site_tools"
import type { BridgeLike } from "../playwright/playwright_tools"
import { def as reverseSiteDef } from "./reverse_site"

function ctx(home: string, overrides: Partial<ToolContext> = {}): ToolContext {
  const tmp = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(tmp, { recursive: true })
  const base: ToolContext = {
    user: "default",
    sessionId: "s1",
    workdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => join(tmp, p),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, content) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(join(p, ".."), { recursive: true })
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
function recordingBridge(opResults: Record<string, unknown>) {
  const calls: Array<{ op: string; args: Record<string, unknown> }> = []
  const bridge: BridgeLike = {
    request: async (op, args) => {
      calls.push({ op, args })
      return opResults[op] ?? {}
    },
  }
  return { bridge, calls }
}

const sampleEntries: CapturedRequest[] = [
  { id: 1, time: Date.now(), method: "GET", url: "https://api.example.com/v1/users?page=1", resourceType: "xhr", status: 200 },
  { id: 2, time: Date.now(), method: "POST", url: "https://api.example.com/v1/users", resourceType: "fetch", status: 201, requestHeaders: { "content-type": "application/json" }, postData: '{"name":"a","password":"***"}', responseHeaders: { "content-type": "application/json" }, body: "{\"id\":1}" },
]

describe("parseJsonObject", () => {
  test("parses json string and object literals", () => {
    expect(parseJsonObject('{"a":"1","b":"2"}')).toEqual({ a: "1", b: "2" })
    expect(parseJsonObject({ a: 1, b: true })).toEqual({ a: "1", b: "true" })
    expect(parseJsonObject(undefined)).toBeNull()
    expect(parseJsonObject("")).toBeNull()
  })
  test("throws on invalid json", () => {
    expect(() => parseJsonObject("not-json")).toThrow(/JSON 对象/)
    expect(() => parseJsonObject("[1,2]")).toThrow(/JSON 对象/)
  })
})

describe("redactHeaders", () => {
  test("masks sensitive header values", () => {
    const out = redactHeaders({ "content-type": "application/json", Authorization: "Bearer xyz", "Set-Cookie": "sid=1" })
    expect(out["content-type"]).toBe("application/json")
    expect(out.Authorization).toBe("***")
    expect(out["Set-Cookie"]).toBe("***")
  })
})

describe("formatHttpResult", () => {
  test("renders status, headers and body", () => {
    const out = formatHttpResult(200, "OK", { "content-type": "application/json" }, '{"ok":true}')
    expect(out).toContain("HTTP 200 OK")
    expect(out).toContain("content-type: application/json")
    expect(out).toContain('{"ok":true}')
  })
})

describe("http_request", () => {
  test("sends request and returns status/headers/body", async () => {
    const seen: Array<{ url: string; init?: RequestInit }> = []
    const fetcher = (async (url: string | URL | Request, init?: RequestInit) => {
      seen.push({ url: String(url), init })
      return new Response('{"ok":true}', {
        status: 200,
        statusText: "OK",
        headers: { "content-type": "application/json", "set-cookie": "sid=1" },
      })
    }) satisfies FetchLike
    const tool = createHttpRequestTool({ fetch: fetcher })
    const r = await tool.execute({ url: "https://api.example.com/v1/users", params: '{"page":"2"}', method: "POST", body: '{"name":"a"}', headers: '{"X-Test":"1"}' }, ctx("/tmp/gebai-http1"))
    expect(seen).toHaveLength(1)
    expect(seen[0].url).toBe("https://api.example.com/v1/users?page=2")
    expect(seen[0].init?.method).toBe("POST")
    expect(seen[0].init?.body).toBe('{"name":"a"}')
    expect((seen[0].init?.headers as Record<string, string>)["X-Test"]).toBe("1")
    expect((seen[0].init?.headers as Record<string, string>)["Content-Type"]).toBe("application/json")
    expect(r.output).toContain("HTTP 200 OK")
    expect(r.output).toContain('{"ok":true}')
    // set-cookie 脱敏
    expect(r.output).toContain("set-cookie: ***")
  })

  test("defaults content-type to json when body given", async () => {
    const seen: Array<RequestInit | undefined> = []
    const fetcher = (async (_url: unknown, init?: RequestInit) => {
      seen.push(init)
      return new Response("ok")
    }) satisfies FetchLike
    const tool = createHttpRequestTool({ fetch: fetcher })
    await tool.execute({ url: "https://api.example.com/x", method: "POST", body: "{}" }, ctx("/tmp/gebai-http2"))
    expect((seen[0]?.headers as Record<string, string>)["Content-Type"]).toBe("application/json")
  })

  test("rejects private urls when sandboxed", async () => {
    let called = false
    const fetcher = (async () => { called = true; return new Response("x") }) satisfies FetchLike
    const tool = createHttpRequestTool({ fetch: fetcher })
    const r = await tool.execute({ url: "http://127.0.0.1:8080/admin" }, ctx("/tmp/gebai-http3", { sandboxed: true }))
    expect(called).toBe(false)
    expect(r.output).toContain("URL 不允许")
  })

  test("does not restrict private urls in local mode", async () => {
    let url = ""
    const fetcher = (async (u: string) => { url = u; return new Response("ok") }) satisfies FetchLike
    const tool = createHttpRequestTool({ fetch: fetcher })
    await tool.execute({ url: "http://127.0.0.1:8080/admin" }, ctx("/tmp/gebai-http4", { sandboxed: false }))
    expect(url).toBe("http://127.0.0.1:8080/admin")
  })

  test("returns error on invalid headers json", async () => {
    const tool = createHttpRequestTool({ fetch: (async () => new Response("x")) satisfies FetchLike })
    const r = await tool.execute({ url: "https://api.example.com/x", headers: "bad" }, ctx("/tmp/gebai-http5"))
    expect(r.output).toMatch(/JSON 对象/)
  })

  test("reports fetch failures", async () => {
    const fetcher = (async () => { throw new Error("boom") }) satisfies FetchLike
    const tool = createHttpRequestTool({ fetch: fetcher })
    const r = await tool.execute({ url: "https://api.example.com/x" }, ctx("/tmp/gebai-http6"))
    expect(r.output).toContain("http_request 失败: boom")
  })
})

describe("capture tools", () => {
  test("start/stop/clear forward to bridge with sessionId", async () => {
    const { bridge, calls } = recordingBridge({ network_start: { captured: 3 }, network_stop: { captured: 4 }, network_clear: { cleared: 4 } })
    const tools = createCaptureTools({ bridge })
    const c = ctx("/tmp/gebai-cap1")
    const start = await tools.capture_start.execute({}, c)
    expect(start.output).toContain("已开始记录")
    expect(calls[0]).toEqual({ op: "network_start", args: { sessionId: "s1" } })
    const stop = await tools.capture_stop.execute({}, c)
    expect(stop.output).toContain("4 条")
    expect(calls[1].op).toBe("network_stop")
    const clear = await tools.capture_clear.execute({}, c)
    expect(clear.output).toContain("已清空 4 条")
    expect(calls[2].op).toBe("network_clear")
  })

  test("capture_list renders summary and detail with redaction preserved", async () => {
    const { bridge, calls } = recordingBridge({ network_list: { entries: sampleEntries, captured: 2, recording: false } })
    const tools = createCaptureTools({ bridge })
    const c = ctx("/tmp/gebai-cap2")
    const summary = await tools.capture_list.execute({}, c)
    expect(summary.output).toContain("GET https://api.example.com/v1/users?page=1")
    expect(summary.output).toContain("200")
    expect(summary.output).not.toContain("postData")
    expect(calls[0].args.detail).toBe(false)

    const detail = await tools.capture_list.execute({ detail: true }, c)
    expect(detail.output).toContain("请求体: ")
    expect(detail.output).toContain('password":"***"')
    expect(detail.output).not.toContain('password":"x"')
  })

  test("capture_list filters pass through and empty result hints", async () => {
    const { bridge, calls } = recordingBridge({ network_list: { entries: [], captured: 0, recording: true } })
    const tools = createCaptureTools({ bridge })
    const c = ctx("/tmp/gebai-cap3")
    const r = await tools.capture_list.execute({ method: "POST", url: "users", status: 200 }, c)
    expect(calls[0].args).toMatchObject({ method: "POST", url: "users", status: 200 })
    expect(r.output).toContain("capture_start")
  })

  test("capture_list saves full record to file", async () => {
    const home = "/tmp/gebai-cap4"
    const { bridge } = recordingBridge({ network_list: { entries: sampleEntries, captured: 2, recording: false } })
    const tools = createCaptureTools({ bridge })
    const c = ctx(home)
    const r = await tools.capture_list.execute({ detail: true, file: "api_capture.json" }, c)
    expect(r.output).toContain("api_capture.json")
    const saved = JSON.parse(await Bun.file(join(home, "users", "default", "sessions", "s1", "tmp", "api_capture.json")).text())
    expect(saved).toHaveLength(2)
    expect(saved[0].method).toBe("GET")
  })
})

describe("reverse_site def", () => {
  test("name/tools conform to namespace rules", () => {
    expect(reverseSiteDef.name).toMatch(/^[a-z0-9_]+$/)
    for (const t of Object.keys(reverseSiteDef.tools ?? {})) {
      expect(t).toMatch(/^[a-zA-Z0-9_]+$/)
      expect(`${reverseSiteDef.name}_${t}`.length).toBeLessThanOrEqual(40)
    }
  })
  test("依赖 playwright（自动连带装载）：只声明接口逆向独有工具，不复刻 playwright/全局工具", () => {
    const names = Object.keys(reverseSiteDef.tools ?? {})
    for (const t of ["http_request", "capture_start", "capture_stop", "capture_clear", "capture_list"]) {
      expect(names).toContain(t)
    }
    // 浏览器自动化全套经 dependencies 连带装载（playwright_ 命名空间）；文件/编排工具走全局名——本 def 不重复声明
    for (const t of ["open", "content", "evaluate", "screenshot", "fetch_url", "read", "write", "agent_run", "agent_list", "agent_load"]) {
      expect(names).not.toContain(t)
    }
    expect(reverseSiteDef.dependencies).toEqual(["playwright"])
    expect(reverseSiteDef.requiresApproval).toMatchObject({ http_request: true })
    expect(reverseSiteDef.requiresApproval?.open).toBeUndefined() // 审批映射由 playwright def 单源维护
  })
})
