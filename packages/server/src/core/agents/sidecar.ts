/**
 * 多语言子代理边车宿主（core/agents/sidecar.ts）：与任意语言实现（Python/C++/Go/…）的
 * 子代理进程通过 NDJSON over stdio 通信（协议见 native-agents/README.md）。
 *
 * 进程管理对齐 CV sidecar（core/cv/sidecar.ts）已验证模式，并按「子代理进程」语义增强：
 * - 惰性启动 + 启动串行化（并发首次请求共用一次启动）
 * - 请求超时：杀进程重启、该次请求拒绝（下次调用用新进程）
 * - 崩溃自愈：进程意外退出自动重启一次并重试在途请求一次（长驻子代理的常见抖动自愈，
 *   如 pip_install 建好 venv 后边车主动退出重启即切换 venv 解释器——无需重启服务）
 * - 自动重启不无限：进程退出后短时间内反复死亡 → 放弃重启（待下次调用再拉起），防抖动风暴
 * - stderr 环形缓冲（16KB，排障）；stdout 协议行解析带行上限防护
 * - 父进程退出清理（process exit hook）+ 驱动侧 stdin EOF 自杀 双保险防孤儿进程
 *
 * 本模块语言无关：spawn 命令来自 manifest（agent.json 的 command/args），不做任何
 * Python 特化——python 是 native-agents/ 下的第一个内置实现，不是机制的一部分。
 */
import { existsSync } from "node:fs"
import { join } from "node:path"
import { isBinaryMode, resolveGebaiHome } from "../base/config"

/** 边车请求默认超时：tool.call 由驱动侧自定义工具决定（AI 库推理可到分钟级）。 */
const REQUEST_TIMEOUT_MS = 120_000
/** init/tools.list 握手超时（进程冷启 + 解释器启动，给足余量）。 */
const HANDSHAKE_TIMEOUT_MS = 30_000
/** stdout 协议行上限（异常大行 = 协议损坏，丢弃防内存膨胀）。 */
const LINE_LIMIT = 1 << 24
/** 崩溃自动重启的冷却判定：连续两次退出间隔不足该值累计为「连续快速退出」，第 3 次放弃自动重启
 *  （防抖动风暴；稳定运行后的单次崩溃总是自愈重发）。 */
const RESTART_BACKOFF_MS = 10_000

/** 子进程抽象（Bun.spawn 子集，测试可注入替身）。 */
export interface SidecarProc {
  stdin: { write(data: string): unknown; end?(): unknown }
  stdout: ReadableStream<Uint8Array>
  stderr: ReadableStream<Uint8Array>
  kill(): void
  readonly killed: boolean
}

export type SidecarSpawnFn = (cmd: string[], opts: { cwd?: string; env?: Record<string, string> }) => SidecarProc

const defaultSpawn: SidecarSpawnFn = (cmd, opts) => {
  const proc = Bun.spawn(cmd, {
    cwd: opts.cwd,
    env: opts.env ?? process.env,
    stdout: "pipe",
    stderr: "pipe",
    stdin: "pipe",
  })
  return {
    stdin: proc.stdin as unknown as SidecarProc["stdin"],
    stdout: proc.stdout as unknown as ReadableStream<Uint8Array>,
    stderr: proc.stderr as unknown as ReadableStream<Uint8Array>,
    kill: () => proc.kill(),
    get killed() {
      return proc.killed
    },
  }
}

/** 边车协议响应（tool.call 结果：ok=false 时 error 携带驱动侧错误文本）。 */
export interface SidecarResponse {
  id?: number | null
  ok?: boolean
  result?: unknown
  error?: string
  [k: string]: unknown
}

interface Pending {
  resolve: (r: SidecarResponse) => void
  reject: (e: Error) => void
  /** 崩溃自愈重试标记：置位后进程重启时重发一次请求。 */
  replay?: { id: number; line: string }
}

