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
import { clear, confirmDialog, dropdown, h, icon, promptDialog, showMenu, toast } from "./ui"
import { pathTail, samePath } from "./terminal-core"
import { appPath } from "@gebai/sdk"
import { WorkbenchSocket } from "./ws-client"
import { realSessionIds, readTermSessions, writeTermSessions } from "./term-sessions"
import { MIN_CONTRAST_RATIO, searchMatchColors, terminalTheme } from "./term-theme"
import {
  DEFAULT_SEARCH_OPTIONS,
  SEARCH_TOGGLES,
  parseSearchOptions,
  searchDecorations,
  searchStatusText,
  toggleSearchOption,
  type SearchToggle,
  type TermSearchOptions,
} from "./term-search"
import {
  CURSOR_STYLES,
  DEFAULT_PREFS,
  FONT_SIZE_KEY,
  FOLLOW_ROOT_KEY,
  LINE_HEIGHT_STEPS,
  PREFS_KEY,
  clampFontSize,
  clampLineHeight,
  nextInCycle,
  parsePrefs,
  serializePrefs,
  type TermPrefs,
} from "./term-prefs"
import { resolveTabLabel, shouldConfirmClose, tabTooltip } from "./term-tabs"
import { TERM_KEYS, termKeyBindings, type TermActions } from "./term-keys"
import "../css/terminal.css"
import "../css/terminal-pty.css"
import type { Terminal as XTerm } from "@xterm/xterm"
import type { FitAddon } from "@xterm/addon-fit"
import type { SearchAddon } from "@xterm/addon-search"
import type { TerminalHooks, TerminalPanel } from "./terminal-legacy"
import { workbenchKeymap } from "./keymap-wb"
import { matchKey, parseSpec } from "../keymap"

/* ------------------------------ 终端键位 ------------------------------ */

/** 等一帧：布局尺寸要等浏览器算完样式才可信（FitAddon 量列数依赖它）。 */
function nextFrame(): Promise<void> {
  return new Promise((resolve) => requestAnimationFrame(() => resolve()))
}

/** 光标样式的菜单文案（xterm 的取值是英文术语，菜单里给中文）。 */
const CURSOR_LABEL: Record<TermPrefs["cursorStyle"], string> = { bar: "竖线", block: "方块", underline: "下划线" }

/** 事件是否命中一组键位写法（与键位表同源）。 */
function hits(e: KeyboardEvent, specs: readonly string[]): boolean {
  return specs.some((s) => {
    const p = parseSpec(s)
    return !!p && matchKey(e, p)
  })
}

/** 面板动作句柄：键位表只注册一次（面板可能重建），动作按最新面板转发。 */
let termActions: TermActions | null = null
let termKeysRegistered = false

/**
 * 登记终端键位（首次调用生效）：焦点限定在终端面板内、摘获阶段——
 * 这样面板里按下的键不会被工作台全局键抢走，shell 的 readline 键（删词/历史/行尾）全部回归。
 */
function registerTermKeys(actions: TermActions): void {
  termActions = actions
  if (termKeysRegistered) return
  termKeysRegistered = true
  workbenchKeymap.addAll(termKeyBindings(() => termActions))
}

/* ------------------------------ 偏好读写 ------------------------------ */

/** 读偏好：JSON 键为主，字号与跟随根缺省时读旧键（升级不重置用户设置）。 */
function readPrefs(): TermPrefs {
  const read = (k: string): unknown => {
    try {
      return localStorage.getItem(k)
    } catch {
      return null
    }
  }
  return parsePrefs(read(PREFS_KEY), { fontSize: read(FONT_SIZE_KEY), followRoot: read(FOLLOW_ROOT_KEY) })
}

/** 写偏好：JSON 键 + 字号/跟随根两个旧键（降级实现读同一份值，两版式字号一致）。 */
function savePrefs(p: TermPrefs): void {
  try {
    localStorage.setItem(PREFS_KEY, serializePrefs(p))
    localStorage.setItem(FONT_SIZE_KEY, String(p.fontSize))
    localStorage.setItem(FOLLOW_ROOT_KEY, p.followRoot ? "1" : "0")
  } catch {
    /* 存储不可用：仅本次生效 */
  }
}

/** 搜索开关的本地键（与查找框共生命周期：刷新后保持上次的开关状态）。 */
const SEARCH_OPTS_KEY = "gebai.ui.termSearchOpts"

function readSearchOpts(): TermSearchOptions {
  try {
    return parseSearchOptions(localStorage.getItem(SEARCH_OPTS_KEY))
  } catch {
    return { ...DEFAULT_SEARCH_OPTIONS }
  }
}

function saveSearchOpts(o: TermSearchOptions): void {
  try {
    localStorage.setItem(SEARCH_OPTS_KEY, JSON.stringify(o))
  } catch {
    /* 同上 */
  }
}

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
  const base = appPath("/vendor/xterm")
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

/**
 * 从主题 CSS 变量取一次配色：背景/前景/选区来自界面主题，**16 色 ANSI 调色板不来自主题**
 * （按背景明暗取 VSCode 默认的暗/亮两套，见 term-theme——亮色与常规色必须是两档取值）。
 */
