/**
 * 文件内容卡（`code`/`file` 内容块统一渲染）：头部（文件名/语言徽标 + hover 工具栏：复制/原文件/下载）
 * + 内容区按类型渲染——markdown 语言渲染 md、源码语法高亮、图片内联、PDF/沙箱 html 卡内 iframe、
 * 二进制类型占位提示。`file` 块内容按需加载（files/content），进入视口才 fetch（无 IntersectionObserver
 * 环境立即加载）；卡内文本渲染上限同 show 口径，超出截断引导「原文件」查看全文。
 */
import type { ContentBlock } from "@gebai/sdk"
import { el, filesPreview } from "./state"
import { copyText, desktopDownloadHint } from "./ui"
import { highlightedCode, markdownBlock, blockText } from "./markdown"
import { openImageViewer } from "./diagram"
import { previewFrame, sandboxedHtml, iconButton, flashButton, ICON_COPY, ICON_DOWNLOAD, ICON_FULLSCREEN } from "./html-view"

/* 扩展名 → 高亮语言（与服务端 core/diff.ts 的 EXT_LANG 保持一致；file 块无 language 字段按扩展推断）。 */
const EXT_LANG: Record<string, string> = {
  ts: "typescript", tsx: "typescript", js: "javascript", jsx: "javascript", mjs: "javascript", cjs: "javascript",
  json: "json", py: "python", pyw: "python", sh: "bash", bash: "bash", zsh: "bash",
  css: "css", scss: "scss", less: "less", html: "xml", htm: "xml", xml: "xml", svg: "xml", vue: "xml", svelte: "xml",
  md: "markdown", markdown: "markdown", yml: "markdown", yaml: "markdown",
  go: "go", rs: "rust", java: "java", kt: "kotlin", kts: "kotlin", rb: "ruby",
  c: "c", h: "c", cpp: "cpp", hpp: "cpp", cc: "cpp", cs: "csharp", php: "php",
  sql: "sql", lua: "lua", swift: "swift", dart: "dart",
}

export function langForFile(name: string, mime: string): string {
  const ext = name.split(".").pop()?.toLowerCase() ?? ""
  if (!ext && mime.startsWith("text/")) return ""
  return EXT_LANG[ext] ?? ""
}

/** 卡内文本渲染上限（与服务端 SHOW_TEXT_MAX_CHARS 同口径；超出截断 + 引导原文件查看）。 */
const CARD_TEXT_MAX_CHARS = 40_000

/** 文件渲染形态分类（卡内与「原文件」弹窗共用分派）。office = docx/xlsx/pptx 阅读视图
 *  （服务端 files/preview?render=office 输出结构化 HTML，前端沙箱 iframe 渲染）。 */
function fileKind(name: string, mime: string): "image" | "pdf" | "html" | "markdown" | "text" | "office" | "binary" {
  if (mime.startsWith("image/")) return "image"
  if (mime === "application/pdf") return "pdf"
  if (mime.startsWith("application/vnd.openxmlformats-officedocument") || /\.(docx|xlsx|xlsm|pptx)$/i.test(name)) return "office"
  if (mime === "text/html" || /\.html?$/i.test(name)) return "html"
  const lang = langForFile(name, mime)
  if (lang === "markdown") return "markdown"
  if (lang || mime.startsWith("text/")) return "text"
  return "binary"
}

/** 文本内容渲染：markdown 语言 → markdown 渲染；其余语法高亮（复制入口统一在卡工具栏）。 */
function textBody(lang: string, text: string): HTMLElement {
  if (lang === "markdown") return markdownBlock(text)
  const pre = el("pre", "file-code")
  pre.appendChild(highlightedCode(lang, text))
  return pre
}

/** 下载锚点（文件卡头部/文件链接 chip/弹窗共用）：经 files/preview 附件形式获取
 *  （会话相对与项目绝对路径统一入口），桌面 WebView 形态提示下载位置；stopPropagation 防触发
 *  外层可点击容器（chip 弹窗）。 */
