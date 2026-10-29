/**
 * 进程接管（takeover）：拉起常驻伴生应用（相期窗口 / 册府知识库）前，
 * 先关闭**已经在跑的同一可执行文件**，再由调用方拉起新实例。
 *
 * ## 为什么需要这一步（不 kill 直接 spawn 的后果）
 *
 * 常驻应用重启服务后仍占着旧资源（端口/数据文件/单实例锁），新实例起不来
 * 或起来后行为错乱——典型形态：旧窗口连着已死的服务端 WS 反复重连，用户在
 * 旧窗口上的操作全部无响应。与其诊断「为什么没反应」，不如**接管时先收旧的**。
 *
 * ## 判定标准：按 exe 全路径精确匹配
 *
 * 不按进程名（xiangqi.exe 可能是别人构建的另一个实例）、不按命令行模糊匹配
 * （误杀面不可控）。枚举系统进程的可执行文件路径，与本进程**将要拉起**的
 * exe 路径归一化（大小写/分隔符）后精确相等才算命中。
 *
 * ## 平台实现
 *
 * - Windows：`Get-CimInstance Win32_Process`（wmic 已废弃，Win11 24H2+ 移除），
 *   `ExecutablePath` 字段给全路径；kill 走 `taskkill /PID <pid> /T /F`（杀进程树，
 *   常驻应用可能有子进程）。
 * - POSIX：`ps -eo pid=,comm=` 拿不到全路径，用 `/proc/<pid>/exe` 逐个读太贵——
 *   改用 `pgrep -f <exe路径>`（命令行含 exe 全路径即命中；pgrep 不误杀自身，
 *   匹配串是绝对路径不含通配元字符）+ `kill <pid>`（先 TERM 后 KILL）。
 *
 * ## 失败语义：尽力而为，绝不阻断
 *
 * 枚举失败/kill 失败/等待退出超时都只记日志返回——接管是**拉起前的清理**，
 * 清理不干净顶多新旧并存（新实例自己会报资源冲突），而把它变成启动失败
 * 意味着「清理工具坏了 ⇒ 整个应用起不来」，代价不成比例。
 */

