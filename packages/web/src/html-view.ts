import type { ContentBlock } from "@gebai/sdk"
import { el } from "./state"
import { copyText, toast, tip } from "./ui"

/* ---------- HTML 内容块渲染：域隔离沙箱 iframe 直接渲染（脚本可执行，但隔离于宿主页面） ---------- */

/**
 * 沙箱 CSP：放行内联/外部脚本、样式与网络（配合 sandbox 域隔离使用）。
 * 作为 meta 注入 srcdoc 文档 head 开头；页面自带 CSP 与注入策略取交集，只会更严。
 */
const SANDBOX_CSP =
  '<meta http-equiv="Content-Security-Policy" content="default-src * data: blob:; script-src \'unsafe-inline\' \'unsafe-eval\'; style-src \'unsafe-inline\'">'

/**
 * 主题同步脚本：iframe 内维护可更新的 style#gebai-theme，注入主题变量集（:root）、背景与滚动条色值。
 * 初始快照由宿主渲染时注入；此后宿主主题切换时经 postMessage 广播（gebai-host/theme）动态更新。
 * 内容页自带背景/变量时其自身样式（同层后在文档中靠后）覆盖注入值，不破坏内容设计。
 */
function themeScript(bg: string, thumb: string, thumbHover: string, vars: Record<string, string>, themeId: string): string {
  const apply =
    "function apply(b,t,h,v,th){var s='';" +
    "if(v){s+=':root{';for(var k in v)s+=k+':'+v[k]+';';s+='}'}" +
    "if(b)s+='html{background-color:'+b+'}';" +
    "if(t)s+='html{scrollbar-width:thin;scrollbar-color:'+t+' transparent}" +
    "html::-webkit-scrollbar{width:10px;height:10px}" +
    "html::-webkit-scrollbar-thumb{background:'+t+';border-radius:8px;border:2px solid transparent;background-clip:content-box}" +
    "html::-webkit-scrollbar-thumb:hover{background:'+(h||t)+';border:2px solid transparent;background-clip:content-box}" +
    "html::-webkit-scrollbar-track{background:transparent}';" +
    "document.getElementById('gebai-theme').textContent=s;" +
    "if(th)document.documentElement.setAttribute('data-theme',th)}"
  return (
    `<script>(function(){` +
    `var st=document.createElement('style');st.id='gebai-theme';document.head.appendChild(st);` +
    `${apply}` +
    `window.addEventListener('message',function(e){var d=e.data;if(d&&d.source==='gebai-host'&&d.type==='theme')apply(d.bg,d.thumb,d.thumbHover,d.vars,d.theme)});` +
    `apply(${JSON.stringify(bg)},${JSON.stringify(thumb)},${JSON.stringify(thumbHover)},${JSON.stringify(vars)},${JSON.stringify(themeId)});` +
    `})()<\/script>`
  )
}

/** 注入 iframe 的主题变量集（工具/预览页面可用 var(--x) 引用，随主题切换更新）。 */
const THEME_VARS = [
  "--bg",
  "--bg-elev",
  "--bg-elev-2",
  "--bg-inset",
  "--text",
  "--text-muted",
  "--text-faint",
  "--accent",
  "--accent-hover",
  "--accent-soft",
  "--on-accent",
  "--border",
  "--border-strong",
  "--success",
  "--warning",
  "--danger",
  "--radius-sm",
  "--radius-md",
  "--radius-lg",
  "--font-mono",
] as const

/**
 * 组装沙箱文档：原始 HTML 原样保留（脚本可执行），注入 CSP meta 与主题同步脚本。
 * 安全由 iframe 域隔离保证（sandbox="allow-scripts" 无 allow-same-origin：
 * 脚本运行在 opaque origin，无法访问宿主 DOM/Cookie/存储、无法顶层导航/弹窗/表单/下载）。
 */
