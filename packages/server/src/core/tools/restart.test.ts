/** restart_server 工具单测：双平台拉起器脚本构造、环境变量挑选、状态读取、服务模式不注入。 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync, readFileSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import type { RestartContinuation, RestartDeps } from "./restart"
import {
  buildLauncherScript,
  buildLauncherScriptPosix,
  buildLauncherScriptWin,
  consumeRestartContinuation,
  makeRestartServerTool,
  pickRestartEnv,
  readContinuation,
  readContinuationResult,
  removeContinuation,
  restartDir,
  writeContinuation,
} from "./restart"

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
    devReload: false,
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
    expect(script).toContain("Move-Item") // 日志轮转（Start-Process 覆盖写会吞掉上次诊断日志）
    expect(script).toContain("server.log.out.prev")
    expect(script).toContain("Start-Process -FilePath")
    expect(script).toContain("-WorkingDirectory '/repo/packages/server'")
    expect(script).toContain("$env:GEBAI_PORT = '3001'")
    expect(script).toContain("$env:GEBAI_HOME = '/repo'")
    expect(script).toContain("/api/v1/sub-agents")
    expect(script).toContain("state.json")
  })

  test("buildLauncherScriptWin：状态文件无 BOM（[IO.File]::WriteAllText，PS5.1 Set-Content UTF8 会带 BOM）", () => {
    const script = buildLauncherScriptWin(makeDeps())
    expect(script).toContain("[IO.File]::WriteAllText")
    expect(script).not.toContain("Set-Content -Encoding UTF8")
  })

  test("buildLauncherScriptWin：GEBAI_BASE_PATH 时探测 URL 带前缀", () => {
    const script = buildLauncherScriptWin(makeDeps({ env: { GEBAI_PORT: "3001", GEBAI_BASE_PATH: "/gebai" } }))
    expect(script).toContain("'http://127.0.0.1:' + $port + '/gebai/api/v1/sub-agents'")
    expect(script).not.toContain("'http://127.0.0.1:' + $port + '/api'")
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
    expect(script).toContain("server.log.out.prev") // 日志轮转（保留上次重启的诊断日志）
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
      platform: "win32", // 断言 PowerShell 拉起器：显式注入，不随宿主平台漂移
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
    expect(deployed[0].platform).toBe("win32")
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

// Windows 部署器实机冒烟：Win11 24H2+ 移除 WMIC 后 wmic 不可用，拉起器部署改用二段式
// PowerShell Start-Process（跳过条件：非 win32 宿主）。
// 注：冒烟只验证部署器（外层 powershell 退出码）——拉起器脚本内容用假 oldPid/端口构造，
// 对不存在进程立即超时跳过、端口探测后按预期失败退出并写状态文件，无副作用。
;(process.platform === "win32" ? describe : describe.skip)("restart_server（Windows 部署器实机冒烟）", () => {
  test("默认部署器（win32）：二段式 Start-Process 成功启动拉起器，不再依赖 WMIC", async () => {
    const { makeRestartServerTool } = await import("./restart")
    // 动态选空闲高位端口：固定幻端口（如 1）在 Windows 管理员下可绑定，拉起器会真启动服务成孤儿进程
    const freePort = await new Promise<number>((resolve, reject) => {
      const srv = require("node:net").createServer()
      srv.listen(0, "127.0.0.1", () => {
        const p = srv.address().port
        srv.close(() => resolve(p))
      })
      srv.on("error", reject)
    })
    const tool = makeRestartServerTool({
      tmpDir: mkdtempSync(join(tmpdir(), "restart-smoke-")),
      // 假 PID 立即满足「等旧进程退出」；拉起器在空闲端口上真启动一次服务——就绪后写成功状态（验证全链路），
      // 该孤立服务无下游依赖，进程关闭后随 job object 回收
      oldPid: -1,
      port: freePort,
      exitDelayMs: 60_000,
      exit: () => {},
    })
    const res = await tool.execute({ action: "restart" }, ctxStub(tmpdir()))
    expect(res.output).toContain("重启已布置")
  })
})

describe("restart_server 续跑（prompt 参数）", () => {
  test("续跑请求落盘：写后可读回，删除后为空", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-cont-"))
    try {
      const req: RestartContinuation = { sessionId: "s1", user: "u1", role: "admin", prompt: "服务已重启，请继续", at: Date.now(), oldPid: 42 }
      await writeContinuation(req, dir)
      expect(await readContinuation(dir)).toEqual(req)
      await removeContinuation(dir)
      expect(await readContinuation(dir)).toBeNull()
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("restart 动作带 prompt：写续跑请求（当前会话/用户）+ 输出提示续跑布置", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-cont-"))
    try {
      const tool = makeRestartServerTool({
        tmpDir: dir,
        platform: "win32",
        exitDelayMs: 5,
        exit: () => {},
        spawnLauncher: async () => ({ ok: true }),
      })
      const res = await tool.execute({ action: "restart", prompt: "  服务已重启，请继续验证  " }, ctxStub(dir))
      expect(res.output).toContain("续跑已布置")
      const cont = await readContinuation(dir)
      expect(cont?.prompt).toBe("服务已重启，请继续验证") // 首尾空白归一
      expect(cont?.sessionId).toBe("s") // 缺省=当前会话（ctxStub）
      expect(cont?.user).toBe("u")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("restart 动作带 prompt + session：按指定会话续跑", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-cont-"))
    try {
      const tool = makeRestartServerTool({ tmpDir: dir, platform: "win32", exitDelayMs: 5, exit: () => {}, spawnLauncher: async () => ({ ok: true }) })
      await tool.execute({ action: "restart", prompt: "继续", session: "abcdef01abcdef01abcdef01abcdef01" }, ctxStub(dir))
      expect((await readContinuation(dir))?.sessionId).toBe("abcdef01abcdef01abcdef01abcdef01")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("拉起器部署失败：不退出且清理续跑请求（不残留待消费指令）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-cont-"))
    let exited = 0
    try {
      const tool = makeRestartServerTool({
        tmpDir: dir,
        platform: "win32",
        exitDelayMs: 5,
        exit: () => {
          exited++
        },
        spawnLauncher: async () => ({ ok: false, error: "部署失败" }),
      })
      const res = await tool.execute({ action: "restart", prompt: "继续" }, ctxStub(dir))
      expect(res.output).toContain("拉起器部署失败")
      await new Promise((r) => setTimeout(r, 40))
      expect(exited).toBe(0)
      expect(await readContinuation(dir)).toBeNull()
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("status 动作展示续跑请求与结果", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-cont-"))
    try {
      await writeContinuation({ sessionId: "s9", user: "u", prompt: "继续干活", at: Date.now(), oldPid: 1 }, dir)
      const tool = makeRestartServerTool({ tmpDir: dir })
      const res = await tool.execute({ action: "status" }, ctxStub(dir))
      expect(res.output).toContain("续跑请求（等待新服务消费）")
      expect(res.output).toContain("s9")
      expect(res.output).toContain("续跑结果：无")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })
})

describe("restart_server 继承 dev-reload（重启后前端构建 watch 不丢）", () => {
  test("pickRestartEnv：GEBAI_DEV_RELOAD 作为启动级变量被继承", () => {
    expect(pickRestartEnv({ GEBAI_DEV_RELOAD: "1", GEBAI_PORT: "3001" })).toEqual({ GEBAI_PORT: "3001", GEBAI_DEV_RELOAD: "1" })
  })

  test("devReload 模式（--reload 启动）：拉起器脚本给新进程显式带 GEBAI_DEV_RELOAD=1（argv 不随拉起器复制）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-cont-"))
    const scripts: string[] = []
    try {
      const tool = makeRestartServerTool({
        tmpDir: dir,
        platform: "win32",
        devReload: true,
        env: { GEBAI_PORT: "3001" },
        exitDelayMs: 5,
        exit: () => {},
        spawnLauncher: async (script) => {
          scripts.push(script)
          return { ok: true }
        },
      })
      const res = await tool.execute({ action: "restart" }, ctxStub(dir))
      expect(res.output).toContain("dev-reload 模式继承")
      expect(readFileSync(scripts[0], "utf8")).toContain("$env:GEBAI_DEV_RELOAD = '1'")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("非 devReload 模式：不向新进程注入该变量", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-cont-"))
    const scripts: string[] = []
    try {
      const tool = makeRestartServerTool({
        tmpDir: dir,
        platform: "linux",
        devReload: false,
        env: { GEBAI_PORT: "3001" },
        exitDelayMs: 5,
        exit: () => {},
        spawnLauncher: async (script) => {
          scripts.push(script)
          return { ok: true }
        },
      })
      await tool.execute({ action: "restart" }, ctxStub(dir))
      expect(readFileSync(scripts[0], "utf8")).not.toContain("GEBAI_DEV_RELOAD")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })
})

describe("consumeRestartContinuation（新服务启动消费续跑请求）", () => {
  /** 虚拟时钟：sleep 推进虚拟时间，避免 timeout 分支真等 25s。 */
  function clock(stepMs: number) {
    let t = 1_700_000_000_000
    return { now: () => t, sleep: async () => void (t += stepMs), at: () => t }
  }

  function makeDir(): string {
    return mkdtempSync(join(tmpdir(), "restart-consume-"))
  }

  const REQ: RestartContinuation = { sessionId: "sess-1", user: "admin", role: "admin", prompt: "服务已重启，请继续", at: 1_700_000_000_000, oldPid: 7 }

  test("无请求：静默返回，不触发运行", async () => {
    const dir = makeDir()
    try {
      let ran = 0
      const out = await consumeRestartContinuation({ tmpDir: dir, pid: 99, run: async () => void ran++, log: () => {} })
      expect(out.consumed).toBe(false)
      expect(out.reason).toBe("无续跑请求")
      expect(ran).toBe(0)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("confirmed（state.json 的 pid 为本进程）：按提示词续跑、请求被消费、结果落盘", async () => {
    const dir = makeDir()
    try {
      const c = clock(500)
      await writeContinuation({ ...REQ, at: c.now() }, dir)
      writeFileSync(join(restartDir(dir), "state.json"), JSON.stringify({ ok: true, port: 3000, pid: 99 }))
      const seen: RestartContinuation[] = []
      let during: Awaited<ReturnType<typeof readContinuationResult>> = null
      const out = await consumeRestartContinuation({
        tmpDir: dir,
        pid: 99,
        run: async (req) => {
          seen.push(req)
          during = await readContinuationResult(dir) // 执行期间应先有「执行中」记录（任务中途被杀也留痕迹）
        },
        now: c.now,
        sleep: c.sleep,
        log: () => {},
      })
      expect(out).toMatchObject({ consumed: true, confirm: "confirmed" })
      expect(seen).toHaveLength(1)
      expect(seen[0].sessionId).toBe("sess-1")
      expect(seen[0].prompt).toBe("服务已重启，请继续")
      expect(during).toMatchObject({ ok: null, note: "续跑执行中", confirm: "confirmed" })
      expect(await readContinuation(dir)).toBeNull() // 一次性消费
      expect((await readContinuationResult(dir))?.ok).toBe(true)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("拉起器已判失败（ok=false）：仍执行续跑（人工恢复场景不丢提示词），confirm=failed", async () => {
    const dir = makeDir()
    try {
      const c = clock(500)
      await writeContinuation({ ...REQ, at: c.now() }, dir)
      writeFileSync(join(restartDir(dir), "state.json"), JSON.stringify({ ok: false, error: "端口被其他进程占用" }))
      let ran = 0
      const out = await consumeRestartContinuation({ tmpDir: dir, pid: 99, run: async () => void ran++, now: c.now, sleep: c.sleep, log: () => {} })
      expect(out.confirm).toBe("failed")
      expect(ran).toBe(1)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("等待超时未确认（无 state.json）：兜底执行，confirm=timeout", async () => {
    const dir = makeDir()
    try {
      const c = clock(500)
      await writeContinuation({ ...REQ, at: c.now() }, dir)
      let ran = 0
      const out = await consumeRestartContinuation({
        tmpDir: dir,
        pid: 99,
        run: async () => void ran++,
        now: c.now,
        sleep: c.sleep,
        waitStateMs: 1000,
        pollMs: 500,
        log: () => {},
      })
      expect(out.confirm).toBe("timeout")
      expect(ran).toBe(1)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("请求过期（>10 分钟）：不执行并清理，不给无关启动误跑", async () => {
    const dir = makeDir()
    try {
      const c = clock(500)
      await writeContinuation({ ...REQ, at: c.now() - 11 * 60_000 }, dir)
      let ran = 0
      const out = await consumeRestartContinuation({ tmpDir: dir, pid: 99, run: async () => void ran++, now: c.now, sleep: c.sleep, log: () => {} })
      expect(out.consumed).toBe(false)
      expect(out.reason).toBe("已过期")
      expect(ran).toBe(0)
      expect(await readContinuation(dir)).toBeNull()
      expect((await readContinuationResult(dir))?.reason).toContain("过期")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("会话不存在：不运行，结果记录原因（请求同样被消费，不重复尝试）", async () => {
    const dir = makeDir()
    try {
      const c = clock(500)
      await writeContinuation({ ...REQ, at: c.now() }, dir)
      writeFileSync(join(restartDir(dir), "state.json"), JSON.stringify({ ok: true, pid: 99 }))
      let ran = 0
      const out = await consumeRestartContinuation({
        tmpDir: dir,
        pid: 99,
        run: async () => void ran++,
        sessionExists: async () => false,
        now: c.now,
        sleep: c.sleep,
        log: () => {},
      })
      expect(out.reason).toBe("会话不存在")
      expect(ran).toBe(0)
      expect((await readContinuationResult(dir))?.reason).toContain("会话不存在")
      expect(await readContinuation(dir)).toBeNull()
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("续跑执行抛错：结果记录错误（不抛出到启动路径）", async () => {
    const dir = makeDir()
    try {
      const c = clock(500)
      await writeContinuation({ ...REQ, at: c.now() }, dir)
      writeFileSync(join(restartDir(dir), "state.json"), JSON.stringify({ ok: true, pid: 99 }))
      const out = await consumeRestartContinuation({
        tmpDir: dir,
        pid: 99,
        run: async () => {
          throw new Error("模型不可用")
        },
        now: c.now,
        sleep: c.sleep,
        log: () => {},
      })
      expect(out.consumed).toBe(false)
      expect(out.reason).toContain("模型不可用")
      const res = await readContinuationResult(dir)
      expect(res?.ok).toBe(false)
      expect(res?.error).toContain("模型不可用")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
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
