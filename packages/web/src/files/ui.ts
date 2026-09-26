/**
 * 文件工作台 · UI 基础件：DOM 构造、图标、格式化、对话框、右键菜单、轻提示。
 *
 * 自包含（不依赖主界面 `state/ui/markdown` 等模块）——`/files` 是独立入口，
 * 避免为一两个工具函数把整个聊天界面的模块图拖进来；样式令牌与主界面共用（见 files.css）。
 */

import { nextScopeId, popKeyScope, pushEscScope, pushKeyScope, type FocusKind } from "../keymap"
import { extOfPath } from "@gebai/sdk"

/* ------------------------------ DOM ------------------------------ */

type Attrs = Record<string, string | number | boolean | undefined | null | ((e: Event) => void)>

/** 创建元素：`h("div", { class: "x", onclick: fn }, [child, "text"])`。 */
export function h<K extends keyof HTMLElementTagNameMap>(tag: K, attrs: Attrs = {}, children: Array<Node | string | null | undefined> = []): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag)
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue
    if (k === "class") node.className = String(v)
    else if (k === "text") node.textContent = String(v)
    else if (k === "html") node.innerHTML = String(v)
    else if (k === "value" && node instanceof HTMLInputElement) node.value = String(v)
    else if (k === "checked" && node instanceof HTMLInputElement) node.checked = v === true || v === "true"
    else if (k.startsWith("on") && typeof v === "function") node.addEventListener(k.slice(2), v as EventListener)
    else if (k === "style") node.setAttribute("style", String(v))
    else if (v === true) node.setAttribute(k, "")
    else node.setAttribute(k, String(v))
  }
  append(node, children)
  return node
}

/** 追加子节点（字符串转文本节点，null/undefined 跳过）。 */
export function append(parent: Node, children: Array<Node | string | null | undefined>): void {
  for (const c of children) {
    if (c === null || c === undefined) continue
    parent.appendChild(typeof c === "string" ? document.createTextNode(c) : c)
  }
}

export function clear(node: Element): void {
  while (node.firstChild) node.removeChild(node.firstChild)
}

export function qs<T extends Element = HTMLElement>(sel: string, root: ParentNode = document): T {
  const el = root.querySelector<T>(sel)
  if (!el) throw new Error(`元素不存在: ${sel}`)
  return el
}

/* ------------------------------ 图标 ------------------------------ */

