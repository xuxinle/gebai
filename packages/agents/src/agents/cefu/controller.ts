/**
 * 册府控制器：TS 直接控制霜天册府应用（`shuangtian/apps/cefu`）。
 *
 * ## 形态：宿主拉起**带窗口的**册府 + 控制通道调桥函数
 *
 * ```
 * 册府（有窗口的 C++ 进程）◀──TCP 长度前缀 JSON-RPC──▶ 本控制器（跑在 gebai 宿主里）
 * --control-port 0 --control-file ctl.json        script {code:"app.cefu.search(...)"}
 * ```
 *
 * ## 为何缺省带窗口（不是无头）
 *
 * 同一份霜天应用两个形态（有头给人看、无头给机器用），桥函数与 SQLite 完全同源——
 * 那是从客卿边车（旧 keqing/cpp/cefu，已移除）换过来时就定下的。但**给歌白用工
 * 不等于得无头**：无头形态下用户看不到任何东西（一个知识库在后台默默写库），
 * 而且**三维图谱页根本不存在**（`main.cpp` 的 `mount_graph_view` 在
 * `app.headless()` 时直接拒绝挂载）。窗口形态下控制通道照样能用，没有失去任何机器
 * 能力，却让“她记了什么”变得可看、可翻、可改。
 *
 * 无头只给真的没有桌面的场合（CI 容器 / 纯 SSH 服务器）：`CEFU_HEADLESS=1`。
 *
 * ## 与用户手动打开的册府共存
 *
 * 启动前先试**复用**（`tryReuseCefu`）：ctl.json 还能握手就接上去，不杀进程——
 * 旧时代“先 kill 再拉”在窗口形态下会点掉用户正在用的窗口。
 *
 * ## 为什么控制器是进程级单例（而不是会话级）
 *
 * 册府进程持有 SQLite 连接与索引内存态，多实例 = 多个写者竞争同一个库文件。
 * 工具调用时懒启动（第一次用到才 spawn），进程死了下次调用自动重拉。
 *
 * ## 控制协议（与霜天 e2e `tools/cefu_e2e.py` 同一份）
 *
 * - 帧格式：4 字节大端长度 + UTF-8 JSON `{id, method, params}`
 * - 首个请求必须是 `hello`（params 带 control-file 里的 token，并带 `agent` 标识句）
 * - 桥函数调用走 `script` 方法的 `code` 分支：eval 表达式 `app.cefu.<fn>(args...)`
 *   （桥函数挂 JS 全局 `app` 对象下，`script {function}` 只能调全局函数名，
 *   含点号的名字解析不到——实测用 code 走 eval 是唯一通路）
 */

import { existsSync, mkdirSync, readFileSync, rmSync } from "node:fs"
import { spawn, type ChildProcess } from "node:child_process"
import { dirname, isAbsolute, join, resolve } from "node:path"
import { resolveDataPath } from "@gebai/sdk/node"
import { takeoverProcess } from "../../core/process-takeover"
import { AGENT_HANDSHAKE } from "../shuangtian/shuangtian_client"

/** 册府数据目录名（`{GEBAI_HOME}/data/cefu`）。 */
export const CEFU_HOME_DIRNAME = "cefu"

/** 可执行文件名：Windows 带 `.exe`（spawn 不补后缀）。 */
export const CEFU_EXE_NAME = process.platform === "win32" ? "cefu.exe" : "cefu"

/** 构建档探测顺序（与相期同表：dev 主用，quick/release 发行，debug 兜底）。 */
export const CEFU_PROFILES = ["dev", "quick", "release", "debug"] as const

/** 仓库标记：`shuangtian/apps/cefu/ui` 在源码树存在、发行产物不存在。 */
const REPO_MARKER = ["shuangtian", "apps", "cefu", "ui"] as const

const MAX_UP = 16

// ── 启动姿态（纯函数，测试可钉） ────────────────────────────────────────────

