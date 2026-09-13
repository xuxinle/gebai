/**
 * 文件工作台 · 终端面板（真 PTY + xterm.js）。
 *
 * 与服务端的分工：服务端在 Windows 上给出真伪控制台（ConPTY，见 core/exec/pty-session.ts），
 * 本文件用 **xterm.js**（VSCode 同款终端内核）渲染 ANSI 全序列——真彩色、光标定位、清屏、
 * 行内编辑、TUI 全屏程序都由它处理；键盘输入经 `onData` 原样回传（Tab 补全、方向键历史、
 * Ctrl+C 全交给 shell，不再前端模拟），尺寸经 `FitAddon` 量出后回传（TUI 程序据此重排）。
 *
 * 通道：复用项目既有 `/ws`（消息 `term.open/attach/input/resize/close/list` + 服务端推送
 * `term.out/exit/ready/error`）——终端是字节级交互，轮询做不到按键即时（详见 ws-handlers/terminal.ts）。
 *
 * 资源：xterm 及其 addon 走 vendor 静态伺服（`/vendor/xterm/*`，稳定文件名，UMD script 注入），
 * 与 Monaco / mermaid 同一套约定：不进打包链、dev-reload 重建后 URL 不变。类型仍按包导入（仅编译期）。
 *
 * 会话与连接解耦：标签关闭才关会话；工具窗收起只是 deactivate（WS 保留），页面刷新后按会话 id
 * 重新 attach（服务端回放缓冲），shell 里跑着的东西不丢。
 */
import { clear, dropdown, h, icon, showMenu, toast } from "./ui"
import { pathTail, samePath } from "./terminal-core"
import "../css/terminal.css"
import "../css/terminal-pty.css"
import type { Terminal as XTerm } from "@xterm/xterm"
import type { FitAddon } from "@xterm/addon-fit"
import type { SearchAddon } from "@xterm/addon-search"
import type { TerminalHooks, TerminalPanel } from "./terminal-legacy"

/** 字号 / 跟随根 的本地持久化键（与降级实现同口径）。 */
const FONT_KEY = "gebai.ui.termFontSize"
const FOLLOW_KEY = "gebai.ui.termFollowRoot"
/** 认证令牌（服务模式登录后写入，与聊天页共享）。 */
const AUTH_TOKEN_KEY = "gebai.auth.token"

/** xterm 运行时（UMD 全局：`Terminal` 与各 addon 都直接挂构造函数本身）。 */
interface XtermVendor {
  Terminal: new (opts: Record<string, unknown>) => XTerm
  FitAddon: new () => FitAddon
  SearchAddon: new () => SearchAddon
  WebLinksAddon: new () => { activate(terminal: XTerm): void; dispose(): void }
}

/* ------------------------------ vendor 加载 ------------------------------ */

function basePath(): string {
  return (import.meta.env.BASE_URL || "/").replace(/\/$/, "")
}

/**
 * 动态 import 一个 vendor 模块（绝对 URL，绕开打包链）：xterm 6 的 UMD 包无法把导出挂到全局
 * （见 build-vendor.ts 注释），只能按 ESM 取命名导出。模块缓存天然幂等，另存 Promise 以免并发重复请求。
 */
const moduleCache = new Map<string, Promise<Record<string, unknown>>>()
function loadModule(url: string): Promise<Record<string, unknown>> {
  const hit = moduleCache.get(url)
  if (hit) return hit
  const p = import(/* @vite-ignore */ url) as Promise<Record<string, unknown>>
  moduleCache.set(url, p)
  return p
}

/** 注入样式表（同 URL 只注入一次）。 */
function loadCss(url: string): void {
  if (document.querySelector(`link[data-gebai-css="${url}"]`)) return
  const link = document.createElement("link")
  link.rel = "stylesheet"
  link.href = url
  link.setAttribute("data-gebai-css", url)
  document.head.appendChild(link)
}

let vendorPromise: Promise<XtermVendor> | null = null

/** 加载 xterm 运行时（样式 + 四个 ESM 模块，并行取回）。 */
export function loadXterm(): Promise<XtermVendor> {
  if (vendorPromise) return vendorPromise
  const base = `${basePath()}/vendor/xterm`
  vendorPromise = (async () => {
    loadCss(`${base}/xterm.css`)
    const [core, fit, search, links] = await Promise.all([
      loadModule(`${base}/xterm.mjs`),
      loadModule(`${base}/addon-fit.mjs`),
      loadModule(`${base}/addon-search.mjs`),
      loadModule(`${base}/addon-web-links.mjs`),
    ])
    const Terminal = core.Terminal as XtermVendor["Terminal"] | undefined
    const FitAddon = fit.FitAddon as XtermVendor["FitAddon"] | undefined
    const SearchAddon = search.SearchAddon as XtermVendor["SearchAddon"] | undefined
    const WebLinksAddon = links.WebLinksAddon as XtermVendor["WebLinksAddon"] | undefined
    if (!Terminal || !FitAddon || !SearchAddon || !WebLinksAddon) {
      throw new Error("xterm 资源不完整（缺少 /vendor/xterm/*.mjs：请执行 bun run build-vendor）")
    }
    return { Terminal, FitAddon, SearchAddon, WebLinksAddon }
  })()
  return vendorPromise
}

