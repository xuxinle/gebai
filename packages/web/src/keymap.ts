/**
 * 键盘快捷键的**唯一声明源**与分发器：键位规范化、匹配、焦点守卫、浮层层级、冲突校验、帮助数据。
 *
 * 为什么集中：此前每个模块各自 `addEventListener("keydown")`（主界面 25 处、工作台 24 处），
 * 谁也说不清同一个手势被谁抢走——终端里按 Ctrl+W 顺手关掉标签、一次 Esc 同时关掉一排浮层、
 * Alt+↓ 的含义随差异标签里的文件数变化，都是「分散登记」的必然结果。一张表 + 一个分发器之后：
 * 键位只有一处可写、重复与浏览器冲突在测试里直接报错（`validateKeymap`）、帮助 UI 与文档由它生成。
 *
 * 键位策略：**一套常用键，在所有形态下都成立**。`Ctrl+S` 保存、`Ctrl+P` 快速打开、`F5` 刷新、
 * `Alt+N` 新会话——浏览器里能接管的就接管（Chromium 的按键先到页面，依据见 `browserConflict()`）。
 *
 * 硬约束：**表内零保留键**。浏览器自己处理掉的组合（`Ctrl+N/T/W`、`Ctrl+Tab`…）页面收不到事件，
 * 登记它们等于做出一套「浏览器形态用不了」的快捷键——所以这类键一律不用，另取可达的键
 * （新会话 `Alt+N`、关标签 `Alt+W`）；`browserConflict()` 把这条约束做成可执行判定，测试逐条断言。
 */
/* ------------------------------ 键位与焦点 ------------------------------ */

/** 键盘事件的最小形状（与 KeyboardEvent 结构兼容，便于用例直接构造字面量）。 */
export interface KeyEventLike {
  key: string
  /** 物理键位（`KeyboardEvent.code`）：macOS 下 Option+字母会产出死键字符，靠它认回原键。 */
  code?: string
  ctrlKey: boolean
  metaKey: boolean
  altKey: boolean
  shiftKey: boolean
  repeat?: boolean
  isComposing?: boolean
  defaultPrevented?: boolean
  target?: unknown
  preventDefault(): void
  stopPropagation(): void
}

/** 按键发生时的焦点环境：守卫按它决定「这个键该不该归歌白管」。 */
export type FocusKind = "other" | "input" | "editor" | "terminal"

/** 终端面板内的元素（含 xterm 画布与降级终端的输入行）：全局键在这里一律让位给 shell。 */
const TERMINAL_SELECTOR = ".xterm, .fw-term-panel, .fw-term-host, .fw-term-body"
/** 编辑器内（Monaco 与降级编辑器）：Monaco 自己也吃按键，需要它的场景用捕获阶段显式声明。 */
const EDITOR_SELECTOR = ".monaco-editor, .fw-fallback"

/**
 * 判定焦点环境。顺序要紧：Monaco 与 xterm 内部都是隐藏 textarea——
 * 先按容器归属认出「编辑器 / 终端」，剩下的才是普通输入框（聊天框、搜索框、提交框）。
 */
export function focusKind(target: unknown): FocusKind {
  const el = target as (HTMLElement & { tagName?: string; isContentEditable?: boolean }) | null
  if (!el || typeof el !== "object") return "other"
  const inSel = (sel: string): boolean => (typeof el.closest === "function" ? !!el.closest(sel) : false)
  if (inSel(TERMINAL_SELECTOR)) return "terminal"
  if (inSel(EDITOR_SELECTOR)) return "editor"
  if (el.tagName === "INPUT" || el.tagName === "TEXTAREA" || el.isContentEditable) return "input"
  return "other"
}

