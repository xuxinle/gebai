/**
 * 文件工作台 · 终端面板（底部工具窗，与 Git 面板互斥停靠）。
 *
 * 命令回显为什么在前端：服务端不引入 PTY（项目零原生依赖），命令在**管道模式**下执行、
 * 没有 tty 回显，所以「提示符 + 命令」由前端在提交瞬间本地回显；命令结束由服务端注入哨兵行，
 * 解析成 `exits[{token,code,cwd}]` 随 `read` 返回，前端据此补一行退出码（非 0 才提示）
 * 并用返回的 cwd 刷新标题栏当前目录。
 *
 * 会话模型：每条会话一份输出缓冲（`terminal-core.ts`）+ 一份 DOM 视图，切标签只切 `hidden`
 * ——各自保留滚动位置与已渲染行，**不随工作台切根销毁**（切根只影响「跟随当前根」是否补一条 cd）。
 * 网络模型：REST + 增量轮询（250ms，连续静默降频到 1000ms），与工作台其余部分一致，不新开 WS。
 *
 * ANSI / `\r` / `\b` / 缓冲 / 历史的规则都在 core 里（无 DOM 可单测），本文件只做 DOM 与网络。
 */
import { clear, dropdown, h, icon, toast } from "./ui"
import { TERM_HISTORY_MAX, TermBuffer, pathTail, pushHistory, samePath, type AnsiColor, type TermLine } from "./terminal-core"
import "../css/terminal.css"

export interface TerminalHooks {
  /** 当前工作台根 id（如 `proj:gebai`） */
  root: () => string
  /** 根内相对 cwd 初值（`""` = 根目录） */
  cwd: () => string
  /** 会话语境（服务端 env 解析用） */
  session: () => string | undefined
  /** 浏览器本地 env（与 FsApi 一致） */
  env: () => Record<string, string>
  /** 收起工具窗（标题栏关闭按钮） */
  close: () => void
}

export interface TerminalPanel {
  el: HTMLElement
  /** 工具窗展开：首次探测能力、建立轮询、恢复可见性监听 */
  activate: () => void
  /** 工具窗收起 / 页面隐藏：暂停轮询（不关闭服务端会话） */
  deactivate: () => void
  /** 工作台切换根：跟随开启时自动 cd 到新根 */
  onRootChanged: () => void
  hasSession: () => boolean
}

/* ------------------------------ 服务端客户端 ------------------------------ */

/**
 * 最小客户端（函数形式，不扩 `api.ts`）：终端是独立能力域，且 `api.ts` 归 FsApi 的
 * 文件/Git 端点。约定与 FsApi 完全一致——`session`/`env` 透传进 query（服务端据此解析
 * 预置项目与会话环境）、非 2xx 抛错（带 `code`）、失败只 toast 不抛未捕获异常；
 * 令牌取本地持久化的那一份（`gebai.auth.token`，服务模式登录后写入，与聊天页共享）。
 */
const AUTH_TOKEN_KEY = "gebai.auth.token"
/** 输出轮询：活跃 250ms；连续 3 次无新输出后降频到 1000ms（长命令不必挤 4 倍请求） */
const POLL_FAST_MS = 250
const POLL_SLOW_MS = 1000
const IDLE_TICKS = 3
/** 明命令历史（跨会话保留，与 shell 的 history 同一习惯；上限 100 条） */
const HISTORY_KEY = "gebai.ui.termHistory"
/** 跟随当前根开关（默认开启） */
const FOLLOW_KEY = "gebai.ui.termFollowRoot"

interface TermShell {
  id: string
  name: string
  path: string
  available: boolean
}

interface TermInfoResp {
  enabled: boolean
  reason?: string
  sandboxed: boolean
  writable: boolean
  shells: TermShell[]
  defaultShell: string
  maxSessions: number
  idleMs: number
}

interface TermCreatedResp {
  id: string
  shell: string
  shellName: string
  cwd: string
  root: string
  cursor: number
  output: string
  startedAt: number
}

interface TermExit {
  token: string
  code: number
  cwd?: string
}

interface TermReadResp {
  cursor: number
  text: string
  exits: TermExit[]
  alive: boolean
}

class TermError extends Error {
  constructor(
    public status: number,
    message: string,
    public code?: string,
  ) {
    super(message)
    this.name = "TermError"
  }
}

