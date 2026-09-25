/**
 * local_infer 的【跨平台进程管理层】：启动参数组装 → 脱离进程树启动 → 就绪轮询 → 进程/端口操作。
 *
 * 为什么不再依赖 PowerShell：验证与部署机可能是 Linux（无 pwsh）。TS 层直接用 node:child_process
 * 的 spawn 就能跨平台完成启动/停止/探活，`infer/scripts/run-server.ps1` 降为 Windows 的兼容回退
 * （见 server.ts 的 start 的 `mode: "script"`）。
 *
 * 设计要点：
 *   * **平台命令一律是纯函数**（killPidCmd / killAllCmd / portOwnerCmd / parsePortOwner / pidAliveCmd /
 *     listProcsCmd，平台作显式参数，缺省 process.platform）——测试以 "win32"/"linux" 直接断言，不随宿主漂移。
 *   * **不继承调用方的 stdio**：stdout/stderr 重定向到日志文件 fd，`detached: true` + `unref()`——否则
 *     工具会一直挂在长驻子进程的句柄上无法返回（这正是 run-server.ps1 当年要用 WMI 创建进程的原因）。
 *   * **失败必须可诊断**：可执行文件预检 + 「启动后短窗内退出」判定，抛错时附带日志尾部，不静默成功。
 *   * **argv 原样回传**（LaunchResult.argv）：本次启动行为可审计、可复现。
 *   * 引擎从哪来（矩阵/下载/安装）在 engines.ts；本文件只管「拿到 exe 之后怎么起、怎么停、怎么探」。
 */
import { spawn, type ChildProcess } from "node:child_process"
import { accessSync, constants, existsSync, mkdirSync, openSync, closeSync, appendFileSync, readFileSync, readdirSync, statSync } from "node:fs"
import { availableParallelism } from "node:os"
import { basename, dirname, isAbsolute, join, win32 } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { findLlamaServer, installedEngine, loadEngineMatrix, detectPlatform, listEngineViews, type InstalledEngine } from "./engines"
import { tailLines } from "./paths"

// ── 常量与平台化名字 ──────────────────────────────────────────────────────

/** 进程名（kill/pgrep/tasklist 用）。 */
export const SERVER_NAME = "llama-server"

/** 各平台的 llama-server 可执行文件名。 */
export function serverExeName(platform: NodeJS.Platform = process.platform): string {
  return platform === "win32" ? `${SERVER_NAME}.exe` : SERVER_NAME
}

/** 可执行文件名的两种写法（跨平台解压包可能只带一种；探测时都试）。 */
function exeNameCandidates(platform: NodeJS.Platform): string[] {
  return platform === "win32" ? [`${SERVER_NAME}.exe`, SERVER_NAME] : [SERVER_NAME, `${SERVER_NAME}.exe`]
}

/** 跨平台绝对路径判定（仓库里共享 C:\ 与 / 两种形态的配置，见 paths.isAbsolutePath）。 */
function isAbs(p: string): boolean {
  return isAbsolute(p) || win32.isAbsolute(p)
}

/** 缺省线程数（run-server.ps1 的 0.5×逻辑核；至少 1）。 */
export function defaultThreads(): number {
  const n = typeof availableParallelism === "function" ? availableParallelism() : 4
  return Math.max(1, Math.floor(n / 2))
}

// ── 启动参数组装（纯函数） ────────────────────────────────────────────────

/** llama-server 参数来源（档位 + 命令行覆盖后的最终值）。 */
export interface ServerArgOptions {
  /** 模型权重绝对路径（-m）。 */
  modelPath: string
  host?: string
  port: number
  ctx?: number
  ngl?: number
  /** 留在 CPU 的专家层数：null/undefined = 不给该参数（引擎自行决定）；0 = 全部专家进显存（有效值）。 */
  ncmoe?: number | null
  parallel?: number
  threads?: number
  batch?: number
  ubatch?: number
  flashAttn?: string
  cacheTypeK?: string
  cacheTypeV?: string
  /** 模型别名（-a；OpenAI 端点 /v1/models 里显示的名字）。 */
  alias?: string
  /** 档位附加参数（原样追加在末尾）。 */
  extraArgs?: string[]
  contBatching?: boolean
  backendSampling?: boolean
}

/**
 * 组装 llama-server 参数（纯函数，逐字段可断言）。
 * 缺省：host=127.0.0.1、ctx=32768、ngl=99（`ngl: 0` 是有效值=全 CPU，不会被缺省吞掉）；
 * threads/batch/ubatch/parallel/flashAttn/cacheTypeK/cacheTypeV/alias 未给则**不产出**该参数。
 */