/** 键名归一：同一物理键的多种写法（ArrowDown / down / ↓）收敛成一个规范名。 */
const KEY_ALIASES: Record<string, string> = {
  escape: "Esc",
  esc: "Esc",
  enter: "Enter",
  " ": "Space",
  space: "Space",
  spacebar: "Space",
  tab: "Tab",
  backspace: "Backspace",
  delete: "Delete",
  home: "Home",
  end: "End",
  pageup: "PageUp",
  pagedown: "PageDown",
  arrowup: "↑",
  arrowdown: "↓",
  arrowleft: "←",
  arrowright: "→",
  up: "↑",
  down: "↓",
  left: "←",
  right: "→",
  // Shift+= 出的是 "+"，与 "=" 同一物理键（终端字号这类场景两种按法都要认）
  "+": "=",
}

export function normalizeKeyName(raw: string): string {
  if (!raw) return ""
  const alias = KEY_ALIASES[raw.toLowerCase()]
  if (alias) return alias
  if (/^f\d{1,2}$/i.test(raw)) return raw.toUpperCase()
  return raw.length === 1 ? raw.toUpperCase() : raw
}

/** 解析后的键位：修饰键是否精确匹配 + 规范键名。 */
export interface ParsedKey {
  ctrl: boolean
  alt: boolean
  shift: boolean
  key: string
}

/**
 * 解析键位写法（"Ctrl+Alt+S"、"Ctrl+Alt+↓"、"F2"、"Y"、"Esc"）。
 * `Ctrl`/`Cmd`/`Meta` 视作同一个修饰键（Mac 上按 Cmd 也命中）；写法不合法返回 null（由校验报出）。
 */
export function parseSpec(spec: string): ParsedKey | null {
  let s = spec.trim()
  let trailingPlus = false
  if (s.length > 1 && s.endsWith("+")) {
    trailingPlus = true
    s = s.slice(0, -1)
  }
  const mods = { ctrl: false, alt: false, shift: false }
  let key = ""
  for (const raw of s.split("+")) {
    const p = raw.trim()
    if (!p) continue
    const low = p.toLowerCase()
    if (low === "ctrl" || low === "control" || low === "cmd" || low === "meta" || low === "mod") mods.ctrl = true
    else if (low === "alt" || low === "option") mods.alt = true
    else if (low === "shift") mods.shift = true
    else key = p
  }
  if (trailingPlus) key = "+"
  if (!key) return null
  return { ...mods, key: normalizeKeyName(key) }
}

/** 规范显示写法（修饰键固定顺序 Ctrl+Alt+Shift+键）：帮助 UI、文档、提示文案共用。 */
export function formatSpec(spec: string): string {
  const p = parseSpec(spec)
  if (!p) return spec
  const parts: string[] = []
  if (p.ctrl) parts.push("Ctrl")
  if (p.alt) parts.push("Alt")
  if (p.shift) parts.push("Shift")
  parts.push(p.key)
  return parts.join("+")
}

/**
 * 事件是否命中键位：修饰键**精确相等**（`Ctrl+Alt+S` 不会被 `Ctrl+Alt+Shift+S` 触发）。
 *
 * 另带一条 macOS 专用回退：那里的 Option 是字符组合键，`Option+N` 得到的 `e.key` 是 `ñ`（死键字符），
 * 按字符比永远不会命中——只有比对物理键位（`e.code` 的 `KeyN`）才认得回 `Alt+N`。
 * 回退仅在 `e.key` 不匹配时启用，且要求组合里带 Alt（其余组合的 `e.key` 本来就可靠）。
 */
export function matchKey(e: KeyEventLike, parsed: ParsedKey): boolean {
  // 缺失的修饰键字段按 false 处理（程序化构造的事件常不带它们）
  const ctrl = !!(e.ctrlKey || e.metaKey)
  if (ctrl !== parsed.ctrl || !!e.altKey !== parsed.alt || !!e.shiftKey !== parsed.shift) return false
  if (normalizeKeyName(e.key ?? "") === parsed.key) return true
  return !!parsed.alt && keyFromCode(e.code) === parsed.key
}

/** `KeyboardEvent.code` → 规范键名（仅字母与数字区；其余键位无死键问题，不靠 code 认键）。 */
export function keyFromCode(code: string | undefined): string {
  if (!code) return ""
  const letter = /^Key([A-Z])$/.exec(code)
  if (letter) return letter[1]!
  const digit = /^Digit([0-9])$/.exec(code)
  if (digit) return digit[1]!
  return ""
}