const ICONS: Record<string, string> = {
  chevronRight: '<path d="M6 4l4 4-4 4" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>',
  chevronUp: '<path d="M4 10l4-4 4 4" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>',
  // 合并：两线汇于一条（三窗格合并视图 / 冲突行按钮）
  merge: '<path d="M4 2v5a4 4 0 004 4h4M12 2v3M12 11v3M14 8l-3-3 3-3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>',
  chevronDown: '<path d="M4 6l4 4 4-4" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>',
  chevronLeft: '<path d="M10 4L6 8l4 4" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>',
  folder: '<path d="M2 4.5A1.5 1.5 0 013.5 3h3l1.5 2h5A1.5 1.5 0 0114.5 6.5v6A1.5 1.5 0 0113 14H3.5A1.5 1.5 0 012 12.5v-8z" fill="currentColor" opacity=".85"/>',
  folderOpen: '<path d="M2 12.5V4.5A1.5 1.5 0 013.5 3h3L8 5h4.5A1.5 1.5 0 0114 6.5V8H5.6a1.5 1.5 0 00-1.45 1.1L2 12.5z" fill="currentColor" opacity=".85"/><path d="M4.6 9.1A1 1 0 015.55 8.5h8.2a1 1 0 01.96 1.28l-1.1 3.7A1.5 1.5 0 0112.2 14.5H3.2a1 1 0 01-.96-1.28l2.36-4.12z" fill="currentColor" opacity=".5"/>',
  file: '<path d="M4 2.5A1.5 1.5 0 015.5 1H9l4 4v9.5A1.5 1.5 0 0111.5 16h-6A1.5 1.5 0 014 14.5v-12z" fill="currentColor" opacity=".3"/><path d="M9 1v4h4" fill="none" stroke="currentColor" stroke-width="1.3"/>',
  save: '<path d="M3 3h8.5L14 5.5V15a1 1 0 01-1 1H3a1 1 0 01-1-1V4a1 1 0 011-1z" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M5 3v4h6V3M5 16v-5h6v5" fill="none" stroke="currentColor" stroke-width="1.4"/>',
  edit: '<path d="M11.5 2.5l2 2L6 12l-2.5.5L4 10l7.5-7.5z" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"/>',
  eye: '<path d="M1 8s2.5-4.5 7-4.5S15 8 15 8s-2.5 4.5-7 4.5S1 8 1 8z" fill="none" stroke="currentColor" stroke-width="1.3"/><circle cx="8" cy="8" r="1.9" fill="none" stroke="currentColor" stroke-width="1.3"/>',
  refresh: '<path d="M13.5 8a5.5 5.5 0 11-1.7-4M13.5 2v4h-4" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/>',
  search: '<circle cx="7" cy="7" r="4.2" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M10.2 10.2L14 14" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  git: '<circle cx="4.5" cy="4" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="4.5" cy="12.5" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="12" cy="7.5" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M4.5 6v4.5M6.4 6.4C8 7 10 7 12 7.5" fill="none" stroke="currentColor" stroke-width="1.4"/>',
  download: '<path d="M8 2v8m0 0l3-3m-3 3L5 7" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><path d="M2.5 12.5v1A1.5 1.5 0 004 15h8a1.5 1.5 0 001.5-1.5v-1" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  upload: '<path d="M8 14V6m0 0L5 9m3-3l3 3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><path d="M2.5 4.5v-1A1.5 1.5 0 014 2h8a1.5 1.5 0 011.5 1.5v1" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  // 抓取（fetch）：云 + 单箭头向下——只更新远程跟踪分支、不合并不动工作区，
  // 与拉取（download 整体下载到本地）区分：抓的是“远程状态”而非代码落地
  fetch: '<path d="M4.5 10.5a3 3 0 01.4-5.9A3.8 3.8 0 0112.3 6a2.6 2.6 0 01-.3 5" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/><path d="M8 8v6m0 0l2.2-2.2M8 14l-2.2-2.2" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/>',
  plus: '<path d="M8 3v10M3 8h10" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  // 资源管理器头部：新建文件 / 新建文件夹 / 折叠全部 / 更多
  filePlus: '<path d="M3.6 3A1.6 1.6 0 015.2 1.4h3.4l3.2 3.2v6.4" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/><path d="M8.6 1.4v3.2h3.2" fill="none" stroke="currentColor" stroke-width="1.3"/><path d="M11.8 11.4v4M9.8 13.4h4" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  folderPlus: '<path d="M1.8 4.6A1.5 1.5 0 013.3 3.1h2.9l1.4 1.9h4.9A1.5 1.5 0 0114 6.5v5.4" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/><path d="M11.8 11.4v4M9.8 13.4h4" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  collapseAll: '<path d="M3 3.6h10M3 12.4h10" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/><path d="M6.4 6.6L8 5l1.6 1.6M6.4 9.4L8 11l1.6-1.6" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/>',
  more: '<circle cx="3.4" cy="8" r="1.25" fill="currentColor"/><circle cx="8" cy="8" r="1.25" fill="currentColor"/><circle cx="12.6" cy="8" r="1.25" fill="currentColor"/>',
  minus: '<path d="M3 8h10" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  // 停止/中断：实心圆角方块（与主界面发送键的停止态同一语汇；终端标题栏的「中断当前命令」用它）
  stop: '<rect x="4.2" y="4.2" width="7.6" height="7.6" rx="1.5" fill="currentColor"/>',
  // 改动列表的两种视图（一对，形状上互补好认）：缩进层级 vs 等长行
  treeView: '<path d="M2.2 3.5h3.2M2.2 8h3.2M2.2 12.5h3.2M5.4 3.5h2.2v9H5.4" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/><path d="M7.6 6.6h2M7.6 12.4h2M9.6 6.6v5.8" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round"/>',
  listView: '<path d="M2.6 4h10.8M2.6 8h10.8M2.6 12h10.8" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  close: '<path d="M4 4l8 8M12 4l-8 8" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  trash: '<path d="M3 4.5h10M6.5 4.5V3h3v1.5M4.5 4.5l.7 9.2a1 1 0 001 .8h5.6a1 1 0 001-.8l.7-9.2" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>',
  check: '<path d="M3 8.5l3.2 3.2L13 5" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>',
  branch: '<circle cx="4.5" cy="4" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="4.5" cy="12.5" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="12" cy="7.5" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M4.5 6v4.5M6.4 6.4C8 7 10 7 12 7.5" fill="none" stroke="currentColor" stroke-width="1.4"/>',
  tag: '<path d="M8.5 1.5H14v5.5L7 14.5 1.5 9 8.5 1.5z" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"/><circle cx="11.4" cy="4.6" r="1.1" fill="currentColor"/>',
  history: '<circle cx="8" cy="8" r="6" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M8 4.6V8l2.4 1.6" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>',
  sync: '<path d="M13.5 8a5.5 5.5 0 01-9.4 3.9M2.5 8a5.5 5.5 0 019.4-3.9" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/><path d="M13.8 2.6v3.2h-3.2M2.2 13.4v-3.2h3.2" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/>',
  diff: '<path d="M4 2v12M12 2v12M2 5h4M10 11h4" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  settings: '<circle cx="8" cy="8" r="2.2" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M8 1.5l1 1.8 2-.4.4 2 1.8 1-1 1.8 1 1.8-1.8 1-.4 2-2-.4L8 14.5l-1-1.8-2 .4-.4-2-1.8-1 1-1.8-1-1.8 1.8-1 .4-2 2 .4L8 1.5z" fill="none" stroke="currentColor" stroke-width="1.2"/>',
  back: '<path d="M10 3L5 8l5 5" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>',
  // 同窗切换的一对图标（嵌入态活动栏最下方）：**左箭头 = 关闭文件工作台**（把窗口还给会话工作台），
  // 与宿主入口那颗「全屏文件工作台」的右箭头互为反向——箭头指向「我要去哪一侧」。
  arrowLeft: '<path d="M13 8H3M7 4L3 8l4 4" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>',
  // 分屏（与会话并列，两栏）：与宿主入口主按钮同一图形语言
  split: '<rect x="2" y="3.2" width="12" height="9.6" rx="1.6" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M8 3.6v8.8" stroke="currentColor" stroke-width="1.4"/>',
  // 停靠侧左右互换（两条反向箭头）
  swap: '<path d="M3 5.5h8M8.5 3l2.5 2.5L8.5 8" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><path d="M13 10.5H5M7.5 8L5 10.5 7.5 13" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>',
  expand: '<path d="M6 2h8v8M6 14H2V6" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  play: '<path d="M5 3l8 5-8 5V3z" fill="currentColor"/>',
  zoomIn: '<circle cx="7" cy="7" r="4.5" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M10.5 10.5L14 14M7 5v4M5 7h4" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>',
  zoomOut: '<circle cx="7" cy="7" r="4.5" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M10.5 10.5L14 14M5 7h4" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>',
  rotate: '<path d="M13.5 8a5.5 5.5 0 11-2-4.2M13.5 2.2v3.6h-3.6" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/>',
  archive: '<path d="M2 3.5h12v3H2zM3 6.5h10V14a1 1 0 01-1 1H4a1 1 0 01-1-1V6.5z" fill="none" stroke="currentColor" stroke-width="1.3"/><path d="M6.5 9h3" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>',
  info: '<circle cx="8" cy="8" r="6.2" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M8 7.2v4.2M8 4.9v.1" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  warning: '<path d="M8 2l6 11H2L8 2z" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"/><path d="M8 6.5v3.2M8 11.4v.1" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  terminal: '<path d="M3 3h10a1 1 0 011 1v8a1 1 0 01-1 1H3a1 1 0 01-1-1V4a1 1 0 011-1z" fill="none" stroke="currentColor" stroke-width="1.3"/><path d="M4.6 6.2L7 8.2l-2.4 2M8.4 10.6h3" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/>',
  wheel: '<circle cx="8" cy="8" r="6" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="8" cy="8" r="2" fill="currentColor"/>',
  // 更多操作轮盘入口（3×3 方块点阵，与主界面标题栏轮盘的入口图标同款）
  apps: '<rect x="2" y="2" width="3.6" height="3.6" rx="1" fill="currentColor"/><rect x="6.2" y="2" width="3.6" height="3.6" rx="1" fill="currentColor"/><rect x="10.4" y="2" width="3.6" height="3.6" rx="1" fill="currentColor"/><rect x="2" y="6.2" width="3.6" height="3.6" rx="1" fill="currentColor"/><rect x="6.2" y="6.2" width="3.6" height="3.6" rx="1" fill="currentColor"/><rect x="10.4" y="6.2" width="3.6" height="3.6" rx="1" fill="currentColor"/><rect x="2" y="10.4" width="3.6" height="3.6" rx="1" fill="currentColor"/><rect x="6.2" y="10.4" width="3.6" height="3.6" rx="1" fill="currentColor"/><rect x="10.4" y="10.4" width="3.6" height="3.6" rx="1" fill="currentColor"/>',
  // 侧边 blame 列（每行出自谁手）：人形 + 归因引线（GitLens 同语汇，与「文件历史」的时钟图标区分开）
  blame: '<circle cx="5.2" cy="4.2" r="2.1" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M2.2 12.8c0-2.3 1.4-3.7 3-3.7s3 1.4 3 3.7" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/><path d="M10.2 4.6h3.4M10.2 7.8h3.4M10.2 11h3.4" stroke="currentColor" stroke-width="1.3" stroke-linecap="round"/>',
  // 行尾 blame（光标行尾的注释）：代码行 + 行尾两段注释（与侧边列的“人形”区分开）
  blameEol: '<path d="M2.2 4.6h5.6M2.2 8h4.2M2.2 11.4h5" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/><path d="M10.4 6.3h3.4M10.4 9.7h3.4" stroke="currentColor" stroke-width="1.2" stroke-linecap="round" opacity=".55"/>',
  copy: '<rect x="5" y="5" width="8" height="9" rx="1.2" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M3 10.5V3.8A1.3 1.3 0 014.3 2.5h5" fill="none" stroke="currentColor" stroke-width="1.4"/>',
  // 粘贴（剪贴板板身 + 顶部夹子）：与「复制」成对出现（右键菜单与 Ctrl+V）
  paste: '<path d="M6.4 2.6h3.2v2.2H6.4z" fill="none" stroke="currentColor" stroke-width="1.3"/><rect x="3.4" y="4" width="9.2" height="9.6" rx="1.4" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M5.8 7.6h4.4M5.8 10.4h3" stroke="currentColor" stroke-width="1.3" stroke-linecap="round"/>',
  undo: '<path d="M3.5 8a5 5 0 115 5H6" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/><path d="M6 4.5L3 7.6l3 3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>',
  // 自动换行：一条长线 + 折返的一笔 + 指回左侧的箭头（长行不再横向滚，而是折回来）
  // 转到符号（大纲）：缩进的短横线 + 名字条，形状与编辑器大纲一致
  symbols: '<path d="M2.6 4h2.2M7 4h6.4M5 8h2.2M9.4 8h4M5 12h2.2M9.4 12h3" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>',
  wrap: '<path d="M2 4h12M2 8h8.6a2.4 2.4 0 010 4.8H7.4" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/><path d="M9.6 10.7L7.4 12.9l2.2 2.2" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/>',
}

