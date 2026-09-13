/**
 * WS 终端域消息：PTY 会话的创建 / 附加 / 输入 / 尺寸 / 关闭（文件工作台终端面板的实时通道）。
 *
 * 为什么终端走 WS 而不是 REST 轮询：PTY 是**字节级交互**——每次按键（含 Tab 补全、方向键、Ctrl+C）
 * 都要即时进 shell，shell 的输出也要求即时回显；250ms 轮询下打字与补全都不可用。WS 双向低延迟，
 * 且沿用项目既有 `/ws` 通道的鉴权、跨源防护与背压 sink，不新增端口与握手逻辑。
 *
 * 门禁与 REST 终端端点一致：文件工作台开关、终端开关、沙箱（终端等同任意命令执行，非豁免用户拒绝）、
 * 只读模式。命令审计：PTY 模式没有「命令边界」可解析，按**输入流里的回车**切分记录（近似但可用），
 * 与会话创建一并写入 audit-fs.jsonl。
 */
import type { AuthUser } from "../auth"
import type { AppDeps } from "../app"
import { mkdirSync, statSync } from "node:fs"
import { resolveInRoot, resolveRoot } from "../core/fs/roots"
import { detectShells, type ShellSpec } from "../core/exec/term-session"
import { buildRootContext, fsEnabled } from "../routes/fs-shared"
import type { WsHandler } from "./context"

/** 终端未启用（GEBAI_TERMINAL=false 或服务未注入）。 */
const TERM_OFF = "终端能力未启用（GEBAI_TERMINAL=false）"
/** 只读检视环境：不允许执行命令。 */
const READ_ONLY = "文件工作台为只读模式（GEBAI_FS_WRITE=false）：不允许执行命令"
/** 沙箱非豁免用户：不开放终端。 */
const SANDBOX_DENIED = "沙箱模式下不开放终端（终端等同任意命令执行）：请使用本地模式或沙箱豁免用户"

/** 每会话的输入缓冲（用于把输入流切成「命令」记审计）：仅驻内存，会话关闭即清。 */
const inputBuffers = new Map<string, string>()

/** 门禁：返回中文原因表示拒绝，null 表示放行。 */
function gate(d: AppDeps, user: AuthUser): string | null {
  if (!fsEnabled(d)) return "文件工作台未启用（GEBAI_FS_ENABLED=false）"
  if (d.config.terminalEnabled === false) return TERM_OFF
  if (d.sandbox.enforcedFor(user.id)) return SANDBOX_DENIED
  if (!d.terminalPty) return TERM_OFF
  if (d.config.fsWrite === false) return READ_ONLY
  return null
}

/** cwd 目录保证存在（会话 tmp/ 这类目录并非必然创建，缺失目录会让 shell 启动失败）。 */
function ensureDir(abs: string): void {
  try {
    if (statSync(abs).isDirectory()) return
  } catch {
    /* 不存在：建之 */
  }
  try {
    mkdirSync(abs, { recursive: true })
  } catch {
    /* 尽力而为 */
  }
}

/** 记一条终端审计（命令近似切分：输入里的回车即一条）。 */
function audit(d: AppDeps, user: string, entry: { root: string; path?: string; cmd: string; shell: string; ok: boolean; error?: string }): void {
  d.fsAudit?.record({
    ts: Date.now(),
    user,
    source: "web",
    action: "term.exec",
    root: entry.root,
    path: entry.path,
    detail: { cmd: entry.cmd.split(/\r?\n/)[0]?.slice(0, 200) ?? "", shell: entry.shell, mode: "pty" },
    ok: entry.ok,
    error: entry.error,
  })
}

/** 选 shell：请求指定优先，其次默认（GEBAI_TERMINAL_SHELL），最后平台优先级第一个可用。 */
function pickShell(d: AppDeps, want: string): ShellSpec | null {
  const shells = d.terminal?.shells() ?? detectShells()
  const usable = shells.filter((s) => s.available)
  if (!usable.length) return null
  if (want) {
    const hit = usable.find((s) => s.id === want || s.path === want)
    if (hit) return hit
  }
  const def = d.terminal?.info().defaultShell ?? ""
  return usable.find((s) => s.id === def) ?? usable[0]!
}