export function downloadAnchor(sessionId: string, path: string, name: string): HTMLAnchorElement {
  const a = document.createElement("a")
  a.className = "file-dl-icon"
  a.title = "下载"
  a.innerHTML = ICON_DOWNLOAD
  a.href = filesPreview(sessionId, path, true)
  a.download = name
  a.onclick = (e) => {
    e.stopPropagation()
    desktopDownloadHint(name)
  }
  return a
}

/** 工具栏（hover 渐显）：复制 / 原文件查看（弹窗）。enableCopy 可后置回填（按需加载完成后）。 */
function fileToolbar(opts: { copy?: () => string; source?: () => void }): {
  el: HTMLElement
  enableCopy: (provider: () => string) => void
} {
  const bar = el("div", "file-toolbar")
  let copyBtn: HTMLButtonElement | null = null
  if (opts.copy) {
    copyBtn = iconButton("复制", ICON_COPY)
    copyBtn.classList.add("is-copy")
    const provider = opts.copy
    copyBtn.onclick = async () => {
      try {
        await copyText(provider())
        flashButton(copyBtn!, "已复制")
      } catch {
        /* 剪贴板不可用忽略 */
      }
    }
    bar.appendChild(copyBtn)
  }
  if (opts.source) {
    const btn = iconButton("原文件查看", ICON_FULLSCREEN)
    btn.onclick = opts.source
    bar.appendChild(btn)
  }
  return {
    el: bar,
    enableCopy: (provider) => {
      if (copyBtn) {
        copyBtn.classList.remove("is-copy")
        copyBtn.onclick = async () => {
          try {
            await copyText(provider())
            flashButton(copyBtn!, "已复制")
          } catch {
            /* 剪贴板不可用忽略 */
          }
        }
      }
    },
  }
}

/** 文件卡容器：头部（标题 + 徽标 + hover 工具栏 + **常驻下载**）+ 内容区。 */
function fileCard(title: string, badge: string | undefined, toolbar: HTMLElement, body: HTMLElement, persistentDl?: HTMLElement): HTMLElement {
  const card = el("div", "file-card")
  const head = el("div", "file-head")
  const titleEl = el("span", "file-title", title)
  head.appendChild(titleEl)
  if (badge) head.appendChild(el("span", "file-badge", badge))
  head.appendChild(toolbar)
  if (persistentDl) head.appendChild(persistentDl)
  card.append(head, body)
  return card
}

/** 弹窗外框（原文件查看 / 文件链接点击共用）：标题 + 下载（可选，常驻标题栏）+ 关闭 + 内容区，
 *  Esc/点击遮罩关闭。 */
function previewShell(name: string, download?: { sessionId: string; path: string }): { overlay: HTMLElement; body: HTMLElement } {
  const overlay = el("div", "preview-overlay")
  const card = el("div", "preview-card")
  const head = el("div", "preview-head")
  const closeBtn = el("button", "preview-close", "✕")
  head.appendChild(el("span", "preview-title", name))
  if (download) {
    const dl = downloadAnchor(download.sessionId, download.path, name)
    dl.classList.add("preview-dl")
    head.appendChild(dl)
  }
  head.appendChild(closeBtn)
  const body = el("div", "preview-body")
  card.append(head, body)
  overlay.appendChild(card)
  document.body.appendChild(overlay)
  function closePreview() {
    overlay.remove()
    document.removeEventListener("keydown", onKey)
  }
  const onKey = (e: KeyboardEvent) => {
    if (e.key === "Escape") closePreview()
  }
  document.addEventListener("keydown", onKey)
  overlay.onclick = (e) => {
    if (e.target === overlay) closePreview()
  }
  closeBtn.onclick = closePreview
  return { overlay, body }
}

/** 拉取 Office 阅读视图 HTML（files/preview?render=office）：失败返回 null（调用方回退占位提示）。 */
async function fetchOfficeView(sessionId: string, path: string): Promise<string | null> {
  try {
    const r = await fetch(`${filesPreview(sessionId, path)}&render=office`)
    if (!r.ok) throw new Error(`HTTP ${r.status}`)
    return await r.text()
  } catch {
    return null
  }
}