/** 图标元素缓存：目录树每行 1~2 个图标，逐次 `innerHTML` 解析 SVG 是纯浪费（克隆已有节点即可）。 */
const iconCache = new Map<string, SVGSVGElement>()

/** 生成图标元素（默认 14px，currentColor）。返回的总是新节点（可安全改属性/挂监听）。 */
export function icon(name: keyof typeof ICONS | string, size = 14): SVGSVGElement {
  const key = `${name}|${size}`
  const cached = iconCache.get(key)
  if (cached) return cached.cloneNode(true) as SVGSVGElement
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg")
  svg.setAttribute("viewBox", "0 0 16 16")
  svg.setAttribute("width", String(size))
  svg.setAttribute("height", String(size))
  svg.setAttribute("class", "fw-icon")
  svg.setAttribute("aria-hidden", "true")
  svg.innerHTML = ICONS[name] ?? ICONS.file
  iconCache.set(key, svg)
  return svg.cloneNode(true) as SVGSVGElement
}

export { ICONS }

/* ------------------------------ 格式化 ------------------------------ */

export function formatSize(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes < 0) return "-"
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`
  return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`
}

export function formatTime(ms: number): string {
  if (!ms) return "-"
  const d = new Date(ms)
  const p = (n: number) => String(n).padStart(2, "0")
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
}