/* ------------------------------ 终端外观 ------------------------------ */

/** 从主题 CSS 变量取色（背景/前景跟随主题；ANSI 16 色用通用暗色调色板）。 */
function xtermTheme(): Record<string, string> {
  const cs = getComputedStyle(document.documentElement)
  const v = (name: string, fallback: string) => cs.getPropertyValue(name).trim() || fallback
  const fg = v("--text", "#d4d4d4")
  return {
    background: v("--bg-inset", "#181818"),
    foreground: fg,
    cursor: fg,
    cursorAccent: v("--bg-inset", "#181818"),
    selectionBackground: "rgba(120, 160, 255, 0.35)",
    black: "#1e1e1e",
    red: "#f14c4c",
    green: "#23d18b",
    yellow: "#e5c07b",
    blue: "#3b8eea",
    magenta: "#d670d6",
    cyan: "#29b8db",
    white: "#d4d4d4",
    brightBlack: "#6a6a6a",
    brightRed: "#f14c4c",
    brightGreen: "#23d18b",
    brightYellow: "#f5f543",
    brightBlue: "#3b8eea",
    brightMagenta: "#d670d6",
    brightCyan: "#29b8db",
    brightWhite: "#ffffff",
  }
}

/** 终端字体（优先 Windows 自带等宽字体，回退到主题等宽变量与通用 monospace）。 */
function terminalFontFamily(): string {
  const cs = getComputedStyle(document.documentElement)
  const mono = cs.getPropertyValue("--font-mono").trim()
  const stack = "Consolas, 'Cascadia Mono', 'Courier New', monospace"
  return mono && !mono.includes("var(") ? `${stack.slice(0, stack.indexOf(","))}, ${mono}` : stack
}

function readFontSize(): number {
  try {
    const n = Number(localStorage.getItem(FONT_KEY))
    return Number.isFinite(n) && n >= 8 && n <= 28 ? n : 13
  } catch {
    return 13
  }
}

function saveFontSize(n: number): void {
  try {
    localStorage.setItem(FONT_KEY, String(n))
  } catch {
    /* 存储不可用时仅本次生效 */
  }
}

function readFollow(): boolean {
  try {
    return localStorage.getItem(FOLLOW_KEY) !== "0"
  } catch {
    return true
  }
}

function saveFollow(on: boolean): void {
  try {
    localStorage.setItem(FOLLOW_KEY, on ? "1" : "0")
  } catch {
    /* 同上 */
  }
}

/* ------------------------------ WS 客户端 ------------------------------ */

interface Reply {
  ok: boolean
  payload?: Record<string, unknown>
  error?: string
}

/** 终端 WS 客户端：请求按 id 关联应答，推送按消息类型分发。 */
class TermSocket {
  private ws: WebSocket | null = null
  private connecting: Promise<void> | null = null
  private seq = 0
  private pending = new Map<string, (r: Reply) => void>()
  private pushHandlers = new Map<string, Set<(payload: Record<string, unknown>) => void>>()
  private closers = new Set<(reason: string) => void>()
  private closed = false

  constructor(private readonly sessionId: () => string | undefined) {}

  private url(): string {
    const proto = location.protocol === "https:" ? "wss" : "ws"
    return `${proto}://${location.host}${basePath()}/ws`
  }

  on(type: string, cb: (payload: Record<string, unknown>) => void): () => void {
    let set = this.pushHandlers.get(type)
    if (!set) {
      set = new Set()
      this.pushHandlers.set(type, set)
    }
    set.add(cb)
    return () => set.delete(cb)
  }

  onClose(cb: (reason: string) => void): () => void {
    this.closers.add(cb)
    return () => this.closers.delete(cb)
  }

