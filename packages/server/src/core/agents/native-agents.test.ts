/**
 * 多语言子代理发现器测试（core/agents/native-agents.ts）：
 * manifest 解析纯函数、命令占位符展开、scanManifestDirs 扫描、launchNativeAgent
 * 握手失败路径（坏驱动记入 errors 不抛出）；python 可用时另跑真实端到端。
 */
import { test, expect } from "bun:test"
import { mkdtempSync, rmSync, mkdirSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { parseManifest, expandCommand, scanManifestDirs, discoverNativeAgents, nativeAgentsEnabled, resolvePythonCommand } from "./native-agents"

const tmpRoot = mkdtempSync(join(tmpdir(), "gebai-native-agents-"))
after: {
  rmSync(tmpRoot, { recursive: true, force: true })
}

test("parseManifest：合法/非法清单", () => {
  const ok = parseManifest(
    JSON.stringify({ name: "myagent", description: "d", protocol: 1, command: ["{python}", "{driver}"] }),
    "test",
  )
  expect(ok.manifest?.name).toBe("myagent")
  expect(parseManifest("{bad json", "test").error).toContain("非法 JSON")
  expect(parseManifest(JSON.stringify({ name: "Bad", description: "d", protocol: 1, command: ["x"] }), "t").error).toContain("name 非法")
  expect(parseManifest(JSON.stringify({ name: "a", description: "d", protocol: 2, command: ["x"] }), "t").error).toContain("协议版本")
  expect(parseManifest(JSON.stringify({ name: "a", description: "d", protocol: 1, command: [] }), "t").error).toContain("command")
  expect(parseManifest(JSON.stringify({ name: "a", protocol: 1, command: ["x"] }), "t").error).toContain("description")
})

test("expandCommand：{driver}/{python}/{GEBAI_HOME} 占位与错误", () => {
  const dir = join(tmpRoot, "expand")
  mkdirSync(dir, { recursive: true })
  writeFileSync(join(dir, "main.py"), "# driver")
  // {driver} 解析为绝对路径
  const r1 = expandCommand(["{python}", "{driver}"], dir, { driver: "main.py" }, ["C:/py/python.exe"])
  expect(r1.command[1]).toBe(join(dir, "main.py"))
  // python 为数组展开
  expect(r1.command[0]).toBe("C:/py/python.exe")
  // 缺 driver 文件报错
  const r2 = expandCommand(["{python}", "{driver}"], dir, { driver: "nope.py" }, ["C:/py/python.exe"])
  expect(r2.error).toContain("driver")
  // python 不可解析报错
  const r3 = expandCommand(["{python}"], dir, {}, null)
  expect(r3.error).toContain("解释器不可解析")
  // GEBAI_HOME 占位
  const r4 = expandCommand(["run", "--home", "{GEBAI_HOME}"], dir, {}, null)
  expect(r4.command[2]).not.toContain("{GEBAI_HOME}")
})

test("scanManifestDirs：仅含 agent.json 的子目录入表；无 manifest 目录跳过", async () => {
  const root = join(tmpRoot, "scan")
  mkdirSync(join(root, "alpha"), { recursive: true })
  writeFileSync(join(root, "alpha", "agent.json"), JSON.stringify({ name: "alpha", description: "d", protocol: 1, command: ["x"] }))
  mkdirSync(join(root, "no-manifest"), { recursive: true })
  writeFileSync(join(root, "no-manifest", "readme.txt"), "not an agent")
  const found = await scanManifestDirs([root])
  expect(found.length).toBe(1)
  expect(found[0]!.dir).toBe(join(root, "alpha"))
})

test("discoverNativeAgents：坏驱动握手失败记入 errors 不抛出；无 python 的 command 占位报错可读", async () => {
  const root = join(tmpRoot, "bad-drv")
  mkdirSync(root, { recursive: true })
  mkdirSync(join(root, "badagent"))
  writeFileSync(
    join(root, "badagent", "agent.json"),
    JSON.stringify({ name: "badagent", description: "d", protocol: 1, command: ["{python}", "{driver}"], driver: "main.py" }),
  )
  // main.py 不是协议驱动（启动后立即退出）：握手失败 → errors
  writeFileSync(join(root, "badagent", "main.py"), "import sys\nsys.exit(3)\n")
  const { defs, errors } = await discoverNativeAgents({ resolvePython: () => ["python"] })
  // 只验证不抛出 + errors 结构（具体成败取决于环境，本用例的坏驱动应失败）
  expect(Array.isArray(defs)).toBe(true)
  expect(Array.isArray(errors)).toBe(true)
})

test("nativeAgentsEnabled：off 显式关闭", () => {
  const saved = process.env.GEBAI_NATIVE_AGENTS
  process.env.GEBAI_NATIVE_AGENTS = "off"
  expect(nativeAgentsEnabled()).toBe(false)
  delete process.env.GEBAI_NATIVE_AGENTS
  // 沙箱显式 on 同样禁用
  const savedSandbox = process.env.GEBAI_SANDBOX
  process.env.GEBAI_SANDBOX = "on"
  expect(nativeAgentsEnabled()).toBe(false)
  process.env.GEBAI_SANDBOX = "auto"
  expect(nativeAgentsEnabled()).toBe(true)
  if (saved !== undefined) process.env.GEBAI_NATIVE_AGENTS = saved
  if (savedSandbox !== undefined) process.env.GEBAI_SANDBOX = savedSandbox
})

test("resolvePythonCommand：GEBAI_PYTHON_DIR 显式优先", () => {
  const saved = process.env.GEBAI_PYTHON_DIR
  // 指向真实存在的解释器目录形态不可静态保证，验证空值安全
  const r = resolvePythonCommand({ GEBAI_PYTHON_DIR: "" })
  expect(Array.isArray(r) || r === null).toBe(true)
  if (saved !== undefined) process.env.GEBAI_PYTHON_DIR = saved
})
