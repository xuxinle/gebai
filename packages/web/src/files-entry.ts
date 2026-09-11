/**
 * 文件工作台入口的**跳转（新标签）**一侧：URL 拼装、在新标签/当前标签打开、副按钮绑定。
 *
 * 分工：标题栏入口的**主按钮 = 分屏打开**（见 files-split.ts，那是这个仓位最常用的动作）；
 * 悬浮时从右侧弹出的**副按钮 = 新标签打开**（整个工作台页面，与聊天并行浏览时用）。
 * 本模块只负责后者与 URL 拼装（分屏也要用它拼 iframe 的 src）。
 *
 * 为什么新标签而不是同页路由：文件工作台是重量级 IDE 式工作区（Monaco + Git 面板 + 大量
 * 资源请求），独立页面带来故障隔离——编辑器崩了不影响会话，反之亦然。
 *
 * 传参：
 *   · `session`  = 会话 id（令 `sess:` 根指向该会话工作区，Agent 产物就地可查）；
 *                 缺省取当前会话（历史消息里的产物链接需显式传它所属会话）；
 *   · `root`     = 根 id（`proj:gebai` / `abs:/path` …），显式指定则直接打开该根；
 *   · `project`  = 预置项目名（等价于 `root=proj:<name>`）；
 *   · `path`     = 直达文件（会话相对或绝对路径均可，工作台自行定位所属根）；
 *   · `line`     = 直达行号（1 起始，与 `path` 同用）；
 *   · `from`/`to`= 直接开比较视图的两端；
 *   · 主题**不进 URL**：工作台与主界面共享同一份用户级偏好（localStorage `gebai.ui.style`，
 *     theme-core 的 initTheme 会以 urlPrefs:false 忽略 URL 上的主题参数），
 *     否则一个旧链接就能把两页拆成两套配色。
 */
import { getCurrentSession } from "./state"

/** 打开工作台的参数（各字段可选，缺省按当前会话/主题补齐）。 */
export interface FilesOpenOpts {
  path?: string
  root?: string
  project?: string
  from?: string
  to?: string
  /** 1 起始行号（与 path 同用，打开后跳到该行）。 */
  line?: number
  /** 显式会话 id（历史消息的产物链接须传它渲染时的会话，否则落到当前会话的工作区）。 */
  session?: string
}

/** 文件工作台 URL（保留会话上下文；主题不走 URL）。 */
export function filesUrl(opts: FilesOpenOpts = {}): string {
  const base = (import.meta.env.BASE_URL || "/").replace(/\/$/, "")
  const params = new URLSearchParams()
  const session = opts.session ?? getCurrentSession()?.id
  if (session) params.set("session", session)
  if (opts.root) params.set("root", opts.root)
  if (opts.project) params.set("project", opts.project)
  if (opts.path) params.set("path", opts.path)
  if (opts.line && opts.line > 0) params.set("line", String(opts.line))
  if (opts.from) params.set("from", opts.from)
  if (opts.to) params.set("to", opts.to)
  const qs = params.toString()
  return `${base}/files${qs ? `?${qs}` : ""}`
}

/** 在当前标签（或新标签）打开文件工作台。 */
export function openFiles(opts: FilesOpenOpts = {}, newTab = true): void {
  const url = filesUrl(opts)
  if (newTab) window.open(url, "_blank", "noopener")
  else location.href = url
}

/**
 * 入口的副按钮 = **新标签打开**（分屏在主按钮上，见 files-split.ts）。
 * 两者分工：主按钮是常驻可见的那一个，承担最常用的动作（分屏对照）；
 * 副按钮只在浮空时从右侧弹出，承担"这次要看整页"的少数情况。
 */
export function bindFilesEntry(): void {
  const btn = document.getElementById("files-tab-btn") as HTMLButtonElement | null
  if (!btn) return
  btn.addEventListener("click", () => openFiles())
}

// 主界面快捷键：Ctrl+Shift+E 开关分屏（分屏已开则关闭）——VSCode 里同一个键也是"显示/隐藏侧边编辑器"，
// 比"再开一个新标签"更贴合这个手势的预期（连按两次不该攒出两个标签页）。
// 输入框内不触发，不与聊天输入冲突。
document.addEventListener("keydown", (e) => {
  if (!(e.ctrlKey || e.metaKey) || !e.shiftKey) return
  if (e.key.toLowerCase() !== "e") return
  const t = e.target as HTMLElement | null
  if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.isContentEditable)) return
  e.preventDefault()
  void import("./files-split").then((m) => m.toggleSplit({ path: undefined }))
})
