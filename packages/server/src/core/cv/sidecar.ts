/**
 * CV GPU sidecar 客户端（core/cv）：spawn 常驻 node 子进程运行 cv-driver.mjs（onnxruntime-node
 * 原生推理），供检测类重模型走 GPU（Windows dml / cuda / macOS coreml，逐级探测显式上报，
 * 全失败回落 native cpu 多线程）——Bun 进程内 wasm 单线程是数量级瓶颈（YOLO11-L@1280 ≈ 350
 * GFLOPs，wasm 数十秒级，GPU 0.1-0.5s）。进程管理仿浏览器桥接（core/browser/bridge.ts）：
 * 惰性启动、请求超时杀进程重启、stderr 环形缓冲。协议：行分隔 JSON 头 + 定长原始字节帧
 * （见 cv-driver.mjs 顶部说明）。OCR 等轻推理不迁移（wasm 0.5-2s 已达标），仅检测消费。
 *
 * onnxruntime-node 不随构建内嵌（体积/许可）：解析顺序 GEBAI_CV_ORT_NODE_DIR →
 * {GEBAI_HOME}/vendor/onnxruntime-node（内嵌形态预留）→ node_modules（源码/部署形态安装了
 * 该依赖时）；全部不可解析 → sidecar 不可用 → 检测自动回落 wasm（保留现有方案兜底）。
 */
import { dirname, join } from "node:path"
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs"
import { isBinaryMode, resolveGebaiHome } from "../base/config"

const DRIVER_FILE = "cv-driver.mjs"
const REQUEST_TIMEOUT_MS = 120_000
const HEADER_LINE_LIMIT = 1 << 20 // 响应头行上限（帧数据走二进制通道，头恒小）

/** 子进程抽象（Bun.spawn 子集，测试可注入替身）。 */
export interface SidecarProc {
  stdin: { write(data: string | Uint8Array): unknown; flush?(): unknown }
  stdout: ReadableStream<Uint8Array>
  stderr: ReadableStream<Uint8Array>
  kill(): void
  readonly killed: boolean
}

export type SidecarSpawn = (cmd: string[]) => SidecarProc

