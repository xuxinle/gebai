/**
 * 文件工作台 · 编辑器层：Monaco（VSCode 同款内核）只读/编辑双态 + 差异视图，附降级编辑器。
 *
 * 为什么用 AMD 版 Monaco（`/vendor/monaco/vs/loader.js`）而不是 `import * as monaco from "monaco-editor"`：
 * - vite 会把 ESM 版切成大量带 hash 的 chunk，dev-reload（vite build --watch）重建后旧页面引用旧 chunk → 404；
 *   与 mermaid/plantuml/d2 同一处置：**稳定文件名的静态 vendor** 由 build-vendor.ts 拷入 public/vendor/；
 * - 语言高亮 / 各语言 worker 按需从同目录加载，无需前端打包器介入。
 *
 * 主题：用 `editor.defineTheme` 把当前界面的 CSS 令牌（--bg-elev/--text/--accent…）映射成 Monaco 主题，
 * 主题切换时重新 defineTheme + setTheme（与主界面换肤联动，而不是硬编码一套配色）。
 * **半透明令牌必须先合成再喂给 Monaco**（经 `cssVarToHex`）：主题令牌多为半透明（默认主题 acrylic 的
 * `--bg-elev` 是 `rgba(18,18,23,.82)`，`--bg-inset` 亮色下是 `rgba(0,0,0,.05)`），直接丢 alpha 只取 rgb
 * 分量会得到与页面无关的假色——亮色主题下编辑器会被判成暗色（`base: vs-dark`）并铺成黑底，
 * 再叠上亮色主题的深色前景 `--text` 就是「黑底黑字」。
 * 降级：vendor 缺失（如裁剪构建）/加载超时 → 自动降级为 `highlight.js 静态高亮 + textarea 编辑`，
 * 功能不缺失（查看/编辑/保存仍可用），仅体验降级。
 */

import { cssVarToHex } from "../css-color"
import { wbAbsUrl } from "./url-base"
import {
  createMetricsSync,
  EDITOR_FONT_FAMILY,
  EDITOR_FONT_SIZE,
  ensureEditorFont,
  monoFastPathDisabled,
  type MetricsSync,
} from "./editor-metrics"
import { blameHover, blameLabel, toBlameIndex, type BlameLine } from "./blame"
import { flattenSymbols, type FlatSym } from "./symbols-core"
import { canExtract, extractSymbolsAsync, type ExtractSource } from "./symbols-extract"
import { flatSymbolsOf, installSymbolProviders, symbolSourceOf } from "./symbols"
import { installLspProviders, hasLsp } from "./lsp"
import { readWordWrap, saveWordWrap } from "./wrap"
import { readMinimap, saveMinimap } from "./minimap"
import { installContextMenuPrune } from "./context-menu"
import { buildChatSnippet, formatAbsRef, normalizeLineRange, type LineRange } from "./editor-ref"
import { showMenu, toast, type MenuItem } from "./ui"

export type { BlameLine }

type Monaco = typeof import("monaco-editor")

/** 光标/选择变化回调。 */
export type CursorListener = (info: { line: number; column: number; selected: number }) => void

export interface EditorOptions {
  value: string
  language: string
  readOnly: boolean
  /** 自动换行覆盖项；缺省用模块级开关（`isWordWrap()`：用户偏好，轮盘 / Alt+Z 切换） */
  wordWrap?: boolean
  /**
   * 小地图覆盖项；缺省用模块级开关（`isMinimap()`：用户偏好，轮盘切换，**默认开启**）。
   * 窄栏里的只读对照（合并 / 暂存窗格）显式传 `false`——那几处不听用户偏好。
   */
  minimap?: boolean
  /** 大文件降级阈值（字符数）：超过则关闭小地图/括号彩化/词法高亮，保流畅 */
  largeFileChars?: number
  /**
   * 右键菜单要用的宿主信息：
   * - `absPath`：「复制路径（含行号）」（**没给就整项不注册**，不留一个点了没用的菜单项）；
   * - `sendToChat`：「发送会话」——**仅分屏（被主界面嵌入）时给**：独立标签页里没有对话输入框可发；
   * - `groups`：宿主自己的**菜单组**（如工作台的 Git 历史），内容与文案全由宿主定。
   */
  menu?: EditorMenuHooks
}

/** 编辑器的右键菜单接口（由工作台侧提供：绝对路径要查根清单，发到对话要走分屏桥）。 */
export interface EditorMenuHooks {
  /** 这个文件的绝对路径（主界面按「根 + 相对路径」算好后传入）。 */
  absPath?: string | (() => string)
  /** 把一段带出处的代码送进对话输入框（缺省 = 不提供该项）。 */
  sendToChat?: (snippet: EditorSnippet) => void
  /**
   * 宿主的附加菜单组（缺省 = 没有）。
   * 入参是本编辑器的句柄：组内容常要按**编辑器自身能力**定（如降级实现画不出 blame 行装饰，
   * 那两项就不该出现），而句柄要到装配菜单时才建好。
   */
  groups?: (ed: EditorHandle) => EditorMenuGroup[]
}

/**
 * 编辑器右键菜单里的一个**自定义组**：一组项自成一段——Monaco 侧组与组之间自动加分隔线
 * （组名规则见 `installMenuActions`），与内置的「复制路径 / 发送会话」分开。
 */
export interface EditorMenuGroup {
  /** 组标识（短名，如 `git`）：实际组名 `gebai.<id>`，与其它组的先后按组名字典序排 */
  id: string
  items: EditorMenuItem[]
}

/** 自定义组里的一项。 */
export interface EditorMenuItem {
  /** 项标识（同组内唯一）：实际动作名 `gebai.<组>.<项>`，Monaco 侧还会再带编辑器前缀 */
  id: string
  /**
   * 菜单文案。**状态要自己带进文案**（如 `✓ 行尾溯源`）——Monaco 的菜单项在注册那一刻定下标题，
   * 开关类项的状态一变就得重装（见 `EditorHandle.refreshMenu`）。
   */
  label: string
  run: () => void
}

/** 编辑器当前选区（右键菜单与外部调用共用；行号 1 起始）。 */
export interface EditorSelection {
  /**
   * 归一后的行区间（末行在行首时不算选中，见 editor-ref）；
   * **null = 取不到行号**（降级编辑器的只读态没有光标）——此时引用就只是路径。
   */
  range: LineRange | null
  /** 选中文本；无选中时 = 光标所在整行（**降级编辑器的只读态没有光标**：那时为空串，见 EditorHandle.selection）。 */
  text: string
  /** 是否真的有选区（false = 上面是当前行，或只读降级态下没有光标） */
  hasSelection: boolean
  /** 引用串：有绝对路径时 `/a/b.ts:12-20`（无行号则是纯路径），拿不到绝对路径时降级为 `当前文件:12` */
  ref: string
  /** 语言 id（代码块标注用） */
  language: string
}

/** 「发送会话」的载荷（组装好的 Markdown 片段 + 供宿主提示的结构化字段）。 */
export interface EditorSnippet extends EditorSelection {
  /** 已经组装好的文本（引用行 + 代码块，见 editor-ref 的 buildChatSnippet） */
  markdown: string
}

/** 行级 blame 信息见 `blame.ts`（两种显示形态共用同一份数据）。 */