/**
 * 册府启动姿态：`window`（带窗口，**缺省**）/ `headless`（无窗口）。
 *
 * ## 为何缺省改成 window（2026-11-11，用户报「拟人自动启动的册府不要用无头模式」）
 *
 * 人格（拟人模式）一旦声明 `agents: ["cefu"]`，面板的 `memoryStats` 与 `cefu_*`
 * 工具首次调用就会拉起册府。旧实现**写死 `--headless`**，于是：
 *
 * - 用户在桌面上看不到任何窗口——一个知识库在后台默默写库，没有任何可见性；
 * - 无头形态下**三维图谱页不存在**（`main.cpp` 的 `mount_graph_view` 在
 *   `app.headless()` 时直接拒绝挂载），界面上有一整块能力是空的；
 * - 用户想「顺手看看她记了什么」时，只能另开一个册府，两实例抢同一个 SQLite。
 *
 * 而窗口形态下这一切自动成立：窗口、托盘常驻、图谱都在，控制通道照样能调桥函数
 * （工具面走的是 TCP，与有无窗口无关）。所以窗口才是**默认**，无头只留给
 * 真的没有桌面的场合。
 *
 * ## 为何还保留无头
 *
 * 无桌面环境（CI 容器、纯 SSH 的 Linux 服务器）上开窗口会失败。框架自带
 * 「建窗失败 → 回退 headless」的降级（`app.cpp` 的 `create_window` 分支），但那
 * 要真去试一次建窗（付一遍后端初始化），而 `CEFU_HEADLESS=1` 能直接跳过。
 */
export type CefuMode = "window" | "headless"

/** 解析启动姿态：`CEFU_HEADLESS=1` / `true` / `yes` → 无头，其余（含未设）→ 窗口。 */
export function resolveCefuMode(env: Record<string, string | undefined>): CefuMode {
  const raw = (env.CEFU_HEADLESS ?? "").trim().toLowerCase()
  return raw === "1" || raw === "true" || raw === "yes" || raw === "on" ? "headless" : "window"
}

/** 启动参数的输入（全部显式传入，便于单测）。 */
export interface CefuArgsInput {
  mode: CefuMode
  dbPath: string
  /** 一个**确定不存在**的种子路径（关掉示例数据导入，见 `ensureCefu` 的注释）。 */
  noSeedPath: string
  ctlFile: string
  /** UI 资源目录（`main.xml` 所在）。空/未给时靠 exe 内嵌资源。 */
  uiDir?: string | null
}

/**
 * 拼册府命令行。
 *
 * 抽成纯函数是为了能钉住两件事：
 * 1. **窗口模式不得出现 `--headless`**（这正是用户报的缺陷——写死在字符串里，
 *    读代码时很容易以为“它本来就这样”而没意识到是个默认值）；
 * 2. `--seed` / `--control-file` / `--ui` 一个都不能漏（漏 seed 会把示例数据
 *    导进生产库；漏 control-file 会让自动化永远连不上）。
 */
export function buildCefuArgs(input: CefuArgsInput): string[] {
  const args = input.mode === "headless" ? ["--headless"] : []
  args.push("--db", input.dbPath, "--seed", input.noSeedPath, "--control-port", "0", "--control-file", input.ctlFile)
  if (input.uiDir) args.push("--ui", input.uiDir)
  return args
}

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
  const dir = join(repoRoot, "shuangtian", "apps", "cefu", "ui")
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
    await conn.call("hello", { token, agent: AGENT_HANDSHAKE }, 10_000)
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

  /** 只断开控制连接（**不停进程**）。复用别人的实例时用：见 `stopCefu`。 */
  close(): void {
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
  /** 自己 spawn 的进程；**复用已有实例时为 null**（那个进程不归我们杀）。 */
  proc: ChildProcess | null
  conn: CefuConnection
  ctlFile: string
  bin: string
  /** 是否本控制器启动（决定退出时能不能 kill）：复用别人的实例时 false。 */
  owned: boolean
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
  // 复用来的实例没有 proc（不归我们管）：进程还活着就是 running，但报不出 pid
  if (runtime.proc === null) return { running: true, bin: runtime.bin }
  return { running: !runtime.proc.killed, pid: runtime.proc.pid, bin: runtime.bin }
}

