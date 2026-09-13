/**
 * 文件工作台 · 终端 PTY 会话（ConPTY）：会话表 + 输出缓冲 + 生命周期（创建/输入/尺寸/关闭/回收）。
 *
 * 与 core/exec/term-session.ts（管道式持久 shell）的分工：那边没有 TTY，命令边界靠哨兵行推断、
 * 回显与提示符由前端自绘，交互式程序（vim/top/ssh 密码提示）不可用；本模块给终端一条**真正的
 * 伪控制台**——shell 输出原样经过伪控制台（含 ANSI 全序列、真彩色、光标定位），键盘输入与
 * 窗口尺寸（resize）直接透传，命令回显、Tab 补全、行编辑都由 shell 自己完成。
 *
 * 驱动：Windows 上由 pty-driver.ts 编译出的独立 exe（ConPTY 需要原生 API，见该文件说明）。
 * 非 Windows 或驱动不可用时 `available()` 返回原因，调用方降级到管道式终端。
 *
 * 输出模型：驱动把伪控制台字节流按块（base64）投递，这里按 UTF-8（含 GBK 兜底）增量解码成文本、
 * 追加到滚动缓冲（重连回放用），并即时推给订阅者（WS 连接）。会话与连接解耦：连接断开只是退订，
 * 会话继续跑（重连 attach 回放缓冲），空闲超时由 sweep 回收。
 */
import { spawn } from "node:child_process"
import { randomUUID } from "node:crypto"
import { basename } from "node:path"
import { FsError, fsBadRequest, logicalPath } from "../fs/roots"
import { StreamTextDecoder, type ShellSpec } from "./term-session"
import { preparePtyDriver, type PtyDriverLaunch } from "./pty-driver"

/** 并发会话上限（与管道式终端同口径：会话是常驻进程）。 */
export const PTY_MAX_SESSIONS = 8
/** 空闲回收阈值（30 分钟无交互）。 */
export const PTY_IDLE_MS = 30 * 60 * 1000
/** 单会话回放缓冲上限（字符）：超出只保留尾部。 */
export const PTY_MAX_BUFFER = 200_000

/** 驱动事件（pty-driver 协议的一行 JSON）。 */
export interface PtyDriverEvent {
  t: "ready" | "out" | "exit" | "error"
  pid?: number
  /** out 事件：base64 原始字节 */
  d?: string
  /** exit 事件：子进程退出码 */
  code?: number
  /** error 事件：驱动侧错误描述 */
  m?: string
}

/** 驱动进程句柄（默认 spawner 返回；测试注入假实现）。 */
export interface PtyChild {
  pid: number | null
  write(data: string): void
  kill(): void
}

/** 驱动生成器（依赖注入：默认 spawn exe；测试接假实现）。 */
export type PtySpawner = (opts: {
  cmd: string[]
  onEvent: (evt: PtyDriverEvent) => void
  onExit: (code: number | null) => void
}) => PtyChild

/** 推送给前端的事件。 */
export type PtyEvent =
  | { type: "ready"; pid: number | null }
  | { type: "out"; data: string }
  | { type: "exit"; code: number | null }
  | { type: "error"; message: string }

export type PtySink = (evt: PtyEvent) => void

/** 会话对外视图。 */
export interface PtySessionInfo {
  id: string
  shell: string
  shellName: string
  cwd: string
  root: string
  cols: number
  rows: number
  alive: boolean
  startedAt: number
}

export interface PtyServiceOptions {
  spawner?: PtySpawner
  /** 驱动启动方式（缺省按平台准备；注入便于测试）。 */
  launch?: () => PtyDriverLaunch
  maxSessions?: number
  idleMs?: number
  maxBufferChars?: number
  now?: () => number
  killTree?: (pid: number | null) => void
}

/** 会话内部状态。 */
interface PtySession {
  id: string
  rootId: string
  rootAbs: string
  cwdAbs: string
  cwdRel: string
  shell: ShellSpec
  cols: number
  rows: number
  child: PtyChild
  /**
   * 驱动世代：每次重建（interrupt）自增；回调携带建时世代，与当前不符的回调丢弃。
   * 不这么做的话，被杀掉的旧驱动在退出时会走 onExit——把刚重建的新会话标成「已结束」。
   */
  gen: number
  ready: boolean
  alive: boolean
  exitCode: number | null
  /** 已解码输出（保留尾部 maxBufferChars 字符供重连回放）。 */
  buffer: string
  base: number
  decoder: StreamTextDecoder
  startedAt: number
  lastActive: number
  /** 订阅者：键是连接级标识（WS sink 对象），值为推送回调——连接断开时按键精确退订。 */
  subscribers: Map<object, PtySink>
}

