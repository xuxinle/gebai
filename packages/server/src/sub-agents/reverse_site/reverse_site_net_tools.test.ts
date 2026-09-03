import { describe, expect, test } from "bun:test"
import { mkdirSync } from "node:fs"
import { join } from "node:path"
import type { ToolContext } from "../../core/base/types"
import { buildHar, createNetTools } from "./reverse_site_net_tools"
import type { CapturedRequest } from "./reverse_site_tools"
import type { BridgeLike } from "../playwright/playwright_tools"

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

const detailEntries = [
  {
    id: 1,
    time: 1700000000000,
    method: "POST",
    url: "https://a.com/api/v1/items?page=1&size=20",
    resourceType: "fetch",
    status: 200,
    error: null,
    requestHeaders: { "content-type": "application/json", authorization: "***" },
    postData: '{"q":"x"}',
    responseHeaders: { "content-type": "application/json" },
    body: '{"items":[]}',
  },
] satisfies CapturedRequest[]

describe("buildHar", () => {
  test("maps captured entries to HAR 1.2 structure", () => {
    const har = buildHar(detailEntries as never) as {
      log: { version: string; creator: { name: string }; entries: Array<Record<string, any>> }
    }
    expect(har.log.version).toBe("1.2")
    const e = har.log.entries[0]
    expect(e.request.method).toBe("POST")
    expect(e.request.url).toContain("/api/v1/items")
    expect(e.request.queryString).toContainEqual({ name: "page", value: "1" })
    expect(e.request.headers).toContainEqual({ name: "content-type", value: "application/json" })
    // 脱敏口径保留（authorization = ***）
    expect(e.request.headers).toContainEqual({ name: "authorization", value: "***" })
    expect(e.request.postData.text).toBe('{"q":"x"}')
    expect(e.response.status).toBe(200)
    expect(e.response.content.text).toBe('{"items":[]}')
    expect(e.response.content.mimeType).toBe("application/json")
    expect(e.timings).toEqual({ send: 0, wait: 0, receive: 0 })
    expect(e.startedDateTime).toBe(new Date(1700000000000).toISOString())
  })
  test("tolerates invalid url", () => {
    const har = buildHar([{ ...detailEntries[0], url: "::not-a-url::" } as never]) as { log: { entries: Array<Record<string, any>> } }
    expect(har.log.entries[0].request.queryString).toEqual([])
  })
})

describe("net tools", () => {
  test("capture_har writes HAR file and returns file block", async () => {
    const home = "/tmp/gebai-net1"
    const { bridge, calls } = recordingBridge({ network_list: { entries: detailEntries, captured: 1, recording: false } })
    const tools = createNetTools({ bridge })
    const r = await tools.capture_har.execute({ file: "out.har" }, ctx(home))
    expect(r.output).toContain("已导出 1 条")
    expect(r.blocks?.[0]?.type).toBe("file")
    expect(calls[0].args).toMatchObject({ detail: true, limit: 500 })
    const saved = JSON.parse(await Bun.file(join(home, "users", "default", "sessions", "s1", "tmp", "out.har")).text())
    expect(saved.log.version).toBe("1.2")
  })
  test("capture_har hints when nothing recorded", async () => {
    const { bridge } = recordingBridge({ network_list: { entries: [], captured: 0, recording: false } })
    const tools = createNetTools({ bridge })
    const r = await tools.capture_har.execute({}, ctx("/tmp/gebai-net2"))
    expect(r.output).toContain("capture_start")
  })

  test("capture_ws renders frames with direction markers", async () => {
    const { bridge, calls } = recordingBridge({
      network_ws_list: {
        frames: [
          { id: 1, time: 1700000000000, dir: "sent", url: "wss://a.com/ws", payload: '{"op":"sub"}' },
          { id: 2, time: 1700000000001, dir: "recv", url: "wss://a.com/ws", payload: '{"data":1}' },
        ],
        total: 2,
        recording: true,
      },
    })
    const tools = createNetTools({ bridge })
    const r = await tools.capture_ws.execute({ url: "a.com", last: 50 }, ctx("/tmp/gebai-net3"))
    expect(r.output).toContain("↑发")
    expect(r.output).toContain("↓收")
    expect(r.output).toContain('{"op":"sub"}')
    expect(calls[0].args).toMatchObject({ url: "a.com", last: 50 })
  })
  test("capture_ws empty hint mentions recording state", async () => {
    const { bridge } = recordingBridge({ network_ws_list: { frames: [], total: 0, recording: false } })
    const tools = createNetTools({ bridge })
    const r = await tools.capture_ws.execute({}, ctx("/tmp/gebai-net4"))
    expect(r.output).toContain("未捕获到 WebSocket 帧")
  })

  test("route add forwards mode/mock fields; list/clear passthrough", async () => {
    const { bridge, calls } = recordingBridge({ route_add: { added: "**/api/**", mode: "mock", total: 1 }, route_list: { routes: [{ pattern: "**/api/**", mode: "mock", status: 200 }] }, route_clear: { cleared: 1 } })
    const tools = createNetTools({ bridge })
    const c = ctx("/tmp/gebai-net5")
    const add = await tools.route.execute({ pattern: "**/api/**", mode: "mock", status: 201, content_type: "text/html", body: "<b/>", headers: '{"x-a":"1"}' }, c)
    expect(add.output).toContain("[mock] **/api/**")
    expect(calls[0].args).toMatchObject({ pattern: "**/api/**", mode: "mock", status: 201, contentType: "text/html", body: "<b/>", headers: { "x-a": "1" } })
    const list = await tools.route.execute({ action: "list" }, c)
    expect(list.output).toContain("[mock] **/api/**")
    const clear = await tools.route.execute({ action: "clear" }, c)
    expect(clear.output).toContain("已清空 1 条")
  })
  test("route add defaults to block mode without headers", async () => {
    const { bridge, calls } = recordingBridge({ route_add: { added: "**/*.jpg", mode: "block", total: 1 } })
    const tools = createNetTools({ bridge })
    const r = await tools.route.execute({ pattern: "**/*.jpg" }, ctx("/tmp/gebai-net6"))
    expect(r.output).toContain("[block] **/*.jpg")
    expect(calls[0].args).toMatchObject({ mode: "block", headers: null })
  })
  test("route reports invalid headers json", async () => {
    const { bridge } = recordingBridge({ route_add: {} })
    const tools = createNetTools({ bridge })
    const r = await tools.route.execute({ pattern: "**", mode: "modify", headers: "not-json" }, ctx("/tmp/gebai-net7"))
    expect(r.output).toMatch(/JSON 对象/)
  })
})