export function timeAgo(ms: number): string {
  if (!ms) return ""
  const diff = Date.now() - ms
  const m = Math.round(diff / 60000)
  if (m < 1) return "刚刚"
  if (m < 60) return `${m} 分钟前`
  const hour = Math.round(m / 60)
  if (hour < 24) return `${hour} 小时前`
  const day = Math.round(hour / 24)
  if (day < 30) return `${day} 天前`
  const mon = Math.round(day / 30)
  if (mon < 12) return `${mon} 个月前`
  return `${Math.round(mon / 12)} 年前`
}

/** 文件扩展名（小写，不含点）。实现收敛到 SDK（与编辑器语言/预览类别共用同一套容错）。 */
export function extOf(name: string): string {
  return extOfPath(name)
}

/** 文件类型 → 图标颜色类（IDE 风格的语义色，不依赖具体主题）。 */
const ICON_COLOR: Record<string, string> = {
  ts: "blue", tsx: "blue", mts: "blue", cts: "blue",
  js: "yellow", jsx: "yellow", mjs: "yellow", cjs: "yellow",
  json: "yellow", jsonc: "yellow", json5: "yellow", jsonl: "yellow",
  md: "cyan", markdown: "cyan", mdx: "cyan", rst: "cyan",
  py: "green", rb: "red", go: "cyan", rs: "orange", java: "red", kt: "purple", kts: "purple",
  c: "blue", h: "blue", cpp: "blue", cc: "blue", hpp: "blue", cs: "purple", swift: "orange", dart: "cyan",
  html: "orange", htm: "orange", vue: "green", svelte: "orange", css: "blue", scss: "pink", sass: "pink", less: "blue",
  yml: "purple", yaml: "purple", toml: "orange", ini: "gray", conf: "gray", env: "yellow",
  sh: "green", bash: "green", zsh: "green", ps1: "blue", bat: "gray", cmd: "gray",
  png: "purple", jpg: "purple", jpeg: "purple", gif: "purple", webp: "purple", svg: "orange", ico: "purple", bmp: "purple", avif: "purple",
  pdf: "red", doc: "blue", docx: "blue", xls: "green", xlsx: "green", ppt: "orange", pptx: "orange",
  zip: "yellow", tar: "yellow", gz: "yellow", "7z": "yellow", rar: "yellow", jar: "yellow",
  mp4: "pink", mov: "pink", webm: "pink", mkv: "pink", avi: "pink", mp3: "pink", wav: "pink", flac: "pink", ogg: "pink",
  sql: "cyan", graphql: "pink", gql: "pink", proto: "cyan", lock: "gray", sum: "gray",
}

