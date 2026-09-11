/**
 * 文件工作台 · 非文本查看器：图片（缩放/旋转/适应）、音视频、PDF、Office 阅读视图、压缩包、
 * 字体、二进制 hex、图表渲染（mermaid/plantuml/d2/echarts 本地引擎）。
 *
 * 设计：
 * - 每个查看器返回一个 dispose 函数（切换标签/换文件时统一回收，避免 blob URL、播放器、定时器泄漏）；
 * - 能交给浏览器的绝不自己实现：图片/音视频/PDF 直接走 `fs/raw`（原生解码 + HTTP Range 拖动），
 *   只有浏览器无能为力的（Office 结构化阅读、zip 目录、二进制 hex、图表）才在前端加工；
 * - 图表引擎复用 public/vendor 的稳定文件名资源（与主界面图表同一套引擎，离线可用）。
 */
import type { FsApi, FileStat } from "./api"
import { h, icon, toast, formatSize, formatTime, extOf } from "./ui"

export interface ViewerCtx {
  api: FsApi
  root: string
  path: string
  name: string
  kind: string
  stat: FileStat
  notify: (msg: string, kind?: "info" | "success" | "error") => void
}

function rawUrl(ctx: ViewerCtx, extra: Record<string, string | undefined> = {}): string {
  return ctx.api.url("/api/v1/fs/raw", { root: ctx.root, path: ctx.path, ...extra })
}

export function downloadUrl(ctx: Pick<ViewerCtx, "api" | "root" | "path">): string {
  return ctx.api.url("/api/v1/fs/download", { root: ctx.root, path: ctx.path })
}

/** 生成查看器工具条。 */
function viewerBar(children: Array<Node | string | null>): HTMLElement {
  return h("div", { class: "fw-viewer-bar" }, children)
}

function barButton(label: string, iconName: string, onClick: () => void, title?: string): HTMLButtonElement {
  const btn = h("button", { class: "fw-btn ghost sm", title: title ?? label }, [icon(iconName), h("span", { text: label })])
  btn.onclick = onClick
  return btn
}

/** 空状态提示（含引导操作）。 */
function placeholder(message: string, hint?: string, actions: Node[] = []): HTMLElement {
  return h("div", { class: "fw-placeholder" }, [h("div", { class: "fw-placeholder-msg", text: message }), hint ? h("div", { class: "fw-placeholder-hint", text: hint }) : null, h("div", { class: "fw-placeholder-actions" }, actions)])
}

const BASE = () => (import.meta.env.BASE_URL || "/").replace(/\/$/, "")

/* ------------------------------ 图片 ------------------------------ */