export function buildServerArgs(o: ServerArgOptions): string[] {
  if (!o.modelPath) throw new Error("buildServerArgs: 缺少模型路径")
  if (!Number.isFinite(o.port)) throw new Error("buildServerArgs: 缺少端口")
  const a: string[] = ["-m", o.modelPath, "--host", o.host ?? "127.0.0.1", "--port", String(o.port)]
  a.push("-c", String(o.ctx ?? 32768))
  a.push("-ngl", String(o.ngl ?? 99))
  if (o.flashAttn) a.push("-fa", o.flashAttn)
  if (o.cacheTypeK) a.push("-ctk", o.cacheTypeK)
  if (o.cacheTypeV) a.push("-ctv", o.cacheTypeV)
  if (o.threads != null) a.push("-t", String(o.threads))
  if (o.batch != null) a.push("-b", String(o.batch))
  if (o.ubatch != null) a.push("-ub", String(o.ubatch))
  if (o.parallel != null) a.push("-np", String(o.parallel))
  if (o.alias) a.push("-a", o.alias)
  if (o.ncmoe != null) a.push("-ncmoe", String(o.ncmoe))
  if (o.contBatching) a.push("--cont-batching")
  if (o.backendSampling) a.push("-bs")
  if (o.extraArgs?.length) a.push(...o.extraArgs)
  return a
}

// ── 启动（跨平台，脱离进程树） ────────────────────────────────────────────

export interface LaunchSpec {
  /** 可执行文件绝对路径。 */
  exe: string
  args: string[]
  /** 服务日志（stdout+stderr 合并重定向到这里）。 */
  logPath: string
  cwd?: string
  env?: Record<string, string>
}

export interface LaunchResult {
  pid: number
  logPath: string
  /** 实际使用的完整命令行（可审计）。 */
  argv: string[]
}

/** 「启动后多快退出算启动失败」的判定窗口（毫秒）。 */
const EARLY_EXIT_WINDOW_MS = 1500

/** 往日志追加一行 launcher 自己的说明（spawn 失败等不在 llama-server 输出里的信息）。 */
function logNote(logPath: string, msg: string): void {
  try {
    appendFileSync(logPath, `[launcher ${new Date().toISOString()}] ${msg}\n`, "utf-8")
  } catch {
    /* 日志不可写不该掩盖原始错误 */
  }
}

/** 日志尾部（抛错时附带，便于直接看到「为什么起不来」）。 */
export function logTail(logPath: string, lines = 20): string {
  const t = tailLines(logPath, lines)
  const text = t.text.trim() || "（日志为空）"
  return `日志尾部（${logPath}${t.truncated ? "，已截断" : ""}）:\n${text}`
}

/** 可执行文件预检：不存在/不可执行时**同步**抛出可读错误（spawn 的 ENOENT 是异步事件，不利于 fail-fast）。 */
export function assertExecutable(exe: string, platform: NodeJS.Platform = process.platform): void {
  if (!existsSync(exe)) {
    throw new Error(`找不到可执行文件：${exe}（本平台没有已安装引擎时，先用 local_infer_engines 查看可下载项、local_infer_engine_fetch 安装）`)
  }
  if (platform !== "win32") {
    try {
      accessSync(exe, constants.X_OK)
    } catch {
      throw new Error(`可执行文件没有执行权限：${exe}（chmod +x 后重试）`)
    }
  }
}

/** 打开日志 fd（目录不存在则创建）。 */
function openLogFd(logPath: string): number {
  mkdirSync(dirname(logPath), { recursive: true })
  return openSync(logPath, "a")
}

/** 内部：真正 spawn（预制条件由调用方保证），返回子进程与 pid。 */
function spawnRaw(spec: LaunchSpec): { child: ChildProcess; pid: number } {
  const fd = openLogFd(spec.logPath)
  try {
    const child = spawn(spec.exe, spec.args, {
      cwd: spec.cwd,
      env: { ...process.env, ...(spec.env ?? {}) },
      detached: true, // 自成进程组：父进程退出/被杀不影响服务
      windowsHide: true, // 仅 Windows 有效
      stdio: ["ignore", fd, fd], // **不继承调用方 stdio**，否则工具挂住
    })
    // spawn 的失败（ENOENT/EACCES）以 error 事件异步到达：挂一次性处理器写进日志，
    // 避免 unhandled 'error' 把调用方进程直接崩掉。
    child.on("error", (e: Error) => logNote(spec.logPath, `spawn 失败：${e.message}`))
    child.unref()
    return { child, pid: child.pid ?? 0 }
  } finally {
    closeSync(fd) // 子进程已持有 fd 副本；父进程不保留句柄
  }
}

