/** restart_server：重启本歌白服务进程（仅本地模式注入，服务模式不注册）。
 *
 * 可靠性设计——「自杀后谁拉起」：重启的最大风险是旧进程死了新进程起不来（进程树连坐）。
 * 方案是外部拉起器彻底脱离服务进程树（Windows/Linux 双平台）：
 * 1. 工具执行时把拉起器脚本落盘到固定路径（`{tmpdir}/gebai-restart/`）并部署为独立进程：
 *    - Windows：PowerShell 脚本，二段式启动：先 `powershell Start-Process powershell -File …` 起一个
 *      独立窗口进程（与当前服务零亲缘），由它再执行拉起器脚本——服务退出/被杀不影响拉起器运行
 *      （WMIC 已废弃：Win11 24H2+ 默认移除，不可再用）；
 *    - Linux/macOS：bash 脚本，经 `setsid` 启动（新会话新进程组，防 kill -PGID 整杀连坐），
 *      标准流重定向 /dev/null——服务退出后拉起器被 init 收养，继续运行；
 * 2. 拉起器：等旧进程退出（60s）→ 等端口释放（30s；被第三方占用直接失败，不动无辜进程）→
 *    同端口/同 cwd/同关键环境变量启动新服务（输出重定向日志）→ 就绪探测（45s；端口属主 ≠ 旧 PID
 *    且 HTTP 200 双条件——单看 200 会在旧服务未死时误判）→ 写状态文件（成功=新 PID；失败=原因+日志尾部）；
 * 3. 工具先布置拉起器，再延迟退出当前进程（延迟内工具结果送达调用方——飞书/WS 回复先行，自杀在后）；
 * 4. 失败兜底：状态文件与日志都在 `{tmpdir}/gebai-restart/` 下，`status` 动作读取（不重启）。
 *
 * 平台差异：PowerShell 脚本必须带 UTF-8 BOM（Windows PowerShell 5.1 对无 BOM 文件按 ANSI 解析，
 * 中文注释/字符串会撕裂语法）；bash 脚本用 UTF-8 无 BOM。端口属主探测 Windows 用 Get-NetTCPConnection，
 * Linux 用 ss -ltnp（iproute2 基础组件，各发行版标配）。状态文件一律 UTF-8 无 BOM：PS5.1 的
 * Set-Content -Encoding UTF8 会写入 BOM，读方（status 动作）与消费方均按无 BOM 预期解析。
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
  /** 平台（win32=PowerShell 拉起器；其余=bash 拉起器）。 */
  platform: NodeJS.Platform
  /** 自杀延迟（工具结果送达窗口，毫秒）。 */
  exitDelayMs: number
  /** 进程退出（默认 process.exit；测试注入）。 */
  exit: (code: number) => void
  /** 拉起器部署（默认按平台 Start-Process / setsid；测试注入）。 */
  spawnLauncher: (scriptPath: string, platform: NodeJS.Platform) => Promise<{ ok: boolean; error?: string }>
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

// ---- Windows：PowerShell 拉起器 ----