/** 进程树终止（与 term-session 同口径：Windows 走 taskkill /T，POSIX 按进程组）。 */
const defaultKillTree = (pid: number | null): void => {
  if (pid == null) return
  try {
    if (process.platform === "win32") {
      spawn("taskkill", ["/pid", String(pid), "/T", "/F"], { stdio: "ignore" }).on("error", () => {})
    } else {
      process.kill(-pid, "SIGKILL")
    }
  } catch {
    /* 进程已退出 */
  }
}

/** 默认驱动生成器：spawn 驱动 exe，按行解析 JSON 事件。 */
const defaultSpawner: PtySpawner = (opts) => {
  const child = spawn(opts.cmd[0]!, opts.cmd.slice(1), { stdio: ["pipe", "pipe", "pipe"], windowsHide: true })
  let pending = ""
  child.stdout?.on("data", (d: Buffer) => {
    pending += d.toString("utf8")
    let i = pending.indexOf("\n")
    while (i >= 0) {
      const line = pending.slice(0, i).trim()
      pending = pending.slice(i + 1)
      if (line) {
        try {
          opts.onEvent(JSON.parse(line) as PtyDriverEvent)
        } catch {
          /* 非协议行（驱动异常输出）：忽略，不影响会话 */
        }
      }
      i = pending.indexOf("\n")
    }
  })
  // stderr 必须有人听（否则流上的 error 事件会抛到进程级）；内容对上层无意义
  child.stderr?.on("data", () => {})
  child.on("error", () => opts.onExit(null))
  child.on("close", (code) => opts.onExit(code))
  child.stdin?.on("error", () => {})
  return {
    pid: child.pid ?? null,
    write: (data: string) => {
      try {
        child.stdin?.write(data)
      } catch {
        /* 驱动已退出 */
      }
    },
    kill: () => defaultKillTree(child.pid ?? null),
  }
}

/** shell 启动命令行（引号包裹路径；PowerShell 关版本横幅）。 */
export function shellCommandLine(shell: ShellSpec): string {
  const quoted = `"${shell.path.replace(/"/g, '\\"')}"`
  if (shell.id === "powershell" || shell.id === "pwsh") return `${quoted} -NoLogo`
  return quoted
}

/** 终端会话服务：会话表 + 回放缓冲 + 订阅推送 + 生命周期。 */
export class PtySessionService {
  private spawner: PtySpawner
  private launchFn: () => PtyDriverLaunch
  private maxSessions: number
  private idleMs: number
  private maxBufferChars: number
  private now: () => number
  private killTree: (pid: number | null) => void
  private sessions = new Map<string, PtySession>()

  constructor(opts: PtyServiceOptions = {}) {
    this.spawner = opts.spawner ?? defaultSpawner
    this.launchFn = opts.launch ?? preparePtyDriver
    this.maxSessions = opts.maxSessions ?? PTY_MAX_SESSIONS
    this.idleMs = opts.idleMs ?? PTY_IDLE_MS
    this.maxBufferChars = opts.maxBufferChars ?? PTY_MAX_BUFFER
    this.now = opts.now ?? Date.now
    this.killTree = opts.killTree ?? defaultKillTree
  }

  /** 能力探测：驱动可用性（不可用时调用方降级到管道式终端）。 */
  available(): { ok: boolean; reason?: string } {
    const launch = this.launchFn()
    return launch.ok ? { ok: true } : { ok: false, reason: launch.reason }
  }

  /** 预热（组合根启动后调用）：首次编译驱动 exe，避免用户首点终端时等待。 */
  async warmup(): Promise<void> {
    try {
      this.launchFn()
    } catch {
      /* 预热失败不影响运行期降级判定 */
    }
  }

  /** 创建会话：spawn 驱动并下发 open（shell/cwd/尺寸）；并发超限或驱动不可用抛错。 */
  create(opts: { rootId: string; rootAbs: string; cwdAbs: string; shell: ShellSpec; cols: number; rows: number }): PtySessionInfo {
    this.sweep()
    const launch = this.launchFn()
    if (!launch.ok || !launch.cmd) throw new FsError(503, launch.reason ?? "PTY 驱动不可用")
    if (this.sessions.size >= this.maxSessions) {
      throw fsBadRequest(`终端会话数已达上限（${this.maxSessions}）：请先关闭不用的会话再新建。`)
    }
    const id = `p${randomUUID().replace(/-/g, "").slice(0, 8)}`
    const cols = Math.max(2, Math.min(1000, Math.floor(opts.cols) || 120))
    const rows = Math.max(2, Math.min(1000, Math.floor(opts.rows) || 30))
    const s: PtySession = {
      id,
      rootId: opts.rootId,
      rootAbs: opts.rootAbs,
      cwdAbs: opts.cwdAbs,
      cwdRel: logicalPath(opts.rootAbs, opts.cwdAbs),
      shell: opts.shell,
      cols,
      rows,
      child: { pid: null, write: () => {}, kill: () => {} },
      gen: 0,
      ready: false,
      alive: false,
      exitCode: null,
      buffer: "",
      base: 0,
      decoder: new StreamTextDecoder(),
      startedAt: this.now(),
      lastActive: this.now(),
      subscribers: new Map(),
    }
    this.sessions.set(id, s)
    try {
      this.spawnDriver(s, launch.cmd)
    } catch (err) {
      this.sessions.delete(id)
      throw new FsError(500, `终端驱动启动失败：${(err as Error).message}`)
    }
    // open 行必须首行下发：驱动以它作为启动参数（shell 命令行 / cwd / 初始尺寸）
    s.child.write(
      `${JSON.stringify({ t: "open", shell: shellCommandLine(opts.shell), cwd: opts.cwdAbs, cols, rows })}\n`,
    )
    return this.view(s)
  }