/* ------------------------------ 分组与绑定声明 ------------------------------ */

/** 帮助 UI 与文档的分组（顺序即展示顺序）。 */
export type KeyGroupId =
  | "main.session"
  | "main.composer"
  | "main.approval"
  | "main.nav"
  | "main.overlay"
  | "wb.file"
  | "wb.view"
  | "wb.diff"
  | "wb.term"
  | "wb.ui"

export const KEY_GROUPS: Record<KeyGroupId, string> = {
  "main.session": "主界面 · 会话与布局",
  "main.composer": "主界面 · 输入框",
  "main.approval": "主界面 · 审批",
  "main.nav": "主界面 · 消息导航",
  "main.overlay": "主界面 · 弹窗与浮层",
  "wb.file": "工作台 · 文件",
  "wb.view": "工作台 · 视图与面板",
  "wb.diff": "工作台 · 差异与合并",
  "wb.term": "工作台 · 终端",
  "wb.ui": "工作台 · 弹窗与菜单",
}

export interface KeyBinding {
  /** 唯一 id（形如 `workbench.save`），冲突校验与帮助文案定位都用它。 */
  id: string
  /** 键位写法（可多个，如终端字号同时认 `=` 与 `+`）。 */
  keys: string | string[]
  /** 帮助 UI / 文档里的动作说明。 */
  label: string
  group: KeyGroupId
  run: (e: KeyEventLike) => void
  /**
   * 允许生效的焦点环境（默认 `["other","editor"]`——输入框与终端内不接管）。
   * 输入框内也要生效的显式加 `"input"`；主界面的默认焦点就是聊天输入框，
   * 所以那里的带修饰键绑定一律用 `FOCUS_WITH_INPUT`（测试断言这条约束，见 keymap.test.ts）。
   */
  focus?: FocusKind[]
  /** 命中后的附加条件（如「活动标签是差异视图」）；不满足则继续让给下一个绑定。 */
  when?: (e: KeyEventLike) => boolean
  /** 监听阶段：默认冒泡；需要抢在 Monaco / xterm 之前接管的用 `"capture"`。 */
  phase?: "capture" | "bubble"
  /** 命中后是否阻止默认行为与继续传播（默认 true；**接管浏览器默认的键位必须为 true**）。 */
  intercept?: boolean
  /**
   * 与浏览器默认绑定的关系声明：该键浏览器有默认行为（保存网页 / 打印 / 刷新…），歌白**接管**它。
   * 仅此一种取值——被浏览器自己吞掉的**保留键**不允许入表（见 `browserConflict()` 的硬约束）。
   */
  browser?: "override"
  /** 是否允许长按重复触发（默认 false：按住不放不该连发动作）。 */
  allowRepeat?: boolean
  /** 帮助 UI 里的补充说明（例如「工作台内接管 Monaco 的光标组合」）。 */
  note?: string
  /**
   * false = 该键由模块自行管理元素级监听，只登记进表（帮助 / 文档 / 冲突校验用），
   * 不参与分发。元素级监听必须先于全局判定（列表行 Enter、输入框内 Enter 等）。
   */
  owned?: boolean
}

/**
 * 默认焦点：普通页面区域与编辑器内生效，输入框与终端内不接管。
 *
 * 主界面是例外——那里的默认焦点就是聊天输入框（进草稿页/切会话/回答结束都会 `focusInput()`），
 * 所以会话语的全局键显式带上 `"input"`（见 `FOCUS_WITH_INPUT`）；工作台则相反：
 * 默认焦点在编辑器（本就含在默认集里），输入框是临时落点，除保存/刷新/提交等显式例外不接管。
 */
export const DEFAULT_FOCUS: FocusKind[] = ["other", "editor"]

/**
 * 除终端外的所有焦点环境（含输入框）：**接管浏览器默认的全局键**用它——否则在过滤框/提交框里按 Ctrl+S
 * 弹出的是浏览器的「保存网页」。个别键例外：`Ctrl+F` 在输入框里保留查找语义，只声明 `other`/`editor`。
 */
