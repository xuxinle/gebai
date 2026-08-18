import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname, resolve } from "node:path"
import { jsTool, buildChildScript, scriptSessionContext, jsRuntimeCommand, JS_TOOL_MAX_CALLS, makeDynamicTool } from "./js-tool"
import { writeTool } from "./tools"
import type { ToolContext, Tool, ToolResult } from "./types"

function ctx(home: string, sessionId = "s1", env: Record<string, string> = {}): ToolContext {
  const base = home || tmpdir()
  const tmp = join(base, "users", "default", "sessions", sessionId, "tmp")
  mkdirSync(tmp, { recursive: true })
  return {
    user: "default",
    sessionId,
    workdir: tmp,
    home: base,
    env,
    sandboxed: false,
    resolvePath: (p) => resolve(tmp, p),
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
    resolveProjectPath: () => {
      throw new Error("未知预置项目")
    },
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
}

function mkTool(name: string, execute: (args: Record<string, unknown>, ctx: ToolContext) => Promise<ToolResult>): Tool {
  return { name, description: "", parameters: { type: "object", properties: {} }, execute }
}

/** 注册 mock 工具的 ctx：tools.echo 原样回显参数、tools.boom 抛错、tools.writefile 落盘。 */
function ctxWithTools(home: string, extra: ToolContext["registry"] = undefined as never): ToolContext {
  const c = ctx(home)
  const echo: Tool = mkTool("echo", async (args) => ({ output: `echo:${JSON.stringify(args)}`, data: args }))
  const boom: Tool = mkTool("boom", async () => {
    throw new Error("爆炸")
  })
  const tools: Record<string, Tool> = { echo, boom, write: writeTool }
  c.registry = {
    schemas: () => Object.keys(tools).map((name) => ({ name, description: "", parameters: { type: "object", properties: {} } })),
    resolve: (name) => (tools[name] ? { name, tool: tools[name] } : undefined),
    getAgentNames: () => [],
  }
  void extra
  return c
}

describe("js 脚本工具", () => {
  test("工具像内置函数一样直接调用（无 tools. 前缀），返回 output/data", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-fn-"))
    const c = ctxWithTools(home)
    const r = await jsTool.execute(
      {
        code: `const r = await echo({ msg: "fn" })
const r2 = await tools.echo({ msg: "ns" })
return { fn: r.data.msg, ns: r2.data.msg }`,
      },
      c,
    )
    expect(r.output).toContain("[返回值]")
    const data = r.data as { result: { fn: string; ns: string }; calls: Array<{ name: string; ok: boolean }> }
    expect(data.result.fn).toBe("fn")
    expect(data.result.ns).toBe("ns")
    expect(data.calls.map((x) => x.name)).toEqual(["echo", "echo"])
    rmSync(home, { recursive: true, force: true })
  })

  test("同名变量遮蔽工具函数不冲突（用户代码作用域在内层）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-shadow-"))
    const c = ctxWithTools(home)
    const r = await jsTool.execute(
      {
        code: `const echo = (s) => s + "-local"
return [echo("x"), (await tools.call("echo", {})).output.startsWith("echo:") ]`,
      },
      c,
    )
    const data = r.data as { result: [string, boolean] }
    expect(data.result[0]).toBe("x-local")
    expect(data.result[1]).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })

  test("tools API 直接调用其他工具并取回 output/data", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-echo-"))
    const c = ctxWithTools(home)
    const r = await jsTool.execute(
      {
        code: `const r = await tools.echo({ msg: "hi", n: 3 })
console.log(r.output)
return { got: r.data, len: 2 }`,
      },
      c,
    )
    expect(r.output).toContain('echo:{"msg":"hi","n":3}')
    expect(r.output).toContain("[返回值]")
    const data = r.data as { exitCode: number; result: { got: { msg: string }; len: number }; calls: Array<{ name: string; ok: boolean }> }
    expect(data.exitCode).toBe(0)
    expect(data.result.got.msg).toBe("hi")
    expect(data.calls).toEqual([{ name: "echo", ok: true }])
    rmSync(home, { recursive: true, force: true })
  })

  test("工具抛错在脚本内可 try/catch 容错，未捕获则脚本失败", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-err-"))
    const c = ctxWithTools(home)
    // 容错：捕获后继续
    const ok = await jsTool.execute(
      {
        code: `let err = null
try { await tools.boom({}) } catch (e) { err = e.message }
console.log("caught:", err)`,
      },
      c,
    )
    expect(ok.output).toContain("caught: 工具 boom 执行失败: 爆炸")
    // 未捕获：失败结果 + calls 记录错误 + strict 抛工具级错误
    const fail = await jsTool.execute({ code: `await tools.boom({})` }, c)
    expect(fail.output).toContain("[脚本失败]")
    expect(((fail.data as { calls: Array<{ ok: boolean; error: string }> }).calls)[0].ok).toBe(false)
    await expect(jsTool.execute({ code: `await tools.boom({})`, strict: true }, c)).rejects.toThrow(/脚本执行失败/)
    rmSync(home, { recursive: true, force: true })
  })

  test("ctx 注入会话上下文（user/sessionId/workdir/env/projects/messages）与 input", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-ctx-"))
    const c = ctxWithTools(home)
    c.projects = [{ name: "demo", path: "/tmp/demo" }]
    c.recentMessages = () => [
      { role: "user", content: "帮我统计" },
      { role: "assistant", content: "好的" },
    ]
    const r = await jsTool.execute(
      {
        code: `console.log(ctx.user, ctx.sessionId, ctx.sandboxed)
console.log(ctx.projects[0].name, ctx.messages.length, typeof ctx.env)
console.log("input:", JSON.stringify(input))`,
        input: { from: "flow" },
      },
      c,
    )
    expect(r.output).toContain("default s1 false")
    expect(r.output).toContain("demo 2 object")
    expect(r.output).toContain('input: {"from":"flow"}')
    rmSync(home, { recursive: true, force: true })
  })

  test("沙箱模式 ctx.env 与子进程 env 剔除敏感变量", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-sens-"))
    const c = ctxWithTools(home)
    c.sandboxed = true
    c.env = { MY_API_KEY: "secret-value", MY_FLAG: "1" }
    process.env.__GEBAI_TEST_TOKEN = "leak-me"
    try {
      const snap = scriptSessionContext(c)
      expect((snap.env as Record<string, string>).MY_API_KEY).toBeUndefined()
      expect((snap.env as Record<string, string>).MY_FLAG).toBe("1")
      expect((snap.env as Record<string, string>).__GEBAI_TEST_TOKEN).toBeUndefined()
      const r = await jsTool.execute({ code: `return { key: process.env.MY_API_KEY ?? "stripped", flag: process.env.MY_FLAG ?? "" }` }, c)
      const data = r.data as { result: { key: string; flag: string } }
      expect(data.result.key).toBe("stripped")
      expect(data.result.flag).toBe("1")
    } finally {
      delete process.env.__GEBAI_TEST_TOKEN
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("未知工具与防嵌套 js 调用返回错误（不中断进程）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-nest-"))
    const c = ctxWithTools(home)
    const r = await jsTool.execute(
      {
        code: `const errs = []
for (const name of ["nope", "js"]) {
  try { await tools.call(name, {}) } catch (e) { errs.push(e.message) }
}
return errs`,
      },
      c,
    )
    const data = r.data as { result: string[] }
    expect(data.result[0]).toContain("未知工具: nope")
    expect(data.result[1]).toContain("防嵌套")
    rmSync(home, { recursive: true, force: true })
  })

  test("安全模式：RPC 分发层拦截风险工具（同 flow step 层规则）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-safe-"))
    const c = ctxWithTools(home)
    c.safeMode = true
    const r = await jsTool.execute(
      {
        code: `try { await tools.write({ path: "x.txt", content: "1" }) } catch (e) { console.log(e.message.slice(0, 6)) }`,
      },
      c,
    )
    expect(r.output).toContain("安全模式")
    const data = r.data as { calls: Array<{ ok: boolean }> }
    expect(data.calls[0].ok).toBe(false)
    rmSync(home, { recursive: true, force: true })
  })

  test("内部工具调用经同一 ctx 守卫生效（writeGuard/fileGuard）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-guard-"))
    const c = ctxWithTools(home)
    let guarded: string[] = []
    c.writeGuard = async (paths) => {
      guarded = paths
      return "写范围受限：本会话禁止写入"
    }
    const r = await jsTool.execute({ code: `return (await tools.write({ path: "a.txt", content: "x" })).output` }, c)
    const data = r.data as { result: string }
    expect(data.result).toContain("写范围受限")
    expect(guarded.length).toBe(1)
    rmSync(home, { recursive: true, force: true })
  })

  test("工具调用总数超上限被拒（防失控）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-cap-"))
    const c = ctxWithTools(home)
    const r = await jsTool.execute(
      {
        code: `let n = 0
for (let i = 0; i < ${JS_TOOL_MAX_CALLS + 5}; i++) {
  try { await tools.echo({ i }) ; n++ } catch (e) { return { n, err: e.message.includes("超上限") } }
}`,
        timeout: 120,
      },
      c,
    )
    const data = r.data as { result: { n: number; err: boolean } }
    expect(data.result.n).toBe(JS_TOOL_MAX_CALLS)
    expect(data.result.err).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })

  test("语法错误返回失败结果（不抛工具级异常，strict 抛）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-syntax-"))
    const c = ctxWithTools(home)
    const r = await jsTool.execute({ code: `this is not valid js (((` }, c)
    expect(r.output).toContain("[脚本失败]")
    expect((r.data as { exitCode: number }).exitCode).not.toBe(0)
    await expect(jsTool.execute({ code: `(((`, strict: true }, c)).rejects.toThrow(/脚本执行失败/)
    rmSync(home, { recursive: true, force: true })
  })

  test("超时终止：死循环脚本按 timeout 杀进程并返回超时结果", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-timeout-"))
    const c = ctxWithTools(home)
    const start = Date.now()
    const r = await jsTool.execute({ code: `while (true) { await new Promise(r => setTimeout(r, 50)) }`, timeout: 2 }, c)
    expect(Date.now() - start).toBeLessThan(20000)
    expect(r.output).toContain("[脚本失败]")
    expect(r.output).toContain("timed out")
    expect((r.data as { timedOut: boolean; exitCode: number }).timedOut).toBe(true)
    expect((r.data as { exitCode: number }).exitCode).toBe(124)
    rmSync(home, { recursive: true, force: true })
  })

  test("取消信号：abort 时终止子进程", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-abort-"))
    const c = ctxWithTools(home)
    const ac = new AbortController()
    const p = jsTool.execute({ code: `while (true) { await new Promise(r => setTimeout(r, 50)) }` }, c)
    setTimeout(() => ac.abort(), 300)
    c.signal = ac.signal
    const r = await p
    expect(r.output).toContain("[interrupted]")
    rmSync(home, { recursive: true, force: true })
  })

  test("脚本可读写会话工作目录文件（Bun API 与 write 工具一致视角）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-files-"))
    const c = ctxWithTools(home)
    const readTool: Tool = mkTool("read", async (args) => ({ output: await Bun.file(resolve(c.workdir, String(args.path))).text() }))
    ;(c.registry as unknown as { resolve: (n: string) => unknown }).resolve = (name: string) =>
      name === "read" ? { name: "read", tool: readTool } : undefined
    const r = await jsTool.execute(
      {
        code: `await Bun.write("data.txt", "line1\\nline2")
const txt = await (await tools.read({ path: "data.txt" })).output
return { lines: txt.split("\\n").length }`,
      },
      c,
    )
    const data = r.data as { result: { lines: number } }
    expect(data.result.lines).toBe(2)
    rmSync(home, { recursive: true, force: true })
  })

  test("console 输出分级标注，无输出成功脚本有明确提示", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-log-"))
    const c = ctxWithTools(home)
    const r = await jsTool.execute({ code: `console.log("a"); console.warn("b"); console.error("c")` }, c)
    expect(r.output).toContain("a")
    expect(r.output).toContain("[warn] b")
    expect(r.output).toContain("[error] c")
    const silent = await jsTool.execute({ code: `1 + 1` }, c)
    expect(silent.output).toBe("（脚本执行成功，无输出）")
    rmSync(home, { recursive: true, force: true })
  })

  test("requiresApproval 默认审批、approval:false 免审", () => {
    const ra = jsTool.requiresApproval
    expect(typeof ra === "function" && ra({}, undefined as never)).toBe(true)
    expect(typeof ra === "function" && ra({ approval: false }, undefined as never)).toBe(false)
  })

  test("buildChildScript：用户代码嵌入不受反引号/插值影响；空 code 拒绝", async () => {
    const userCode = "const s = `t${notInterpolated}`"
    const script = buildChildScript(userCode, { ctx: { user: "u" }, input: null })
    expect(script).toContain("var ctx = ")
    expect(script).toContain("const s = `t${notInterpolated}`")
    const home = mkdtempSync(join(tmpdir(), "gebai-js-build-"))
    const c = ctxWithTools(home)
    const r = await jsTool.execute({ code: "" }, c)
    expect(r.output).toContain("code 不能为空")
    rmSync(home, { recursive: true, force: true })
  })

  test("jsRuntimeCommand：脚本调试模式 bun 直跑（无 exec 前缀）", () => {
    const cmd = jsRuntimeCommand("/tmp/x.ts")
    expect(cmd[0]).toBe(process.execPath)
    expect(cmd[1]).toBe("/tmp/x.ts")
  })

  test("必填参数校验：缺参即时拒绝并指明参数名", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-required-"))
    const c = ctxWithTools(home)
    const readLike: Tool = {
      ...mkTool("read_like", async (args) => ({ output: `got ${String(args.path)}` })),
      parameters: { type: "object", properties: { path: { type: "string" } }, required: ["path"] },
    }
    const baseResolve = c.registry.resolve.bind(c.registry)
    const baseSchemas = c.registry.schemas.bind(c.registry)
    c.registry = {
      schemas: () => [...baseSchemas(), { name: "read_like", description: "", parameters: { type: "object", properties: {} } }],
      resolve: (name) => (name === "read_like" ? { name, tool: readLike } : baseResolve(name)),
      getAgentNames: () => [],
    }
    const r = await jsTool.execute(
      {
        code: `try { await read_like({}) } catch (e) { console.log(e.message); return "rejected" }`,
      },
      c,
    )
    expect(r.output).toContain("缺少必填参数: path")
    rmSync(home, { recursive: true, force: true })
  })
})