export function iconColorFor(name: string, type: string): string {
  if (type === "dir") return "folder"
  if (type === "symlink") return "cyan"
  return ICON_COLOR[extOf(name)] ?? "gray"
}

/* ------------------------------ 轻提示 ------------------------------ */

let toastHost: HTMLElement | null = null
/** 报错浮层条数上限：报错常驻不消退，无上限会占满右下角。 */
const MAX_ERROR_TOASTS = 4

/** 移除一条轻提示（淡出后摘除）。 */
function dropToast(el: HTMLElement): void {
  if (!el.isConnected) return
  el.classList.add("out")
  setTimeout(() => el.remove(), 260)
}

/**
 * 轻提示：`error` **常驻**——报错信息是排查依据，不自动消退（`ms` 对该类无效），点浮层或关闭按钮移除，
 * 超出 MAX_ERROR_TOASTS 淘汰最旧；其余类型到时自动消退。
 */
export function toast(message: string, kind: "info" | "success" | "error" | "warn" = "info", ms = 3200): void {
  if (!toastHost?.isConnected) {
    toastHost = h("div", { class: "fw-toasts" })
    document.body.appendChild(toastHost)
  }
  const el = h("div", { class: `fw-toast ${kind}` }, [icon(kind === "error" ? "warning" : kind === "success" ? "check" : "info"), h("span", { text: message })])
  toastHost.appendChild(el)
  if (kind === "error") {
    const close = h("button", { class: "fw-toast-close", text: "×", title: "关闭" })
    close.addEventListener("click", (ev) => {
      ev.stopPropagation()
      dropToast(el)
    })
    el.appendChild(close)
    const errors = [...toastHost.children].filter((c) => c.classList.contains("error")) as HTMLElement[]
    for (const old of errors.slice(0, Math.max(0, errors.length - MAX_ERROR_TOASTS))) dropToast(old)
  } else {
    setTimeout(() => dropToast(el), ms)
  }
  el.onclick = () => dropToast(el)
}