export const FOCUS_ALL_FIELDS: FocusKind[] = ["other", "editor", "input"]

/** 主界面会话区的全局键用这组：聊天输入框是默认焦点，快捷键必须在那里也能用。 */
export const FOCUS_WITH_INPUT: FocusKind[] = FOCUS_ALL_FIELDS

/** 单键与列表两种写法归一成数组。 */
export function toSpecList(keys: string | string[]): string[] {
  return Array.isArray(keys) ? keys : [keys]
}

/* ------------------------------ 浏览器冲突 ------------------------------ */

/** 键位与浏览器默认绑定的关系。 */
export type BrowserConflictLevel = "free" | "override" | "reserved"

export interface BrowserConflict {
  level: BrowserConflictLevel
  /** 被接管的浏览器行为 / 拿不到的原因；族级判定（Ctrl+Shift、F 键区）没有具体行为，留空。 */
  what?: string
}

/**
 * Chromium 的**保留命令**：浏览器在把按键交给页面之前就自己处理掉，页面收不到事件、`preventDefault` 无效。
 *
 * 依据：`chrome/browser/ui/views/frame/browser_view.cc` → `PreHandleKeyboardEvent()`（注释原文
 * *"if the accelerator is associated with the browser, and it is a reserved one (e.g. Ctrl+w), process it"*），
 * 清单取自 `chrome/browser/ui/browser_command_controller.cc` → `IsReservedCommandOrKey()`。
 *
 * 这些键**不入表**：页面拿不到的键在浏览器形态下就是没功能的快捷键，而键位只有一套
 * （新会话因此用 `Alt+N` 而非 `Ctrl+N`）。仅 `TYPE_APP`/PWA/桌面形态（WebView2、`--app` 窗口）下
 * 这些键会回到页面（源码里 `IsReservedCommandOrKey()` 对 `TYPE_APP` 直接 `return false`），
 * 但键位表不为此分叉。
 */
const CHROMIUM_RESERVED: Record<string, string> = {
  "Ctrl+N": "打开新窗口",
  "Ctrl+T": "打开新标签页",
  "Ctrl+W": "关闭标签页",
  "Ctrl+Shift+N": "打开无痕窗口",
  "Ctrl+Shift+T": "恢复刚关闭的标签页",
  "Ctrl+Shift+W": "关闭窗口",
  "Ctrl+Tab": "切换标签页",
  "Ctrl+Shift+Tab": "反向切换标签页",
  "Ctrl+PageDown": "切换标签页",
  "Ctrl+PageUp": "反向切换标签页",
  "Ctrl+Shift+Q": "退出浏览器（Linux/ChromeOS）",
  "Alt+F4": "系统：关闭窗口",
  "Ctrl+Alt+Del": "系统：安全选项",
  "Ctrl+Shift+Esc": "系统：任务管理器",
}

/**
 * 浏览器有默认绑定、但**按键先到页面**的组合：同一处对非保留命令返回 `NOT_HANDLED_IS_SHORTCUT`，
 * 页面 `preventDefault` 即接管——这就是「用常用键」的技术前提。值是被接管的那件事，帮助 UI 用它做标注。
 */