/** 单个边车进程宿主：一个 manifest（一个子代理）一个实例，由 native-agents 发现器持有。
 *  command 支持工厂函数：每次启动时解析（占位符如 {python} 需运行时解析——venv 创建后
 *  边车重启即自动切换 venv 解释器，无需重启服务）。 */
export class AgentSidecar {
  private opts: { spawn: SidecarSpawnFn; command: string[] | (() => string[]); cwd?: string; env?: Record<string, string>; requestTimeoutMs: number }
  private proc: SidecarProc | null = null
  private stdin: SidecarProc["stdin"] | null = null
  private pending = new Map<number, Pending>()
  private nextId = 1
  private stderrTail = ""
  private starting: Promise<void> | null = null
  private lastExitAt = 0
  private consecutiveExits = 0
  private exitHookInstalled = false
  private disposed = false
  private buf = ""

  constructor(opts: {
    command: string[] | (() => string[])
    cwd?: string
    env?: Record<string, string>
    spawn?: SidecarSpawnFn
    requestTimeoutMs?: number
  }) {
    this.opts = {
      spawn: opts.spawn ?? defaultSpawn,
      command: opts.command,
      cwd: opts.cwd,
      env: opts.env,
      requestTimeoutMs: opts.requestTimeoutMs ?? REQUEST_TIMEOUT_MS,
    }
  }

  /** 握手 + 工具清单拉取（发现器启动阶段用）。 */
  async init(): Promise<{ name: string; protocol: number; [k: string]: unknown }> {
    const r = await this.request("init", {}, HANDSHAKE_TIMEOUT_MS)
    const info = (r.result ?? {}) as { name?: string; protocol?: number }
    if (!info.name || typeof info.protocol !== "number") {
      throw new Error(`边车 init 响应缺 name/protocol: ${JSON.stringify(r).slice(0, 300)}`)
    }
    return info as { name: string; protocol: number }
  }

  async toolsList(): Promise<Array<{ name: string; description: string; parameters: Record<string, unknown> }>> {
    const r = await this.request("tools.list", {}, HANDSHAKE_TIMEOUT_MS)
    const tools = r.result
    if (!Array.isArray(tools)) throw new Error("边车 tools.list 响应非数组")
    for (const t of tools) {
      if (!t || typeof t.name !== "string" || typeof t.description !== "string" || typeof t.parameters !== "object") {
        throw new Error(`边车 tools.list 条目缺 name/description/parameters: ${JSON.stringify(t).slice(0, 200)}`)
      }
    }
    return tools as Array<{ name: string; description: string; parameters: Record<string, unknown> }>
  }

  /** 工具调用（驱动侧执行自定义工具并返回 {output, data?}）。 */
  async toolCall(tool: string, args: Record<string, unknown>, timeoutMs?: number): Promise<{ output: string; data?: unknown; error?: string }> {
    const r = await this.request("tool.call", { tool, args }, timeoutMs)
    if (!r.ok) return { output: "", error: r.error || "边车工具调用失败" }
    const result = (r.result ?? {}) as { output?: string; data?: unknown }
    return { output: typeof result.output === "string" ? result.output : JSON.stringify(result), data: result.data }
  }

  /** 发送单行请求并等待配对响应。超时：杀进程重启 + 拒绝本次（下次调用拿新进程）。 */
  private async request(op: string, args: Record<string, unknown>, timeoutMs?: number): Promise<SidecarResponse> {
    if (this.disposed) throw new Error("边车已销毁")
    await this.ensureStarted()
    const id = this.nextId++
    const line = JSON.stringify({ id, op, args }) + "\n"
    return new Promise<SidecarResponse>((resolve, reject) => {
      const ms = timeoutMs ?? this.opts.requestTimeoutMs
      const timer = setTimeout(() => {
        this.pending.delete(id)
        this.killAndRestart(`请求超时（${op} ${ms}ms）`)
        reject(new Error(`边车请求超时（${Math.round(ms / 1000)}s）: ${op}`))
      }, ms)
      this.pending.set(id, {
        resolve: (r) => { clearTimeout(timer); resolve(r) },
        reject: (e) => { clearTimeout(timer); reject(e) },
        // 崩溃自愈：进程意外退出时该请求随进程重启重发一次（超时/正常响应不重发）
        replay: { id, line },
      })
      // 写入可能 EPIPE（驱动主动退出如 pip 装完自杀/进程刚死管道已断）：吞掉转为该请求拒绝，
      // 不让未捕获异常打死进程；onExit 的自愈重启照常进行
      try {
        const w = this.stdin?.write(line)
        if (w && typeof (w as Promise<unknown>).catch === "function") (w as Promise<unknown>).catch(() => {})
      } catch {
        this.pending.delete(id)
        clearTimeout(timer)
        reject(new Error("边车写入失败（进程已退出；将自动重启）"))
      }
    })
  }

