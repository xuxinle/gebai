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
import { statSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { Tool, ToolResult } from "../base/types"
import { isBinaryMode } from "../base/config"
import { schema } from "./shared"
import { log as logger } from "@gebai/sdk/node"

/** 重启工作目录（拉起器脚本/状态文件/续跑请求/新服务日志）。 */
export function restartDir(tmp: string = tmpdir()): string {
  return join(tmp, "gebai-restart")
}

/** 续跑请求（restart_server 的 `prompt` 参数）：旧进程自杀前落盘，新服务启动后**消费一次**——
 *  把提示词作为用户消息注入原会话并触发新一轮运行（模型据此继续工作，无需用户重新发消息）。 */
export interface RestartContinuation {
  /** 目标会话（缺省为调用工具时的当前会话）。 */
  sessionId: string
  /** 会话属主用户。 */
  user: string
  /** 发起用户角色（admin/user；续跑任务的公共资源权限判定用）。 */
  role?: string
  /** 重启就绪后自动注入会话的用户提示词（原样落盘为 user 消息）。 */
  prompt: string
  /** 写盘时刻（ms）：新服务据此判有效期（过期不执行，防陈旧请求被无关启动误消费）。 */
  at: number
  /** 旧服务 PID（诊断用）。 */
  oldPid: number
}

/** 续跑执行结果（新服务消费后写入 continue.result.json，status 动作展示）。
 *  `ok: null` = 已开始执行、任务尚在跑（先写「执行中」再跑、完成后覆盖——任务中途进程再被重启也留下痕迹）。 */
export interface ContinuationResult {
  ok: boolean | null
  sessionId: string
  /** 「本次启动由重启拉起器承接」的确认结论：confirmed=state.json 的 pid 匹配本进程 /
   *  failed=拉起器已判失败（人工拉起兜底）/ timeout=等待超时未确认。 */
  confirm?: "confirmed" | "failed" | "timeout"
  promptChars?: number
  /** 附注（status 展示；如「续跑执行中」）。 */
  note?: string
  /** 未执行的原因（过期/会话不存在等）。 */
  reason?: string
  /** 执行异常（engine.run 抛错）。 */
  error?: string
  at: number
}

/** 续跑请求文件路径。 */
export function continuationFile(tmp: string = tmpdir()): string {
  return join(restartDir(tmp), "continue.json")
}

/** 续跑执行结果文件路径。 */
export function continuationResultFile(tmp: string = tmpdir()): string {
  return join(restartDir(tmp), "continue.result.json")
}

/** 写续跑请求：**在部署拉起器之前**调用——写失败即中止本次重启（不留「重启成功但续跑指令丢失」的半成品）。 */
export async function writeContinuation(req: RestartContinuation, tmp: string = tmpdir()): Promise<void> {
  await mkdir(restartDir(tmp), { recursive: true })
  await writeFile(continuationFile(tmp), JSON.stringify(req), "utf8")
}

/** 读续跑请求（不存在/损坏返回 null）。 */
export async function readContinuation(tmp: string = tmpdir()): Promise<RestartContinuation | null> {
  try {
    const raw = await readFile(continuationFile(tmp), "utf8")
    const parsed = JSON.parse(raw.replace(/^\uFEFF/, "")) as RestartContinuation
    if (!parsed || typeof parsed.sessionId !== "string" || typeof parsed.user !== "string" || typeof parsed.prompt !== "string") return null
    return parsed
  } catch {
    return null
  }
}

/** 删除续跑请求（消费一次性语义：先删再执行，并发/重复启动不会双跑）。 */
export async function removeContinuation(tmp: string = tmpdir()): Promise<void> {
  const { unlink } = await import("node:fs/promises")
  await unlink(continuationFile(tmp)).catch(() => {})
}

/** 写续跑执行结果（覆盖式：只保留最近一次）。 */
export async function writeContinuationResult(res: ContinuationResult, tmp: string = tmpdir()): Promise<void> {
  await mkdir(restartDir(tmp), { recursive: true })
  await writeFile(continuationResultFile(tmp), JSON.stringify(res), "utf8").catch(() => {})
}

/** 读续跑执行结果（无记录返回 null）。 */
export async function readContinuationResult(tmp: string = tmpdir()): Promise<ContinuationResult | null> {
  try {
    return JSON.parse((await readFile(continuationResultFile(tmp), "utf8")).replace(/^\uFEFF/, "")) as ContinuationResult
  } catch {
    return null
  }
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
  /** 当前进程是否 dev-reload 模式（`--reload` 参数或 GEBAI_DEV_RELOAD=1）：**argv 参数不会被拉起器
   *  复制**（新进程固定 `bun run <entry>`），重启时须改经环境变量传给新进程——否则前端构建 watch
   *  （vite build --watch）与页面热刷新通道在重启后永久丢失，直到再次手工以 --reload 启动。 */
  devReload: boolean
  /** 进程退出（默认 process.exit；测试注入）。 */
  exit: (code: number) => void
  /** 拉起器部署（默认按平台 Start-Process / setsid；测试注入）。 */
  spawnLauncher: (scriptPath: string, platform: NodeJS.Platform) => Promise<{ ok: boolean; error?: string }>
}

/** 新进程须继承的启动级环境变量（端口/家目录/监听地址/模式；凭据类不复制——.env 由 loadConfig 自行加载）。 */
export function pickRestartEnv(env: Record<string, string | undefined> = process.env): Record<string, string> {
  const keys = ["GEBAI_PORT", "GEBAI_HOST", "GEBAI_HOME", "GEBAI_MODE", "GEBAI_BASE_PATH", "GEBAI_DEV_RELOAD"]
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
    // 日志轮转：Start-Process 的 -RedirectStandardOutput 为**覆盖写**，连续重启会吞掉上一次的诊断日志
    `if (Test-Path '${logPs}.out') { Move-Item '${logPs}.out' '${logPs}.out.prev' -Force }`,
    `if (Test-Path '${logPs}.err') { Move-Item '${logPs}.err' '${logPs}.err.prev' -Force }`,
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
    // 端口仍被占用：**区分「真被其他进程抢占」与「僵尸套接字」**——端口释放的判据是 socket 句柄
    // 引用计数归零，不是「拥有者进程存在」：句柄被其他进程继承时，进程已消失而端口仍 LISTENING
    // （netstat 显示 PID / tasklist 查不到 / taskkill 报找不到进程，用户空间无法释放）。
    // 两种情形都严格失败（不动无辜进程、不自动换端口），但必须报准原因——否则用户只能猜。
    "$owner = (Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1).OwningProcess",
    "if ($owner) {",
    "  $ownerAlive = $false",
    "  try { Get-Process -Id $owner -ErrorAction Stop | Out-Null; $ownerAlive = $true } catch {}",
    "  $tail = ''",
    // 中止即还原轮转掉的诊断日志（成功路径才需要让位给新日志）：**必须先还原再读**——
    // 文件在脚本开头已被轮转为 .prev，直接读 .err 会拿到空值，使 logTail 恒为空
    `  if (Test-Path '${logPs}.out.prev') { Move-Item '${logPs}.out.prev' '${logPs}.out' -Force }`,
    `  if (Test-Path '${logPs}.err.prev') { Move-Item '${logPs}.err.prev' '${logPs}.err' -Force }`,
    `  if (Test-Path '${logPs}.err') { $tail = (Get-Content '${logPs}.err' -Tail 20 -Encoding UTF8 -ErrorAction SilentlyContinue) -join [char]10 }`,
    "  if ($ownerAlive) {",
    `    [IO.File]::WriteAllText('${statePs}', (@{ ok = $false; reason = 'occupied'; port = $port; ownerPid = [int]$owner; error = ('端口 ' + $port + ' 被其他进程(PID ' + $owner + ')占用，未启动新服务'); logTail = $tail } | ConvertTo-Json -Compress))`,
    "  } else {",
    `    [IO.File]::WriteAllText('${statePs}', (@{ ok = $false; reason = 'zombie-socket'; port = $port; ownerPid = [int]$owner; error = ('端口 ' + $port + ' 被僵尸套接字占用：属主进程 PID ' + $owner + ' 已不存在，但端口仍 LISTENING（socket 句柄被其他进程持有、引用计数未归零）；用户空间无法释放该端口'); logTail = $tail } | ConvertTo-Json -Compress))`,
    "  }",
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
    `if (Test-Path '${logPs}.err') { $tail = (Get-Content '${logPs}.err' -Tail 20 -Encoding UTF8 -ErrorAction SilentlyContinue) -join [char]10 }`,
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
    // 日志轮转：直接覆盖会吞掉上一次重启的诊断日志（保留 .prev）
    `mv -f ${shq(logOut)} ${shq(`${logOut}.prev`)} 2>/dev/null || true`,
    `mv -f ${shq(logErr)} ${shq(`${logErr}.prev`)} 2>/dev/null || true`,
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
    // 端口仍被监听：同样区分「真占用」与「监听记录陈旧（属主已不存在）」——两情形都严格失败，
    // 但原因要报准（Linux 下进程退出时内核会关闭其 fd、端口随之释放，此情形应属罕见）
    `if ss -ltn "sport = :$port" 2>/dev/null | grep -q LISTEN; then`,
    `  owner=$(ss -ltnp "sport = :$port" 2>/dev/null | grep -oP 'pid=\\K[0-9]+' | head -1)`,
    '  owner_alive=0',
    '  if [ -n "$owner" ] && kill -0 "$owner" 2>/dev/null; then owner_alive=1; fi',
    // 中止即还原轮转掉的诊断日志（与 Windows 同口径），再读其尾部供 state 展示
    `  mv -f ${shq(`${logOut}.prev`)} ${shq(logOut)} 2>/dev/null || true`,
    `  mv -f ${shq(`${logErr}.prev`)} ${shq(logErr)} 2>/dev/null || true`,
    '  tail_log() { tail -n 20 ' + shq(logErr) + ' 2>/dev/null | tr "\\n" " "; }',
    '  if [ "$owner_alive" = "1" ]; then',
    `    printf '{"ok":false,"reason":"occupied","port":%s,"ownerPid":"%s","error":"端口 %s 被其他进程(PID %s)占用，未启动新服务","logTail":"%s"}\\n' "$port" "\${owner:-unknown}" "$port" "\${owner:-unknown}" "$(tail_log)" > ${shq(stateFile)}`,
    '  else',
    `    printf '{"ok":false,"reason":"zombie-socket","port":%s,"ownerPid":"%s","error":"端口 %s 仍处于 LISTENING 但属主进程 %s 已不存在（陈旧监听记录）；用户空间无法释放该端口","logTail":"%s"}\\n' "$port" "\${owner:-unknown}" "$port" "\${owner:-unknown}" "$(tail_log)" > ${shq(stateFile)}`,
    '  fi',
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
    // 5) 写状态文件（成功=就绪；失败=日志尾部）——tail_log 已在端口占用分支前定义
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

/** 读 state.json（拉起器写入的重启结论；不存在/损坏返回 null）。 */
async function readStateFile(tmpDir: string): Promise<{ ok?: boolean | null; pid?: number; error?: string } | null> {
  try {
    return JSON.parse((await readFile(join(restartDir(tmpDir), "state.json"), "utf8")).replace(/^\uFEFF/, ""))
  } catch {
    return null
  }
}

/** 续跑消费依赖（测试注入全部可选，便于离线验证判定分支）。 */
export interface ConsumeContinuationDeps {
  /** 当前进程 PID（与 state.json 的 pid 比对，确认「本次启动由重启拉起器承接」）。 */
  pid: number
  /** 实际触发续跑（装配处包装 engine.run：把 req.prompt 作为 user 消息注入 req.sessionId 并跑一轮）。 */
  run: (req: RestartContinuation) => Promise<void>
  tmpDir?: string
  /** 会话存在性校验（缺省跳过）。 */
  sessionExists?: (sessionId: string, user: string) => Promise<boolean>
  /** 等 state.json 确认的最长时间（ms，默认 25000）。 */
  waitStateMs?: number
  /** 轮询间隔（ms，默认 500）。 */
  pollMs?: number
  /** 请求有效期（ms，默认 10 分钟）。 */
  maxAgeMs?: number
  now?: () => number
  sleep?: (ms: number) => Promise<void>
  log?: (msg: string) => void
}

export interface ConsumeContinuationOutcome {
  consumed: boolean
  reason: string
  confirm?: "confirmed" | "failed" | "timeout"
}

/**
 * 消费续跑请求（新服务启动后调用的非阻塞后台任务）：把旧进程留下的提示词注入原会话继续执行。
 * 确认与兜底：
 * - state.json 的 pid === 本进程 → confirmed（正常路径：本次启动确由重启拉起器承接）；
 * - 拉起器已写失败结论（ok=false）→ failed：本进程非它拉起（人工恢复/桌面外壳），**仍执行**——
 *   提示词是调用方的明确意图，「重启失败后人工恢复」正是最需要它不丢的场景；
 * - 等待超时未确认 → timeout：同样执行（覆盖状态文件写入失败/延迟的极端情形）；
 * - 过期（> maxAgeMs）→ 丢弃不执行（防陈旧请求被无关启动误消费）；
 * - **先删请求文件再执行**（一次性语义：并发/重复启动不会双跑）；结论写 continue.result.json
 *   供 restart_server(action="status") / server.log 确认。
 */
export async function consumeRestartContinuation(deps: ConsumeContinuationDeps): Promise<ConsumeContinuationOutcome> {
  const tmp = deps.tmpDir ?? tmpdir()
  const now = deps.now ?? (() => Date.now())
  const sleep = deps.sleep ?? ((ms: number) => new Promise<void>((r) => setTimeout(r, ms)))
  const log = deps.log ?? ((m: string) => logger.info(`[restart] ${m}`))
  const req = await readContinuation(tmp)
  if (!req) return { consumed: false, reason: "无续跑请求" }
  const age = now() - req.at
  if (!Number.isFinite(age) || age > (deps.maxAgeMs ?? 10 * 60_000) || age < -60_000) {
    await removeContinuation(tmp)
    await writeContinuationResult({ ok: false, sessionId: req.sessionId, reason: "续跑请求已过期，未执行", at: now() }, tmp)
    log(`续跑请求已过期（${Math.round(age / 1000)}s），丢弃`)
    return { consumed: false, reason: "已过期" }
  }
  // 等 state.json 确认：就绪探测完成后由拉起器写入，通常在服务监听后 1~3s 内到达
  const waitMs = deps.waitStateMs ?? 25_000
  const pollMs = deps.pollMs ?? 500
  const deadline = now() + waitMs
  let confirm: "confirmed" | "failed" | "timeout" = "timeout"
  let detail = "等待 state.json 确认超时"
  for (;;) {
    const st = await readStateFile(tmp)
    if (st) {
      if (st.pid === deps.pid) {
        confirm = "confirmed"
        detail = "state.json 的 pid 为本进程"
        break
      }
      if (st.ok === false) {
        confirm = "failed"
        detail = st.error ?? "拉起器判定重启失败"
        break
      }
    }
    if (now() >= deadline) break
    await sleep(pollMs)
  }
  await removeContinuation(tmp)
  log(`续跑请求（会话 ${req.sessionId}，${req.prompt.length} 字）：确认=${confirm}（${detail}）`)
  if (deps.sessionExists && !(await deps.sessionExists(req.sessionId, req.user).catch(() => false))) {
    await writeContinuationResult({ ok: false, sessionId: req.sessionId, confirm, reason: "会话不存在，续跑未执行", at: now() }, tmp)
    log(`续跑未执行：会话 ${req.sessionId} 不存在`)
    return { consumed: false, reason: "会话不存在", confirm }
  }
  try {
    // 先写「执行中」再跑：续跑任务可能一直跑到下次重启（本用例：任务本身就含下一次重启）——
    // 不先写则在任务中途进程被杀时完全无痕迹，status 看不到任何续跑记录
    await writeContinuationResult({ ok: null, sessionId: req.sessionId, confirm, note: "续跑执行中", promptChars: req.prompt.length, at: now() }, tmp)
    await deps.run(req)
    await writeContinuationResult({ ok: true, sessionId: req.sessionId, confirm, promptChars: req.prompt.length, at: now() }, tmp)
    log(`续跑完成：会话 ${req.sessionId}`)
    return { consumed: true, reason: "已续跑", confirm }
  } catch (err) {
    const msg = String((err as Error).message || err).slice(0, 500)
    await writeContinuationResult({ ok: false, sessionId: req.sessionId, confirm, error: msg, at: now() }, tmp)
    log(`续跑失败：${msg}`)
    return { consumed: false, reason: `续跑失败: ${msg}`, confirm }
  }
}

/**
 * 重启失败原因的处置说明（status 展示用；state.json 只存结构化短字段，长文案在此展开）。
 * 区分「真被占用」与「僵尸套接字」是必要的：两者处置方式完全不同，而现象都是「端口起不来」。
 */
export function explainRestartState(state: Record<string, unknown>): string[] {
  const reason = String(state.reason ?? "")
  const pid = state.ownerPid !== undefined ? String(state.ownerPid) : "未知"
  const port = state.port !== undefined ? String(state.port) : "未知"
  const lines: string[] = []
  if (reason === "occupied") {
    lines.push(`诊断：端口 ${port} 被其他进程（PID ${pid}）占用——该进程仍存活，且不是本次要替换的旧进程，故不抢端口、立即失败。`)
    lines.push("处置：先结束该进程（或改用其他端口）后重试。")
  } else if (reason === "zombie-socket") {
    lines.push(
      `诊断：端口 ${port} 被僵尸套接字占用——属主进程 PID ${pid} 已不存在，但端口仍处于 LISTENING` +
        "（socket 句柄被其他进程持有，内核引用计数未归零，故端口不释放）。",
    )
    lines.push(
      "为何杀不掉：netstat 显示的只是创建者 PID；真正持有句柄的可能是继承了它的子进程——" +
        "taskkill / 任务管理器对该 PID 无效（进程已不存在），用户空间也无法直接释放该端口。",
    )
    lines.push(
      "处置（代价从低到高）：① 注销并重新登录（终止本会话全部进程、释放句柄，比重启电脑快）；" +
        "② 结束可能持有句柄的后代进程（如 dev-reload 的构建 watch、浏览器驱动）；③ 重启电脑；" +
        "或改用其他端口启动。",
    )
  } else if (reason === "ready-timeout" || String(state.error ?? "").includes("就绪超时")) {
    lines.push("诊断：新服务已拉起但未在 90 秒内就绪（看下方 logTail 判断启动失败原因）。")
  }
  if (state.error) lines.push(`原始错误：${String(state.error)}`)
  const tail = typeof state.logTail === "string" ? state.logTail.trim() : ""
  if (tail) lines.push(`新服务日志尾部：${tail}`)
  return lines
}

/**
 * 拉起器脚本的「代码新鲜度」。
 *
 * 为何需要单独判定：脚本由**当前运行进程的内存代码**在重启那一刻生成（见本文件头的可靠性设计），
 * 因此「改了 restart.ts 再重启」时，本次生成的脚本仍是**旧逻辑**——拉起器相关改动需要**再重启一次**
 * 才真正生效（新进程从磁盘加载新代码，它生成的脚本才是新的）。这个特性不影响其他文件的改动
 * （新进程直接读新源码），只影响本文件自身，但很容易让“验证已生效”的结论落空，故在 status 里显式提示。
 */
export interface LauncherFreshness {
  /** 是否做了判定（二进制模式/源文件不可读时为 false——编译形态下无源码可比）。 */
  checked: boolean
  /** 运行中代码落后于磁盘源码（拉起器改动需再重启一次才生效）。 */
  stale: boolean
  /** 磁盘上 restart.ts 的 mtime（ms）。 */
  sourceMtime?: number
  /** 本进程启动时刻（ms）。 */
  startedAt?: number
}

/** 判定拉起器代码是否落后于磁盘源码（参数可注入，便于单测）。 */
export function launcherCodeFreshness(
  opts: { sourceFile?: string; startedAt?: number; now?: number } = {},
): LauncherFreshness {
  const sourceFile = opts.sourceFile ?? join(import.meta.dirname ?? "", "restart.ts")
  const startedAt = opts.startedAt ?? (opts.now ?? Date.now()) - process.uptime() * 1000
  let sourceMtime: number
  try {
    sourceMtime = statSync(sourceFile).mtimeMs
  } catch {
    return { checked: false, stale: false } // 二进制模式（$bunfs 虚拟路径）或文件不可读：无源码可比
  }
  // 容差 1s：改完立即重启时 mtime 可能与启动时刻同秒，避免误报
  return { checked: true, stale: sourceMtime > startedAt + 1000, sourceMtime, startedAt }
}

/** 把新鲜度判定格式化为 status 的一行（纯函数，便于断言）。 */
export function formatLauncherFreshness(f: LauncherFreshness): string {
  if (!f.checked) return "拉起器代码：无法判定（二进制/编译形态无源码可比）"
  const at = (ms?: number) => (ms ? new Date(ms).toLocaleString("zh-CN") : "未知")
  if (!f.stale) return `拉起器代码：最新（restart.ts 未在本进程启动后修改，mtime ${at(f.sourceMtime)}）`
  return [
    `⚠️ 拉起器代码落后：restart.ts 于 ${at(f.sourceMtime)} 修改，而本进程启动于 ${at(f.startedAt)}——`,
    "   本进程生成的拉起器脚本仍是**改动前**的逻辑；拉起器相关改动需**再重启一次**才生效（不影响其他文件的改动）。",
  ].join("\n")
}

/** 读取最近一次重启状态 + 续跑情况（status 动作）。 */
async function readState(deps: Pick<RestartDeps, "tmpDir">): Promise<ToolResult> {
  const stateFile = join(restartDir(deps.tmpDir), "state.json")
  let text: string
  try {
    const raw = (await readFile(stateFile, "utf8")).replace(/^\uFEFF/, "").trim()
    let parsed: Record<string, unknown> | null = null
    try {
      parsed = JSON.parse(raw) as Record<string, unknown>
    } catch {
      /* 损坏/半截写入：按原文展示 */
    }
    text = parsed
      ? [`最近一次重启状态（ok=${String(parsed.ok)}）：`, ...explainRestartState(parsed), `原始 state.json：${raw}`].join("\n")
      : `最近一次重启状态：\n${raw}`
  } catch {
    text = `尚无重启记录（state.json 不存在）——从未执行过重启，或 ${restartDir()} 被清理。`
  }
  const cont = await readContinuation(deps.tmpDir)
  const res = await readContinuationResult(deps.tmpDir)
  const lines = [text]
  lines.push(formatLauncherFreshness(launcherCodeFreshness()))
  lines.push(cont ? `续跑请求（等待新服务消费）：会话 ${cont.sessionId}，提示词 ${cont.prompt.length} 字，写入于 ${new Date(cont.at).toISOString()}` : "续跑请求：无")
  lines.push(res ? `续跑结果：${JSON.stringify(res)}` : "续跑结果：无")
  return { output: lines.join("\n") }
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
    devReload: process.argv.includes("--reload") || process.env.GEBAI_DEV_RELOAD === "1",
    exit: (code) => process.exit(code),
    spawnLauncher: defaultSpawnLauncher,
    ...overrides,
  }
  return {
    name: "restart_server",
    description:
      "重启本歌白服务进程（仅本地模式可用，Windows/Linux/macOS）。执行后当前连接（飞书/Web）会短暂中断，几秒后自动恢复——外部拉起器（独立于服务进程树）等待旧进程退出与端口释放，再以同端口/同配置启动新服务并确认就绪；Web 页面在服务重启后自动重新加载（无需手动刷新），dev-reload（--reload）能力随重启继承。" +
      "可传 prompt 指定「重启后续跑」：新服务就绪后自动把这段提示词作为用户消息注入本会话并继续执行（服务重启会中断在途任务，续跑指令用于告诉模型重启后接着干什么）。" +
      "结果写入系统临时目录 gebai-restart/state.json，续跑情况见同目录 continue.result.json，日志在 server.log.*。action=status 查看最近一次重启与续跑状态（不重启；并提示拉起器代码是否落后于磁盘源码）。服务模式（多用户部署）不提供本工具。",
    parameters: schema({
      action: { type: "string", enum: ["restart", "status"], description: "restart=执行重启（默认）；status=只读最近一次重启状态与续跑情况" },
      prompt: {
        type: "string",
        description:
          "可选（仅 action=restart）：重启完成、新服务就绪后自动注入本会话的用户提示词（续跑指令）——新服务把它作为用户消息落盘并继续执行，无需用户重新发消息。重启会中断当前任务，需要接着干活时务必传：写清重启后要做什么（如「服务已重启，请继续验证 XX 并汇报结果」）。缺省不续跑。",
      },
      session: { type: "string", description: "可选：续跑目标会话 id（默认当前会话）" },
    }),
    requiresApproval: true,
    async execute(args, ctx): Promise<ToolResult> {
      if (args.action === "status") return readState(deps)
      const dir = restartDir(deps.tmpDir)
      await mkdir(dir, { recursive: true })
      // 续跑请求（prompt 参数）：先落盘——写失败即中止本次重启（不留「重启了但续跑指令丢失」的半成品）
      const prompt = typeof args.prompt === "string" ? args.prompt.trim() : ""
      let continuation: RestartContinuation | null = null
      if (prompt) {
        const sessionArg = typeof args.session === "string" ? args.session.trim() : ""
        continuation = {
          sessionId: sessionArg || ctx.sessionId,
          user: ctx.user,
          role: ctx.userRole,
          prompt,
          at: Date.now(),
          oldPid: deps.oldPid,
        }
        try {
          await writeContinuation(continuation, deps.tmpDir)
        } catch (err) {
          return { output: `续跑请求写入失败，未执行重启（服务仍在运行）：${String((err as Error).message || err)}` }
        }
      }
      // dev-reload 继承：拉起器固定 `bun run <entry>`（argv 的 --reload 不会被复制），改经环境变量传给新进程
      const effDeps: RestartDeps =
        deps.devReload && deps.env.GEBAI_DEV_RELOAD !== "1" ? { ...deps, env: { ...deps.env, GEBAI_DEV_RELOAD: "1" } } : deps
      // 状态文件先写「进行中」（旧内容清除；重启失败时拉起器覆盖为失败原因）
      await writeFile(join(dir, "state.json"), JSON.stringify({ ok: null, note: "重启进行中", oldPid: deps.oldPid, at: new Date().toISOString() }), "utf8")
      const deployed = await deployLauncher(effDeps)
      if (!deployed.ok) {
        // 未重启：清理续跑请求，避免残留文件被下次无关启动消费
        if (continuation) await removeContinuation(deps.tmpDir)
        return { output: `拉起器部署失败，未执行重启（服务仍在运行）：${deployed.error ?? "未知错误"}` }
      }
      // 先送达本回复再退出：延迟自杀窗口内引擎已完成本轮工具结果回传
      const contNote = continuation
        ? `续跑已布置：新服务就绪后将在会话 ${continuation.sessionId} 自动注入该提示词（${prompt.length} 字）并继续执行。`
        : ""
      const output =
        `重启已布置：外部拉起器（独立进程）已启动，${deps.exitDelayMs}ms 后当前进程退出（PID ${deps.oldPid}）。` +
        `新服务将以端口 ${deps.port} 拉起${deps.devReload ? "（dev-reload 模式继承）" : ""}，就绪探测最长 90s。` +
        contNote +
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