export const terminalHandlers: Record<string, WsHandler> = {
  /** 新建 PTY 会话并订阅其输出。 */
  "term.open": async ({ d, user, p, ws, reply }) => {
    const denied = gate(d, user)
    if (denied) return reply(false, undefined, denied)
    const svc = d.terminalPty!
    const avail = svc.available()
    if (!avail.ok) return reply(false, undefined, avail.reason ?? "PTY 驱动不可用")
    const sessionId = typeof p.session === "string" && p.session ? p.session : undefined
    const ctx = await buildRootContext(d, user, { sessionId, envInput: p.env, withSessions: true })
    const root = resolveRoot(String(p.root ?? ""), ctx)
    if (!root.writable) return reply(false, undefined, READ_ONLY)
    const cwdAbs = resolveInRoot(root.abs, String(p.cwd ?? ""), { allowAbsolute: root.kind === "abs" })
    ensureDir(cwdAbs)
    const shell = pickShell(d, String(p.shell ?? ""))
    if (!shell) {
      return reply(false, undefined, "本机未检测到可用的 Shell（Windows 探测 cmd.exe / powershell.exe / pwsh.exe，POSIX 探测 /bin/bash、/bin/sh、$SHELL）")
    }
    const cols = Number(p.cols) || 120
    const rows = Number(p.rows) || 30
    const info = svc.create({ rootId: root.id, rootAbs: root.abs, cwdAbs, shell, cols, rows })
    subscribe(svc, info.id, ws)
    audit(d, user.id, { root: root.id, path: info.cwd, cmd: "(新建终端)", shell: shell.id, ok: true })
    return reply(true, { session: info })
  },

  /** 附加到已有会话（页面刷新/切换标签后重连）：回放缓冲 + 继续增量推送。 */
  "term.attach": async ({ d, user, p, ws, reply }) => {
    const denied = gate(d, user)
    if (denied) return reply(false, undefined, denied)
    const svc = d.terminalPty!
    const id = String(p.id ?? "")
    if (!svc.has(id)) return reply(false, undefined, "终端会话不存在或已关闭")
    const cols = Number(p.cols)
    const rows = Number(p.rows)
    if (cols > 0 && rows > 0) {
      try {
        svc.resize(id, cols, rows)
      } catch {
        /* 会话已退出：忽略 */
      }
    }
    subscribe(svc, id, ws)
    return reply(true, { id })
  },

  /** 键盘输入 / 粘贴内容透传（原始字节级）。 */
  "term.input": ({ d, user, p, reply }) => {
    const denied = gate(d, user)
    if (denied) return reply(false, undefined, denied)
    const svc = d.terminalPty!
    const id = String(p.id ?? "")
    const data = typeof p.data === "string" ? p.data : ""
    svc.write(id, data)
    // 审计近似切分：输入里每遇到回车即视为执行了一条命令
    if (data.includes("\r") || data.includes("\n")) {
      const buf = (inputBuffers.get(id) ?? "") + data
      const parts = buf.split(/\r?\n/)
      const rest = parts.pop() ?? ""
      for (const cmd of parts) {
        if (cmd.trim()) audit(d, user.id, { root: "", cmd, shell: "", ok: true })
      }
      inputBuffers.set(id, rest)
    } else {
      const acc = (inputBuffers.get(id) ?? "") + data
      inputBuffers.set(id, acc.slice(-500))
    }
    return reply(true)
  },

  /** 终端尺寸变化（xterm fit 后下发；TUI 程序据此重排）。 */
  "term.resize": ({ d, user, p, reply }) => {
    const denied = gate(d, user)
    if (denied) return reply(false, undefined, denied)
    d.terminalPty!.resize(String(p.id ?? ""), Number(p.cols) || 0, Number(p.rows) || 0)
    return reply(true)
  },

  /**
   * 中断当前命令（Ctrl+C）：终止进程树并以原目录重建 shell。
   * 为何不是直写 ETX：ConPTY 下 `\x03` 不会被转成 CTRL_C_EVENT（详见 core/exec/pty-session.ts）。
   */
  "term.interrupt": ({ d, user, p, reply }) => {
    const denied = gate(d, user)
    if (denied) return reply(false, undefined, denied)
    audit(d, user.id, { root: "", cmd: "(中断当前命令)", shell: "", ok: true })
    return reply(true, d.terminalPty!.interrupt(String(p.id ?? "")))
  },

  /** 关闭会话（前端关闭标签）。 */
  "term.close": ({ d, p, reply }) => {
    if (!fsEnabled(d)) return reply(false, undefined, "文件工作台未启用（GEBAI_FS_ENABLED=false）")
    const id = String(p.id ?? "")
    d.terminalPty?.close(id)
    inputBuffers.delete(id)
    return reply(true)
  },

  /** 会话清单（页面重载后接管已有会话用）。 */
  "term.list": ({ d, user, reply }) => {
    const denied = gate(d, user)
    if (denied) return reply(false, undefined, denied)
    return reply(true, { sessions: d.terminalPty!.list() })
  },
}

/** 订阅会话输出并把事件推给该连接（消息类型 `term.out` / `term.exit` / `term.ready` / `term.error`）。 */
function subscribe(svc: NonNullable<AppDeps["terminalPty"]>, id: string, ws: { send(data: string): void }): void {
  svc.subscribe(
    id,
    (evt) => {
      try {
        ws.send(JSON.stringify({ type: `term.${evt.type}`, payload: { id, ...evt } }))
      } catch {
        /* 连接已断开：会话侧会在 detach（WS close）时清掉订阅 */
      }
    },
    ws,
  )
}

/** 仅供测试/诊断：当前输入缓冲条数。 */
export function terminalInputBufferCount(): number {
  return inputBuffers.size
}