function basePath(): string {
  return (import.meta.env.BASE_URL || "/").replace(/\/$/, "")
}

/** 认证头：服务模式下 localStorage 有令牌（与聊天页同一份），本地模式无令牌则不加。 */
function authHeaders(): Record<string, string> {
  try {
    const t = localStorage.getItem(AUTH_TOKEN_KEY)
    return t ? { Authorization: `Bearer ${t}` } : {}
  } catch {
    return {}
  }
}

async function request<T>(
  hooks: TerminalHooks,
  method: "GET" | "POST",
  endpoint: string,
  opts: { params?: Record<string, string | number | undefined>; body?: unknown } = {},
): Promise<T> {
  const url = new URL(`${basePath()}${endpoint}`.replace(/\/{2,}/g, "/"), location.origin)
  const session = hooks.session()
  if (session) url.searchParams.set("session", session)
  const env = hooks.env()
  if (env && Object.keys(env).length) url.searchParams.set("env", JSON.stringify(env))
  for (const [k, v] of Object.entries(opts.params ?? {})) if (v !== undefined) url.searchParams.set(k, String(v))
  const init: RequestInit = { method, headers: authHeaders() }
  if (opts.body !== undefined) {
    init.body = JSON.stringify(opts.body)
    ;(init.headers as Record<string, string>)["Content-Type"] = "application/json"
  }
  const res = await fetch(url.pathname + url.search, init)
  const text = await res.text()
  let parsed: unknown = null
  try {
    parsed = text ? JSON.parse(text) : null
  } catch {
    parsed = null
  }
  if (!res.ok) {
    const e = parsed as { error?: string; code?: string } | null
    throw new TermError(res.status, e?.error ?? `终端请求失败（${res.status}）`, e?.code)
  }
  return parsed as T
}

function errMsg(err: unknown): string {
  return err instanceof Error ? err.message : String(err)
}

/* ------------------------------ 局部持久化 ------------------------------ */

function readHistory(): string[] {
  try {
    const arr = JSON.parse(localStorage.getItem(HISTORY_KEY) ?? "[]") as unknown
    return Array.isArray(arr) ? arr.filter((x): x is string => typeof x === "string").slice(-TERM_HISTORY_MAX) : []
  } catch {
    return []
  }
}

function saveHistory(list: string[]): void {
  try {
    localStorage.setItem(HISTORY_KEY, JSON.stringify(list))
  } catch {
    /* 存储不可用（隐私模式/配额满）：历史仅本次会话有效 */
  }
}