  /** 建立连接（并发调用共享同一次尝试）。 */
  ensureOpen(): Promise<void> {
    if (this.ws?.readyState === WebSocket.OPEN) return Promise.resolve()
    if (this.connecting) return this.connecting
    this.connecting = new Promise<void>((resolve, reject) => {
      let settled = false
      const ws = new WebSocket(this.url())
      const done = (err?: Error) => {
        if (settled) return
        settled = true
        this.connecting = null
        if (err) reject(err)
        else resolve()
      }
      const timer = setTimeout(() => {
        try {
          ws.close()
        } catch {
          /* 忽略 */
        }
        done(new Error("终端通道连接超时"))
      }, 8000)
      ws.onopen = () => {
        clearTimeout(timer)
        this.ws = ws
        // 服务模式：WS 无法带 Header，连接后先认证（服务端按到达顺序串行处理）
        const token = readToken()
        if (token) {
          ws.send(JSON.stringify({ type: "auth.login", payload: { token } }))
        }
        done()
      }
      ws.onmessage = (ev) => this.dispatch(String(ev.data))
      ws.onerror = () => done(new Error("终端通道连接失败"))
      ws.onclose = () => {
        clearTimeout(timer)
        if (this.ws === ws) this.ws = null
        done(new Error("终端通道已断开"))
        for (const cb of this.closers) cb("closed")
      }
    })
    return this.connecting
  }

  private dispatch(raw: string): void {
    let msg: { type?: string; id?: string; ok?: boolean; payload?: Record<string, unknown>; error?: string }
    try {
      msg = JSON.parse(raw)
    } catch {
      return
    }
    if (!msg.type) return
    if (msg.id) {
      const cb = this.pending.get(msg.id)
      if (cb) {
        this.pending.delete(msg.id)
        cb({ ok: msg.ok !== false, payload: msg.payload, error: msg.error })
        return
      }
    }
    const set = this.pushHandlers.get(msg.type)
    if (!set) return
    const payload = msg.payload ?? {}
    for (const cb of set) {
      try {
        cb(payload)
      } catch {
        /* 单个处理器异常不影响其它订阅 */
      }
    }
  }

  /** 发请求并等应答（服务端回执带同一 id）。 */
  async request(type: string, payload: Record<string, unknown> = {}): Promise<Reply> {
    try {
      await this.ensureOpen()
    } catch (err) {
      return { ok: false, error: (err as Error).message }
    }
    const ws = this.ws
    if (!ws || ws.readyState !== WebSocket.OPEN) return { ok: false, error: "终端通道未连接" }
    const id = `r${++this.seq}`
    const session = this.sessionId()
    const body = session ? { ...payload, session } : payload
    return await new Promise<Reply>((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        resolve({ ok: false, error: "终端请求超时" })
      }, 15000)
      this.pending.set(id, (r) => {
        clearTimeout(timer)
        resolve(r)
      })
      try {
        ws.send(JSON.stringify({ type, id, payload: body }))
      } catch (err) {
        clearTimeout(timer)
        this.pending.delete(id)
        resolve({ ok: false, error: (err as Error).message })
      }
    })
  }

  /** 单向发送（输入/尺寸这类高频消息不等应答）。 */
  send(type: string, payload: Record<string, unknown> = {}): void {
    const ws = this.ws
    if (!ws || ws.readyState !== WebSocket.OPEN) return
    try {
      ws.send(JSON.stringify({ type, payload }))
    } catch {
      /* 连接刚断：下一条会重新走 ensureOpen */
    }
  }

  close(): void {
    this.closed = true
    try {
      this.ws?.close()
    } catch {
      /* 忽略 */
    }
    this.ws = null
  }

  get isClosed(): boolean {
    return this.closed
  }
}

function readToken(): string | null {
  try {
    return localStorage.getItem(AUTH_TOKEN_KEY)
  } catch {
    return null
  }
}

/** 服务端 info（能力/Shell 清单）：终端面板打开时探测一次。 */
interface TermInfo {
  enabled: boolean
  reason?: string
  writable: boolean
  shells: Array<{ id: string; name: string; path: string; available: boolean }>
  defaultShell: string
  pty: boolean
  ptyReason?: string
}

async function fetchInfo(hooks: TerminalHooks): Promise<TermInfo | null> {
  const url = new URL(`${basePath()}/api/v1/terminal/info`.replace(/\/{2,}/g, "/"), location.origin)
  const session = hooks.session()
  if (session) url.searchParams.set("session", session)
  const env = hooks.env()
  if (env && Object.keys(env).length) url.searchParams.set("env", JSON.stringify(env))
  try {
    const token = readToken()
    const res = await fetch(url.pathname + url.search, { headers: token ? { Authorization: `Bearer ${token}` } : {} })
    if (!res.ok) return null
    return (await res.json()) as TermInfo
  } catch {
    return null
  }
}

