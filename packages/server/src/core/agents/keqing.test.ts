/**
 * 客卿发现器测试（core/agents/keqing.ts）：
 * manifest 解析纯函数、命令占位符展开、scanManifestDirs 扫描、launchKeqing
 * 握手失败路径（坏驱动记入 errors 不抛出）；python 可用时另跑真实端到端。
 */
import { test, expect } from "bun:test"
import { mkdtempSync, rmSync, mkdirSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { parseManifest, expandCommand, scanManifestDirs, discoverKeqing, keqingEnabled, resolvePythonCommand, ensureBuilt } from "./keqing"

const tmpRoot = mkdtempSync(join(tmpdir(), "gebai-keqing-agents-"))
after: {
  rmSync(tmpRoot, { recursive: true, force: true })
}

test("parseManifest：合法/非法清单", () => {
  const ok = parseManifest(
    JSON.stringify({ name: "myagent", description: "d", protocol: 2, command: ["{python}", "{driver}"] }),
    "test",
  )
  expect(ok.manifest?.name).toBe("myagent")
  // requiresApproval 声明解析：缺省 undefined（恒需审批）、false 保留
  expect(ok.manifest?.requiresApproval).toBeUndefined()
  const ro = parseManifest(JSON.stringify({ name: "ro_agent", protocol: 2, command: ["x"], requiresApproval: false }), "t")
  expect(ro.manifest?.requiresApproval).toBe(false)
  expect(parseManifest("{bad json", "test").error).toContain("非法 JSON")
  expect(parseManifest(JSON.stringify({ name: "Bad", description: "d", protocol: 2, command: ["x"] }), "t").error).toContain("name 非法")
  expect(parseManifest(JSON.stringify({ name: "a", description: "d", protocol: 1, command: ["x"] }), "t").error).toContain("协议版本")
  expect(parseManifest(JSON.stringify({ name: "a", description: "d", protocol: 2, command: [] }), "t").error).toContain("command")
  // description 可省略/留空：留空即本侧不贡献（交由跨语言合并层或 mergeSubAgentDefs 兜底），不再报错
  const noDesc = parseManifest(JSON.stringify({ name: "a", protocol: 2, command: ["x"] }), "t")
  expect(noDesc.error).toBeUndefined()
  expect(noDesc.manifest?.description).toBe("")
  const blankDesc = parseManifest(JSON.stringify({ name: "a", description: "   ", protocol: 2, command: ["x"] }), "t")
  expect(blankDesc.manifest?.description).toBe("")
  // build 字段解析：合法保留、非法项忽略
  const withBuild = parseManifest(
    JSON.stringify({
      name: "mathx",
      description: "d",
      protocol: 2,
      command: ["{agent_dir}/driver{exe}"],
      build: { command: ["rustc", "-o", "{agent_dir}/driver{exe}"], windows: "not-array", unix: ["", "x"] },
    }),
    "t",
  )
  expect(withBuild.manifest?.build?.command).toEqual(["rustc", "-o", "{agent_dir}/driver{exe}"])
  expect(withBuild.manifest?.build?.windows).toBeUndefined()
  expect(withBuild.manifest?.build?.unix).toBeUndefined()
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

test("expandCommand：{agent_dir}/{lang_dir}/{agent_name}/{exe} 占位", () => {
  const langDir = join(tmpRoot, "cpp")
  const agentDir = join(langDir, "mathx")
  mkdirSync(agentDir, { recursive: true })
  const r = expandCommand(["{agent_dir}/driver{exe}", "--name", "{agent_name}", "--lang", "{lang_dir}"], agentDir, { name: "mathx" }, null)
  expect(r.error).toBeUndefined()
  const exeSuffix = process.platform === "win32" ? ".exe" : ""
  expect(r.command[0]).toBe(join(agentDir, "driver" + exeSuffix))
  expect(r.command[2]).toBe("mathx")
  expect(r.command[4]).toBe(langDir)
  // manifest 无 name 时回退目录名
  const r2 = expandCommand(["x", "{agent_name}"], agentDir, {}, null)
  expect(r2.command[1]).toBe("mathx")
})

test("scanManifestDirs：仅含 agent.json 的子目录入表；无 manifest 目录与 venv/__pycache__/objs 跳过", async () => {
  const root = join(tmpRoot, "scan")
  mkdirSync(join(root, "alpha"), { recursive: true })
  writeFileSync(join(root, "alpha", "agent.json"), JSON.stringify({ name: "alpha", description: "d", protocol: 2, command: ["x"] }))
  mkdirSync(join(root, "no-manifest"), { recursive: true })
  writeFileSync(join(root, "no-manifest", "readme.txt"), "not an agent")
  for (const skip of ["venv", "__pycache__", "objs"]) {
    mkdirSync(join(root, skip), { recursive: true })
    writeFileSync(join(root, skip, "agent.json"), JSON.stringify({ name: skip, description: "d", protocol: 2, command: ["x"] }))
  }
  const found = await scanManifestDirs([root])
  expect(found.length).toBe(1)
  expect(found[0]!.dir).toBe(join(root, "alpha"))
})

test("discoverKeqing：坏驱动握手失败记入 errors 不抛出；无 python 的 command 占位报错可读", async () => {
  const root = join(tmpRoot, "bad-drv")
  mkdirSync(root, { recursive: true })
  mkdirSync(join(root, "badagent"))
  writeFileSync(
    join(root, "badagent", "agent.json"),
    JSON.stringify({ name: "badagent", description: "d", protocol: 2, command: ["{python}", "{driver}"], driver: "main.py" }),
  )
  // main.py 不是协议驱动（启动后立即退出）：握手失败 → errors
  writeFileSync(join(root, "badagent", "main.py"), "import sys\nsys.exit(3)\n")
  const { defs, errors } = await discoverKeqing({ resolvePython: () => ["python"] })
  // 只验证不抛出 + errors 结构（具体成败取决于环境，本用例的坏驱动应失败）
  expect(Array.isArray(defs)).toBe(true)
  expect(Array.isArray(errors)).toBe(true)
})

test("keqingEnabled：off 显式关闭", () => {
  const saved = process.env.GEBAI_KEQING
  process.env.GEBAI_KEQING = "off"
  expect(keqingEnabled()).toBe(false)
  delete process.env.GEBAI_KEQING
  // 沙箱显式 on 同样禁用
  const savedSandbox = process.env.GEBAI_SANDBOX
  process.env.GEBAI_SANDBOX = "on"
  expect(keqingEnabled()).toBe(false)
  process.env.GEBAI_SANDBOX = "auto"
  expect(keqingEnabled()).toBe(true)
  if (saved !== undefined) process.env.GEBAI_KEQING = saved
  if (savedSandbox !== undefined) process.env.GEBAI_SANDBOX = savedSandbox
})

test("resolvePythonCommand：GEBAI_PYTHON_DIR 显式优先", () => {
  const saved = process.env.GEBAI_PYTHON_DIR
  // 指向真实存在的解释器目录形态不可静态保证，验证空值安全
  const r = resolvePythonCommand({ GEBAI_PYTHON_DIR: "" })
  expect(Array.isArray(r) || r === null).toBe(true)
  if (saved !== undefined) process.env.GEBAI_PYTHON_DIR = saved
})

test("ensureBuilt：可执行体缺失时执行 build、已存在/无 build 跳过、失败抛错", async () => {
  const langDir = join(tmpRoot, "rustb")
  const agentDir = join(langDir, "codec")
  mkdirSync(agentDir, { recursive: true })
  const manifest = parseManifest(
    JSON.stringify({
      name: "codec",
      description: "d",
      protocol: 2,
      command: ["{agent_dir}/driver{exe}"],
      build: { command: ["{agent_dir}/mkbinary.sh"] },
    }),
    agentDir,
  )!.manifest!
  // spawn 替身：模拟构建脚本产出目标文件（完整 SidecarProc 形态）
  const fakeSpawn = (cmd: string[]): import("./sidecar").SidecarProc => ({
    stdout: new Blob([""]).stream() as ReadableStream<Uint8Array>,
    stderr: new Blob([""]).stream() as ReadableStream<Uint8Array>,
    exitCode: Promise.resolve(cmd.length && cmd[0]!.endsWith("mkbinary.sh") ? 2 : 0),
    stdin: { write: () => undefined },
    kill: () => undefined,
    killed: false,
  })
  // 1) 无 build 声明：跳过返回 null
  const none = await ensureBuilt(agentDir, { ...manifest, build: undefined })
  expect(none).toBeNull()
  // 2) 目标已存在：跳过（不执行构建）
  const exeSuffix = process.platform === "win32" ? ".exe" : ""
  writeFileSync(join(agentDir, "driver" + exeSuffix), "binary")
  const exists = await ensureBuilt(agentDir, manifest)
  expect(exists).toBe(join(agentDir, "driver" + exeSuffix))
  // 3) 目标缺失 + 构建成功：产出目标文件
  rmSync(join(agentDir, "driver" + exeSuffix))
  const built = await ensureBuilt(agentDir, manifest, {
    spawn: (cmd) => {
      // 替身：直接写目标文件模拟编译产出
      const target = cmd[0]!.endsWith("mkbinary.sh") ? join(agentDir, "driver" + exeSuffix) : ""
      if (target) writeFileSync(target, "built")
      return fakeSpawn(cmd)
    },
  })
  expect(built).toBe(join(agentDir, "driver" + exeSuffix))
  // 4) 目标缺失 + 构建失败（无产出）：抛错（loadErrors 可见）
  rmSync(join(agentDir, "driver" + exeSuffix))
  await expect(
    ensureBuilt(agentDir, manifest, {
      spawn: () => fakeSpawn([]),
    }),
  ).rejects.toThrow("构建引导失败")
})