import { spawn } from "node:child_process"
import { mkdtempSync, rmSync, unlinkSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"

export interface TakeoverLogger {
  info(msg: string): void
  warn(msg: string): void
}

export interface TakeoverResult {
  /** 命中的旧进程（pid 与 exe 路径）。 */
  killed: Array<{ pid: number; exe: string }>
  /** 枚举/kill 失败（不阻断拉起，仅记录）。 */
  warnings: string[]
  /** kill 后仍活着的 pid（等退出超时）。 */
  survivors: number[]
}

/** 路径归一化比较：Windows 大小写不敏感 + 分隔符统一（其余平台原样）。 */
//（keep）samePath 暂未用：Windows 判定已内联到 PowerShell 过滤器；POSIX 用 pgrep -f。

/** 枚举「可执行文件路径等于 target 的进程 pid」。失败返回 []（记 warning 由调用方决定）。 */
async function pidsByExePath(target: string, tmpDirForScript: string): Promise<number[]> {
  if (process.platform === "win32") {
    // -Filter 按 Name 缩小范围再在 PS 里比对路径：全进程表拉 ExecutablePath 也行，
    // 但按名过滤传输量小一个量级；名字取 basename（xiangqi.exe）。
    // 复杂命令写成临时 ps1（uv_spawn 对策，见 runCapture 注释）。
    const name = target.split(/[\\/]/).pop() ?? target
    const lower = target.replace(/'/g, "''").replace(/\//g, "\\").toLowerCase()
    const ps = [
      `[Console]::OutputEncoding=[Text.Encoding]::UTF8`,
      `Get-CimInstance Win32_Process -Filter "Name='${name.replace(/'/g, "''")}'" |`,
      `  Where-Object { $_.ExecutablePath -and ('' + $_.ExecutablePath).Replace('/','\\').ToLower() -eq '${lower}' } |`,
      `  ForEach-Object { $_.ProcessId }`,
    ].join("\n")
    const scriptFile = join(tmpDirForScript, `takeover-${process.pid}-${Math.random().toString(36).slice(2, 8)}.ps1`)
    writeFileSync(scriptFile, ps, "utf8")
    const out = await runCapture({ scriptFile })
    if (out.error) throw new Error(out.error)
    return out.stdout
      .split(/[\s,]+/)
      .map((s) => Number(s.trim()))
      .filter((n) => Number.isInteger(n) && n > 0)
  }
  // POSIX：pgrep -f 匹配完整命令行；exe 全路径作为子串足够精确（无通配元字符）
  const out = await runCapture({ cmd: ["pgrep", "-f", target] })
  if (out.error) {
    // pgrep 无命中退出码 1：不是错误
    if (out.code === 1) return []
    throw new Error(out.error)
  }
  return out.stdout
    .split("\n")
    .map((s) => Number(s.trim()))
    .filter((n) => Number.isInteger(n) && n > 0)
}

interface CaptureResult {
  stdout: string
  code: number | null
  error?: string
}

/** 执行命令并捕获输出。
 *
 * 注意：Windows 下经 `bun run <文件>` 执行时，node:child_process/Bun.spawn 对**含管道、
 * 引号嵌套等复杂命令行**会报 uv_spawn（实测：echo hi 能起、长 PowerShell 查询必炸）。
 * 对策：复杂命令一律写成临时 .ps1 文件，spawn 只传 `powershell -NoProfile -File <path>`
 * （参数最短、无命令行构造面）。脚本文件用完即删。
 */
async function runCapture(args: { scriptFile?: string; cmd?: string[] }): Promise<CaptureResult> {
  let cmd: string[]
  let tmpFile: string | null = null
  if (args.scriptFile) {
    tmpFile = args.scriptFile
    cmd = ["powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", tmpFile]
  } else {
    cmd = args.cmd ?? []
  }
  return new Promise((resolve) => {
    const child = spawn(cmd[0], cmd.slice(1), {
      windowsHide: true,
      stdio: ["ignore", "pipe", "pipe"],
    })
    let stdout = ""
    let stderr = ""
    child.stdout?.on("data", (d: Buffer) => (stdout += d.toString()))
    child.stderr?.on("data", (d: Buffer) => (stderr += d.toString()))
    child.on("error", (err) => resolve({ stdout, code: null, error: String((err as Error).message || err) }))
    child.on("close", (code) => {
      if (tmpFile) {
        try {
          unlinkSync(tmpFile)
        } catch { /* 已删除/删除失败 */ }
      }
      resolve({ stdout, code, error: code !== 0 && !stdout ? stderr || `exit ${code}` : undefined })
    })
  })
}

/** pid 是否还活着（signal 0 探测）。 */
function alive(pid: number): boolean {
  try {
    process.kill(pid, 0)
    return true
  } catch {
    return false
  }
}

/** 杀一个进程树并等退出。返回是否确认退出。 */
async function killTree(pid: number, timeoutMs: number): Promise<boolean> {
  if (process.platform === "win32") {
    await runCapture({ cmd: ["taskkill", "/PID", String(pid), "/T", "/F"] })
  } else {
    try {
      process.kill(pid, "SIGTERM")
    } catch {
      /* 已退出 */
    }
  }
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (!alive(pid)) return true
    await new Promise((r) => setTimeout(r, 150))
  }
  if (process.platform !== "win32") {
    try {
      process.kill(pid, "SIGKILL")
    } catch {
      /* 已退出 */
    }
    const hard = Date.now() + 2_000
    while (Date.now() < hard) {
      if (!alive(pid)) return true
      await new Promise((r) => setTimeout(r, 150))
    }
  }
  return false
}

/**
 * 接管：关闭所有「可执行文件路径 = bin」的运行中进程。
 *
 * 幂等：没有旧进程时立即返回空结果（不 spawn 任何枚举命令以外的进程）。
 * env 注入（测试替身）：`GEBAI_TAKEOVER_DISABLE=1` 时整体跳过（CI 环境无权限枚举进程）。
 */
export async function takeoverProcess(
  bin: string,
  opts: { log?: TakeoverLogger; waitExitMs?: number; env?: Record<string, string | undefined> } = {},
): Promise<TakeoverResult> {
  const env = opts.env ?? process.env
  if (env.GEBAI_TAKEOVER_DISABLE === "1") return { killed: [], warnings: [], survivors: [] }
  const result: TakeoverResult = { killed: [], warnings: [], survivors: [] }
  // 临时 ps1 脚本目录（Windows 枚举用）：os tmpdir 下建私有目录，用完由 runCapture 逐个删文件
  let tmpDir: string | null = null
  let pids: number[]
  try {
    if (process.platform === "win32") {
      tmpDir = mkdtempSync(join(tmpdir(), "gebai-takeover-"))
    }
    pids = await pidsByExePath(bin, tmpDir ?? ".")
  } catch (err) {
    const msg = `枚举进程失败（跳过接管，直接拉起）：${String((err as Error).message || err)}`
    result.warnings.push(msg)
    opts.log?.warn(msg)
    if (tmpDir) {
      try {
        rmSync(tmpDir, { recursive: true, force: true })
      } catch { /* 清理失败不影响 */ }
    }
    return result
  }
  if (pids.length === 0) {
    if (tmpDir) {
      try {
        rmSync(tmpDir, { recursive: true, force: true })
      } catch { /* 清理失败不影响 */ }
    }
    return result
  }
  for (const pid of pids) {
    const exited = await killTree(pid, opts.waitExitMs ?? 8_000)
    if (exited) {
      result.killed.push({ pid, exe: bin })
      opts.log?.info(`已关闭旧实例 pid=${pid}（${bin}）`)
    } else {
      result.survivors.push(pid)
      const msg = `旧实例 pid=${pid} kill 后未在时限内退出（新实例可能报资源冲突）`
      result.warnings.push(msg)
      opts.log?.warn(msg)
    }
  }
  if (tmpDir) {
    try {
      rmSync(tmpDir, { recursive: true, force: true })
    } catch { /* 清理失败不影响 */ }
  }
  return result
}