  /** 启动串行化：并发请求共用一次启动。 */
  private ensureStarted(): Promise<void> {
    if (this.proc && !this.proc.killed) return Promise.resolve()
    this.starting ??= this.start().finally(() => (this.starting = null))
    return this.starting
  }

  private async start(): Promise<void> {
    if (this.disposed) throw new Error("边车已销毁")
    const cmd = typeof this.opts.command === "function" ? this.opts.command() : this.opts.command
    const proc = this.opts.spawn(cmd, { cwd: this.opts.cwd, env: this.opts.env })
    this.proc = proc
    this.stdin = proc.stdin
    this.buf = ""
    this.stderrTail = ""
    if (!this.exitHookInstalled) {
      this.exitHookInstalled = true
      process.on("exit", () => {
        try {
          this.proc?.kill()
        } catch { /* 已退出 */ }
      })
    }
    const procLocal = proc
    const decoder = new TextDecoder()
    void (async () => {
      const reader = proc.stdout.getReader()
      try {
        for (;;) {
          const { done, value } = await reader.read()
          if (done) break
          this.buf += decoder.decode(value, { stream: true })
          for (;;) {
            const nl = this.buf.indexOf("\n")
            if (nl < 0) {
              if (this.buf.length > LINE_LIMIT) this.buf = ""
              break
            }
            const lineText = this.buf.slice(0, nl).replace(/\r$/, "") // 容忍 CRLF 行尾（Windows 驱动 text-mode stdout 默认翻译；协议健壮性）
            this.buf = this.buf.slice(nl + 1)
            if (!lineText.trim()) continue
            try {
              this.dispatch(JSON.parse(lineText) as SidecarResponse)
            } catch { /* 非 JSON 行：协议损坏，忽略 */ }
          }
        }
      } catch { /* 进程退出 */ }
      if (this.proc === procLocal) this.onExit()
    })()
    const errDecoder = new TextDecoder()
    void (async () => {
      const reader = proc.stderr.getReader()
      try {
        for (;;) {
          const { done, value } = await reader.read()
          if (done) break
          this.stderrTail = (this.stderrTail + errDecoder.decode(value, { stream: true })).slice(-16_000)
        }
      } catch { /* 进程退出 */ }
    })()
  }

  private dispatch(msg: SidecarResponse): void {
    if (msg.id == null) return
    const p = this.pending.get(msg.id)
    if (!p) return
    this.pending.delete(msg.id)
    if (msg.ok === false) p.reject(Object.assign(new Error(msg.error || "边车错误"), { sidecarResponse: msg }))
    else p.resolve(msg)
  }