const BROWSER_BOUND: Record<string, string> = {
  "Ctrl+B": "书签（Firefox 为侧栏）",
  "Ctrl+D": "收藏当前页",
  "Ctrl+E": "地址栏搜索",
  "Ctrl+F": "页面查找",
  "Ctrl+G": "查找下一个",
  "Ctrl+H": "历史记录",
  "Ctrl+J": "下载内容",
  "Ctrl+K": "地址栏搜索",
  "Ctrl+L": "聚焦地址栏",
  "Ctrl+M": "标签页静音",
  "Ctrl+O": "打开本地文件",
  "Ctrl+P": "打印",
  "Ctrl+Q": "退出浏览器（Linux/ChromeOS）",
  "Ctrl+R": "刷新页面",
  "Ctrl+S": "保存网页",
  "Ctrl+U": "查看源代码",
  "Ctrl+=": "放大页面",
  "Ctrl+-": "缩小页面",
  "Ctrl+0": "重置页面缩放",
  "Ctrl+Shift+C": "DevTools 审查元素（DevTools 打开时）",
  "Ctrl+Shift+D": "Firefox 收藏全部标签页",
  "Ctrl+Shift+E": "Firefox 网络监视器",
  "Ctrl+Shift+I": "DevTools",
  "Ctrl+Shift+J": "DevTools 控制台",
  "Ctrl+Shift+V": "粘贴为纯文本",
  "Alt+←": "后退",
  "Alt+→": "前进",
  "Alt+Home": "主页",
  "Alt+D": "聚焦地址栏",
  "Alt+E": "浏览器菜单",
  "Alt+F": "浏览器菜单",
  F1: "帮助",
  F3: "页面查找",
  F4: "地址栏下拉（Windows）",
  F5: "刷新页面",
  F6: "聚焦地址栏",
  F7: "光标浏览",
  F10: "菜单栏",
  F11: "全屏",
  F12: "DevTools",
}

/** 文本编辑类 Ctrl 组合：页面本来就在用（复制/粘贴/撤销/全选…），不算浏览器冲突。 */
const EDITING_CTRL_KEYS = new Set(["A", "C", "V", "X", "Z", "Y", "Enter"])
/** 浏览器没绑定的功能键（歌白在用）。 */
const FREE_FKEYS = new Set(["F2", "F8", "F9"])

/**
 * 判定一个键位与浏览器默认绑定的关系——本策略的**可执行形式**（测试遍历全表断言声明完整）：
 *
 * - `Ctrl+Alt+*`：浏览器与系统都没有默认绑定（系统级只有 `Ctrl+Alt+Del`，已在保留表里）；
 * - 文本编辑类 Ctrl 组合（`Ctrl+C/V/X/A/Z/Y/Enter`）：按键本来就归页面，终端的中断语义、输入框的复制粘贴建在它上面；
 * - 其余 `Ctrl+*` / `Ctrl+Shift+*`：**族级保守判定**——命中 `BROWSER_BOUND` 报出具体行为，没命中的也算 `override`
 *   （这一族最容易撞车：新增键位必须自己查一遍并显式声明）；`Ctrl+1..9` 是标签页切换；
 * - 裸 `Alt+字母`：浏览器只绑了方向键/Home/D/E/F（已在表里），其余（`Alt+Z`、`Alt+G`…）判 `free`；`Alt+数字` 是 Firefox 切标签；
 * - 功能键：`F2`/`F8`/`F9` 浏览器不绑，其余按 `override`（`F5` 刷新页面、`F7` 光标浏览、`F12` DevTools…）；
 * - 其余裸键（`Y`/`N`/`Esc`/`Enter`/方向键…）：`free`。
 */
export function browserConflict(spec: string): BrowserConflict {
  const parsed = parseSpec(spec)
  if (!parsed) return { level: "reserved", what: "键位写法无法解析" }
  const key = formatSpec(spec)
  const reserved = CHROMIUM_RESERVED[key]
  if (reserved) return { level: "reserved", what: reserved }
  const bound = BROWSER_BOUND[key]
  if (bound) return { level: "override", what: bound }
  if (parsed.ctrl && parsed.alt) return { level: "free" }
  if (parsed.ctrl) {
    if (!parsed.shift && EDITING_CTRL_KEYS.has(parsed.key)) return { level: "free" }
    if (!parsed.shift && /^[1-9]$/.test(parsed.key)) return { level: "override", what: `切换到第 ${parsed.key} 个标签页` }
    return { level: "override" }
  }
  if (parsed.alt) {
    if (/^[1-9]$/.test(parsed.key)) return { level: "override", what: "Firefox 切换标签页" }
    return { level: "free" }
  }
  if (/^F\d{1,2}$/.test(parsed.key)) return FREE_FKEYS.has(parsed.key) ? { level: "free" } : { level: "override" }
  return { level: "free" }
}
/* ------------------------------ 分发器 ------------------------------ */