function renderImage(host: HTMLElement, ctx: ViewerCtx): () => void {
  let scale = 1
  let rotate = 0
  let fit = true
  const img = h("img", { class: "fw-image", src: rawUrl(ctx), alt: ctx.name, draggable: "false" })
  const stage = h("div", { class: "fw-image-stage" }, [img])
  const zoomLabel = h("span", { class: "fw-viewer-info", text: "100%" })

  const apply = () => {
    img.style.transform = `rotate(${rotate}deg) scale(${scale})`
    img.style.maxWidth = fit ? "100%" : "none"
    img.style.maxHeight = fit ? "100%" : "none"
    zoomLabel.textContent = `${Math.round(scale * 100)}%${fit ? " · 适应" : ""}`
  }
  const zoom = (delta: number) => {
    fit = false
    scale = Math.max(0.05, Math.min(20, scale * delta))
    apply()
  }
  const bar = viewerBar([
    barButton("适应", "expand", () => {
      fit = true
      scale = 1
      apply()
    }),
    barButton("1:1", "zoomIn", () => {
      fit = false
      scale = 1
      apply()
    }),
    barButton("放大", "zoomIn", () => zoom(1.25)),
    barButton("缩小", "zoomOut", () => zoom(0.8)),
    barButton("旋转", "rotate", () => {
      rotate = (rotate + 90) % 360
      apply()
    }),
    zoomLabel,
    h("span", { class: "fw-viewer-spacer" }),
    h("span", { class: "fw-viewer-info", text: `${ctx.stat.size ? formatSize(ctx.stat.size) : ""}` }),
  ])

  stage.addEventListener(
    "wheel",
    (e) => {
      if (!e.ctrlKey && !e.metaKey && !e.altKey) return
      e.preventDefault()
      zoom(e.deltaY < 0 ? 1.15 : 1 / 1.15)
    },
    { passive: false },
  )
  // 拖拽平移（放大后查看局部）：window 上的 mousemove/mouseup **只在拖动期间挂**，松手即摘。
  // 早期是建视图时注册、永不摘——每开一张图就漏一对全局监听器（闭包还持着 stage）
  let dragging = false
  let sx = 0
  let sy = 0
  let sl = 0
  let st = 0
  const onMove = (e: MouseEvent): void => {
    if (!dragging) return
    stage.scrollLeft = sl - (e.clientX - sx)
    stage.scrollTop = st - (e.clientY - sy)
  }
  const onUp = (): void => {
    if (!dragging) return
    dragging = false
    stage.classList.remove("dragging")
    window.removeEventListener("mousemove", onMove)
    window.removeEventListener("mouseup", onUp)
  }
  stage.addEventListener("mousedown", (e) => {
    if (fit) return
    dragging = true
    sx = e.clientX
    sy = e.clientY
    sl = stage.scrollLeft
    st = stage.scrollTop
    stage.classList.add("dragging")
    window.addEventListener("mousemove", onMove)
    window.addEventListener("mouseup", onUp)
  })
  apply()
  host.appendChild(bar)
  host.appendChild(stage)

  const onDouble = () => {
    fit = !fit
    scale = 1
    apply()
  }
  img.addEventListener("dblclick", onDouble)
  img.addEventListener("error", () => {
    host.appendChild(placeholder("图片加载失败", "文件可能已损坏或类型不受浏览器支持，可尝试下载后用本地程序打开。"))
  })
  return () => {
    img.removeEventListener("dblclick", onDouble)
    onUp() // 拖动中被关闭：顺手把 window 监听一起摘掉
  }
}

/* ------------------------------ 音视频 ------------------------------ */

function renderMedia(host: HTMLElement, ctx: ViewerCtx): () => void {
  const isVideo = ctx.kind === "video"
  const el = isVideo
    ? h("video", { class: "fw-media", src: rawUrl(ctx), controls: true, preload: "metadata", playsinline: true })
    : h("audio", { class: "fw-media-audio", src: rawUrl(ctx), controls: true, preload: "metadata" })
  const wrap = h("div", { class: "fw-media-wrap" }, [
    isVideo ? el : h("div", { class: "fw-audio-card" }, [icon("play", 42), h("div", { class: "fw-audio-name", text: ctx.name }), el]),
    viewerBar([
      h("span", { class: "fw-viewer-info", text: `${formatSize(ctx.stat.size)} · ${ctx.stat.mime}` }),
      h("span", { class: "fw-viewer-spacer" }),
      barButton("下载", "download", () => window.open(downloadUrl(ctx), "_blank")),
    ]),
  ])
  host.appendChild(wrap)
  return () => {
    try {
      ;(el as HTMLMediaElement).pause()
    } catch {
      /* 忽略 */
    }
    el.removeAttribute("src")
  }
}

/* ------------------------------ PDF ------------------------------ */

function renderPdf(host: HTMLElement, ctx: ViewerCtx): () => void {
  // 浏览器内置 PDF 阅读器（Chromium/Edge/Safari 支持；Range 已开，跳页与滚动流畅）
  const frame = h("iframe", { class: "fw-frame", src: rawUrl(ctx), title: ctx.name })
  host.appendChild(
    h("div", { class: "fw-frame-wrap" }, [
      viewerBar([
        h("span", { class: "fw-viewer-info", text: `PDF · ${formatSize(ctx.stat.size)}` }),
        h("span", { class: "fw-viewer-spacer" }),
        barButton("新标签打开", "expand", () => window.open(rawUrl(ctx), "_blank")),
        barButton("下载", "download", () => window.open(downloadUrl(ctx), "_blank")),
      ]),
      frame,
    ]),
  )
  return () => frame.remove()
}