function readFollow(): boolean {
  try {
    return localStorage.getItem(FOLLOW_KEY) !== "0" // 默认开启：不存或非 "0" 都视为开启
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

/* ------------------------------ 渲染映射 ------------------------------ */

/** SGR 语义色 → CSS 类（具体色值在 terminal.css，核心模块不关心颜色）。 */
const COLOR_CLASS: Record<AnsiColor, string> = {
  black: "fw-term-fg-black",
  red: "fw-term-fg-red",
  green: "fw-term-fg-green",
  yellow: "fw-term-fg-yellow",
  blue: "fw-term-fg-blue",
  magenta: "fw-term-fg-magenta",
  cyan: "fw-term-fg-cyan",
  white: "fw-term-fg-white",
  brightBlack: "fw-term-fg-brightBlack",
  brightRed: "fw-term-fg-brightRed",
  brightGreen: "fw-term-fg-brightGreen",
  brightYellow: "fw-term-fg-brightYellow",
  brightBlue: "fw-term-fg-brightBlue",
  brightMagenta: "fw-term-fg-brightMagenta",
  brightCyan: "fw-term-fg-brightCyan",
  brightWhite: "fw-term-fg-brightWhite",
}

/** 轮询计时器句柄（浏览器与 Bun 下 setTimeout 的返回类型不同，统一取 ReturnType）。 */
type TimerHandle = ReturnType<typeof setTimeout>

/** 一条会话的全部运行态与 DOM。 */
interface Session {
  id: string
  shellName: string
  /** 服务端返回的当前目录（原样保存：可能绝对、可能根内相对；显示只取尾段） */
  cwd: string
  cursor: number
  buffer: TermBuffer
  alive: boolean
  /** 已提交但还没等到退出码的命令数（标签忙碌指示 / 中断按钮可用性） */
  pending: number
  /** 已消费的哨兵 token（服务端可能重发同一批 exits） */
  seen: Set<string>
  view: HTMLElement
  out: HTMLElement
  input: HTMLInputElement
  prompt: HTMLElement
  jump: HTMLButtonElement
  /** 已渲染的行节点（与缓冲下标对齐，缓冲裁行时同步裁它） */
  lineNodes: HTMLElement[]
  /** 视图首行对应的缓冲下标 */
  renderedFrom: number
  atBottom: boolean
  /** 读取失败已提示过（避免每 250ms 刷一条） */
  warned: boolean
  /** 输入法组合中（中文候选态下 Enter 属于候选确认，不能当提交） */
  composing: boolean
}

/* ------------------------------ 面板 ------------------------------ */

export function createTerminalPanel(hooks: TerminalHooks): TerminalPanel {
  const sessions: Session[] = []
  let activeId: string | null = null
  let info: TermInfoResp | null = null
  let probing: Promise<TermInfoResp | null> | null = null
  let probeFailed = false
  /** 工具窗是否展开（deactivate 后停止一切轮询） */
  let active = false
  /** 页面是否不可见（tick 在途时靠它判断是否该继续排期） */
  let paused = false
  let timer: TimerHandle | 0 = 0
  let idleTicks = 0
  let followRoot = readFollow()
  let history = readHistory()
  let histIdx = history.length
  let histDraft = ""
  /** 根 id → 绝对路径：终端契约里 root 是 id、cwd 是根内相对，跟随根的 cd 目标只有根注册表有 */
  const rootPaths = new Map<string, string>()

  /* ---------- 骨架 ---------- */

  function actionBtn(name: string, title: string, onClick: () => void, cls = ""): HTMLButtonElement {
    const b = h("button", { class: `fw-term-btn ${cls}`.trim(), title })
    b.appendChild(icon(name, 13))
    b.onclick = onClick
    return b
  }

  const el = h("div", { class: "fw-term-panel" })
  const tabbar = h("div", { class: "fw-term-tabs" })
  const cwdName = h("span", { class: "fw-term-cwd-name" })
  const cwdChip = h("span", { class: "fw-term-cwd", hidden: true }, [cwdName])
  const newBtn = actionBtn("plus", "新建终端", () => void pickShell(newBtn))
  const clearBtn = actionBtn("trash", "清屏（Ctrl+L）", () => {
    const s = activeSession()
    if (s) clearScreen(s)
  })
  const intBtn = actionBtn(
    "minus",
    "中断当前命令（Ctrl+C）",
    () => {
      const s = activeSession()
      if (s) void interrupt(s)
    },
    "danger",
  )
  const followBtn = h("button", { class: "fw-term-btn" })
  followBtn.appendChild(icon("folder", 13))
  followBtn.onclick = () => {
    followRoot = !followRoot
    saveFollow(followRoot)
    syncFollowBtn()
    if (followRoot) void applyFollowRoot() // 刚打开就对齐一次：当前根可能已经不是会话所在根
  }
  const closeBtn = actionBtn("close", "关闭工具窗", () => hooks.close())
  const titlebar = h("div", { class: "fw-term-titlebar" }, [
    h("span", { class: "fw-term-title" }, [icon("terminal", 13), h("span", { text: "终端" })]),
    tabbar,
    h("div", { class: "fw-term-grow" }),
    cwdChip,
    h("div", { class: "fw-term-actions" }, [newBtn, clearBtn, intBtn, followBtn, closeBtn]),
  ])
  const body = h("div", { class: "fw-term-body" })
  el.append(titlebar, body)

  const placeholder = h("div", { class: "fw-term-empty" })
  const newBigBtn = h("button", { class: "fw-term-btn big" }, [icon("plus", 13), h("span", { text: "新建终端" })])
  newBigBtn.onclick = () => void pickShell(newBigBtn)

  function syncFollowBtn(): void {
    followBtn.classList.toggle("on", followRoot)
    followBtn.title = followRoot ? "跟随当前根：已开启（点击关闭）" : "跟随当前根：已关闭（点击开启）"
  }

  /* ---------- 状态与渲染 ---------- */

  function activeSession(): Session | null {
    return sessions.find((s) => s.id === activeId) ?? null
  }

  function renderTabs(): void {
    clear(tabbar)
    for (const s of sessions) {
      const closeTab = h("button", { class: "fw-term-tab-close", title: "关闭该终端" }, [icon("close", 11)])
      closeTab.onclick = (e) => {
        e.stopPropagation()
        void closeSession(s)
      }
      const cls = `fw-term-tab${s.id === activeId ? " active" : ""}${s.pending > 0 ? " busy" : ""}${s.alive ? "" : " dead"}`
      const tab = h("div", { class: cls, title: `${s.shellName} · ${s.cwd || hooks.root()}` }, [
        h("span", { class: "fw-term-dot" }),
        h("span", { class: "fw-term-tab-name", text: s.shellName }),
        closeTab,
      ])
      tab.onclick = () => selectSession(s)
      tabbar.appendChild(tab)
    }
  }

  /** 占位（加载中 / 不可用 / 空态）：三态共用一块元素，避免在与会话视图的显隐之间反复增删。 */
  function paintPlaceholder(): void {
    clear(placeholder)
    if (probeFailed && !info) {
      placeholder.className = "fw-term-notice"
      placeholder.append(
        h("div", { class: "fw-term-notice-title" }, [icon("warning", 14), h("span", { text: "终端服务不可用" })]),
        h("div", { class: "fw-term-notice-hint", text: "无法访问 /api/v1/terminal/info，请确认服务端已启用终端能力。" }),
      )
      return
    }
    if (!info) {
      placeholder.className = "fw-term-notice"
      placeholder.appendChild(h("div", { class: "fw-term-notice-hint", text: "正在探测终端能力…" }))
      return
    }
    if (!info.enabled) {
      placeholder.className = "fw-term-notice"
      placeholder.append(
        h("div", { class: "fw-term-notice-title" }, [icon("warning", 14), h("span", { text: "终端不可用" })]),
        h("div", { class: "fw-term-notice-hint", text: info.reason || "服务端未启用终端能力。" }),
      )
      return
    }
    placeholder.className = "fw-term-empty"
    placeholder.append(h("div", { text: "没有打开的终端" }), newBigBtn)
  }

  function syncPlaceholder(): void {
    if (sessions.length) {
      if (placeholder.isConnected) placeholder.remove()
      return
    }
    paintPlaceholder()
    if (!placeholder.isConnected) body.appendChild(placeholder)
  }

  function paintLine(node: HTMLElement, line: TermLine): void {
    clear(node)
    for (const run of line.runs) {
      if (!run.text) continue
      const cls = `${run.color ? COLOR_CLASS[run.color] : ""}${run.bold ? " fw-term-bold" : ""}`.trim()
      if (!cls) node.appendChild(document.createTextNode(run.text))
      else node.appendChild(h("span", { class: cls, text: run.text }))
    }
    if (line.truncated) node.appendChild(h("span", { class: "fw-term-trunc", text: "…" }))
  }

  /** 全量重画（清屏 / 缓冲裁量异常时）：同时丢掉夹在行之间的信息行。 */
  function rebuildOutput(s: Session): void {
    clear(s.out)
    s.lineNodes = []
    s.renderedFrom = s.buffer.trimmedLines
    for (const line of s.buffer.lines()) {
      const node = h("div", { class: "fw-term-line" })
      paintLine(node, line)
      s.lineNodes.push(node)
      s.out.appendChild(node)
    }
    scrollToBottom(s)
  }

  /**
   * 增量渲染：缓冲裁掉的行同步从 DOM 顶部摘掉，其余只补缺失行，并重画「正在写的那一行」
   * （`\r` 覆盖与追加都只改最后一行）。信息行（退出码等）直接 append 到输出区、不参与行对齐。
   */
  function renderOutput(s: Session): void {
    const lines = s.buffer.lines()
    const base = s.buffer.trimmedLines
    if (base < s.renderedFrom || base - s.renderedFrom > s.lineNodes.length) {
      rebuildOutput(s)
      return
    }
    const drop = base - s.renderedFrom
    if (drop > 0) {
      for (let i = 0; i < drop; i++) s.lineNodes[i]?.remove()
      s.lineNodes.splice(0, drop)
      s.renderedFrom = base
    }
    for (let i = s.lineNodes.length; i < lines.length; i++) {
      const node = h("div", { class: "fw-term-line" })
      paintLine(node, lines[i]!)
      s.lineNodes.push(node)
      s.out.appendChild(node)
    }
    const tail = s.lineNodes[s.lineNodes.length - 1]
    if (tail) paintLine(tail, lines[lines.length - 1]!)
    if (s.atBottom) s.out.scrollTop = s.out.scrollHeight
  }

  function appendMeta(s: Session, text: string, warn = false): void {
    // 先结行再插信息行：让信息行落在「上一段输出」之后，而不是与正在写的那一行抢占；
    // 已经在行首（输出自带换行）就不再多补一个空行
    const tail = s.buffer.lines()
    if (tail[tail.length - 1]!.text) s.buffer.write("\n")
    s.out.appendChild(h("div", { class: `fw-term-meta${warn ? " warn" : ""}`, text }))
    renderOutput(s) // 新起一行补在信息行之后，后续输出继续往下排
    if (s.atBottom) s.out.scrollTop = s.out.scrollHeight
  }

  function writeLocal(s: Session, text: string): void {
    s.buffer.write(text)
    renderOutput(s)
  }

  function promptText(s: Session): string {
    return `${pathTail(s.cwd || hooks.root())} $ `
  }

  function updatePrompt(s: Session): void {
    s.prompt.textContent = promptText(s)
    s.prompt.title = s.cwd || ""
  }

  function updateCwdChip(): void {
    const s = activeSession()
    if (!s) {
      cwdChip.hidden = true
      intBtn.disabled = true
      clearBtn.disabled = true
      return
    }
    cwdChip.hidden = false
    cwdChip.title = s.cwd || ""
    cwdName.textContent = pathTail(s.cwd || hooks.root())
    intBtn.disabled = !s.alive
    clearBtn.disabled = false
  }

  function scrollToBottom(s: Session): void {
    s.atBottom = true
    s.out.scrollTop = s.out.scrollHeight
    s.jump.hidden = true
  }

  function selectSession(s: Session): void {
    activeId = s.id
    for (const o of sessions) o.view.hidden = o.id !== s.id
    renderTabs()
    updateCwdChip()
    idleTicks = 0
    if (active) void pull(s)
    s.input.focus()
  }

  /* ---------- 会话 ---------- */

  function makeSession(res: TermCreatedResp): Session {
    const buffer = new TermBuffer()
    const input = h("input", {
      class: "fw-term-input",
      type: "text",
      spellcheck: "false",
      autocomplete: "off",
      placeholder: "输入命令，回车执行（Ctrl+C 中断 · ↑↓ 历史 · Ctrl+L 清屏）",
    })
    const prompt = h("span", { class: "fw-term-prompt" })
    const out = h("div", { class: "fw-term-out" })
    const jump = h("button", { class: "fw-term-jump", hidden: true }, [icon("chevronDown", 12), h("span", { text: "回到底部" })])
    const view = h("div", { class: "fw-term-view", hidden: true }, [out, jump, h("div", { class: "fw-term-inputrow" }, [prompt, input])])
    const s: Session = {
      id: res.id,
      shellName: res.shellName || res.shell || "shell",
      cwd: res.cwd ?? "",
      cursor: res.cursor ?? 0,
      buffer,
      alive: true,
      pending: 0,
      seen: new Set(),
      view,
      out,
      input,
      prompt,
      jump,
      lineNodes: [],
      renderedFrom: 0,
      atBottom: true,
      warned: false,
      composing: false,
    }
    input.addEventListener("keydown", (e) => onInputKey(s, e))
    // 输入法组合中（中文候选态）不拦截按键：Enter 属于候选确认、不是提交
    input.addEventListener("compositionstart", () => (s.composing = true))
    input.addEventListener("compositionend", () => (s.composing = false))
    out.addEventListener("scroll", () => {
      s.atBottom = s.out.scrollHeight - s.out.scrollTop - s.out.clientHeight <= 24
      s.jump.hidden = s.atBottom
    })
    jump.onclick = () => scrollToBottom(s)
    updatePrompt(s)
    return s
  }

  async function createSession(shell?: string): Promise<void> {
    const i = await ensureInfo()
    if (!i) return
    if (!i.enabled) {
      toast(i.reason || "终端未启用（服务端 GEBAI_TERMINAL=false）", "warn", 6000)
      return
    }
    if (i.maxSessions > 0 && sessions.length >= i.maxSessions) {
      toast(`终端数量已达上限（${i.maxSessions} 个）`, "warn")
      return
    }
    try {
      const res = await request<TermCreatedResp>(hooks, "POST", "/api/v1/terminal/create", {
        body: {
          root: hooks.root(),
          cwd: hooks.cwd(),
          shell: shell || i.defaultShell || undefined,
          session: hooks.session(),
          env: hooks.env(),
        },
      })
      const s = makeSession(res)
      sessions.push(s)
      body.appendChild(s.view) // 视图只挂一次：切标签切 hidden，重挂会丢滚动位置
      if (res.output) {
        s.buffer.write(res.output) // 建会话时可能已有 banner（shell 头几行）
        renderOutput(s)
      }
      selectSession(s)
      syncPlaceholder()
      renderTabs()
    } catch (err) {
      toast(`新建终端失败：${errMsg(err)}`, "error", 6000)
    }
  }

  async function closeSession(s: Session): Promise<void> {
    const idx = sessions.indexOf(s)
    if (idx < 0) return
    sessions.splice(idx, 1)
    s.view.remove()
    if (activeId === s.id) activeId = (sessions[idx - 1] ?? sessions[0])?.id ?? null
    renderTabs()
    syncPlaceholder()
    updateCwdChip()
    const next = activeSession()
    if (next) {
      next.view.hidden = false
      next.input.focus()
    }
    try {
      await request<{ ok: boolean }>(hooks, "POST", "/api/v1/terminal/close", { body: { id: s.id } })
    } catch {
      // 服务端可能已按空闲回收：本地已移除，不再打扰用户
    }
  }

  /** 清屏：只清本地缓冲（清屏是视角操作，没必要让服务端重开会话、丢掉 shell 状态）。 */
  function clearScreen(s: Session): void {
    s.buffer.clear()
    rebuildOutput(s)
  }

  /** 新建终端：多 shell 时弹菜单选，单 shell 直接建（少一次点击）。 */
  async function pickShell(anchor: HTMLElement): Promise<void> {
    const i = await ensureInfo()
    if (!i || !i.enabled) return
    const avail = (i.shells ?? []).filter((sh) => sh.available !== false)
    if (avail.length > 1) {
      dropdown(
        anchor,
        avail.map((sh) => ({
          label: `${sh.name}${sh.id === i.defaultShell ? "（默认）" : ""}`,
          icon: "terminal",
          onClick: () => void createSession(sh.id),
        })),
      )
      return
    }
    await createSession(avail[0]?.id ?? i.defaultShell)
  }

  /* ---------- 命令与输入 ---------- */

  async function submit(s: Session, raw: string): Promise<void> {
    const cmd = raw.trim()
    if (!cmd) return
    if (!s.alive) {
      toast("会话已结束，请新建终端", "warn")
      return
    }
    // 管道模式无 tty 回显：提示符与命令由前端立刻回显（服务端只回命令的输出 + 哨兵结果）
    writeLocal(s, `${promptText(s)}${cmd}\n`)
    remember(cmd)
    const busyBefore = s.pending > 0
    s.pending++
    if (!busyBefore) renderTabs()
    try {
      // exec:true = 交给服务端按「命令」执行（注入哨兵取退出码与 cwd），而不是原样写 shell 的 stdin
      const res = await request<{ ok: boolean; cursor?: number }>(hooks, "POST", "/api/v1/terminal/input", {
        body: { id: s.id, data: `${cmd}\n`, exec: true },
      })
      if (typeof res.cursor === "number") s.cursor = res.cursor
    } catch (err) {
      s.pending = Math.max(0, s.pending - 1)
      if (s.pending === 0) renderTabs()
      appendMeta(s, `提交失败：${errMsg(err)}`, true)
      return
    }
    // 立即拉一次：echo 之类的短命令不必等下一个轮询周期
    if (active) void pull(s)
  }

  async function interrupt(s: Session): Promise<void> {
    if (!s.alive) return
    writeLocal(s, "^C\n") // 中断的本地反馈（服务端会终止当前命令并重建 shell）
    const busyBefore = s.pending > 0
    s.pending = 0
    if (busyBefore) renderTabs()
    try {
      const res = await request<{ ok: boolean; cwd?: string }>(hooks, "POST", "/api/v1/terminal/interrupt", { body: { id: s.id } })
      if (res.cwd) {
        s.cwd = res.cwd
        updatePrompt(s)
        if (s.id === activeId) updateCwdChip()
      }
    } catch (err) {
      appendMeta(s, `中断失败：${errMsg(err)}`, true)
    }
    if (active) void pull(s)
  }

  function remember(cmd: string): void {
    history = pushHistory(history, cmd, TERM_HISTORY_MAX)
    saveHistory(history)
    histIdx = history.length
    histDraft = ""
  }

  /** ↑/↓ 走历史；从最新一条再按 ↓ 回到用户正在写的那半句（shell 的习惯行为）。 */
  function navHistory(dir: -1 | 1): void {
    const s = activeSession()
    if (!s || !history.length) return
    if (dir < 0) {
      if (histIdx === history.length) histDraft = s.input.value
      histIdx = Math.max(0, histIdx - 1)
      s.input.value = history[histIdx] ?? ""
      return
    }
    if (histIdx >= history.length) return
    histIdx++
    s.input.value = histIdx >= history.length ? histDraft : (history[histIdx] ?? "")
  }

  function onInputKey(s: Session, e: KeyboardEvent): void {
    if (s.composing || e.isComposing) return // 中文输入法候选态：Enter 属于候选确认
    if (e.key === "Enter") {
      e.preventDefault()
      const cmd = s.input.value
      s.input.value = ""
      void submit(s, cmd)
      return
    }
    if (e.key === "ArrowUp" || e.key === "ArrowDown") {
      if (!history.length) return // 无历史：让 ↑↓ 照常在输入框内移动光标
      e.preventDefault()
      navHistory(e.key === "ArrowUp" ? -1 : 1)
      return
    }
    if (!e.ctrlKey || e.altKey || e.metaKey) return
    const key = e.key.toLowerCase()
    if (key === "c") {
      if (s.input.selectionStart !== s.input.selectionEnd) return // 有选中文本：交给浏览器复制
      e.preventDefault()
      void interrupt(s)
      return
    }
    if (key === "l") {
      e.preventDefault()
      clearScreen(s)
    }
  }

  /* ---------- 读输出（轮询） ---------- */

  async function pull(s: Session): Promise<number> {
    try {
      const res = await request<TermReadResp>(hooks, "GET", "/api/v1/terminal/read", { params: { id: s.id, since: s.cursor } })
      let changed = 0
      if (typeof res.cursor === "number") s.cursor = res.cursor
      const text = res.text ?? ""
      if (text) {
        s.buffer.write(text)
        renderOutput(s)
        changed = text.length
      }
      const busyBefore = s.pending > 0
      for (const ex of res.exits ?? []) {
        if (!ex || s.seen.has(ex.token)) continue
        s.seen.add(ex.token)
        s.pending = Math.max(0, s.pending - 1)
        if (ex.cwd) {
          s.cwd = ex.cwd
          updatePrompt(s)
          if (s.id === activeId) updateCwdChip()
        }
        // 退出码 0 是常态，不必占一行；非 0 才值得提示（弱化样式，不打断输出阅读）
        if (ex.code !== 0) appendMeta(s, `退出码 ${ex.code}`)
        changed++
      }
      const busyAfter = s.pending > 0
      if (busyBefore !== busyAfter) renderTabs()
      if (res.alive === false && s.alive) {
        s.alive = false
        s.input.disabled = true
        s.input.placeholder = "会话已结束，请新建终端"
        appendMeta(s, "会话已结束", true)
        renderTabs()
        if (s.id === activeId) updateCwdChip()
      }
      return changed
    } catch (err) {
      // 只在首次失败提示：服务重启/网络抖动时避免每 250ms 刷一条 toast
      if (!s.warned) {
        s.warned = true
        toast(`终端读取失败：${errMsg(err)}`, "error", 6000)
      }
      return 0
    }
  }

  function pullActive(): Promise<unknown> {
    const s = activeSession()
    return s && s.alive ? pull(s) : Promise.resolve(0)
  }

  function schedule(ms: number): void {
    if (!active || paused) return
    if (timer) clearTimeout(timer)
    timer = setTimeout(() => void tick(), ms)
  }

  async function tick(): Promise<void> {
    timer = 0
    if (!active || paused) return
    const s = activeSession()
    const got = s && s.alive ? await pull(s) : 0
    if (!active || paused) return
    idleTicks = got > 0 ? 0 : idleTicks + 1
    schedule(idleTicks >= IDLE_TICKS ? POLL_SLOW_MS : POLL_FAST_MS)
  }

  function onVisibility(): void {
    paused = document.visibilityState === "hidden"
    if (paused) {
      if (timer) {
        clearTimeout(timer)
        timer = 0
      }
      return
    }
    if (!active) return
    void pullActive() // 回到前台立即补一次（后台期间可能积了整段输出）
    schedule(POLL_FAST_MS)
  }

  /* ---------- 能力探测与跟随当前根 ---------- */

  /** 能力探测（只探一次，结果缓存）：enabled=false 时只显示原因，不创建会话。 */
  function ensureInfo(): Promise<TermInfoResp | null> {
    if (info) return Promise.resolve(info)
    if (probing) return probing
    probing = request<TermInfoResp>(hooks, "GET", "/api/v1/terminal/info")
      .then((res) => {
        info = res
        syncPlaceholder()
        syncFollowBtn()
        return res
      })
      .catch((err) => {
        probeFailed = true
        syncPlaceholder()
        toast(`终端能力探测失败：${errMsg(err)}`, "error", 6000)
        return null
      })
      .finally(() => {
        probing = null
      })
    return probing
  }

  /**
   * 根 id → 绝对路径。终端契约里的 `root` 是 id、`cwd` 是根内相对值，
   * 绝对路径只有根注册表（`/api/v1/roots`，与资源管理器同一份）有，所以跟随时按需取一次并缓存。
   */
  async function resolveRootPath(rootId: string): Promise<string | null> {
    const hit = rootPaths.get(rootId)
    if (hit) return hit
    try {
      const res = await request<{ roots: Array<{ id: string; path: string }> }>(hooks, "GET", "/api/v1/roots")
      for (const r of res.roots ?? []) if (r?.id && r?.path) rootPaths.set(r.id, r.path)
    } catch {
      return null
    }
    return rootPaths.get(rootId) ?? null
  }

  /** 跟随当前根：会话内 `cd` 到新根的绝对路径（cd 也走命令通道，新的 cwd 由哨兵回传）。 */
  async function applyFollowRoot(): Promise<void> {
    const s = activeSession()
    if (!s || !s.alive) return
    const path = await resolveRootPath(hooks.root())
    if (!path) return // 根清单里没有这个 id：保持原目录，不猜路径
    if (samePath(path, s.cwd)) return
    await submit(s, `cd ${quoteArg(path)}`)
  }

  /* ---------- 装配 ---------- */

  syncFollowBtn()
  paintPlaceholder()
  body.appendChild(placeholder)
  renderTabs()
  updateCwdChip()

  return {
    el,
    hasSession: () => sessions.length > 0,
    activate() {
      if (active) {
        void pullActive()
        return
      }
      active = true
      paused = document.visibilityState === "hidden"
      document.addEventListener("visibilitychange", onVisibility)
      void (async () => {
        const i = await ensureInfo()
        // 首次展开即建一条会话（能力关闭时不建，只留占位说明）
        if (i?.enabled && !sessions.length) await createSession()
        if (active) void pullActive()
      })()
      void pullActive()
      schedule(POLL_FAST_MS)
      activeSession()?.input.focus()
    },
    deactivate() {
      // 只暂停轮询：服务端会话保留（空闲由服务端按 idleMs 回收），再次展开即恢复
      active = false
      if (timer) {
        clearTimeout(timer)
        timer = 0
      }
      document.removeEventListener("visibilitychange", onVisibility)
    },
    onRootChanged() {
      if (followRoot) void applyFollowRoot()
    },
  }
}

/** 命令参数加引号（跟随根的 cd 用）：cmd 与 POSIX shell 都吃双引号，内部双引号按最通用写法转义。 */
function quoteArg(p: string): string {
  return `"${p.replace(/"/g, '\\"')}"`
}