/**
 * 浮层作用域：弹窗 / 菜单 / 查看器在打开时入栈，关闭时出栈。
 * 栈顶作用域的绑定优先命中——这就是「一次 Esc 只关最上层」的实现方式
 * （此前 11 个文档级 Esc 监听互不相让，按一次会连续关掉一排浮层）。
 */
export interface KeyScope {
  id: string
  bindings: KeyBinding[]
}

/** 可挂监听的宿主（document 或测试替身）。 */
export interface KeyTarget {
  addEventListener(type: string, listener: (e: KeyEventLike) => void, capture?: boolean): void
  removeEventListener(type: string, listener: (e: KeyEventLike) => void, capture?: boolean): void
}

export interface Keymap {
  /** 当前表内全部绑定（含安装后新增的），供帮助 UI 与校验读取。 */
  bindings(): readonly KeyBinding[]
  /** 安装后追加绑定（模块在初始化函数里注册各自键位；重复登记由 `validateKeymap` 在测试里拦住）。 */
  add(binding: KeyBinding): void
  addAll(bindings: readonly KeyBinding[]): void
  install(target?: KeyTarget): void
  uninstall(): void
  pushScope(scope: KeyScope): void
  popScope(id: string): void
  hasScope(id: string): boolean
}

interface Prepared {
  binding: KeyBinding
  keys: ParsedKey[]
}

function prepare(bindings: KeyBinding[]): Prepared[] {
  return bindings.map((binding) => ({
    binding,
    keys: toSpecList(binding.keys)
      .map((s) => parseSpec(s))
      .filter((k): k is ParsedKey => !!k),
  }))
}

/**
 * 建键位表：`install()` 后接管 document 的 keydown（捕获 + 冒泡各一个监听）。
 * 命中判定顺序 = 作用域栈顶优先 → 表内声明顺序；命中即拦截（`intercept: false` 除外）。
 */
export function createKeymap(bindings: KeyBinding[]): Keymap {
  const base: Prepared[] = prepare(bindings)
  const scopes: Array<{ id: string; prepared: Prepared[] }> = []
  const installed: Array<{ target: KeyTarget; fn: (e: KeyEventLike) => void; capture: boolean }> = []

  function handle(e: KeyEventLike, phase: "capture" | "bubble"): void {
    // 输入法组合态一律不接管：Enter/Esc 属于候选确认
    if (e.defaultPrevented || e.isComposing) return
    const queue = [...scopes].reverse().flatMap((s) => s.prepared).concat(base)
    for (const { binding, keys } of queue) {
      if (binding.owned === false) continue
      if ((binding.phase ?? "bubble") !== phase) continue
      if (e.repeat && binding.allowRepeat !== true) continue
      if (!keys.some((k) => matchKey(e, k))) continue
      if (!(binding.focus ?? DEFAULT_FOCUS).includes(focusKind(e.target))) continue
      if (binding.when && !binding.when(e)) continue
      if (binding.intercept !== false) {
        e.preventDefault()
        e.stopPropagation()
      }
      binding.run(e)
      return
    }
  }

  return {
    bindings: () => base.map((p) => p.binding),
    add(b) {
      base.push(...prepare([b]))
    },
    addAll(list) {
      base.push(...prepare([...list]))
    },
    install(target: KeyTarget = document as unknown as KeyTarget) {
      const capture = (e: KeyEventLike) => handle(e, "capture")
      const bubble = (e: KeyEventLike) => handle(e, "bubble")
      target.addEventListener("keydown", capture, true)
      target.addEventListener("keydown", bubble, false)
      installed.push({ target, fn: capture, capture: true }, { target, fn: bubble, capture: false })
    },
    uninstall() {
      for (const it of installed) it.target.removeEventListener("keydown", it.fn, it.capture)
      installed.length = 0
    },
    pushScope(scope) {
      scopes.push({ id: scope.id, prepared: prepare(scope.bindings) })
    },
    popScope(id) {
      const i = scopes.findIndex((s) => s.id === id)
      if (i >= 0) scopes.splice(i, 1)
    },
    hasScope(id) {
      return scopes.some((s) => s.id === id)
    },
  }
}