  /** 进程意外退出：短冷却内非频繁 → 自动重启 + 在途请求重发一次；频繁死亡 → 拒绝在途请求、
   *  不再自动拉起（下次调用 ensureStarted 再试）。 */
  private onExit(): void {
    const procWasAlive = !!this.proc
    this.proc = null
    this.stdin = null
    if (this.disposed) {
      this.rejectAll(new Error("边车已销毁"))
      return
    }
    const now = Date.now()
    const rapidExit = this.lastExitAt > 0 && now - this.lastExitAt < RESTART_BACKOFF_MS
    this.consecutiveExits = rapidExit ? this.consecutiveExits + 1 : 1
    this.lastExitAt = now
    if (procWasAlive && this.consecutiveExits < 3) {
      // 崩溃自愈：拉起新进程并在启动完成后重发在途请求（各重发一次，重发后不再带 replay）
      void this.ensureStarted()
        .then(() => {
          // 已重试过（无 replay 标记）的在途请求立即拒绝：重试名额已用——典型是该请求本身
          // 触发进程退出（os._exit/致命崩溃），再发只会杀死新进程，等超时白等
          for (const [id, p] of [...this.pending]) {
            if (p.replay) continue
            this.pending.delete(id)
            p.reject(new Error("边车进程在请求重试后仍退出（该请求可能触发进程退出，已放弃重试；后续调用用新进程）"))
          }
          for (const [id, p] of [...this.pending]) {
            const rp = p.replay
            if (!rp) continue
            const { resolve, reject } = p
            const timer = setTimeout(() => {
              this.pending.delete(id)
              this.killAndRestart(`重试请求超时（id ${id}）`)
              reject(new Error("边车请求重试超时"))
            }, this.opts.requestTimeoutMs)
            this.pending.set(id, {
              resolve: (r) => { clearTimeout(timer); resolve(r) },
              reject: (e) => { clearTimeout(timer); reject(e) },
              // 重发请求不再登记 replay（新进程再死即按抖动处理，拒绝防无限循环）
            })
            // 重发请求不再登记 replay（新进程再死即按抖动处理，拒绝防无限循环）；写入 EPIPE 同样吞掉
            try {
              const w = this.stdin?.write(rp.line)
              if (w && typeof (w as Promise<unknown>).catch === "function") (w as Promise<unknown>).catch(() => {})
            } catch { /* 新进程天折：请求由超时/退出路径收尾 */ }
          }
        })
        .catch(() => this.rejectAll(new Error("边车崩溃后重启失败")))
      return
    }
    const reason = this.consecutiveExits >= 3 ? `边车进程连续快速退出（${this.consecutiveExits} 次），放弃自动重启` : "边车进程退出"
    this.rejectAll(new Error(this.exitErrorText(reason)))
  }

  private exitErrorText(prefix: string): string {
    const lastLine = this.stderrTail.split("\n").filter(Boolean).pop()?.slice(0, 500) ?? ""
    return lastLine ? `${prefix}：${lastLine}` : `${prefix}（无错误输出）`
  }

  private rejectAll(err: Error): void {
    for (const p of this.pending.values()) p.reject(err)
    this.pending.clear()
  }

  private killAndRestart(reason: string): void {
    console.error(`[native-agent] ${reason}`)
    const proc = this.proc
    this.proc = null
    this.stdin = null
    this.rejectAll(new Error(reason))
    try {
      proc?.kill()
    } catch { /* 已退出 */ }
  }

  /** 主动销毁（发现器重扫/服务关闭）：杀进程、拒绝在途、后续请求报已销毁。 */
  dispose(reason?: string): void {
    this.disposed = true
    this.killAndRestart(reason ? `边车已销毁：${reason}` : "边车已销毁")
  }

  /** 最近 stderr 尾部（排障/错误附因用）。 */
  stderrPreview(): string {
    return this.stderrTail.slice(-2000)
  }
}

/** 内置多语言子代理源目录解析：源码形态 src/core/agents → 仓库内 native-agents/（与 src 同级）；
 *  dist 形态（bun build 产物，模块位于 dist 根）→ 同目录 native-agents/（build-subagents 构建
 *  时整树复制）；二进制形态（bun --compile 单文件）→ {GEBAI_HOME}/vendor/native-agents/
 *  （安装包模式预置物化）。 */
export function nativeAgentsSourceDir(): string {
  if (isBinaryMode()) return join(resolveGebaiHome(), "vendor", "native-agents")
  const srcForm = join(import.meta.dirname, "..", "..", "..", "native-agents")
  if (existsSync(srcForm)) return srcForm
  return join(import.meta.dirname, "native-agents")
}