export interface EditorHandle {
  kind: "monaco" | "fallback"
  getValue(): string
  setValue(value: string): void
  setLanguage(language: string): void
  setReadOnly(readOnly: boolean): void
  isReadOnly(): boolean
  /** 自动换行开关（查看/编辑两态都即时生效） */
  setWordWrap(on: boolean): void
  /**
   * 小地图开关（仅**代码编辑器**有效）。
   * 大文件（超阈值降级）**不会**被打开——那正是为了性能刻意关掉的，偏好不该把它顶回来。
   */
  setMinimap(on: boolean): void
  focus(): void
  layout(): void
  revealLine(line: number, column?: number): void
  getScrollTop(): number
  setScrollTop(top: number): void
  /**
   * 光标位置（1 起始行/列）——切标签时与滚动位置一起记下，切回来放回原处。
   * 与 `getCursor()` 的区别：那个多带一个选中字符数（状态栏用），这个只回答“在哪儿”。
   */
  getCursorPos(): { line: number; column: number }
  /** 把光标放到指定位置（**不聚焦**：切标签是“看一眼”，不该把焦点从别处抢过来）。 */
  setCursorPos(pos: { line: number; column: number }): void
  getCursor(): { line: number; column: number; selected: number }
  onCursor(cb: CursorListener): void
  onChange(cb: () => void): void
  /** 全文替换（撤销栈视为一次编辑；保存后重新对齐基线用） */
  markClean(): void
  /** 文件内符号列表（「转到符号」面板用；当前语言不支持提取时为空数组）。异步：可能走语法树解析。 */
  listSymbols(): Promise<FlatSym[]>
  /** 当前语言是否有符号提取能力（面板据此决定提示文案）。 */
  supportsSymbols(): boolean
  /** 上次提取实际使用的路径（语法树 or 词法；面板据此标注结果来源）。 */
  symbolSource(): Promise<ExtractSource>
  /**
   * 打开编辑器自带的大纲（Monaco 的「转到符号」动作，数据来自内置语言服务）。
   * 需要编辑器持有文本焦点；不可用时返回 false，由调用方走工作台面板。
   */
  showOutline(): boolean
  /**
   * 设置 blame 数据与两种显示形态的开关（两态**互相独立**）：
   * `gutter` = 左侧作者列（全局）；`inline` = 光标行行尾注释。数据为空则两态都画不出。
   */
  setBlame(lines: BlameLine[], show: { gutter: boolean; inline: boolean }): void
  /**
   * 重装右键菜单里的**自定义组**（见 `EditorMenuHooks.groups`）：Monaco 的菜单项标题在注册那一刻
   * 定下，文案带状态的项（`✓ 行尾溯源`）状态一变就得重装——宿主在状态改变处调一次即可。
   * 降级实现的自绘菜单每次弹出前现取（没有可重装的东西），这里是空操作。
   */
  refreshMenu(): void
  /** 当前 model（LSP 文档同步用；降级编辑器无 model，返回 null）。 */
  model(): import("monaco-editor").editor.ITextModel | null
  /**
   * 当前选区（右键菜单用；无选中时给光标所在整行）。
   * 不可用时返回 null（无 model、降级实现下无值）；**降级编辑器的只读态没有光标**，那里无选区时
   * `range` 为 null（也就没有可发的文本）。
   */
  selection(): EditorSelection | null
  dispose(): void
}

export interface DiffHandle {
  kind: "monaco" | "fallback"
  layout(): void
  /** 自动换行开关（差异两侧一起切） */
  setWordWrap(on: boolean): void
  dispose(): void
  /**
   * 差异块导航（Monaco 可用；降级模式为 null）。
   * `state().index` 从 1 起，0 = 位置未知（尚未定位）；`onChange` 让工具条上的计数实时跟随。
   */
  nav?: DiffNav
}

/** 差异块导航：在「上/下一处差异」间跳，位置跟随滚动实时变化。 */
export interface DiffNav {
  next(): void
  prev(): void
  state(): { index: number; total: number }
  /**
   * 订阅计数变化（滚动、跳转、差异重算都会触发）。
   * 返回退订函数——订阅方是**标签栏**，每次重建标签栏都要退订旧的，
   * 否则重渲染几次就有几个野订阅在更新早已移除的 DOM。
   */
  onChange(cb: (s: { index: number; total: number }) => void): () => void
}

let monacoPromise: Promise<Monaco | null> | null = null
let monacoRef: Monaco | null = null
/** 是否真的加载失败/超时过（对外读法：`monacoLoadFailed()`）。 */
let monacoFailed = false
/** 已插入 head 的 AMD loader script（失败/超时后清理或复用，防重试时叠加多个 loader）。 */
let monacoScript: HTMLScriptElement | null = null
let currentTheme = "gebai-dark"
let lightTheme = false

/**
 * 自动换行（word wrap）：**模块级偏好 + 活动实例注册**。
 *
 * 为什么是模块级而不是每个编辑器各带一个入参：它是用户级偏好（刷新、跨文件、跨入口都一致），
 * 切换点却有多处（动作轮盘 / Alt+Z），而同一时刻可能有好几个编辑器活着（每个文件标签一个，
 * 合并视图还一次开三个）——把「记住开关」与「应用到全部」收在一处，调用方说一次 toggle 即可，
 * 不必自己遍历标签。实例 dispose 时自行注销，集合不会留住已卸载的编辑器。
 */
let wrapOn = readWordWrap()
type WrapAware = { setWordWrap: (on: boolean) => void }
const wrapTargets = new Set<WrapAware>()

/** 当前开关（轮盘按钮据此显示状态）。 */
export function isWordWrap(): boolean {
  return wrapOn
}

/** 设置开关：写回偏好并应用到所有活动编辑器（含差异视图）。 */
export function setWordWrap(on: boolean): void {
  wrapOn = on
  saveWordWrap(on)
  for (const t of wrapTargets) t.setWordWrap(on)
}

/** 切换开关并返回新状态（轮盘按钮与 Alt+Z 共用）。 */
export function toggleWordWrap(): boolean {
  setWordWrap(!wrapOn)
  return wrapOn
}

/**
 * 小地图（minimap）：与自动换行**同一套机制**（模块级偏好 + 活动实例注册）——为何是模块级而不走
 * 每个编辑器的入参，理由见上（用户级偏好、多个切换点、同时可能十几个实例活着）。
 *
 * 三处与自动换行不同，都是刻意定的：
 * ① **默认开启**（偏好模块里存储口径反过来：关闭才写键）；
 * ② **只作用于代码编辑器**：差异视图恒定关闭小地图（那个视图的“地图”职责由右侧概览尺承担，
 *    两侧再各挂一张缩略图只是噪声），合并 / 暂存窗格与它们同一取向；
 * ③ **不覆盖显式传参**：视图自己传了 `minimap`（合并 / 暂存传 false）就不听用户偏好——
 *    那些是窄栏里的只读对照，再塞一张缩略图真放不下。见下面的 `opts.minimap ?? minimapOn`。
 */
let minimapOn = readMinimap()
type MinimapAware = { setMinimap: (on: boolean) => void }
const minimapTargets = new Set<MinimapAware>()

/** 当前开关（轮盘按钮据此显示状态）。 */
export function isMinimap(): boolean {
  return minimapOn
}

/** 设置开关：写回偏好并应用到所有活动代码编辑器。 */
export function setMinimap(on: boolean): void {
  minimapOn = on
  saveMinimap(on)
  for (const t of minimapTargets) t.setMinimap(on)
}

/** 切换开关并返回新状态（轮盘按钮共用）。 */
export function toggleMinimap(): boolean {
  setMinimap(!minimapOn)
  return minimapOn
}

/** Monaco vendor 目录（`public/vendor/monaco/vs`）：loader 的 `paths.vs` 与 worker URL 都要能被内部二次拼接，故取绝对 URL。 */
export function monacoVsPath(): string {
  return wbAbsUrl("/vendor/monaco/vs")
}

/**
 * 加载 Monaco（AMD loader，单例；失败/超时返回 null 触发降级）。
 *
 * 单例语义分两层（可重试）：
 * - **进行中**的加载全局共享同一 promise——并发调用（空闲预热 + 用户打开文件）不重复下载内核；
 * - **失败/超时**不固化——复位单例并移除 loader script，后续调用可重新尝试。
 *   旧版把 resolve(null) 的 promise 永久缓存：一次网络抖动/超时就把本页永久降级为轻量编辑器。
 *   另：即使本次已超时，若 AMD require 稍后才成功（window.monaco 就位），下次调用会直接复用。
 */
export function loadMonaco(timeoutMs = 25000): Promise<Monaco | null> {
  if (monacoRef) return Promise.resolve(monacoRef)
  if (monacoPromise) return monacoPromise
  const promise = new Promise<Monaco | null>((resolve) => {
    const w = window as unknown as Record<string, unknown>
    let settled = false
    const settle = (m: Monaco | null): void => {
      if (settled) return
      settled = true
      if (m) {
        defineTheme(m)
        // 符号 provider 挂在 Monaco 的全局注册表上：内核就位时装一次，同页所有编辑器（含差异/合并）都能用
        installSymbolProviders(m)
        // 语言服务器 provider 同处挂载：本机没有可用服务器（清单为空 / 探测失败 / GEBAI_LSP=false）时为空操作
        installLspProviders(m)
      }
      monacoRef = m
      if (m) monacoFailed = false
      resolve(m)
      if (m) return
      /* 失败/超时：记下“这次真的没加载出来”（状态栏据此提示降级，见 `monacoLoadFailed`），
         并撤销单例缓存（下次调用可重试）、清掉 loader script（重试时重新插入）。 */
      monacoFailed = true
      queueMicrotask(() => {
        if (monacoPromise === promise) monacoPromise = null
        monacoScript?.remove()
        monacoScript = null
      })
    }
    if (w.monaco) {
      settle(w.monaco as Monaco)
      return
    }
    const vs = monacoVsPath()
    w.MonacoEnvironment = {
      getWorkerUrl: (_moduleId: string, _label: string) => `${vs}/base/worker/workerMain.js`,
    }
    const script = monacoScript ?? document.createElement("script")
    monacoScript = script
    script.src = `${vs}/loader.js`
    script.async = true
    script.onload = () => {
      const requireFn = w.require as ((deps: string[], cb: () => void) => void) | undefined
      try {
        ;(w.require as { config?: (o: unknown) => void }).config?.({ paths: { vs } })
        requireFn?.(["vs/editor/editor.main"], () => {
          settle(((window as unknown as Record<string, unknown>).monaco as Monaco | undefined) ?? null)
        })
      } catch {
        settle(null)
      }
    }
    script.onerror = () => {
      console.warn("[files] Monaco 加载失败（vendor 缺失？），降级为轻量编辑器")
      settle(null)
    }
    if (!script.isConnected) document.head.appendChild(script)
    setTimeout(() => {
      if (!monacoRef) settle(null)
    }, timeoutMs)
  })
  monacoPromise = promise
  return promise
}