/**
 * 尝试复用**已在跑的**实例（需先读到 ctl.json 且握手通过）。
 *
 * ## 为何要先试复用（窗口形态才有的问题）
 *
 * 无头时代“杀掉旧的再拉新的”无代价：旧的没窗口，没人看得见也没人碰。
 * 但窗口形态下那是用户桌面上一个**正在用的窗口**——里面有他手动打开的词条、
 * 正在编辑的内容。服务重启（或面板刷新触发首次调用）就把它 kill 掉，
 * 用户看到的是“我的册府窗口被莫名点掉了”。所以先用后杀：
 * ctl.json 里的 port/token 还能握手成功，就说明它活着且是同一份数据（同一个
 * `{GEBAI_HOME}/data/cefu`），直接接上去（仅断开连接，不停进程）。
 *
 * 复用失败的任何原因（文件没了/端口死了/token 不匹配/握手超时）都返回 null，
 * 由调用方走原有的“接管 + 新拉”，行为与改前一致——所以这是纯增益的兼容层。
 */
async function tryReuseCefu(
  ctlFile: string,
  bin: string,
  opts: CefuControllerOptions,
): Promise<CefuRuntime | null> {
  const ctl = readCtlFile(ctlFile)
  if (!ctl) return null
  try {
    const connect = opts.connect ?? defaultConnect
    const { socket } = await connect(ctl.port)
    const conn = await CefuConnection.handshake(socket, makeFrameReader(socket), ctl.token)
    opts.log?.info(`[cefu] 复用已在运行的册府：端口 ${ctl.port}（不重启，它可能是用户正开着的窗口）`)
    return { proc: null, conn, ctlFile, bin, owned: false }
  } catch (err) {
    opts.log?.info(`[cefu] 旧实例不可复用（${String((err as Error).message || err)}），改走接管重拉`)
    return null
  }
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
 * 没拿到 GEBAI_HOME 时的数据根兑底（**从仓库标记上溯找仓库根**）。
 *
 * ## 为何需要它（真实故障）
 *
 * 原本 `GEBAI_HOME` 为空就直接抛“册府启动需要 GEBAI_HOME”。这在**服务进程里**
 * 不会发生（compose 注入了），但工具边车/子会话/直接调工具的场景经常没有——
 * 结果就是“工具在但在任何场景下都报错”，而报错文案看起来像配置问题，
 * 实际是代码没兑底。
 *
 * 与相期 `resolveXiangqiBin` 同一手法：不上数级数，而是逐级上溯找
 * `shuangtian/apps/cefu/ui`（源码树里一定在）——数级数写错过一次。
 * 找不到才回落到 cwd（此处调用方会拼 data/cefu，至少在能写的工作目录）。
 */
function defaultGebaiHome(env: Record<string, string | undefined>): string {
  void env
  let cur = resolve(import.meta.dirname)
  for (let i = 0; i < 16; i++) {
    if (existsSync(join(cur, ...REPO_MARKER))) return cur
    const parent = dirname(cur)
    if (parent === cur) break
    cur = parent
  }
  return process.cwd()
}

/**
 * 启动（或复用）册府进程并返回连接。
 *
 * 三道门，顺序不能换：
 * 1. 已有 runtime 且进程活着（或复用来的、我们不持有 proc）⇒ 直接返回；
 * 2. 先试**复用**已在跑的实例（窗口形态下它可能是用户正开着的窗口，不能杀）；
 * 3. 实在不行才接管（kill 同一 exe 的旧实例）+ 新拉——服务重启后旧进程还占着
 *    SQLite 连接与 ctl 文件，不接管会出现两个进程写同一个库。
 */
export async function ensureCefu(opts: CefuControllerOptions = {}): Promise<CefuRuntime> {
  // 复用来的实例 proc 为 null（不归我们管）：只要连接还在就算活着
  if (runtime && (runtime.proc === null || !runtime.proc.killed)) return runtime
  if (starting) return starting
  starting = (async () => {
    const env = opts.env ?? process.env
    const gebaiHome = opts.gebaiHome ?? env.GEBAI_HOME ?? defaultGebaiHome(env)
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
    // ① 先试复用：**窗口形态下这一步是必需的**（见 `tryReuseCefu` 的说明）——
    //    旧实例可能是用户正开着的册府窗口，为了“自己起一个”而把它 kill 掉是错的。
    const reused = await tryReuseCefu(ctlFile, probe.bin, opts)
    if (reused !== null) {
      runtime = reused
      return reused
    }
    // 旧 ctl 必须清掉：control-port 为 0 时端口由 OS 分配，若上次进程已退出而 ctl 残留，
    // 新进程写入前会读到旧端口（ECONNREFUSED）；清理后读取天然等待新进程写入。
    try {
      rmSync(ctlFile, { force: true })
    } catch {
      /* 删不掉也不阻断（连接失败重试仍可救） */
    }
    // ② 接管旧实例（失败不阻断：顶多新旧并存，新进程自己报资源冲突）
    const take = await takeoverProcess(probe.bin, { log: opts.log, env })
    for (const w of take.warnings) opts.log?.warn(`[cefu] 接管警告：${w}`)
    const mode = resolveCefuMode(env)
    const spawnFn =
      opts.spawnFn ??
      ((bin: string, args: string[]) =>
        spawn(bin, args, {
          stdio: "ignore",
          // ⚠ `windowsHide` 固定为 false：它是 STARTUPINFO 的 `SW_HIDE`，
          // 会把子进程的第一个窗口**以隐藏态创建**——窗口形态下就成了「进程起了、
          // 界面上什么都没有」的静默故障。无头形态没有窗口，开着也无害。
          windowsHide: false,
          cwd: uiDir ?? dirname(bin),
        }))
    // `--seed <不存在的路径>`：**生产库不导入示例种子**。
    //
    // 册府缺省会在空库首次启动时导入 `seed.md`（7 条示例值班知识）——那是为界面
    // 演示准备的（“打开就有内容而不是空白页”）。但它们全部 `needs_review=1` 且
    // `confidence=inferred`，进了生产库就是**检索污染源**：人格会命中
    // “TSDB 写入延迟升高”这种与当前部署无关的假条目，还可能当真引给用户。
    //
    // 为何不能传空串：册府的 `resolve_seed("")` 会把空串当成“用缺省”——
    // `is_regular_file("")` 为 false，于是去试候选列表并命中 `apps/cefu/seed.md`，
    // 又导了回来（实测踩到）。传一个**确定不存在**的路径才能真的关掉。
    const noSeed = join(home, "no-seed.md")   // 不创建它；册府读不到就跳过导入
    const args = buildCefuArgs({ mode, dbPath, noSeedPath: noSeed, ctlFile, uiDir })
    opts.log?.info(
      `[cefu] 拉起册府（${mode === "window" ? "带窗口" : "无头"}）：${probe.bin} ${args.join(" ")}`,
    )
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
    runtime = { proc, conn, ctlFile, bin: probe.bin, owned: true }
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
    // 进程可能死了：清状态重试一次（下次 ensureCefu 重拉）。
    // 复用来的实例没有 proc（`proc === null`）：不能判“活着吗”，就当连接坏了
    // 一样清掉重试——重试时 `tryReuseCefu` 会重新判断（握手失败就接管重拉）。
    const dead = rt.proc === null || rt.proc.exitCode !== null
    if (dead) {
      runtime = null
      const rt2 = await ensureCefu(opts)
      return rt2.conn.eval(expr)
    }
    throw err
  }
}

/**
 * 停止册府（服务关闭/测试收尾）。
 *
 * ## 只停自己起来的那个
 *
 * 窗口形态下这个区别很重要：复用到的实例是**用户桌面上的窗口**，服务关闭时把它
 * 关掉，等于“重启一下歌白，用户正开着的册府就没了”。所以复用来的（`owned=false`）
 * 只断开控制连接、不动进程；只有自己 spawn 的才发 shutdown / kill。
 */
export async function stopCefu(): Promise<void> {
  const rt = runtime
  runtime = null
  if (!rt) return
  if (!rt.owned) {
    // 不是我们的进程：只断连接（socket.end），不发 shutdown——那会让那个窗口消失
    try {
      rt.conn.close()
    } catch {
      /* 已断开 */
    }
    return
  }
  try {
    await rt.conn.shutdown()
  } catch {
    /* 优雅退出失败走硬杀 */
  }
  if (rt.proc !== null && rt.proc.exitCode === null) {
    try {
      rt.proc.kill()
    } catch {
      /* 已退出 */
    }
  }
}
