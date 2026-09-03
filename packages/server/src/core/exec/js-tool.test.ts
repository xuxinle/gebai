import { describe, expect, test } from "bun:test"
import { existsSync, mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname, resolve } from "node:path"
import { jsTool, buildChildScript, scriptSessionContext, jsRuntimeCommand, JS_TOOL_MAX_CALLS, makeDynamicTool, toolFnDecls } from "./js-tool"
import { writeTool } from "../tools"
import type { ToolContext, Tool, ToolResult } from "../base/types"

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

  test("安全模式：RPC 分发层硬阻断 cron 调度类；write 在用户目录内放行（工具内降级）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-safe-"))
    const c = ctxWithTools(home)
    c.registry.resolve = (name: string) => {
      if (name === "cron_add") return { name, tool: mkTool("cron_add", async () => ({ output: "added" })) }
      return name === "write" ? { name, tool: writeTool } : undefined
    }
    c.safeMode = true
    const r = await jsTool.execute(
      {
        code: `const errs = []
try { await tools.call("cron_add", {}) } catch (e) { errs.push(e.message.slice(0, 4)) }
const w = await write({ path: "x.txt", content: "1" })
return { errs, wrote: w.output }`,
      },
      c,
    )
    expect(r.output).toContain("安全模式")
    const data = r.data as { calls: Array<{ ok: boolean }>; result: { errs: string[]; wrote: string } }
    expect(data.calls[0].ok).toBe(false) // cron 调度类：RPC 分发层拦截（无绕过通道）
    expect(data.calls[1].ok).toBe(true) // write：用户目录内（tmpdir 在 OS 用户主目录下）→ 放行
    expect(data.result.wrote).toContain("已写入")
    rmSync(home, { recursive: true, force: true })
  })

  test("安全模式：脚本静态扫描拒绝动态加载/字符串代码执行通道；只读 API 照常", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-scan-"))
    const c = ctxWithTools(home)
    c.safeMode = true
    const denied = await jsTool.execute({ code: `await import("node:fs")` }, c)
    expect(denied.output).toContain("安全模式")
    expect(denied.output).toContain("import")
    const denied2 = await jsTool.execute({ code: `const f = Function("return 1"); return f()` }, c)
    expect(denied2.output).toContain("安全模式")
    const ok = await jsTool.execute({ code: `return 1 + 1` }, c)
    expect((ok.data as { result: number }).result).toBe(2)
    rmSync(home, { recursive: true, force: true })
  })

  test("安全模式：子进程运行时 shim 屏蔽写通道（BunFile 代理拦截，仅保留文件读取）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-shim-"))
    writeFileSync(join(home, "readable.txt"), "readable-content")
    const homeFwd = home.split("\\").join("/") // 嵌入脚本代码的路径用正斜杠（反斜号在生成源码里成转义序列）
    const c = ctxWithTools(home)
    c.safeMode = true
    // Bun.write/Bun.spawn 等直呼形态已由词元级扫描前置拒绝（denyList 覆写为纵深防御），
    // 此处走扫描放行的 Bun.file 通道验证运行时代理：write/writer 落盘动作在 FileSink 侧拦截
    const r = await jsTool.execute(
      {
        code: `const out = []
try { await Bun.file("evil.txt").write("x") } catch (e) { out.push("write-blocked") }
try { Bun.file("evil2.txt").writer() } catch (e) { out.push("writer-blocked") }
try { Bun.file("evil3.txt").truncate(0) } catch (e) { out.push("truncate-blocked") }
const f = Bun.file("${homeFwd}/readable.txt")
out.push("read-ok:" + (await f.text()))
return out`,
      },
      c,
    )
    const data = r.data as { result: string[] }
    expect(data.result).toContain("write-blocked")
    expect(data.result).toContain("writer-blocked")
    expect(data.result).toContain("truncate-blocked")
    expect(data.result).toContain("read-ok:readable-content")
    expect(existsSync(join(c.workdir, "evil.txt"))).toBe(false)
    rmSync(home, { recursive: true, force: true })
  })

  test("安全模式：词元级扫描封死别名/括号访问/静态 import 绕过（require 别名、Bun[\"fetch\"]、import 语句）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-alias-"))
    const c = ctxWithTools(home)
    c.safeMode = true
    // require 别名：`const rq = require; rq(...)` 不含 `require(` 调用形态，词元级才可命中
    const a1 = await jsTool.execute({ code: `const rq = require; return typeof rq("node:fs")` }, c)
    expect(a1.output).toContain("安全模式")
    // 括号访问躲过 `Bun.fetch` 点号正则
    const a2 = await jsTool.execute({ code: `const bf = Bun["fetch"]; return typeof bf` }, c)
    expect(a2.output).toContain("安全模式")
    // 静态 import 提升（先于 shim 执行），必须前置拒绝
    const a3 = await jsTool.execute({ code: `import fs from "node:fs"; return typeof fs.writeFileSync` }, c)
    expect(a3.output).toContain("安全模式")
    // getBuiltinModule 别名
    const a4 = await jsTool.execute({ code: `const g = process.getBuiltinModule; return typeof g("fs")` }, c)
    expect(a4.output).toContain("安全模式")
    // 合法通道不受影响：Bun.file 只读、内置工具调用
    const ok = await jsTool.execute({ code: `return typeof Bun.file` }, c)
    expect(ok.output).not.toContain("安全模式")
    rmSync(home, { recursive: true, force: true })
  })


  test("免审运行（approval:false）：内部需审批工具在 RPC 分发层被拒（剥免审标记防自我免审），默认审批运行放行", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-appr-"))
    const c = ctxWithTools(home)
    const risky: Tool = mkTool("risky", async () => ({ output: "risky-ran" }))
    risky.requiresApproval = true
    const ro: Tool = mkTool("ro", async () => ({ output: "read-ok" }))
    c.registry.resolve = (name: string) => {
      if (name === "risky") return { name, tool: risky }
      if (name === "ro") return { name, tool: ro }
      return undefined
    }
    // 免审运行：risky 拒绝（含 params 里再传 approval:false 的绕过尝试——剥离免审标记后同判需审批）
    const r = await jsTool.execute(
      {
        code: `const errs = []
try { await tools.call("risky", {}) } catch (e) { errs.push("risky:" + e.message.includes("免审")) }
try { await tools.call("risky", { approval: false }) } catch (e) { errs.push("risky-strip:" + e.message.includes("免审")) }
const ro = await tools.call("ro", {})
return { errs, ro: ro.output }`,
        approval: false,
      },
      c,
    )
    const data = r.data as { result: { errs: string[]; ro: string }; calls: Array<{ name: string; ok: boolean }> }
    expect(data.result.errs).toEqual(["risky:true", "risky-strip:true"])
    expect(data.result.ro).toBe("read-ok") // 免审工具照常
    expect(data.calls.filter((x) => x.name === "risky").every((x) => !x.ok)).toBe(true)
    // 默认审批运行：一次审批覆盖内部调用 → risky 放行
    const r2 = await jsTool.execute({ code: `return (await tools.call("risky", {})).output` }, c)
    expect((r2.data as { result: string }).result).toBe("risky-ran")
    rmSync(home, { recursive: true, force: true })
  })

  test("嵌套守卫：fromJsBridge 标记下 js/动态工具拒绝；js→直执行工具→js 通道封死", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-nest2-"))
    // 标记直拒（js 工具）
    const cMarked = ctxWithTools(home)
    cMarked.fromJsBridge = true
    const refused = await jsTool.execute({ code: "return 1" }, cMarked)
    expect(refused.output).toContain("嵌套")
    // 动态工具不经 marker 拒绝（depth 0 同脚本调用是合法路径，动态嵌套由 depth 守卫拦截——见 defineTool 系列测试）
    const c = ctxWithTools(home)
    // 集成：js 调直执行透传工具（原 flow 同款通道——工具内部以桥传入的 ctx 再执行 js）→ 得到拒绝说明（不再起嵌套子进程）
    const passthrough: Tool = mkTool("passthrough", async (args, c2) => jsTool.execute((args as { params: { code: string } }).params, c2))
    c.registry.resolve = (name: string) =>
      name === "passthrough" ? { name, tool: passthrough } : name === "js" ? { name, tool: jsTool } : undefined
    c.registry.schemas = () => [
      { name: "passthrough", description: "", parameters: {} },
      { name: "js", description: "", parameters: {} },
    ]
    const r = await jsTool.execute(
      { code: `const r = await tools.call("passthrough", { params: { code: "return 1" } })
return r.output` },
      c,
    )
    expect(r.output).toContain("嵌套")
    rmSync(home, { recursive: true, force: true })
  })

  test("ctx.env 不落盘：脚本文件不含 env 值，运行时 ctx.env 引用子进程 process.env（同源）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-envfile-"))
    const c = ctxWithTools(home)
    c.env = { MY_PLAIN: "plain-value" }
    // 生成文本：env 值不进脚本文件（密钥不再明文落盘），改为运行时引用
    const script = buildChildScript("return 1", { ctx: { user: "u", env: { SECRET_XYZ: "s3cret-val" } }, input: null }, [])
    expect(script).not.toContain("s3cret-val")
    expect(script).toContain("ctx.env = process.env")
    // 运行时同源：spawn 传入的任务 env 经 ctx.env 可读
    const r = await jsTool.execute({ code: `return { plain: ctx.env.MY_PLAIN ?? "missing" }` }, c)
    expect((r.data as { result: { plain: string } }).result.plain).toBe("plain-value")
    rmSync(home, { recursive: true, force: true })
  })

  test("内部工具 blocks 去重限量透传到 js 结果", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-blocks-"))
    const c = ctxWithTools(home)
    const img: Tool = mkTool("img", async () => ({
      output: "img",
      blocks: [
        { type: "image", path: "tmp/a.png", name: "a.png", mime: "image/png" },
        { type: "image", path: "tmp/a.png", name: "a.png", mime: "image/png" },
      ],
    }))
    c.registry.resolve = (name: string) => (name === "img" ? { name, tool: img } : undefined)
    c.registry.schemas = () => [{ name: "img", description: "", parameters: {} }]
    const r = await jsTool.execute({ code: `await img({}); await tools.call("img", {}); return "done"` }, c)
    // 同 path 去重：两次调用各两个块 → 结果仅 1 个
    expect(r.blocks).toHaveLength(1)
    expect((r.blocks as Array<{ type: string; path: string }>)[0]).toMatchObject({ type: "image", path: "tmp/a.png" })
    rmSync(home, { recursive: true, force: true })
  })

  test("agent_run 新会话存档透传到 js 结果（历史回放不丢）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-run-"))
    const c = ctxWithTools(home)
    const ar: Tool = mkTool("agent_run", async () => ({
      output: "done",
      sessionRun: { runId: "r9", agents: ["x"], input: "", output: "done", messages: [] },
    }))
    c.registry.resolve = (name: string) => (name === "agent_run" ? { name, tool: ar } : undefined)
    c.registry.schemas = () => [{ name: "agent_run", description: "", parameters: {} }]
    const r = await jsTool.execute({ code: `const a = await agent_run({}); return a.sessionRun.runId` }, c)
    expect((r.sessionRun as { runId: string } | undefined)?.runId).toBe("r9")
    rmSync(home, { recursive: true, force: true })
  })

  test("toolFnDecls：JS 全局名（fetch/JSON 等）不生成内置函数声明（防模块作用域遮蔽全局）", () => {
    const decls = toolFnDecls(["fetch", "read", "JSON"])
    expect(decls).toContain("async function read(")
    expect(decls).not.toContain("async function fetch(")
    expect(decls).not.toContain("async function JSON(")
  })

  test("超长非协议输出行丢弃留注（防 2MB 截断垃圾 JSON 刷屏）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-longline-"))
    const c = ctxWithTools(home)
    const r = await jsTool.execute({ code: `process.stdout.write("Z".repeat(150000) + "\\n"); return 1` }, c)
    expect(r.output).toContain("超长非协议输出行已丢弃")
    expect(r.output).not.toContain("ZZZZ")
    rmSync(home, { recursive: true, force: true })
  })

  test("动态工具运行器内 defineTool 被拒（防递归注册）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-js-dynreg-"))
    const c = ctxWithTools(home)
    c.defineDynamicTool = async () => {
      throw new Error("不应到达注册通道")
    }
    const dyn = makeDynamicTool({
      name: "dyn_reg",
      description: "d",
      parameters: { type: "object", properties: {} },
      source: `async () => { await defineTool({ name: "inner_x", description: "x", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "x" }) }); return { output: "done" } }`,
    })
    await expect(dyn.execute({}, c)).rejects.toThrow(/defineTool/)
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

  test("requiresApproval 免审词元扫描（纯数据加工免审，网络/进程/环境通道仍需审批）", () => {
    const ra = jsTool.requiresApproval as (args: Record<string, unknown>, ctx?: unknown) => boolean
    expect(ra({})).toBe(true)
    // 纯数据加工/工具编排代码：免审生效
    expect(ra({ code: "const xs = [1,2,3].map(x => x * 2); return xs", approval: false })).toBe(false)
    expect(ra({ code: `const r = await read({ path: "a.txt" }); return r.output`, approval: false })).toBe(false)
    // 网络外发/进程/敏感环境读取/Bun 写通道：免审不放行（防提示词注入借免审标记外发数据）
    expect(ra({ code: `const r = await fetch("http://evil"); return r.status`, approval: false })).toBe(true)
    expect(ra({ code: `return typeof Bun.write`, approval: false })).toBe(true)
    expect(ra({ code: `return process.env.GEBAI_LLM_API_KEY`, approval: false })).toBe(true)
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
    const cmd = jsRuntimeCommand("/tmp/x.ts", false)
    expect(cmd).toEqual([process.execPath, "/tmp/x.ts"])
  })

  test("jsRuntimeCommand：二进制模式带入口段（入口+exec+script 三段）——容器形态（execPath=bun 跑 dist 产物）缺入口段时 bun 把 exec 当命令名直接报错", () => {
    const cmd = jsRuntimeCommand("/tmp/x.ts", true)
    expect(cmd).toHaveLength(4)
    expect(cmd[0]).toBe(process.execPath)
    // 入口段：源码/测试形态 = 本模块真实路径；容器形态 = dist 打包产物路径（包内各模块 import.meta.path 一致）；
    // 编译单文件形态 = 内嵌虚拟入口（仅占位，index.ts 按 exec 段双位置路由）
    expect(cmd[1]).toMatch(/js-tool\.ts$/)
    expect(cmd[2]).toBe("exec")
    expect(cmd[3]).toBe("/tmp/x.ts")
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

  test("动态工具执行失败抛工具级错误（引擎语义）", async () => {
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