/**
 * 首屏就绪后的**空闲预热**（静默，失败与未预热等价）。
 *
 * Monaco 首次加载要拉约 1MB 分块（editor 核心 + 语言 + worker），本地实测约 0.5s、远程更久。
 * 放在 requestIdleCallback（无此 API 时退化为短延时）里预热：不与他人争首屏带宽，
 * 用户首次打开文件时内核已就位，不再等加载。预热失败不影响后续：打开文件时自会重试并可能降级。
 */
export function prewarmMonaco(): void {
  if (monacoRef || monacoPromise) return
  const run = (): void => {
    void loadMonaco().catch(() => undefined)
  }
  const schedule = (): void => {
    const ric = (window as unknown as { requestIdleCallback?: (cb: () => void, opts?: { timeout: number }) => number }).requestIdleCallback
    if (typeof ric === "function") ric(run, { timeout: 2500 })
    else setTimeout(run, 500)
  }
  // 后台标签页（Ctrl+点击开的新标签）：首屏并不在看，先不拉这 1MB，等切到前台再预热
  if (document.hidden) {
    document.addEventListener(
      "visibilitychange",
      () => {
        if (!document.hidden) schedule()
      },
      { once: true },
    )
    return
  }
  schedule()
}

/* --------------------------- 主题映射 --------------------------- */

/**
 * 读取 CSS 令牌的**浏览器计算值**（任意颜色语法 → `rgb()`/`rgba()` 文本）。
 * 令牌值可能是 hsl()/color-mix()/颜色关键字，交给浏览器在探针元素上算一次最稳。
 */
function computedVar(name: string): string {
  const raw = getComputedStyle(document.documentElement).getPropertyValue(name).trim()
  if (!raw) return ""
  const probe = document.createElement("span")
  probe.style.color = raw
  probe.style.position = "absolute"
  probe.style.opacity = "0"
  document.body.appendChild(probe)
  const computed = getComputedStyle(probe).color
  probe.remove()
  return computed
}

function alpha(hex: string, a: number): string {
  const v = Math.round(Math.max(0, Math.min(1, a)) * 255)
    .toString(16)
    .padStart(2, "0")
  return `${hex}${v}`
}

/** 依据当前 CSS 令牌重定义 Monaco 主题（light/dark 同名两套，按当前主题明暗选择）。 */
export function defineTheme(monaco: Monaco): void {
  // 逐层合成出**不透明**实色：页面底（body 计算背景）→ 编辑器底。
  // 编辑器底取**视图面板底**（--bg-elev，与左栏/工具窗/标签栏/状态栏同一层）：编辑器坐在工作台视图里，
  // 两邊同亮度才是一整块；早期用内凹色 --bg-inset（输入框/代码内嵌的语义）比面板暗 4~19 级——
  // 亮色亚克力下就是面板 250 里嵌一块 236 的灰块（用户可见：视图背景与编辑器背景亮度差得有点大）。
  // 与面板同样直接叠在**页面底**上（面板就在透明容器里直接露页面底，叠在 --bg 上会多亮一档）。
  const page = getComputedStyle(document.body).backgroundColor || "#0d1117"
  const pick = (name: string, fallback: string, backdrop: string): string => cssVarToHex(computedVar(name), backdrop, fallback)
  const bg = pick("--bg-elev", pick("--bg", page, page), page)
  const fg = pick("--text", "#e6edf3", bg)
  const muted = pick("--text-muted", "#8b949e", bg)
  const faint = pick("--text-faint", "#6e7681", bg)
  const accent = pick("--accent", "#6366f1", bg)
  const border = pick("--border", "#262b3a", bg)
  const elev = pick("--bg-elev-2", bg, bg) // 浮层/建议框：面板再抬一档（各主题均定义；缺失则与编辑器同底，靠 border 分辨）
  const success = pick("--success", "#3fb950", bg)
  const danger = pick("--danger", "#f85149", bg)
  const warning = pick("--warning", "#d29922", bg)

  // 明暗判定：合成后的编辑器底色亮度（不依赖 data-theme，任何主题/黑白变体都能自适应）
  const rgb = bg.match(/\w\w/g) ?? []
  const lum = rgb.length === 3 ? (parseInt(rgb[0], 16) * 0.299 + parseInt(rgb[1], 16) * 0.587 + parseInt(rgb[2], 16) * 0.114) / 255 : 0
  lightTheme = lum > 0.5

  monaco.editor.defineTheme("gebai", {
    base: lightTheme ? "vs" : "vs-dark",
    inherit: true,
    rules: [
      { token: "", foreground: fg.slice(1), background: bg.slice(1) },
      { token: "comment", foreground: faint.slice(1), fontStyle: "italic" },
      { token: "keyword", foreground: accent.slice(1) },
      { token: "string", foreground: success.slice(1) },
      { token: "number", foreground: warning.slice(1) },
      { token: "regexp", foreground: warning.slice(1) },
      { token: "type", foreground: accent.slice(1) },
      { token: "type.identifier", foreground: accent.slice(1) },
      { token: "function", foreground: muted.slice(1) },
      { token: "variable", foreground: fg.slice(1) },
      { token: "tag", foreground: accent.slice(1) },
      { token: "attribute.name", foreground: warning.slice(1) },
      { token: "delimiter", foreground: muted.slice(1) },
      { token: "invalid", foreground: danger.slice(1) },
    ],
    colors: {
      "editor.background": bg,
      "editor.foreground": fg,
      "editorLineNumber.foreground": faint,
      "editorLineNumber.activeForeground": muted,
      "editorCursor.foreground": accent,
      "editor.selectionBackground": alpha(accent, lightTheme ? 0.22 : 0.3),
      "editor.inactiveSelectionBackground": alpha(accent, 0.14),
      "editor.selectionHighlightBackground": alpha(accent, 0.14),
      "editor.lineHighlightBackground": alpha(muted, lightTheme ? 0.09 : 0.07),
      "editor.lineHighlightBorder": "#00000000",
      "editorIndentGuide.background1": alpha(border, 0.75),
      "editorIndentGuide.activeBackground1": alpha(accent, 0.5),
      "editorBracketMatch.background": alpha(accent, 0.18),
      "editorBracketMatch.border": alpha(accent, 0.5),
      "editorWhitespace.foreground": alpha(faint, 0.5),
      "editorGutter.background": bg,
      "editorWidget.background": elev,
      "editorWidget.border": border,
      "editorSuggestWidget.background": elev,
      "editorSuggestWidget.selectedBackground": alpha(accent, 0.22),
      "editorHoverWidget.background": elev,
      "editorHoverWidget.border": border,
      "input.background": bg,
      "input.border": border,
      "scrollbar.shadow": "#00000000",
      "scrollbarSlider.background": alpha(muted, 0.22),
      "scrollbarSlider.hoverBackground": alpha(muted, 0.36),
      "scrollbarSlider.activeBackground": alpha(accent, 0.5),
      "minimap.background": bg,
      "menu.background": elev,
      "menu.border": border,
      "list.hoverBackground": alpha(muted, 0.12),
      "list.activeSelectionBackground": alpha(accent, 0.24),
      "diffEditor.insertedTextBackground": alpha(success, 0.16),
      "diffEditor.removedTextBackground": alpha(danger, 0.16),
      "diffEditor.insertedLineBackground": alpha(success, 0.09),
      "diffEditor.removedLineBackground": alpha(danger, 0.09),
      "diffEditorGutter.insertedLineBackground": alpha(success, 0.2),
      "diffEditorGutter.removedLineBackground": alpha(danger, 0.2),
    },
  })
}

