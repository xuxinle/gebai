/**
 * Office 阅读视图渲染（DESIGN「文件链接弹窗查看」office 分支）：`files/preview?render=office`
 * 的服务端取数——docx/xlsx/xlsm/pptx → 结构化 HTML（标题层级/表格/列表/图片 data URI 内嵌/
 * 带样式的单元格/合并单元格跨行跨列/幻灯片大纲）。解析单一真相源在 wps（复用 ooxml 读取模型
 * 与 exceljs），app 层经此模块惰性引入；内容全量 HTML 转义、样式自带，不还原精确分页与版式
 * （阅读视图口径，非排版视图）。
 */
import { Workbook } from "exceljs"
import type { Cell } from "exceljs"
import { readDocx, readPptx, unzipFiles } from "./ooxml"
import type { DocxBlock } from "./ooxml"
import { cellText } from "./excel"

/** 支持阅读视图的扩展名（与 files/preview?render=office 的分派一致）。 */
export const OFFICE_PREVIEW_EXTS = new Set(["docx", "xlsx", "xlsm", "pptx"])

export async function renderOfficeReadingView(absPath: string): Promise<string> {
  const bytes = new Uint8Array(await Bun.file(absPath).arrayBuffer())
  const ext = /\.([a-z0-9]+)$/i.exec(absPath)?.[1].toLowerCase() ?? ""
  if (ext === "docx") return docxView(unzipFiles(bytes))
  if (ext === "xlsx" || ext === "xlsm") return await xlsxView(bytes)
  if (ext === "pptx") return pptxView(unzipFiles(bytes))
  throw new Error(`不支持的预览类型 .${ext || "(无扩展名)"}（支持 docx/xlsx/xlsm/pptx）`)
}

// ---------------------------------------------------------------------------
// 公共：转义与 HTML 外壳
// ---------------------------------------------------------------------------

function esc(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;")
}

/** argb（FFRRGGBB）→ #RRGGBB；非法返回 null。 */
function cssColor(argb: unknown): string | null {
  if (typeof argb !== "string" || !/^[0-9a-fA-F]{6,8}$/.test(argb)) return null
  const hex = argb.slice(-6).toUpperCase()
  return hex === "FFFFFF" ? null : `#${hex}`
}

const VIEW_CSS = `<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{color-scheme:light}
html{background:#fff}
body{margin:0;background:#fff;color:#1f2328;font:14px/1.75 -apple-system,"Segoe UI","Microsoft YaHei","PingFang SC",sans-serif}
main{max-width:880px;margin:0 auto;padding:28px 32px 48px}
.meta{color:#6a737d;font-size:12px;margin-bottom:20px;padding-bottom:10px;border-bottom:1px solid #eaeef2}
h1{font-size:1.7em;margin:1em 0 .5em}h2{font-size:1.4em;margin:1.1em 0 .5em}h3{font-size:1.2em;margin:1em 0 .45em}
h4,h5,h6{font-size:1.05em;margin:.9em 0 .4em}
p{margin:.5em 0}
ul,ol{margin:.4em 0;padding-left:1.6em}
table{border-collapse:collapse;margin:.8em 0;width:100%;font-size:.95em}
th,td{border:1px solid #d5dbe1;padding:5px 9px;text-align:left;vertical-align:middle;word-break:break-word}
thead th{background:#f1f4f8}
blockquote{margin:.7em 0;padding:.2em 1em;border-left:3px solid #b6bfca;color:#57606a}
pre{background:#f6f8fa;border:1px solid #e6eaef;border-radius:6px;padding:10px 12px;overflow:auto;font:12.5px/1.6 Consolas,Menlo,monospace}
img{max-width:100%;height:auto;border-radius:4px}
figure{margin:.8em 0}figcaption{color:#6a737d;font-size:12px;text-align:center;margin-top:4px}
.slide{border:1px solid #dfe4ea;border-radius:10px;padding:16px 20px 12px;margin:14px 0;background:#fcfdfe}
.slide-no{display:inline-block;background:#eef2f8;color:#42506b;border-radius:10px;font-size:12px;padding:1px 10px;margin-bottom:6px}
.slide table{width:auto;max-width:100%}
.note{color:#6a737d;font-size:12.5px;border-top:1px dashed #e3e8ee;margin-top:8px;padding-top:6px}
.sheet{margin:22px 0}
.sheet-name{font-size:1.15em;font-weight:600;margin-bottom:6px}
.sheet-note,.cap{color:#6a737d;font-size:12px;margin:4px 0}
td.num{text-align:right}
</style></head><body><main>`