const defaultSpawn: SidecarSpawn = (cmd) => {
  const proc = Bun.spawn(cmd, { stdout: "pipe", stderr: "pipe", stdin: "pipe" })
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

/** 驱动脚本路径：源码模式与本文件同目录；二进制（bun --compile）形态从内嵌产物
 *  （./cvdriver.embedded.generated.json，构建脚本 scripts/build-cvdriver-embed.ts 生成）
 *  物化到 {GEBAI_HOME}/vendor/cv/（与 playwright driver 同思路的打包闭环）。 */
export async function resolveCvDriverFile(): Promise<string> {
  if (!isBinaryMode()) return join(import.meta.dir, DRIVER_FILE)
  const dir = join(resolveGebaiHome(), "vendor", "cv")
  const file = join(dir, DRIVER_FILE)
  if (!existsSync(file)) {
    const embedded = await import("./cvdriver.embedded.generated.json")
      .then((m) => m.default as { gzip: true; driver: string })
      .catch(() => null)
    if (!embedded?.driver) {
      throw new Error("CV sidecar 驱动内嵌产物缺失（构建时请先运行 scripts/build-cvdriver-embed.ts）")
    }
    mkdirSync(dir, { recursive: true })
    writeFileSync(file, Bun.gunzipSync(Buffer.from(embedded.driver, "base64")))
  }
  return file
}

/** 从包入口文件路径向上找 onnxruntime-node 包根（含 package.json 且 name 匹配的目录）。 */
function ortNodePackageRoot(entry: string): string | null {
  let dir = entry
  for (let i = 0; i < 4; i++) {
    dir = dirname(dir)
    const pkg = join(dir, "package.json")
    if (existsSync(pkg)) {
      try {
        if (JSON.parse(readFileSync(pkg, "utf8")).name === "onnxruntime-node") return dir
      } catch { /* 损坏的 package.json 视为不匹配 */ }
    }
  }
  return null
}

/**
 * onnxruntime-node 解析（不进 bundle 图——运行时解析，bundle 注册表启动安全）：
 * GEBAI_CV_ORT_NODE_DIR（包根或其父目录）→ **{GEBAI_HOME}/models/vendor/node_modules**
 * （资源子仓库约定位置——npm 整包含依赖放入即生效，见 models/README.md；包内
 * onnxruntime-common 等依赖经 node 标准向上查找天然可用）→ {GEBAI_HOME}/vendor →
 * node_modules（源码/部署形态）。不可解析返回 null（sidecar 不可用，检测回落 wasm）。
 */
export function resolveOrtNodeDir(env: Record<string, string> = process.env as Record<string, string>): string | null {
  const explicit = String(env.GEBAI_CV_ORT_NODE_DIR ?? "").trim()
  if (explicit) return existsSync(join(explicit, "onnxruntime-node", "package.json")) ? join(explicit, "onnxruntime-node") : explicit
  try {
    const home = resolveGebaiHome()
    for (const prefix of [join(home, "models", "vendor"), join(home, "vendor")]) {
      // 两种布局：prefix/onnxruntime-node（裸包）或 prefix/node_modules/onnxruntime-node（npm 布局含依赖闭包）
      for (const pkg of [join(prefix, "node_modules", "onnxruntime-node"), join(prefix, "onnxruntime-node")]) {
        if (existsSync(join(pkg, "package.json"))) return pkg
      }
    }
  } catch { /* GEBAI_HOME 不可解析（测试环境等）——继续走 node_modules 探查 */ }
  try {
    // 拼接规避 bundler 对字面量的静态解析（运行时按需加载，与 ort-loader 同款）
    const resolved = Bun.resolveSync("onnxruntime-" + "node", import.meta.dir)
    return ortNodePackageRoot(resolved)
  } catch {
    return null
  }
}

export interface SidecarRunOutput {
  dims: readonly number[]
  data: Float32Array
}

interface PendingEntry {
  resolve: (v: { result: unknown; bytes?: Uint8Array }) => void
  reject: (e: Error) => void
}

/** sidecar 客户端：常驻子进程 + 请求/响应（JSON 头行 + 可选二进制帧）。 */
export class CvSidecar {
  private readonly opts: { spawn: SidecarSpawn; driverPath?: string; ortNodeDir: string | null; requestTimeoutMs: number }
  private proc: SidecarProc | null = null
  private stdin: SidecarProc["stdin"] | null = null
  private pending = new Map<number, PendingEntry>()
  private nextId = 1
  private started = false
  private initialized = false
  private stderrTail = ""
  private starting: Promise<void> | null = null
  private sessions = new Map<string, { sessionId: number; ep: string }>()
  private exitHookInstalled = false

  constructor(opts: { spawn?: SidecarSpawn; driverPath?: string; ortNodeDir?: string | null; requestTimeoutMs?: number } = {}) {
    this.opts = {
      spawn: opts.spawn ?? defaultSpawn,
      driverPath: opts.driverPath,
      ortNodeDir: opts.ortNodeDir === undefined ? resolveOrtNodeDir() : opts.ortNodeDir,
      requestTimeoutMs: opts.requestTimeoutMs ?? REQUEST_TIMEOUT_MS,
    }
  }

  /** 实际落地的执行提供者（session.create 上报；供输出如实标注后端）。 */
  async createSession(modelPath: string, ep: string): Promise<{ sessionId: number; ep: string; tried: string[] }> {
    const r = await this.request("session.create", { modelPath, ep })
    return r.result as { sessionId: number; ep: string; tried: string[] }
  }

  /** 张量推理：dims [1,3,H,W]，data 长度 = H*W*3；返回第一个输出张量。 */
  async runTensor(sessionId: number, dims: readonly number[], data: Float32Array): Promise<SidecarRunOutput> {
    const r = await this.request("session.run", { sessionId, dims: Array.from(dims) }, { data, dims })
    if (!r.bytes) throw new Error("sidecar 响应缺少输出张量帧")
    const u8 = new Uint8Array(r.bytes.length)
    u8.set(r.bytes)
    return { dims: (r.result as { dims: number[] }).dims, data: new Float32Array(u8.buffer) }
  }

  /** 模型推理（会话按模型键缓存；进程重启自动重建）——通用 ONNX 会话运行入口，
   *  检测与 OCR（det/rec）共用；modelKey 建议含文件大小（模型变更自动重建会话）。 */
  async runModel(opts: {
    modelKey: string
    modelPath: string
    ep: string
    dims: readonly number[]
    data: Float32Array
  }): Promise<SidecarRunOutput & { ep: string }> {
    let session = this.sessions.get(opts.modelKey)
    if (!session) {
      const created = await this.createSession(opts.modelPath, opts.ep)
      session = { sessionId: created.sessionId, ep: created.ep }
      this.sessions.set(opts.modelKey, session)
    }
    const out = await this.runTensor(session.sessionId, opts.dims, opts.data)
    return { ...out, ep: session.ep }
  }

  private async request(
    op: string,
    args: Record<string, unknown>,
    tensor?: { data: Float32Array; dims: readonly number[] },
  ): Promise<{ result: unknown; bytes?: Uint8Array }> {
    await this.ensureStarted()
    const id = this.nextId++
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        this.kill(`请求超时（${op}）`)
        reject(new Error(`CV sidecar 请求超时（${Math.round(this.opts.requestTimeoutMs / 1000)}s）: ${op}`))
      }, this.opts.requestTimeoutMs)
      this.pending.set(id, {
        resolve: (v) => { clearTimeout(timer); resolve(v) },
        reject: (e) => { clearTimeout(timer); reject(e) },
      })
      const bytes = tensor ? new Uint8Array(tensor.data.buffer, tensor.data.byteOffset, tensor.data.byteLength) : null
      const header: Record<string, unknown> = { id, op, args }
      if (bytes) header.tensorBytes = bytes.length
      this.stdin?.write(JSON.stringify(header) + "\n")
      if (bytes) this.stdin?.write(bytes)
      this.stdin?.flush?.()
    })
  }

  /** 启动串行化：并发的首次请求共用同一次启动（init 注入 ort 目录）。 */
  private ensureStarted(): Promise<void> {
    if (this.started && this.proc && !this.proc.killed) return Promise.resolve()
    this.starting ??= this.start().finally(() => (this.starting = null))
    return this.starting
  }

  private async start(): Promise<void> {
    const path = this.opts.driverPath ?? (await resolveCvDriverFile())
    if (!existsSync(path)) {
      throw new Error(`CV sidecar 驱动缺失: ${path}（dist 构建需复制 cv-driver.mjs 与入口同目录；二进制形态需构建时运行 scripts/build-cvdriver-embed.ts）`)
    }
    const proc = this.opts.spawn(["node", path])
    this.proc = proc
    this.started = true
    this.initialized = false
    this.stderrTail = ""
    this.stdin = proc.stdin
    // 父进程退出时杀子进程：process.exit 不保证关闭 stdio 管道（驱动侧 stdin EOF 兜底
    // 因此失效），一次性脚本/异常退出会留下孤儿 node 进程（实测 ~600MB/个持 GPU 会话）
    if (!this.exitHookInstalled) {
      this.exitHookInstalled = true
      process.on("exit", () => {
        try {
          this.proc?.kill()
        } catch { /* 已退出 */ }
      })
    }
    const procLocal = proc
    let buf = Buffer.alloc(0)
    let frame: { header: { id: number; result: unknown; outBytes: number }; parts: Buffer[] } | null = null
    const reader = proc.stdout.getReader()
    void (async () => {
      try {
        for (;;) {
          const { done, value } = await reader.read()
          if (done) break
          buf = Buffer.concat([buf, Buffer.from(value)])
          for (;;) {
            if (frame) {
              const need = frame.header.outBytes - frame.parts.reduce((n, b) => n + b.length, 0)
              if (buf.length < need) break
              const bytes = Buffer.concat([...frame.parts, buf.subarray(0, need)])
              buf = buf.subarray(need)
              this.dispatch(frame.header, new Uint8Array(bytes))
              frame = null
            } else {
              const nl = buf.indexOf(0x0a)
              if (nl < 0) {
                if (buf.length > HEADER_LINE_LIMIT) buf = Buffer.alloc(0) // 异常大行：协议损坏，丢弃
                break
              }
              const line = buf.subarray(0, nl).toString("utf8")
              buf = buf.subarray(nl + 1)
              let msg: { id?: number; ok?: boolean; result?: unknown; error?: string; outBytes?: number }
              try {
                msg = JSON.parse(line)
              } catch {
                continue
              }
              if (msg.ok && msg.outBytes && msg.id !== undefined) {
                frame = { header: { id: msg.id, result: msg.result, outBytes: msg.outBytes }, parts: [] }
              } else {
                this.dispatchLine(msg)
              }
            }
          }
        }
      } catch { /* 进程退出 */ }
      if (this.proc === procLocal) this.onExit()
    })()
    const errReader = proc.stderr.getReader()
    void (async () => {
      try {
        for (;;) {
          const { done, value } = await errReader.read()
          if (done) break
          this.stderrTail = (this.stderrTail + new TextDecoder().decode(value)).slice(-16_000)
        }
      } catch { /* 进程退出 */ }
    })()
    if (!this.initialized) {
      await this.request("init", { ortDir: this.opts.ortNodeDir })
      this.initialized = true
    }
  }

  private dispatchLine(msg: { id?: number; ok?: boolean; result?: unknown; error?: string }): void {
    const p = this.pending.get(msg.id!)
    if (!p) return
    this.pending.delete(msg.id!)
    if (msg.ok) p.resolve({ result: msg.result })
    else p.reject(new Error(msg.error || "CV sidecar 错误"))
  }

  private dispatch(header: { id: number; result: unknown }, bytes: Uint8Array): void {
    const p = this.pending.get(header.id)
    if (!p) return
    this.pending.delete(header.id)
    p.resolve({ result: header.result, bytes })
  }

  private onExit(): void {
    const lastLine = this.stderrTail.split("\n").filter(Boolean).pop()?.slice(0, 500) ?? ""
    const err = new Error(`CV sidecar 进程退出${lastLine ? `：${lastLine}` : "（无错误输出）"}`)
    for (const p of this.pending.values()) p.reject(err)
    this.pending.clear()
    this.sessions.clear() // 进程已死，sessionId 失效；下次调用重新建会话
    this.started = false
    this.initialized = false
    this.proc = null
  }

  kill(reason?: string): void {
    if (reason) console.error(`[cv-sidecar] ${reason}`)
    const proc = this.proc
    this.onExit()
    try {
      proc?.kill()
    } catch { /* 已退出 */ }
  }
}