/* ------------------------------ 帮助与校验 ------------------------------ */

export interface HelpRow {
  id: string
  keys: string[]
  label: string
  note?: string
  /** 被这条键接管的浏览器行为（如「打印」）——帮助 UI 标注用；缺省 = 与浏览器无冲突。 */
  takesOver?: string
}

export interface HelpGroup {
  id: KeyGroupId
  title: string
  rows: HelpRow[]
}

/** 帮助 UI 与文档的唯一数据源：按分组输出（空组不出现）。 */
export function helpGroups(bindings: readonly KeyBinding[]): HelpGroup[] {
  const groups: HelpGroup[] = (Object.keys(KEY_GROUPS) as KeyGroupId[]).map((id) => ({ id, title: KEY_GROUPS[id], rows: [] }))
  const byId = new Map(groups.map((g) => [g.id, g]))
  for (const b of bindings) {
    const g = byId.get(b.group)
    if (!g) continue
    const taken = [...new Set(toSpecList(b.keys).map((k) => browserConflict(k).what).filter((w): w is string => !!w))]
    g.rows.push({ id: b.id, keys: toSpecList(b.keys).map(formatSpec), label: b.label, note: b.note, takesOver: taken[0] })
  }
  return groups.filter((g) => g.rows.length > 0)
}

export type KeymapIssueKind = "duplicate" | "browser-undeclared" | "browser-reserved" | "invalid"

export interface KeymapIssue {
  kind: KeymapIssueKind
  id: string
  detail: string
}

function focusOverlap(a: KeyBinding, b: KeyBinding): boolean {
  const fa = a.focus ?? DEFAULT_FOCUS
  const fb = b.focus ?? DEFAULT_FOCUS
  return fa.some((f) => fb.includes(f))
}

/**
 * 浏览器相关声明校验：
 * ① 接管浏览器默认行为的键必须显式声明 `browser: "override"` 且真拦截（漏写会让「浏览器动作 + 歌白动作」双触发）；
 * ② **保留键（页面收不到）一律报错**——键位只有一套，浏览器里按不动的键不算可用键位。
 * 规则可执行化的意义：新增一条 `Ctrl+S` 却忘了声明接管、或顺手写上 `Ctrl+N`，测试直接变红。
 */
function browserIssues(b: KeyBinding, spec: string): KeymapIssue[] {
  // 元素级登记（`owned: false`）只是把**既有行为**登记进表（如 Monaco 自己的 Ctrl+F 查找）：
  // 它们由各自控件处理，不归分发器，故不适用「接管要声明」的约束。
  if (b.owned === false) return []
  const conflict = browserConflict(spec)
  const shown = formatSpec(spec)
  if (conflict.level === "reserved") {
    return [
      {
        kind: "browser-reserved",
        id: b.id,
        detail: `${shown}：浏览器自己处理该按键（${conflict.what ?? "浏览器保留组合"}），页面收不到、preventDefault 无效——请另取可达的键（如 Alt+N / Alt+W）`,
      },
    ]
  }
  if (conflict.level === "override") {
    if (b.browser !== "override") {
      return [{ kind: "browser-undeclared", id: b.id, detail: `${shown}：浏览器默认是「${conflict.what ?? "浏览器快捷键"}」，接管它需显式声明 browser: "override"` }]
    }
    if (b.intercept === false) {
      return [{ kind: "browser-undeclared", id: b.id, detail: `${shown}：接管浏览器默认必须拦截默认行为（intercept 不能为 false）` }]
    }
    return []
  }
  return []
}

/**
 * 校验一张键位表（测试断言返回空数组）：
 * ① 写法可解析 ② 浏览器相关声明完整（接管 / 保留都要显式声明）③ 同阶段 + 同键位 + 焦点重叠的重复登记。
 * 作用域表（`KeyScope`）可单独校验——作用域内与基表同键是合法的（栈顶优先就是它的语义）。
 */
