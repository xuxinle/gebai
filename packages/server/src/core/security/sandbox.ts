import { spawn, spawnSync } from "node:child_process"
import { createWriteStream, existsSync, mkdirSync, mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, relative, resolve } from "node:path"
import { sessionPath } from "../base/paths"
import { resolveInSandbox, stripTmpPrefix } from "../base/paths"
import { which } from "../exec/which"
import { isSensitive } from "../session/env"
import { log } from "@gebai/sdk/node"

export interface SandboxOptions {
  home: string
  enabled: boolean
  /** 豁免用户判定（如 auth=none 默认用户即操作者本人）：豁免用户不受路径沙箱限制、脚本环境不剔除敏感变量
   *  且不做脚本根收敛（本地模式行为不变）。 */
  isExempt?: (user: string) => boolean
  /** 脚本隔离模式（GEBAI_SCRIPT_ISOLATION）：auto（默认）在服务模式下收敛脚本环境、bwrap 可用时追加
   *  文件系统隔离；off/env/bwrap 可显式指定。 */
  scriptIsolation?: ScriptIsolationMode
}

/* ---------- 脚本运行根与会话隔离（服务模式） ---------- */

/** 脚本隔离模式（GEBAI_SCRIPT_ISOLATION）：
 *  - `off`：不收敛（宿主 HOME/TEMP/XDG 原样传给脚本）；
 *  - `env`：环境收敛——HOME/TEMP/XDG_* 指向会话内目录（跨平台）；
 *  - `bwrap`：环境收敛 + bubblewrap 文件系统隔离（Linux，近似 chroot：系统目录只读、仅会话目录可写）；
 *  - `auto`（默认）：本地模式/豁免用户恒 `off`（操作者本人机器，行为不变）；服务模式走 `env`，
 *    检测到可用的 bubblewrap 时升为 `bwrap`。 */
export type ScriptIsolationMode = "auto" | "off" | "env" | "bwrap"

/** 会话内脚本运行时目录（相对会话目录）：HOME / TEMP / XDG_* 的落点。 */
export const SCRIPT_RUNTIME_DIR = "script-env"

/** 脚本环境收敛表：变量 → 会话内子目录。不改写时脚本的配置/缓存/临时文件落在宿主共享位置
 *  （用户家目录、系统临时目录），跨用户、跨会话相互可见可覆盖。 */
const SCRIPT_ENV_VARS: Array<[string, string]> = [
  ["HOME", "home"],
  ["USERPROFILE", "home"],
  ["TMPDIR", "tmp"],
  ["TMP", "tmp"],
  ["TEMP", "tmp"],
  ["XDG_CONFIG_HOME", "config"],
  ["XDG_CACHE_HOME", "cache"],
  ["XDG_DATA_HOME", "data"],
  ["XDG_STATE_HOME", "state"],
]

/** 脚本环境覆盖表（纯函数）：sessionDir=会话目录绝对路径 → 变量到会话内路径的映射。 */
export function scriptEnvOverride(sessionDir: string): Record<string, string> {
  const out: Record<string, string> = {}
  for (const [key, dir] of SCRIPT_ENV_VARS) out[key] = join(sessionDir, SCRIPT_RUNTIME_DIR, dir)
  return out
}

/** 脚本环境需要预先创建的目录（会话内绝对路径）——HOME/TEMP 指向不存在的目录时部分程序直接报错。 */
export function scriptEnvDirs(sessionDir: string): string[] {
  return [...new Set(SCRIPT_ENV_VARS.map(([, dir]) => join(sessionDir, SCRIPT_RUNTIME_DIR, dir)))]
}

/** bubblewrap 只读绑定的系统目录（存在者才绑定）：脚本可读系统运行库与配置，但不可写——
 *  宿主应用目录、系统配置、其他用户与会话的数据均不可见。 */
