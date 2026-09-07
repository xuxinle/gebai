/**
 * 真机端到端验证脚本（bun 运行，非测试）：多语言子代理全链路——
 * 真实 python 驱动、真实 spawn、真实 SubAgentManager/ToolRegistry。
 * 验证：发现注册 python 子代理 → 工具名带前缀 → python_run 常驻状态保持 → 崩溃自愈 → pip status。
 */
import { SubAgentManager } from "../src/core/agents/subagents"
import { ToolRegistry } from "../src/core/base/registry"
import { disposeAllNativeAgents } from "../src/core/agents/native-agents"

const registry = new ToolRegistry()
const m = new SubAgentManager({ registry, preloadOverride: [] })
m.setNativeAgentsOpts({}) // 本地形态默认启用
await m.discover()

// 收尾时回收边车进程（防孤儿 python 残留）
process.on("exit", () => disposeAllNativeAgents())

const py = m.def("python")
if (!py) {
  console.error("FAIL: python 子代理未注册")
  console.error("loadErrors:", m.loadError("python"))
  process.exit(1)
}
console.log("PASS: python 子代理已注册，工具:", Object.keys(py.tools ?? {}))
console.log("提示词前 80 字:", py.systemPrompt.slice(0, 80).replace(/\n/g, " "))

// 装载（agent_load 等价入口）
const loaded = await m.load("python")
console.log("PASS: 装载", loaded)

// 解析注册后的工具（python_run / python_pip / python_status）
const runTool = registry.resolve("python_run")
const pipTool = registry.resolve("python_pip")
if (!runTool || !pipTool) {
  console.error("FAIL: python_run/python_pip 未在注册表", runTool, pipTool)
  process.exit(1)
}
console.log("PASS: 注册表解析 python_run/python_pip OK")

const fakeCtx = {
  user: "admin",
  workdir: process.cwd(),
  sessionWorkdir: process.cwd(),
  home: process.cwd(),
  env: {},
  resolvePath: (p: string) => p,
  readFile: async () => "",
  writeFile: async () => {},
  listFiles: async () => [],
  listDir: async () => [],
  deleteFile: async () => {},
  moveFile: async () => {},
  runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
  uploadAttachment: async (r: { name: string }) => r.name,
  publish: () => {},
  projects: [],
  resolveProjectPath: (n: string) => n,
  getTodos: async () => [],
  setTodos: async () => {},
  registry: { schemas: () => [], resolve: (n: string) => ({ name: n, tool: runTool.tool }), getAgentNames: () => ["python"] },
  listSubAgentDefs: () => [],
  loadSubAgent: async () => {},
} as never

// 1) 常驻状态：两次调用同一 session，变量保持
const r1 = await runTool.tool.execute({ code: "import math\nval = math.pi\nval", session: "e2e" }, fakeCtx)
console.log("run#1:", r1.output)
if (!r1.output.includes("3.14")) {
  console.error("FAIL: 首次执行应回显 math.pi")
  process.exit(1)
}
const r2 = await runTool.tool.execute({ code: "round(val * 2, 4)", session: "e2e" }, fakeCtx)
console.log("run#2（常驻状态跨调用保持）:", r2.output)
if (r2.output.trim() !== "6.2832") {
  console.error("FAIL: 常驻状态丢失（val 未保持）")
  process.exit(1)
}
console.log("PASS: 常驻命名空间状态保持")

// 2) stderr 捕获 + 错误回显
const r3 = await runTool.tool.execute({ code: "import sys\nsys.stderr.write('warn-note\\n')\n1/0", session: "e2e" }, fakeCtx)
console.log("run#3 错误捕获（含 ZeroDivisionError）:", r3.output.includes("ZeroDivisionError") ? "OK" : r3.output)
if (!r3.output.includes("ZeroDivisionError")) {
  console.error("FAIL: 异常栈未回显")
  process.exit(1)
}
console.log("PASS: 异常栈如实回显")

// 3) pip status（真实 subprocess，不动 venv）
const r4 = await pipTool.tool.execute({ action: "status" }, fakeCtx)
console.log("pip status 输出前 3 行:", r4.output.split("\n").slice(0, 3).join(" | "))
if (!r4.output.includes("venv:")) {
  console.error("FAIL: pip status 无 venv 报告")
  process.exit(1)
}
console.log("PASS: python_pip status 报告")

// 4) 崩溃自愈：驱动内 os._exit(1)（真实崩溃）→ 宿主重启重发 → 本次响应丢失但下次调用新进程成功
const r5 = await runTool.tool.execute({ code: "import os\nos._exit(1)", session: "crash" }, fakeCtx).catch((e: Error) => `ERR:${e.message}`)
console.log("run#5 崩溃调用返回:", String(r5).slice(0, 120))
const r6 = await runTool.tool.execute({ code: "'alive-after-crash'", session: "crash2" }, fakeCtx)
if (!r6.output.includes("alive-after-crash")) {
  console.error("FAIL: 崩溃后新进程未恢复:", r6.output)
  process.exit(1)
}
console.log("PASS: 崩溃自愈（真实 os._exit → 重启 → 新进程可用）")

// 5) venv 全链路：pip install（无 venv 自动创建 → 装完驱动退出 → 宿主自愈重启 → 命令工厂切 venv 解释器）
const r7 = await pipTool.tool.execute({ action: "install", packages: "six", timeout: 240 }, fakeCtx)
console.log("pip install six 返回前 2 行:", r7.output.split("\n").slice(0, 2).join(" | "))
if (!/exit 0|Successfully/.test(r7.output)) {
  console.error("WARN: pip install six 未成功（网络受限环境可跳过后续 venv 断言）:", r7.output.slice(0, 400))
} else {
  // 边车已重启：status 上报的解释器应为 venv 内 python
  const statusTool = registry.resolve("python_status")!
  const r8 = await statusTool.tool.execute({}, fakeCtx)
  console.log("重启后 executable:", (r8.data as { executable?: string })?.executable)
  if (!(r8.data as { executable?: string })?.executable?.includes("venv")) {
    console.error("FAIL: 边车重启后未切换到 venv 解释器:", JSON.stringify(r8.data))
    process.exit(1)
  }
  console.log("PASS: venv 自动创建 + 边车重启切换解释器")
  // venv 内 import 验证（six 装在 venv）
  const r9 = await runTool.tool.execute({ code: "import six\nsix.__version__", session: "postvenv" }, fakeCtx)
  console.log("venv 内 import six:", r9.output.slice(0, 80))
  if (/ModuleNotFoundError|ImportError/.test(r9.output)) {
    console.error("FAIL: venv 内 six 不可用")
    process.exit(1)
  }
  console.log("PASS: venv 依赖可用")
}

console.log("\n=== 真机端到端全部通过 ===")
disposeAllNativeAgents() // 显式回收后再退出（exit hook 兄弟保险，防孤儿 python）
process.exit(0)
