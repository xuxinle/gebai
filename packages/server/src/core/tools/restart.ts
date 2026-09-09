/** restart_server：重启本歌白服务进程（仅本地模式注入，服务模式不注册）。
 *
 * 可靠性设计——「自杀后谁拉起」：重启的最大风险是旧进程死了新进程起不来（进程树连坐）。
 * 方案是外部拉起器彻底脱离服务进程树：
 * 1. 工具执行时把拉起器脚本（PowerShell）落盘到固定路径（%TEMP%/gebai-restart/），
 *    经 `wmic process call create` 启动——拉起器父进程是 WMI 宿主（Win32_Process provider），
 *    与服务进程零亲缘，服务退出/被杀不影响拉起器运行；
 * 2. 拉起器：等旧端口释放（最长 60s）→ 同端口/同 cwd/同关键环境变量启动新服务（独立 cmd 进程，
 *    输出重定向日志）→ 轮询 HTTP 就绪（最长 45s）→ 写状态文件（成功=新 PID；失败=原因+日志尾部）；
 * 3. 工具先布置拉起器（含新进程命令行），再延迟退出当前进程（延迟内工具结果送达调用方——
 *    飞书/WS 回复先行，自杀在后）；
 * 4. 失败兜底：端口被第三方占用/启动崩溃等，状态文件与日志都在 %TEMP%/gebai-restart/ 下，
 *    用户或下一次会话可查（`status` 动作读取状态文件，不重启）。
 *
 * Windows 专用（wmic 拉起器）；非 Windows 本地模式返回不可用说明（本地模式主力场景为 Windows 桌面）。
 */
import { mkdir, readFile, writeFile } from "node:fs/promises"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { Tool, ToolResult } from "../base/types"
import { isBinaryMode } from "../base/config"
import { schema } from "./shared"

/** 重启工作目录（拉起器脚本/状态文件/新服务日志）。 */
export function restartDir(tmp: string = tmpdir()): string {
  return join(tmp, "gebai-restart")
}

export interface RestartDeps {
  /** 新服务启动端口（默认当前服务端口）。 */
  port: number
  /** 新服务工作目录（dev=packages/server；binary 模式无意义但保持记录）。 */
  cwd: string
  /** 服务入口（dev=src/index.ts 绝对路径；binary=null 用 process.execPath 自启动）。 */
  entry: string | null
  /** 二进制模式（bun --compile 单文件：spawn 自身可执行文件）。 */
  binary: boolean
  /** 旧进程 PID（拉起器等待其退出）。 */
  oldPid: number
  /** 拉起器/状态文件目录。 */
  tmpDir: string
  /** 关键环境变量（新进程须继承：端口/家目录/监听地址等启动级配置）。 */
  env: Record<string, string>
  /** 平台（win32 才可用 wmic 拉起器）。 */
  platform: NodeJS.Platform
  /** 自杀延迟（工具结果送达窗口，毫秒）。 */
  exitDelayMs: number
  /** 进程退出（默认 process.exit；测试注入）。 */
  exit: (code: number) => void
  /** 拉起器部署（默认 wmic；测试注入）。 */
  spawnLauncher: (cmdline: string) => Promise<{ ok: boolean; error?: string }>
}

/** 构造新服务启动命令行（cmd /c 包装：环境变量前缀 + cd + 启动 + 日志重定向，独立进程语义）。 */
export function buildNewServerCommand(deps: RestartDeps): string {
  const log = join(restartDir(deps.tmpDir), "server.log")
  const envPrefix = Object.entries(deps.env)
    .map(([k, v]) => `set "${k}=${v}"&&`)
    .join("")
  const cd = `cd /d "${deps.cwd}"&&`
  const start = deps.binary
    ? `"${process.execPath}" > "${log}" 2>&1`
    : `"${process.execPath}" run "${deps.entry}" > "${log}" 2>&1`
  return `cmd /c ${envPrefix}${cd}${start}`
}