function themeColors(): { theme: Record<string, string>; match: ReturnType<typeof searchMatchColors> } {
  const cs = getComputedStyle(document.documentElement)
  const v = (name: string, fallback: string) => cs.getPropertyValue(name).trim() || fallback
  const background = v("--bg-inset", "#181818")
  return {
    theme: terminalTheme({ background, foreground: v("--text", "#d4d4d4") }),
    match: searchMatchColors(background),
  }
}

/**
 * 服务端是不是 Windows（决定要不要启用 xterm 的 ConPTY 语义）。
 * 先看服务端给的 shell 路径（最权威：终端跑在服务端，不在浏览器所在机器），再回落 UA。
 */
function serverIsWindows(info: TermInfo | null): boolean {
  const p = info?.shells.find((s) => s.available)?.path ?? ""
  if (p) return /^[a-zA-Z]:[\\/]/.test(p) || /\\.exe$/i.test(p)
  return /Windows/i.test(navigator.userAgent)
}

/** 终端字体：自带 JetBrains Mono（public/fonts/ 随产物分发，四字重 @font-face 见 base.css）
 * 置于一切系统字体之前——终端度量（列宽/行高/字形对齐）不受目标机器字体环境干扰；
 * 字体文件未就绪时由 font-display: swap 回退到后续系统字体，无白屏。 */