const BWRAP_RO_DIRS = ["/usr", "/bin", "/sbin", "/lib", "/lib64", "/lib32", "/libx32", "/etc", "/opt", "/var", "/run", "/nix", "/snap", "/usr/local"]

/** bubblewrap 参数构造（纯函数，可单测）：系统目录只读 + 会话目录读写 + /proc /dev，chdir 到 cwd。
 *  调用方负责在其中追加 `-- <shell> -c <cmd>` 与所用解释器路径。 */
export function bwrapArgs(opts: { sessionDir: string; cwd: string; exists?: (p: string) => boolean }): string[] {
  const has = opts.exists ?? existsSync
  const args = ["--die-with-parent", "--unshare-pid", "--unshare-ipc", "--unshare-uts", "--unshare-cgroup-try"]
  for (const dir of BWRAP_RO_DIRS) if (has(dir)) args.push("--ro-bind", dir, dir)
  args.push("--proc", "/proc", "--dev", "/dev")
  args.push("--bind", opts.sessionDir, opts.sessionDir)
  args.push("--chdir", opts.cwd)
  return args
}

/** ANSI 控制序列（CSI：颜色/光标/擦除）；工具输出面向模型上下文，控制码是噪声。 */
const ANSI_ESCAPE = /[\u001B\u009B]\[[0-9;?]*[ -/]*[@-~]/g

/** 输出解码：优先 UTF-8；含替换字符（U+FFFD）时按 GBK 回退（Windows 老程序仍按 GBK 输出时兜底）；
 *  末尾剥离 ANSI 控制序列。 */
export function decodeOutput(buf: Buffer): string {
  let s = buf.toString("utf8")
  if (s.includes("\uFFFD")) {
    try {
      // Bun/Node 运行时支持 WHATWG GBK 解码；类型定义未收录该 label，绕行断言
      s = new TextDecoder("gbk" as never).decode(buf)
    } catch {
      /* 解码器不可用则保留 UTF-8 结果 */
    }
  }
  return s.replace(ANSI_ESCAPE, "")
}

/** Windows 下 sh / 脚本子进程的命令解释器（`exec` 与 `spawnBackground` 共用解析）。 */
export interface WinShell {
  /** 解释器可执行文件（PATH 探测解析所得路径；cmd 回落为 cmd.exe） */
  file: string
  /** PowerShell 形态：true=经 `-Command` 包装执行；false=cmd.exe 回落（`chcp 65001 >nul && …` 与 `&&`/`%VAR%` 语义） */
  powershell: boolean
}

/** PowerShell 启动参数：`-NoProfile`（不加载用户 profile：启动更快、行为确定）、`-NonInteractive`（交互提示转为
 *  语句级错误——缺输入立即失败而非挂起）、`-ExecutionPolicy Bypass`（允许执行 .ps1 脚本）。 */
const PS_ARGS = ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command"]

/** PowerShell 引导段（在用户命令之前）：
 *  - 输出编码切 UTF-8：解释器自身输出与原生命令输出的解码入口（与 decodeOutput 的 UTF-8 优先 + GBK 兜底口径一致）；
 *    无控制台环境下赋值会失败——try/catch 兜底不阻断命令（cmd 分支的 `chcp 65001 && …` 在 chcp 失败时会连命令一起吞掉）；
 *  - `$OutputEncoding`：管道传给原生命令的数据编码（默认 ASCII，中文会丢）；
 *  - `$ProgressPreference`：管道下进度记录会当文本写进 stdout，关掉防污染输出。 */
const PS_PREAMBLE = [
  "try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }",
  "$OutputEncoding = [Text.Encoding]::UTF8",
  "$ProgressPreference = 'SilentlyContinue'",
].join("\n")

/** PowerShell 收尾段（在用户命令之后）：原生命令（git/bun/python 等）的退出码默认被解释器归一成 0/1
 *  （`-Command` 的既定行为），显式 `exit $LASTEXITCODE` 才带出真实码——失败的原生命令保留其码、
 *  PowerShell 层面的失败归一为 1、成功不干预。逐行拼接：用户命令以 `#` 注释结尾时不会被吞掉。 */
const PS_EPILOGUE = ["if ($?) { exit 0 }", "if ($LASTEXITCODE) { exit $LASTEXITCODE }", "exit 1"].join("\n")

/** PowerShell 执行包装：引导段 + 用户命令 + 收尾段（`-Command` 的完整命令行）。 */
export function wrapPowerShellCommand(cmd: string): string {
  return [PS_PREAMBLE, cmd, PS_EPILOGUE].join("\n")
}

let winShellCache: WinShell | undefined

/** Windows 命令解释器解析（进程内缓存）：`GEBAI_SH_SHELL` 显式指定 > PATH 中的 `pwsh.exe`（PowerShell 7+：
 *  支持 `&&`/`||` 链式命令）> 系统内置 `powershell.exe`（Windows PowerShell 5.1，多命令用 `;` 分隔）。
 *  `GEBAI_SH_SHELL=cmd` 回落到 cmd.exe（保留 `&&` 与 `%VAR%` 语义）。POSIX 不经此解析（见 resolvePosixShell）。 */
export function resolveWinShell(): WinShell {
  if (winShellCache) return winShellCache
  const override = (process.env.GEBAI_SH_SHELL ?? "").trim()
  const name = override.replace(/[\\/]+/g, "/").split("/").pop() ?? override
  winShellCache = override
    ? /^cmd(\.exe)?$/i.test(name)
      ? { file: "cmd.exe", powershell: false }
      : { file: which(override) ?? override, powershell: true }
    : { file: which("pwsh.exe") ?? which("powershell.exe") ?? "powershell.exe", powershell: true }
  return winShellCache
}

/** 清空解释器解析缓存（测试用：PATH / GEBAI_SH_SHELL 变更后重新解析）。 */
export function _resetWinShellCache(): void {
  winShellCache = undefined
}

let posixShellCache: string | undefined

/** POSIX 下 sh / 脚本子进程的命令解释器解析（进程内缓存）：`GEBAI_SH_SHELL` 显式指定 >
 *  PATH 中的 `bash` > `/bin/sh`（极简容器只带 POSIX sh 时回落，保证可用）。 */
export function resolvePosixShell(): string {
  if (posixShellCache) return posixShellCache
  const override = (process.env.GEBAI_SH_SHELL ?? "").trim()
  posixShellCache = override ? (which(override) ?? override) : (which("bash") ?? "/bin/sh")
  return posixShellCache
}

/** 清空 POSIX 解释器解析缓存（测试用：PATH / GEBAI_SH_SHELL 变更后重新解析）。 */
export function _resetPosixShellCache(): void {
  posixShellCache = undefined
}

/** POSIX 子进程启动计划：`<shell> -c <命令>`（与 PowerShell 形态同为显式解释器启动，
 *  不经 Node `shell:true` 的隐式 `/bin/sh`）。 */
export function posixShellPlan(cmd: string): { file: string; args: string[] } {
  return { file: resolvePosixShell(), args: ["-c", cmd] }
}

/** Windows 子进程启动计划：PowerShell 形态返回 `{ file, args }`（命令已由 `-Command` 承载，须以 `shell:false` spawn）；
 *  cmd 回落返回 null——调用方沿用 `chcp 65001 >nul && …` + `shell:true` 的既有形态。 */
export function winShellPlan(cmd: string): { file: string; args: string[] } | null {
  const shell = resolveWinShell()
  return shell.powershell ? { file: shell.file, args: [...PS_ARGS, wrapPowerShellCommand(cmd)] } : null
}

export class Sandbox {
  constructor(private opts: SandboxOptions) {}

  /** Whether path constraints are enforced for the current run form. */
  get enabled(): boolean {
    return this.opts.enabled
  }

  /** 用户是否豁免沙箱（isExempt 判定通过即豁免，与全局开关无关）。 */
  isExempt(user: string): boolean {
    return !!this.opts.isExempt?.(user)
  }

  /** 该用户是否受沙箱约束：全局启用 且 非豁免用户（豁免用户按本地模式放开——绝对路径直用、脚本环境不脱敏）。 */
  enforcedFor(user: string): boolean {
    return this.opts.enabled && !this.isExempt(user)
  }

  /**
   * Resolve a tool-supplied path.
   * - 会话内相对路径统一以会话 `tmp/` 为基准（与 sh/py 工作目录、glob/grep 搜索范围、文件面板一致，
   *   跨工具传递路径无需考虑前缀）；带 `tmp/` 前缀的逻辑路径（列表/附件/截断产物契约，如 `tmp/a.txt`）剥离前缀后解析
   * - 沙箱启用（服务端部署/GEBAI_SANDBOX=on）且用户未豁免：仅允许会话 `tmp/` 内路径，拒绝 `../`、绝对路径、符号链接
   * - 沙箱禁用或用户豁免（本地运行/GEBAI_SANDBOX=off/默认用户豁免）：放开限制——绝对路径直接使用；
   *   相对路径仍基于会话 `tmp/` 解析，允许越界
   */
  resolvePath(user: string, sessionId: string | null, input: string): string {
    if (sessionId) {
      const tmp = this.workdir(user, sessionId)
      const rel = stripTmpPrefix(input)
      return this.enforcedFor(user) ? resolveInSandbox(tmp, rel) : resolve(tmp, rel)
    }
    const root = join(this.opts.home, "users", user)
    return this.enforcedFor(user) ? resolveInSandbox(root, input) : resolve(root, input)
  }

  workdir(user: string, sessionId: string): string {
    return join(sessionPath(this.opts.home, user, sessionId), "tmp")
  }

  /** 会话目录（会话隔离根）：服务模式下脚本 cwd、相对路径基准、脚本环境（HOME/TEMP/XDG）均落在其内。 */
  sessionDir(user: string, sessionId: string): string {
    return sessionPath(this.opts.home, user, sessionId)
  }

  /** 当前用户实际生效的脚本隔离模式：本地模式/豁免用户恒 off（操作者本人机器）；服务模式默认 env、
   *  bubblewrap 可用时升为 bwrap。 */
  isolationFor(user: string): "off" | "env" | "bwrap" {
    if (!this.enforcedFor(user)) return "off"
    const mode = this.opts.scriptIsolation ?? "auto"
    if (mode === "off") return "off"
    if (mode === "env") return "env"
    const usable = this.bwrapUsable()
    // 显式请求 bwrap 却不可用（容器未授予 user namespace 等）：只告警一次，行为仍安全回落 env
    if (!usable && mode === "bwrap" && !this.bwrapWarned) {
      this.bwrapWarned = true
      log.warn(`[sandbox] GEBAI_SCRIPT_ISOLATION=bwrap 但 bubblewrap 不可用（${this.bwrapProbeError}）：回落环境收敛（HOME/TEMP/XDG 仍在会话目录内，文件系统隔离不可用）`)
    }
    return usable ? "bwrap" : "env"
  }

  private bwrapProbe: boolean | undefined
  /** 探测失败原因（bwrap 的 stderr 摘要）：显式请求 bwrap 时随告警输出，便于容器部署排查。 */
  private bwrapProbeError = ""
  /** 回落告警只发一次（isolationFor 每次脚本执行都会调用）。 */
  private bwrapWarned = false

  /**
   * bubblewrap 可用性（探测一次并缓存）：不只要在 PATH 里，还要能完成一次**与真实执行同参数形态**的
   * 隔离启动——容器内未授予 user namespace（seccomp/AppArmor 拦截）时 bwrap 存在但每次调用都失败。
   * 故探测用 `bwrapArgs` 同一套参数（临时目录充当会话目录）与真实解释器，而不是简化形态：
   * 简化形态通过不代表真实形态通过（绑定项、只读/可写划分都可能出问题）。
   * 不可用时回落 env 收敛（环境隔离仍生效，文件系统隔离缺失）。
   */
  private bwrapUsable(): boolean {
    if (this.bwrapProbe !== undefined) return this.bwrapProbe
    this.bwrapProbe = false
    if (process.platform === "win32") {
      this.bwrapProbeError = "Windows 无 bubblewrap（无等价的内核隔离机制）"
      return false
    }
    const bin = which("bwrap")
    if (!bin) {
      this.bwrapProbeError = "PATH 中未找到 bwrap"
      return false
    }
    let probeDir: string | null = null
    try {
      // 同步探测（spawnBackground 为同步接口，探测不能依赖 await）；结果缓存，仅首次付启动成本
      probeDir = mkdtempSync(join(tmpdir(), "gebai-bwrap-probe-"))
      const shell = resolvePosixShell()
      const args = [...bwrapArgs({ sessionDir: probeDir, cwd: probeDir }), "--", shell, "-c", "true"]
      const r = spawnSync(bin, args, { timeout: 5000, encoding: "utf8" })
      this.bwrapProbe = r.status === 0
      if (!this.bwrapProbe) {
        this.bwrapProbeError = `${r.stderr || String(r.error ?? "")}`.trim().slice(0, 200) || `退出码 ${r.status}`
      }
    } catch (err) {
      this.bwrapProbe = false
      this.bwrapProbeError = (err as Error).message
    } finally {
      if (probeDir) {
        try {
          rmSync(probeDir, { recursive: true, force: true })
        } catch {
          /* 尽力而为：临时目录残留不影响功能 */
        }
      }
    }
    return this.bwrapProbe
  }

  /** bwrap 不可用时的原因（诊断用；未探测过返回空串）。 */
  bwrapUnavailableReason(): string {
    this.bwrapUsable()
    return this.bwrapProbeError
  }

  /** 脚本子进程环境覆盖（会话内 HOME/TEMP/XDG）：本地模式/豁免用户/未提供会话时返回空对象（行为不变）。 */
  scriptEnv(user?: string, sessionId?: string | null): Record<string, string> {
    if (!user || !sessionId) return {}
    if (this.isolationFor(user) === "off") return {}
    return scriptEnvOverride(this.sessionDir(user, sessionId))
  }

  /** 预创建脚本运行时目录（缺失时部分程序报错）；尽力而为，失败按原样 spawn 报错。 */
  private prepareScriptDirs(user?: string, sessionId?: string | null): void {
    if (!user || !sessionId || this.isolationFor(user) === "off") return
    for (const dir of scriptEnvDirs(this.sessionDir(user, sessionId))) {
      try {
        mkdirSync(dir, { recursive: true })
      } catch {
        /* 尽力而为 */
      }
    }
  }

  /** 脚本 spawn 计划：bwrap 模式下把命令包进隔离沙箱（系统目录只读 + 会话目录可写 + chdir 会话内）。
   *  cwd 不在会话内（本地模式残留/异常配置）时不包装——bwrap 的 --chdir 会直接失败。 */
  private scriptPlan(
    opts: { user?: string; sessionId?: string | null; cwd?: string; shellPlan: { file: string; args: string[] } | null },
  ): { file: string; args: string[] } | null {
    if (!opts.shellPlan) return null
    if (!opts.user || !opts.sessionId || this.isolationFor(opts.user) !== "bwrap") return opts.shellPlan
    const sessionDir = this.sessionDir(opts.user, opts.sessionId)
    const cwd = opts.cwd ?? this.workdir(opts.user, opts.sessionId)
    if (relative(sessionDir, cwd).startsWith("..")) return opts.shellPlan
    const bin = which("bwrap")
    if (!bin) return opts.shellPlan
    return { file: bin, args: [...bwrapArgs({ sessionDir, cwd }), "--", opts.shellPlan.file, ...opts.shellPlan.args] }
  }

  /** 保证 cwd 存在（会话 tmp/ 等目录并非必然存在——纯命令会话无附件/写文件时从未创建，
   *  spawn 对缺失 cwd 直接 ENOENT，sh/py/js/ls 全体报「找不到路径」）；尽力而为，失败按原样 spawn 报错。 */
  private async ensureCwd(cwd?: string): Promise<void> {
    if (!cwd) return
    const { mkdir } = await import("node:fs/promises")
    await mkdir(cwd, { recursive: true }).catch(() => {})
  }

  exec(
    cmd: string,
    opts: {
      cwd?: string
      env?: Record<string, string>
      timeoutMs?: number
      shell?: boolean
      input?: string
      /** 外部取消信号（停止按钮/任务取消/子Agent超时）：abort 时立即按进程树终止命令并返回中断结果。 */
      signal?: AbortSignal
      /** 发起用户：豁免用户（如 auth=none 默认用户）脚本子进程不剔除敏感变量（本地操作者本人环境）。 */
      user?: string
      /** 会话 id（服务模式）：脚本运行根收敛——HOME/TEMP/XDG 指向会话内目录，bwrap 模式追加文件系统隔离。 */
      sessionId?: string | null
    } = {},
  ): Promise<{ stdout: string; stderr: string; code: number }> {
    // cwd 保证存在后再 spawn（缺失目录 spawn 直接 ENOENT）
    return this.ensureCwd(opts.cwd).then(
      () =>
        new Promise((resolve) => {
          this.prepareScriptDirs(opts.user, opts.sessionId)
      const timeoutMs = opts.timeoutMs ?? 5 * 60 * 1000
      // 沙箱（服务端部署）模式下脚本子进程环境剔除敏感变量（*_KEY/*_TOKEN/*_SECRET/PASSWORD 等）：
      // 脚本（sh/py/任务）是可任意执行的代码，若继承服务端全局密钥（如 OPENAI_API_KEY），
      // 任意用户（含审批跳过场景）可经 env/读取将其外泄；脱敏后脚本仍可用非敏感全局变量，
      // 敏感配置仅限进程内工具（feishu 等）经 ToolContext.env 使用。豁免用户（本地操作者本人）不剔除。
      const merged = { ...process.env, ...opts.env }
      const stripSensitive = this.opts.enabled && (opts.user == null || !this.isExempt(opts.user))
      const filtered = stripSensitive ? Object.fromEntries(Object.entries(merged).filter(([k]) => !isSensitive(k))) : merged
      // 脚本运行根收敛（服务模式）：HOME/TEMP/XDG 指向会话内目录——后于任务 env 合并，环境收敛优先
      const env = { ...filtered, ...this.scriptEnv(opts.user, opts.sessionId) }
      // 走 shell 时按平台解析解释器并显式启动（不经 Node 的隐式 `/bin/sh`）：Windows 经 PowerShell
      // （winShellPlan：解释器引导段已切 UTF-8 输出编码；GEBAI_SH_SHELL=cmd 回落 cmd.exe 分支的
      // `chcp 65001 >nul && …` 切代码页）、POSIX 经 bash（posixShellPlan）；输出统一按 UTF-8 解码
      // （decodeOutput 兜底 GBK）；bwrap 可用时整条命令包进隔离沙箱（系统只读 + 会话目录可写）
      const isWin = process.platform === "win32"
      const usesShell = opts.shell !== false
      const basePlan = usesShell ? (isWin ? winShellPlan(cmd) : posixShellPlan(cmd)) : null
      const plan = isWin ? basePlan : this.scriptPlan({ user: opts.user, sessionId: opts.sessionId, cwd: opts.cwd, shellPlan: basePlan })
      const shellCmd = !basePlan && isWin && usesShell ? `chcp 65001 >nul && ${cmd}` : cmd
      // detached：Unix 下子进程成为独立进程组组长，超时/取消可按进程组整体终止（kill(-pid)），
      // 防 shell 被杀后其孙进程（如 sleep/后台任务）残留；
      // Windows 下不使用 detached：实测 detached 子进程的外部程序（.exe）stdout/stderr 管道输出
      // 会完全丢失（解释器内置命令正常），且 Windows 分支走 taskkill /T 进程树终止，无需进程组语义
      const child = plan
        ? spawn(plan.file, plan.args, { cwd: opts.cwd, env, stdio: ["pipe", "pipe", "pipe"], detached: !isWin })
        : spawn(shellCmd, { cwd: opts.cwd, env, shell: usesShell, stdio: ["pipe", "pipe", "pipe"], detached: !isWin })
      const stdoutChunks: Buffer[] = []
      const stderrChunks: Buffer[] = []
      let settled = false
      const killAll = () => {
        try {
          if (!child.pid) return
          if (isWin) {
            // Windows：taskkill /T 按进程树终止（cmd shell 与其派生的脚本/程序一并杀），
            // 仅 child.kill 只能杀 cmd.exe，孙进程（python/sleep 等）会残留继续运行
            try {
              // taskkill 启动失败（系统缺失等）会异步 emit 'error'：挂监听吞掉，child.kill 兜底
              spawn("taskkill", ["/pid", String(child.pid), "/T", "/F"], { stdio: "ignore" }).on("error", () => {})
            } catch {
              /* taskkill 不可用 */
            }
            try {
              child.kill("SIGKILL")
            } catch {
              /* 已退出 */
            }
          } else {
            try {
              process.kill(-child.pid, "SIGKILL")
            } catch {
              /* 进程组不存在（已退出） */
            }
            try {
              child.kill("SIGKILL")
            } catch {
              /* 已退出 */
            }
          }
        } catch {
          /* 进程已退出 */
        }
      }
      const finishInterrupted = () => {
        if (settled) return
        settled = true
        killAll()
        resolve({ stdout: decodeOutput(Buffer.concat(stdoutChunks)), stderr: `${decodeOutput(Buffer.concat(stderrChunks))}\n[interrupted by user]`, code: 124 })
      }
      // 外部取消：立即终止（区别于超时，stderr 标记 [interrupted by user] 供工具/模型区分）
      const onAbort = () => finishInterrupted()
      if (opts.signal) {
        if (opts.signal.aborted) onAbort()
        else opts.signal.addEventListener("abort", onAbort, { once: true })
      }
      const timer = setTimeout(() => {
        if (!settled) {
          settled = true
          opts.signal?.removeEventListener("abort", onAbort)
          killAll()
          resolve({ stdout: decodeOutput(Buffer.concat(stdoutChunks)), stderr: `${decodeOutput(Buffer.concat(stderrChunks))}\n[timed out after ${timeoutMs}ms]`, code: 124 })
        }
      }, timeoutMs)
      child.stdout.on("data", (d) => stdoutChunks.push(Buffer.isBuffer(d) ? d : Buffer.from(d)))
      child.stderr.on("data", (d) => stderrChunks.push(Buffer.isBuffer(d) ? d : Buffer.from(d)))
      if (opts.input != null) {
        child.stdin.write(opts.input)
      }
      child.stdin.end()
      child.on("error", (err) => {
        if (settled) return
        settled = true
        clearTimeout(timer)
        opts.signal?.removeEventListener("abort", onAbort)
        resolve({ stdout: decodeOutput(Buffer.concat(stdoutChunks)), stderr: String(err), code: 1 })
      })
      child.on("close", (code) => {
        if (settled) return
        settled = true
        clearTimeout(timer)
        opts.signal?.removeEventListener("abort", onAbort)
        resolve({ stdout: decodeOutput(Buffer.concat(stdoutChunks)), stderr: decodeOutput(Buffer.concat(stderrChunks)), code: code ?? 1 })
      })
        }),
    )
  }

  /**
   * 后台任务进程（sh async:true，DESIGN「sh 异步执行」）：与 exec 同规则的 shell（Windows PowerShell / POSIX bash）/env 脱敏/编码/进程组语义，
   * 但不等待完成——stdout+stderr 合并持续写入 opts.logPath（WriteStream 落盘，不占内存），立即返回进程句柄。
   * 无超时（生命周期上限由 ShTaskRunner 惰性检查并 kill）；句柄 kill() 按进程树终止并收尾日志流。
   */
  spawnBackground(
    cmd: string,
    opts: {
      cwd?: string
      env?: Record<string, string>
      logPath: string
      input?: string
      /** 发起用户：豁免用户脚本子进程不剔除敏感变量（与 exec 同规则）。 */
      user?: string
      /** 会话 id（服务模式）：脚本运行根收敛（同 exec：HOME/TEMP/XDG 会话内目录、bwrap 隔离）。 */
      sessionId?: string | null
    },
  ): { pid: number | null; exited: Promise<number>; kill: () => void } {
    // cwd/logPath 父目录保证存在（同步句柄无法 await；与 exec 的 ensureCwd 同语义，缺失目录 spawn 直接 ENOENT）
    try {
      mkdirSync(opts.cwd ?? dirname(opts.logPath), { recursive: true })
      mkdirSync(dirname(opts.logPath), { recursive: true })
      this.prepareScriptDirs(opts.user, opts.sessionId)
    } catch {
      /* 尽力而为：失败按原样 spawn 报错 */
    }
    const merged = { ...process.env, ...opts.env }
    const stripSensitive = this.opts.enabled && (opts.user == null || !this.isExempt(opts.user))
    const filtered = stripSensitive ? Object.fromEntries(Object.entries(merged).filter(([k]) => !isSensitive(k))) : merged
    const env = { ...filtered, ...this.scriptEnv(opts.user, opts.sessionId) }
    const isWin = process.platform === "win32"
    const basePlan = isWin ? winShellPlan(cmd) : posixShellPlan(cmd)
    const plan = isWin ? basePlan : this.scriptPlan({ user: opts.user, sessionId: opts.sessionId, cwd: opts.cwd, shellPlan: basePlan })
    const shellCmd = !basePlan && isWin ? `chcp 65001 >nul && ${cmd}` : cmd
    const child = plan
      ? spawn(plan.file, plan.args, { cwd: opts.cwd, env, stdio: ["pipe", "pipe", "pipe"], detached: !isWin })
      : spawn(shellCmd, { cwd: opts.cwd, env, shell: true, stdio: ["pipe", "pipe", "pipe"], detached: !isWin })
    const log = createWriteStream(opts.logPath, { flags: "a" })
    child.stdout?.pipe(log)
    child.stderr?.pipe(log)
    if (opts.input != null) child.stdin?.write(opts.input)
    child.stdin?.end()
    const exited = new Promise<number>((resolveExit, rejectExit) => {
      child.once("error", (err) => {
        log.end()
        rejectExit(err)
      })
      child.once("close", (code) => {
        log.end()
        resolveExit(code ?? 1)
      })
    })
    const kill = () => {
      const pid = child.pid
      try {
        if (pid == null) return
        if (isWin) {
          try {
            spawn("taskkill", ["/pid", String(pid), "/T", "/F"], { stdio: "ignore" }).on("error", () => {})
          } catch {
            /* taskkill 不可用 */
          }
          try {
            child.kill("SIGKILL")
          } catch {
            /* 已退出 */
          }
        } else {
          try {
            process.kill(-pid, "SIGKILL")
          } catch {
            /* 进程组不存在（已退出） */
          }
          try {
            child.kill("SIGKILL")
          } catch {
            /* 已退出 */
          }
        }
      } catch {
        /* 进程已退出 */
      }
    }
    return { pid: child.pid ?? null, exited, kill }
  }
}