/** 按当前令牌定义并切到 gebai 主题。 */
function applyGebaiTheme(monaco: Monaco): void {
  defineTheme(monaco)
  monaco.editor.setTheme("gebai")
  currentTheme = "gebai"
}

/**
 * 主题切换时重映射（main.ts 监听 gebai:theme-change / 本地切换）。
 *
 * 为什么不直接同步定义：换肤会带动 body 背景的 CSS 过渡（base.css 的全局过渡名单含 body），
 * 而合成链（页面底 → --bg → --bg-inset）以 body 底色打底——过渡途中取色会把**中间值**
 * 固化成编辑器底色（亮色切回后整块偏暗），且此后不再有事件触发重定义、不会自愈。
 *
 * “两帧底色一致”这种稳定性判据不够：过渡要等样式变更后的下一帧才启动，头两帧读到的
 * 还是旧值（看者“稳定”），所以改用 `getAnimations()` 直接看 body 的 background-color 过渡
 * 是否还在跑（过渡结束才定义）；第一帧只作让位。getAnimations 不可用时按典型过渡时长延时兜底。
 */
export function refreshEditorTheme(): void {
  if (!monacoRef) return
  const monaco = monacoRef
  const apply = (): void => applyGebaiTheme(monaco)
  if (typeof requestAnimationFrame !== "function" || typeof document.body?.getAnimations !== "function") {
    setTimeout(apply, 320)
    return
  }
  const body = document.body
  const deadline = performance.now() + 1000
  let frames = 0
  const transitioning = (): boolean =>
    body.getAnimations().some((a) => a.playState === "running" && (a as CSSTransition).transitionProperty === "background-color")
  const step = (): void => {
    frames++
    if (frames > 1 && !transitioning()) {
      apply()
      return
    }
    if (performance.now() > deadline) {
      apply()
      return
    }
    requestAnimationFrame(step)
  }
  requestAnimationFrame(step)
}

export { currentTheme }

/* --------------------------- 编辑器实现 --------------------------- */

/** 大文件阈值：超过后关闭小地图与高级特性。 */
const LARGE_FILE_CHARS = 1_500_000

/**
 * 选区字符数：按行长度累加算出，**不物化选区文本**。
 *
 * 早期这里用 `model.getValueInRange(sel).length`——选中多少就分配多少字符的字符串：
 * 拖选/全选大文件时每个 mousemove 都要分配一遍（MB 级），是输入与拖选卡顿的元凶之一。
 * 行长度是 Monaco 的行元数据（O(1)），累加只与**选中行数**相关，与字符量无关。
 */
function rangeLength(model: { getLineLength: (n: number) => number }, startLine: number, startColumn: number, endLine: number, endColumn: number): number {
  if (endLine < startLine || (endLine === startLine && endColumn <= startColumn)) return 0
  let len = endLine - startLine // 行间换行
  for (let l = startLine; l <= endLine; l++) len += model.getLineLength(l)
  return Math.max(0, len - (startColumn - 1) - (model.getLineLength(endLine) - endColumn + 1))
}