/** 脱离当前进程树启动（同步版：只保证 spawn 已提交，不等待进程存活）。 */
export function spawnServer(spec: LaunchSpec): LaunchResult {
  assertExecutable(spec.exe)
  const { pid } = spawnRaw(spec)
  return { pid, logPath: spec.logPath, argv: [spec.exe, ...spec.args] }
}

/**
 * 启动并确认「没有立刻死掉」：启动失败（exe 不存在/无权限）或进程在短窗内退出 → reject，错误里带日志尾部。
 * 成功返回 LaunchResult（含 pid 与 argv）；就绪与否由调用方用 waitReady 轮询。
 */
export async function spawnServerAsync(spec: LaunchSpec): Promise<LaunchResult> {
  try {
    assertExecutable(spec.exe)
  } catch (e) {
    throw new Error(`${(e as Error).message}\n${logTail(spec.logPath)}`)
  }
  const { child, pid } = spawnRaw(spec)
  const result: LaunchResult = { pid, logPath: spec.logPath, argv: [spec.exe, ...spec.args] }
  return await new Promise<LaunchResult>((resolve, reject) => {
    let settled = false
    const timer = setTimeout(() => {
      if (settled) return
      settled = true
      resolve(result)
    }, EARLY_EXIT_WINDOW_MS)
    child.once("error", (e: Error) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      reject(new Error(`启动失败：${e.message}\n${logTail(spec.logPath)}`))
    })
    child.once("exit", (code, signal) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      reject(new Error(`进程启动后立即退出（${signal ? `信号 ${signal}` : `exit ${code}`}）\n${logTail(spec.logPath)}`))
    })
  })
}

// ── 就绪轮询 ──────────────────────────────────────────────────────────────

export interface WaitReadyOptions {
  timeoutMs?: number
  intervalMs?: number
  fetchImpl?: typeof fetch
  /** 每次探测前先看进程是否还活着（死了就不必白等到超时）。 */
  isAlive?: () => Promise<boolean>
}

/**
 * 轮询 /health 直到就绪。**不抛错**：超时/进程退出一律以 `{ ok: false, error }` 返回（调用方决定怎么报）。
 * 单次请求超时上限 3s（引擎在加载模型时可能整段不响应），整体预算由 timeoutMs 兜底。
 */
export async function waitReady(baseUrl: string, o: WaitReadyOptions = {}): Promise<{ ok: boolean; secs: number; error?: string }> {
  const timeoutMs = Math.max(0, o.timeoutMs ?? 180_000)
  const intervalMs = Math.max(50, o.intervalMs ?? 2000)
  const doFetch = o.fetchImpl ?? fetch
  const root = baseUrl.replace(/\/+$/, "")
  const t0 = Date.now()
  const secs = (): number => Math.round((Date.now() - t0) / 1000)
  let last = ""
  for (;;) {
    if (o.isAlive) {
      try {
        if (!(await o.isAlive())) return { ok: false, secs: secs(), error: "进程已退出（启动失败），见日志尾部" }
      } catch {
        /* 探活本身失败（命令不可用等）不该阻断等待 */
      }
    }
    try {
      const res = await doFetch(`${root}/health`, { signal: AbortSignal.timeout(Math.max(1000, Math.min(intervalMs * 2, 3000))) })
      if (res.ok) return { ok: true, secs: secs() }
      last = `HTTP ${res.status}`
    } catch (e) {
      const err = e as Error
      last = err?.name === "TimeoutError" ? "请求超时（尚未开始监听）" : (err?.message ?? String(e))
    }
    const left = timeoutMs - (Date.now() - t0)
    if (left <= 0) return { ok: false, secs: secs(), error: last || "等待就绪超时" }
    await new Promise((resolve) => setTimeout(resolve, Math.min(intervalMs, left)))
  }
}

// ── 平台命令（纯函数：平台显式注入，测试不随宿主漂移） ────────────────────