/** 按文件名推断下载徽标（mime 缺省时的兜底展示）。 */
function kindBadge(kind: ReturnType<typeof fileKind>): string | undefined {
  if (kind === "pdf") return "PDF"
  if (kind === "binary") return "文件"
  return undefined
}

/** 进入视口才执行加载（无 IntersectionObserver 环境立即执行——单测桩）。 */
function whenVisible(target: HTMLElement, load: () => void): void {
  if (typeof IntersectionObserver === "undefined") {
    load()
    return
  }
  const io = new IntersectionObserver(
    (entries) => {
      if (entries.some((e) => e.isIntersecting)) {
        io.disconnect()
        load()
      }
    },
    { rootMargin: "200px" },
  )
  io.observe(target)
}

/** 取数失败文案：404 = 历史卡片指向的文件已被删除/清理（如 agent 测试后清理临时文件），
 *  不再引导下载（同样 404）；其余错误附下载引导（网络抖动等，文件可能仍在）。 */
function fetchFailText(err: unknown, where: "card" | "popup"): string {
  if ((err as Error).message.includes("404")) return "文件已不存在（可能已被删除或清理）。"
  const hint = where === "card" ? "可点击卡片头部「下载」获取文件。" : "可点击标题栏「下载」获取文件。"
  return `${where === "card" ? "内容加载失败" : "无法预览该文件"}: ${(err as Error).message}。${hint}`
}

/** code 内容块 → 文件内容卡：markdown 语言渲染 md、其余语法高亮；path 附带时提供原文件查看与常驻下载。 */
export function renderCodeCard(container: HTMLElement, b: Extract<ContentBlock, { type: "code" }>, sessionId: string): void {
  const lang = b.language ?? ""
  const title = b.name || (lang && lang !== "markdown" ? `${lang} 代码` : "文本")
  const body = el("div", "file-body")
  body.appendChild(textBody(lang, b.text))
  const hasPath = !!b.path
  const toolbar = fileToolbar({
    copy: () => b.text,
    source: hasPath ? () => openFilePreview(sessionId, b.name ?? "file", b.path!, mimeFor(b.name ?? "", "code")) : undefined,
  })
  // 下载常驻文件卡头部（不随 hover 工具栏显隐）
  const dl = b.path ? downloadAnchor(sessionId, b.path, b.name ?? "file") : undefined
  container.appendChild(fileCard(title, lang && lang !== "markdown" ? lang : undefined, toolbar.el, body, dl))
}

