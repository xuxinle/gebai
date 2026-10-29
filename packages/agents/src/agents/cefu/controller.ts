/**
 * 册府控制器：TS 直接控制霜天册府应用（`shuangtian/examples/cefu`）。
 *
 * ## 形态：宿主 spawn 无头册府 + 控制通道调桥函数
 *
 * ```
 * 册府（headless C++ 进程）◀──TCP 长度前缀 JSON-RPC──▶ 本控制器（跑在 gebai 宿主里）
 *   --headless --control-port 0 --control-file ctl.json        script {code:"app.cefu.search(...)"}
 * ```
 *
 * 与客卿边车（旧 keqing/cpp/cefu，已移除）的区别：那条路是「册府逻辑重新实现成
 * 无 UI 边车」，本路是「同一份霜天应用跑无头模式」——**一份 C++ 代码两个形态**
 * （有头给人看、无头给歌白用），桥函数（`cefu.*`）与数据（SQLite）完全同源，
 * 不会再出现「边车版加了功能、应用版没跟上」的分叉。
 *
 * ## 为什么控制器是进程级单例（而不是会话级）
 *
 * 册府进程持有 SQLite 连接与索引内存态，多实例 = 多个写者竞争同一个库文件。
 * 工具调用时懒启动（第一次用到才 spawn），进程死了下次调用自动重拉。
 *
 * ## 控制协议（与霜天 e2e `tools/cefu_e2e.py` 同一份）
 *
 * - 帧格式：4 字节大端长度 + UTF-8 JSON `{id, method, params}`
 * - 首个请求必须是 `hello`（params 带 control-file 里的 token）
 * - 桥函数调用走 `script` 方法的 `code` 分支：eval 表达式 `app.cefu.<fn>(args...)`
 *   （桥函数挂 JS 全局 `app` 对象下，`script {function}` 只能调全局函数名，
 *   含点号的名字解析不到——实测用 code 走 eval 是唯一通路）
 */

import { existsSync, mkdirSync, readFileSync, rmSync } from "node:fs"
import { spawn, type ChildProcess } from "node:child_process"
import { dirname, isAbsolute, join, resolve } from "node:path"
import { resolveDataPath } from "@gebai/sdk/node"
import { takeoverProcess } from "../../core/process-takeover"

/** 册府数据目录名（`{GEBAI_HOME}/data/cefu`）。 */
export const CEFU_HOME_DIRNAME = "cefu"

/** 可执行文件名：Windows 带 `.exe`（spawn 不补后缀）。 */
export const CEFU_EXE_NAME = process.platform === "win32" ? "cefu.exe" : "cefu"

/** 构建档探测顺序（与相期同表：dev 主用，quick/release 发行，debug 兜底）。 */
export const CEFU_PROFILES = ["dev", "quick", "release", "debug"] as const

/** 仓库标记：`shuangtian/examples/cefu/ui` 在源码树存在、发行产物不存在。 */
const REPO_MARKER = ["shuangtian", "examples", "cefu", "ui"] as const

const MAX_UP = 16

// ── exe 探测（纯函数，测试可注入） ──────────────────────────────────────────

export interface CefuProbeInput {
  gebaiHome: string
  /** 服务自身所在目录（生产传 `import.meta.dirname`；测试注入）。 */
  moduleDir: string
  /** `CEFU_BIN` 覆盖。 */
  override?: string
  profiles?: readonly string[]
  exists?: (p: string) => boolean
  /** PATH 查找（测试注入；缺省 Bun.which）。 */
  which?: (name: string) => string | null
}

export interface CefuBinResult {
  bin: string | null
  repoRoot: string | null
  candidates: string[]
  fromOverride: boolean
}

function findRepoRoot(start: string, exists: (p: string) => boolean): string | null {
  let cur = resolve(start)
  for (let i = 0; i < MAX_UP; i++) {
    if (exists(join(cur, ...REPO_MARKER))) return cur
    const parent = dirname(cur)
    if (parent === cur) break
    cur = parent
  }
  return null
}

/**
 * 探测册府 exe：`CEFU_BIN` 覆盖 > `{GEBAI_HOME}/shuangtian/build/<档>` >
 * 仓库树上溯 > `{GEBAI_HOME}/cefu(.exe)` 直查 > `PATH`。
 * （与相期 `resolveXiangqiBin` 同构，探测面宁宽勿狭。）
 */