/** PowerShell 单引号字符串内转义：' → ''。 */
function psq(s: string): string {
  return s.replace(/'/g, "''")
}

/** Windows 拉起器脚本：等旧进程退出 → 等端口释放 → 启动新服务 → 就绪探测 → 写状态文件。 */
export function buildLauncherScriptWin(deps: RestartDeps): string {
  const dir = restartDir(deps.tmpDir)
  const statePs = psq(join(dir, "state.json"))
  const logPs = psq(join(dir, "server.log"))
  // 探测路径：GEBAI_BASE_PATH 前缀时挂上（默认 /api/v1/sub-agents；leading / 从 BASE_PATH 补齐）
  const basePath = (deps.env.GEBAI_BASE_PATH || "").replace(/\/+$/, "")
  const probe = `${basePath}/api/v1/sub-agents`
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
    // 2) 再等端口释放（旧进程死了端口可能短暂 TIME_WAIT；TCP 探测）
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
    `  [IO.File]::WriteAllText('${statePs}', (@{ ok = $false; error = ('端口 ' + $port + ' 被其他进程(PID ' + $owner + ')占用，未启动新服务') } | ConvertTo-Json -Compress))`,
    "  exit 1",
    "}",
    // 3) 启动新服务（Start-Process：新进程脱离原服务进程树——拉起器由独立 PowerShell 宿主拉起、
    //    与原服务零亲缘；拉起器退出后 Windows 不连坐子进程（无 job object），新服务继续运行）
    `$env:GEBAI_PORT = '${String(deps.port)}'`,
    ...Object.entries(deps.env).filter(([k]) => k !== "GEBAI_PORT").map(([k, v]) => `$env:${k} = '${psq(v)}'`),
    `Start-Process -FilePath '${psq(process.execPath)}' ${deps.binary ? `` : `-ArgumentList 'run','${psq(deps.entry!)}' `}-WorkingDirectory '${psq(deps.cwd)}' -WindowStyle Hidden -RedirectStandardOutput '${logPs}.out' -RedirectStandardError '${logPs}.err' | Out-Null`,
    // 4) 等 HTTP 就绪 + 端口属主为新进程（只看 200 会误判——老服务未死时端口一直 200；
    //    属主必须存在且 ≠ 老进程 PID）
    "$deadline = (Get-Date).AddSeconds(90)",
    "$ready = $false",
    "$newPid = 0",
    "while ((Get-Date) -lt $deadline) {",
    "  $owner = (Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1).OwningProcess",
    "  if ($owner -and [int]$owner -ne $oldPid) {",
    `    try { $r = Invoke-WebRequest -Uri ('http://127.0.0.1:' + $port + '${probe}') -UseBasicParsing -TimeoutSec 3; if ($r.StatusCode -eq 200) { $ready = $true; $newPid = [int]$owner; break } } catch {}`,
    "  }",
    "  Start-Sleep -Milliseconds 800",
    "}",
    // 5) 写状态文件（成功=就绪；失败=日志尾部）
    "$tail = ''",
    `if (Test-Path '${logPs}.err') { $tail = (Get-Content '${logPs}.err' -Tail 20 -ErrorAction SilentlyContinue) -join [char]10 }`,
    "if ($ready) {",
    `  [IO.File]::WriteAllText('${statePs}', (@{ ok = $true; port = $port; pid = $newPid; note = '新服务已就绪' } | ConvertTo-Json -Compress))`,
    "} else {",
    `  [IO.File]::WriteAllText('${statePs}', (@{ ok = $false; error = '就绪超时（90s）'; logTail = $tail } | ConvertTo-Json -Compress))`,
    "}",
  ].join("\n")
}

/** Windows 默认拉起器部署：二段式 PowerShell Start-Process（WMIC 已废弃：Win11 24H2+ 默认移除，不可再用）。
 *
 * 不能直接 Bun.spawn("powershell -File launcher.ps1")：拉起器会成为服务的直接子进程，Bun 子进程经
 * job object 连坐——服务退出时拉起器一起被杀，重启必败。二段式：外层 powershell（服务的短暂子进程）
 * 经 Start-Process 启动内层 powershell 执行拉起器——Start-Process 创建的新进程不受父 job object
 * kill-on-close 连坐（中间的外层进程退出后链路断开），与原服务彻底零亲缘；外层确认内层已创建后
 * 立即退出（存活秒级），服务随后自杀不牵连任何人。
 */
async function startProcessSpawnLauncher(scriptPath: string): Promise<{ ok: boolean; error?: string }> {
  const proc = Bun.spawn(
    [
      "powershell",
      "-NoProfile",
      "-ExecutionPolicy",
      "Bypass",
      "-Command",
      `$p = Start-Process -FilePath powershell -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File','${psq(scriptPath)}') -WindowStyle Hidden -PassThru; if ($p) { exit 0 } else { exit 1 }`,
    ],
    { stdout: "pipe", stderr: "pipe", windowsHide: true },
  )
  const [out, err, code] = await Promise.all([new Response(proc.stdout).text(), new Response(proc.stderr).text(), proc.exited])
  if (code !== 0) {
    return { ok: false, error: `二段式 Start-Process 启动失败 (exit=${code}): ${(out + err).slice(-300)}` }
  }
  return { ok: true }
}

// ---- Linux/macOS：bash 拉起器 ----

/** bash 单引号字符串内转义：' → '\''。 */
function shq(s: string): string {
  return `'${s.replace(/'/g, "'\\''")}'`
}