/** file 内容块 → 文件内容卡：按 mime/扩展分派，内容进入视口后按需加载。 */
export function renderFileCard(container: HTMLElement, b: Extract<ContentBlock, { type: "file" }>, sessionId: string): void {
  const name = b.name || b.path
  const mime = b.mime ?? ""
  const kind = fileKind(name, mime)
  const body = el("div", "file-body")
  const toolbar = fileToolbar({
    source: () => openFilePreview(sessionId, name, b.path, mime),
  })
  const card = fileCard(name, kindBadge(kind), toolbar.el, body, downloadAnchor(sessionId, b.path, name))
  container.appendChild(card)

  const fail = (err: unknown) => body.appendChild(blockText(fetchFailText(err, "card")))
  if (kind === "image") {
    const img = document.createElement("img")
    img.src = filesPreview(sessionId, b.path)
    img.alt = name
    img.className = "file-img"
    img.loading = "lazy"
    img.onclick = () => openImageViewer(filesPreview(sessionId, b.path), name)
    body.appendChild(img)
    return
  }
  if (kind === "pdf") {
    const frame = document.createElement("iframe")
    frame.src = filesPreview(sessionId, b.path)
    frame.title = name
    frame.className = "file-frame"
    frame.loading = "lazy"
    body.appendChild(frame)
    return
  }
  // 其余形态需 fetch：进入视口后按需加载（历史长会话不产生全量请求）
  whenVisible(card, () => {
    if (body.firstChild && (body.firstChild as HTMLElement).className === "file-pending") body.textContent = ""
    if (kind === "office") {
      // Office 阅读视图（服务端渲染 HTML）：失败回退占位提示（下载入口在卡片头部常驻）
      void fetchOfficeView(sessionId, b.path).then((html) => {
        if (html === null) {
          body.appendChild(el("div", "file-fallback", "📦 该文档无法渲染为阅读视图（可能已损坏），可点击卡片头部「下载」获取文件"))
          return
        }
        body.appendChild(previewFrame(sandboxedHtml(html)))
      })
      return
    }
    if (kind === "binary") {
      // 二进制无内联形态：占位提示（下载入口在卡片头部常驻）
      body.appendChild(el("div", "file-fallback", "📦 该类型无内联预览，可点击卡片头部「下载」获取文件"))
      return
    }
    void fetch(filesPreview(sessionId, b.path))
      .then(async (r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`)
        const text = await r.text()
        if (kind === "html") {
          body.appendChild(previewFrame(sandboxedHtml(text)))
          return
        }
        let shown = text
        if (text.length > CARD_TEXT_MAX_CHARS) {
          shown = `${text.slice(0, CARD_TEXT_MAX_CHARS)}\n…（内容过长已截断，全文 ${text.length} 字符可点「原文件查看」或下载）`
        }
        body.appendChild(textBody(kind === "markdown" ? "markdown" : langForFile(name, mime), shown))
        toolbar.enableCopy(() => text)
      })
      .catch(fail)
  })
  body.appendChild(el("div", "file-pending", "预览加载中…"))
}

/** 按名称/来源推断 mime（code 块无 mime 字段，供「原文件」弹窗分派）。 */
function mimeFor(name: string, _origin: string): string {
  const kind = fileKind(name, "")
  if (kind === "image") return "image/*"
  if (kind === "pdf") return "application/pdf"
  if (kind === "html") return "text/html"
  if (kind === "markdown") return "text/markdown"
  return "text/plain"
}

/** 原文件查看弹窗（工具栏「原文件」/ 文件链接点击入口）：标题栏含下载（经 files/preview 附件形式），
 *  内容按类型分派——图片直显 / PDF 内嵌 / md 渲染 / html 沙箱 iframe / 文本高亮。
 *  取数统一经 files/preview（会话相对 tmp/ 路径与项目绝对路径均支持，服务端按用户隔离边界解析）。 */
export function openFilePreview(sessionId: string, name: string, path: string, mime?: string): void {
  const { body } = previewShell(name, { sessionId, path })

  const kind = fileKind(name, mime ?? "")
  if (kind === "image") {
    const img = document.createElement("img")
    img.src = filesPreview(sessionId, path)
    img.alt = name
    body.appendChild(img)
    return
  }
  if (kind === "pdf") {
    const frame = document.createElement("iframe")
    frame.src = filesPreview(sessionId, path)
    frame.title = name
    frame.style.cssText = "width:100%;height:72vh;border:0;border-radius:8px;background:#fff"
    body.appendChild(frame)
    return
  }
  if (kind === "office") {
    // Office 阅读视图：服务端渲染的结构化 HTML（标题/表格/图片/幻灯片大纲），沙箱 iframe 承载
    void fetchOfficeView(sessionId, path).then((html) => {
      if (html === null) {
        body.appendChild(blockText("该文档无法渲染为阅读视图（可能已损坏），可点击标题栏「下载」获取文件。"))
        return
      }
      body.appendChild(previewFrame(sandboxedHtml(html)))
    })
    return
  }
  void fetch(filesPreview(sessionId, path))
    .then(async (r) => {
      if (!r.ok) throw new Error(`HTTP ${r.status}`)
      const text = await r.text()
      if (kind === "html") {
        body.appendChild(previewFrame(sandboxedHtml(text)))
        return
      }
      if (kind === "markdown") {
        body.appendChild(markdownBlock(text))
        return
      }
      const pre = el("pre")
      pre.className = "preview-code"
      pre.appendChild(highlightedCode(langForFile(name, mime ?? ""), text))
      body.appendChild(pre)
    })
    .catch((err) => {
      body.appendChild(blockText(fetchFailText(err, "popup")))
    })
}