export function resolveCefuBin(input: CefuProbeInput): CefuBinResult {
  const exists = input.exists ?? existsSync
  const profiles = input.profiles ?? CEFU_PROFILES
  const which = input.which ?? ((name: string) => Bun.which(name) ?? null)
  const override = input.override?.trim()
  if (override) {
    const abs = isAbsolute(override) ? override : resolve(override)
    const ok = exists(abs)
    return { bin: ok ? abs : null, repoRoot: ok ? findRepoRoot(dirname(abs), exists) : null, candidates: [abs], fromOverride: true }
  }
  const candidates: string[] = []
  const push = (p: string): void => {
    if (!candidates.includes(p)) candidates.push(p)
  }
  for (const profile of profiles) push(join(input.gebaiHome, "shuangtian", "build", profile, "bin", CEFU_EXE_NAME))
  const fromModule = findRepoRoot(input.moduleDir, exists)
  const fromHome = findRepoRoot(input.gebaiHome, exists)
  const roots = [fromModule, fromHome].filter((r): r is string => r !== null)
  for (const root of roots) {
    for (const profile of profiles) push(join(root, "shuangtian", "build", profile, "bin", CEFU_EXE_NAME))
  }
  push(join(input.gebaiHome, CEFU_EXE_NAME))
  const onPath = which(CEFU_EXE_NAME)
  if (onPath) push(onPath)
  for (const candidate of candidates) {
    if (exists(candidate)) {
      return { bin: candidate, repoRoot: roots[0] ?? findRepoRoot(dirname(candidate), exists), candidates, fromOverride: false }
    }
  }
  return { bin: null, repoRoot: fromModule ?? fromHome, candidates, fromOverride: false }
}

/** 册府 UI 资源目录（`main.xml`）：无头模式也要（应用装载失败会启动即退出）。 */
export function resolveCefuUiDir(repoRoot: string | null, exists: (p: string) => boolean = existsSync): string | null {
  if (repoRoot === null) return null
  const dir = join(repoRoot, "shuangtian", "examples", "cefu", "ui")
  return exists(join(dir, "main.xml")) ? dir : null
}

// ── 数据根（与相期同一公式口径） ────────────────────────────────────────────

export function cefuHomeFor(gebaiHome: string, env: Record<string, string | undefined>): string {
  const explicit = env.CEFU_HOME?.trim()
  if (explicit) return explicit
  const home = (gebaiHome || env.GEBAI_HOME || "").trim()
  return resolveDataPath(home === "" ? process.cwd() : home, CEFU_HOME_DIRNAME, CEFU_HOME_DIRNAME)
}

// ── 控制通道客户端（长度前缀 JSON-RPC） ─────────────────────────────────────

interface RpcResponse {
  id: number
  ok?: boolean
  result?: unknown
  error?: string | { message?: string } | Record<string, unknown>
}

/** 一条已建立的册府控制连接（hello 已过）。 */
export class CefuConnection {
  private rid = 0
  private constructor(
    private readonly socket: { write(data: Uint8Array): void; end(): void },
    private readonly read: (timeoutMs: number) => Promise<Buffer>,
  ) {}

  /** 从已连 socket 建立会话（hello 握手）。 */
  static async handshake(
    socket: { write(data: Uint8Array): void; end(): void; on(event: "data", cb: (d: Buffer) => void): void },
    readChunk: (timeoutMs: number) => Promise<Buffer>,
    token: string,
  ): Promise<CefuConnection> {
    const conn = new CefuConnection(socket, readChunk)
    await conn.call("hello", { token }, 10_000)
    return conn
  }

  private async call(method: string, params: Record<string, unknown>, timeoutMs: number): Promise<unknown> {
    this.rid += 1
    const body = Buffer.from(JSON.stringify({ id: this.rid, method, params }), "utf8")
    const frame = Buffer.alloc(4 + body.length)
    frame.writeUInt32BE(body.length, 0)
    body.copy(frame, 4)
    this.socket.write(frame)
    const raw = await this.read(timeoutMs)
    const parsed = JSON.parse(raw.toString("utf8")) as RpcResponse
    if (parsed.ok === false) {
      const msg = typeof parsed.error === "string" ? parsed.error : JSON.stringify(parsed.error ?? {})
      throw new Error(`册府控制通道 ${method} 失败：${msg}`)
    }
    return parsed.result
  }