/** 根 id → 绝对路径（跟随当前根用；与资源管理器同一份根清单）。 */
async function fetchRootPath(rootId: string): Promise<string | null> {
  try {
    const token = readToken()
    const res = await fetch(`${basePath()}/api/v1/roots`, { headers: token ? { Authorization: `Bearer ${token}` } : {} })
    if (!res.ok) return null
    const body = (await res.json()) as { roots?: Array<{ id: string; path: string }> }
    return body.roots?.find((r) => r.id === rootId)?.path ?? null
  } catch {
    return null
  }
}
/* ------------------------------ 面板 ------------------------------ */

interface PtyTab {
  /** 服务端会话 id（`p…`）。 */
  id: string
  shellName: string
  /** 标签页外层（一个会话一个，切标签切 hidden）。 */
  view: HTMLElement
  /** xterm 挂载点。 */
  host: HTMLElement
  term: XTerm
  fit: FitAddon
  search: SearchAddon
  tabEl: HTMLElement
  /** shell id（重启时按它重建同一 shell）。 */
  shellId: string
  cwd: string
  alive: boolean
  ro: ResizeObserver
}

/** 终端面板（PTY + xterm）：多标签、复制粘贴、搜索、字号、跟随当前根。 */
export function createPtyTerminal(hooks: TerminalHooks): TerminalPanel {
  let vendor: XtermVendor | null = null
  let info: TermInfo | null = null
  let active = false
  let booting: Promise<void> | null = null
  let followRoot = readFollow()
  let fontSize = readFontSize()
  let activeId: string | null = null
  const tabs: PtyTab[] = []
  const socket = new TermSocket(() => hooks.session())
  const rootPaths = new Map<string, string>()

  /* ---------- 骨架 ---------- */

  const tabbar = h("div", { class: "fw-term-tabs" })
  const cwdName = h("span", { class: "fw-term-cwd-name" })
  const cwdChip = h("span", { class: "fw-term-cwd", hidden: true }, [cwdName])
  const body = h("div", { class: "fw-pty-body" })
  const placeholder = h("div", { class: "fw-term-notice" })
  const searchBar = h("div", { class: "fw-pty-search", hidden: true })
  const searchInput = h("input", { class: "fw-input sm", placeholder: "在终端中查找…", type: "search" })
  const searchStatus = h("span", { class: "fw-pty-search-status" })
  searchBar.append(
    icon("search", 13),
    searchInput,
    searchStatus,
    btn("chevronUp", "上一处（Shift+Enter）", () => findInTerm(-1)),
    btn("chevronDown", "下一处（Enter）", () => findInTerm(1)),
    btn("close", "关闭搜索（Esc）", () => toggleSearch(false)),
  )

  function btn(name: string, title: string, onClick: () => void, cls = ""): HTMLButtonElement {
    const b = h("button", { class: `fw-term-btn ${cls}`.trim(), title })
    b.appendChild(icon(name, 13))
    b.onclick = onClick
    return b
  }

  const newBtn = btn("plus", "新建终端（选择 Shell）", () => pickShell())
  const clearBtn = btn("trash", "清屏（Ctrl+L / 右键菜单）", () => activeTab()?.term.clear())
  const intBtn = btn("minus", "中断当前命令（Ctrl+C）", () => void interruptActive(), "danger")
  const searchBtn = btn("search", "在终端中查找（Ctrl+F）", () => toggleSearch())
  const moreBtn = btn("settings", "终端设置（字号 / 跟随当前根 / 重启）", () => openMore())
  const closeBtn = btn("close", "关闭工具窗", () => hooks.close())
  const titlebar = h("div", { class: "fw-term-titlebar" }, [
    h("span", { class: "fw-term-title" }, [icon("terminal", 13), h("span", { text: "终端" })]),
    tabbar,
    h("div", { class: "fw-term-grow" }),
    cwdChip,
    h("div", { class: "fw-term-actions" }, [newBtn, clearBtn, intBtn, searchBtn, moreBtn, closeBtn]),
  ])
  const el = h("div", { class: "fw-term-panel" }, [titlebar, body])

  /* ---------- 订阅推送 ---------- */

  socket.on("term.out", (p) => {
    const tab = tabs.find((t) => t.id === p.id)
    if (!tab) return
    const data = typeof p.data === "string" ? p.data : ""
    if (data) tab.term.write(data)
  })
  socket.on("term.ready", (p) => {
    const tab = tabs.find((t) => t.id === p.id)
    if (tab) {
      tab.alive = true
      paintTabs()
    }
  })
  socket.on("term.exit", (p) => {
    const tab = tabs.find((t) => t.id === p.id)
    if (!tab) return
    tab.alive = false
    paintTabs()
  })
  socket.on("term.error", (p) => {
    const msg = typeof p.message === "string" ? p.message : "终端驱动错误"
    toast(msg, "error", 6000)
  })
  socket.onClose(() => {
    // 断线：会话仍在服务端跑，标签保留；下一次输入会经 ensureOpen 自动重连
    paintTabs()
  })

  /* ---------- 启动与占位 ---------- */

  function paintNotice(title: string, hint: string): void {
    clear(placeholder)
    placeholder.className = "fw-term-notice"
    placeholder.append(
      h("div", { class: "fw-term-notice-title" }, [icon("warning", 14), h("span", { text: title })]),
      h("div", { class: "fw-term-notice-hint", text: hint }),
    )
    if (!placeholder.isConnected) body.appendChild(placeholder)
  }

  function dropNotice(): void {
    if (placeholder.isConnected) placeholder.remove()
  }

  function ensureBooted(): Promise<void> {
    if (booting) return booting
    booting = (async () => {
      info = await fetchInfo(hooks)
      if (!info) {
        paintNotice("终端服务不可用", "无法访问 /api/v1/terminal/info：请确认服务端已启用文件工作台与终端能力。")
        return
      }
      if (!info.enabled) {
        paintNotice("终端不可用", info.reason || "服务端未启用终端能力。")
        return
      }
      if (!info.pty) {
        paintNotice("当前环境不支持 PTY 终端", info.ptyReason || "服务端未提供 PTY 驱动（降级为管道式终端）。")
        return
      }
      try {
        vendor = await loadXterm()
      } catch (err) {
        paintNotice("终端内核加载失败", (err as Error).message)
        return
      }
      dropNotice()
    })()
    return booting
  }

  /* ---------- 标签管理 ---------- */

  function activeTab(): PtyTab | null {
    return tabs.find((t) => t.id === activeId) ?? null
  }

  function paintTabs(): void {
    clear(tabbar)
    for (const t of tabs) {
      const closeTabBtn = h("button", { class: "fw-term-tab-close", title: "关闭该终端" }, [icon("close", 11)])
      closeTabBtn.onclick = (e) => {
        e.stopPropagation()
        void closeTab(t.id)
      }
      const cls = `fw-term-tab${t.id === activeId ? " active" : ""}${t.alive ? "" : " dead"}`
      const tabEl = h("div", { class: cls, title: `${t.shellName} · ${t.cwd || hooks.root()}` }, [
        h("span", { class: "fw-term-dot" }),
        h("span", { class: "fw-term-tab-name", text: t.shellName }),
        closeTabBtn,
      ])
      tabEl.onclick = () => selectTab(t.id)
      t.tabEl = tabEl
      tabbar.appendChild(tabEl)
    }
    syncCwdChip()
  }

  function syncCwdChip(): void {
    const t = activeTab()
    if (!t) {
      cwdChip.hidden = true
      return
    }
    const tail = pathTail(t.cwd || hooks.root())
    cwdChip.hidden = false
    cwdName.textContent = tail
    cwdChip.title = t.cwd || hooks.root()
  }

  function selectTab(id: string): void {
    const t = tabs.find((x) => x.id === id)
    if (!t) return
    activeId = id
    for (const x of tabs) x.view.hidden = x.id !== id
    paintTabs()
    // 切到可见后再 fit（hidden 元素量不出尺寸）
    requestAnimationFrame(() => {
      fitTab(t)
      t.term.focus()
    })
  }

  function fitTab(t: PtyTab): void {
    if (t.view.hidden) return
    try {
      t.fit.fit()
    } catch {
      /* 容器尺寸为 0（面板收起）：跳过 */
    }
  }

  function fitActive(): void {
    const t = activeTab()
    if (t) fitTab(t)
  }

  async function createTab(shell?: string): Promise<void> {
    if (opening) return opening.then(() => undefined)
    opening = openTab(shell).finally(() => {
      opening = null
    })
    await opening
  }

  /** 开一条终端：先建 xterm 视图（立刻可见），再向服务端申请 PTY 会话。 */
  async function openTab(shell?: string): Promise<PtyTab | null> {
    try {
      await ensureBooted()
    } catch {
      /* 上面已给出占位说明 */
    }
    if (!vendor || !info?.pty) return null
    const t = makeXtermTab()
    tabs.push(t)
    activeId = t.id
    body.appendChild(t.view)
    dropNotice()
    selectTab(t.id)
    const reply = await socket.request("term.open", {
      root: hooks.root(),
      cwd: hooks.cwd(),
      shell: shell ?? "",
      cols: t.term.cols,
      rows: t.term.rows,
      env: hooks.env(),
    })
    if (!reply.ok) {
      // 建会话失败：撤掉这个空标签，避免留下一个永远连不上的终端
      t.term.write(`\r\n\x1b[31m无法启动终端：${reply.error ?? "未知错误"}\x1b[0m\r\n`)
      t.alive = false
      socket.send("term.close", { id: t.id })
      paintTabs()
      toast(reply.error ?? "无法启动终端", "error", 6000)
      return t
    }
    const session = reply.payload?.session as { id?: string; shellName?: string; cwd?: string } | undefined
    const realId = String(session?.id ?? "")
    if (realId) {
      // 会话 id 由服务端发放（本地占位 id 仅用于创建期）：同时迁移 activeId，
      // 否则 activeTab() 按 id 找不到活动标签，快捷键/中断/跟随根会静默地不生效。
      const oldId = t.id
      t.id = realId
      if (activeId === oldId) activeId = realId
    }
    if (session?.shellName) t.shellName = String(session.shellName)
    if (session?.cwd) t.cwd = String(session.cwd)
    if (shell) t.shellId = shell
    t.alive = true
    paintTabs()
    return t
  }

  async function closeTab(id: string): Promise<void> {
    const idx = tabs.findIndex((t) => t.id === id)
    if (idx < 0) return
    const t = tabs[idx]
    // 会话 id 未知（创建未应答就关闭）时服务端幂等返回成功
    socket.send("term.close", { id: t.id })
    t.ro.disconnect()
    t.term.dispose()
    t.view.remove()
    tabs.splice(idx, 1)
    if (activeId === id) {
      const next = tabs[Math.min(idx, tabs.length - 1)]
      activeId = next?.id ?? null
      if (next) selectTab(next.id)
      else {
        activeId = null
        paintTabs()
        paintNotice("没有打开的终端", "点「＋」新建一个终端会话。")
      }
    } else paintTabs()
  }

  /** 本地占位 id（真正 id 由服务端给出，应答后接管到 tab.id）。 */
  let localSeq = 0
  /** 正在创建中的终端：激活与快捷键可能同时触发，串行化避免一次点击开出两个会话。 */
  let opening: Promise<PtyTab | null> | null = null

  function makeXtermTab(): PtyTab {
    const localId = `tmp${++localSeq}`
    const v = vendor!
    const host = h("div", { class: "fw-pty-term" })
    const view = h("div", { class: "fw-pty-view", hidden: true }, [host])
    const term = new v.Terminal({
      fontFamily: terminalFontFamily(),
      fontSize,
      lineHeight: 1.25,
      cursorBlink: true,
      cursorStyle: "bar",
      scrollback: 8000,
      allowProposedApi: true,
      theme: xtermTheme(),
      // 让 xterm 按 Windows ConPTY 的语义处理换行/光标（包装行、退格行为）
      windowsPty: { backend: "conpty" },
    })
    const fit = new v.FitAddon()
    const search = new v.SearchAddon()
    term.loadAddon(fit)
    term.loadAddon(search)
    term.loadAddon(new v.WebLinksAddon())
    term.open(host)
    term.onData((data) => {
      const id = currentId()
      if (!id) return
      // 连接可能因空闲被回收/断线：发送前确保通道已就绪（已连接时立即返回）
      void socket
        .ensureOpen()
        .then(() => socket.send("term.input", { id, data }))
        .catch(() => {})
    })
    term.onResize(({ cols, rows }) => {
      const id = currentId()
      if (!id) return
      void socket
        .ensureOpen()
        .then(() => socket.send("term.resize", { id, cols, rows }))
        .catch(() => {})
    })
    term.attachCustomKeyEventHandler((e) => onTermKey(e))
    term.onSelectionChange(() => {
      /* 选中变化：无需处理（复制走快捷键/右键菜单） */
    })
    host.oncontextmenu = (e) => {
      e.preventDefault()
      openContextMenu(e.clientX, e.clientY)
    }
    const t: PtyTab = {
      id: localId,
      shellId: "",
      shellName: "终端",
      view,
      host,
      term,
      fit,
      search,
      tabEl: document.createElement("div"),
      cwd: "",
      alive: false,
      ro: new ResizeObserver(() => fitTab(t)),
    }
    t.ro.observe(host)
    // currentId 在闭包里用 activeId 对应 tab 的 id：xterm 回调不携带 tab 引用
    const currentId = () => tabs.find((x) => x.term === term)?.id ?? null
    return t
  }

  /**
   * 面板级快捷键（在面板根元素上以**捕获阶段**监听）。
   *
   * 为何不只用 xterm 的 `attachCustomKeyEventHandler`：该回调在实测中未拦下组合键
   * （Ctrl+F 未开搜索、Ctrl+Shift+C 未复制），而面板级捕获监听是 DOM 语义、行为确定，
   * 且能在 xterm 之前阻止事件（`stopPropagation`），效果等同于自定义键处理器。
   */
  function onPanelKey(e: KeyboardEvent): void {
    if (e.type !== "keydown") return
    const ctrl = e.ctrlKey || e.metaKey
    if (!ctrl) return
    const key = e.key.toLowerCase()
    const take = () => {
      e.preventDefault()
      e.stopPropagation()
    }
    if (e.shiftKey && key === "c") {
      take()
      void copySelection()
      return
    }
    if (e.shiftKey && key === "v") {
      take()
      void pasteClipboard()
      return
    }
    if (key === "f") {
      take()
      toggleSearch(true)
      return
    }
    if (key === "c" && !e.shiftKey) {
      // Ctrl+C：有选区则复制，否则中断当前命令（ConPTY 不认 ETX，见服务端 interrupt）
      take()
      const t = activeTab()
      if (t?.term.getSelection()) void copySelection()
      else void interruptActive()
      return
    }
    if (key === "=" || key === "+") {
      take()
      applyFontSize(fontSize + 1)
      return
    }
    if (key === "-") {
      take()
      applyFontSize(fontSize - 1)
      return
    }
    if (key === "0") {
      take()
      applyFontSize(13)
    }
  }

  el.addEventListener("keydown", onPanelKey, true)

  /** 中断当前命令（终止进程树并以原目录重建 shell）。连按节流：避免一次紧张操作把 shell 重建多次。 */
  let lastInterrupt = 0
  async function interruptActive(): Promise<void> {
    const t = activeTab()
    if (!t) return
    if (!t.alive) {
      toast("终端进程已结束，请新建一个终端", "warn")
      return
    }
    const now = Date.now()
    if (now - lastInterrupt < 800) return
    lastInterrupt = now
    const reply = await socket.request("term.interrupt", { id: t.id })
    if (!reply.ok) toast(reply.error ?? "中断失败", "error")
  }

  /* ---------- 交互：快捷键 / 菜单 / 搜索 ---------- */

  /** xterm 键盘预处理：返回 false 表示不交给终端（由面板处理）。 */
  function onTermKey(e: KeyboardEvent): boolean {
    const ctrl = e.ctrlKey || e.metaKey
    if (e.type !== "keydown") return true
    if (ctrl && e.shiftKey && e.key.toLowerCase() === "c") {
      void copySelection()
      return false
    }
    if (ctrl && e.shiftKey && e.key.toLowerCase() === "v") {
      void pasteClipboard()
      return false
    }
    if (ctrl && !e.shiftKey && e.key.toLowerCase() === "f") {
      toggleSearch(true)
      return false
    }
    if (ctrl && e.shiftKey && e.key.toLowerCase() === "f") {
      toggleSearch(true)
      return false
    }
    if (ctrl && (e.key === "=" || e.key === "+")) {
      applyFontSize(fontSize + 1)
      return false
    }
    if (ctrl && e.key === "-") {
      applyFontSize(fontSize - 1)
      return false
    }
    if (ctrl && e.key === "0") {
      applyFontSize(13)
      return false
    }
    if (e.key === "Escape" && !searchBar.hidden) {
      toggleSearch(false)
      return false
    }
    return true
  }

  async function copySelection(): Promise<void> {
    const t = activeTab()
    const sel = t?.term.getSelection() ?? ""
    if (!sel) return
    try {
      await navigator.clipboard.writeText(sel)
      toast("已复制终端选区", "success")
    } catch {
      toast("复制失败：剪贴板不可用", "error")
    }
  }

  async function pasteClipboard(): Promise<void> {
    const t = activeTab()
    if (!t || !t.alive) return
    try {
      const text = await navigator.clipboard.readText()
      if (text) t.term.paste(text)
    } catch {
      toast("粘贴失败：请允许剪贴板访问，或用 Ctrl+V", "error")
    }
  }

  function openContextMenu(x: number, y: number): void {
    const t = activeTab()
    const hasSel = !!t?.term.getSelection()
    showMenu(x, y, [
      { label: "复制", icon: "copy", disabled: !hasSel, onClick: () => void copySelection() },
      { label: "粘贴", icon: "upload", disabled: !t?.alive, onClick: () => void pasteClipboard() },
      { label: "全选", icon: "check", onClick: () => t?.term.selectAll() },
      { separator: true },
      { label: "清屏", icon: "trash", onClick: () => t?.term.clear() },
      { label: "查找…", icon: "search", onClick: () => toggleSearch(true) },
      { separator: true },
      { label: "新建终端", icon: "plus", onClick: () => void createTab() },
      { label: "重启该终端", icon: "refresh", disabled: !t, onClick: () => void restartTab(t!) },
    ])
  }

  function toggleSearch(open?: boolean): void {
    const next = open ?? searchBar.hidden
    searchBar.hidden = !next
    if (next) {
      const t = activeTab()
      if (t) t.view.appendChild(searchBar)
      searchInput.focus()
      searchInput.select()
    } else {
      activeTab()?.search.clearDecorations()
      searchInput.value = ""
      searchStatus.textContent = ""
      activeTab()?.term.focus()
    }
  }

  let searchSeq = 0
  function findInTerm(direction: 1 | -1): void {
    const t = activeTab()
    if (!t) return
    const q = searchInput.value.trim()
    if (!q) return
    const seq = ++searchSeq
    const found =
      direction === 1
        ? t.search.findNext(q, { incremental: false, decorations: { matchOverviewRuler: "#3b8eea", activeMatchColorOverviewRuler: "#f5f543" } })
        : t.search.findPrevious(q, { decorations: { matchOverviewRuler: "#3b8eea", activeMatchColorOverviewRuler: "#f5f543" } })
    if (seq === searchSeq) searchStatus.textContent = found ? "" : "无匹配"
  }

  searchInput.oninput = () => {
    const t = activeTab()
    if (!t) return
    const q = searchInput.value
    if (!q) {
      t.search.clearDecorations()
      searchStatus.textContent = ""
      return
    }
    t.search.findNext(q, { incremental: true })
  }
  searchInput.onkeydown = (e) => {
    if (e.key === "Enter") {
      e.preventDefault()
      findInTerm(e.shiftKey ? -1 : 1)
    } else if (e.key === "Escape") {
      e.preventDefault()
      toggleSearch(false)
    }
  }

  /* ---------- 设置与窗口尺寸 ---------- */

  function applyFontSize(next: number): void {
    fontSize = Math.max(8, Math.min(28, next))
    saveFontSize(fontSize)
    for (const t of tabs) {
      t.term.options.fontSize = fontSize
      fitTab(t)
    }
  }

  function openMore(): void {
    const r = moreBtn.getBoundingClientRect()
    showMenu(r.left, r.bottom + 4, [
      { label: `字号 ${fontSize}（增大）`, icon: "zoomIn", onClick: () => applyFontSize(fontSize + 1) },
      { label: `字号 ${fontSize}（减小）`, icon: "zoomOut", onClick: () => applyFontSize(fontSize - 1) },
      { label: "重置字号", icon: "refresh", onClick: () => applyFontSize(13) },
      { separator: true },
      { label: `${followRoot ? "✓ " : ""}跟随当前根`, icon: "folder", onClick: () => toggleFollow() },
      { label: "重启当前终端", icon: "refresh", onClick: () => void restartTab(activeTab()) },
    ])
  }

  function toggleFollow(): void {
    followRoot = !followRoot
    saveFollow(followRoot)
    if (followRoot) void applyFollowRoot()
  }

  /** 切换工作台根：跟随开启时在当前终端里 cd 过去。 */
  async function applyFollowRoot(): Promise<void> {
    const t = activeTab()
    if (!t || !t.alive) return
    const rootId = hooks.root()
    let abs = rootPaths.get(rootId)
    if (abs === undefined) {
      const hit = await fetchRootPath(rootId)
      if (!hit) return
      rootPaths.set(rootId, hit)
      abs = hit
    }
    if (samePath(abs, t.cwd)) return
    socket.send("term.input", { id: t.id, data: `cd "${abs.replace(/"/g, '\\"')}"\r` })
  }

  async function restartTab(t: PtyTab | null): Promise<void> {
    if (!t) return
    const shell = t.shellId
    await closeTab(t.id)
    if (active || tabs.length) await createTab(shell)
  }

  function pickShell(): void {
    void (async () => {
      await ensureBooted()
      const shells = (info?.shells ?? []).filter((s) => s.available)
      if (!shells.length) {
        toast("本机未检测到可用的 Shell", "error")
        return
      }
      if (shells.length === 1) {
        void createTab(shells[0]!.id)
        return
      }
      dropdown(newBtn, shells.map((s) => ({ label: s.name, icon: "terminal", onClick: () => void createTab(s.id) })))
    })()
  }

  /* ---------- 装配 ---------- */

  paintTabs()
  paintNotice("正在准备终端…", "首次使用会加载终端内核（xterm.js）。")

  return {
    el,
    hasSession: () => tabs.length > 0,
    activate() {
      active = true
      void ensureBooted().then(() => {
        if (!active) return
        if (info?.pty && !tabs.length && !opening) {
          void createTab()
          return
        }
        fitActive()
        activeTab()?.term.focus()
      })
    },
    deactivate() {
      active = false
    },
    onRootChanged() {
      if (followRoot) void applyFollowRoot()
    },
  }
}