function shell(meta: string, body: string): string {
  return `${VIEW_CSS}${meta ? `<div class="meta">${esc(meta)}</div>` : ""}${body}</main></body></html>`
}

// ---------------------------------------------------------------------------
// docx：块模型 → HTML（图片按文档内出现顺序 data URI 内嵌，超额降级为标记）
// ---------------------------------------------------------------------------

/** 单图 ≤2MB、总量 ≤8MB：超出不内嵌（保留 [图片] 标记，文末不再重复）。 */
const IMG_SINGLE_MAX = 2 * 1024 * 1024
const IMG_TOTAL_MAX = 8 * 1024 * 1024
const IMG_MIME: Record<string, string> = { png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", bmp: "image/bmp" }

function docxView(files: Record<string, Uint8Array>): string {
  const read = readDocx(files)
  const mediaKeys = Object.keys(files)
    .filter((k) => k.startsWith("word/media/") && !k.endsWith("/"))
    .sort()
  let mediaIdx = 0
  let mediaBytes = 0
  let embedded = 0
  const nextImg = (): string | null => {
    while (mediaIdx < mediaKeys.length) {
      const key = mediaKeys[mediaIdx++]
      const ext = /\.([a-z0-9]+)$/i.exec(key)?.[1].toLowerCase() ?? ""
      const mime = IMG_MIME[ext]
      const data = files[key]
      if (!mime || !data || data.length > IMG_SINGLE_MAX || mediaBytes + data.length > IMG_TOTAL_MAX) continue
      mediaBytes += data.length
      embedded++
      return `<img src="data:${mime};base64,${Buffer.from(data).toString("base64")}" alt="${esc(key)}">`
    }
    return null
  }
  const inlineImages = (escaped: string): string =>
    escaped.split("[图片]").reduce((acc, part, i) => (i === 0 ? acc : acc + (nextImg() ?? "<span>🖼</span>") + part))

  const pageNote = read.page ? `${(read.page.widthTw / 567).toFixed(1)}×${(read.page.heightTw / 567).toFixed(1)}cm ${read.page.landscape ? "横向" : "纵向"}` : ""
  const meta = `${read.blocks.length} 块 · 图片 ${read.mediaCount}${pageNote ? ` · ${pageNote}` : ""} · 阅读视图（不还原精确版式，完整效果请下载打开）`

  const out: string[] = []
  let i = 0
  while (i < read.blocks.length) {
    const b = read.blocks[i]
    if (b.kind === "heading") {
      out.push(`<h${Math.min(6, b.level)}>${inlineImages(esc(b.text))}</h${Math.min(6, b.level)}>`)
      i++
    } else if (b.kind === "paragraph") {
      out.push(`<p>${inlineImages(esc(b.text)).replace(/\n/g, "<br>")}</p>`)
      i++
    } else if (b.kind === "table") {
      const [head, ...rows] = b.rows
      const tr = (cells: string[], tag: "th" | "td") => `<tr>${cells.map((c) => `<${tag}>${inlineImages(esc(c)).replace(/\n/g, "<br>")}</${tag}>`).join("")}</tr>`
      out.push(`<table>${head ? `<thead>${tr(head, "th")}</thead>` : ""}<tbody>${rows.map((r) => tr(r, "td")).join("")}</tbody></table>`)
      out.push(`<div class="cap">表格 ${b.rows.length} 行 × ${head?.length ?? 0} 列</div>`)
      i++
    } else if (b.kind === "listItem") {
      // 连续同组列表项 → 嵌套列表（层级 0-3）
      const ordered = b.ordered
      const items: Array<{ text: string; level: number }> = []
      while (i < read.blocks.length && read.blocks[i].kind === "listItem") {
        const it = read.blocks[i] as Extract<DocxBlock, { kind: "listItem" }>
        if (it.ordered !== ordered) break
        items.push({ text: it.text, level: it.level })
        i++
      }
      out.push(nestedList(items, ordered, inlineImages))
    } else {
      i++
    }
  }
  return shell(meta, out.join("\n"))
}

/** 连续列表项 → 嵌套 ul/ol（栈式：depth=已打开列表数-1，项层级收敛到 prev+1 防跳级丢项）。 */
function nestedList(items: Array<{ text: string; level: number }>, ordered: boolean, inlineImages: (s: string) => string): string {
  const out: string[] = []
  const openList = () => out.push(ordered ? "<ol>" : "<ul>")
  const closeList = () => out.push(ordered ? "</ol>" : "</ul>")
  let depth = -1
  let prev = 0
  for (const it of items) {
    let lvl = Math.max(0, Math.min(3, it.level))
    if (lvl > prev + 1) lvl = prev + 1
    while (depth > lvl) {
      closeList()
      depth--
    }
    while (depth < lvl) {
      openList()
      depth++
    }
    out.push(`<li>${inlineImages(esc(it.text))}</li>`)
    prev = lvl
  }
  while (depth >= 0) {
    closeList()
    depth--
  }
  return out.join("")
}

// ---------------------------------------------------------------------------
// xlsx：exceljs → 带样式表格（合并单元格 rowspan/colspan、字体/底色/对齐）
// ---------------------------------------------------------------------------

const SHEET_ROW_CAP = 500
const SHEET_COL_CAP = 64

async function xlsxView(bytes: Uint8Array): Promise<string> {
  const wb = new Workbook()
  // exceljs 对 load 入参的自有 Buffer 声明滞后（同 excel.ts 口径，运行时兼容——统一断言）
  await wb.xlsx.load(Buffer.from(bytes) as never)
  const caps: string[] = []
  const sheets = wb.worksheets.map((ws) => {
    const rowCount = Math.min(ws.rowCount || 1, SHEET_ROW_CAP)
    const colCount = Math.min(ws.columnCount || ws.actualColumnCount || 1, SHEET_COL_CAP)
    if ((ws.rowCount || 0) > SHEET_ROW_CAP || (ws.columnCount || 0) > SHEET_COL_CAP) {
      caps.push(`${ws.name}（截取前 ${rowCount} 行 × ${colCount} 列，共 ${ws.rowCount} 行 × ${ws.columnCount} 列）`)
    }
    // 合并区域：左上格跨行跨列，被覆盖格跳过
    const span = new Map<string, { rs: number; cs: number }>()
    const covered = new Set<string>()
    for (const range of ws.model.merges ?? []) {
      const m = /^([A-Za-z]+)(\d+):([A-Za-z]+)(\d+)$/.exec(range)
      if (!m) continue
      const c1 = colNum(m[1])
      const c2 = colNum(m[3])
      const r1 = Number(m[2])
      const r2 = Number(m[4])
      if (!c1 || !c2) continue
      span.set(`${r1}:${c1}`, { rs: Math.max(1, r2 - r1 + 1), cs: Math.max(1, c2 - c1 + 1) })
      for (let r = r1; r <= Math.min(r2, rowCount); r++) for (let c = c1; c <= Math.min(c2, colCount); c++) if (r !== r1 || c !== c1) covered.add(`${r}:${c}`)
    }
    let rowsHtml = ""
    for (let r = 1; r <= rowCount; r++) {
      let tds = ""
      for (let c = 1; c <= colCount; c++) {
        if (covered.has(`${r}:${c}`)) continue
        const cell = ws.getRow(r).getCell(c)
        const text = cellText(cell as Cell, false)
        const sp = span.get(`${r}:${c}`)
        const style = cellStyle(cell as Cell)
        tds += `<td${sp ? ` rowspan="${sp.rs}" colspan="${sp.cs}"` : ""}${style}>${esc(text).replace(/\n/g, "<br>") || "&nbsp;"}</td>`
      }
      if (!tds) continue // 整行被上方合并单元格覆盖：该行由跨行单元格代偿，不再输出空行
      rowsHtml += `<tr>${tds}</tr>`
    }
    const dims = `${ws.rowCount} 行 × ${ws.columnCount} 列`
    return `<section class="sheet"><div class="sheet-name">${esc(ws.name)}</div><div class="sheet-note">${dims}</div><table>${rowsHtml}</table></section>`
  })
  const meta = `工作簿 · ${wb.worksheets.length} 个工作表 · 阅读视图（数值为存档值/公式回显，交互请下载打开）${caps.length ? ` · ${caps.join("、")}` : ""}`
  return shell(meta, sheets.join("\n"))
}

function colNum(s: string): number {
  let n = 0
  for (const c of s.toUpperCase()) n = n * 26 + (c.charCodeAt(0) - 64)
  return n || 0
}

function cellStyle(cell: Cell): string {
  const props: string[] = []
  const f = cell.font
  if (f?.bold) props.push("font-weight:700")
  if (f?.italic) props.push("font-style:italic")
  const color = cssColor(f?.color?.argb)
  if (color) props.push(`color:${color}`)
  if (f?.size) props.push(`font-size:${Math.min(f.size, 18)}px`)
  const fill = cell.fill
  if (fill && typeof fill === "object" && (fill as { type?: string }).type === "pattern") {
    const bg = cssColor((fill as { fgColor?: { argb?: unknown } }).fgColor?.argb)
    if (bg) props.push(`background:${bg}`)
  }
  const al = cell.alignment
  if (al?.horizontal) props.push(`text-align:${al.horizontal}`)
  if (typeof cell.value === "number") props.push("text-align:right")
  return props.length ? ` style="${props.join(";")}"` : ""
}

// ---------------------------------------------------------------------------
// pptx：幻灯片大纲（文本/表格/图表标记/备注；不伪造页面布局）
// ---------------------------------------------------------------------------

function pptxView(files: Record<string, Uint8Array>): string {
  const read = readPptx(files)
  const chartTotal = read.slides.filter((s) => s.hasChart).length
  const imageTotal = read.slides.reduce((n, s) => n + s.imageCount, 0)
  const meta = `${read.widthIn.toFixed(2)}×${read.heightIn.toFixed(2)} 英寸 · ${read.slides.length} 页 · 图表 ${chartTotal} · 图片 ${imageTotal} · 大纲视图（不还原页面布局，完整效果请下载打开）`
  const slides = read.slides.map((sl, i) => {
    const parts: string[] = [`<section class="slide"><span class="slide-no">第 ${i + 1} 页</span>`]
    sl.texts.forEach((t, ti) => {
      const html = esc(t).replace(/\n/g, "<br>")
      parts.push(ti === 0 ? `<h3>${html}</h3>` : `<p>${html}</p>`)
    })
    for (const tbl of sl.tables) {
      parts.push(`<table><tbody>${tbl.map((r) => `<tr>${r.map((c) => `<td>${esc(c)}</td>`).join("")}</tr>`).join("")}</tbody></table>`)
      parts.push(`<div class="cap">表格 ${tbl.length} 行 × ${tbl[0]?.length ?? 0} 列</div>`)
    }
    if (sl.hasChart) parts.push(`<p>📊 图表</p>`)
    if (!sl.texts.length && sl.imageCount) parts.push(`<p>（图片页，${sl.imageCount} 张图）</p>`)
    if (sl.notes) parts.push(`<div class="note">备注：${esc(sl.notes).replace(/\n/g, "<br>")}</div>`)
    parts.push(`</section>`)
    return parts.join("")
  })
  return shell(meta, slides.join("\n"))
}