/* ------------------------------ Office ------------------------------ */

function renderOffice(host: HTMLElement, ctx: ViewerCtx): () => void {
  const frame = h("iframe", { class: "fw-frame", src: ctx.api.url("/api/v1/fs/office", { root: ctx.root, path: ctx.path }), title: ctx.name })
  host.appendChild(
    h("div", { class: "fw-frame-wrap" }, [
      viewerBar([
        h("span", { class: "fw-viewer-info", text: `${extOf(ctx.name).toUpperCase()} 阅读视图（结构化解析，非原版式）` }),
        h("span", { class: "fw-viewer-spacer" }),
        barButton("下载原文件", "download", () => window.open(downloadUrl(ctx), "_blank"), "需要完整排版请下载后用 WPS/Office 打开"),
      ]),
      frame,
    ]),
  )
  return () => frame.remove()
}

/* ------------------------------ 压缩包 ------------------------------ */

function renderArchive(host: HTMLElement, ctx: ViewerCtx): () => void {
  const host2 = h("div", { class: "fw-archive" })
  host.appendChild(host2)
  let disposed = false
  const load = async () => {
    host2.replaceChildren(h("div", { class: "fw-loading", text: "正在读取压缩包目录…" }))
    try {
      const res = await ctx.api.archive(ctx.root, ctx.path)
      if (disposed) return
      const rows = res.entries
        .slice()
        .sort((a, b) => a.name.localeCompare(b.name))
        .map((e) =>
          h("div", { class: `fw-archive-row${e.isDir ? " dir" : ""}` }, [
            icon(e.isDir ? "folder" : "file"),
            h("span", { class: "fw-archive-name", text: e.name, title: e.name }),
            h("span", { class: "fw-archive-size", text: e.isDir ? "" : formatSize(e.size) }),
            h("span", { class: "fw-archive-ratio", text: e.isDir || !e.compressedSize ? "" : `${Math.round((1 - e.compressedSize / Math.max(1, e.size)) * 100)}%` }),
            h("span", { class: "fw-archive-time", text: e.mtime ? formatTime(e.mtime) : "" }),
            e.isDir
              ? null
              : (() => {
                  const dl = h("button", { class: "fw-icon-btn", title: "导出该条目" }, [icon("download")])
                  dl.onclick = () => window.open(ctx.api.url("/api/v1/fs/archive", { root: ctx.root, path: ctx.path, entry: e.name, download: true }), "_blank")
                  return dl
                })(),
          ]),
        )
      const extractBtn = barButton("全部解压到同级目录", "archive", async () => {
        const base = ctx.path.includes("/") ? ctx.path.slice(0, ctx.path.lastIndexOf("/")) : ""
        const dirName = ctx.name.replace(/\.(zip|jar|war)$/i, "")
        const target = [base, dirName].filter(Boolean).join("/")
        try {
          const out = await ctx.api.archiveExtract(ctx.root, ctx.path, target)
          ctx.notify(`已解压 ${out.extracted} 个文件到 ${target}`, "success")
        } catch (err) {
          ctx.notify(`解压失败：${(err as Error).message}`, "error")
        }
      })
      host2.replaceChildren(
        viewerBar([
          h("span", { class: "fw-viewer-info", text: `${res.entries.length} 个条目 · ${formatSize(res.size)}` }),
          h("span", { class: "fw-viewer-spacer" }),
          extractBtn,
          barButton("下载", "download", () => window.open(downloadUrl(ctx), "_blank")),
        ]),
        h("div", { class: "fw-archive-head" }, [
          h("span", { class: "fw-archive-name", text: "名称" }),
          h("span", { class: "fw-archive-size", text: "大小" }),
          h("span", { class: "fw-archive-ratio", text: "压缩率" }),
          h("span", { class: "fw-archive-time", text: "修改时间" }),
        ]),
        h("div", { class: "fw-archive-list" }, rows),
      )
    } catch (err) {
      if (disposed) return
      host2.replaceChildren(
        placeholder(`无法浏览该压缩包：${(err as Error).message}`, "仅支持 ZIP 系（zip/jar/war）；tar/gz/7z/rar 请下载后本地解压。", [
          (() => {
            const b = h("button", { class: "fw-btn primary" }, [icon("download"), h("span", { text: "下载文件" })])
            b.onclick = () => window.open(downloadUrl(ctx), "_blank")
            return b
          })(),
        ]),
      )
    }
  }
  void load()
  return () => {
    disposed = true
  }
}