/** 判断某个 PID 是否存活。win32 用 tasklist 精确过滤，POSIX 用 kill -0（不发信号，仅探活）。 */
export function pidAliveCmd(pid: number, platform: NodeJS.Platform = process.platform): string {
  return platform === "win32" ? `tasklist /FI "PID eq ${pid}" /FO CSV /NH` : `kill -0 ${pid} 2>/dev/null`
}

/** 从探活命令的输出/退出码判断存活：win32 看输出是否含该 PID，POSIX 看退出码。 */
export function pidAliveVerdict(
  pid: number,
  out: { stdout: string; stderr: string; code: number },
  platform: NodeJS.Platform = process.platform,
): boolean {
  if (platform === "win32") return new RegExp(`"${pid}"`).test(out.stdout)
  return out.code === 0
}

/** 按 PID 终止进程树（llama-server 会拉起子进程；Windows 必须 /T 才不留孤儿）。 */
export function killPidCmd(pid: number, platform: NodeJS.Platform = process.platform): string {
  return platform === "win32" ? `taskkill /PID ${pid} /T /F` : `kill -9 ${pid} 2>/dev/null || true`
}

/** 终止某进程名的全部实例（无状态文件时的兜底；name 缺省 llama-server）。 */
export function killAllCmd(name: string = SERVER_NAME, platform: NodeJS.Platform = process.platform): string {
  if (platform === "win32") {
    const image = name.toLowerCase().endsWith(".exe") ? name : `${name}.exe`
    return `taskkill /IM ${image} /T /F`
  }
  return `pkill -f ${name} || true`
}

/** 列出进程（人读用）。 */
export function listProcsCmd(name: string = SERVER_NAME, platform: NodeJS.Platform = process.platform): string {
  if (platform === "win32") {
    const image = name.toLowerCase().endsWith(".exe") ? name : `${name}.exe`
    return `tasklist /FI "IMAGENAME eq ${image}" /FO CSV /NH`
  }
  return `pgrep -a ${name} || true`
}

/** 查询端口监听（含占用者 pid：win32 netstat -ano，POSIX ss -ltnp）。 */
export function portOwnerCmd(port: number, platform: NodeJS.Platform = process.platform): string {
  return platform === "win32" ? `netstat -ano | findstr ":${port} "` : `ss -ltnp 2>/dev/null | grep ":${port} " || true`
}

/** 端口是否在监听（win32 netstat 打 LISTENING；POSIX ss 打 LISTEN）。 */
export function isListening(stdout: string): boolean {
  return /LISTENING/i.test(stdout) || /\bLISTEN\b/i.test(stdout)
}

/**
 * 从端口查询输出里解析占用者 PID。
 * win32：netstat 末列（LISTENING 行）；POSIX：ss 的 `pid=<n>`，退化到 `netstat -ltnp` 的 `<pid>/<name>`。
 */
export function parsePortOwner(stdout: string, platform: NodeJS.Platform = process.platform): number | undefined {
  if (platform === "win32") {
    for (const line of stdout.split(/\r?\n/)) {
      if (!/LISTENING/i.test(line)) continue
      const m = line.trim().match(/(\d+)\s*$/)
      if (m) return Number(m[1])
    }
    return undefined
  }
  const m = stdout.match(/pid=(\d+)/)
  if (m) return Number(m[1])
  for (const line of stdout.split(/\r?\n/)) {
    if (!/\bLISTEN\b/i.test(line)) continue
    const m2 = line.trim().match(/(\d+)\/[\w.\-]+$/)
    if (m2) return Number(m2[1])
  }
  return undefined
}

// ── 进程/端口操作（执行函数：命令构造走上面的纯函数） ────────────────────

/** 探活。 */
export async function pidAlive(pid: number, ctx: ToolContext, platform: NodeJS.Platform = process.platform): Promise<boolean> {
  const r = await ctx.runCommand(pidAliveCmd(pid, platform), { timeoutMs: 20000 })
  return pidAliveVerdict(pid, r, platform)
}

/** 按 PID 终止进程树。 */
export async function killPid(pid: number, ctx: ToolContext, platform: NodeJS.Platform = process.platform): Promise<{ ok: boolean; output: string }> {
  const r = await ctx.runCommand(killPidCmd(pid, platform), { timeoutMs: 30000 })
  return { ok: r.code === 0, output: (r.stdout || r.stderr || "").trim() }
}

