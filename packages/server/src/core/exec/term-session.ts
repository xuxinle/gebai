/** 文件工作台 · 终端会话（DESIGN「文件工作台·终端」）：常驻 shell 进程 + 管道，不引入 PTY 与原生依赖。
 *
 * 与 core/security/sandbox.ts 的一次性 exec 的分工：exec 是「跑完即返回」（无会话、无增量输出、无中断）；
 * 本模块维护一个 stdin 保持打开的常驻 shell，把 stdout/stderr 合并进服务端滚动缓冲，前端按游标增量读取。
 *
 * 命令结束判定为什么用**哨兵行**：管道模式下既拿不到提示符、也拿不到「本条命令结束」的事件，
 * 唯一可靠的边界是自己往 stdin 再写一条 echo（输出形如 `{TOKEN}{exitCode}|{cwd}`）；
 * 服务端在输出里认出该行即知命令收尾，并把它从输出文本中剥离（用户不该看到哨兵）。
 *
 * 依赖注入口径与 core/exec/sh-tasks.ts 的 ShTaskRunner 一致（spawner/killTree/now 可注入假实现）：
 * 测试不真起 shell，也就不受平台与命令行环境影响。
 */
import { randomUUID } from "node:crypto"
import { spawn } from "node:child_process"
import { existsSync, mkdirSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { basename, join } from "node:path"
import { FsError, fsBadRequest, logicalPath } from "../fs/roots"

/** 并发会话上限：超出直接 400（会话是常驻进程，无上限会被前端误用堆成进程池）。 */
export const TERMINAL_MAX_SESSIONS = 8
/** 空闲回收阈值：30 分钟无交互的会话自动关闭（前端复用会话但用户早已离开的场景）。 */
export const TERMINAL_IDLE_MS = 30 * 60 * 1000
/** 单会话滚动缓冲上限（字符）：超限只保留尾部——前端按游标读增量，历史丢失只影响回看。 */
export const TERMINAL_MAX_BUFFER = 200_000
/** 退出事件队列上限（客户端长时间不读时的防御，避免队列随命令数无界增长）。 */
const MAX_EXITS = 256
/** 哨兵 TOKEN 前缀（每次命令随机后缀：命令输出里几乎不可能自然出现，见 newToken）。 */
export const TERMINAL_TOKEN_PREFIX = "__GBEND_"

/** 本机 Shell 条目（`available` 为探测结果；清单只列真实存在的解释器）。 */
export interface ShellSpec {
  id: string
  name: string
  path: string
  available: boolean
}

/** shell 进程句柄（默认 spawner 返回；测试注入假实现）。 */
export interface TermChild {
  pid: number | null
  /** 写入 stdin（命令/交互输入/哨兵行）。 */
  write(data: string): void
}

/** 进程生成器（依赖注入：默认接 node:child_process.spawn；测试接假实现）。 */
export type TermSpawner = (opts: {
  shell: ShellSpec
  cwd: string
  env?: Record<string, string>
  onData: (chunk: Uint8Array) => void
  onExit: (code: number | null) => void
}) => TermChild

/** 命令结束事件（哨兵行解析结果；cwd 为 shell 自报的绝对路径）。 */
export interface ExitEvent {
  token: string
  code: number
  cwd: string
}

/** 会话对外视图（list 用；cwd 为根内相对路径）。 */
export interface TerminalSessionInfo {
  id: string
  shell: string
  shellName: string
  cwd: string
  root: string
  startedAt: number
  busy: boolean
}

/** 会话创建结果（REST 契约的字段来源）。 */
export interface TerminalSessionCreated extends TerminalSessionInfo {
  /** 当前读取游标（= 缓冲末尾位置）。 */
  cursor: number
  /** 创建瞬间的已有输出（Windows 首行提示等）。 */
  output: string
}

export interface TerminalServiceOptions {
  spawner?: TermSpawner
  /** Shell 清单（注入固定清单便于测试；不注入时按平台探测本机）。 */
  shells?: ShellSpec[] | (() => ShellSpec[])
  /** GEBAI_TERMINAL_SHELL 指定的默认 Shell（id 或路径；不可用则回落到平台默认）。 */
  defaultShell?: string
  maxSessions?: number
  idleMs?: number
  maxBufferChars?: number
  now?: () => number
  /** 进程树终止（测试注入可断言，不真杀进程）。 */
  killTree?: (pid: number | null) => void
}

/** 会话内部状态（不导出：外部只经 TerminalService 方法与视图结构交互）。 */
interface ExitRecord extends ExitEvent {
  /** 事件发生时的绝对游标位置（供按游标重放，见 read）。 */
  pos: number
}

/** 建会话时的占位句柄：spawn 需要回调闭包引用会话对象，故先入表再启动（见 create）。 */
const NO_CHILD: TermChild = { pid: null, write: () => {} }

interface TermSession {
  id: string
  rootId: string
  /** 根绝对路径（cwd 相对它的相对形式供前端展示与审计）。 */
  rootAbs: string
  /** shell 当前工作目录（绝对路径，随哨兵行回报更新）。 */
  cwdAbs: string
  cwdRel: string
  shell: ShellSpec
  env?: Record<string, string>
  child: TermChild
  /** 已落盘的脚本序号（非 ASCII 命令走脚本文件执行，见 scriptCommand）。 */
  scriptSeq?: number
  /** 进程世代：中断/重建后自增，旧进程的异步回调据此作废（否则旧 shell 的退出会把新会话标死）。 */
  gen: number
  alive: boolean
  /** 未收尾的命令 TOKEN（空集 = 空闲）：busy 判据，也是识别输出里哨兵的依据。 */
  tokens: Set<string>
  /** 滚动缓冲与已写游标（游标是绝对字符位置，缓冲只保留尾部）。 */
  buffer: string
  base: number
  /** 未处理完的输出（未换行行尾）与其已推送前缀长度（见 flushPartial）。 */
  pending: string
  pendingFlushed: number
  /** 退出事件队列与已投递水位（见 read）。 */
  exits: ExitRecord[]
  ack: number
  decoder: StreamTextDecoder
  startedAt: number
  lastActive: number
}

/**
 * 增量解码：UTF-8 为主、GBK 兜底（口径同 core/security/sandbox.ts 的 decodeOutput，但按块流式）。
 *
 * 为什么保留双解码器：UTF-8 解码器会把多字节序列的**未完成尾部字节**留在自己的缓冲里，
 * 因此输出里出现 U+FFFD 只代表真正的非法字节（Windows 老程序不遵循 chcp 65001 仍按 GBK 输出的场合），
 * 此时改用 GBK 结果；两个解码器处理各自的多字节边界，无须调用方拼块。
 */
export class StreamTextDecoder {
  private utf8 = new TextDecoder("utf-8")
  private gbk: TextDecoder | null = null
  private gbkUnavailable = false

  push(chunk: Uint8Array): string {
    const text = this.utf8.decode(chunk, { stream: true })
    if (!text.includes("\uFFFD")) return text
    if (!this.gbk && !this.gbkUnavailable) {
      try {
        // Bun/Node 支持 WHATWG GBK 解码；类型定义未收录该 label，绕行断言
        this.gbk = new TextDecoder("gbk" as never)
      } catch {
        this.gbkUnavailable = true
      }
    }
    return this.gbk ? this.gbk.decode(chunk, { stream: true }) : text
  }

  /** 重置（中断后重建 shell：新进程的字节流与旧流无关，残留的半截序列必须丢弃）。 */
  reset(): void {
    this.utf8 = new TextDecoder("utf-8")
    this.gbk = this.gbkUnavailable ? null : new TextDecoder("gbk" as never)
  }
}

/** 随机哨兵 TOKEN（一次性）：形如 `__GBEND_ab12cd__`。 */
export function newToken(): string {
  return `${TERMINAL_TOKEN_PREFIX}${randomUUID().replace(/-/g, "").slice(0, 6)}__`
}

/** 命令结束哨兵（各 shell 语法）：输出 `{TOKEN}{exitCode}|{cwd}` 一行。 */
export function sentinelCommand(shellId: string, token: string): string {
  if (shellId === "cmd") return `echo ${token}%errorlevel%^|%CD%`
  if (shellId === "pwsh" || shellId === "powershell") {
    return `Write-Output ("${token}" + $(if ($?) {0} else {1}) + "|" + (Get-Location).Path)`
  }
  return `echo "${token}$?|$PWD"`
}

/** shell 启动参数：cmd 关命令回显（/q），PowerShell 关版本横幅（-NoLogo）——免得首屏被环境字占满。 */
function shellArgs(id: string): string[] {
  if (id === "cmd") return ["/q"]
  if (id === "pwsh" || id === "powershell") return ["-NoLogo"]
  return []
}

/** Windows 控制台默认 GBK 代码页：shell 起来后先经 stdin 切 UTF-8；重定向掉提示行，不留噪声输出。 */
function shellInit(id: string): string {
  if (process.platform !== "win32") return ""
  if (id === "pwsh" || id === "powershell") {
    // 同时放开进程级执行策略：非 ASCII 命令走 `& "脚本.ps1"`（见 needsScriptFile），
    // 默认 Restricted 会直接拒绝执行；-Scope Process 只影响本会话进程，不动系统设置。
    return "chcp 65001 | Out-Null; Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force"
  }
  return "chcp 65001 >nul"
}

/** 终端会话的临时脚本目录（非 ASCII 命令落盘执行用；随会话关闭清理）。 */
function scriptDirOf(id: string): string {
  return join(tmpdir(), "gebai-term", id)
}

/**
 * 该命令是否需要落成脚本文件再执行。
 *
 * cmd 与 PowerShell 从**管道**读 stdin 时按控制台 OEM/ANSI 代码页解析，UTF-8 的中文命令会被解成乱码：
 * cmd 实测会停在 `More?` 未闭合状态并随后退出 shell（用户只是敲了一条中文 echo）。
 * 落盘脚本由 shell 按自身代码页读取（会话已 chcp 65001），中文命令因此可正常执行；
 * POSIX shell 直接写管道即可（字节原样传递、不解释编码），无需落盘。
 */
function needsScriptFile(data: string, shellId: string): boolean {
  if (shellId !== "cmd" && shellId !== "powershell" && shellId !== "pwsh") return false
  // eslint-disable-next-line no-control-regex
  return /[^\x00-\x7F]/.test(data)
}

/** 只保留最近 keep 个脚本文件（会话可能跑很久，脚本不该无界堆积）。 */
function pruneScripts(dir: string, keepFrom: number): void {
  try {
    for (const name of readdirSync(dir)) {
      const n = /^c(\d+)\./.exec(name)
      if (n && Number(n[1]) <= keepFrom) rmSync(join(dir, name), { force: true })
    }
  } catch {
    /* 目录不存在/无权限：忽略（脚本清理不该影响命令执行） */
  }
}

/** 默认进程生成器：常驻 shell + 三条管道（stdin 保持打开，stdout/stderr 合并回调）。 */
const defaultSpawner: TermSpawner = (opts) => {
  const isWin = process.platform === "win32"
  const child = spawn(opts.shell.path, shellArgs(opts.shell.id), {
    cwd: opts.cwd,
    env: { ...process.env, ...opts.env },
    stdio: ["pipe", "pipe", "pipe"],
    // POSIX 下自建进程组：中断时按 -pid 整组终止，shell 派生的长命令不留残（Windows 走 taskkill /T）
    detached: !isWin,
  })
  child.stdout?.on("data", (d: Buffer) => opts.onData(d))
  child.stderr?.on("data", (d: Buffer) => opts.onData(d))
  child.on("error", () => opts.onExit(null))
  child.on("close", (code) => opts.onExit(code))
  // stdin 的错误（EPIPE：shell 已退出）必须有监听者，否则流上的 error 事件会抛到进程级
  child.stdin?.on("error", () => {})
  return {
    pid: child.pid ?? null,
    write: (data: string) => {
      try {
        child.stdin?.write(data)
      } catch {
        /* 进程已退出 */
      }
    },
  }
}

/** 进程树终止：Windows 走 taskkill /T（只杀 shell 会留下它派生的长命令），POSIX 按进程组 -pid。 */
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

/** 在 PATH 中查找可执行文件（Windows 按 PATHEXT 补扩展名探测）。 */
function which(cmd: string): string | null {
  const isWin = process.platform === "win32"
  if (cmd.includes("/") || cmd.includes("\\")) {
    if (!existsSync(cmd)) return null
    try {
      return statSync(cmd).isFile() ? cmd : null
    } catch {
      return null
    }
  }
  const exts = isWin ? (process.env.PATHEXT ?? ".COM;.EXE;.BAT;.CMD").split(";").filter(Boolean) : [""]
  const dirs = (process.env.PATH ?? "").split(isWin ? ";" : ":")
  for (const dir of dirs) {
    if (!dir) continue
    for (const ext of exts) {
      const name = ext && !cmd.toLowerCase().endsWith(ext.toLowerCase()) ? `${cmd}${ext}` : cmd
      const p = join(dir, name)
      try {
        if (statSync(p).isFile()) return p
      } catch {
        /* 不在该目录：继续 */
      }
    }
  }
  return null
}

/** 按平台探测本机 Shell 清单（只列真实存在的解释器；顺序即默认优先级）。 */
export function detectShells(): ShellSpec[] {
  const out: ShellSpec[] = []
  const add = (id: string, name: string, cmd: string) => {
    const p = which(cmd)
    if (p) out.push({ id, name, path: p, available: true })
  }
  if (process.platform === "win32") {
    add("cmd", "命令提示符", "cmd.exe")
    add("powershell", "Windows PowerShell", "powershell.exe")
    add("pwsh", "PowerShell", "pwsh.exe")
    return out
  }
  add("bash", "Bash", "/bin/bash")
  add("sh", "sh", "/bin/sh")
  const login = process.env.SHELL
  if (login && !out.some((s) => s.path === login)) {
    const p = which(login)
    if (p) out.push({ id: basename(p).replace(/\.[^.]+$/, ""), name: "登录 Shell", path: p, available: true })
  }
  return out
}

/**
 * 终端会话服务：会话表 + 输出缓冲 + 哨兵解析 + 生命周期（创建/输入/读取/中断/关闭/回收）。
 * 全部方法同步（进程交互只有管道写入与内存缓冲，无 I/O 等待），路由层直接取返回值组响应。
 */
export class TerminalService {
  private spawner: TermSpawner
  private shellList?: ShellSpec[] | (() => ShellSpec[])
  private configuredShell: string
  private maxSessions: number
  private idleMs: number
  private maxBufferChars: number
  private now: () => number
  private killTree: (pid: number | null) => void
  private sessions = new Map<string, TermSession>()

  constructor(opts: TerminalServiceOptions = {}) {
    this.spawner = opts.spawner ?? defaultSpawner
    this.shellList = opts.shells
    this.configuredShell = opts.defaultShell ?? ""
    this.maxSessions = opts.maxSessions ?? TERMINAL_MAX_SESSIONS
    this.idleMs = opts.idleMs ?? TERMINAL_IDLE_MS
    this.maxBufferChars = opts.maxBufferChars ?? TERMINAL_MAX_BUFFER
    this.now = opts.now ?? Date.now
    this.killTree = opts.killTree ?? defaultKillTree
  }

  /** 本机 Shell 清单（注入优先，便于测试固定平台差异）。 */
  shells(): ShellSpec[] {
    const injected = typeof this.shellList === "function" ? this.shellList() : this.shellList
    return injected ?? detectShells()
  }

  /** 能力与元信息（可用性开关由路由层按 GEBAI_TERMINAL / 沙箱 / 只读判定后合并）。 */
  info(): { enabled: boolean; shells: ShellSpec[]; defaultShell: string; maxSessions: number; idleMs: number } {
    const shells = this.shells()
    return {
      enabled: true,
      shells,
      defaultShell: this.defaultShellId(shells),
      maxSessions: this.maxSessions,
      idleMs: this.idleMs,
    }
  }

  /** 默认 Shell id：GEBAI_TERMINAL_SHELL 命中（id 或路径）优先，否则取平台优先级里第一个可用的。 */
  private defaultShellId(shells: ShellSpec[]): string {
    const usable = shells.filter((s) => s.available)
    if (!usable.length) return ""
    if (this.configuredShell) {
      const want = this.configuredShell
      const hit = usable.find((s) => s.id === want || s.path === want || basename(s.path) === basename(want))
      if (hit) return hit.id
    }
    return usable[0].id
  }

  /** 创建会话：spawn 常驻 shell 并写入编码初始化命令；并发超限抛 400。 */
  create(opts: { rootId: string; rootAbs: string; cwdAbs: string; shell?: ShellSpec | string; env?: Record<string, string> }): TerminalSessionCreated {
    this.sweep()
    if (this.sessions.size >= this.maxSessions) {
      throw fsBadRequest(`终端会话数已达上限（${this.maxSessions}）：请先关闭不用的会话再新建。`)
    }
    const shell = this.resolveShell(opts.shell)
    if (!shell) {
      const usable = this.shells().filter((s) => s.available)
      if (!usable.length) {
        throw new FsError(503, "本机未检测到可用的 Shell（Windows 探测 cmd.exe / powershell.exe / pwsh.exe，POSIX 探测 /bin/bash、/bin/sh、$SHELL）")
      }
      throw fsBadRequest(`指定的 Shell 不可用: ${String(opts.shell)}（可用: ${usable.map((s) => s.id).join(" / ")}）`)
    }
    const id = `t${randomUUID().replace(/-/g, "").slice(0, 8)}`
    const s: TermSession = {
      id,
      rootId: opts.rootId,
      rootAbs: opts.rootAbs,
      cwdAbs: opts.cwdAbs,
      cwdRel: logicalPath(opts.rootAbs, opts.cwdAbs),
      shell,
      env: opts.env,
      child: NO_CHILD,
      gen: 0,
      alive: false,
      tokens: new Set(),
      buffer: "",
      base: 0,
      pending: "",
      pendingFlushed: 0,
      exits: [],
      ack: 0,
      decoder: new StreamTextDecoder(),
      startedAt: this.now(),
      lastActive: this.now(),
    }
    this.sessions.set(id, s)
    try {
      this.spawn(s)
    } catch (err) {
      // 启动失败（shell 路径失效/权限）不留在会话表里占额度
      this.sessions.delete(id)
      throw err
    }
    return { ...this.view(s), cursor: this.cursorOf(s), output: s.buffer }
  }

  /**
   * 写入会话 stdin。
   * - `exec:true`（默认）：写 `data + "\n"` 后立即写哨兵行，据此判定本条命令结束；
   * - `exec:false`：原样写 `data`（交互式输入、控制字符）。
   * 返回的 cursor 是**写入前**的读取位置：前端以它作 since 读增量，命令刚产生的输出不会被漏掉
   * （若返回写入后的位置，写与读之间已到达的输出会被当成「已消费」）。
   */
  input(opts: { id: string; data: string; exec?: boolean }): { ok: true; cursor: number; root: string; cwd: string; shell: string } {
    const s = this.require(opts.id)
    if (!s.alive) throw new FsError(409, "终端会话已结束（Shell 已退出）：请关闭该会话后重新创建。")
    const cursor = this.cursorOf(s)
    const exec = opts.exec !== false
    if (exec) {
      const token = newToken()
      s.tokens.add(token)
      s.child.write(`${this.scriptCommand(s, opts.data)}\n`)
      s.child.write(`${sentinelCommand(s.shell.id, token)}\n`)
    } else {
      s.child.write(opts.data)
    }
    s.lastActive = this.now()
    return { ok: true, cursor, root: s.rootId, cwd: s.cwdRel, shell: s.shell.id }
  }

  /**
   * 把命令转换成实际写入 stdin 的那一行。
   * 普通命令原样返回；含非 ASCII 且 shell 为 cmd / PowerShell 时（见 needsScriptFile）落成脚本文件，
   * 返回 launcher（`call "…"` / `& "…"`）——否则中文命令会被 shell 按本地代码页解成乱码。
   * 落盘失败退回原样写入：与旧行为一致，至少让用户看到 shell 自己的报错。
   */
  private scriptCommand(s: TermSession, data: string): string {
    if (!needsScriptFile(data, s.shell.id)) return data
    try {
      const dir = scriptDirOf(s.id)
      mkdirSync(dir, { recursive: true })
      const seq = (s.scriptSeq = (s.scriptSeq ?? 0) + 1)
      const ext = s.shell.id === "cmd" ? "cmd" : "ps1"
      const file = join(dir, `c${seq}.${ext}`)
      writeFileSync(file, `${data}\r\n`, "utf8")
      if (seq > 20) pruneScripts(dir, seq - 20)
      return s.shell.id === "cmd" ? `call "${file}"` : `& "${file}"`
    } catch {
      return data
    }
  }

  /** 会话私有的脚本目录清理（关闭/回收时调用；尽力而为）。 */
  private cleanupScripts(id: string): void {
    try {
      rmSync(scriptDirOf(id), { recursive: true, force: true })
    } catch {
      /* 忽略 */
    }
  }

  /**
   * 增量读取：返回 `since` 之后的输出（哨兵行已剥离）与新游标。
   *
   * exits 的投递口径：**队列式**（每个事件只投递一次），因为哨兵行可能出现在输出末尾而不占任何文本位置
   * （如 `cd dir` 这类无输出命令，事件位置恰好等于客户端当前游标，纯按位置比较会把事件永久漏掉）；
   * 同时保留「调用方游标早于事件位置」的重放——旧游标重读仍能拿到事件。
   */
  read(id: string, since: number): { cursor: number; text: string; exits: ExitEvent[]; alive: boolean } {
    const s = this.require(id)
    const from = Number.isFinite(since) ? Math.max(0, Math.floor(since)) : 0
    const text = from <= s.base ? s.buffer : s.buffer.slice(from - s.base)
    const exits: ExitEvent[] = []
    for (let i = 0; i < s.exits.length; i++) {
      const e = s.exits[i]
      if (i >= s.ack || e.pos > from) exits.push({ token: e.token, code: e.code, cwd: e.cwd })
    }
    s.ack = s.exits.length
    this.trimExits(s)
    return { cursor: this.cursorOf(s), text, exits, alive: s.alive }
  }

  /** 中断当前命令：按进程树终止 shell，再以原 cwd 重建（保留会话 id、滚动缓冲与 cwd）。 */
  interrupt(id: string): { ok: true; cwd: string } {
    const s = this.require(id)
    this.killTree(s.child.pid)
    // 世代自增：旧进程的 close 回调不再影响本会话（否则重建后被立即标死）
    s.gen++
    s.alive = false
    s.tokens.clear()
    // 被杀的 shell 不会补完半截行（可能是未解析完的哨兵）：与新进程的字节流一并丢弃
    s.pending = ""
    s.pendingFlushed = 0
    // 半截输出（无换行收尾）已被即时推送：先闭合该行，提示语不与之粘连
    if (s.buffer && !s.buffer.endsWith("\n")) this.append(s, "\n")
    this.append(s, "[已中断当前命令]\n")
    try {
      this.spawn(s)
    } catch (err) {
      s.alive = false
      this.append(s, `[重建 Shell 失败：${err instanceof Error ? err.message : String(err)}]\n`)
    }
    s.lastActive = this.now()
    return { ok: true, cwd: s.cwdRel }
  }

  /** 关闭会话（幂等：会话不存在/已关闭同样返回成功，前端不必先查存在性）。 */
  close(id: string): { ok: true } {
    const s = this.sessions.get(id)
    if (!s) return { ok: true }
    this.killTree(s.child.pid)
    s.gen++
    s.alive = false
    this.sessions.delete(id)
    this.cleanupScripts(id)
    return { ok: true }
  }

  /** 会话清单（先回收空闲会话）。 */
  list(): TerminalSessionInfo[] {
    this.sweep()
    return [...this.sessions.values()].map((s) => this.view(s))
  }

  /** 空闲回收：关闭超过 idleMs 未交互的会话；**跳过正在执行命令的**（回收会把用户任务腰斩）。
   *  惰性触发（create/list/read/info 前），不额外占用定时器。 */
  sweep(): string[] {
    const now = this.now()
    const closed: string[] = []
    for (const s of [...this.sessions.values()]) {
      if (s.tokens.size > 0) continue
      if (now - s.lastActive < this.idleMs) continue
      this.killTree(s.child.pid)
      s.gen++
      s.alive = false
      this.sessions.delete(s.id)
      this.cleanupScripts(s.id)
      closed.push(s.id)
    }
    return closed
  }

  /* ------------------------------ 内部：进程与输出 ------------------------------ */

  private require(id: string): TermSession {
    const s = this.sessions.get(id)
    if (!s) throw new FsError(404, `终端会话不存在: ${id}`)
    return s
  }

  private resolveShell(shell?: ShellSpec | string): ShellSpec | null {
    const usable = this.shells().filter((s) => s.available)
    if (!shell) {
      const id = this.defaultShellId(this.shells())
      return usable.find((s) => s.id === id) ?? usable[0] ?? null
    }
    if (typeof shell === "string") return usable.find((s) => s.id === shell) ?? null
    return shell.available ? shell : null
  }

  /** 启动（或重建）shell 进程并写入编码初始化命令。 */
  private spawn(s: TermSession): void {
    const gen = ++s.gen
    s.child = this.spawner({
      shell: s.shell,
      cwd: s.cwdAbs,
      env: s.env,
      onData: (chunk) => {
        if (gen === s.gen) this.onData(s, chunk)
      },
      onExit: (code) => {
        if (gen === s.gen) this.onExit(s, code)
      },
    })
    s.alive = true
    const init = shellInit(s.shell.id)
    if (init) s.child.write(`${init}\n`)
  }

  private onData(s: TermSession, chunk: Uint8Array): void {
    // 有输出即视为活跃：长命令（构建/日志）不该被空闲回收误伤
    s.lastActive = this.now()
    const text = s.decoder.push(chunk)
    if (text) this.ingest(s, text)
  }

  private onExit(s: TermSession, code: number | null): void {
    s.alive = false
    s.tokens.clear()
    this.append(s, `[Shell 已退出（退出码 ${code ?? "未知"}）]\n`)
  }

  /** 缓冲末尾位置（游标）：缓冲只保留尾部，故当前位置 = 起点 + 长度。 */
  private cursorOf(s: TermSession): number {
    return s.base + s.buffer.length
  }

  /** 输出文本：按行处理（哨兵是整行形态），未换行的行尾按需即时推送。 */
  private ingest(s: TermSession, text: string): void {
    s.pending += text
    for (;;) {
      const nl = s.pending.indexOf("\n")
      if (nl < 0) break
      const raw = s.pending.slice(0, nl)
      s.pending = s.pending.slice(nl + 1)
      // 行尾 CR 必须剥（Windows shell 输出为 CRLF）：否则哨兵里的 cwd 会带 \r，重建 shell 时目录名非法
      const line = raw.endsWith("\r") ? raw.slice(0, -1) : raw
      const flushed = s.pendingFlushed
      s.pendingFlushed = 0
      this.handleLine(s, line, Math.min(flushed, line.length))
    }
    this.flushPartial(s)
  }

  private handleLine(s: TermSession, line: string, flushed: number): void {
    const hit = this.matchSentinel(s, line)
    if (hit) {
      // 哨兵前的残留输出（上一条命令以不换行收尾）：补一个换行闭合该行，免得与后续输出粘连
      if (hit.head.length > flushed) this.append(s, `${hit.head.slice(flushed)}\n`)
      else if (hit.head.length > 0) this.append(s, "\n")
      this.finish(s, hit)
      return
    }
    this.append(s, `${line.slice(flushed)}\n`)
  }

  /**
   * 未换行的行尾：能判定为普通输出就立即推送（交互式提示符、进度输出这类不换行的内容必须可见），
   * 唯一例外是「可能是哨兵开头」的片段——它必须留住，否则哨兵被拆到两个输出块时会漏进文本。
   */
  private flushPartial(s: TermSession): void {
    const p = s.pending
    if (!p) return
    for (const token of s.tokens) {
      if (token.startsWith(p) || p.startsWith(token)) return
    }
    const flush = p.replace(/\r+$/, "") // 行尾 \r 留到整行处理时再剥（此处剥掉会与 flushed 计数错位）
    if (flush.length <= s.pendingFlushed) return
    this.append(s, flush.slice(s.pendingFlushed))
    s.pendingFlushed = flush.length
  }

  /**
   * 在行内定位未收尾的哨兵（TOKEN 随机且一次性，命中即消费）。
   * 两点容错：
   * 1. 「行内」而非「行首」：上一条命令若以不换行的输出收尾（如 cmd 的 `set /p`），或 shell 在管道模式
   *    下仍打印提示符（cmd.exe 即如此），哨兵会拼在别的文本后面，只看行首会漏判、busy 永不结束；
   * 2. 严格匹配 `{code}|{cwd}` 形态：命令回显（用户执行 `echo on`）里也会出现 TOKEN，
   *    但那行的载荷是 `%errorlevel%^|%CD%` 这类字面量，跳过它才能等到真正的结果行。
   */
  private matchSentinel(s: TermSession, line: string): { token: string; head: string; code: number; cwd: string } | null {
    for (const token of s.tokens) {
      const at = line.indexOf(token)
      if (at < 0) continue
      const payload = /^(\d+)\|(.*)$/.exec(line.slice(at + token.length))
      if (!payload) continue
      s.tokens.delete(token)
      return { token, head: line.slice(0, at), code: Number.parseInt(payload[1], 10), cwd: payload[2] }
    }
    return null
  }

  /** 命令收尾：记退出事件（带当前游标位置），并按 shell 自报的 cwd 更新会话位置（用户 cd 后的真相）。 */
  private finish(s: TermSession, hit: { token: string; code: number; cwd: string }): void {
    s.exits.push({ token: hit.token, code: hit.code, cwd: hit.cwd, pos: this.cursorOf(s) })
    if (hit.cwd) {
      s.cwdAbs = hit.cwd
      s.cwdRel = logicalPath(s.rootAbs, hit.cwd)
    }
    s.lastActive = this.now()
    this.trimExits(s)
  }

  /** 写入滚动缓冲：超限只保留尾部（前端按游标读增量，历史丢失只影响回看）。 */
  private append(s: TermSession, text: string): void {
    if (!text) return
    s.buffer += text
    if (s.buffer.length > this.maxBufferChars) {
      const drop = s.buffer.length - this.maxBufferChars
      s.buffer = s.buffer.slice(drop)
      s.base += drop
      this.trimExits(s)
    }
  }

  /** 退出事件裁剪：已投递且落在缓冲之外的事件不再可重放；队列长度另设上限防无界增长。 */
  private trimExits(s: TermSession): void {
    let drop = 0
    while (drop < s.exits.length && drop < s.ack && s.exits[drop].pos <= s.base) drop++
    if (drop > 0) {
      s.exits.splice(0, drop)
      s.ack -= drop
    }
    const over = s.exits.length - MAX_EXITS
    if (over > 0) {
      s.exits.splice(0, over)
      s.ack = Math.max(0, s.ack - over)
    }
  }

  private view(s: TermSession): TerminalSessionInfo {
    return {
      id: s.id,
      shell: s.shell.id,
      shellName: s.shell.name,
      cwd: s.cwdRel,
      root: s.rootId,
      startedAt: s.startedAt,
      busy: s.tokens.size > 0,
    }
  }
}
