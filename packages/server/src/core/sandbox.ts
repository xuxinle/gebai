import { spawn } from "node:child_process"
import { join, resolve } from "node:path"
import { sessionPath } from "./paths"
import { resolveInSandbox } from "./paths"
import { isSensitive } from "./env"

export interface SandboxOptions {
  home: string
  enabled: boolean
  /** 豁免用户判定（如 auth=none 默认用户即操作者本人）：豁免用户不受路径沙箱限制、脚本环境不剔除敏感变量。 */
  isExempt?: (user: string) => boolean
}

/** 输出解码：优先 UTF-8；含替换字符（U+FFFD）时按 GBK 回退（Windows 老程序不遵循 chcp 65001 时仍输出 GBK）。 */
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
  return s
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
   * - 沙箱启用（服务端部署/GEBAI_SANDBOX=on）且用户未豁免：仅允许会话目录内路径，拒绝 `../`、绝对路径、符号链接
   * - 沙箱禁用或用户豁免（本地运行/GEBAI_SANDBOX=off/默认用户豁免）：放开限制——绝对路径直接使用；
   *   相对路径仍基于会话根解析（保持 tmp 产物路径语义），允许越界
   */
  resolvePath(user: string, sessionId: string | null, input: string): string {
    const root = sessionId ? sessionPath(this.opts.home, user, sessionId) : join(this.opts.home, "users", user)
    if (this.enforcedFor(user)) return resolveInSandbox(root, input)
    return resolve(root, input)
  }

  workdir(user: string, sessionId: string): string {
    return join(sessionPath(this.opts.home, user, sessionId), "tmp")
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
    } = {},
  ): Promise<{ stdout: string; stderr: string; code: number }> {
    return new Promise((resolve) => {
      const timeoutMs = opts.timeoutMs ?? 5 * 60 * 1000
      // 沙箱（服务端部署）模式下脚本子进程环境剔除敏感变量（*_KEY/*_TOKEN/*_SECRET/PASSWORD 等）：
      // 脚本（sh/py/cron）是可任意执行的代码，若继承服务端全局密钥（如 OPENAI_API_KEY），
      // 任意用户（含审批跳过场景）可经 env/读取将其外泄；脱敏后脚本仍可用非敏感全局变量，
      // 敏感配置仅限进程内工具（feishu 等）经 ToolContext.env 使用。豁免用户（本地操作者本人）不剔除。
      const merged = { ...process.env, ...opts.env }
      const stripSensitive = this.opts.enabled && (opts.user == null || !this.isExempt(opts.user))
      const env = stripSensitive ? Object.fromEntries(Object.entries(merged).filter(([k]) => !isSensitive(k))) : merged
      // Windows 下 cmd 默认 GBK 代码页：先切 UTF-8（chcp 65001），统一按 UTF-8 解码输出（decodeOutput 兜底 GBK）
      const isWin = process.platform === "win32"
      const shellCmd = isWin && opts.shell !== false ? `chcp 65001 >nul && ${cmd}` : cmd
      // detached：Unix 下子进程成为独立进程组组长，超时/取消可按进程组整体终止（kill(-pid)），
      // 防 shell 被杀后其孙进程（如 sleep/后台任务）残留；
      // Windows 下不使用 detached：实测 detached 子进程的外部程序（.exe）stdout/stderr 管道输出
      // 会完全丢失（cmd 内置命令正常），且 Windows 分支走 taskkill /T 进程树终止，无需进程组语义
      const child = spawn(shellCmd, { cwd: opts.cwd, env, shell: opts.shell !== false, stdio: ["pipe", "pipe", "pipe"], detached: !isWin })
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
    })
  }
}