export async function createEditor(host: HTMLElement, opts: EditorOptions): Promise<EditorHandle> {
  const loaded = await loadMonaco()
  if (!loaded) return createFallbackEditor(host, opts)
  // 字体就绪后再建：Monaco 只在 create 时量一次字符宽度，量在回退字体上会留下「随列号累积」的光标偏移
  //（字体已在缓存里时零等待；超时照常建编辑器，字体落地后由度量自检重测）
  await ensureEditorFont()
  // 取局部非空别名：闭包（侧边列/行尾注释的渲染函数）里 TS 不再保留对 `loaded` 的窄化
  const monaco: Monaco = loaded
  defineTheme(monaco)
  const large = opts.value.length > (opts.largeFileChars ?? LARGE_FILE_CHARS)
  const model = monaco.editor.createModel(opts.value, opts.language)
  /*
   * 编辑器与 blame 侧边列并排（flex）：侧边列**在 Monaco 容器之外**，不挤占也不覆盖代码内容——
   * 这是与「把注释注入行首」的关键区别（那种做法会把每行代码整体右移，看着就像代码里多了一列字）。
   * 列隐藏时 `hidden`（display:none），编辑器自动占满（automaticLayout 跟容器尺寸变化重排）。
   */
  const wrap = document.createElement("div")
  wrap.className = "fw-editor-wrap"
  const blameGutter = document.createElement("div")
  blameGutter.className = "fw-blame-gutter"
  blameGutter.hidden = true
  const blameInner = document.createElement("div")
  blameInner.className = "fw-blame-gutter-inner"
  blameGutter.appendChild(blameInner)
  const edHost = document.createElement("div")
  // 类名用 surface 而不是 host：调用方（files/main.ts）已经有一个 `.fw-editor-host` 作为外容器，
  // 同名会让选择器与样式双重歧义
  edHost.className = "fw-editor-surface"
  wrap.append(blameGutter, edHost)
  host.appendChild(wrap)
  const ed = monaco.editor.create(edHost, {
    model,
    theme: "gebai",
    readOnly: opts.readOnly,
    automaticLayout: true,
    minimap: { enabled: !large && (opts.minimap ?? minimapOn), maxColumn: 90, renderCharacters: false },
    fontFamily: EDITOR_FONT_FAMILY,
    fontSize: EDITOR_FONT_SIZE,
    lineHeight: 20,
    tabSize: 2,
    renderWhitespace: "selection",
    renderLineHighlight: large ? "none" : "all",
    scrollBeyondLastLine: false,
    smoothScrolling: true,
    wordWrap: (opts.wordWrap ?? wrapOn) ? "on" : "off",
    bracketPairColorization: { enabled: !large },
    guides: { bracketPairs: !large, indentation: !large },
    stickyScroll: { enabled: false },
    unicodeHighlight: { ambiguousCharacters: false, invisibleCharacters: false },
    scrollbar: { verticalScrollbarSize: 11, horizontalScrollbarSize: 11, useShadows: false },
    overviewRulerBorder: false,
    contextmenu: true,
    quickSuggestions: !opts.readOnly,
    suggestOnTriggerCharacters: !opts.readOnly,
    formatOnPaste: !opts.readOnly,
    padding: { top: 8, bottom: 24 },
    fixedOverflowWidgets: true,
    // 本会话自检判过「缓存宽度 ≠ 实绘宽度」时，新建编辑器直接带上关闭项（见 editor-metrics）
    ...(monoFastPathDisabled() ? { disableMonospaceOptimizations: true } : {}),
  })
  // 字体后到 / 像素比变化后的度量复检（首次自检：确认缓存宽度与实绘一致）
  const metrics = createMetricsSync(monaco, () => [ed])
  ed.onDidLayoutChange(() => metrics.check())
  /** 编辑器句柄的类型别名（闭包内用，避免为了窄化再重复断言）。 */
  type DecoCollection = ReturnType<typeof ed.createDecorationsCollection>

  /* ---------- 行内 blame：侧边列 + 光标行行尾 ---------- */

  /** 行号 → blame 条目；null = 没有数据（两态都画不出东西）。 */
  let blameIndex: Map<number, BlameLine> | null = null
  /** 两种显示形态的开关（**互不影响**：可以只开行尾、只开侧边列、或都开）。 */
  let gutterOn = false
  let inlineOn = false
  /** 侧边列的行节点池（滚动时逐帧复用，不重建 DOM）。 */
  const blameRows: HTMLElement[] = []
  let blameRaf = 0
  /** 光标行行尾注释（单独一个集合：只随光标移动更新一行）。 */
  let cursorBlame: DecoCollection | null = null

  /**
   * 重画侧边列：只渲染**当前可见行**（含折行时按行遍历）。
   * 位置用 `getScrolledVisiblePosition`（相对编辑器视口的 y），折行/自适应行高都对得上；
   * 它内部就是逐行几何，不用自己假定行高（CSS 行高与 Monaco 保持一致的口径留给样式表）。
   */
  function paintBlame(): void {
    blameRaf = 0
    if (!gutterOn || !blameIndex) return
    let i = 0
    for (const r of ed.getVisibleRanges()) {
      for (let line = r.startLineNumber; line <= r.endLineNumber; line++) {
        const info = blameIndex.get(line)
        if (!info) continue
        const pos = ed.getScrolledVisiblePosition({ lineNumber: line, column: 1 })
        if (!pos) continue
        const row = blameRows[i] ?? (blameRows[i] = document.createElement("div"))
        if (!row.isConnected) {
          row.className = "fw-blame-row"
          blameInner.appendChild(row)
        }
        i++
        row.hidden = false
        row.style.top = `${pos.top}px`
        row.style.height = `${pos.height}px`
        row.textContent = blameLabel(info)
        row.classList.toggle("is-uncommitted", info.uncommitted)
        row.dataset.tip = blameHover(info)
      }
    }
    for (; i < blameRows.length; i++) blameRows[i]!.hidden = true
  }

  function scheduleBlame(): void {
    if (!gutterOn || !blameIndex || blameRaf) return
    blameRaf = requestAnimationFrame(paintBlame)
  }

  /** 光标行行尾注释：只在光标所在行显示，样式比侧边列更弱（当前行的视线内提示）。 */
  function updateCursorBlame(): void {
    const pos = inlineOn ? ed.getPosition() : null
    const info = blameIndex && pos ? blameIndex.get(pos.lineNumber) : undefined
    if (!info || !pos) {
      cursorBlame?.clear()
      return
    }
    const range = new monaco.Range(pos.lineNumber, model.getLineMaxColumn(pos.lineNumber), pos.lineNumber, model.getLineMaxColumn(pos.lineNumber))
    const deco = {
      range,
      options: {
        description: "git-blame-eol",
        showIfCollapsed: true,
        after: {
          content: `  ${blameLabel(info)}`,
          inlineClassName: info.uncommitted ? "fw-blame-eol is-uncommitted" : "fw-blame-eol",
          /* 光标不在注释里停下（默认 Both 会让方向键卡在这段注入文本上）——它不是内容，只是批注 */
          cursorStops: monaco.editor.InjectedTextCursorStops?.None ?? null,
        },
      },
    }
    if (cursorBlame) cursorBlame.set([deco])
    else cursorBlame = ed.createDecorationsCollection([deco])
  }

  const blameSubs = [
    ed.onDidScrollChange(() => scheduleBlame()),
    ed.onDidLayoutChange(() => scheduleBlame()),
    ed.onDidChangeModelContent(() => {
      scheduleBlame()
      updateCursorBlame()
    }),
    ed.onDidChangeCursorPosition(() => updateCursorBlame()),
  ]
  /** 选区长度（同范围复用上次结果：光标事件与选区事件都会问一次，拖选时每个事件都要算） */
  let selKey = ""
  let selLen = 0
  const selectionLength = (): number => {
    const sel = ed.getSelection()
    if (!sel || sel.isEmpty()) {
      selKey = ""
      selLen = 0
      return 0
    }
    const key = `${sel.startLineNumber}:${sel.startColumn}-${sel.endLineNumber}:${sel.endColumn}`
    if (key !== selKey) {
      selKey = key
      selLen = rangeLength(model, sel.startLineNumber, sel.startColumn, sel.endLineNumber, sel.endColumn)
    }
    return selLen
  }

  /* ---------- 右键菜单：复制路径 / 发送会话 ---------- */

  /** 菜单项的回收句柄（Monaco 的全局菜单注册 + 降级实现的自绘菜单共用一份口径）。 */
  let menuDisposables: { dispose(): void }[] = []

  /**
   * 当前选区（右键菜单取一次快照）。
   *
   * **无选中时给光标所在整行**：右键“发送会话”却不选中任何东西是很常见的手势（想在光标这一行提问），
   * 这时给个空的代码块等于白点一下——整行是这个场景下最有用的默认粒度。
   */
  const selectionNow = (): EditorSelection | null => {
    const sel = ed.getSelection()
    const pos = ed.getPosition()
    if (!sel && !pos) return null
    const raw = sel ?? { startLineNumber: pos!.lineNumber, startColumn: pos!.column, endLineNumber: pos!.lineNumber, endColumn: pos!.column }
    const hasSelection = !!sel && !sel.isEmpty()
    const range = hasSelection
      ? normalizeLineRange({ startLine: raw.startLineNumber, startColumn: raw.startColumn, endLine: raw.endLineNumber, endColumn: raw.endColumn })
      : { startLine: pos?.lineNumber ?? raw.startLineNumber, endLine: pos?.lineNumber ?? raw.endLineNumber }
    const text = hasSelection ? model.getValueInRange(sel!) : model.getLineContent(range.startLine)
    const absPath = absPathOf()
    return {
      range,
      text,
      hasSelection,
      // 没有绝对路径时只能给行号后缀（"：12" 看起来像坏掉的路径，故前面补一个占位名）
      ref: absPath ? formatAbsRef(absPath, range) : `当前文件${formatAbsRef("", range)}`,
      language: model.getLanguageId(),
    }
  }

  /** 绝对路径（允许传函数：根清单/仓库根可能晚于编辑器建立到达，每次取时现算最稳）。 */
  function absPathOf(): string {
    const v = opts.menu?.absPath
    return typeof v === "function" ? v() : (v ?? "")
  }

  /**
   * 把宿主给的项注册进 Monaco 的编辑器右键菜单（`EditorContext`）：内置两项（复制路径 / 发送会话）
   * + `EditorMenuHooks.groups` 的自定义组（如工作台的 Git 历史）。
   *
   * 三个容易错的地方：
   * ① **必须自己 dispose**——`addAction` 返回的句柄会往**全局**菜单注册表里加一条（
   *    带 `editorId` 前提，只在本编辑器弹），而编辑器自己的 `dispose()` 只清它内部的 action 表、
   *    **不动那份注册**。本页每个文件标签各建一个编辑器（切查看/编辑态、重新加载还会重建），
   *    不手回收就是“每重建一次，右键菜单里多一组重项”（实测已验）。
   * ② **分组名** `gebai` 落在 `9_cutcopypaste` 与 `navigation` 之间（菜单分组按名字典序，
   *    而 `navigation` 被硬编码在最前）——即“剪切/复制/粘贴之后、转到定义之前”，正是这两项该在的位置。
   *    自定义组取名 `gebai.<id>`：字典序排在 `gebai` 之后，于是它们与这两项之间**自带一条组间
   *    分隔线**（菜单按组画分隔线，组名相同才并成一组）。
   * ③ **可重装**：菜单项的标题在注册那一刻定下，文案带状态的项（`✓ 行尾溯源`）状态一变就得
   *    重装一次——所以进来先摘掉上一轮（见 `refreshMenu`）。
   */
  function installMenuActions(handle: EditorHandle): void {
    for (const d of menuDisposables) d.dispose()
    menuDisposables = []
    const disposables: { dispose(): void }[] = []
    // 有 hook 就注册（而**不是**看当前能不能算出绝对路径）：路径是取时现算的，
    // 拿不到时降级为「当前文件:12」也比“菜单项整个不在”好排查。
    if (opts.menu?.absPath) {
      disposables.push(
        ed.addAction({
          id: "gebai.copyAbsPath",
          label: "复制路径",
          contextMenuGroupId: "gebai",
          contextMenuOrder: 1,
          run: () => {
            const sel = selectionNow()
            if (!sel) return
            void navigator.clipboard.writeText(sel.ref).then(
              () => toast(`已复制 ${sel.ref}`, "success"),
              () => toast("复制失败：剪贴板不可用", "error"),
            )
          },
        }),
      )
      const send = opts.menu.sendToChat
      if (send) {
        disposables.push(
          ed.addAction({
            id: "gebai.sendToChat",
            label: "发送会话",
            contextMenuGroupId: "gebai",
            contextMenuOrder: 2,
            run: () => {
              const sel = selectionNow()
              if (!sel) return
              send({ ...sel, markdown: buildChatSnippet({ absPath: absPathOf(), range: sel.range, text: sel.text, language: sel.language }) })
            },
          }),
        )
      }
    }
    // 宿主自定义组（如 Git 历史）：文案、可用性、组内顺序全由宿主定，这里只负责落位
    for (const g of opts.menu?.groups?.(handle) ?? []) {
      g.items.forEach((it, i) => {
        disposables.push(
          ed.addAction({
            id: `gebai.${g.id}.${it.id}`,
            label: it.label,
            contextMenuGroupId: `gebai.${g.id}`,
            contextMenuOrder: i + 1,
            run: () => it.run(),
          }),
        )
      })
    }
    menuDisposables = disposables
  }

  const handle: EditorHandle = {
    kind: "monaco",
    model: () => model,
    selection: selectionNow,
    getValue: () => model.getValue(),
    setValue: (v) => {
      model.setValue(v)
    },
    setLanguage: (lang) => {
      monaco.editor.setModelLanguage(model, lang || "plaintext")
    },
    setReadOnly: (ro) => ed.updateOptions({ readOnly: ro }),
    isReadOnly: () => ed.getOption(monaco.editor.EditorOption.readOnly),
    setWordWrap: (on) => ed.updateOptions({ wordWrap: on ? "on" : "off" }),
    // 大文件仍不给开（`!large` 是性能取舍，偏好不覆盖它）
    setMinimap: (on) => ed.updateOptions({ minimap: { enabled: on && !large } }),
    focus: () => ed.focus(),
    layout: () => ed.layout(),
    revealLine: (line, column = 1) => {
      ed.revealLineInCenter(line + 1)
      ed.setPosition({ lineNumber: line + 1, column })
      ed.focus()
    },
    getScrollTop: () => ed.getScrollTop(),
    setScrollTop: (top) => ed.setScrollTop(top),
    getCursorPos: () => {
      const pos = ed.getPosition()
      return { line: pos?.lineNumber ?? 1, column: pos?.column ?? 1 }
    },
    setCursorPos: (pos) => ed.setPosition({ lineNumber: pos.line, column: pos.column }),
    getCursor: () => {
      const pos = ed.getPosition()
      return { line: pos?.lineNumber ?? 1, column: pos?.column ?? 1, selected: selectionLength() }
    },
    onCursor: (cb) => {
      ed.onDidChangeCursorPosition((e) => {
        cb({ line: e.position.lineNumber, column: e.position.column, selected: selectionLength() })
      })
      ed.onDidChangeCursorSelection((e) => {
        cb({ line: e.selection.positionLineNumber, column: e.selection.positionColumn, selected: selectionLength() })
      })
    },
    onChange: (cb) => {
      ed.onDidChangeModelContent(() => cb())
    },
    markClean: () => {
      /* Monaco 无需额外处理：脏标记由上层按内容比对维护 */
    },
    listSymbols: () => flatSymbolsOf(model),
    supportsSymbols: () => canExtract(model.getLanguageId()) || hasLsp(model.getLanguageId()),
    symbolSource: () => symbolSourceOf(model),
    showOutline: () => {
      const action = ed.getAction("editor.action.quickOutline")
      if (!action) return false
      void action.run()
      return true
    },
    setBlame: (lines, show) => {
      /*
       * 两种形态互相独立（两个按钮各自开关）：
       * ① **侧边列**（`gutter`）——编辑器左侧独立一列，与代码内容分开（在 Monaco 容器之外，不挤占也不覆盖）；
       *    位置用 `getScrolledVisiblePosition` 随滚动/折行按帧重算，只渲染可见行。
       * ② **光标行行尾**（`inline`）——`after` 注入到光标所在行行尾，样式更弱；光标移动只更新这一个集合。
       *
       * 两处都是**装饰/注入文本，不进模型**：复制、保存、撤销拿到的都是原文，不会被 blame 污染。
       * 注入文本挂在**空 range** 上时必须显式 `showIfCollapsed: true`——Monaco 取注入文本会
       * 按 `showIfCollapsed || !range.isEmpty()` 过滤空 range，不给这个标记就是静默丢弃。
       */
      blameIndex = lines.length ? toBlameIndex(lines) : null
      gutterOn = show.gutter && !!blameIndex
      inlineOn = show.inline && !!blameIndex
      blameGutter.hidden = !gutterOn
      if (!gutterOn) {
        for (const row of blameRows) row.remove()
        blameRows.length = 0
        if (blameRaf) cancelAnimationFrame(blameRaf)
        blameRaf = 0
      }
      // 侧边列显隐改变编辑器可用宽度：先 layout（会触发 onDidLayoutChange → 重画可见行）
      ed.layout()
      paintBlame()
      updateCursorBlame()
    },
    refreshMenu: () => installMenuActions(handle),
    dispose: () => {
      wrapTargets.delete(handle)
      minimapTargets.delete(handle)
      menuPrune.dispose()
      metrics.dispose()
      // 菜单项是**全局注册表**里的条目（见 installMenuActions）：不在这里回收，重建编辑器就多一组重项
      for (const d of menuDisposables) d.dispose()
      menuDisposables = []
      cursorBlame?.clear()
      for (const sub of blameSubs) sub.dispose()
      if (blameRaf) cancelAnimationFrame(blameRaf)
      ed.dispose()
      model.dispose()
      wrap.remove()
    },
  }
  // 句柄齐了才装菜单：自定义组的文案/可用性按**编辑器自身能力**定（见 EditorMenuHooks.groups）
  installMenuActions(handle)
  wrapTargets.add(handle)
  minimapTargets.add(handle)
  // 剪掉 Monaco 自带、工作台用不到的右键菜单条目（Peek / Copy / Command Palette，见 context-menu.ts）
  const menuPrune = installContextMenuPrune(ed.getDomNode() ?? edHost)
  return handle
}

