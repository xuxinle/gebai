/** restart_server 工具单测：双平台拉起器脚本构造、环境变量挑选、状态读取、服务模式不注入。 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync, readFileSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import type { RestartDeps } from "./restart"
import { buildLauncherScript, buildLauncherScriptPosix, buildLauncherScriptWin, makeRestartServerTool, pickRestartEnv, restartDir } from "./restart"

function makeDeps(overrides: Partial<RestartDeps> = {}): RestartDeps {
  return {
    port: 3001,
    cwd: "/repo/packages/server",
    entry: "/repo/packages/server/src/index.ts",
    binary: false,
    oldPid: 1234,
    tmpDir: tmpdir(),
    env: { GEBAI_PORT: "3001", GEBAI_HOME: "/repo" },
    platform: "win32",
    exitDelayMs: 10,
    exit: () => {},
    spawnLauncher: async () => ({ ok: true }),
    ...overrides,
  }
}

/** 测试用 ToolContext 最小桩。 */
function ctxStub(workdir: string): never {
  return {
    sessionId: "s",
    user: "u",
    workdir,
    home: workdir,
    env: {},
    sandboxed: false,
    resolvePath: (p: string) => p,
    readFile: async () => "",
    readBinaryFile: async () => new Uint8Array(),
    writeFile: async () => {},
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async () => ({}),
  } as never
}

describe("restart_server（Windows 拉起器）", () => {
  test("buildLauncherScriptWin：等旧进程/端口释放 → Start-Process 启动（环境继承+cwd+日志重定向）→ 就绪探测 → 状态文件", () => {
    const script = buildLauncherScriptWin(makeDeps())
    expect(script).toContain("$oldPid = 1234")
    expect(script).toContain("$port = 3001")
    expect(script).toContain("Get-Process -Id $oldPid")
    expect(script).toContain("Start-Process -FilePath")
    expect(script).toContain("-WorkingDirectory '/repo/packages/server'")
    expect(script).toContain("$env:GEBAI_PORT = '3001'")
    expect(script).toContain("$env:GEBAI_HOME = '/repo'")
    expect(script).toContain("/api/v1/sub-agents")
    expect(script).toContain("state.json")
  })

  test("buildLauncherScriptWin：binary 模式无 run 入口参数", () => {
    const script = buildLauncherScriptWin(makeDeps({ binary: true }))
    expect(script).toContain("Start-Process -FilePath")
    expect(script).not.toContain("-ArgumentList 'run'")
  })

  test("buildLauncherScript 按平台分发：win32 → PowerShell；其余 → bash", () => {
    expect(buildLauncherScript(makeDeps())).toContain("$ErrorActionPreference")
    expect(buildLauncherScript(makeDeps({ platform: "linux" }))).toContain("#!/usr/bin/env bash")
    expect(buildLauncherScript(makeDeps({ platform: "darwin" }))).toContain("#!/usr/bin/env bash")
  })
})

describe("restart_server（Linux/macOS 拉起器）", () => {
  test("buildLauncherScriptPosix：等旧进程/端口释放（ss）→ nohup 启动（env 导出+cd+日志）→ 就绪探测（属主+curl）→ 状态文件", () => {
    const script = buildLauncherScriptPosix(makeDeps({ platform: "linux" }))
    expect(script).toContain("#!/usr/bin/env bash")
    expect(script).toContain("old_pid=1234")
    expect(script).toContain("port=3001")
    expect(script).toContain("kill -0")
    expect(script).toContain('ss -ltn "sport = :$port"')
    expect(script).toContain("nohup")
    expect(script).toContain("export GEBAI_PORT='3001'")
    expect(script).toContain("export GEBAI_HOME='/repo'")
    expect(script).toContain("server.log.out")
    expect(script).toContain("curl -sf -o /dev/null -m 3")
    expect(script).toContain("pid=\\K[0-9]+")
    expect(script).toContain("state.json")
  })

  test("buildLauncherScriptPosix：第三方占端口 → 失败退出不动无辜进程", () => {
    const script = buildLauncherScriptPosix(makeDeps({ platform: "linux" }))
    expect(script).toContain("被其他进程(PID")
    expect(script).toContain("exit 1")
  })

  test("buildLauncherScriptPosix：binary 模式无 run 参数", () => {
    const script = buildLauncherScriptPosix(makeDeps({ platform: "linux", binary: true }))
    const nohupLine = script.split("\n").find((l) => l.startsWith("nohup"))
    expect(nohupLine).toBeDefined()
    expect(nohupLine).not.toContain(" run ")
  })

  test("buildLauncherScriptPosix：路径含单引号正确转义（shq）", () => {
    const script = buildLauncherScriptPosix(
      makeDeps({ platform: "linux", cwd: "/repo/'quote'/server", env: { GEBAI_HOME: "/h'ome" } }),
    )
    expect(script).toContain("'/repo/'\\''quote'\\''/server'")
    expect(script).toContain("export GEBAI_HOME='/h'\\''ome'")
  })
})