/* ------------------------------ 对话框 ------------------------------ */

interface PromptOpts {
  title: string
  label?: string
  value?: string
  placeholder?: string
  okText?: string
  /** 多行输入（提交信息用） */
  multiline?: boolean
  /** 附加说明（如「将覆盖同名文件」） */
  hint?: string
}

/** 通用输入对话框（Promise 化；取消返回 null）。 */
export function promptDialog(opts: PromptOpts): Promise<string | null> {
  return new Promise((resolve) => {
    const input = opts.multiline
      ? h("textarea", { class: "fw-input fw-textarea", placeholder: opts.placeholder ?? "", rows: 5 })
      : h("input", { class: "fw-input", placeholder: opts.placeholder ?? "" })
    input.value = opts.value ?? ""
    const okBtn = h("button", { class: "fw-btn primary", text: opts.okText ?? "确定" })
    const cancelBtn = h("button", { class: "fw-btn", text: "取消" })
    const body = h("div", { class: "fw-dialog-body" }, [
      opts.label ? h("label", { class: "fw-label", text: opts.label }) : null,
      input,
      opts.hint ? h("div", { class: "fw-hint", text: opts.hint }) : null,
    ])
    const dialog = h("div", { class: "fw-dialog" }, [h("div", { class: "fw-dialog-title", text: opts.title }), body, h("div", { class: "fw-dialog-actions" }, [cancelBtn, okBtn])])
    const overlay = h("div", { class: "fw-overlay" }, [dialog])
    const done = (v: string | null) => {
      overlay.remove()
      popKeyScope(scopeId)
      resolve(v)
    }
    // 打开即聚焦输入框，所以这层键位要在输入焦点下也能用
    const inFields: FocusKind[] = ["other", "editor", "input"]
    const scopeId = nextScopeId("wb.prompt")
    pushKeyScope({
      id: scopeId,
      bindings: [
        { id: "wb.prompt.esc", keys: "Esc", label: "取消输入对话框", group: "wb.ui", focus: inFields, run: () => done(null) },
        { id: "wb.prompt.submit", keys: "Enter", label: "提交输入对话框", group: "wb.ui", focus: inFields, when: () => !opts.multiline, run: () => done(input.value) },
        { id: "wb.prompt.submitMultiline", keys: "Ctrl+Enter", label: "提交输入对话框（含多行字段）", group: "wb.ui", focus: inFields, run: () => done(input.value) },
      ],
    })
    okBtn.onclick = () => done(input.value)
    cancelBtn.onclick = () => done(null)
    overlay.onclick = (e) => {
      if (e.target === overlay) done(null)
    }
    document.body.appendChild(overlay)
    setTimeout(() => {
      input.focus()
      if (input instanceof HTMLInputElement) input.select()
    }, 20)
  })
}