/** 降级编辑器：只读用 highlight.js 静态高亮，编辑用 textarea（功能对齐，体验降级）。 */
async function createFallbackEditor(host: HTMLElement, opts: EditorOptions): Promise<EditorHandle> {
  const wrap = document.createElement("div")
  wrap.className = "fw-fallback"
  // 自动换行在降级实现里只能靠 CSS（textarea 没有 Monaco 的选项）：类切换即生效
  wrap.classList.toggle("fw-wrap", wrapOn)
  const pre = document.createElement("pre")
  pre.className = "fw-fallback-code hljs"
  const area = document.createElement("textarea")
  area.className = "fw-fallback-edit"
  area.spellcheck = false
  area.value = opts.value
  let readOnly = opts.readOnly
  // 语言会随标签切换而变（上层调 setLanguage）：高亮与符号提取都读这个变量，而不是 opts.language
  let lang = opts.language
  if (readOnly) area.style.display = "none"
  else pre.style.display = "none"
  wrap.appendChild(pre)
  wrap.appendChild(area)
  host.appendChild(wrap)

  const absPathOf = (): string => {
    const v = opts.menu?.absPath
    return typeof v === "function" ? v() : (v ?? "")
  }

  /**
   * 当前选区。两态取法不同，因为**两态下用户能选中的东西不是同一个**：
   *
   * - **编辑态**（textarea 可见）：光标/选区就是 textarea 的 `selectionStart/End`——真实且直接。
   *   无选中时给光标所在整行（与 Monaco 那份同语义）。
   * - **只读态**（高亮后的 `pre` 可见，textarea 被 `display:none`）：此时 textarea 里的
   *   `selectionStart` 停在**文档末尾**（JS 赋值后光标就在末尾），拿它当“当前行”会得到**最后一行**——
   *   一个与用户看到的完全无关的位置（实测：右键第一行，复制出来是 `:140`）。只读态的真实交互是在 `pre` 上
   *   用鼠标选，所以改从 **DOM 选区**算：highlight.js 只给文本套标签、**不增删字符**，把 DOM 里的点
   *   按文本节点累加成字符偏移即可精确对应到源码（行号才能算对）。
   *   只读态**没有光标**，因此无选区时不给行号（`range: null`，复制得到的就是纯路径）。
   */
  const lineOf = (offset: number): number => area.value.slice(0, offset).split("\n").length
  const columnOf = (offset: number): number => offset - area.value.lastIndexOf("\n", offset - 1)

  /** 字符偏移 → 光标位置（切标签记位置用，与 `lineOf`/`columnOf` 同一口径）。 */
  const posOfOffset = (offset: number): { line: number; column: number } => ({ line: lineOf(offset), column: columnOf(offset) })
  /** 光标位置 → 字符偏移（列超过该行长度时落到行尾，不越到下一行）。 */
  const offsetOfPos = (pos: { line: number; column: number }): number => {
    const lines = area.value.split("\n")
    const line = Math.min(Math.max(pos.line, 1), lines.length)
    let offset = 0
    for (let i = 0; i < line - 1; i++) offset += lines[i].length + 1
    return offset + Math.min(Math.max(pos.column - 1, 0), lines[line - 1].length)
  }

  /** DOM 里的点（节点 + 偏移）→ `pre` 内的字符偏移（无匹配返回 null）。 */
  const offsetInPre = (node: Node, offset: number): number | null => {
    let total = 0
    const textLen = (n: Node): number => n.textContent?.length ?? 0
    /** 返回配中点的字符偏移；未配中时把途经的文本长度累进 total。 */
    const walk = (n: Node): number | null => {
      if (n === node) {
        // 文本节点：偏移直接加到已累计的长度上
        if (n.nodeType === Node.TEXT_NODE) return total + offset
        // 元素节点：offset 是**子节点下标**，取它之前那些子节点的文本长度（不看子节点内部）
        let local = 0
        for (const k of Array.from(n.childNodes).slice(0, offset)) local += textLen(k)
        return total + local
      }
      if (n.nodeType === Node.TEXT_NODE) {
        total += textLen(n)
        return null
      }
      for (const c of Array.from(n.childNodes)) {
        const hit = walk(c)
        if (hit !== null) return hit
      }
      return null
    }
    return walk(pre)
  }

  /** 只读态下用户在高亮文本上的选区 → 字符偏移（没有选区返回 null）。 */
  const domOffsets = (): { start: number; end: number } | null => {
    const s = window.getSelection()
    if (!s || s.isCollapsed || s.rangeCount === 0) return null
    const r = s.getRangeAt(0)
    if (!pre.contains(r.startContainer) || !pre.contains(r.endContainer)) return null
    const a = offsetInPre(r.startContainer, r.startOffset)
    const b = offsetInPre(r.endContainer, r.endOffset)
    if (a === null || b === null) return null
    const start = Math.min(a, b)
    const end = Math.max(a, b)
    return end > start ? { start, end } : null
  }

  const selectionNow = (): EditorSelection | null => {
    const ro = readOnly && pre.isConnected
    let start = 0
    let end = 0
    let hasSelection = false
    if (ro) {
      const off = domOffsets()
      if (off) {
        start = off.start
        end = off.end
        hasSelection = true
      }
    } else {
      start = Math.min(area.selectionStart, area.selectionEnd)
      end = Math.max(area.selectionStart, area.selectionEnd)
      hasSelection = end > start
    }
    const range = hasSelection
      ? normalizeLineRange({ startLine: lineOf(start), startColumn: columnOf(start), endLine: lineOf(end), endColumn: columnOf(end) })
      : ro
        ? null // 只读态没有光标：不给行号（比报一个“最后一行”诚实）
        : { startLine: lineOf(area.selectionStart), endLine: lineOf(area.selectionStart) }
    const text = hasSelection ? area.value.slice(start, end) : range ? (area.value.split("\n")[range.startLine - 1] ?? "") : ""
    const absPath = absPathOf()
    return {
      range,
      text,
      hasSelection,
      ref: absPath ? formatAbsRef(absPath, range) : `当前文件${formatAbsRef("", range)}`,
      language: lang,
    }
  }

  /* 降级编辑器没有 Monaco 的右键菜单（原生菜单又被全站屏蔽），这几项得自绘一份：
     否则降级部署下右键编辑器就什么都没有（连“复制路径”都指不到）。
     挂在容器而非 pre/textarea 上：两态互切会换可见元素（见 setReadOnly），绑到具体元素上会在切态后失效。
     只放这几项——文本的剪切/复制/粘贴键位照旧，不在这里重做一套编辑菜单。 */
  wrap.addEventListener("contextmenu", (ev) => {
    const e = ev as MouseEvent
    e.preventDefault()
    const sel = selectionNow()
    const items: MenuItem[] = []
    if (opts.menu?.absPath) {
      items.push({ label: "复制路径", icon: "copy", onClick: () => void navigator.clipboard.writeText(sel?.ref ?? "").then(() => toast(`已复制 ${sel?.ref ?? ""}`, "success")) })
    }
    const send = opts.menu?.sendToChat
    // 无内容可发时不摆这一项（只读降级态又没选任何东西时：发个空代码块没有意义）
    if (send && sel && sel.text) {
      items.push({
        label: "发送会话",
        icon: "send",
        onClick: () => send({ ...sel, markdown: buildChatSnippet({ absPath: absPathOf(), range: sel.range, text: sel.text, language: sel.language }) }),
      })
    }
    // 宿主自定义组（如 Git 历史）：与 Monaco 那份同一口径，自成一段（分隔线起头）
    for (const g of opts.menu?.groups?.(handle) ?? []) {
      if (!g.items.length) continue
      items.push({ separator: true })
      for (const it of g.items) items.push({ label: it.label, onClick: it.run })
    }
    if (items.length) showMenu(e.clientX, e.clientY, items)
  })

  const render = async () => {
    if (!readOnly) return
    try {
      const mod = (await import("highlight.js/lib/common")) as unknown as { default: { highlight: (c: string, o: { language: string }) => { value: string } } }
      const res = mod.default.highlight(area.value, { language: lang === "plaintext" ? "plaintext" : lang })
      pre.innerHTML = res.value
    } catch {
      pre.textContent = area.value
    }
  }
  await render()
  let cursorCb: CursorListener | null = null
  area.addEventListener("keyup", () => {
    if (!cursorCb) return
    const upto = area.value.slice(0, area.selectionStart)
    const lines = upto.split("\n")
    cursorCb({ line: lines.length, column: lines[lines.length - 1].length + 1, selected: Math.abs(area.selectionEnd - area.selectionStart) })
  })
  const handle: EditorHandle = {
    kind: "fallback",
    model: () => null,
    selection: selectionNow,
    getValue: () => area.value,
    setValue: (v) => {
      area.value = v
      void render()
    },
    setLanguage: (next) => {
      lang = next
      void render()
    },
    setReadOnly: (ro) => {
      readOnly = ro
      area.readOnly = ro
      pre.style.display = ro ? "" : "none"
      area.style.display = ro ? "none" : ""
      void render()
    },
    isReadOnly: () => area.readOnly,
    setWordWrap: (on) => wrap.classList.toggle("fw-wrap", on),
    focus: () => area.focus(),
    layout: () => {},
    revealLine: (line) => {
      const lines = area.value.split("\n")
      const before = lines.slice(0, line).join("\n").length
      area.setSelectionRange(before, before)
      area.scrollTop = Math.max(0, (line - 6) * 20)
    },
    getScrollTop: () => area.scrollTop,
    setScrollTop: (top) => {
      area.scrollTop = top
    },
    // 降级编辑器（大文件 / 弃用高亮路径）没有光标事件，但 textarea 的 selectionStart 一直在：
    // 从中反推行/列即可——这只有切标签时算一次，不值得为它加一套监听。
    getCursorPos: () => posOfOffset(area.selectionStart),
    setCursorPos: (pos) => {
      const offset = offsetOfPos(pos)
      area.setSelectionRange(offset, offset)
    },
    getCursor: () => ({ line: 1, column: 1, selected: 0 }),
    onCursor: (cb) => {
      cursorCb = cb
    },
    onChange: () => {
      /* 降级模式由上层 input 事件驱动 */
    },
    markClean: () => {},
    setBlame: () => {},
    // 降级实现（highlight.js / textarea）没有小地图：空实现，也不进 minimapTargets 注册表
    setMinimap: () => {},
    // 降级实现的自绘菜单每次弹出前现取（见上面的 contextmenu），没有需要重装的东西
    refreshMenu: () => {},
    listSymbols: async () => flattenSymbols((await extractSymbolsAsync(area.value, lang)).symbols),
    supportsSymbols: () => canExtract(lang),
    symbolSource: async () => (await extractSymbolsAsync(area.value, lang)).source,
    showOutline: () => false,
    dispose: () => {
      wrapTargets.delete(handle)
      wrap.remove()
    },
  }
  wrapTargets.add(handle)
  return handle
}