/* ---------------- 惰性共享单例 ---------------- */

let sharedSidecar: CvSidecar | null = null
let cvSidecarProbed = false
let cvSidecarPoisoned = false
let sidecarFactoryOverride: (() => CvSidecar | null) | null = null

/** 测试注入：整体替身（返回 null 模拟 sidecar 不可用）。 */
export function setCvSidecarFactoryForTests(f: (() => CvSidecar | null) | null): void {
  sidecarFactoryOverride = f
  sharedSidecar = null
}

/** 测试复位：清空毒化/探测/单例/替身状态（afterAll 调用，防跨测试文件泄漏）。 */
export function resetCvSidecarForTests(): void {
  cvSidecarPoisoned = false
  cvSidecarProbed = false
  sharedSidecar = null
  sidecarFactoryOverride = null
}

/** 毒化：sidecar 运行期失败（超时/崩溃/驱动报错）后本进程生命周期内不再重试——避免
 *  auto 回落路径上每次检测都先等一遍 sidecar 超时再回落 wasm。 */
export function poisonCvSidecar(reason?: string): void {
  cvSidecarPoisoned = true
  if (reason) console.error(`[cv-sidecar] 已毒化，检测回落 wasm: ${reason.slice(0, 300)}`)
}

/** 全进程共享惰性 sidecar 单例：驱动缺失 / ort-node 不可解析 / 运行期失败毒化 → null（检测回落 wasm）。
 *  毒化优先于替身工厂（毒化的 sidecar 保持死亡，不因注入替身复活）。可用性探测一次性
 *  （进程生命周期内不重试——避免每次检测都做文件系统探测）。 */
export function cvSidecarClient(): CvSidecar | null {
  if (cvSidecarPoisoned) return null
  if (sidecarFactoryOverride) return sidecarFactoryOverride()
  if (sharedSidecar === null && !cvSidecarProbed) {
    cvSidecarProbed = true
    if (resolveOrtNodeDir() !== null) sharedSidecar = new CvSidecar()
  }
  return sharedSidecar
}