/** 按端口找占用者并终止（找不到占用者时 ok=false、pid 为空）。 */
export async function killByPort(port: number, ctx: ToolContext, platform: NodeJS.Platform = process.platform): Promise<{ ok: boolean; pid?: number; output: string }> {
  const r = await ctx.runCommand(portOwnerCmd(port, platform), { timeoutMs: 20000 })
  const pid = parsePortOwner(r.stdout, platform)
  if (!pid) return { ok: false, output: (r.stdout || r.stderr || "").trim() }
  const k = await killPid(pid, ctx, platform)
  return { ok: k.ok, pid, output: k.output }
}

// ── 引擎可执行文件定位 ────────────────────────────────────────────────────

/** 已安装引擎的目录引用（vendor/<id> 下带 .engine.json 标记）。 */
export interface InstalledEngineRef {
  id: string
  exe: string
  dir: string
  tag?: string
}

/** 引擎层（engines.ts）判定单个 id：异常/未实现时返回 null（不阻断本地文件系统扫描）。 */
function tryEngineLayer(home: string, id: string): InstalledEngine | null {
  try {
    return installedEngine(home, id)
  } catch {
    return null
  }
}

/** 引擎层推荐的本机已安装引擎（矩阵 × 本机平台 × 已安装状态，已安装优先排序）：
 *  调用方没指名引擎时，优先听引擎层的推荐，再用文件系统扫描兜底。 */
function engineLayerPick(home: string, platform: NodeJS.Platform): InstalledEngine | null {
  try {
    const matrix = loadEngineMatrix(home)
    if (!matrix) return null
    const plat = detectPlatform(platform, process.arch)
    const hit = listEngineViews(home, matrix, plat).find((v) => v.installed === true && typeof v.exe === "string")
    if (!hit) return null
    return installedEngine(home, String(hit.id))
  } catch {
    return null
  }
}

/**
 * 扫描 vendor/<id>/.engine.json（引擎安装状态的**唯一事实来源**，见 engines.ts 的设计说明）。
 * 标记文件缺失即视为未安装——与 installedEngine 的判定一致；标记里的 exe 支持相对路径。
 */
export function listInstalledEngines(home: string, platform: NodeJS.Platform = process.platform): InstalledEngineRef[] {
  const vendor = join(home, "vendor")
  if (!existsSync(vendor)) return []
  const out: InstalledEngineRef[] = []
  for (const id of readdirSync(vendor)) {
    if (id.startsWith(".")) continue
    const dir = join(vendor, id)
    const marker = join(dir, ".engine.json")
    if (!existsSync(marker)) continue
    try {
      if (!statSync(dir).isDirectory()) continue
      const j = JSON.parse(readFileSync(marker, "utf-8")) as { exe?: string; tag?: string }
      const exe = j.exe ? (isAbs(j.exe) ? j.exe : join(dir, j.exe)) : exeNameCandidates(platform).map((n) => join(dir, n)).find((p) => existsSync(p))
      if (!exe || !existsSync(exe)) continue
      out.push({ id, exe, dir, tag: j.tag })
    } catch {
      /* 标记损坏则跳过（不阻断其它引擎） */
    }
  }
  return out.sort((a, b) => a.id.localeCompare(b.id))
}

/**
 * vendor/<id>/ 下**无安装标记**但真能找到 llama-server 的目录（手工解压/未写标记的安装）。
 * 与 listInstalledEngines 的区别：那个认 `.engine.json`（installEngine 的安装记录），这个只认文件本身——
 * 安装状态定义上仍以标记为准，所以它归在 via "engine_dir"（目录约定命中）而不是 "engine"。
 * 只认本平台主名（Linux 不拿 .exe 去 spawn）——避免选到跨平台解压包里的错二进制。
 */
function scanVendorBins(home: string, platform: NodeJS.Platform): { id: string; exe: string } | null {
  const vendor = join(home, "vendor")
  if (!existsSync(vendor)) return null
  const want = serverExeName(platform).toLowerCase()
  for (const id of readdirSync(vendor).sort()) {
    if (id.startsWith(".")) continue // .cache 等隐藏目录不算
    const dir = join(vendor, id)
    try {
      if (!statSync(dir).isDirectory()) continue
    } catch {
      continue
    }
    if (existsSync(join(dir, ".engine.json"))) continue // 有标记的已归步骤 ②，不重复报
    try {
      const hit = findLlamaServer(dir, platform)
      if (hit && basename(hit).toLowerCase() === want) return { id, exe: hit }
    } catch {
      /* 单个目录不可读不影响其它候选 */
    }
  }
  return null
}