/* --------------------------- 差异视图 --------------------------- */

export interface DiffOptions {
  original: string
  modified: string
  language: string
  /** 并列 / 行内 */
  inline?: boolean
}

export async function createDiffEditor(host: HTMLElement, opts: DiffOptions): Promise<DiffHandle> {
  const monaco = await loadMonaco()
  if (!monaco) {
    const pre = document.createElement("pre")
    pre.className = "fw-fallback-code"
    pre.textContent = `--- 原\n${opts.original}\n\n+++ 改\n${opts.modified}`
    host.appendChild(pre)
    return { kind: "fallback", layout: () => {}, setWordWrap: () => {}, dispose: () => pre.remove() }
  }
  defineTheme(monaco)
  // 同 createEditor：字符宽度只在 create 时量一次，先等字体就绪（差异视图两侧共用同一测量口径）
  await ensureEditorFont()
  const original = monaco.editor.createModel(opts.original, opts.language)
  const modified = monaco.editor.createModel(opts.modified, opts.language)
  const ed = monaco.editor.createDiffEditor(host, {
    theme: "gebai",
    readOnly: true,
    wordWrap: wrapOn ? "on" : "off",
    automaticLayout: true,
    renderSideBySide: !opts.inline,
    // 右侧概览尺 = 「差异都在哪」的地图（配合上一处/下一处按钮，是这个视图的核心导航手段）。
    // 普通编辑器关闭它保持干净；差异视图正需要它。
    renderOverviewRuler: true,
    ignoreTrimWhitespace: false,
    fontFamily: EDITOR_FONT_FAMILY,
    fontSize: EDITOR_FONT_SIZE,
    lineHeight: 20,
    scrollBeyondLastLine: false,
    minimap: { enabled: false },
    renderLineHighlight: "none",
    padding: { top: 8, bottom: 16 },
    fixedOverflowWidgets: true,
    scrollbar: { verticalScrollbarSize: 11, horizontalScrollbarSize: 11, useShadows: false },
    // 本会话自检判过「缓存宽度 ≠ 实绘宽度」时，新建编辑器直接带上关闭项（见 editor-metrics）
    ...(monoFastPathDisabled() ? { disableMonospaceOptimizations: true } : {}),
  })
  ed.setModel({ original, modified })
  // 字体后到 / 像素比变化后的度量复检（差异视图两个内层编辑器一起校准）
  const metrics: MetricsSync = createMetricsSync(monaco, () => [ed.getModifiedEditor(), ed.getOriginalEditor()])
  ed.getModifiedEditor().onDidLayoutChange(() => metrics.check())

  /* ---- 差异块导航 ----
   * Monaco 不直接提供 goToNextDiff，但 getLineChanges() 给出全部差异块
   *（原/改两侧的行号区间），据此自己跳。两个关键取舍：
   *  1. 「当前块」以**视口顶部**判定而不是内部游标：用户滚到哪里，计数就显示哪一块
   *     （与 VSCode 的 diff 导航一致）；
   *  2. 纯删除块在 modified 侧行号为 0，此时改在 original 侧定位——
   *     两侧滚动是同步的（diff editor 自带同步滚动），露一边两边都会跟。
   */
  let idx = -1
  const navCbs = new Set<(s: { index: number; total: number }) => void>()
  const changes = (): Array<{ originalStartLineNumber: number; originalEndLineNumber: number; modifiedStartLineNumber: number; modifiedEndLineNumber: number }> =>
    (ed.getLineChanges() ?? []) as never
  /** 差异块清单缓存（滚动事件每帧都问；onDidUpdateDiff 时失效）。 */
  let changeList: ReturnType<typeof changes> | null = null
  const list = (): ReturnType<typeof changes> => (changeList ??= changes())

  /** 视口顶部所在/之后的第一个差异块（都与视口无关时为最后一块）。按 modifiedEndLineNumber 单调递增做二分。 */
  function currentIndex(): number {
    const cs = list()
    if (!cs.length) return -1
    const range = ed.getModifiedEditor().getVisibleRanges()[0]
    const top = range ? range.startLineNumber : 1
    let lo = 0
    let hi = cs.length - 1
    while (lo < hi) {
      const mid = (lo + hi) >> 1
      if (cs[mid].modifiedEndLineNumber >= top) hi = mid
      else lo = mid + 1
    }
    return cs[lo].modifiedEndLineNumber >= top ? lo : cs.length - 1
  }

  function emit(): void {
    const total = list().length
    const s = { index: total ? currentIndex() + 1 : 0, total }
    for (const cb of navCbs) cb(s)
  }

  function reveal(i: number): void {
    const cs = list()
    const c = cs[i]
    if (!c) return
    idx = i
    const me = ed.getModifiedEditor()
    const oe = ed.getOriginalEditor()
    const hasMod = c.modifiedStartLineNumber >= 1
    const target = hasMod ? me : oe
    const start = hasMod ? c.modifiedStartLineNumber : c.originalStartLineNumber
    const end = hasMod ? Math.max(c.modifiedEndLineNumber, c.modifiedStartLineNumber) : Math.max(c.originalEndLineNumber, c.originalStartLineNumber)
    // 选中整块：目标一眼可见（仅 revealLine 时，块很长也不好认）。
    // 末列取该行**最大列**——单行变更若用 col 1 → col 1 是零宽选区（等于一个光标），
    // 编辑器不会画任何选区高亮，"跳过去了"就看不出来。
    const model = target.getModel()
    target.setSelection({ startLineNumber: start, startColumn: 1, endLineNumber: end, endColumn: model ? model.getLineMaxColumn(end) : 1 })
    target.revealLineInCenterIfOutsideViewport(start)
    // Monaco 只在**获焦**时画强选区高亮；顺便让后续按键（方向键、Ctrl+F）落到差异视图上。
    // 不抢表单焦点：在提交框/搜索框里打字时按 F7/Shift+F7，不应把光标拽走。
    const active = document.activeElement as HTMLElement | null
    const inField = !!active && (active.tagName === "INPUT" || active.tagName === "TEXTAREA" || active.isContentEditable)
    if (!inField) target.focus()
    emit()
  }

  /** 从当前块出发到下一（dir=1）/ 上一（dir=-1）处；到头**回卷**（连点不会没反馈地卡住，同 IDEA）。 */
  function step(dir: 1 | -1): void {
    const cs = list()
    if (!cs.length) return
    const base = idx < 0 ? (dir === 1 ? -1 : 0) : idx
    const next = base + dir
    reveal(next < 0 ? cs.length - 1 : next >= cs.length ? 0 : next)
  }

  // 滚动 / 差异重算后刷新计数（滚动事件每帧都会回调：合并到一帧再算，计数 DOM 也随之少刷）
  let scrollRaf = 0
  const sub = ed.getModifiedEditor().onDidScrollChange(() => {
    if (scrollRaf) return
    scrollRaf = requestAnimationFrame(() => {
      scrollRaf = 0
      if (idx >= 0) idx = currentIndex()
      emit()
    })
  })
  const diffSub = ed.onDidUpdateDiff(() => {
    changeList = null
    idx = -1
    emit()
  })
  emit()

  const nav: DiffNav = {
    next: () => step(1),
    prev: () => step(-1),
    state: () => ({ index: list().length ? Math.max(1, (idx < 0 ? currentIndex() : idx) + 1) : 0, total: list().length }),
    onChange: (cb) => {
      navCbs.add(cb)
      cb(nav.state())
      return () => navCbs.delete(cb)
    },
  }

  /* 键盘：差异导航（F7/Shift+F7 跳差异块、F8/Shift+F8 跨文件与冲突）由工作台键位表在
   * document 的**捕获阶段**接管（files/main.ts 的 wb.diffPrev/Next、wb.issuePrev/Next）。
   * 必须捕获——Monaco 的 diff editor 内置了 F7/Shift+F7（diffReview）并会 stopPropagation，
   * 冒泡阶段根本收不到；document 捕获又早于 Monaco 自己的 keybinding 服务。 */
  const handle: DiffHandle = {
    kind: "monaco",
    nav,
    layout: () => ed.layout(),
    // 差异两侧一起切：只改一侧会变成“一边折行、一边横滚”
    setWordWrap: (on) => {
      const v: "on" | "off" = on ? "on" : "off"
      ed.getOriginalEditor().updateOptions({ wordWrap: v })
      ed.getModifiedEditor().updateOptions({ wordWrap: v })
    },
    // 差异视图恒无小地图（见 minimapOn 的说明）：开关对它无效，也不进 minimapTargets
    dispose: () => {
      wrapTargets.delete(handle)
      metrics.dispose()
      sub.dispose()
      diffSub.dispose()
      ed.dispose()
      original.dispose()
      modified.dispose()
    },
  }
  wrapTargets.add(handle)
  // 构造项在部分内核版本下不透传给两侧子编辑器：以子编辑器为准对齐一次，保证初始态一致
  if (wrapOn) handle.setWordWrap(true)
  return handle
}

/** 是否已在当前页面加载出 Monaco（用于状态栏提示与测试）。 */
export function monacoReady(): boolean {
  return !!monacoRef
}

/**
 * Monaco 是否**真的加载失败/超时**（`loadMonaco()` 返回 null 时为真）。
 *
 * 为何要跟 `monacoReady()` 分开：后者问的是“**已经**加载好了吗”，没加载好包含两种情况——
 * “还在预热（1MB 脚本正在下）”与“vendor 缺失/加载失败”。状态栏原先拿 `!monacoReady()` 当降级依据，
 * 于是**预热那几十毫秒~几秒里会闪一个「轻量模式」**（实测：只打开目录根、还没打开任何文件时，
 * 状态栏就挂着「轻量模式」）——用户看到会以为 Monaco 坏了。现在提示只看这个标志：没加载完就什么都不说。
 * 失败后再次调用 `loadMonaco()` 成功会把它清回 false（重试可用）。
 */
export function monacoLoadFailed(): boolean {
  return monacoFailed
}