export function validateKeymap(bindings: readonly KeyBinding[], where = "base"): KeymapIssue[] {
  const issues: KeymapIssue[] = []
  const seen = new Map<string, KeyBinding>()
  for (const b of bindings) {
    const specs = toSpecList(b.keys)
    if (specs.length === 0) issues.push({ kind: "invalid", id: b.id, detail: "未声明键位" })
    for (const spec of specs) {
      const parsed = parseSpec(spec)
      if (!parsed) {
        issues.push({ kind: "invalid", id: b.id, detail: `键位写法无法解析：${spec}` })
        continue
      }
      issues.push(...browserIssues(b, spec))
      // 元素级登记不参与重复检测：它们各自作用在具体控件上（列表行、输入框），天然互斥
      if (b.owned === false) continue
      const slot = `${b.phase ?? "bubble"}|${formatSpec(spec)}`
      const prev = seen.get(slot)
      if (prev && focusOverlap(prev, b)) {
        issues.push({ kind: "duplicate", id: b.id, detail: `${formatSpec(spec)} 与 ${prev.id} 重复（${where}）` })
      } else if (!prev) {
        seen.set(slot, b)
      }
    }
  }
  return issues
}

/* ------------------------------ 活跃键位表 ------------------------------ */

/**
 * 当前文档的活跃键位表（主界面与文件工作台各一份；分屏时工作台在 iframe 内，属于另一个文档、
 * 另一份表）。浮层模块（对话框、菜单、下拉、查看器）经 `pushKeyScope` 入栈自己的 Esc 绑定，
 * 不再各自往 document 上挂 keydown——这正是「一次 Esc 只关最上层」的来源。
 */
let activeMap: Keymap | null = null

export function setActiveKeymap(map: Keymap | null): void {
  activeMap = map
}

export function activeKeymap(): Keymap | null {
  return activeMap
}

/** 浮层打开：把该层绑定推到栈顶；无活跃表（测试环境/未安装）时静默忽略。 */
export function pushKeyScope(scope: KeyScope): void {
  activeMap?.pushScope(scope)
}

export function popKeyScope(id: string): void {
  activeMap?.popScope(id)
}

/** 浮层作用域 id 生成（同名浮层可能同时存在两层，如确认框里再开确认框）。 */
let scopeSeq = 0
export function nextScopeId(prefix: string): string {
  scopeSeq += 1
  return `${prefix}#${scopeSeq}`
}

/**
 * 推入一个「Esc 关闭」作用域并返回其 id——绝大多数浮层（弹窗、查看器、菜单、下拉）的全部键位
 * 需求就是这一条。关闭时用返回的 id 调 `popKeyScope`。
 *
 * 默认**不包含终端焦点**：终端里的 Esc 属于 shell（vim 退出插入模式、less 取消搜索等）。
 * 但「浮层已经把终端遮住」的场合（文件工作台的右键菜单/下拉开在面板上、且不搬焦点）必须
 * 把终端算进来，否则 Esc 会穿透给 shell：菜单关不掉，而且 shell 拿到一枚孤立的 ESC
 * （readline 进入 ESC 前缀态，后续括号粘贴被它吃掉；vim 则直接退出插入模式）。
 *
 * 这时还必须走**捕获阶段**：xterm 在自己的 textarea 上就 `preventDefault + stopPropagation`
 * 把按键吃掉了（它要把 ESC 发给 shell），document 冒泡阶段的绑定永远收不到。
 */
export function pushEscScope(prefix: string, label: string, run: () => void, group: KeyGroupId = "main.overlay", opts: { includeTerminal?: boolean } = {}): string {
  const id = nextScopeId(prefix)
  const focus: FocusKind[] = opts.includeTerminal ? ["other", "editor", "input", "terminal"] : ["other", "editor", "input"]
  const phase = opts.includeTerminal ? ("capture" as const) : undefined
  pushKeyScope({ id, bindings: [{ id: `${prefix}.esc`, keys: "Esc", label, group, focus, phase, run }] })
  return id
}