/* ------------------------------ 字体 ------------------------------ */

function renderFont(host: HTMLElement, ctx: ViewerCtx): () => void {
  const family = `fw-preview-${Date.now()}`
  const style = document.createElement("style")
  style.textContent = `@font-face{font-family:"${family}";src:url("${rawUrl(ctx)}");}`
  document.head.appendChild(style)
  const sample = h("div", { class: "fw-font-sample", style: `font-family:"${family}",sans-serif` }, [
    h("div", { class: "fw-font-line", style: "font-size:34px", text: "歌白 Gebai 字体预览 0123456789" }),
    h("div", { class: "fw-font-line", style: "font-size:20px", text: "永和九年，岁在癸丑，暮春之初，会于会稽山阴之兰亭。" }),
    h("div", { class: "fw-font-line", style: "font-size:14px", text: "The quick brown fox jumps over the lazy dog. !@#$%^&*()_+-=[]{}" }),
  ])
  host.appendChild(
    h("div", { class: "fw-font-wrap" }, [
      viewerBar([h("span", { class: "fw-viewer-info", text: `${extOf(ctx.name).toUpperCase()} · ${formatSize(ctx.stat.size)}` }), h("span", { class: "fw-viewer-spacer" }), barButton("下载", "download", () => window.open(downloadUrl(ctx), "_blank"))]),
      sample,
    ]),
  )
  return () => style.remove()
}

/* ------------------------------ 二进制 hex ------------------------------ */

async function renderBinary(host: HTMLElement, ctx: ViewerCtx): Promise<() => void> {
  const limit = 256 * 1024
  const wrap = h("div", { class: "fw-hex-wrap" })
  host.appendChild(wrap)
  wrap.replaceChildren(h("div", { class: "fw-loading", text: "正在读取文件头部…" }))
  let bytes: Uint8Array
  try {
    const res = await fetch(rawUrl(ctx), { headers: { Range: `bytes=0-${limit - 1}` } })
    bytes = new Uint8Array(await res.arrayBuffer())
  } catch (err) {
    wrap.replaceChildren(placeholder(`读取失败：${(err as Error).message}`))
    return () => {}
  }
  const lines: string[] = []
  const decoder = new TextDecoder("utf-8")
  for (let off = 0; off < bytes.length; off += 16) {
    const chunk = bytes.subarray(off, off + 16)
    const hex = Array.from(chunk, (b) => b.toString(16).padStart(2, "0"))
    const hexStr = [hex.slice(0, 8).join(" "), hex.slice(8).join(" ")].join("  ")
    const ascii = decoder
      .decode(chunk)
      .replace(/[\x00-\x1f\x7f-\x9f]/g, ".")
      .replace(/[^\x20-\x7e]/g, "·")
    lines.push(`${off.toString(16).padStart(8, "0")}  ${hexStr.padEnd(49)}  |${ascii.padEnd(16)}|`)
  }
  wrap.replaceChildren(
    viewerBar([
      h("span", { class: "fw-viewer-info", text: `${ctx.stat.mime} · ${formatSize(ctx.stat.size)}${ctx.stat.size > limit ? `（仅显示前 ${formatSize(limit)}）` : ""}` }),
      h("span", { class: "fw-viewer-spacer" }),
      barButton("下载", "download", () => window.open(downloadUrl(ctx), "_blank")),
    ]),
    h("div", { class: "fw-hex-hint", text: "该格式不支持内嵌预览，以下为十六进制转储（只读）。" }),
    h("pre", { class: "fw-hex", text: lines.join("\n") }),
  )
  return () => {}
}

/* ------------------------------ 图表渲染 ------------------------------ */

type DiagramKind = "mermaid" | "plantuml" | "d2" | "echarts"

