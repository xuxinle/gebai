import { describe, expect, test } from "bun:test"
import { mkdirSync, mkdtempSync, writeFileSync, readFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { ToolContext } from "../../core/base/types"
import { createPlaywrightSessionTools } from "./playwright_session_tools"
import type { BridgeLike } from "../../core/browser/bridge"

function ctx(home: string, overrides: Partial<ToolContext> = {}): ToolContext {
  const sid = "0123456789abcdef0123456789abcdef"
  const tmp = join(home, "users", "default", "sessions", sid, "tmp")
  mkdirSync(tmp, { recursive: true })
  const base: ToolContext = {
    user: "default",
    sessionId: sid,
    workdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => join(tmp, String(p).replace(/^tmp[\\/]/, "")), // 真实 sandbox 会剥离 tmp/ 前缀后基于会话 tmp 解析
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

describe("session tools", () => {
  test("pdf resolves default path and forwards format/landscape", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pws1-"))
    const { bridge, calls } = recordingBridge({ pdf: { path: "x" } })
    const tools = createPlaywrightSessionTools({ bridge })
    const r = await tools.pdf.execute({ format: "A3", landscape: true }, ctx(home))
    expect(r.output).toMatch(/已导出 PDF: tmp\/playwright_pdf_\d+\.pdf/)
    expect(r.blocks?.[0]?.type).toBe("file")
    expect(calls[0].args).toMatchObject({ format: "A3", landscape: true })
    const r2 = await tools.pdf.execute({ file: "tmp/report.pdf" }, ctx(home))
    expect(r2.output).toContain("tmp/report.pdf")
  })

  test("downloads lists and saves by index", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pws2-"))
    const src = join(home, "raw-download.xlsx")
    writeFileSync(src, "binary-ish")
    const { bridge, calls } = recordingBridge({
      downloads_list: { downloads: [{ filename: "report.xlsx", path: src, url: "https://a.com/report.xlsx", time: 1 }] },
    })
    const tools = createPlaywrightSessionTools({ bridge })
    const list = await tools.downloads.execute({}, ctx(home))
    expect(list.output).toContain("report.xlsx")
    expect(list.output).toContain("https://a.com/report.xlsx")
    const save = await tools.downloads.execute({ action: "save", index: 0, dest: "tmp/report.xlsx" }, ctx(home))
    expect(save.output).toContain("已保存下载到: tmp/report.xlsx")
    expect(readFileSync(join(home, "users", "default", "sessions", "0123456789abcdef0123456789abcdef", "tmp", "report.xlsx"), "utf8")).toBe("binary-ish")
    expect(calls[0].op).toBe("downloads_list")
  })
  test("downloads empty hint and invalid index", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pws3-"))
    const { bridge } = recordingBridge({ downloads_list: { downloads: [] } })
    const tools = createPlaywrightSessionTools({ bridge })
    const empty = await tools.downloads.execute({}, ctx(home))
    expect(empty.output).toContain("暂无下载记录")
    const bad = await tools.downloads.execute({ action: "save", index: 5 }, ctx(home))
    expect(bad.output).toContain("序号无效")
  })

  test("dialogs auto/list/clear forward mode and prompt text", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pws4-"))
    const { bridge, calls } = recordingBridge({
      dialog_auto: { auto: { mode: "accept", promptText: "ok" } },
      dialog_list: { dialogs: [{ type: "confirm", message: "确认删除?", defaultText: "", handled: "accept", time: 1 }], auto: { mode: "accept", promptText: "" } },
      dialog_clear: { cleared: 1 },
    })
    const tools = createPlaywrightSessionTools({ bridge })
    const auto = await tools.dialogs.execute({ action: "auto", mode: "accept", prompt_text: "ok" }, ctx(home))
    expect(auto.output).toContain("accept")
    expect(calls[0].args).toMatchObject({ mode: "accept", promptText: "ok" })
    const list = await tools.dialogs.execute({}, ctx(home))
    expect(list.output).toContain("[confirm] 确认删除?")
    const clear = await tools.dialogs.execute({ action: "clear" }, ctx(home))
    expect(clear.output).toContain("已清空 1 条")
  })

  test("emulate maps snake_case to bridge keys and validates viewport pair", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pws5-"))
    const { bridge, calls } = recordingBridge({ emulate: { emulated: { viewport: { width: 390, height: 844 } } } })
    const tools = createPlaywrightSessionTools({ bridge })
    const r = await tools.emulate.execute({ user_agent: "UA/1.0", locale: "zh-CN", timezone: "Asia/Shanghai", width: 390, height: 844, mobile: true }, ctx(home))
    expect(r.output).toContain("仿真已应用")
    expect(calls[0].args).toMatchObject({ userAgent: "UA/1.0", locale: "zh-CN", timezoneId: "Asia/Shanghai", width: 390, height: 844, mobile: true }) // 桥协议键驼峰（driver 契约）
    const bad = await tools.emulate.execute({ width: 100 }, ctx(home))
    expect(bad.output).toContain("width 与 height")
    const none = await tools.emulate.execute({}, ctx(home))
    expect(none.output).toContain("未提供仿真参数")
    const reset = await tools.emulate.execute({ action: "reset" }, ctx(home))
    expect(reset.output).toContain("已恢复默认")
    expect(calls.at(-1)?.args.reset).toBe(true)
  })

  test("cookies list renders values, set parses JSON, urls filter passthrough", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pws6-"))
    const { bridge, calls } = recordingBridge({
      cookies_get: { cookies: [{ name: "sid", value: "abc", domain: ".a.com", path: "/", httpOnly: true, secure: true }] },
      cookies_set: { set: 1 },
      cookies_clear: { cleared: true },
    })
    const tools = createPlaywrightSessionTools({ bridge })
    const list = await tools.cookies.execute({ urls: '["https://a.com"]' }, ctx(home))
    expect(list.output).toContain("sid=abc")
    expect(list.output).toContain("[httpOnly]")
    expect(calls[0].args).toMatchObject({ urls: ["https://a.com"] })
    const set = await tools.cookies.execute({ action: "set", cookies: '[{"name":"sid","value":"v","url":"https://a.com"}]' }, ctx(home))
    expect(set.output).toContain("已注入 1 个")
    expect(calls[1].args.cookies).toEqual([{ name: "sid", value: "v", url: "https://a.com" }])
    const bad = await tools.cookies.execute({ action: "set", cookies: "not-json" }, ctx(home))
    expect(bad.output).toContain("不是合法 JSON")
    const clear = await tools.cookies.execute({ action: "clear" }, ctx(home))
    expect(clear.output).toContain("已清空")
  })

  test("local_storage actions forward key/value", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pws7-"))
    const { bridge, calls } = recordingBridge({
      local_storage: { items: { token: "t1", theme: "dark" } },
    })
    const tools = createPlaywrightSessionTools({ bridge })
    const list = await tools.local_storage.execute({}, ctx(home))
    expect(list.output).toContain("token = t1")
    const r = await tools.local_storage.execute({ action: "get", key: "token" }, ctx(home))
    void r
    expect(calls[1].args).toMatchObject({ action: "get", key: "token" })
    await tools.local_storage.execute({ action: "set", key: "theme", value: "light" }, ctx(home))
    expect(calls[2].args).toMatchObject({ action: "set", key: "theme", value: "light" })
    const missing = await tools.local_storage.execute({ action: "set", key: "x" }, ctx(home))
    expect(missing.output).toContain("缺少 value")
  })

  test("storage_state save writes file and restore forwards state", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pws8-"))
    const state = { cookies: [{ name: "sid", value: "v", domain: ".a.com", path: "/" }], origins: [{ origin: "https://a.com", localStorage: [{ name: "t", value: "1" }] }] }
    const { bridge, calls } = recordingBridge({ storage_save: { state, cookies: 1, origins: 1 }, storage_apply: { cookies: 1, origins: 1, warnings: [] } })
    const tools = createPlaywrightSessionTools({ bridge })
    const save = await tools.storage_state.execute({ file: "tmp/login.json" }, ctx(home))
    expect(save.output).toContain("登录态已保存")
    expect(JSON.parse(readFileSync(join(home, "users", "default", "sessions", "0123456789abcdef0123456789abcdef", "tmp", "login.json"), "utf8"))).toEqual(state)
    const restore = await tools.storage_state.execute({ action: "restore", file: "tmp/login.json" }, ctx(home))
    expect(restore.output).toContain("登录态已恢复")
    expect(calls[1].op).toBe("storage_apply")
    expect((calls[1].args as { state: unknown }).state).toEqual(state)
  })
  test("storage_state restore hints missing file and rejects private origin when sandboxed", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pws9-"))
    const { bridge, calls } = recordingBridge({ storage_apply: {} })
    const tools = createPlaywrightSessionTools({ bridge })
    const missing = await tools.storage_state.execute({ action: "restore" }, ctx(home))
    expect(missing.output).toContain("不存在")
    const state = { cookies: [], origins: [{ origin: "http://127.0.0.1:8080", localStorage: [] }] }
    {
      const { writeFile } = await import("node:fs/promises")
      await writeFile(join(home, "users", "default", "sessions", "0123456789abcdef0123456789abcdef", "tmp", "browser_state.json"), JSON.stringify(state))
    }
    const rejected = await tools.storage_state.execute({ action: "restore" }, ctx(home, { sandboxed: true }))
    expect(rejected.output).toContain("URL 不允许")
    expect(calls.some((c) => c.op === "storage_apply")).toBe(false)
  })
})