function terminalFontFamily(): string {
  return "'JetBrains Mono', Consolas, 'Cascadia Mono', 'Courier New', monospace"
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
  const url = new URL(appPath("/api/v1/terminal/info"), location.origin)
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

/** 根 id → 绝对路径 + 展示名（跟随当前根、cwd 芯片用；与资源管理器同一份根清单）。 */
async function fetchRootInfo(rootId: string): Promise<{ path: string; name: string } | null> {
  try {
    const token = readToken()
    const res = await fetch(appPath("/api/v1/roots"), { headers: token ? { Authorization: `Bearer ${token}` } : {} })
    if (!res.ok) return null
    const body = (await res.json()) as { roots?: Array<{ id: string; path: string; name?: string }> }
    const hit = body.roots?.find((r) => r.id === rootId)
    return hit ? { path: hit.path, name: hit.name ?? "" } : null
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
  /** shell 经 OSC 0/2 上报的标题（标签名优先于 shell 名）。 */
  oscTitle: string
  /** 用户重命名（最高优先，仅本次页面生命周期）。 */
  customTitle: string
  /** 退出码（dead 标签 tooltip 用）。 */
  exitCode: number | null
  /** 非活动标签有新输出（标签上的亮点提示）。 */
  unread: boolean
  /** 最近一次收到输出的时间：关闭确认的忙闲近似判定（见 term-tabs）。 */
  lastOutputAt: number
}

/** 终端面板（PTY + xterm）：多标签、复制粘贴、搜索、字号、偏好、跟随当前根。 */
export function createPtyTerminal(hooks: TerminalHooks): TerminalPanel {
  let vendor: XtermVendor | null = null
  let info: TermInfo | null = null
  let active = false
  let booting: Promise<void> | null = null
  let prefs = readPrefs()
  let searchOpts = readSearchOpts()
  let activeId: string | null = null
  const tabs: PtyTab[] = []
  const socket = new WorkbenchSocket({ label: "终端", context: () => hooks.session() })
  /** 根 id → 绝对路径 + 展示名（跟随根与 cwd 芯片共用一次拉取）。 */
  const rootInfo = new Map<string, { path: string; name: string }>()

  /* ---------- 骨架 ---------- */

  const tabbar = h("div", { class: "fw-term-tabs" })
  const cwdName = h("span", { class: "fw-term-cwd-name" })
  const cwdChip = h("span", { class: "fw-term-cwd", hidden: true }, [cwdName])
  const body = h("div", { class: "fw-pty-body" })
  const placeholder = h("div", { class: "fw-term-notice" })
  const searchBar = h("div", { class: "fw-pty-search", hidden: true })
  const searchInput = h("input", { class: "fw-input sm", placeholder: "在终端中查找…", type: "search" })
  const searchStatus = h("span", { class: "fw-pty-search-status" })
  /** 查找框内的三个开关（区分大小写 / 全词 / 正则）——与 VSCode 终端查找同顺序、同语义。 */
  const searchToggles = SEARCH_TOGGLES.map((t) => {
    const b = h("button", { class: "fw-term-btn fw-term-toggle", title: t.title, text: t.label })
    b.onclick = () => {
      searchOpts = toggleSearchOption(searchOpts, t.key)
      saveSearchOpts(searchOpts)
      paintSearchToggles()
      const tab = activeTab()
      // 先清一次：search addon 对**同一个关键词**不重建高亮与计数（它按关键词缓存判定），
      // 不清就会「换了开关但命中数还是旧的」
      tab?.search.clearDecorations()
      searchInput.focus()
      findInTerm(1)
    }
    return { key: t.key as SearchToggle, el: b }
  })
  searchBar.append(
    icon("search", 13),
    searchInput,
    searchStatus,
    ...searchToggles.map((t) => t.el),
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

  const newBtn = btn("plus", "新建终端（选 Shell；Ctrl+Shift+` 直接新建）", () => pickShell())
  const clearBtn = btn("trash", "清屏（Ctrl+K）", () => {
    const t = activeTab()
    t?.term.clear()
  })
  const intBtn = btn("minus", "中断当前命令（Ctrl+C）", () => void interruptActive(), "danger")
  const searchBtn = btn("search", "在终端中查找（Ctrl+F）", () => toggleSearch())
  const moreBtn = btn("settings", "终端设置（字号 / 行高 / 光标 / 跟随当前根 / 重启）", () => openMore())
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
    if (!data) return
    tab.lastOutputAt = Date.now()
    // 非活动标签来了新输出：点个亮点（VSCode 的活动指示），切回去就消
    if (tab.id !== activeId && !tab.unread) {
      tab.unread = true
      paintTabs()
    }
    tab.term.write(data)
  })
  socket.on("term.ready", (p) => {
    const tab = tabs.find((t) => t.id === p.id)
    if (!tab) return
    tab.alive = true
    // 会话就绪后对齐一次尺寸：建会话与 fit 之间有竞态窗口（见 openTab）
    syncSize(tab)
    paintTabs()
  })
  socket.on("term.exit", (p) => {
    const tab = tabs.find((t) => t.id === p.id)
    if (!tab) return
    tab.alive = false
    tab.exitCode = typeof p.code === "number" ? p.code : null
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
      const label = resolveTabLabel({ shellName: t.shellName, oscTitle: t.oscTitle, custom: t.customTitle })
      const cls = `fw-term-tab${t.id === activeId ? " active" : ""}${t.alive ? "" : " dead"}${t.unread ? " unread" : ""}`
      const tabEl = h("div", { class: cls, title: tabTooltip({ label, shellName: t.shellName, cwd: t.cwd, alive: t.alive, exitCode: t.exitCode }) }, [
        h("span", { class: "fw-term-dot" }),
        h("span", { class: "fw-term-tab-name", text: label }),
        closeTabBtn,
      ])
      tabEl.onclick = () => selectTab(t.id)
      // 双击重命名（与 VSCode 的「重命名终端」同语义）；右键菜单是同一套动作的入口
      tabEl.ondblclick = (e) => {
        e.stopPropagation()
        void renameTab(t)
      }
      tabEl.oncontextmenu = (e) => {
        e.preventDefault()
        e.stopPropagation()
        openTabMenu(t, e.clientX, e.clientY)
      }
      t.tabEl = tabEl
      tabbar.appendChild(tabEl)
    }
    syncCwdChip()
  }

  /** 标签右键菜单：重命名 / 重启 / 关闭（与双击重命名同源）。 */
  function openTabMenu(t: PtyTab, x: number, y: number): void {
    showMenu(x, y, [
      { label: "重命名…", icon: "edit", onClick: () => void renameTab(t) },
      { label: "重启该终端", icon: "refresh", onClick: () => void restartTab(t) },
      { label: "复制工作目录", icon: "copy", disabled: !t.cwd, onClick: () => void copyText(t.cwd, "已复制工作目录") },
      { separator: true },
      { label: "关闭该终端", icon: "close", onClick: () => void closeTab(t.id) },
    ])
  }

  /** 重命名（空输入 = 恢复默认：回到 shell 标题 / shell 名）。 */
  async function renameTab(t: PtyTab): Promise<void> {
    const cur = resolveTabLabel({ shellName: t.shellName, oscTitle: t.oscTitle, custom: t.customTitle })
    const next = await promptDialog({ title: "重命名终端", label: "留空恢复默认标题", value: cur, placeholder: t.shellName })
    if (next === null) return
    t.customTitle = next.trim()
    paintTabs()
  }

  /** cwd 芯片：显示当前目录尾名，回落到根目录名/路径尾（旧实现回落的是根 **id**，会显示成 `bind:self_optimize`）。 */
  function syncCwdChip(): void {
    const t = activeTab()
    if (!t) {
      cwdChip.hidden = true
      return
    }
    const rootId = hooks.root()
    const info = rootInfo.get(rootId)
    const where = t.cwd || info?.path || ""
    const label = pathTail(where) || info?.name || rootId
    cwdChip.hidden = false
    cwdName.textContent = label
    cwdChip.title = where || label
  }

  function selectTab(id: string): void {
    const t = tabs.find((x) => x.id === id)
    if (!t) return
    activeId = id
    t.unread = false
    for (const x of tabs) x.view.hidden = x.id !== id
    paintTabs()
    persist()
    // 切到可见后再 fit（hidden 元素量不出尺寸）
    requestAnimationFrame(() => {
      fitTab(t)
      t.term.focus()
    })
  }

  /* ---------- 会话记忆（刷新后 attach 回已有会话，而不是新建） ---------- */

  /**
   * 把「当前开着的会话」写回 localStorage（含活动项）。
   *
   * 只在三处变化后调用：新建成功（id 迁移成服务端 id 之后）、关闭、切标签；
   * 不存前端占位 id（`tmpN`）——那种 id 服务端不认识，下次刷新 attach 必然失败。
   */
  function persist(): void {
    // 去重：并发接管/重复调用下不允许同一会话占两个位置（重复项会让下次刷新 attach 出两个标签）
    const ids = [...new Set(realSessionIds(tabs.map((t) => t.id)))]
    writeTermSessions("pty", ids, activeId && ids.includes(activeId) ? activeId : null)
  }

  /**
   * 按记忆重新接管服务端已有会话（页面刷新 / 重新打开工具窗）。
   *
   * 只接管**服务端确实还在**的会话：`term.list` 拿到清单后逐个 `term.attach`（服务端会回放缓冲，
   * 所以终端内容与 shell 里跑着的进程都回来）。服务端已回收到（空闲超时）或服务重启造成的失效 id
   * 直接丢弃——一个都没接管到时清空记忆，避免每次刷新都白试一轮。
   */
  async function restoreTabs(): Promise<number> {
    if (tabs.length) return 0 // 已经有标签（已接管 / 用户先新建了一个）：不再重复接管
    const mem = readTermSessions("pty")
    if (!mem.ids.length) return 0
    const reply = await socket.request("term.list")
    if (!reply.ok) return 0
    const live = (reply.payload?.sessions as Array<{ id?: string; shell?: string; shellName?: string; cwd?: string; alive?: boolean }> | undefined) ?? []
    const byId = new Map(live.filter((s) => s.id).map((s) => [String(s.id), s]))
    let kept = 0
    for (const id of mem.ids) {
      const meta = byId.get(id)
      if (!meta) continue
      const t = makeXtermTab()
      t.id = id
      t.shellId = String(meta.shell ?? "")
      t.shellName = String(meta.shellName ?? "终端")
      t.cwd = String(meta.cwd ?? "")
      t.alive = meta.alive !== false
      tabs.push(t)
      body.appendChild(t.view)
      dropNotice()
      const attached = await socket.request("term.attach", { id, cols: t.term.cols, rows: t.term.rows })
      if (!attached.ok) {
        // 服务端举手了（比如会话刚好被回收）：拆掉这个标签，不留“连不上的终端”
        t.ro.disconnect()
        t.term.dispose()
        t.view.remove()
        tabs.pop()
        continue
      }
      kept++
      persist() // 逐个落盘：中途被打断也不丢已接管的会话
    }
    if (!kept) {
      writeTermSessions("pty", [], null)
      return 0
    }
    const want = mem.active && tabs.some((t) => t.id === mem.active) ? mem.active : tabs[0]!.id
    selectTab(want) // 内部会 persist，把实际接管到的清单（含活动项）落盘
    return kept
  }

  /** 正在接管中的恢复（与 `opening` 同构：并发 activate 必须串行）。 */
  let restoring: Promise<number> | null = null

  /**
   * 接管的串行入口。
   *
   * `activate()` 会被并发触发（启动阶段恢复工具窗状态 + 用户点活动栏、切换视图等），
   * 而 restore 是异步的——不加互斥时后一次进来看到的 `tabs` 仍是空，会把同一批会话 attach 两遍。
   */
  function restoreOnce(): Promise<number> {
    if (restoring) return restoring
    restoring = restoreTabs().finally(() => {
      restoring = null
    })
    return restoring
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

  /**
   * 开一条终端：先建 xterm 视图（立刻可见），**量出真实尺寸再申请 PTY 会话**。
   *
   * 顺序要紧：xterm 建出来时是默认 80×24，面板尺寸要等到它进入可见布局后才量得到（FitAddon）。
   * 若先把默认尺寸随 `term.open` 发出去，则 PTY 会以 80×24 起步、而屏幕按真实列数渲染——
   * `COLUMNS/LINES` 错、长行折行错位、TUI 按 80×24 排版（实测症状），而且只有窗口 resize
   * 才偶然被纠正（因为那次 fit 触发的 resize 终于带上了真实会话 id）。
   */
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
    await nextFrame()
    fitTab(t)
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
    // open 往返期间用户可能已拖过面板/改过窗口：拿到真 id 后对齐一次尺寸
    syncSize(t)
    paintTabs()
    persist()
    return t
  }

  /**
   * 关闭标签：有进程在跑时先确认（VSCode 的 confirmOnKill 同语义）。
   * 忙闲为**近似判定**（最近有输出即认为在跑，见 term-tabs），确认只是一次拦截，不阻塞其它操作。
   */
  async function closeTab(id: string): Promise<void> {
    const idx = tabs.findIndex((t) => t.id === id)
    if (idx < 0) return
    const t = tabs[idx]
    if (shouldConfirmClose({ alive: t.alive, lastOutputAt: t.lastOutputAt, now: Date.now() })) {
      const ok = await confirmDialog({
        title: "关闭这个终端？",
        message: `「${resolveTabLabel({ shellName: t.shellName, oscTitle: t.oscTitle, custom: t.customTitle })}」最近仍有输出，可能有命令在跑。`,
        okText: "终止并关闭",
        danger: true,
      })
      if (!ok) return
    }
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
    persist()
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
      fontSize: prefs.fontSize,
      lineHeight: prefs.lineHeight,
      cursorBlink: prefs.cursorBlink,
      cursorStyle: prefs.cursorStyle,
      scrollback: 8000,
      allowProposedApi: true,
      // 最小对比度：与 VSCode 默认一致（4.5），ANSI 前景与背景对比不足时向可读方向调整
      minimumContrastRatio: MIN_CONTRAST_RATIO,
      theme: themeColors().theme,
      // 只有 Windows 的 ConPTY 需要这套换行/光标重绘语义；在 POSIX 上启用会让 xterm 按伪控制台的
      // 约定处理包装行（与真实 tty 的行为不一致，长行回显/重绘时会错位）
      ...(serverIsWindows(info) ? { windowsPty: { backend: "conpty" as const } } : {}),
    })
    const fit = new v.FitAddon()
    const search = new v.SearchAddon()
    term.loadAddon(fit)
    term.loadAddon(search)
    term.loadAddon(new v.WebLinksAddon())
    term.open(host)
    applySearchColors(host)
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
    // 命中计数（查找框右侧的「第 n/m 项」）：search addon 每次找完都会报一次
    search.onDidChangeResults((r) => setSearchStatus(r))
    // shell 可以随时改标题（OSC 0/2）：VSCode 用标题作标签名，我们也跟随
    term.onTitleChange((title) => {
      const t = tabs.find((x) => x.term === term)
      if (!t) return
      t.oscTitle = String(title ?? "")
      paintTabs()
    })
    term.onSelectionChange(() => {
      // 选中即复制（默认关）：VSCode 的同名选项，Linux 终端习惯
      if (!prefs.copyOnSelection) return
      const sel = term.getSelection()
      if (sel) void copyText(sel, "已复制终端选区", true)
    })
    // Ctrl+滚轮 缩放字号：浏览器里 Ctrl+滚轮是**整页缩放**，终端里应归字号
    host.addEventListener(
      "wheel",
      (e) => {
        if (!e.ctrlKey || !prefs.wheelZoom) return
        e.preventDefault()
        e.stopPropagation()
        applyPrefs({ fontSize: prefs.fontSize + (e.deltaY < 0 ? 1 : -1) })
      },
      { passive: false, capture: true },
    )
    // 中键粘贴（X11 惯例）；但**鼠标上报开启的程序**（vim set mouse=a、htop…）要自己吃中键
    host.addEventListener("mousedown", (e) => {
      if (e.button !== 1) return
      if (mouseReporting()) return
      e.preventDefault()
      void pasteClipboard()
    })
    host.addEventListener("auxclick", (e) => {
      if (e.button === 1 && !mouseReporting()) e.preventDefault()
    })
    host.oncontextmenu = (e) => {
      e.preventDefault()
      // 鼠标上报开启时，右键是**程序**的（vim 的右键菜单/扩展选择等）；面板菜单让位，
      // 按住 Shift 强制调出（与「Shift 拖动可绕过鼠标上报选文本」同一套约定）
      if (mouseReporting() && !e.shiftKey) return
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
      oscTitle: "",
      customTitle: "",
      exitCode: null,
      unread: false,
      lastOutputAt: 0,
      ro: new ResizeObserver(() => fitTab(t)),
    }
    t.ro.observe(host)
    // currentId 在闭包里用 activeId 对应 tab 的 id：xterm 回调不携带 tab 引用
    const currentId = () => tabs.find((x) => x.term === term)?.id ?? null
    return t
  }

  /** 搜索命中底色（xterm 6 的 decoration 不读 backgroundColor，靠 CSS 变量落到 .xterm-find-result-decoration 上）。 */
  function applySearchColors(host: HTMLElement): void {
    const c = themeColors().match
    host.style.setProperty("--term-find-match", c.matchBackground)
    host.style.setProperty("--term-find-active", c.activeMatchBackground)
  }

  /**
   * 当前终端是否处于**鼠标上报**模式（`CSI ? 1000/1002/1003 h`）：
   * 全屏程序（vim set mouse=a / htop / less --mouse）启用后，鼠标事件应归程序——
   * 中键与右键就不能再被面板的粘贴 / 菜单截走。Shift 仍可强制走面板（xterm 也用 Shift 绕过上报选文本）。
   */
  function mouseReporting(): boolean {
    const t = activeTab()
    if (!t) return false
    try {
      return t.term.modes.mouseTrackingMode !== "none"
    } catch {
      return false
    }
  }

  /** 把当前 xterm 尺寸下发给服务端（建会话后补发 / 重连对齐用）。 */
  function syncSize(t: PtyTab): void {
    if (!t.id || t.id.startsWith("tmp")) return
    void socket
      .ensureOpen()
      .then(() => socket.send("term.resize", { id: t.id, cols: t.term.cols, rows: t.term.rows }))
      .catch(() => {})
  }

  // 键位表登记：document 捕获 + focus 限定在终端面板内（面板重建时只转发到最新动作）
  registerTermKeys({
    copy: () => void copySelection(),
    paste: () => void pasteClipboard(),
    search: () => toggleSearch(true),
    fontSize: (delta) => applyPrefs({ fontSize: prefs.fontSize + delta }),
    fontReset: () => applyPrefs({ fontSize: DEFAULT_PREFS.fontSize }),
    clear: () => {
      const t = activeTab()
      t?.term.clear()
      t?.term.focus()
    },
    selectAll: () => activeTab()?.term.selectAll(),
    newTab: () => void createTab(),
    closeTab: () => {
      const t = activeTab()
      if (t) void closeTab(t.id)
    },
    switchTab: (delta) => switchTab(delta),
    scroll: (to) => {
      const t = activeTab()
      if (!t) return
      if (to === "top") t.term.scrollToTop()
      else t.term.scrollToBottom()
    },
    interrupt: () => {
      // 有选区则复制，否则中断当前命令（ConPTY 不认 ETX，见服务端 interrupt）
      const t = activeTab()
      if (t?.term.getSelection()) void copySelection()
      else void interruptActive()
    },
  })

  /** 切换终端标签（Ctrl+Shift+↑/↓）：按标签栏顺序循环。 */
  function switchTab(delta: 1 | -1): void {
    if (tabs.length < 2) return
    const i = tabs.findIndex((t) => t.id === activeId)
    const next = tabs[(i + delta + tabs.length) % tabs.length]
    if (next) selectTab(next.id)
  }

  /** 中断当前命令（终止进程树并以原目录重建 shell）。连按节流：避免一次紧张操作把 shell 重建多次。 */
  let lastInterrupt = 0
  async function interruptActive(): Promise<void> {
    const t = activeTab()
    if (!t) return
    if (!t.alive) {
      toast("终端进程已结束（按 Enter 重启，或关闭该标签）", "warn")
      return
    }
    const now = Date.now()
    if (now - lastInterrupt < 800) return
    lastInterrupt = now
    const reply = await socket.request("term.interrupt", { id: t.id })
    if (!reply.ok) {
      toast(reply.error ?? "中断失败", "error")
      return
    }
    // 中断后 shell 回到会话创建时的目录（Windows 重建 shell）：cwd 芯片跟着服务端回报走
    const cwd = reply.payload?.cwd
    if (typeof cwd === "string") {
      t.cwd = cwd
      paintTabs()
    }
  }

  /* ---------- 交互：快捷键 / 菜单 / 搜索 ---------- */

  /**
   * xterm 键盘预处理：返回 false 表示不交给终端（由终端自身处理）。
   * 键位定义与键位表同源（TERM_KEYS）——键位表在 document 捕获阶段先接管，
   * 这一路是兜底（实测 `attachCustomKeyEventHandler` 对部分组合键不生效）。
   */
  function onTermKey(e: KeyboardEvent): boolean {
    if (e.type !== "keydown") return true
    if (hits(e, TERM_KEYS.copy)) {
      void copySelection()
      return false
    }
    if (hits(e, TERM_KEYS.paste)) {
      void pasteClipboard()
      return false
    }
    if (hits(e, TERM_KEYS.search)) {
      toggleSearch(true)
      return false
    }
    if (hits(e, TERM_KEYS.fontUp)) {
      applyPrefs({ fontSize: prefs.fontSize + 1 })
      return false
    }
    if (hits(e, TERM_KEYS.fontDown)) {
      applyPrefs({ fontSize: prefs.fontSize - 1 })
      return false
    }
    if (hits(e, TERM_KEYS.fontReset)) {
      applyPrefs({ fontSize: DEFAULT_PREFS.fontSize })
      return false
    }
    if (hits(e, TERM_KEYS.clear)) {
      const t = activeTab()
      t?.term.clear()
      return false
    }
    if (hits(e, TERM_KEYS.selectAll)) {
      activeTab()?.term.selectAll()
      return false
    }
    if (hits(e, TERM_KEYS.nextTab) || hits(e, TERM_KEYS.prevTab)) {
      switchTab(hits(e, TERM_KEYS.nextTab) ? 1 : -1)
      return false
    }
    if (hits(e, TERM_KEYS.interrupt)) {
      const t = activeTab()
      if (t?.term.getSelection()) void copySelection()
      else void interruptActive()
      return false
    }
    if (e.key === "Escape" && !searchBar.hidden) {
      toggleSearch(false)
      return false
    }
    // 进程已结束的标签：Enter 就地重启（VSCode 里同样不用先关掉再新建）
    if (e.key === "Enter" && !activeTab()?.alive && !e.ctrlKey && !e.altKey && !e.metaKey && !e.shiftKey) {
      void restartTab(activeTab())
      return false
    }
    return true
  }

  /**
   * 写剪贴板：`navigator.clipboard` 在非安全上下文/权限受限时会直接拒绝，
   * 此时回退到隐藏 textarea + `execCommand("copy")`（已废弃但仍是唯一的回退通道）。
   * 返回是否写成功（调用方据此决定要不要报错）。
   */
  async function copyText(text: string, okMessage: string, quiet = false): Promise<boolean> {
    if (!text) return false
    try {
      await navigator.clipboard.writeText(text)
      if (!quiet) toast(okMessage, "success")
      return true
    } catch {
      /* 走下面的回退 */
    }
    try {
      const ta = h("textarea", { class: "fw-term-clip" })
      ta.value = text
      document.body.appendChild(ta)
      ta.select()
      const ok = document.execCommand("copy")
      ta.remove()
      if (ok) {
        if (!quiet) toast(okMessage, "success")
        return true
      }
    } catch {
      /* 两者都不可用：如实报错 */
    }
    if (!quiet) toast("复制失败：剪贴板不可用", "error")
    return false
  }

  async function copySelection(): Promise<void> {
    const sel = activeTab()?.term.getSelection() ?? ""
    if (!sel) return
    await copyText(sel, "已复制终端选区")
  }

  async function pasteClipboard(): Promise<void> {
    const t = activeTab()
    if (!t || !t.alive) {
      toast("终端进程已结束（按 Enter 重启，或关闭该标签）", "warn")
      return
    }
    try {
      const text = await navigator.clipboard.readText()
      if (text) t.term.paste(text)
    } catch {
      // 读剪贴板需要权限/安全上下文：把可行路径说清楚（原生粘贴事件不经过权限接口）
      toast("无法读取剪贴板：请用 Ctrl+V / Shift+Insert / 右键粘贴", "error", 5000)
    }
  }

  function openContextMenu(x: number, y: number): void {
    const t = activeTab()
    const hasSel = !!t?.term.getSelection()
    const dead = !!t && !t.alive
    showMenu(x, y, [
      { label: "复制", icon: "copy", disabled: !hasSel, onClick: () => void copySelection() },
      { label: "粘贴", icon: "upload", disabled: !t?.alive, onClick: () => void pasteClipboard() },
      { label: "全选", icon: "check", disabled: !t, onClick: () => t?.term.selectAll() },
      { separator: true },
      { label: "清屏", icon: "trash", disabled: !t, onClick: () => t?.term.clear() },
      { label: "查找…", icon: "search", disabled: !t, onClick: () => toggleSearch(true) },
      { label: "滚动到顶部", icon: "chevronUp", disabled: !t, onClick: () => t?.term.scrollToTop() },
      { label: "滚动到底部", icon: "chevronDown", disabled: !t, onClick: () => t?.term.scrollToBottom() },
      { separator: true },
      { label: "新建终端", icon: "plus", onClick: () => void createTab() },
      { label: dead ? "重新启动该终端" : "重启该终端", icon: "refresh", disabled: !t, onClick: () => void restartTab(t!) },
      { label: "关闭该终端", icon: "close", disabled: !t, onClick: () => void closeTab(t!.id) },
    ])
  }

  function toggleSearch(open?: boolean): void {
    const next = open ?? searchBar.hidden
    searchBar.hidden = !next
    if (next) {
      const t = activeTab()
      if (t) t.view.appendChild(searchBar)
      paintSearchToggles()
      searchInput.focus()
      searchInput.select()
    } else {
      activeTab()?.search.clearDecorations()
      searchInput.value = ""
      searchStatus.textContent = ""
      activeTab()?.term.focus()
    }
  }

  /** 三个开关的按下态（与持久化的 searchOpts 同源）。 */
  function paintSearchToggles(): void {
    for (const t of searchToggles) t.el.classList.toggle("active", searchOpts[t.key])
  }

  /** 当前终端的命中计数 → 查找框文案（由 search addon 的 onDidChangeResults 驱动）。 */
  function setSearchStatus(r: { resultIndex: number; resultCount: number } | null): void {
    searchStatus.textContent = searchStatusText(r, searchInput.value.trim())
  }

  /**
   * 查找调用包一层：开了正则开关后，**输入过程中必然出现半截式**（`[`、`(`、`\`）——
   * xterm 的 search addon 会直接把非法模式交给 RegExp 并抛异常，不包一层就是一个未捕获错误。
   */
  function runSearch(fn: () => boolean): void {
    try {
      fn()
    } catch {
      searchStatus.textContent = "正则无效"
    }
  }

  function findInTerm(direction: 1 | -1): void {
    const t = activeTab()
    if (!t) return
    const q = searchInput.value.trim()
    if (!q) {
      t.search.clearDecorations()
      setSearchStatus(null)
      return
    }
    // decorations 必须**每次**传入：search addon 只在收到它时才建命中装饰，
    // 且缺底色字段就是「不画」而不是「用默认色」——漏传 = 搜索看不见任何高亮。
    const opts = { ...searchOpts, incremental: false, decorations: searchDecorations(themeColors().match) }
    runSearch(() => (direction === 1 ? t.search.findNext(q, opts) : t.search.findPrevious(q, opts)))
  }

  searchInput.oninput = () => {
    const t = activeTab()
    if (!t) return
    const q = searchInput.value
    if (!q) {
      t.search.clearDecorations()
      setSearchStatus(null)
      return
    }
    // 增量搜索：边打边定位，但同时带上开关与 decoration（与回车路径同口径）
    runSearch(() => t.search.findNext(q, { ...searchOpts, incremental: true, decorations: searchDecorations(themeColors().match) }))
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

  /* ---------- 偏好落地 ---------- */

  /**
   * 应用偏好（局部改动走这里，不要直接改 prefs 字段）：
   * xterm 的 options 支持热更新，改动后要**重新 fit**（字号/行高变了列数就变了），
   * 并把尺寸变化下发服务端（TUI 程序据此重排）。
   */
  function applyPrefs(patch: Partial<TermPrefs>): void {
    const next: TermPrefs = {
      ...prefs,
      ...patch,
      fontSize: patch.fontSize === undefined ? prefs.fontSize : clampFontSize(patch.fontSize),
      lineHeight: patch.lineHeight === undefined ? prefs.lineHeight : clampLineHeight(patch.lineHeight),
    }
    const fontChanged = next.fontSize !== prefs.fontSize || next.lineHeight !== prefs.lineHeight
    prefs = next
    savePrefs(prefs)
    for (const t of tabs) {
      t.term.options.fontSize = prefs.fontSize
      t.term.options.lineHeight = prefs.lineHeight
      t.term.options.cursorStyle = prefs.cursorStyle
      t.term.options.cursorBlink = prefs.cursorBlink
      if (fontChanged) fitTab(t)
    }
    if (fontChanged) toast(`终端字号 ${prefs.fontSize}`, "info", 1200)
  }

  /** 主题可能被用户切换（明暗变化）：重算调色板与搜索底色并落到所有终端。 */
  function applyTheme(): void {
    for (const t of tabs) {
      t.term.options.theme = themeColors().theme
      applySearchColors(t.host)
    }
  }

  /** 设置菜单（字号 / 行高 / 光标 / 选中即复制 / Ctrl+滚轮 / 跟随当前根 / 重启）。 */
  function openMore(): void {
    const r = moreBtn.getBoundingClientRect()
    showMenu(r.left, r.bottom + 4, [
      { label: `字号 ${prefs.fontSize}（增大）`, icon: "zoomIn", onClick: () => applyPrefs({ fontSize: prefs.fontSize + 1 }) },
      { label: `字号 ${prefs.fontSize}（减小）`, icon: "zoomOut", onClick: () => applyPrefs({ fontSize: prefs.fontSize - 1 }) },
      { label: "重置字号", icon: "refresh", onClick: () => applyPrefs({ fontSize: DEFAULT_PREFS.fontSize }) },
      { label: `行高 ${prefs.lineHeight}（切换）`, icon: "wrap", onClick: () => applyPrefs({ lineHeight: nextInCycle(LINE_HEIGHT_STEPS, prefs.lineHeight) }) },
      { label: `光标：${CURSOR_LABEL[prefs.cursorStyle]}（切换）`, icon: "edit", onClick: () => applyPrefs({ cursorStyle: nextInCycle(CURSOR_STYLES, prefs.cursorStyle) }) },
      { label: `${prefs.cursorBlink ? "✓ " : ""}光标闪烁`, icon: "eye", onClick: () => applyPrefs({ cursorBlink: !prefs.cursorBlink }) },
      { label: `${prefs.copyOnSelection ? "✓ " : ""}选中即复制`, icon: "copy", onClick: () => applyPrefs({ copyOnSelection: !prefs.copyOnSelection }) },
      { label: `${prefs.wheelZoom ? "✓ " : ""}Ctrl+滚轮 缩放字号`, icon: "wheel", onClick: () => applyPrefs({ wheelZoom: !prefs.wheelZoom }) },
      { separator: true },
      { label: `${prefs.followRoot ? "✓ " : ""}跟随当前根`, icon: "folder", onClick: () => applyPrefs({ followRoot: !prefs.followRoot }) },
      { label: "跟随当前根（立即 cd 一次）", icon: "folderOpen", onClick: () => void applyFollowRoot(true) },
      { label: "重启当前终端", icon: "refresh", onClick: () => void restartTab(activeTab()) },
    ])
  }

  /** 切换工作台根：跟随开启时在当前终端里 cd 过去（`force` 用于菜单里的「立即 cd 一次」）。 */
  async function applyFollowRoot(force = false): Promise<void> {
    const t = activeTab()
    if (!t || !t.alive) return
    if (!force && !prefs.followRoot) return
    const rootId = hooks.root()
    let hit = rootInfo.get(rootId)
    if (!hit) {
      const fetched = await fetchRootInfo(rootId)
      if (!fetched) return
      rootInfo.set(rootId, fetched)
      hit = fetched
    }
    const abs = hit.path
    if (!abs) return
    if (samePath(abs, t.cwd)) return
    // 连接可能刚断：先确保通道再发（与键盘输入同一路）
    void socket
      .ensureOpen()
      .then(() => socket.send("term.input", { id: t.id, data: `cd "${abs.replace(/"/g, '\\"')}"\r` }))
      .catch(() => {})
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

  // 主题切换（明暗可能变）：重算调色板并落到所有终端
  document.addEventListener("gebai:theme-change", () => applyTheme())

  paintTabs()
  paintSearchToggles()
  paintNotice("正在准备终端…", "首次使用会加载终端内核（xterm.js）。")

  return {
    el,
    hasSession: () => tabs.length > 0,
    activate() {
      active = true
      void ensureBooted().then(async () => {
        if (!active) return
        if (info?.pty && !tabs.length && !opening) {
          // 先试着接管刷新前开着的会话（服务端会话与连接解耦，shell 还在跑）；
          // 一个都没接管到（首次打开 / 会话已被回收）才新建。
          const kept = await restoreOnce()
          if (!active) return
          if (!tabs.length && !opening) void createTab()
          else if (kept) fitActive()
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
      if (prefs.followRoot) void applyFollowRoot()
    },
  }
}
