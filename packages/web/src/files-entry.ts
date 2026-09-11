/**
 * 文件工作台入口：标题栏轮盘按钮**左侧**的「文件」按钮 → 新标签打开独立页面 `/files`。
 *
 * 为什么新标签而不是同页路由：文件工作台是重量级 IDE 式工作区（Monaco + Git 面板 + 大量
 * 资源请求），与聊天界面并行使用才是常态（一边让 Agent 改文件、一边自己核对差异）；
 * 独立页面同样带来故障隔离——编辑器崩了不影响会话，反之亦然。
 *
 * 传参：
 *   · `session`  = 当前会话 id（令 `sess:` 根指向本会话工作区，Agent 产物就地可查）；
 *   · `root`     = 会话绑定的项目根（若已知）；
 *   · `project`  = 预置项目名（服务端注册的项目，直接打开该项目根）；
 *   · `path`     = 直达文件（如从消息里的文件链接跳转）；
 *   · `gb_style` = 当前 UI 主题（沿用，避免两页主题不一致）。
 */
import { getCurrentSession } from "./state"

/** 文件工作台 URL（保留当前主题与会话上下文）。 */
export function filesUrl(opts: { path?: string; root?: string; project?: string; from?: string; to?: string } = {}): string {
  const base = (import.meta.env.BASE_URL || "/").replace(/\/$/, "")
  const params = new URLSearchParams()
  const session = getCurrentSession()
  if (session?.id) params.set("session", session.id)
  const theme = document.documentElement.dataset.theme
  if (theme) params.set("gb_style", theme)
  if (opts.root) params.set("root", opts.root)
  if (opts.project) params.set("project", opts.project)
  if (opts.path) params.set("path", opts.path)
  if (opts.from) params.set("from", opts.from)
  if (opts.to) params.set("to", opts.to)
  const qs = params.toString()
  return `${base}/files${qs ? `?${qs}` : ""}`
}

/** 在当前标签（或新标签）打开文件工作台。 */
export function openFiles(opts: { path?: string; root?: string; project?: string; from?: string; to?: string } = {}, newTab = true): void {
  const url = filesUrl(opts)
  if (newTab) window.open(url, "_blank", "noopener")
  else location.href = url
}

export function bindFilesEntry(): void {
  const btn = document.getElementById("files-btn") as HTMLButtonElement | null
  if (!btn) return
  btn.addEventListener("click", () => openFiles())
}

// 主界面快捷键：Ctrl+Shift+E 打开文件工作台（VSCode 习惯；不与聊天输入冲突——输入框内不触发）
document.addEventListener("keydown", (e) => {
  if (!(e.ctrlKey || e.metaKey) || !e.shiftKey) return
  if (e.key.toLowerCase() !== "e") return
  const t = e.target as HTMLElement | null
  if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.isContentEditable)) return
  e.preventDefault()
  openFiles({ path: undefined })
})