  /** 起一份驱动进程并把回调绑定到当前世代（旧世代回调会被忽略，见 PtySession.gen）。 */
  private spawnDriver(s: PtySession, cmd: string[]): void {
    const gen = ++s.gen
    s.child = this.spawner({
      cmd,
      onEvent: (evt) => {
        if (s.gen === gen) this.onDriverEvent(s, evt)
      },
      onExit: (code) => {
        if (s.gen === gen) this.onDriverExit(s, code)
      },
    })
  }

  /** 写入终端（键盘输入 / 粘贴内容，原样透传给 shell）。 */
  write(id: string, data: string): void {
    const s = this.require(id)
    if (!s.alive) throw new FsError(409, "终端会话已结束：请关闭该标签后重新创建。")
    if (!data) return
    s.child.write(`${JSON.stringify({ t: "in", d: Buffer.from(data, "utf8").toString("base64") })}\n`)
    s.lastActive = this.now()
  }

  /**
   * 中断当前命令：终止子进程树并以原 cwd / 尺寸重建 shell（保留会话 id、回放缓冲与订阅）。
   *
   * 为什么不只是往 pty 写 `\x03`：Windows ConPTY 下向输入管道写 ETX 字节不会被 conhost
   * 翻成 CTRL_C_EVENT（实测：`ping -t` / `timeout /t` 均不响应），前台长命令无法停下。
   * 因此中断与管道式终端同口径——杀进程树 + 原 cwd 重建，代价是 shell 内部状态（set/变量）
   * 丢失、cwd 回到会话创建时的目录；换来的是「Ctrl+C 确实能停下当前命令」。
   */
  interrupt(id: string): { ok: true; cwd: string } {
    const s = this.require(id)
    if (!s.alive) throw new FsError(409, "终端会话已结束：请关闭该标签后重新创建。")
    this.killTree(s.child.pid)
    s.alive = false
    s.ready = false
    // 半截输出（未换行）与新进程字节流无关：先闭合该行，提示语不与之粘连
    if (s.buffer && !s.buffer.endsWith("\n")) this.appendOut(s, "\n")
    const note = "^C（已中断当前命令，Shell 已按原目录重建）\n"
    this.appendOut(s, note)
    this.emit(s, { type: "out", data: note })
    s.decoder.reset()
    const launch = this.launchFn()
    if (!launch.ok || !launch.cmd) {
      this.finish(s, null)
      return { ok: true, cwd: s.cwdRel }
    }
    try {
      this.spawnDriver(s, launch.cmd)
      s.child.write(
        `${JSON.stringify({ t: "open", shell: shellCommandLine(s.shell), cwd: s.cwdAbs, cols: s.cols, rows: s.rows })}\n`,
      )
    } catch (err) {
      this.appendOut(s, `[重建 Shell 失败：${(err as Error).message}]\n`)
      this.finish(s, null)
    }
    s.lastActive = this.now()
    return { ok: true, cwd: s.cwdRel }
  }

  /** 调整伪控制台尺寸（shell 据此重排输出；TUI 程序靠它感知窗口大小）。 */
  resize(id: string, cols: number, rows: number): void {
    const s = this.require(id)
    const c = Math.max(2, Math.min(1000, Math.floor(cols) || s.cols))
    const r = Math.max(2, Math.min(1000, Math.floor(rows) || s.rows))
    if (c === s.cols && r === s.rows) return
    s.cols = c
    s.rows = r
    s.child.write(`${JSON.stringify({ t: "resize", cols: c, rows: r })}\n`)
    s.lastActive = this.now()
  }