/**
 * 解析本次启动要用的 llama-server（四步，先可靠后兼容）：
 *   ① 引擎层（engines.ts）：engineId 指定则只认它（installedEngine）；否则听本机推荐（矩阵 × 已安装）
 *   ② 文件系统扫描 `vendor/<id>/.engine.json` 安装标记（矩阵缺失/手工解压的场景也能定位）——① ② 均为 via "engine"
 *   ③ 目录约定回退：档位 engine_dir → vendor/<id>/ 下未写标记但能找到的 llama-server（via "engine_dir"）
 *   ④ 都没有 → via "none" + 可操作 note（指向 local_infer_engines / engine_fetch）
 */
export function resolveEngineExe(o: {
  home: string
  engineId?: string
  engineDir?: string
  platform?: NodeJS.Platform
}): { exe?: string; via: "engine" | "engine_dir" | "none"; note?: string } {
  const platform = o.platform ?? process.platform
  const names = exeNameCandidates(platform)

  // ① 引擎层（矩阵/安装标记）：显式 id 优先，否则听引擎层的本机推荐
  const notes: string[] = []
  if (o.engineId) {
    const inst = tryEngineLayer(o.home, o.engineId)
    if (inst?.exe) return { exe: inst.exe, via: "engine", note: `引擎 ${o.engineId}（${inst.dir}）` }
  } else {
    const pick = engineLayerPick(o.home, platform)
    if (pick?.exe) return { exe: pick.exe, via: "engine", note: `引擎 ${pick.id}（${pick.dir}）` }
  }
  // ② 文件系统扫描安装标记 .engine.json（矩阵缺失/手工解压的场景也能定位已安装引擎）
  const scan = listInstalledEngines(o.home, platform)
  const hit = o.engineId ? scan.find((e) => e.id === o.engineId) : scan[0]
  if (hit) return { exe: hit.exe, via: "engine", note: `引擎 ${hit.id}（${hit.dir}）` }
  if (o.engineId) notes.push(`引擎 ${o.engineId} 未安装`)

  // ③ engine_dir 回退
  if (o.engineDir) {
    const dir = isAbs(o.engineDir) ? o.engineDir : join(o.home, o.engineDir)
    for (const n of names) {
      const p = join(dir, n)
      if (existsSync(p)) return { exe: p, via: "engine_dir", note: `${notes.length ? `${notes.join("；")}；` : ""}从档位 engine_dir 取到：${p}` }
    }
    notes.push(`engine_dir 下没有 ${names.join(" / ")}（${dir}）`)
  }

  // ③' vendor/<id>/ 下手工解压、未写安装标记的 llama-server（不冒充「已安装」，note 里说清楚）
  const manual = scanVendorBins(o.home, platform)
  if (manual) {
    return {
      exe: manual.exe,
      via: "engine_dir",
      note: `${notes.length ? `${notes.join("；")}；` : ""}vendor/${manual.id}/ 下找到 ${basename(manual.exe)}（无安装标记 .engine.json——手工解压或未完成安装）`,
    }
  }

  // ④ none
  return {
    via: "none",
    note: `${notes.length ? `${notes.join("；")}。` : ""}该平台无已安装引擎——先 local_infer_engines 查看可下载项、local_infer_engine_fetch 安装（或把预编译包解压到 profiles.json 的 engine_dir）`,
  }
}

// ── 启动环境（Windows 需要 cudart 在 PATH 里） ────────────────────────────

/**
 * 启动环境变量：Windows 下把最新 CUDA runtime 的 bin 目录前置进 PATH
 * （llama.cpp 预编译 CUDA 包依赖 cudart64_*.dll；run-server.ps1 里同样处理）。
 * 找不到就返回空对象（引擎多为静态链接或自带 DLL，不因缺 PATH 而失败）。
 */
export function launcherEnv(platform: NodeJS.Platform = process.platform, env: Record<string, string | undefined> = process.env): Record<string, string> {
  const out: Record<string, string> = {}
  if (platform !== "win32") return out
  try {
    const root = join(env.ProgramFiles ?? "C:\\Program Files", "NVIDIA GPU Computing Toolkit", "CUDA")
    if (!existsSync(root)) return out
    const vers = readdirSync(root)
      .filter((v) => statSync(join(root, v)).isDirectory())
      .sort()
      .reverse()
    if (!vers.length) return out
    const bin = join(root, vers[0], "bin")
    if (existsSync(bin)) out.PATH = `${bin};${env.PATH ?? ""}`
  } catch {
    /* PATH 处理失败不该阻断启动 */
  }
  return out
}