  /** eval 一段 JS 表达式（桥函数调用通路：`app.cefu.search(...)`）。 */
  async eval(code: string, timeoutMs = 60_000): Promise<unknown> {
    const r = (await this.call("script", { code }, timeoutMs)) as { result?: unknown } | undefined
    return r?.result
  }

  /** 优雅退出册府进程。 */
  async shutdown(timeoutMs = 5_000): Promise<void> {
    await this.call("shutdown", {}, timeoutMs).catch(() => {})
    this.socket.end()
  }
}

/** 从 socket 数据流读一帧（4 字节大端长度 + JSON body）。 */
export function makeFrameReader(socket: { on(event: "data", cb: (d: Buffer) => void): void }): (timeoutMs: number) => Promise<Buffer> {
  let buffer = Buffer.alloc(0)
  const waiters: Array<{ resolve: (b: Buffer) => void; reject: (e: Error) => void; timer: NodeJS.Timeout }> = []
  socket.on("data", (chunk: Buffer) => {
    buffer = Buffer.concat([buffer, chunk])
    for (;;) {
      const waiter = waiters[0]
      if (!waiter) break
      if (buffer.length < 4) break
      const len = buffer.readUInt32BE(0)
      if (buffer.length < 4 + len) break
      const body = buffer.subarray(4, 4 + len)
      buffer = buffer.subarray(4 + len)
      clearTimeout(waiter.timer)
      waiters.shift()
      waiter.resolve(body)
    }
  })
  return (timeoutMs: number) =>
    new Promise<Buffer>((resolveP, rejectP) => {
      const timer = setTimeout(() => {
        const i = waiters.findIndex((w) => w.timer === timer)
        if (i >= 0) waiters.splice(i, 1)
        rejectP(new Error(`册府控制通道读取超时（${timeoutMs}ms）`))
      }, timeoutMs)
      waiters.push({ resolve: resolveP, reject: rejectP, timer })
    })
}

// ── 控制器（进程级单例） ────────────────────────────────────────────────────

export interface CefuControllerOptions {
  /** 测试注入。 */
  gebaiHome?: string
  env?: Record<string, string | undefined>
  spawnFn?: (bin: string, args: string[]) => ChildProcess
  connect?: (port: number) => Promise<{ socket: CefuConnection["socket"] & { on(event: "data", cb: (d: Buffer) => void): void }; port: number }>
  log?: { info(msg: string): void; warn(msg: string): void }
}

interface CefuRuntime {
  proc: ChildProcess
  conn: CefuConnection
  ctlFile: string
  bin: string
}

let runtime: CefuRuntime | null = null
let starting: Promise<CefuRuntime> | null = null

export function resetCefuForTest(): void {
  runtime = null
  starting = null
}

/** 册府当前状态（诊断/工具输出用）。 */
export function cefuState(): { running: boolean; pid?: number; bin?: string } {
  if (!runtime) return { running: false }
  return { running: !runtime.proc.killed, pid: runtime.proc.pid, bin: runtime.bin }
}

/** 读 control-file（册府就绪后写入 port+token）。 */
function readCtlFile(path: string): { port: number; token: string } | null {
  try {
    const raw = readFileSync(path, "utf8").trim()
    if (!raw) return null
    const parsed = JSON.parse(raw) as { port?: number; token?: string }
    if (parsed.port && parsed.token) return { port: Number(parsed.port), token: String(parsed.token) }
    return null
  } catch {
    return null
  }
}

async function defaultConnect(port: number): Promise<{ socket: import("node:net").Socket; port: number }> {
  const net = await import("node:net")
  return new Promise((resolveP, rejectP) => {
    const socket = net.connect(port, "127.0.0.1")
    socket.once("connect", () => resolveP({ socket, port }))
    socket.once("error", (err) => rejectP(err))
  })
}

/**
 * 启动（或复用已启动的）册府无头进程并返回连接。
 *
 * 每次启动前先接管（kill 同一 exe 的旧实例）——服务重启后旧册府进程还占着
 * SQLite 连接与 ctl 文件，不接管会出现两个进程写同一个库。
 */