/** 确认对话框（Promise 化）。 */
export function confirmDialog(opts: { title: string; message: string; okText?: string; danger?: boolean; hint?: string }): Promise<boolean> {
  return new Promise((resolve) => {
    const okBtn = h("button", { class: `fw-btn ${opts.danger ? "danger" : "primary"}`, text: opts.okText ?? "确定" })
    const cancelBtn = h("button", { class: "fw-btn", text: "取消" })
    const dialog = h("div", { class: "fw-dialog" }, [
      h("div", { class: "fw-dialog-title" }, [opts.danger ? icon("warning") : icon("info"), h("span", { text: opts.title })]),
      h("div", { class: "fw-dialog-body" }, [h("div", { class: "fw-dialog-msg", text: opts.message }), opts.hint ? h("div", { class: "fw-hint", text: opts.hint }) : null]),
      h("div", { class: "fw-dialog-actions" }, [cancelBtn, okBtn]),
    ])
    const overlay = h("div", { class: "fw-overlay" }, [dialog])
    const done = (v: boolean) => {
      overlay.remove()
      popKeyScope(scopeId)
      resolve(v)
    }
    const inFields: FocusKind[] = ["other", "editor", "input"]
    const scopeId = nextScopeId("wb.confirm")
    pushKeyScope({
      id: scopeId,
      bindings: [
        { id: "wb.confirm.esc", keys: "Esc", label: "取消确认框", group: "wb.ui", focus: inFields, run: () => done(false) },
        { id: "wb.confirm.ok", keys: "Enter", label: "确认", group: "wb.ui", focus: inFields, run: () => done(true) },
      ],
    })
    okBtn.onclick = () => done(true)
    cancelBtn.onclick = () => done(false)
    overlay.onclick = (e) => {
      if (e.target === overlay) done(false)
    }
    document.body.appendChild(overlay)
    setTimeout(() => okBtn.focus(), 20)
  })
}

/* ------------------------------ 右键菜单 ------------------------------ */

export interface MenuItem {
  label?: string
  icon?: string
  shortcut?: string
  danger?: boolean
  disabled?: boolean
  separator?: boolean
  onClick?: () => void
  submenu?: MenuItem[]
}

let menuHost: HTMLElement | null = null
/** 当前菜单的全局监听卸载函数（closeMenu 必须调它，不能只靠 MutationObserver——见 showMenu 注释）。 */
let menuCleanup: (() => void) | null = null

export function closeMenu(): void {
  menuCleanup?.()
  menuHost?.remove()
  menuHost = null
}