export function diagramKindOf(ext: string): DiagramKind | null {
  if (ext === "mmd" || ext === "mermaid") return "mermaid"
  if (ext === "puml" || ext === "plantuml" || ext === "pu" || ext === "iuml") return "plantuml"
  if (ext === "d2") return "d2"
  if (ext === "echarts") return "echarts"
  return null
}

function loadScript(src: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const s = document.createElement("script")
    s.src = src
    s.async = true
    s.onload = () => resolve()
    s.onerror = () => {
      s.remove()
      reject(new Error(`引擎加载失败：${src}`))
    }
    document.head.appendChild(s)
  })
}

let mermaidReady: Promise<void> | null = null
let echartsReady: Promise<void> | null = null
let vizReady: Promise<void> | null = null
let plantumlMod: Promise<{ renderToString: (lines: string[], ok: (svg: string) => void, err: (m: string) => void) => void }> | null = null
let d2Queue: Promise<unknown> = Promise.resolve()

function isDarkTheme(): boolean {
  const bg = getComputedStyle(document.documentElement).getPropertyValue("--bg").trim()
  const probe = document.createElement("span")
  probe.style.color = bg
  document.body.appendChild(probe)
  const computed = getComputedStyle(probe).color
  probe.remove()
  const m = /rgba?\(([^)]+)\)/.exec(computed)
  if (!m) return true
  const [r, g, b] = m[1].split(",").map(Number)
  return (r * 0.299 + g * 0.587 + b * 0.114) / 255 < 0.5
}

/** 渲染图表源码为 SVG 字符串（懒加载本地引擎；与主界面图表共用同一套 vendor 资源）。 */
export async function renderDiagramSvg(kind: DiagramKind, code: string): Promise<string> {
  const base = BASE()
  if (kind === "mermaid") {
    if (!mermaidReady) {
      mermaidReady = loadScript(`${base}/vendor/mermaid.js`).then(() => {
        const m = (window as unknown as { mermaid?: { initialize: (o: unknown) => void } }).mermaid
        m?.initialize({ startOnLoad: false, theme: isDarkTheme() ? "dark" : "default", securityLevel: "strict" })
      })
    }
    await mermaidReady
    const m = (window as unknown as { mermaid: { render: (id: string, code: string) => Promise<{ svg: string }> } }).mermaid
    const out = await m.render(`fw-mmd-${Date.now()}`, code)
    return out.svg
  }
  if (kind === "echarts") {
    if (!echartsReady) echartsReady = loadScript(`${base}/vendor/echarts.js`)
    await echartsReady
    const echarts = (window as unknown as { echarts: { init: (el: HTMLElement, theme?: unknown, opts?: unknown) => { setOption: (o: unknown) => void; getDom: () => HTMLElement; dispose: () => void } } }).echarts
    const option = JSON.parse(code) as { option?: unknown; width?: number; height?: number }
    const box = document.createElement("div")
    box.style.width = `${option.width ?? 900}px`
    box.style.height = `${option.height ?? 520}px`
    box.style.position = "absolute"
    box.style.left = "-10000px"
    document.body.appendChild(box)
    const chart = echarts.init(box, undefined, { renderer: "svg" })
    chart.setOption(option.option ?? option)
    const svg = box.querySelector("svg")?.outerHTML ?? ""
    chart.dispose()
    box.remove()
    return svg
  }
  if (kind === "plantuml") {
    if (!vizReady) vizReady = loadScript(`${base}/vendor/viz-global.js`)
    await vizReady
    if (!plantumlMod) {
      plantumlMod = import(/* @vite-ignore */ `${base}/vendor/plantuml.js`) as unknown as Promise<{ renderToString: (lines: string[], ok: (svg: string) => void, err: (m: string) => void) => void }>
    }
    const mod = await plantumlMod
    const src = /@start/.test(code) ? code : `@startuml\n${code}\n@enduml`
    return await new Promise<string>((resolve, reject) => {
      mod.renderToString(src.split("\n"), (svg) => resolve(svg), (msg) => reject(new Error(msg)))
    })
  }
  // d2：浏览器构建为单 Worker，必须串行（并发会互相覆盖回调）
  const run = d2Queue.then(async () => {
    const mod = (await import(/* @vite-ignore */ `${base}/vendor/d2js/index.js`)) as { D2: new () => { compile: (c: string, o: unknown) => Promise<{ diagram: unknown }>; render: (d: unknown, o: unknown) => Promise<string> } }
    const d2 = new mod.D2()
    const opts = { themeID: isDarkTheme() ? 200 : 0, noXMLTag: true }
    const compiled = await d2.compile(code, opts)
    return await d2.render(compiled.diagram, { ...opts, ...(compiled as { renderOptions?: object }).renderOptions })
  })
  d2Queue = run.catch(() => {})
  return run
}

