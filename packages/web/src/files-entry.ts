/**
 * 文件工作台入口：标题栏轮盘按钮**左侧**的「文件」按钮 → 新标签打开独立页面 `/files`。
 *
 * 为什么新标签而不是同页路由：文件工作台是重量级 IDE 式工作区（Monaco + Git 面板 + 大量
 * 资源请求），与聊天界面并行使用才是常态（一边让 Agent 改文件、一边自己核对差异）；
 * 独立页面同样带来故障隔离——编辑器崩了不影响会话，反之亦然。
 *
 * 传参：
 *   · `session`  = 会话 id（令 `sess:` 根指向该会话工作区，Agent 产物就地可查）；
 *                 缺省取当前会话（历史消息里的产物链接需显式传它所属会话）；
 *   · `root`     = 根 id（`proj:gebai` / `abs:/path` …），显式指定则直接打开该根；
 *   · `project`  = 预置项目名（等价于 `root=proj:<name>`）；
 *   · `path`     = 直达文件（会话相对或绝对路径均可，工作台自行定位所属根）；
 *   · `line`     = 直达行号（1 起始，与 `path` 同用）；
 *   · `from`/`to`= 直接开比较视图的两端；
 *   · `gb_style` = 当前 UI 主题（沿用，避免两页主题不一致）。
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

/** 文件工作台 URL（保留会话上下文与当前主题）。 */
export function filesUrl(opts: FilesOpenOpts = {}): string {
  const base = (import.meta.env.BASE_URL || "/").replace(/\/$/, "")
  const params = new URLSearchParams()
  const session = opts.session ?? getCurrentSession()?.id
  if (session) params.set("session", session)
  const theme = document.documentElement.dataset.theme
  if (theme) params.set("gb_style", theme)
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

export function bindFilesEntry(): void {
  const btn = document.getElementById("files-btn") as HTMLButtonElement | null
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