/** Linux/macOS 拉起器脚本：等旧进程退出 → 等端口释放 → 启动新服务 → 就绪探测 → 写状态文件。 */
export function buildLauncherScriptPosix(deps: RestartDeps): string {
  const dir = restartDir(deps.tmpDir)
  const stateFile = join(dir, "state.json")
  const logOut = join(dir, "server.log.out")
  const logErr = join(dir, "server.log.err")
  const envExports = Object.entries(deps.env)
    .map(([k, v]) => `export ${k}=${shq(v)}`)
    .join("\n")
  const startCmd = deps.binary
    ? `${shq(process.execPath)}`
    : `${shq(process.execPath)} run ${shq(deps.entry!)}`
  return [
    "#!/usr/bin/env bash",
    "set -u",
    // 目录自建（不依赖部署方 mkdir——脚本自带全流程自包含）
    `mkdir -p ${shq(dir)}`,
    `port=${deps.port}`,
    `old_pid=${deps.oldPid}`,
    // 1) 等旧进程退出（kill -0 探测存在性；最长 60s）
    "deadline=$((SECONDS + 60))",
    "while kill -0 \"$old_pid\" 2>/dev/null; do",
    "  [ $SECONDS -ge $deadline ] && break",
    "  sleep 0.5",
    "done",
    // 2) 等端口释放（ss -ltn 探测监听；旧进程死后可能有短暂残留，最长 30s）
    "deadline=$((SECONDS + 30))",
    `while ss -ltn "sport = :$port" 2>/dev/null | grep -q LISTEN; do`,
    "  [ $SECONDS -ge $deadline ] && break",
    "  sleep 0.5",
    "done",
    // 端口仍被监听：占用者不是老进程（我们只等老进程死）——第三方抢注，启动必撞 EADDRINUSE；
    // 直接失败退出，不动无辜进程
    `if ss -ltn "sport = :$port" 2>/dev/null | grep -q LISTEN; then`,
    `  owner=$(ss -ltnp "sport = :$port" 2>/dev/null | grep -oP 'pid=\\K[0-9]+' | head -1)`,
    `  printf '{"ok":false,"error":"端口 %s 被其他进程(PID %s)占用，未启动新服务"}\\n' "$port" "\${owner:-unknown}" > ${shq(stateFile)}`,
    "  exit 1",
    "fi",
    // 3) 启动新服务（nohup + &：脱离终端挂断信号；拉起器经 setsid 已独立会话，退出后
    //    新服务由 init 收养继续运行）
    envExports,
    `cd ${shq(deps.cwd)} || { printf '{"ok":false,"error":"工作目录不存在"}\\n' > ${shq(stateFile)}; exit 1; }`,
    `nohup ${startCmd} > ${shq(logOut)} 2> ${shq(logErr)} &`,
    // 4) 就绪探测：端口属主存在且 ≠ 旧 PID，且 HTTP 200（单看 200 会在旧服务未死时误判）
    "deadline=$((SECONDS + 90))",
    "ready=0",
    "new_pid=0",
    "while [ $SECONDS -lt $deadline ]; do",
    `  owner=$(ss -ltnp "sport = :$port" 2>/dev/null | grep -oP 'pid=\\K[0-9]+' | head -1)`,
    `  if [ -n "$owner" ] && [ "$owner" != "$old_pid" ]; then`,
    `    if curl -sf -o /dev/null -m 3 "http://127.0.0.1:$port/api/v1/sub-agents"; then`,
    "      ready=1; new_pid=$owner; break",
    "    fi",
    "  fi",
    "  sleep 0.8",
    "done",
    // 5) 写状态文件（成功=就绪；失败=日志尾部）
    `tail_log() { tail -n 20 ${shq(logErr)} 2>/dev/null | tr '\\n' ' '; }`,
    'if [ "$ready" = "1" ]; then',
    `  printf '{"ok":true,"port":%s,"pid":%s,"note":"新服务已就绪"}\\n' "$port" "$new_pid" > ${shq(stateFile)}`,
    "else",
    `  printf '{"ok":false,"error":"就绪超时（90s）","logTail":"%s"}\\n' "$(tail_log)" > ${shq(stateFile)}`,
    "fi",
  ].join("\n")
}

/** Linux/macOS 默认拉起器部署：setsid bash（新会话新进程组，脱离服务进程组——防 kill -PGID 整杀连坐）。 */
async function setsidSpawnLauncher(scriptPath: string): Promise<{ ok: boolean; error?: string }> {
  const proc = Bun.spawn(["setsid", "bash", scriptPath], {
    stdin: "ignore",
    stdout: "ignore",
    stderr: "ignore",
  })
  // 拉起器必须长期存活（等旧进程退出最长 60s+）；部署方只验证它已进入运行态。
  // 不可 await proc.exited 阻塞等它退出（拉起器要等服务退出才结束，会死锁部署方）；
  // 用短窗口内进程是否已退出的窄探测判定「启动即崩」的失败，存活即返回成功。
  await new Promise((r) => setTimeout(r, 150))
  const exitedEarly = proc.exitCode !== null || proc.signalCode !== null
  if (exitedEarly) {
    const code = proc.exitCode ?? proc.signalCode ?? -1
    if (code === 0) return { ok: true } // 瞬间成功退出：脚本内容异常（应有等待阶段），不信任，按失败处理
    return { ok: false, error: `setsid 启动失败 (exit=${code})：bash 或 setsid 不可用` }
  }
  return { ok: true }
}

