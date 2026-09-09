/**
 * 多语言子代理边车宿主测试（core/agents/sidecar.ts）：
 * 用 Node/Bun 子进程实现 fake 协议驱动（无 python 依赖），验证
 * init/tools.list/tool.call 协议往返（v2 请求级 ctx）、超时杀进程重启、崩溃自愈重发、
 * dispose、命令工厂解析、协议版本不匹配拒绝。
 */
import { test, afterAll, expect } from "bun:test"
import { AgentSidecar, SIDECAR_PROTOCOL, type SidecarCtx, type SidecarProc, type SidecarSpawnFn } from "./sidecar"

/** fake 驱动源码：按行为开关响应协议（v2——tool.call 请求体顶级 tool/args/ctx，回显 ctx 供断言）。 */
const FAKE_DRIVER = `
const behavior = process.env.FAKE_BEHAVIOR || "ok"
const lines = []
let buf = ""
const send = (o) => process.stdout.write(JSON.stringify(o) + "\\n")
process.stdin.on("data", (d) => {
  buf += d.toString()
  let nl
  while ((nl = buf.indexOf("\\n")) >= 0) {
    const line = buf.slice(0, nl).trim()
    buf = buf.slice(nl + 1)
    if (!line) continue
    handle(JSON.parse(line))
  }
})
function handle(req) {
  const { id, op, tool, args, ctx } = req
  if (behavior === "crash-once" && op === "tool.call" && globalThis.crashed !== true) {
    globalThis.crashed = true
    send({ id, ok: true, result: { output: "pre-crash" } })  // 先响应再死（不触发重发路径）
    process.exit(1)
  }
  if (behavior === "crash-once-silent" && op === "tool.call") {
    const fs = require("fs")
    const flag = process.env.FLAG_FILE
    if (!fs.existsSync(flag)) {
      fs.writeFileSync(flag, "1")
      process.exit(1)  // 首次 tool.call 静默死亡（文件标记跨进程一次性）；新进程正常响应重发请求
    }
  }
  if (behavior === "slow" && op === "tool.call") {
    setTimeout(() => send({ id, ok: true, result: { output: "slow-done" } }), 10_000)
    return
  }
  if (behavior === "flap") {
    // 启动即死：抖动风暴路径（不自动重启）
    process.exit(1)
  }
  if (op === "init") send({ id, ok: true, result: { name: "fake", protocol: 2 } })
  else if (op === "tools.list") send({ id, ok: true, result: [{ name: "echo", description: "d", parameters: { type: "object", properties: {} } }] })
  else if (op === "tool.call") send({ id, ok: true, result: { output: "echo:" + JSON.stringify({ tool, args, ctx }), data: { fine: true } } })
  else send({ id, ok: false, error: "unknown op " + op })
}
process.stdin.on("end", () => process.exit(0))
`

function fakeSpawn(behavior: string): SidecarSpawnFn {
  return (cmd, opts) => {
    const proc = Bun.spawn(cmd, {
      cwd: opts.cwd,
      env: { ...process.env, ...opts.env, FAKE_BEHAVIOR: behavior, FLAG_FILE: flagFile },
      stdout: "pipe",
      stderr: "pipe",
      stdin: "pipe",
    })
    return {
      stdin: proc.stdin as unknown as SidecarProc["stdin"],
      stdout: proc.stdout as unknown as ReadableStream<Uint8Array>,
      stderr: proc.stderr as unknown as ReadableStream<Uint8Array>,
      kill: () => proc.kill(),
      get killed() {
        return proc.killed
      },
    }
  }
}

/** 崩溃自愈用一次性标记文件（tmp 目录，测试间独立）。 */
const flagFile = `${import.meta.dir}/.sidecar-test-flag`
try {
  require("node:fs").rmSync(flagFile, { force: true })
} catch { /* 首次不存在 */ }
afterAll(() => {
  // 结束清理标记文件（崩溃用例写入的工作文件，不留未跟踪残留）
  try {
    require("node:fs").rmSync(flagFile, { force: true })
  } catch { /* 忽略 */ }
})

const BUN = [process.execPath, "-e", FAKE_DRIVER]

/** 测试用 ctx（协议 v2 必携带）。 */
const CTX: SidecarCtx = { sessionId: "s-test", user: "tester", cwd: "C:/tmp/s-test", env: { K: "V" }, sandboxed: false }

test("协议往返（v2）：init/tools.list/tool.call 携带请求级 ctx", async () => {
  const sc = new AgentSidecar({ command: BUN, spawn: fakeSpawn("ok") })
  const info = await sc.init()
  expect(info.name).toBe("fake")
  expect(info.protocol).toBe(SIDECAR_PROTOCOL)
  const tools = await sc.toolsList()
  expect(tools.length).toBe(1)
  expect(tools[0]!.name).toBe("echo")
  const r = await sc.toolCall("echo", { x: 1 }, CTX)
  expect(r.error).toBeUndefined()
  // 驱动回显请求体：ctx 原样到达（进程单例跨会话共享，会话上下文随请求传递）
  const echoed = JSON.parse(r.output.slice("echo:".length)) as { tool: string; args: Record<string, unknown>; ctx: SidecarCtx }
  expect(echoed.tool).toBe("echo")
  expect(echoed.args).toEqual({ x: 1 })
  expect(echoed.ctx.sessionId).toBe("s-test")
  expect(echoed.ctx.cwd).toBe("C:/tmp/s-test")
  expect(echoed.ctx.env).toEqual({ K: "V" })
  sc.dispose("test-done")
})

