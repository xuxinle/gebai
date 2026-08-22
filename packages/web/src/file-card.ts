/**
 * 文件内容卡（`code`/`file` 内容块统一渲染）：头部（文件名/语言徽标 + hover 工具栏：复制/原文件/下载）
 * + 内容区按类型渲染——markdown 语言渲染 md、源码语法高亮、图片内联、PDF/沙箱 html 卡内 iframe、
 * 二进制类型占位提示。`file` 块内容按需加载（files/content），进入视口才 fetch（无 IntersectionObserver
 * 环境立即加载）；卡内文本渲染上限同 show 口径，超出截断引导「原文件」查看全文。
 */
import type { ContentBlock } from "@gebai/sdk"
import { el, filesContent, filesDownload } from "./state"
import { copyText } from "./ui"
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

/** 文件渲染形态分类（卡内与「原文件」弹窗共用分派）。 */
function fileKind(name: string, mime: string): "image" | "pdf" | "html" | "markdown" | "text" | "binary" {
  if (mime.startsWith("image/")) return "image"
  if (mime === "application/pdf") return "pdf"
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

/** 工具栏（hover 渐显）：复制 / 原文件查看（弹窗）/ 下载。enableCopy 可后置回填（按需加载完成后）。 */
function fileToolbar(opts: { copy?: () => string; source?: () => void; download?: { sessionId: string; path: string; name: string } }): {
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
  if (opts.download) {
    const a = document.createElement("a")
    a.className = "icon-btn file-dl-icon"
    a.title = "下载"
    a.innerHTML = ICON_DOWNLOAD
    a.href = filesDownload(opts.download.sessionId, opts.download.path)
    a.download = opts.download.name
    bar.appendChild(a)
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

/** 文件卡容器：头部（标题 + 徽标 + 工具栏）+ 内容区。 */
function fileCard(title: string, badge: string | undefined, toolbar: HTMLElement, body: HTMLElement): HTMLElement {
  const card = el("div", "file-card")
  const head = el("div", "file-head")
  const titleEl = el("span", "file-title", title)
  head.appendChild(titleEl)
  if (badge) head.appendChild(el("span", "file-badge", badge))
  head.appendChild(toolbar)
  card.append(head, body)
  return card
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

/** code 内容块 → 文件内容卡：markdown 语言渲染 md、其余语法高亮；path 附带时提供原文件/下载工具栏。 */
export function renderCodeCard(container: HTMLElement, b: Extract<ContentBlock, { type: "code" }>, sessionId: string): void {
  const lang = b.language ?? ""
  const title = b.name || (lang && lang !== "markdown" ? `${lang} 代码` : "文本")
  const body = el("div", "file-body")
  body.appendChild(textBody(lang, b.text))
  const hasPath = !!b.path
  const toolbar = fileToolbar({
    copy: () => b.text,
    source: hasPath ? () => openFilePreview(sessionId, b.name ?? "file", b.path!, mimeFor(b.name ?? "", "code")) : undefined,
    download: b.path ? { sessionId, path: b.path, name: b.name ?? "file" } : undefined,
  })
  container.appendChild(fileCard(title, lang && lang !== "markdown" ? lang : undefined, toolbar.el, body))
}

/** file 内容块 → 文件内容卡：按 mime/扩展分派，内容进入视口后按需加载。 */
export function renderFileCard(container: HTMLElement, b: Extract<ContentBlock, { type: "file" }>, sessionId: string): void {
  const name = b.name || b.path
  const mime = b.mime ?? ""
  const kind = fileKind(name, mime)
  const body = el("div", "file-body")
  const toolbar = fileToolbar({
    source: () => openFilePreview(sessionId, name, b.path, mime),
    download: { sessionId, path: b.path, name },
  })
  const card = fileCard(name, kindBadge(kind), toolbar.el, body)
  container.appendChild(card)

  const fail = (err: unknown) => body.appendChild(blockText(`内容加载失败：${(err as Error).message}。可点击工具栏「下载」获取文件。`))
  if (kind === "image") {
    const img = document.createElement("img")
    img.src = filesContent(sessionId, b.path)
    img.alt = name
    img.className = "file-img"
    img.loading = "lazy"
    img.onclick = () => openImageViewer(filesContent(sessionId, b.path), name)
    body.appendChild(img)
    return
  }
  if (kind === "pdf") {
    const frame = document.createElement("iframe")
    frame.src = filesContent(sessionId, b.path)
    frame.title = name
    frame.className = "file-frame"
    frame.loading = "lazy"
    body.appendChild(frame)
    return
  }
  // 其余形态需 fetch：进入视口后按需加载（历史长会话不产生全量请求）
  whenVisible(card, () => {
    if (body.firstChild && (body.firstChild as HTMLElement).className === "file-pending") body.textContent = ""
    if (kind === "binary") {
      // 二进制无内联形态：占位提示（下载入口在工具栏常驻）
      body.appendChild(el("div", "file-fallback", "📦 该类型无内联预览，可点击工具栏「下载」获取文件"))
      return
    }
    void fetch(filesContent(sessionId, b.path))
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

/** 原文件查看弹窗（工具栏「原文件」入口）：图片直显 / PDF 内嵌 / md 渲染 / html 沙箱 iframe / 文本高亮。 */
export function openFilePreview(sessionId: string, name: string, path: string, mime?: string): void {
  const overlay = el("div", "preview-overlay")
  const card = el("div", "preview-card")
  const head = el("div", "preview-head")
  const closeBtn = el("button", "preview-close", "✕")
  head.append(el("span", "preview-title", name), closeBtn)
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

  const kind = fileKind(name, mime ?? "")
  if (kind === "image") {
    const img = document.createElement("img")
    img.src = filesContent(sessionId, path)
    img.alt = name
    body.appendChild(img)
    return
  }
  if (kind === "pdf") {
    const frame = document.createElement("iframe")
    frame.src = filesContent(sessionId, path)
    frame.title = name
    frame.style.cssText = "width:100%;height:72vh;border:0;border-radius:8px;background:#fff"
    body.appendChild(frame)
    return
  }
  void fetch(filesContent(sessionId, path))
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
      body.appendChild(blockText(`无法预览该文件: ${(err as Error).message}。可点击「下载」查看。`))
    })
}