/** 图表预览面板（源码由 Monaco 编辑器展示，此面板负责渲染为图）。 */
export function renderDiagramPreview(host: HTMLElement, ctx: ViewerCtx): () => void {
  const ext = extOf(ctx.name)
  const kind = diagramKindOf(ext)
  const box = h("div", { class: "fw-diagram-box" })
  const wrap = h("div", { class: "fw-diagram-wrap" }, [
    viewerBar([
      h("span", { class: "fw-viewer-info", text: `${(kind ?? "diagram").toUpperCase()} 渲染预览` }),
      h("span", { class: "fw-viewer-spacer" }),
      barButton("重新渲染", "refresh", () => void run()),
    ]),
    box,
  ])
  host.appendChild(wrap)
  let disposed = false
  const run = async () => {
    if (!kind) {
      box.replaceChildren(placeholder("未知图表类型", "支持 .mmd/.mermaid、.puml/.plantuml、.d2、.echarts"))
      return
    }
    box.replaceChildren(h("div", { class: "fw-loading", text: "渲染中…（首次加载本地引擎可能稍慢）" }))
    try {
      const read = await ctx.api.read(ctx.root, ctx.path)
      const svg = await renderDiagramSvg(kind, read.content)
      if (disposed) return
      box.replaceChildren(h("div", { class: "fw-diagram-svg", html: svg }))
    } catch (err) {
      if (disposed) return
      box.replaceChildren(placeholder(`渲染失败：${(err as Error).message}`, "可切回「源码」查看原始代码。"))
    }
  }
  void run()
  return () => {
    disposed = true
  }
}

/* ------------------------------ 分派 ------------------------------ */

/** 按 kind 渲染查看器；返回 dispose。文本类（text）由 Monaco 编辑器承载，不在此列。 */
export function renderViewer(host: HTMLElement, ctx: ViewerCtx): () => void {
  switch (ctx.kind) {
    case "image":
      return renderImage(host, ctx)
    case "video":
    case "audio":
      return renderMedia(host, ctx)
    case "pdf":
      return renderPdf(host, ctx)
    case "office":
      return renderOffice(host, ctx)
    case "archive":
      return renderArchive(host, ctx)
    case "font":
      return renderFont(host, ctx)
    case "diagram":
      return renderDiagramPreview(host, ctx)
    default: {
      // binary：异步读取头部后替换占位内容
      const holder = h("div", { class: "fw-binary-host" })
      host.appendChild(holder)
      let disposed = false
      void renderBinary(holder, ctx).then((dispose) => {
        if (disposed) dispose()
      })
      return () => {
        disposed = true
      }
    }
  }
}

/** 兜底：文件既非文本也不在已知类别（例如未知扩展名的二进制）时给出手动切换入口。 */
export function renderUnknown(host: HTMLElement, ctx: ViewerCtx, onForceText: () => void): () => void {
  const box = placeholder("无法自动识别该文件的预览方式", "可以尝试以文本方式打开（编码自动探测），或直接下载。", [
    (() => {
      const b = h("button", { class: "fw-btn primary" }, [icon("eye"), h("span", { text: "以文本打开" })])
      b.onclick = onForceText
      return b
    })(),
    (() => {
      const b = h("button", { class: "fw-btn" }, [icon("download"), h("span", { text: "下载" })])
      b.onclick = () => window.open(downloadUrl(ctx), "_blank")
      return b
    })(),
  ])
  host.appendChild(box)
  return () => box.remove()
}

void toast