// ---- 通用部署与工具 ----

/** 按平台生成拉起器脚本内容。 */
export function buildLauncherScript(deps: RestartDeps): string {
  return deps.platform === "win32" ? buildLauncherScriptWin(deps) : buildLauncherScriptPosix(deps)
}

/** 部署拉起器：脚本落盘（Windows 带 UTF-8 BOM）+ 平台对应方式启动（脱离进程树）。 */
async function deployLauncher(deps: RestartDeps): Promise<{ ok: boolean; error?: string }> {
  const dir = restartDir(deps.tmpDir)
  await mkdir(dir, { recursive: true })
  const isWin = deps.platform === "win32"
  const script = join(dir, isWin ? "launcher.ps1" : "launcher.sh")
  const content = buildLauncherScript(deps)
  await writeFile(script, isWin ? "\uFEFF" + content : content, "utf8")
  if (!isWin) {
    const { chmod } = await import("node:fs/promises")
    await chmod(script, 0o755).catch(() => {})
  }
  return deps.spawnLauncher(script, deps.platform)
}

/** 默认拉起器部署（按平台分发）。 */
async function defaultSpawnLauncher(scriptPath: string, platform: NodeJS.Platform): Promise<{ ok: boolean; error?: string }> {
  return platform === "win32" ? startProcessSpawnLauncher(scriptPath) : setsidSpawnLauncher(scriptPath)
}

/** 读取最近一次重启状态（status 动作）。 */
async function readState(deps: Pick<RestartDeps, "tmpDir">): Promise<ToolResult> {
  const stateFile = join(restartDir(deps.tmpDir), "state.json")
  try {
    const raw = await readFile(stateFile, "utf8")
    return { output: `最近一次重启状态：\n${raw.trim().replace(/^\uFEFF/, "")}` }
  } catch {
    return { output: `尚无重启记录（state.json 不存在）——从未执行过重启，或 ${restartDir()} 被清理。` }
  }
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
    spawnLauncher: defaultSpawnLauncher,
    ...overrides,
  }
  return {
    name: "restart_server",
    description:
      "重启本歌白服务进程（仅本地模式可用，Windows/Linux/macOS）。执行后当前连接（飞书/Web）会短暂中断，几秒后自动恢复——外部拉起器（独立于服务进程树）等待旧进程退出与端口释放，再以同端口/同配置启动新服务并确认就绪；结果写入系统临时目录 gebai-restart/state.json，日志在同目录 server.log.*。action=status 查看最近一次重启状态（不重启）。服务模式（多用户部署）不提供本工具。",
    parameters: schema({
      action: { type: "string", enum: ["restart", "status"], description: "restart=执行重启（默认）；status=只读最近一次重启状态" },
    }),
    requiresApproval: true,
    async execute(args): Promise<ToolResult> {
      if (args.action === "status") return readState(deps)
      // 状态文件先写「进行中」（旧内容清除；重启失败时拉起器覆盖为失败原因）
      const dir = restartDir(deps.tmpDir)
      await mkdir(dir, { recursive: true })
      await writeFile(join(dir, "state.json"), JSON.stringify({ ok: null, note: "重启进行中", oldPid: deps.oldPid, at: new Date().toISOString() }), "utf8")
      const deployed = await deployLauncher(deps)
      if (!deployed.ok) {
        return { output: `拉起器部署失败，未执行重启（服务仍在运行）：${deployed.error ?? "未知错误"}` }
      }
      // 先送达本回复再退出：延迟自杀窗口内引擎已完成本轮工具结果回传
      const output =
        `重启已布置：外部拉起器（独立进程）已启动，${deps.exitDelayMs}ms 后当前进程退出（PID ${deps.oldPid}）。` +
        `新服务将以端口 ${deps.port} 拉起，就绪探测最长 90s。` +
        `恢复后可用 restart_server(action="status") 或查看 ${dir}${deps.platform === "win32" ? "\\state.json" : "/state.json"} 确认结果。`
      setTimeout(() => deps.exit(0), deps.exitDelayMs)
      return { output }
    },
  }
}

export const globalTools = [
  // 仅本地模式注入（compose 按 config.auth 过滤）；工具体内不再重复判定（注册即本地）
  { name: "restart_server", tool: () => makeRestartServerTool() },
]