describe("js 运行时工具定义（defineTool）", () => {
  /** 带 defineDynamicTool 注入的 ctx：注册进 overlay，resolve/schemas 合并可见（模拟引擎会话覆盖层）。 */
  function ctxWithDefine(home: string): { c: ToolContext; defined: Map<string, Tool> } {
    const c = ctxWithTools(home)
    const defined = new Map<string, Tool>()
    const baseSchemas = c.registry.schemas.bind(c.registry)
    const baseResolve = c.registry.resolve.bind(c.registry)
    const dynSchema = (t: Tool) => ({ name: t.name, description: t.description, parameters: { type: "object", properties: {} } as Record<string, unknown> })
    c.registry = {
      schemas: () => [...[...defined.values()].map(dynSchema), ...baseSchemas()],
      resolve: (name) => (defined.has(name) ? { name, tool: defined.get(name)! } : baseResolve(name)),
      getAgentNames: () => [],
    }
    c.defineDynamicTool = async (def) => {
      const tool = makeDynamicTool(def)
      if (defined.has(tool.name) || baseResolve(tool.name)) throw new Error(`工具名已存在: ${tool.name}`)
      defined.set(tool.name, tool)
    }
    return { c, defined }
  }

  test("defineTool 注册后同脚本内即可像内置函数一样调用；模型侧 execute 端到端执行", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-def-"))
    const { c, defined } = ctxWithDefine(home)
    const r = await jsTool.execute(
      {
        code: `await defineTool({
  name: "shout",
  description: "把文本转为大写并加感叹号",
  parameters: { type: "object", properties: { text: { type: "string" } }, required: ["text"] },
  async execute(args) {
    const r = await echo({ msg: args.text })
    return { output: r.output.toUpperCase() + "!" }
  },
})
const r = await shout({ text: "hi" })
console.log(r.output)`,
      },
      c,
    )
    expect(r.output).toContain('ECHO:{"MSG":"HI"}!')
    expect(defined.has("shout")).toBe(true)
    // 审批默认 true（与 sh 同姿态：固化后的每次调用默认需审批）
    expect(defined.get("shout")!.requiresApproval).toBe(true)
    // 模型侧（后续轮次）直接调用：子进程求值 execute 源码，体内可调其他工具
    const tool = defined.get("shout")!
    const r2 = await tool.execute({ text: "ok" }, c)
    expect(r2.output).toBe('ECHO:{"MSG":"OK"}!')
    rmSync(home, { recursive: true, force: true })
  }, 60000)

  test("execute 返回字符串映射为 output；非法名/重名被拒绝", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-def2-"))
    const { c, defined } = ctxWithDefine(home)
    const r = await jsTool.execute(
      {
        code: `const e1 = await defineTool({ name: "BadName", description: "x", async execute() { return "o" } }).catch(e => e.message)
const e2 = await defineTool({ name: "echo", description: "x", async execute() { return "o" } }).catch(e => e.message)
console.log(e1); console.log(e2)
await defineTool({ name: "plain_ret", description: "返回字符串", requiresApproval: false, async execute(args) { return "值:" + JSON.stringify(args) } })
return "done"`,
      },
      c,
    )
    expect(r.output).toContain("BadName 非法")
    expect(r.output).toContain("工具名已存在: echo")
    // 显式 requiresApproval:false 免审；未声明默认需审批
    expect(defined.get("plain_ret")!.requiresApproval).toBe(false)
    const tool = defined.get("plain_ret")!
    const r2 = await tool.execute({ a: 1 }, c)
    expect(r2.output).toBe('值:{"a":1}')
    rmSync(home, { recursive: true, force: true })
  }, 60000)

  test("defineDynamicTool 未注入时返回不可用错误（脚本可捕获继续）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-def3-"))
    const c = ctxWithTools(home)
    const r = await jsTool.execute(
      {
        code: `try { await defineTool({ name: "x1", description: "x", async execute() {} }) } catch (e) { console.log(e.message) }`,
      },
      c,
    )
    expect(r.output).toContain("不支持运行时工具定义")
    rmSync(home, { recursive: true, force: true })
  })

  test("execute 源码超长被拒绝（防巨源码撑爆 chat.json）", () => {
    expect(() =>
      makeDynamicTool({
        name: "big_source",
        description: "x",
        parameters: { type: "object", properties: {} },
        source: `async () => { ${"a".repeat(100_001)} }`,
      }),
    ).toThrow(/源码超长/)
  })

  test("动态工具不能在动态工具内调用（防递归嵌套）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-def4-"))
    const { c, defined } = ctxWithDefine(home)
    await c.defineDynamicTool!({
      name: "outer",
      description: "外层",
      parameters: { type: "object", properties: {} },
      source: `async (args, ctx) => { try { await outer({}) } catch (e) { return { output: e.message } } }`,
    })
    const tool = defined.get("outer")!
    const r = await tool.execute({}, c)
    expect(r.output).toContain("防递归嵌套")
    rmSync(home, { recursive: true, force: true })
  }, 60000)

  test("动态工具执行失败抛工具级错误（引擎/flow 语义）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-def5-"))
    const { c, defined } = ctxWithDefine(home)
    await c.defineDynamicTool!({
      name: "broken",
      description: "必失败",
      parameters: { type: "object", properties: {} },
      source: `async () => { throw new Error("inner fail") }`,
    })
    await expect(defined.get("broken")!.execute({}, c)).rejects.toThrow(/inner fail/)
    rmSync(home, { recursive: true, force: true })
  }, 60000)
})
