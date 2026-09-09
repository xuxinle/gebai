/** restart_server 工具单测：拉起器命令构造、环境变量挑选、状态读取、服务模式不注入。 */
import { afterEach, describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync, readFileSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import { buildNewServerCommand, buildLauncherScript, makeRestartServerTool, pickRestartEnv, restartDir } from "./restart"

function makeDeps(overrides: Partial<Parameters<typeof buildNewServerCommand>[0]> = {}) {
  return {
    port: 3001,
    cwd: "C:/repo/packages/server",
    entry: "C:/repo/packages/server/src/index.ts",
    binary: false,
    oldPid: 1234,
    tmpDir: tmpdir(),
    env: { GEBAI_PORT: "3001", GEBAI_HOME: "C:/repo" },
    platform: "win32" as const,
    exitDelayMs: 10,
    exit: () => {},
    spawnLauncher: async () => ({ ok: true }),
    ...overrides,
  }
}

describe("restart_server", () => {
  test("buildNewServerCommand：cmd /c + 环境前缀 + cd + 启动 + 日志重定向", () => {
    const cmd = buildNewServerCommand(makeDeps())
    expect(cmd).toContain('cmd /c set "GEBAI_PORT=3001"&&set "GEBAI_HOME=C:/repo"&&')
    expect(cmd).toContain('cd /d "C:/repo/packages/server"&&')
    expect(cmd).toContain('run "C:/repo/packages/server/src/index.ts"')
    expect(cmd).toContain(join(restartDir(tmpdir()), "server.log").replace(/\\/g, "\\"))
  })

  test("buildNewServerCommand：binary 模式直接 spawn 自身可执行文件", () => {
    const cmd = buildNewServerCommand(makeDeps({ binary: true }))
    expect(cmd).toContain(process.execPath)
    expect(cmd).not.toContain("run")
  })

  test("buildLauncherScript：等旧进程/端口释放 → Start-Process 启动（环境继承+cwd+日志重定向）→ 就绪探测 → 状态文件", () => {
    const script = buildLauncherScript(makeDeps())
    expect(script).toContain("$oldPid = 1234")
    expect(script).toContain("$port = 3001")
    expect(script).toContain("Get-Process -Id $oldPid")
    expect(script).toContain("Start-Process -FilePath")
    expect(script).toContain("-WorkingDirectory 'C:/repo/packages/server'")
    expect(script).toContain("$env:GEBAI_PORT = '3001'")
    expect(script).toContain("$env:GEBAI_HOME = 'C:/repo'")
    expect(script).toContain("/api/v1/sub-agents")
    expect(script).toContain("state.json")
  })

  test("pickRestartEnv：只挑启动级变量，未设不产出", () => {
    expect(pickRestartEnv({ GEBAI_PORT: "3001", GEBAI_LLM_API_KEY: "secret", GEBAI_HOME: "C:/h" })).toEqual({
      GEBAI_PORT: "3001",
      GEBAI_HOME: "C:/h",
    })
    expect(pickRestartEnv({})).toEqual({})
  })

  test("status 动作：读状态文件；无记录给出提示", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-test-"))
    const tool = makeRestartServerTool({ tmpDir: dir })
    const none = await tool.execute({ action: "status" }, { sessionId: "s", user: "u", workdir: dir, home: dir, env: {}, sandboxed: false, resolvePath: (p: string) => p, readFile: async () => "", readBinaryFile: async () => new Uint8Array(), writeFile: async () => {}, listFiles: async () => [], listDir: async () => [], deleteFile: async () => {}, moveFile: async () => {}, runCommand: async () => ({ stdout: "", stderr: "", code: 0 }), uploadAttachment: async () => ({}) } as never)
    expect(none.output).toContain("尚无重启记录")
    rmSync(dir, { recursive: true, force: true })
  })

  test("restart 动作：部署拉起器→写进行中状态→延迟退出；exit 被注入捕获不真退出", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-test-"))
    let exited = 0
    const launcherCmds: string[] = []
    const tool = makeRestartServerTool({
      tmpDir: dir,
      exitDelayMs: 5,
      exit: () => {
        exited++
      },
      spawnLauncher: async (cmd) => {
        launcherCmds.push(cmd)
        return { ok: true }
      },
    })
    const res = await tool.execute({ action: "restart" }, { sessionId: "s", user: "u", workdir: dir, home: dir, env: {}, sandboxed: false, resolvePath: (p: string) => p, readFile: async () => "", readBinaryFile: async () => new Uint8Array(), writeFile: async () => {}, listFiles: async () => [], listDir: async () => [], deleteFile: async () => {}, moveFile: async () => {}, runCommand: async () => ({ stdout: "", stderr: "", code: 0 }), uploadAttachment: async () => ({}) } as never)
    expect(res.output).toContain("重启已布置")
    expect(launcherCmds).toHaveLength(1)
    expect(launcherCmds[0]).toContain("powershell")
    expect(launcherCmds[0]).toContain("launcher.ps1")
    // 状态文件先写进行中
    const state = JSON.parse(readFileSync(join(restartDir(dir), "state.json"), "utf8"))
    expect(state.ok).toBeNull()
    // 延迟退出已布置
    await new Promise((r) => setTimeout(r, 60))
    expect(exited).toBe(1)
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
      spawnLauncher: async () => ({ ok: false, error: "wmic 拒绝访问" }),
    })
    const res = await tool.execute({ action: "restart" }, { sessionId: "s", user: "u", workdir: dir, home: dir, env: {}, sandboxed: false, resolvePath: (p: string) => p, readFile: async () => "", readBinaryFile: async () => new Uint8Array(), writeFile: async () => {}, listFiles: async () => [], listDir: async () => [], deleteFile: async () => {}, moveFile: async () => {}, runCommand: async () => ({ stdout: "", stderr: "", code: 0 }), uploadAttachment: async () => ({}) } as never)
    expect(res.output).toContain("拉起器部署失败")
    expect(res.output).toContain("wmic 拒绝访问")
    await new Promise((r) => setTimeout(r, 60))
    expect(exited).toBe(0)
    rmSync(dir, { recursive: true, force: true })
  })

  test("非 Windows：返回不可用说明不布置重启", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-test-"))
    let exited = 0
    const calls: string[] = []
    const tool = makeRestartServerTool({
      tmpDir: dir,
      platform: "linux" as never,
      exit: () => {
        exited++
      },
      spawnLauncher: async (cmd) => {
        calls.push(cmd)
        return { ok: true }
      },
    })
    const res = await tool.execute({}, { sessionId: "s", user: "u", workdir: dir, home: dir, env: {}, sandboxed: false, resolvePath: (p: string) => p, readFile: async () => "", readBinaryFile: async () => new Uint8Array(), writeFile: async () => {}, listFiles: async () => [], listDir: async () => [], deleteFile: async () => {}, moveFile: async () => {}, runCommand: async () => ({ stdout: "", stderr: "", code: 0 }), uploadAttachment: async () => ({}) } as never)
    expect(res.output).toContain("仅支持 Windows")
    expect(calls).toHaveLength(0)
    expect(exited).toBe(0)
    rmSync(dir, { recursive: true, force: true })
  })
})

describe("restart_server 服务模式不注入", () => {
  test("compose 过滤逻辑：auth=server 时 restart_server 被跳过（经 createAllGlobalTools 名单存在 + compose 注释约定）", async () => {
    // 工具在全局表（本地模式注入）；服务模式过滤发生在 compose（config.auth!=="local" 跳过注册）
    const { createAllGlobalTools } = await import("../tools")
    const all = createAllGlobalTools()
    expect(all["restart_server"]).toBeDefined()
    expect(all["restart_server"].description).toContain("仅本地模式")
  })
})

afterEach(() => {
  /* 临时目录各用例自清 */
})