/** 弹出右键菜单 / 下拉菜单（点击外部或 Escape 关闭；屏幕边缘自动回折）。 */
export function showMenu(x: number, y: number, items: MenuItem[]): void {
  closeMenu()
  const host = h("div", { class: "fw-menu-pop" })
  const build = (list: MenuItem[], container: HTMLElement) => {
    for (const it of list) {
      if (it.separator) {
        container.appendChild(h("div", { class: "fw-menu-sep" }))
        continue
      }
      const btn = h("button", { class: `fw-menu-item${it.danger ? " danger" : ""}${it.disabled ? " disabled" : ""}` }, [
        it.icon ? icon(it.icon) : h("span", { class: "fw-menu-icon-gap" }),
        h("span", { class: "fw-menu-label", text: it.label ?? "" }),
        it.shortcut ? h("span", { class: "fw-menu-shortcut", text: it.shortcut }) : null,
        it.submenu ? icon("chevronRight") : null,
      ])
      if (it.disabled) btn.setAttribute("disabled", "")
      else
        btn.onclick = (e) => {
          e.stopPropagation()
          if (it.submenu) return
          closeMenu()
          it.onClick?.()
        }
      if (it.submenu) {
        let sub: HTMLElement | null = null
        let subTimer: number | null = null
        /** 收起子菜单（延迟：给鼠标从父项移到子菜单留出路程）。 */
        const drop = (): void => {
          if (subTimer !== null) clearTimeout(subTimer)
          subTimer = window.setTimeout(() => {
            sub?.remove()
            sub = null
            subTimer = null
          }, 150)
        }
        const hold = (): void => {
          if (subTimer !== null) clearTimeout(subTimer)
          subTimer = null
        }
        btn.onmouseenter = () => {
          hold()
          if (sub) return // 已展开：不重建（重建会把鼠标正下方的子菜单换掉）
          const box = h("div", { class: "fw-menu-pop sub" })
          build(it.submenu ?? [], box)
          host.appendChild(box)
          const hr = host.getBoundingClientRect()
          const r = btn.getBoundingClientRect()
          /*
           * 坐标用 **host 局部**：`.fw-menu-pop` 带 backdrop-filter，而 backdrop-filter（同 filter）
           * 会把自身变成 fixed 子元素的包含块——写视口坐标会被当成 host 内偏移，子菜单跑到很远的地方（鼠标够不到）。
           * 紧贴父项右缘（+2），并保证整个子菜单落在父菜单高度范围内（否则鼠标一出父菜单就丢了）。
           */
          box.style.left = `${r.right - hr.left + 2}px`
          box.style.top = `${Math.max(0, Math.min(r.top - hr.top, hr.height - box.offsetHeight - 8))}px`
          box.onmouseenter = hold
          box.onmouseleave = drop
          sub = box
        }
        btn.onmouseleave = drop
      }
      container.appendChild(btn)
    }
  }
  build(items, host)
  host.style.left = `${x}px`
  host.style.top = `${y}px`
  document.body.appendChild(host)
  const rect = host.getBoundingClientRect()
  if (rect.right > window.innerWidth - 6) host.style.left = `${Math.max(4, window.innerWidth - rect.width - 6)}px`
  if (rect.bottom > window.innerHeight - 6) host.style.top = `${Math.max(4, window.innerHeight - rect.height - 6)}px`
  menuHost = host
  const onDown = (e: MouseEvent) => {
    if (!host.contains(e.target as Node)) closeMenu()
  }
  const cleanup = () => {
    document.removeEventListener("mousedown", onDown)
    popKeyScope(scopeId)
    observer.disconnect()
    if (menuCleanup === cleanup) menuCleanup = null
  }
  const observer = new MutationObserver(() => {
    if (!document.body.contains(host)) cleanup()
  })
  /*
   * 同步注册监听：早期用 `setTimeout(..., 0)` 延后注册、只靠 MutationObserver 清理——
   * 若菜单在下一宏任务前就被关闭（快速连开两次菜单），清理已经跑完、监听随后才加上，
   * 于是每开一次菜单就多一对永不摘除的 document 监听器（闭包还持着已移除的菜单 DOM）。
   * 不会误关当前这次点击：打开菜单的都是 click/contextmenu，而 mousedown 早在它们之前就已派发完。
   */
  // 菜单可能开在终端面板上（终端里右键 / 设置 / 标签菜单）：焦点在终端时 Esc 也要能关菜单，
  // 否则 Esc 会穿透给 shell（readline 进 ESC 前缀态、vim 退出插入模式），而菜单还开着。
  const scopeId = pushEscScope("wb.menu", "关闭菜单", closeMenu, "wb.ui", { includeTerminal: true })
  document.addEventListener("mousedown", onDown)
  observer.observe(document.body, { childList: true })
  menuCleanup = cleanup
}

/** 下拉菜单（按钮锚定；用于工具栏菜单）。 */
export function dropdown(anchor: HTMLElement, items: MenuItem[]): void {
  const r = anchor.getBoundingClientRect()
  showMenu(r.left, r.bottom + 4, items)
}