  /** 订阅会话事件：立即回放已有输出（重连/切换标签），之后增量推送；key 用于连接断开时退订。 */
  subscribe(id: string, sink: PtySink, key: object = sink): () => void {
    const s = this.require(id)
    if (s.buffer) sink({ type: "out", data: s.buffer })
    if (s.alive && s.ready) sink({ type: "ready", pid: s.child.pid })
    if (!s.alive) sink({ type: "exit", code: s.exitCode })
    s.subscribers.set(key, sink)
    return () => {
      s.subscribers.delete(key)
    }
  }

  /** 连接断开：按连接级 key 从所有会话退订（会话本身保留，等空闲回收）。 */
  detach(key: object): void {
    for (const s of this.sessions.values()) s.subscribers.delete(key)
  }

  /** 关闭会话（幂等）。 */
  close(id: string): void {
    const s = this.sessions.get(id)
    if (!s) return
    this.sessions.delete(id)
    s.alive = false
    s.gen++ // 作废在途回调（驱动被杀的退出回调不该再动这个已移除的会话）
    try {
      s.child.write(`${JSON.stringify({ t: "close" })}\n`)
    } catch {
      /* 驱动已退出 */
    }
    this.killTree(s.child.pid)
  }

  /** 会话元信息（list 用）。 */
  list(): PtySessionInfo[] {
    this.sweep()
    return [...this.sessions.values()].map((s) => this.view(s))
  }

  /** 会话是否存在（attach 校验）。 */
  has(id: string): boolean {
    return this.sessions.has(id)
  }

  /** 空闲回收（每次创建/列举时顺带执行）：无交互超阈值即关闭（含尚未 ready 的会话）。 */
  sweep(): void {
    const t = this.now()
    for (const [id, s] of this.sessions) {
      if (t - s.lastActive > this.idleMs) this.close(id)
    }
  }

  /* --------------------------- 内部 --------------------------- */

  private require(id: string): PtySession {
    const s = this.sessions.get(id)
    if (!s) throw new FsError(404, "终端会话不存在或已关闭")
    return s
  }

  private view(s: PtySession): PtySessionInfo {
    return {
      id: s.id,
      shell: s.shell.id,
      shellName: s.shell.name,
      cwd: s.cwdRel,
      root: s.rootId,
      cols: s.cols,
      rows: s.rows,
      alive: s.alive,
      startedAt: s.startedAt,
    }
  }

  private emit(s: PtySession, evt: PtyEvent): void {
    for (const sink of s.subscribers.values()) {
      try {
        sink(evt)
      } catch {
        /* 单个订阅者异常不影响会话 */
      }
    }
  }

  /** 驱动事件：ready 置活、out 解码入缓冲并推送、error 记入缓冲、exit 收尾。 */
  private onDriverEvent(s: PtySession, evt: PtyDriverEvent): void {
    if (evt.t === "ready") {
      s.ready = true
      s.alive = true
      s.lastActive = this.now()
      this.emit(s, { type: "ready", pid: evt.pid ?? s.child.pid })
      return
    }
    if (evt.t === "out") {
      const text = s.decoder.push(Buffer.from(evt.d ?? "", "base64"))
      if (!text) return
      this.appendOut(s, text)
      this.emit(s, { type: "out", data: text })
      return
    }
    if (evt.t === "error") {
      const msg = `[终端驱动错误：${evt.m ?? "未知"}]`
      this.appendOut(s, `${msg}\n`)
      this.emit(s, { type: "error", message: evt.m ?? "未知" })
      return
    }
    if (evt.t === "exit") {
      this.finish(s, typeof evt.code === "number" ? evt.code : null)
    }
  }

  /** 驱动进程退出（未走 exit 事件时的兜底，例如驱动被杀）。 */
  private onDriverExit(s: PtySession, code: number | null): void {
    if (!s.alive && s.exitCode !== null) return
    this.finish(s, code)
  }

  private finish(s: PtySession, code: number | null): void {
    if (!this.sessions.has(s.id)) return
    s.alive = false
    s.ready = false
    s.exitCode = code
    const note = code === null || code === 0 ? "[终端进程已结束]" : `[终端进程已结束：退出码 ${code}]`
    this.appendOut(s, `\n${note}\n`)
    this.emit(s, { type: "exit", code })
    this.emit(s, { type: "out", data: `\n${note}\n` })
  }

  /** 追加输出并裁剪缓冲（超上限丢最旧）。 */
  private appendOut(s: PtySession, text: string): void {
    s.buffer += text
    const over = s.buffer.length - this.maxBufferChars
    if (over > 0) {
      s.buffer = s.buffer.slice(over)
      s.base += over
    }
  }
}

/** shell 展示名（与管道式终端同口径：basename 去扩展名兜底）。 */
export function shellDisplayName(shell: ShellSpec): string {
  if (shell.name) return shell.name
  return basename(shell.path).replace(/\.[^.]+$/, "")
}