describe("restart_server 工具行为", () => {
  test("pickRestartEnv：只挑启动级变量，未设不产出", () => {
    expect(pickRestartEnv({ GEBAI_PORT: "3001", GEBAI_LLM_API_KEY: "secret", GEBAI_HOME: "/h" })).toEqual({
      GEBAI_PORT: "3001",
      GEBAI_HOME: "/h",
    })
    expect(pickRestartEnv({})).toEqual({})
  })

  test("status 动作：读状态文件；无记录给出提示", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-test-"))
    const tool = makeRestartServerTool({ tmpDir: dir })
    const none = await tool.execute({ action: "status" }, ctxStub(dir))
    expect(none.output).toContain("尚无重启记录")
    rmSync(dir, { recursive: true, force: true })
  })

  test("restart 动作：部署拉起器→写进行中状态→延迟退出；exit 注入捕获不真退出", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-test-"))
    let exited = 0
    const deployed: Array<{ script: string; platform: string }> = []
    const tool = makeRestartServerTool({
      tmpDir: dir,
      exitDelayMs: 5,
      exit: () => {
        exited++
      },
      spawnLauncher: async (script, platform) => {
        deployed.push({ script, platform })
        return { ok: true }
      },
    })
    const res = await tool.execute({ action: "restart" }, ctxStub(dir))
    expect(res.output).toContain("重启已布置")
    expect(deployed).toHaveLength(1)
    expect(deployed[0].script.endsWith("launcher.ps1")).toBe(true)
    expect(deployed[0].platform).toBe(process.platform)
    const state = JSON.parse(readFileSync(join(restartDir(dir), "state.json"), "utf8"))
    expect(state.ok).toBeNull()
    await new Promise((r) => setTimeout(r, 60))
    expect(exited).toBe(1)
    rmSync(dir, { recursive: true, force: true })
  })

  test("restart 动作（linux）：部署 launcher.sh", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-test-"))
    const deployed: string[] = []
    const tool = makeRestartServerTool({
      tmpDir: dir,
      platform: "linux",
      exitDelayMs: 5,
      exit: () => {},
      spawnLauncher: async (script) => {
        deployed.push(script)
        return { ok: true }
      },
    })
    await tool.execute({ action: "restart" }, ctxStub(dir))
    expect(deployed[0].endsWith("launcher.sh")).toBe(true)
    rmSync(dir, { recursive: true, force: true })
  })

  test("restart 动作：拉起器部署失败时不退出（服务保持运行）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-test-"))
    let exited = 0
    const tool = makeRestartServerTool({
      tmpDir: dir,
      exitDelayMs: 5,
      exit: () => {
        exited++
      },
      spawnLauncher: async () => ({ ok: false, error: "setsid 不可用" }),
    })
    const res = await tool.execute({ action: "restart" }, ctxStub(dir))
    expect(res.output).toContain("拉起器部署失败")
    expect(res.output).toContain("setsid 不可用")
    await new Promise((r) => setTimeout(r, 60))
    expect(exited).toBe(0)
    rmSync(dir, { recursive: true, force: true })
  })
})

describe("restart_server 服务模式不注入", () => {
  test("compose 过滤逻辑：auth=server 时 restart_server 被跳过（经 createAllGlobalTools 名单存在 + compose 注释约定）", async () => {
    const { createAllGlobalTools } = await import("../tools")
    const all = createAllGlobalTools()
    expect(all["restart_server"]).toBeDefined()
    expect(all["restart_server"].description).toContain("仅本地模式")
  })
})
