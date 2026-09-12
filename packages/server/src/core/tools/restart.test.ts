/** restart_server 工具单测：双平台拉起器脚本构造、环境变量挑选、状态读取、服务模式不注入。 */
import { describe, expect, test } from "bun:test"
import { mkdirSync, mkdtempSync, rmSync, readFileSync, statSync, writeFileSync } from "node:fs"
import { connect, createServer } from "node:net"
import { join } from "node:path"
import { tmpdir } from "node:os"
import type { RestartContinuation, RestartDeps } from "./restart"
import {
  buildLauncherScript,
  buildLauncherScriptPosix,
  buildLauncherScriptWin,
  consumeRestartContinuation,
  explainRestartState,
  formatLauncherFreshness,
  launcherCodeFreshness,
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

  test("拉起器脚本：logTail 先还原再读取（否则读的是已轮转走的空文件，诊断信息丢失）", () => {
    // Windows：脚本开头把 .out/.err 轮转为 .prev，因此失败分支必须**先 Move-Item 还原、再 Get-Content 读**
    const winLines = buildLauncherScriptWin(makeDeps()).split("\n")
    const winRestore = winLines.findIndex((l) => l.includes(".err.prev") && l.includes("Move-Item"))
    const winRead = winLines.findIndex((l) => l.includes("Get-Content") && l.includes(".err'"))
    expect(winRestore).toBeGreaterThan(-1)
    expect(winRead).toBeGreaterThan(-1)
    expect(winRestore).toBeLessThan(winRead) // 顺序断言：还原在前
    // posix：同口径（还原 mv -f 先于 tail_log 定义/调用）
    const posix = buildLauncherScriptPosix(makeDeps({ platform: "linux" }))
    const posixLines = posix.split("\n")
    const posRestore = posixLines.findIndex((l) => l.includes("server.log.err.prev") && l.trim().startsWith("mv -f"))
    const posTail = posixLines.findIndex((l) => l.includes("tail_log()"))
    expect(posRestore).toBeGreaterThan(-1)
    expect(posTail).toBeGreaterThan(-1)
    expect(posRestore).toBeLessThan(posTail)
    // 占用分支必须带 logTail（两平台）；Windows 读日志需显式 -Encoding UTF8（服务日志为 UTF-8，
    // PS5.1 的 Get-Content 缺省按 ANSI 读 → 中文诊断信息会乱码）
    expect(buildLauncherScriptWin(makeDeps())).toContain("logTail = $tail")
    expect(winLines.filter((l) => l.includes("Get-Content")).every((l) => l.includes("-Encoding UTF8"))).toBe(true)
    expect(posix).toContain('"logTail":"%s"')
    // tail_log 只定义一次（避免重复定义）
    expect(posixLines.filter((l) => l.includes("tail_log()")).length).toBe(1)
  })

  test("buildLauncherScriptPosix：第三方占端口 → 失败退出不动无辜进程", () => {
    const script = buildLauncherScriptPosix(makeDeps({ platform: "linux" }))
    expect(script).toContain("被其他进程(PID")
    expect(script).toContain('"reason":"occupied"')
    expect(script).toContain("exit 1")
  })

  test("buildLauncherScriptWin：端口占用分类——属主存活=真占用、属主已死=僵尸套接字（各写准原因）", () => {
    const script = buildLauncherScriptWin(makeDeps())
    // 属主存活判定：端口释放的判据是 socket 句柄引用计数归零，不是「进程存在」
    expect(script).toContain("$ownerAlive = $false")
    expect(script).toContain("Get-Process -Id $owner")
    expect(script).toContain("reason = 'occupied'")
    expect(script).toContain("reason = 'zombie-socket'")
    // 僵尸套接字的错误文案要点明「用户空间无法释放」
    expect(script).toContain("僵尸套接字")
    expect(script).toContain("已不存在，但端口仍 LISTENING")
    // 两种情形都严格失败（不自动换端口、不动无辜进程）
    expect(script).toContain("exit 1")
    // 中止时还原轮转掉的诊断日志（否则现场只剩 .prev）
    expect(script).toContain("Move-Item '")
    expect(script.match(/\.prev' '[^']*server\.log/g)?.length ?? 0).toBeGreaterThanOrEqual(2)
  })

  test("buildLauncherScriptPosix：属主已死 → 陈旧监听记录分类（与 Windows 同口径）", () => {
    const script = buildLauncherScriptPosix(makeDeps({ platform: "linux" }))
    expect(script).toContain("owner_alive=0")
    expect(script).toContain('kill -0 "$owner"')
    expect(script).toContain("zombie-socket")
  })

  test("explainRestartState：僵尸套接字的诊断与处置指引（区分「真占用」）", () => {
    const zombie = explainRestartState({ ok: false, reason: "zombie-socket", port: 3000, ownerPid: 2564, error: "端口 3000 被僵尸套接字占用" })
    const joined = zombie.join("\n")
    expect(joined).toContain("僵尸套接字")
    expect(joined).toContain("PID 2564")
    expect(joined).toContain("已不存在")
    expect(joined).toContain("注销并重新登录") // 关键自救手段（比重启电脑快）
    expect(joined).toContain("taskkill") // 解释为何杀不掉
    const occupied = explainRestartState({ ok: false, reason: "occupied", port: 3000, ownerPid: 99 }).join("\n")
    expect(occupied).toContain("仍存活")
    expect(occupied).not.toContain("僵尸")
    // 就绪超时：提示看新服务日志尾部
    const timeout = explainRestartState({ ok: false, reason: "ready-timeout", error: "就绪超时（90s）", logTail: "boom" }).join("\n")
    expect(timeout).toContain("90 秒内就绪")
    expect(timeout).toContain("boom")
  })

  test("launcherCodeFreshness：进程启动后源码又被改过 → 判定为落后（拉起器改动需再重启一次）", () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-fresh-"))
    try {
      const src = join(dir, "restart.ts")
      writeFileSync(src, "// v1", "utf8")
      const mtime = statSync(src).mtimeMs
      // 进程启动于源码修改**之前** → 运行中代码落后于磁盘
      expect(launcherCodeFreshness({ sourceFile: src, startedAt: mtime - 5000 })).toMatchObject({ checked: true, stale: true })
      // 进程启动于源码修改**之后** → 最新（当前代码就是磁盘代码）
      expect(launcherCodeFreshness({ sourceFile: src, startedAt: mtime + 5000 })).toMatchObject({ checked: true, stale: false })
      // 容差：同一秒内改完立即重启不误报
      expect(launcherCodeFreshness({ sourceFile: src, startedAt: mtime - 500 }).stale).toBe(false)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("launcherCodeFreshness：源码不可读（二进制/编译形态）→ checked=false，不误报落后", () => {
    const r = launcherCodeFreshness({ sourceFile: join(tmpdir(), "definitely-missing-restart.ts") })
    expect(r.checked).toBe(false)
    expect(r.stale).toBe(false)
  })

  test("formatLauncherFreshness：落后时给出重启两次的说明，最新时一行确认，不可判定时如实说明", () => {
    const stale = formatLauncherFreshness({ checked: true, stale: true, sourceMtime: 1_700_000_000_000, startedAt: 1_699_999_000_000 })
    expect(stale).toContain("拉起器代码落后")
    expect(stale).toContain("再重启一次")
    const fresh = formatLauncherFreshness({ checked: true, stale: false, sourceMtime: 1_700_000_000_000, startedAt: 1_700_001_000_000 })
    expect(fresh).toContain("拉起器代码：最新")
    expect(fresh).not.toContain("再重启一次")
    expect(formatLauncherFreshness({ checked: false, stale: false })).toContain("无法判定")
  })

  test("status 动作：僵尸状态展示诊断与处置（不再只打印原始 JSON）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "restart-zombie-"))
    try {
      mkdirSync(join(restartDir(dir)), { recursive: true })
      writeFileSync(
        join(restartDir(dir), "state.json"),
        JSON.stringify({ ok: false, reason: "zombie-socket", port: 3000, ownerPid: 2564, error: "端口 3000 被僵尸套接字占用" }),
        "utf8",
      )
      const tool = makeRestartServerTool({ tmpDir: dir })
      const res = await tool.execute({ action: "status" }, ctxStub(dir))
      expect(res.output).toContain("僵尸套接字")
      expect(res.output).toContain("注销并重新登录")
      expect(res.output).toContain("原始 state.json")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
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

// ---- Windows 部署器实机冒烟辅助（真起服务进程，必须能干净回收） ----

/** 取一个当前空闲的端口（内核分配后立即释放）。 */
async function pickFreePort(): Promise<number> {
  return new Promise<number>((resolve, reject) => {
    const srv = createServer()
    srv.listen(0, "127.0.0.1", () => {
      const addr = srv.address()
      const port = addr && typeof addr === "object" ? addr.port : 0
      srv.close(() => resolve(port))
    })
    srv.on("error", reject)
  })
}

/** 端口监听者 PID（无监听返回 0；探测方式与拉起器同源：Get-NetTCPConnection）。 */
async function portOwnerPid(port: number): Promise<number> {
  const proc = Bun.spawn(
    [
      "powershell",
      "-NoProfile",
      "-Command",
      `((Get-NetTCPConnection -LocalPort ${port} -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1).OwningProcess)`,
    ],
    { stdout: "pipe", stderr: "ignore", windowsHide: true },
  )
  const out = (await new Response(proc.stdout).text()).trim()
  await proc.exited
  const pid = Number(out)
  return Number.isFinite(pid) && pid > 0 ? pid : 0
}

/** 进程是否存在（ESRCH=不存在；EPERM 等=存在但非本进程，按存在计）。 */
function processAlive(pid: number): boolean {
  if (!pid || pid <= 0) return false
  try {
    process.kill(pid, 0)
    return true
  } catch (err) {
    return (err as NodeJS.ErrnoException)?.code !== "ESRCH"
  }
}

/** 端口是否仍在监听（TCP 连接探测）。 */
function portListening(port: number): Promise<boolean> {
  return new Promise<boolean>((resolve) => {
    const sock = connect({ host: "127.0.0.1", port })
    sock.setTimeout(500)
    sock.once("connect", () => {
      sock.destroy()
      resolve(true)
    })
    sock.once("timeout", () => {
      sock.destroy()
      resolve(false)
    })
    sock.once("error", () => resolve(false))
  })
}

/** 等端口释放（最多 timeoutMs）。 */
async function waitPortReleased(port: number, timeoutMs = 15_000): Promise<void> {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (!(await portListening(port))) return
    await new Promise((r) => setTimeout(r, 300))
  }
}

/** 收进程树：SIGTERM → 轮询等退出 → 仍在则 taskkill /T /F。
 *  端口属主进程可能有子进程（`bun run <入口>` 的运行时形态视版本而定），必须连树一起收。 */
async function killProcessTree(pid: number): Promise<void> {
  if (!pid || pid <= 0) return
  try {
    process.kill(pid, "SIGTERM")
  } catch {
    /* 已退出 */
  }
  const soft = Date.now() + 5_000
  while (Date.now() < soft && processAlive(pid)) await new Promise((r) => setTimeout(r, 200))
  if (!processAlive(pid)) return
  await Bun.spawn(["taskkill", "/PID", String(pid), "/T", "/F"], { stdout: "ignore", stderr: "ignore", windowsHide: true }).exited
  const hard = Date.now() + 5_000
  while (Date.now() < hard && processAlive(pid)) await new Promise((r) => setTimeout(r, 200))
}

/** 命令行含指定服务入口的 bun 服务进程 PID 清单（`bun run <入口>`）——冒烟兜底回收与残留复核的依据。
 *  限 `bun.exe`：查询本身的 powershell/cmd 命令行里就带着这个匹配串，不限进程名会把它们自己也匹配进来。 */
async function servicePids(entry: string): Promise<number[]> {
  const pattern = entry.replace(/'/g, "''")
  const proc = Bun.spawn(
    [
      "powershell",
      "-NoProfile",
      "-Command",
      `(Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'bun.exe' -and $_.CommandLine -like '*${pattern}*' } | Select-Object -ExpandProperty ProcessId) -join ','`,
    ],
    { stdout: "pipe", stderr: "ignore", windowsHide: true },
  )
  const out = (await new Response(proc.stdout).text()).trim()
  await proc.exited
  return out
    .split(",")
    .map((s) => Number(s.trim()))
    .filter((n) => Number.isFinite(n) && n > 0)
}

/** 收掉本用例期间新出现的服务进程（基线内的不动），返回仍残留的 PID（超时未收干净）。
 *  进程被终止到从进程表消失有时间差，故轮询直到没有新的。 */
async function killStrayServices(entry: string, baseline: number[], timeoutMs = 15_000): Promise<number[]> {
  const deadline = Date.now() + timeoutMs
  for (;;) {
    const strays = (await servicePids(entry).catch(() => [] as number[])).filter((x) => !baseline.includes(x))
    if (strays.length === 0) return []
    for (const p of strays) await killProcessTree(p)
    if (Date.now() >= deadline) return strays
    await new Promise((r) => setTimeout(r, 500))
  }
}

/** 等拉起器写 state.json 终态（ok 非 null）；超时返回 null。 */
async function waitRestartState(tmpDir: string, timeoutMs = 150_000): Promise<{ ok?: boolean | null; port?: number; pid?: number; error?: string } | null> {
  const file = join(restartDir(tmpDir), "state.json")
  const deadline = Date.now() + timeoutMs
  for (;;) {
    try {
      const st = JSON.parse(readFileSync(file, "utf8").replace(/^\uFEFF/, ""))
      if (st && st.ok !== null && st.ok !== undefined) return st
    } catch {
      /* 尚未写入 */
    }
    if (Date.now() >= deadline) return null
    await new Promise((r) => setTimeout(r, 1000))
  }
}

// Windows 部署器实机冒烟（跳过条件：非 win32 宿主）：Win11 24H2+ 移除 WMIC 后 wmic 不可用，
// 拉起器部署改用二段式 PowerShell Start-Process。
// 本用例**真跑全链路**：拉起器在随机空闲端口真拉起一个完整服务、真等 HTTP 就绪、真写 state.json。
// 因此收尾与隔离是硬要求——拉起器经 Start-Process 由独立 PowerShell 宿主创建（与服务/测试进程零亲缘），
// **不受测试进程 job object 约束**，测试结束不会自动回收：不主动 kill 就会留下「没有任何会话、
// 永远认为服务端空闲」的孤儿实例，抢跑真实实例的闲时待办与定时任务。
// 故：读 state.json 收掉新服务 PID（try/finally 保证断言失败也收）；环境显式隔离到用例临时目录。
;(process.platform === "win32" ? describe : describe.skip)("restart_server（Windows 部署器实机冒烟）", () => {
  test(
    "默认部署器（win32）：二段式 Start-Process 成功启动拉起器，不再依赖 WMIC",
    async () => {
      const tmpDir = mkdtempSync(join(tmpdir(), "restart-smoke-"))
      const entry = join(import.meta.dirname, "..", "..", "index.ts")
      const freePort = await pickFreePort()
      // 基线：用例开始前已存在的服务进程（如 dev 主进程），收尾兜底绝不误杀
      const preexisting = await servicePids(entry)
      const tool = makeRestartServerTool({
        tmpDir,
        entry,
        // 假 PID 立即满足「等旧进程退出」；拉起器在空闲端口上真启动一次服务——就绪后写成功状态（真链路验证）
        oldPid: -1,
        port: freePort,
        exitDelayMs: 60_000,
        exit: () => {},
        // 子进程环境隔离：家目录落用例临时目录（不碰真实 users/、待办、定时任务），
        // 后台副作用全关（只验证「能起、能就绪」），NODE_ENV=test 使子进程跳过仓库 .env 加载
        env: {
          NODE_ENV: "test",
          GEBAI_HOME: join(tmpDir, "home"),
          GEBAI_IDLE_TODO_ENABLED: "false",
          GEBAI_CRON_ENABLED: "false",
          GEBAI_FEISHU_BOT_ENABLED: "false",
          GEBAI_GC_DISABLED: "1",
        },
      })
      let pid = 0
      let ready = false
      try {
        const res = await tool.execute({ action: "restart" }, ctxStub(tmpdir()))
        expect(res.output).toContain("重启已布置")
        const state = await waitRestartState(tmpDir)
        expect(state).not.toBeNull()
        // ok=true = 新服务真监听该端口且 HTTP 200（拉起器 90s 就绪探测的结论）
        expect(state?.ok).toBe(true)
        expect(state?.port).toBe(freePort)
        pid = Number(state?.pid ?? 0)
        expect(pid).toBeGreaterThan(0)
        ready = true
        // state.json 记的 PID 就是该端口的实际监听者（复核拉起器的属主判定）
        expect(await portOwnerPid(freePort)).toBe(pid)
        expect(await portListening(freePort)).toBe(true)
      } finally {
        // 无条件回收：state.json 的 PID + 端口实际属主 + 本用例期间新出现的服务进程
        const owners = new Set<number>()
        if (pid > 0) owners.add(pid)
        if (ready) {
          const owner = await portOwnerPid(freePort).catch(() => 0)
          if (owner > 0) owners.add(owner)
        }
        for (const p of owners) await killProcessTree(p)
        await waitPortReleased(freePort)
        // 兜底：就绪失败时 state.json 没有 pid，按基线差集收掉本次新起的服务进程
        await killStrayServices(entry, preexisting)
        rmSync(tmpDir, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 })
      }
      // 验收点：端口已释放、无新增服务进程（断言失败时不会走到这里——cleanup 已完成）
      expect(await portListening(freePort)).toBe(false)
      expect(await killStrayServices(entry, preexisting)).toEqual([])
    },
    240_000,
  )
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
