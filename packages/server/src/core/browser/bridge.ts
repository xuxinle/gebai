/**
 * 浏览器桥接基建（core 域）：node driver.mjs 子进程的 JSON-RPC 桥、playwright 模块/浏览器
 * channel 解析、pwcore 内嵌产物物化、会话串行化锁与全进程共享惰性桥接单例。
 *
 * 自 sub-agents/playwright/playwright_tools.ts 抽取为 core 域模块——桥接是平台级基建
 * （playwright / reverse_site 子Agent 与透明浏览器代理 fetch-proxy 的共同底座），
 * 不依赖任何子Agent 定义的存在（子Agent 被裁剪/停用不影响）。
 *
 * 架构：Bun 运行时与 playwright driver 的 pipe 通信存在兼容问题（chromium 启动超时），
 * 因此不直接 import playwright，而是 spawn 常驻 `node driver.mjs` 子进程，通过
 * stdin/stdout 行分隔 JSON-RPC 通信（见 driver.mjs 顶部协议说明）。浏览器在 node 进程内
 * 单例，BrowserContext 按会话隔离，空闲自动回收。
 */
import { pathToFileURL } from "node:url"
import { dirname, join, normalize, sep } from "node:path"
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { isBinaryMode, resolveGebaiHome } from "../base/config"

const DRIVER_FILE = "driver.mjs"
const REQUEST_TIMEOUT_MS = 180_000 // 单个请求超时（超时即杀进程重启，浏览器状态丢失）
const STDOUT_CHUNK_LIMIT = 1 << 20 // 响应行上限 1MB（超出判定协议异常）

/** driver 脚本绝对路径：源码模式与本文件同目录（dist 非编译形态则与入口同目录）。 */
export function driverPath(): string {
  return join(import.meta.dir, DRIVER_FILE)
}

/**
 * 解析 driver 脚本路径：源码模式直接取同目录文件；二进制（bun --compile）模式从内嵌产物
 * （`core/driver.embedded.generated.json`，构建脚本 `scripts/build-driver-embed.ts` 生成，gzip base64）
 * 物化到 `{GEBAI_HOME}/vendor/playwright/driver.mjs`（与 d2js 同思路的打包闭环）。
 */
export async function resolveDriverFile(): Promise<string> {
  if (!isBinaryMode()) return driverPath()
  const dir = join(resolveGebaiHome(), "vendor", "playwright")
  const file = join(dir, DRIVER_FILE)
  if (!existsSync(file)) {
    const embedded = await import("../driver.embedded.generated.json")
      .then((m) => m.default as { gzip: true; driver: string })
      .catch(() => null)
    if (!embedded) {
      throw new Error("playwright 桥接驱动内嵌产物缺失（构建时请先运行 scripts/build-driver-embed.ts）")
    }
    mkdirSync(dir, { recursive: true })
    writeFileSync(file, Bun.gunzipSync(Buffer.from(embedded.driver, "base64")))
  }
  return file
}

/** playwright 包入口的 file:// URL（Bun 侧解析真实路径，node 进程按此动态 import）。 */
function playwrightModuleUrl(): string {
  // 拼接规避 bun bundler 对字面量的静态解析（playwright 不打包进产物，运行时按需加载）
  const name = "play" + "wright"
  const resolved = Bun.resolveSync(name, import.meta.dir)
  return pathToFileURL(resolved).href
}

/** 浏览器启动 channel（driver 侧 chromium.launch 的 channel 参数）：
 *  GEBAI_PLAYWRIGHT_CHANNEL 覆写（留空 = 不指定）；缺省 Windows 用系统自带 Edge
 *  （msedge，Win10/11 必装，免 playwright install 下载浏览器），其余平台不指定（默认 chromium）。 */
export function resolveChannel(platform: string = process.platform, envValue = process.env.GEBAI_PLAYWRIGHT_CHANNEL): string {
  if (envValue !== undefined) return envValue
  return platform === "win32" ? "msedge" : ""
}

/** 内嵌 playwright-core 产物（scripts/build-pwcore-embed.ts 生成，gitignore）。 */
export interface EmbeddedPwCore {
  version: string
  entry: string
  files: Array<{ path: string; data: string }>
}

let pwcoreMaterializing: Promise<void> | null = null