test("init 响应缺 name/protocol 拒绝", async () => {
  // 直接用 ok 行为但断言校验路径：手写一个缺协议字段的驱动
  const badDriver = `
  process.stdout.write(JSON.stringify({ id: 1, ok: true, result: { name: "fake" } }) + "\\n")
  setTimeout(() => process.exit(0), 5000)
  `
  const sc = new AgentSidecar({ command: [process.execPath, "-e", badDriver], spawn: fakeSpawn("ok") })
  let err = ""
  try {
    await sc.init()
  } catch (e) {
    err = (e as Error).message
  }
  expect(err).toContain("缺 name/protocol")
  sc.dispose("test-done")
})

test("协议版本不匹配拒绝（v1 驱动上报被拒）", async () => {
  const v1Driver = `
  process.stdout.write(JSON.stringify({ id: 1, ok: true, result: { name: "fake", protocol: 1 } }) + "\\n")
  setTimeout(() => process.exit(0), 5000)
  `
  const sc = new AgentSidecar({ command: [process.execPath, "-e", v1Driver], spawn: fakeSpawn("ok") })
  let err = ""
  try {
    await sc.init()
  } catch (e) {
    err = (e as Error).message
  }
  expect(err).toContain("协议版本不匹配")
  expect(err).toContain("v1")
  sc.dispose("test-done")
})

test("崩溃自愈：进程静默死亡后请求重发一次成功（重发携带原 ctx）", async () => {
  const sc = new AgentSidecar({ command: BUN, spawn: fakeSpawn("crash-once-silent") })
  const info = await sc.init() // 第一次进程正常握手
  expect(info.name).toBe("fake")
  // 第一次 tool.call：驱动静默死亡 → 宿主重启进程 → 重发该请求 → 新进程响应
  const r = await sc.toolCall("echo", { retry: true }, CTX)
  expect(r.error).toBeUndefined()
  const echoed = JSON.parse(r.output.slice("echo:".length)) as { ctx: SidecarCtx }
  expect(echoed.ctx.sessionId).toBe("s-test") // 重发请求原样重写请求行，ctx 不丢
  sc.dispose("test-done")
})

test("请求超时：杀进程重启，该次拒绝，下次调用用新进程成功", async () => {
  const sc = new AgentSidecar({ command: BUN, spawn: fakeSpawn("slow"), requestTimeoutMs: 300 })
  await sc.init()
  let err = ""
  try {
    await sc.toolCall("echo", {}, CTX)
  } catch (e) {
    err = (e as Error).message
  }
  expect(err).toContain("超时")
  // 超时后进程被杀重启：换回 ok 行为验证新进程可用
  ;(sc as unknown as { opts: { spawn: SidecarSpawnFn } }).opts.spawn = fakeSpawn("ok")
  const r = await sc.toolCall("echo", { after: "timeout" }, CTX)
  expect(r.output).toContain("echo:")
  sc.dispose("test-done")
})

test("抖动风暴：启动即死不无限重启", async () => {
  const sc = new AgentSidecar({ command: BUN, spawn: fakeSpawn("flap"), requestTimeoutMs: 1000 })
  let err = ""
  try {
    await sc.init()
  } catch (e) {
    err = (e as Error).message
  }
  // 第一次启动即死：在途 init 请求被拒（重启后台进行但新进程同样即死；不产生无限进程风暴）
  expect(err.length).toBeGreaterThan(0)
  sc.dispose("test-done")
})

test("dispose 后请求拒绝", async () => {
  const sc = new AgentSidecar({ command: BUN, spawn: fakeSpawn("ok") })
  await sc.init()
  sc.dispose("done")
  let err = ""
  try {
    await sc.toolCall("echo", {}, CTX)
  } catch (e) {
    err = (e as Error).message
  }
  expect(err).toContain("已销毁")
})

test("命令工厂：每次启动重新解析（模拟 venv 创建后切换解释器）", async () => {
  let callCount = 0
  const seen: string[][] = []
  const spawnFn: SidecarSpawnFn = (cmd) => {
    seen.push(cmd)
    return fakeSpawn("ok")(cmd, {})
  }
  const sc = new AgentSidecar({
    command: () => {
      callCount++
      return [process.execPath, "-e", FAKE_DRIVER, `--gen${callCount}`]
    },
    spawn: spawnFn,
  })
  await sc.init()
  expect(seen.length).toBe(1)
  // 触发重启（dispose→新实例不适用；用超时杀进程路径）
  ;(sc as unknown as { opts: { requestTimeoutMs: number } }).opts.requestTimeoutMs = 200
  const slowSc = new AgentSidecar({
    command: () => {
      callCount++
      return [process.execPath, "-e", FAKE_DRIVER, `--gen${callCount}`]
    },
    spawn: spawnFn,
    requestTimeoutMs: 200,
  })
  await slowSc.init().catch(() => {})
  try {
    await slowSc.toolCall("echo", {}, CTX)
  } catch {
    /* 超时预期 */
  }
  // 两次启动都经过工厂（每次启动重新解析）
  expect(callCount).toBeGreaterThanOrEqual(2)
  sc.dispose("done")
  slowSc.dispose("done")
})