export function sandboxedHtml(
  html: string,
  inject: { bg?: string; scrollThumb?: string; scrollThumbHover?: string; vars?: Record<string, string>; theme?: string } = {},
): string {
  const parts = [SANDBOX_CSP]
  if (inject.bg || inject.scrollThumb || (inject.vars && Object.keys(inject.vars).length))
    parts.push(themeScript(inject.bg ?? "", inject.scrollThumb ?? "", inject.scrollThumbHover ?? "", inject.vars ?? {}, inject.theme ?? ""))
  const headExtra = parts.join("")
  // `<head(?:\s[^>]*)?>`：[^>]* 会误匹配 `<header>`（meta CSP 注入到 body 中失效——纵深防御层失位）
  const m = html.match(/<head(?:\s[^>]*)?>/i)
  if (m) {
    const idx = m.index! + m[0].length
    return html.slice(0, idx) + headExtra + html.slice(idx)
  }
  return `<!doctype html><html><head>${headExtra}</head><body>${html}</body></html>`
}

/** 读取宿主当前主题快照（计算值，主题 CSS 已应用后调用）：变量集 + 背景/滚动条 + 主题 id。 */
export function currentThemeColors(): { bg: string; thumb: string; thumbHover: string; vars: Record<string, string>; theme: string } {
  const cs = getComputedStyle(document.documentElement)
  const vars: Record<string, string> = {}
  for (const k of THEME_VARS) {
    const v = cs.getPropertyValue(k).trim()
    if (v) vars[k] = v
  }
  return {
    bg: cs.getPropertyValue("--bg-inset").trim(),
    thumb: cs.getPropertyValue("--border-strong").trim(),
    thumbHover: cs.getPropertyValue("--text-faint").trim(),
    vars,
    theme: document.documentElement.dataset.theme ?? "",
  }
}

/** 向所有 HTML 预览 iframe 广播当前主题（变量集 + 背景/滚动条 + 主题 id，iframe 内脚本更新）。 */
function broadcastTheme(): void {
  const c = currentThemeColors()
  if (!c.bg && !c.thumb && Object.keys(c.vars).length === 0) return
  const msg = { source: "gebai-host", type: "theme", bg: c.bg, thumb: c.thumb, thumbHover: c.thumbHover, vars: c.vars, theme: c.theme }
  for (const f of document.querySelectorAll<HTMLIFrameElement>("iframe[data-html-frame]")) {
    f.contentWindow?.postMessage(msg, "*")
  }
}

/** 主题同步：轮询比较主题变量快照（主题 CSS 异步加载，属性监听时机不可靠）+ 主题切换事件立即广播（免 500ms 轮询延迟）。 */
let themeSyncStarted = false
export function ensureThemeSync(): void {
  if (themeSyncStarted) return
  themeSyncStarted = true
  let last = ""
  const sample = (): string => {
    const c = currentThemeColors()
    return [c.bg, c.thumb, c.thumbHover, c.theme, ...Object.entries(c.vars).map(([k, v]) => `${k}${v}`)].join("|")
  }
  last = sample()
  document.addEventListener("gebai:theme-change", () => {
    const cur = sample()
    if (cur !== last) {
      last = cur
      broadcastTheme()
    }
  })
  setInterval(() => {
    const cur = sample()
    if (cur !== last) {
      last = cur
      broadcastTheme()
    }
  }, 500)
}

/**
 * 沙箱预览 iframe：srcdoc 注入文档；allow-scripts 放开脚本，域隔离（opaque origin）防宿主访问。
 * data-html-frame 标记所有 HTML iframe（主题广播用）。
 * 宽度固定 100% 铺满消息流（不参与任何内容宽度反馈）；带显式尺寸（show html 分支 width/height）
 * 时按指定值渲染。全屏查看器同样固定铺满视口。
 */
export function previewFrame(doc: string): HTMLIFrameElement {
  const frame = document.createElement("iframe")
  frame.className = "html-frame"
  frame.setAttribute("sandbox", "allow-scripts")
  frame.setAttribute("referrerpolicy", "no-referrer")
  frame.setAttribute("loading", "lazy")
  frame.setAttribute("data-html-frame", "")
  frame.srcdoc = doc
  // lazy 加载晚于主题切换时，load 后立即补一次主题广播
  frame.addEventListener("load", () => {
    const c = currentThemeColors()
    if (c.bg || c.thumb || Object.keys(c.vars).length)
      frame.contentWindow?.postMessage({ source: "gebai-host", type: "theme", bg: c.bg, thumb: c.thumb, thumbHover: c.thumbHover, vars: c.vars, theme: c.theme }, "*")
  })
  return frame
}