/** 拉起器 PowerShell 脚本：等端口释放 → 启动新服务（wmic 脱离）→ 等就绪 → 写状态文件。 */
export function buildLauncherScript(deps: RestartDeps): string {
  const dir = restartDir(deps.tmpDir)
  // PowerShell 单引号字符串内转义：' → ''
  const psq = (s: string) => s.replace(/'/g, "''")
  const statePs = psq(join(dir, "state.json"))
  const logPs = psq(join(dir, "server.log"))
  return [
    "$ErrorActionPreference = 'Continue'",
    `$port = ${deps.port}`,
    `$oldPid = ${deps.oldPid}`,
    // 1) 等旧进程退出（进程不存在即释放；最长 60s）
    "$deadline = (Get-Date).AddSeconds(60)",
    "while ((Get-Date) -lt $deadline) {",
    "  $alive = $false",
    "  try { Get-Process -Id $oldPid -ErrorAction Stop | Out-Null; $alive = $true } catch {}",
    "  if (-not $alive) { break }",
    "  Start-Sleep -Milliseconds 500",
    "}",
    // 2) 再等端口释放（旧进程死了端口可能短暂 TIME_WAIT/TIME_WAIT 边缘；TCP 探测）
    "$deadline = (Get-Date).AddSeconds(30)",
    "while ((Get-Date) -lt $deadline) {",
    "  $open = $false",
    "  try { $c = New-Object Net.Sockets.TcpClient; $r = $c.BeginConnect('127.0.0.1', $port, $null, $null); $open = $r.AsyncWaitHandle.WaitOne(500); if ($open) { $c.EndConnect($r) }; $c.Close() } catch { $open = $false }",
    "  if (-not $open) { break }",
    "  Start-Sleep -Milliseconds 500",
    "}",
    // 端口仍被占用：占用者不是老进程（我们只杀老进程）——可能是第三方服务抢注，启动必撞 EADDRINUSE；
    // 直接失败退出，不动老服务之外的东西（老进程没死的场景在阶段 1 已等过）
    "$owner = (Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1).OwningProcess",
    "if ($owner) {",
    `  @{ ok = $false; error = ('端口 ' + $port + ' 被其他进程(PID ' + $owner + ')占用，未启动新服务') } | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 '${statePs}'`,
    "  exit 1",
    "}",
    // 3) 启动新服务（Start-Process cmd：新进程脱离原服务进程树——拉起器由 wmic 拉起、与原服务
    //    零亲缘；拉起器退出后 Windows 不连坐子进程（无 job object），新服务继续运行）
    `$env:GEBAI_PORT = '${String(deps.port)}'`,
    ...Object.entries(deps.env).filter(([k]) => k !== "GEBAI_PORT").map(([k, v]) => `$env:${k} = '${psq(v)}'`),
    `Start-Process -FilePath '${psq(process.execPath)}' ${deps.binary ? `` : `-ArgumentList 'run','${psq(deps.entry!)}' `}-WorkingDirectory '${psq(deps.cwd)}' -WindowStyle Hidden -RedirectStandardOutput '${logPs}.out' -RedirectStandardError '${logPs}.err' | Out-Null`,
    // 4) 等 HTTP 就绪 + 端口属主为新进程（只看 200 会误判——老服务未死时端口一直 200；
    //    属主必须存在且 ≠ 老进程 PID）
    "$deadline = (Get-Date).AddSeconds(45)",
    "$ready = $false",
    "$newPid = 0",
    "while ((Get-Date) -lt $deadline) {",
    "  $owner = (Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1).OwningProcess",
    "  if ($owner -and [int]$owner -ne $oldPid) {",
    "    try { $r = Invoke-WebRequest -Uri ('http://127.0.0.1:' + $port + '/api/v1/sub-agents') -UseBasicParsing -TimeoutSec 3; if ($r.StatusCode -eq 200) { $ready = $true; $newPid = [int]$owner; break } } catch {}",
    "  }",
    "  Start-Sleep -Milliseconds 800",
    "}",
    // 5) 写状态文件（成功=就绪；失败=日志尾部）
    "$tail = ''",
    `if (Test-Path '${logPs}.err') { $tail = (Get-Content '${logPs}.err' -Tail 20 -ErrorAction SilentlyContinue) -join [char]10 }`,
    "if ($ready) {",
    `  @{ ok = $true; port = $port; pid = $newPid; note = '新服务已就绪' } | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 '${statePs}'`,
    "} else {",
    `  @{ ok = $false; error = '就绪超时（45s）'; logTail = $tail } | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 '${statePs}'`,
    "}",
  ].join("\n")
}

/** 部署拉起器：脚本落盘 + wmic 启动（脱离进程树）。 */
async function deployLauncher(deps: RestartDeps): Promise<{ ok: boolean; error?: string }> {
  const dir = restartDir(deps.tmpDir)
  await mkdir(dir, { recursive: true })
  const script = join(dir, "launcher.ps1")
  // UTF-8 BOM：Windows PowerShell 5.1 对无 BOM 文件按 ANSI 解析，脚本内中文会撕裂字符串（语法错误）
  await writeFile(script, "\uFEFF" + buildLauncherScript(deps), "utf8")
  // wmic 启动 PowerShell（拉起器父进程=WMI 宿主，与服务进程零亲缘）
  const cmdline = `powershell -NoProfile -ExecutionPolicy Bypass -File "${script}"`
  return deps.spawnLauncher(cmdline)
}

/** 读取最近一次重启状态（status 动作）。 */
async function readState(deps: Pick<RestartDeps, "tmpDir">): Promise<ToolResult> {
  const stateFile = join(restartDir(deps.tmpDir), "state.json")
  try {
    const raw = await readFile(stateFile, "utf8")
    return { output: `最近一次重启状态：\n${raw.trim()}` }
  } catch {
    return { output: "尚无重启记录（state.json 不存在）——从未执行过重启，或 %TEMP%/gebai-restart/ 被清理。" }
  }
}

/** 默认拉起器部署：wmic process call create。 */
async function wmicSpawnLauncher(cmdline: string): Promise<{ ok: boolean; error?: string }> {
  const proc = Bun.spawn(["wmic", "process", "call", "create", cmdline], {
    stdout: "pipe",
    stderr: "pipe",
    windowsHide: true,
  })
  const out = await new Response(proc.stdout).text()
  await proc.exited
  if (!/ReturnValue = 0;?/s.test(out) && !/ReturnValue\s*=\s*0/.test(out)) {
    return { ok: false, error: `wmic 返回异常: ${out.slice(-300)}` }
  }
  return { ok: true }
}

/** 构造 restart_server 工具（依赖注入便于测试）。 */
export function makeRestartServerTool(overrides: Partial<RestartDeps> = {}): Tool {
  const deps: RestartDeps = {
    port: Number(process.env.GEBAI_PORT || 3000),
    cwd: process.cwd(),
    entry: join(import.meta.dirname, "..", "..", "index.ts"),
    binary: isBinaryMode(),
    oldPid: process.pid,
    tmpDir: tmpdir(),
    env: pickRestartEnv(),
    platform: process.platform,
    exitDelayMs: 2500,
    exit: (code) => process.exit(code),
    spawnLauncher: wmicSpawnLauncher,
    ...overrides,
  }
  return {
    name: "restart_server",
    description:
      "重启本歌白服务进程（仅本地模式可用）。执行后当前连接（飞书/Web）会短暂中断，几秒后自动恢复——外部拉起器（独立于服务进程树）等待旧进程退出与端口释放，再以同端口/同配置启动新服务并确认就绪；结果写入 %TEMP%/gebai-restart/state.json，日志在同目录 server.log。action=status 查看最近一次重启状态（不重启）。服务模式（多用户部署）不提供本工具。",
    parameters: schema({
      action: { type: "string", enum: ["restart", "status"], description: "restart=执行重启（默认）；status=只读最近一次重启状态" },
    }),
    requiresApproval: true,
    async execute(args): Promise<ToolResult> {
      if (args.action === "status") return readState(deps)
      if (deps.platform !== "win32") {
        return { output: "restart_server 当前仅支持 Windows（wmic 外部拉起器）。非 Windows 本地模式请手动重启服务。" }
      }
      // 状态文件先写「进行中」（旧内容清除；重启失败时拉起器覆盖为失败原因）
      const dir = restartDir(deps.tmpDir)
      await mkdir(dir, { recursive: true })
      await writeFile(join(dir, "state.json"), JSON.stringify({ ok: null, note: "重启进行中", oldPid: deps.oldPid, at: new Date().toISOString() }), "utf8")
      const deployed = await deployLauncher(deps)
      if (!deployed.ok) {
        return { output: `拉起器部署失败，未执行重启（服务仍在运行）：${deployed.error ?? "未知错误"}` }
      }
      // 先送达本回复再退出：延迟自杀窗口内引擎已完成本轮工具结果回传
      const { output } = {
        output:
          `重启已布置：外部拉起器（独立进程）已启动，${deps.exitDelayMs}ms 后当前进程退出（PID ${deps.oldPid}）。` +
          `新服务将以端口 ${deps.port} 拉起，就绪探测最长 45s。` +
          `恢复后可用 restart_server(action="status") 或查看 %TEMP%/gebai-restart/state.json 确认结果。`,
      }
      setTimeout(() => deps.exit(0), deps.exitDelayMs)
      return { output }
    },
  }
}

/** 新进程须继承的启动级环境变量（端口/家目录/监听地址/模式；凭据类不复制——.env 由 loadConfig 自行加载）。 */
export function pickRestartEnv(env: Record<string, string | undefined> = process.env): Record<string, string> {
  const keys = ["GEBAI_PORT", "GEBAI_HOST", "GEBAI_HOME", "GEBAI_MODE", "GEBAI_BASE_PATH"]
  const out: Record<string, string> = {}
  for (const k of keys) {
    const v = env[k]
    if (v) out[k] = v
  }
  return out
}

export const globalTools = [
  // 仅本地模式注入（compose 按 config.auth 过滤）；工具体内不再重复判定（注册即本地）
  { name: "restart_server", tool: () => makeRestartServerTool() },
]