/** 物化内嵌 playwright-core 到 `{GEBAI_HOME}/vendor/playwright-core/`（版本不一致整目录重建；并发共享一次执行）。导出供测试。 */
export async function materializePwCore(embedded: EmbeddedPwCore): Promise<void> {
  const vendorRoot = join(resolveGebaiHome(), "vendor")
  const dir = join(vendorRoot, "playwright-core")
  const marker = join(vendorRoot, "playwright-core.version")
  if (existsSync(marker) && readFileSync(marker, "utf8") === embedded.version) return
  if (pwcoreMaterializing) {
    await pwcoreMaterializing // 并发调用共享同一次执行（内嵌产物单版本，无需重复物化）
    return
  }
  pwcoreMaterializing = (async () => {
    rmSync(dir, { recursive: true, force: true })
    mkdirSync(vendorRoot, { recursive: true })
    for (const f of embedded.files) {
      const target = join(dir, f.path)
      if (!normalize(target).startsWith(normalize(dir) + sep)) continue // 防穿越（产物自生成，纵深防御）
      mkdirSync(dirname(target), { recursive: true })
      writeFileSync(target, Bun.gunzipSync(Buffer.from(f.data, "base64")))
    }
    writeFileSync(marker, embedded.version)
  })()
  try {
    await pwcoreMaterializing
  } finally {
    pwcoreMaterializing = null // 完成（成功或失败）后复位，后续调用按需重新物化
  }
}

/**
 * 解析 driver 用的 playwright 模块（file:// URL）：二进制形态优先物化内嵌 playwright-core
 * （编译产物中 `Bun.resolveSync` 锚定真实 CWD 的 node_modules 可达性，用户机器上不可依赖），
 * 回退解析 node_modules 的 playwright 包（源码/服务端部署形态）。仅在桥接进程首次启动
 * （首次工具调用）时执行——模块作用域禁止调用（见 createLazyBridge）。
 */
async function resolvePlaywrightModule(): Promise<string> {
  if (isBinaryMode()) {
    const embedded = await import("../pwcore.embedded.generated.json")
      .then((m) => m.default as EmbeddedPwCore)
      .catch(() => null)
    if (embedded) {
      await materializePwCore(embedded)
      return pathToFileURL(join(resolveGebaiHome(), "vendor", "playwright-core", embedded.entry)).href
    }
  }
  try {
    return playwrightModuleUrl()
  } catch (err) {
    throw new Error(
      `playwright 模块解析失败（源码/部署形态需安装 playwright 依赖并 bunx playwright install；单二进制形态需构建时运行 scripts/build-pwcore-embed.ts 生成内嵌产物）: ${err instanceof Error ? err.message : err}`,
    )
  }
}

/* ---------------- 桥接进程 ---------------- */

export interface BridgeLike {
  request: (op: string, args: Record<string, unknown>, timeoutMs?: number) => Promise<unknown>
}