/* ---------- 工具栏图标（16px 线性 SVG，跟随 currentColor） ---------- */

export const ICON_FULLSCREEN = '<svg viewBox="0 0 16 16"><path d="M3 6.5V3h3.5M13 6.5V3H9.5M3 9.5V13h3.5M13 9.5V13H9.5"/></svg>'
export const ICON_SOURCE = '<svg viewBox="0 0 16 16"><path d="M6 3.5 2.5 8l3.5 4.5M10 3.5l3.5 4.5-3.5 4.5"/></svg>'
export const ICON_COPY = '<svg viewBox="0 0 16 16"><rect x="4.5" y="4.5" width="8" height="8" rx="1.5"/><path d="M11.5 3.5h-6a1.5 1.5 0 0 0-1.5 1.5v6"/></svg>'
export const ICON_DOWNLOAD = '<svg viewBox="0 0 16 16"><path d="M8 2v8m0 0L4.5 6.5M8 10l3.5-3.5M3 13h10"/></svg>'

/** 图标按钮：SVG 图标 + title 提示。 */
export function iconButton(title: string, icon: string): HTMLButtonElement {
  const btn = el("button")
  tip(btn, title)
  btn.innerHTML = icon
  return btn
}

/** 按钮成功反馈：图标变 ✓ + 高亮，1.5s 后恢复。 */
export function flashButton(btn: HTMLButtonElement, label: string): void {
  const orig = btn.innerHTML
  const origTitle = btn.title
  btn.innerHTML = '<svg viewBox="0 0 16 16"><path d="M3 8.5l3.5 3.5L13 4.5"/></svg>'
  tip(btn, label)
  btn.classList.add("ok")
  setTimeout(() => {
    btn.innerHTML = orig
    tip(btn, origTitle)
    btn.classList.remove("ok")
  }, 1500)
}

/** 下载 HTML 源码（.html）。 */
export function downloadHtmlSource(code: string, name: string): void {
  const a = document.createElement("a")
  a.href = URL.createObjectURL(new Blob([code], { type: "text/html;charset=utf-8" }))
  a.download = (name || "page").replace(/\.html?$/, "") + ".html"
  a.click()
  setTimeout(() => URL.revokeObjectURL(a.href), 1000)
}

/** 源码查看弹窗（只读 + 复制 + 下载 .html）。 */
function openSourceView(code: string, name: string): void {
  const overlay = el("div", "preview-overlay")
  const card = el("div", "preview-card")
  const head = el("div", "preview-head")
  head.append(el("span", "preview-title", `HTML 源码 · ${name}`))
  const closeBtn = el("button", "preview-close", "✕")
  tip(closeBtn, "关闭（Esc）")
  head.appendChild(closeBtn)
  const body = el("div", "preview-body")
  const editor = el("textarea", "diagram-editor")
  editor.value = code
  editor.readOnly = true
  editor.style.minHeight = "60vh"
  const actions = el("div", "diagram-toolbar")
  const copy = el("button", undefined, "复制")
  copy.onclick = async () => {
    try {
      await copyText(code)
      copy.textContent = "已复制 ✓"
      setTimeout(() => (copy.textContent = "复制"), 1500)
    } catch {
      toast("复制失败")
    }
  }
  const dl = el("button", undefined, "下载 .html")
  dl.onclick = () => downloadHtmlSource(code, name)
  actions.append(copy, dl)
  body.append(editor, actions)
  card.append(head, body)
  overlay.appendChild(card)
  document.body.appendChild(overlay)
  const close = () => {
    overlay.remove()
    document.removeEventListener("keydown", onKey)
  }
  const onKey = (ev: KeyboardEvent) => {
    if (ev.key === "Escape") close()
  }
  closeBtn.onclick = close
  overlay.onclick = (ev) => {
    if (ev.target === overlay) close()
  }
  document.addEventListener("keydown", onKey)
}

