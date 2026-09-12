/**
 * Python 边车驱动的项目工具**热重载**（keqing/python/driver.py）：
 * 工具模块此前只在进程启动时加载一次——改完源码后旧代码持续生效，调试本地能力（vision/docqa）时
 * 极易误判「修改无效」。现在每次 tools.list / tool.call 前按 tools.py 的 mtime 检测变更并重载。
 *
 * 用例直接起驱动进程走 JSON-RPC（真实链路）；无可用 python 时跳过（不把环境缺失当失败）。
 */
import { afterAll, expect, test } from "bun:test"
import { spawn } from "node:child_process"
import { existsSync, mkdirSync, mkdtempSync, rmSync, utimesSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { resolvePythonCommand } from "./keqing"

const root = join(import.meta.dir, "../../../../..")
const DRIVER = join(root, "keqing/python/driver.py")
const pythonCmd = resolvePythonCommand(process.env as Record<string, string>)
const available = !!pythonCmd && pythonCmd.length > 0 && existsSync(DRIVER)
const tmpRoots: string[] = []

afterAll(() => {
  for (const d of tmpRoots) rmSync(d, { recursive: true, force: true })
})

/** 源文件内容：带上版本号，便于断言「重载后确实是新代码」。 */
const toolsSource = (version: number, withExtra: boolean) =>
  [
    `AGENT_NAME = "probe_agent"`,
    `def hello(args):`,
    `    return {"output": "version ${version}"}`,
    ...(withExtra ? [`def extra(args):`, `    return {"output": "extra-ok"}`] : []),
    `TOOLS = [{"name": "hello", "description": "v${version}", "parameters": {"type": "object", "properties": {}}}` +
      (withExtra ? `, {"name": "extra", "description": "e", "parameters": {"type": "object", "properties": {}}}]` : `]`),
    `TOOL_IMPLS = {` + (withExtra ? `"hello": hello, "extra": extra}` : `"hello": hello}`),
    ``,
  ].join("\n")

/** 起驱动进程 + 行式 JSON-RPC 客户端。 */
function startDriver(agentDir: string, home: string) {
  const [cmd, ...pre] = pythonCmd!
  const proc = spawn(cmd!, [...pre, DRIVER], {
    env: { ...process.env, GEBAI_AGENT_DIR: agentDir, GEBAI_HOME: home },
    stdio: ["pipe", "pipe", "pipe"],
  })
  let buf = ""
  const pending: Array<(v: Record<string, unknown>) => void> = []
  proc.stdout.setEncoding("utf8")
  proc.stdout.on("data", (chunk: string) => {
    buf += chunk
    let i: number
    while ((i = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, i)
      buf = buf.slice(i + 1)
      if (!line.trim()) continue
      const fn = pending.shift()
      try {
        fn?.(JSON.parse(line) as Record<string, unknown>)
      } catch {
        fn?.({ ok: false, error: "响应解析失败" })
      }
    }
  })
  let id = 0
  const call = (req: Record<string, unknown>): Promise<Record<string, unknown>> =>
    new Promise((resolve) => {
      pending.push(resolve)
      proc.stdin.write(`${JSON.stringify({ id: ++id, ...req })}\n`)
    })
  const result = async (req: Record<string, unknown>) => {
    const r = await call(req)
    return (r.ok ? r.result : undefined) as never
  }
  return { proc, call, result }
}

/** 改文件并把 mtime 拨到未来（不依赖文件系统时间戳精度，避免 sleep）。 */
function rewriteTools(path: string, content: string) {
  writeFileSync(path, content, "utf8")
  const future = new Date(Date.now() + 3000)
  utimesSync(path, future, future)
}

test.skipIf(!available)("驱动按 tools.py 的 mtime 热重载项目工具（改源码无需重启进程）", async () => {
  const home = mkdtempSync(join(tmpdir(), "gebai-kq-reload-"))
  tmpRoots.push(home)
  const agentDir = join(home, "probe_agent")
  mkdirSync(agentDir, { recursive: true })
  const toolsPath = join(agentDir, "tools.py")
  writeFileSync(toolsPath, toolsSource(1, false), "utf8")

  const { proc, result, call } = startDriver(agentDir, home)
  const names = async () => (await result({ op: "tools.list", args: {} }) as unknown as Array<{ name: string }>).map((t) => t.name).sort()
  const hello = async () => ((await result({ op: "tool.call", tool: "hello", args: {} })) as { output?: string }).output

  try {
    // ① 初始：项目工具与基础工具合并
    expect(await names()).toEqual(["hello", "pip", "run", "status"])
    expect(await hello()).toBe("version 1")

    // ② 改源码（内容变 + 新增工具）：不重启进程，下一次调用即生效
    rewriteTools(toolsPath, toolsSource(2, true))
    expect(await names()).toEqual(["extra", "hello", "pip", "run", "status"])
    expect(await hello()).toBe("version 2")
    expect(((await result({ op: "tool.call", tool: "extra", args: {} })) as { output?: string }).output).toBe("extra-ok")

    // ③ 再改（删掉 extra）：重载从基础集重建，旧工具不残留
    rewriteTools(toolsPath, toolsSource(3, false))
    expect(await names()).toEqual(["hello", "pip", "run", "status"])
    expect(await hello()).toBe("version 3")

    // ④ 删除 tools.py：回退基础工具集（旧工具不可再调用）
    rmSync(toolsPath)
    utimesSync(agentDir, new Date(Date.now() + 6000), new Date(Date.now() + 6000))
    expect(await names()).toEqual(["pip", "run", "status"])
    const gone = await call({ op: "tool.call", tool: "hello", args: {} })
    expect(gone.ok).toBe(false) // 项目工具已不在（协议层返回未知工具）

    // ⑤ 写入语法错误的 tools.py：失败安全（回退基础集，基础工具仍可用，进程不退出）
    rewriteTools(toolsPath, "def broken(:\n")
    expect(await names()).toEqual(["pip", "run", "status"])
    const base = (await result({ op: "tool.call", tool: "run", args: { code: "print(1+1)" } })) as { output?: string }
    expect(base.output).toContain("2")
    expect(proc.exitCode).toBeNull()
  } finally {
    proc.stdin.end()
    proc.kill()
  }
})