export async function ensureCefu(opts: CefuControllerOptions = {}): Promise<CefuRuntime> {
  if (runtime && !runtime.proc.killed) return runtime
  if (starting) return starting
  starting = (async () => {
    const env = opts.env ?? process.env
    const gebaiHome = opts.gebaiHome ?? env.GEBAI_HOME ?? ""
    if (!gebaiHome) throw new Error("册府启动需要 GEBAI_HOME（未设置时无法定位数据根与 exe）")
    const probe = resolveCefuBin({
      gebaiHome,
      moduleDir: import.meta.dirname,
      override: env.CEFU_BIN,
    })
    if (probe.bin === null) {
      throw new Error(
        `未找到册府 exe（试过：${probe.candidates.join("、")}）。设 CEFU_BIN 指定路径，或把 cefu 放进 GEBAI_HOME/PATH。`,
      )
    }
    const uiDir = resolveCefuUiDir(probe.repoRoot)
    const home = cefuHomeFor(gebaiHome, env)
    mkdirSync(home, { recursive: true })
    const dbPath = join(home, "cefu.db")
    const ctlFile = join(home, "ctl.json")
    // 旧 ctl 必须清掉：control-port 为 0 时端口由 OS 分配，若上次进程已退出而 ctl 残留，
    // 新进程写入前会读到旧端口（ECONNREFUSED）；清理后读取天然等待新进程写入。
    try {
      rmSync(ctlFile, { force: true })
    } catch {
      /* 删不掉也不阻断（连接失败重试仍可救） */
    }
    // 接管旧实例（失败不阻断：顶多新旧并存，新进程自己报资源冲突）
    const take = await takeoverProcess(probe.bin, { log: opts.log, env })
    for (const w of take.warnings) opts.log?.warn(`[cefu] 接管警告：${w}`)
    const spawnFn = opts.spawnFn ?? ((bin: string, args: string[]) => spawn(bin, args, { stdio: "ignore", windowsHide: true, cwd: uiDir ?? dirname(bin) }))
    const args = ["--headless", "--db", dbPath, "--control-port", "0", "--control-file", ctlFile]
    if (uiDir) args.push("--ui", uiDir)
    opts.log?.info(`[cefu] 拉起无头册府：${probe.bin} ${args.join(" ")}`)
    const proc = spawnFn(probe.bin, args)
    proc.on("exit", () => {
      // 进程死了：清掉 runtime，下次调用自动重拉
      if (runtime?.proc === proc) runtime = null
    })
    // 等 control-file 就绪（册府启动写 port+token，通常 <1s）
    const deadline = Date.now() + 30_000
    let ctl: { port: number; token: string } | null = null
    while (Date.now() < deadline) {
      if (proc.exitCode !== null) throw new Error(`册府进程启动即退出（exit ${proc.exitCode}）——多半是 UI 资源/参数问题`)
      ctl = readCtlFile(ctlFile)
      if (ctl) break
      await new Promise((r) => setTimeout(r, 200))
    }
    if (!ctl) throw new Error(`册府控制文件未就绪（${ctlFile}，等了 30s）——进程可能启动失败`)
    const connect = opts.connect ?? defaultConnect
    const { socket } = await connect(ctl.port)
    const conn = await CefuConnection.handshake(socket, makeFrameReader(socket), ctl.token)
    runtime = { proc, conn, ctlFile, bin: probe.bin }
    opts.log?.info(`[cefu] 册府就绪：pid=${proc.pid} 端口 ${ctl.port}（db ${dbPath}）`)
    return runtime
  })()
  try {
    const rt = await starting
    starting = null
    return rt
  } catch (err) {
    starting = null
    throw err
  }
}

/** 调一个册府桥函数（`app.cefu.<fn>`），返回 eval 结果。 */
export async function cefuCall(fn: string, argsJson: string, opts: CefuControllerOptions = {}): Promise<unknown> {
  const rt = await ensureCefu(opts)
  const expr = `app.cefu.${fn}(${argsJson})`
  try {
    return await rt.conn.eval(expr)
  } catch (err) {
    // 进程可能死了：清状态重试一次（下次 ensureCefu 重拉）
    if (rt.proc.exitCode !== null) {
      runtime = null
      const rt2 = await ensureCefu(opts)
      return rt2.conn.eval(expr)
    }
    throw err
  }
}

/** 停止册府进程（服务关闭/测试收尾）。 */
export async function stopCefu(): Promise<void> {
  const rt = runtime
  runtime = null
  if (!rt) return
  try {
    await rt.conn.shutdown()
  } catch {
    /* 优雅退出失败走硬杀 */
  }
  if (rt.proc.exitCode === null) {
    try {
      rt.proc.kill()
    } catch {
      /* 已退出 */
    }
  }
}