/** 全屏查看器：大尺寸沙箱预览 + 标题栏图标工具栏（源码/下载）。 */
function openHtmlViewer(doc: string, name: string, raw: string): void {
  const overlay = el("div", "preview-overlay html-viewer-overlay")
  const card = el("div", "preview-card html-viewer")
  const head = el("div", "preview-head")
  head.append(el("span", "preview-title", name))
  const toolbar = el("div", "html-toolbar")
  const srcBtn = iconButton("查看源码", ICON_SOURCE)
  const dlBtn = iconButton("下载 .html", ICON_DOWNLOAD)
  toolbar.append(srcBtn, dlBtn)
  const closeBtn = el("button", "preview-close", "✕")
  tip(closeBtn, "关闭（Esc）")
  head.append(toolbar, closeBtn)
  const body = el("div", "preview-body")
  const frame = previewFrame(doc)
  frame.className = "html-frame html-frame-full"
  body.appendChild(frame)
  card.append(head, body)
  overlay.appendChild(card)
  document.body.appendChild(overlay)
  const close = () => {
    overlay.remove()
    document.removeEventListener("keydown", onKey)
  }
  const onKey = (ev: KeyboardEvent) => {
    if (ev.key === "Escape") close()
  }
  closeBtn.onclick = close
  srcBtn.onclick = () => openSourceView(raw, name)
  dlBtn.onclick = () => downloadHtmlSource(raw, name)
  overlay.onclick = (ev) => {
    if (ev.target === overlay) close()
  }
  document.addEventListener("keydown", onKey)
}

/** 消息流内 HTML 卡片：标题栏（文件名 + 工具栏）+ 沙箱 iframe 预览；标题/预览点击打开占满页面的查看器。 */
export function renderHtmlBlock(container: HTMLElement, b: Extract<ContentBlock, { type: "html" }>) {
  const box = el("div", "html-card")
  const name = (b.name || "页面").replace(/\.html?$/, "")
  const raw = b.html
  if (!raw.trim()) {
    box.appendChild(el("div", "html-error", "HTML 内容为空，无法预览"))
    container.appendChild(box)
    return
  }
  // 主题快照（变量集 + 背景/滚动条 + 主题 id）初始注入 iframe + 主题同步监听
  const c = currentThemeColors()
  const doc = sandboxedHtml(raw, {
    bg: c.bg || undefined,
    scrollThumb: c.thumb || undefined,
    scrollThumbHover: c.thumbHover || undefined,
    vars: c.vars,
    theme: c.theme || undefined,
  })
  ensureThemeSync()
  const head = el("div", "html-head")
  const title = el("span", "html-title clickable", name)
  tip(title, "点击全屏查看")
  head.appendChild(title)
  // 次级按钮（源码/复制/下载）：hover 卡片才显示；全屏按钮常驻并置于最右
  const toolbar = el("div", "html-toolbar html-toolbar-hover")
  const srcBtn = iconButton("查看源码", ICON_SOURCE)
  const copyBtn = iconButton("复制源码", ICON_COPY)
  const dlBtn = iconButton("下载 .html", ICON_DOWNLOAD)
  toolbar.append(srcBtn, copyBtn, dlBtn)
  const fullBtn = el("button", "html-full-btn")
  tip(fullBtn, "全屏")
  fullBtn.innerHTML = ICON_FULLSCREEN
  // 右侧容器：hover 工具栏 + 常驻全屏按钮（保持 space-between 下整体靠右）
  const right = el("div", "html-head-right")
  right.append(toolbar, fullBtn)
  head.append(title, right)
  // 显式尺寸（show html 分支的 width/height 参数）：模型指定后按值渲染；
  // 未指定时 iframe 固定 100% 铺满消息流宽度（无任何内容宽度反馈）
  const explicitW = typeof b.width === "number" && b.width > 0 ? Math.round(b.width) : undefined
  const explicitH = typeof b.height === "number" && b.height > 0 ? Math.round(b.height) : undefined
  const frame = previewFrame(doc)
  if (explicitW !== undefined) frame.style.width = explicitW + "px"
  if (explicitH !== undefined) frame.style.height = explicitH + "px"
  box.append(head, frame)
  container.appendChild(box)

  const open = () => openHtmlViewer(doc, name, raw)
  title.onclick = open
  fullBtn.onclick = open
  frame.onclick = open
  srcBtn.onclick = () => openSourceView(raw, name)
  copyBtn.onclick = async () => {
    try {
      await copyText(raw)
      flashButton(copyBtn, "已复制到剪贴板")
    } catch {
      toast("复制失败")
    }
  }
  dlBtn.onclick = () => downloadHtmlSource(raw, name)
}