export class Bridge implements BridgeLike {
  private readonly opts: { driverPath?: string; playwrightModule?: string; channel: string; requestTimeoutMs: number }
  private proc: Bun.Subprocess | null = null
  private stdin: Bun.FileSink | null = null
  private pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: Error) => void }>()
  private nextId = 1
  private started = false
  private stderrTail = ""
  private resolvedModule: string | null = null
  private starting: Promise<void> | null = null

  /** 构造零副作用（不解析 playwright 模块——bundle 图模块作用域安全，见 createLazyBridge）：
   *  playwrightModule 显式注入优先，缺省首次启动时解析；channel 同理（resolveChannel）。 */
  constructor(opts: { driverPath?: string; playwrightModule?: string; channel?: string; requestTimeoutMs?: number } = {}) {
    this.opts = {
      driverPath: opts.driverPath,
      playwrightModule: opts.playwrightModule,
      channel: opts.channel ?? resolveChannel(),
      requestTimeoutMs: opts.requestTimeoutMs ?? REQUEST_TIMEOUT_MS,
    }
  }

  async request(op: string, args: Record<string, unknown>, timeoutMs = this.opts.requestTimeoutMs): Promise<unknown> {
    await this.ensureStarted()
    const id = this.nextId++
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        this.kill("请求超时")
        reject(new Error(`playwright 桥接请求超时（${Math.round(timeoutMs / 1000)}s）${op}`))
      }, timeoutMs)
      this.pending.set(id, {
        resolve: (v) => { clearTimeout(timer); resolve(v) },
        reject: (e) => { clearTimeout(timer); reject(e) },
      })
      const line = JSON.stringify({ id, op, args }) + "\n"
      this.stdin?.write(line)
    })
  }

  /** 启动串行化：并发的首次请求共用同一次启动（含 init 注入），防驱动未就绪即下发操作。 */
  private ensureStarted(): Promise<void> {
    if (this.started && this.proc && !this.proc.killed) return Promise.resolve()
    this.starting ??= this.start().finally(() => (this.starting = null))
    return this.starting
  }

  private async start(): Promise<void> {
    const path = this.opts.driverPath ?? (await resolveDriverFile())
    const { existsSync } = await import("node:fs")
    if (!existsSync(path)) {
      throw new Error(
        `playwright 桥接驱动缺失: ${path}（dist 构建需将 driver.mjs 与入口同目录；二进制形态需构建时运行 scripts/build-driver-embed.ts 生成内嵌产物）`,
      )
    }
    const proc = Bun.spawn(["node", path], {
      stdout: "pipe",
      stderr: "pipe",
      stdin: "pipe",
    })
    this.proc = proc
    this.started = true
    this.stderrTail = ""
    this.stdin = proc.stdin as Bun.FileSink
    // stdout 行缓冲：按 id 分发响应。注意：进程退出事件（reader done）异步到达，
    // 期间可能已重启新进程——只有事件属于当前 proc 才清理状态，防误杀新进程。
    const procLocal = proc
    let buf = ""
    const reader = proc.stdout.getReader()
    void (async () => {
      try {
        for (;;) {
          const { done, value } = await reader.read()
          if (done) break
          buf += new TextDecoder().decode(value)
          let nl: number
          while ((nl = buf.indexOf("\n")) >= 0) {
            const line = buf.slice(0, nl)
            buf = buf.slice(nl + 1)
            if (line.length > STDOUT_CHUNK_LIMIT) continue // 异常大行：协议损坏，丢弃
            this.dispatch(line)
          }
        }
      } catch { /* 进程退出 */ }
      if (this.proc === procLocal) this.onExit()
    })()
    // stderr 环形缓冲（保留末尾，用于报错上下文）
    const errReader = proc.stderr.getReader()
    void (async () => {
      try {
        for (;;) {
          const { done, value } = await errReader.read()
          if (done) break
          const text = new TextDecoder().decode(value)
          this.stderrTail = (this.stderrTail + text).slice(-64_000)
        }
      } catch { /* 进程退出 */ }
    })()
    // 首次启动注入 playwright 模块路径与浏览器 channel（模块解析惰性执行，失败即本次请求失败）
    this.resolvedModule ??= this.opts.playwrightModule ?? (await resolvePlaywrightModule())
    await this.request("init", { playwrightModule: this.resolvedModule, channel: this.opts.channel }, 30_000)
  }

  private dispatch(line: string): void {
    let msg: { id?: number; ok?: boolean; result?: unknown; error?: string }
    try {
      msg = JSON.parse(line)
    } catch {
      return
    }
    const p = this.pending.get(msg.id!)
    if (!p) return
    this.pending.delete(msg.id!)
    if (msg.ok) p.resolve(msg.result)
    else p.reject(new Error(msg.error || "playwright 桥接错误"))
  }

  private onExit(): void {
    // 错误信息只回传 stderr 最后一行并截断（driver 日志含路径/URL 等内部细节，避免整体泄漏）
    const lastLine = this.stderrTail.split("\n").filter(Boolean).pop()?.slice(0, 500) ?? ""
    const err = new Error(`playwright 桥接进程退出${lastLine ? `：${lastLine}` : "（无错误输出）"}`)
    for (const p of this.pending.values()) p.reject(err)
    this.pending.clear()
    this.started = false
    this.proc = null
  }

  kill(reason?: string): void {
    if (reason) logBridge(reason)
    const proc = this.proc
    this.onExit() // 先清状态，再杀进程；reader-done 回调因 proc 引用不匹配而不再重复清理
    try {
      proc?.kill()
    } catch { /* 已退出 */ }
  }
}

/** 桥接进程日志（stderr），仅诊断用。 */
function logBridge(msg: string): void {
  console.error(`[playwright-bridge] ${msg}`)
}

/* ---------------- 惰性桥接 ---------------- */

/** 惰性桥接：首次请求才构造 Bridge。bundle 图内的模块禁止模块作用域的第三方包解析
 *  （编译产物中 `Bun.resolveSync` 锚定真实 CWD 的 node_modules 可达性，启动期解析失败会炸掉
 *  整个 bundle 注册表、服务启动即退出）——Bridge 构造已零副作用，此处再延迟到首次工具调用，
 *  解析失败降级为工具级运行时错误（不影响服务启动与其他子Agent）。 */
export function lazyBridge(create: () => Bridge): BridgeLike {
  let bridge: Bridge | null = null
  return {
    request: (op, args, timeoutMs) => {
      bridge ??= create()
      return bridge.request(op, args, timeoutMs)
    },
  }
}

let sharedLazyBridge: BridgeLike | undefined

/** 全进程共享惰性桥接单例：playwright / reverse_site 子Agent 与透明浏览器代理
 *  （fetch-proxy）共用同一桥接进程与浏览器会话——同会话混用时操作的是同一浏览器，状态一致。 */
export function createLazyBridge(): BridgeLike {
  return (sharedLazyBridge ??= lazyBridge(() => new Bridge()))
}

/* ---------------- 会话串行化 ---------------- */

/** 同一会话的浏览器操作串行执行，避免并发操作同一页面互相干扰。 */
const sessionLocks = new Map<string, Promise<unknown>>()
export function withSessionLock<T>(sessionId: string, fn: () => Promise<T>): Promise<T> {
  const prev = sessionLocks.get(sessionId) ?? Promise.resolve()
  const next = prev.then(fn, fn)
  sessionLocks.set(sessionId, next.catch(() => {}))
  return next
}

