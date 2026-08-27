/**
 * Word（.docx）工具：word_create（markdown/blocks → 排版文档，docx 库生成）、
 * word_read（解析回 markdown 供模型阅读）、word_append（原 XML 拼接——保留原文档全部格式，
 * 在正文末尾追加分节）。读取/追加共享 markdown.ts 的块模型与 ooxml.ts 的解析基础。
 */
import {
  AlignmentType,
  BorderStyle,
  Document,
  ExternalHyperlink,
  Footer,
  Header,
  HeadingLevel,
  ImageRun,
  LevelFormat,
  PageBreak,
  PageNumber,
  Packer,
  Paragraph,
  ShadingType,
  Table,
  TableCell,
  TableOfContents,
  TableRow,
  TextRun,
  VerticalAlign,
  WidthType,
  convertMillimetersToTwip,
} from "docx"
import { strToU8 } from "fflate"
import type { Tool, ToolContext } from "../../core/types"
import { truncate } from "../../core/tools"
import { escapeXml, readDocx, unzipFiles, xmlStr, zipFiles } from "./ooxml"
import type { MdBlock, MdRun } from "./markdown"
import { parseMarkdown } from "./markdown"
import { asNum, blindOverwriteGuard, fileBlocks, fitImage, normColor, readImage, schema, writeGuards } from "./shared"

// ---------------------------------------------------------------------------
// 输入归一化：markdown 文本或 blocks JSON → MdBlock[]
// ---------------------------------------------------------------------------

type Norm = { blocks: MdBlock[]; warnings: string[] }

function toRuns(v: unknown): MdRun[] {
  if (typeof v === "string") return v ? [{ text: v }] : []
  if (Array.isArray(v)) {
    return v.flatMap((r) => {
      if (typeof r === "string") return r ? [{ text: r }] : []
      if (r && typeof r === "object") {
        const o = r as Record<string, unknown>
        if (!o.text && !o.br) return []
        const run: MdRun = { text: String(o.text ?? "") }
        for (const k of ["bold", "italic", "strike", "code"] as const) if (o[k] === true) run[k] = true
        if (typeof o.href === "string") run.href = o.href
        if (o.size != null) run.size = asNum(o.size, 10.5)
        const c = normColor(o.color)
        if (c) run.color = c
        if (o.br === true) run.br = true
        return [run]
      }
      return []
    })
  }
  return []
}

const ALIGN: Record<string, (typeof AlignmentType)[keyof typeof AlignmentType]> = {
  left: AlignmentType.LEFT,
  center: AlignmentType.CENTER,
  right: AlignmentType.RIGHT,
  justify: AlignmentType.JUSTIFIED,
}

/** blocks JSON → MdBlock[]：type/kind 均接受，字段宽松归一；未知类型记 warning 跳过。 */
function normalizeBlocks(input: unknown): Norm {
  const warnings: string[] = []
  if (!Array.isArray(input)) return { blocks: [], warnings: ["blocks 参数须为数组"] }
  const blocks: MdBlock[] = []
  for (const raw of input) {
    if (!raw || typeof raw !== "object") continue
    const b = raw as Record<string, unknown>
    const kind = String(b.kind ?? b.type ?? "")
    switch (kind) {
      case "heading": {
        const level = Math.max(1, Math.min(9, Math.round(asNum(b.level, 2))))
        blocks.push({ kind: "heading", level, runs: toRuns(b.runs ?? b.text) })
        break
      }
      case "paragraph":
        blocks.push({ kind: "paragraph", runs: toRuns(b.runs ?? b.text) })
        break
      case "list": {
        const items = Array.isArray(b.items)
          ? b.items.map((it) => (typeof it === "string" ? { runs: toRuns(it), level: 0 } : { runs: toRuns((it as Record<string, unknown>)?.runs ?? (it as Record<string, unknown>)?.text), level: Math.max(0, Math.min(3, Math.round(asNum((it as Record<string, unknown>)?.level, 0)))) }))
          : []
        blocks.push({ kind: "list", ordered: b.ordered === true, items })
        break
      }
      case "table": {
        const header = Array.isArray(b.header) ? b.header.map((c) => String(typeof c === "object" && c ? (c as Record<string, unknown>).text ?? "" : c)) : []
        const rows = Array.isArray(b.rows) ? b.rows.map((r) => (Array.isArray(r) ? r.map((c) => toRuns(c)) : [])) : []
        const aligns = Array.isArray(b.aligns)
          ? b.aligns.filter((a): a is "left" | "center" | "right" => a === "left" || a === "center" || a === "right")
          : undefined
        const widths = Array.isArray(b.widths) ? b.widths.map((w) => asNum(w, 0)).filter((w) => w > 0) : undefined
        blocks.push({ kind: "table", header, rows, aligns, widths })
        break
      }
      case "image":
        if (typeof b.path === "string") {
          const img: MdBlock = { kind: "image", path: b.path }
          if (typeof b.alt === "string") img.alt = b.alt
          if (b.width != null) img.width = asNum(b.width, 0)
          if (b.height != null) img.height = asNum(b.height, 0)
          blocks.push(img)
        } else warnings.push("image 块缺少 path，已跳过")
        break
      case "code":
        blocks.push({ kind: "code", text: String(b.text ?? ""), lang: typeof b.lang === "string" ? b.lang : undefined })
        break
      case "quote":
        blocks.push({ kind: "quote", runs: toRuns(b.runs ?? b.text) })
        break
      case "pagebreak":
        blocks.push({ kind: "pagebreak" })
        break
      case "toc":
        blocks.push({ kind: "toc" })
        break
      default:
        warnings.push(`未知块类型 ${kind || "(空)"}，已跳过（支持 heading/paragraph/list/table/image/code/quote/pagebreak/toc）`)
    }
  }
  return { blocks, warnings }
}

function bodyInput(args: Record<string, unknown>): Norm {
  if (typeof args.markdown === "string" && args.markdown.trim()) return parseMarkdown(args.markdown)
  if (args.blocks != null) return normalizeBlocks(args.blocks)
  return { blocks: [], warnings: [] }
}

// ---------------------------------------------------------------------------
// word_create：docx 库生成
// ---------------------------------------------------------------------------

interface WordStyle {
  title?: string
  pageSize: "a4" | "letter"
  orientation: "portrait" | "landscape"
  margins?: { top?: number; right?: number; bottom?: number; left?: number }
  baseFont: string
  baseSize: number
  header?: string
  footer?: string
}

function normalizeStyle(v: unknown): WordStyle {
  const o = v && typeof v === "object" ? (v as Record<string, unknown>) : {}
  const s: WordStyle = {
    pageSize: o.pageSize === "letter" ? "letter" : "a4",
    orientation: o.orientation === "landscape" ? "landscape" : "portrait",
    baseFont: typeof o.baseFont === "string" && o.baseFont.trim() ? o.baseFont.trim() : "Microsoft YaHei",
    baseSize: asNum(o.baseSize, 10.5),
  }
  if (typeof o.title === "string" && o.title.trim()) s.title = o.title.trim()
  if (typeof o.header === "string" && o.header.trim()) s.header = o.header.trim()
  if (typeof o.footer === "string" && o.footer.trim()) s.footer = o.footer.trim()
  if (o.margins && typeof o.margins === "object") {
    const m = o.margins as Record<string, unknown>
    const mm: Record<string, number> = {}
    for (const k of ["top", "right", "bottom", "left"] as const) {
      const n = asNum(m[k], NaN)
      if (Number.isFinite(n) && n >= 0 && n <= 10) mm[k] = n
    }
    if (Object.keys(mm).length) s.margins = mm
  }
  return s
}

const PAGE_TWIPS = { a4: { w: 11906, h: 16838 }, letter: { w: 12240, h: 15840 } }

function runsToDocx(runs: MdRun[]): Array<TextRun | ExternalHyperlink> {
  const out: Array<TextRun | ExternalHyperlink> = []
  for (const r of runs) {
    if (!r.text && !r.br) continue
    const opts: Record<string, unknown> = {}
    if (r.text) opts.text = r.text
    if (r.br) opts.break = 1
    if (r.bold) opts.bold = true
    if (r.italic) opts.italics = true
    if (r.strike) opts.strike = true
    if (r.code) {
      opts.font = "Consolas"
      opts.shading = { type: ShadingType.CLEAR, fill: "F0F0F0" }
    }
    if (r.size) opts.size = Math.round(r.size * 2)
    if (r.color) opts.color = r.color
    if (r.href) {
      out.push(new ExternalHyperlink({ link: r.href, children: [new TextRun({ ...opts, style: "Hyperlink" })] }))
    } else {
      out.push(new TextRun(opts))
    }
  }
  return out
}

async function blocksToDocxChildren(
  blocks: MdBlock[],
  ctx: ToolContext,
  style: WordStyle,
  numberingConfig: Array<{ reference: string; levels: unknown[] }>,
): Promise<{ children: Array<Paragraph | Table | TableOfContents>; stats: Record<string, number>; warnings: string[] }> {
  const base = { font: style.baseFont, size: style.baseSize }
  const children: Array<Paragraph | Table | TableOfContents> = []
  const stats: Record<string, number> = { heading: 0, paragraph: 0, listItem: 0, table: 0, image: 0, code: 0, quote: 0, toc: 0, pagebreak: 0 }
  const warnings: string[] = []
  let listIdx = 0

  for (const b of blocks) {
    switch (b.kind) {
      case "heading": {
        stats.heading++
        const lvl = Math.min(6, Math.max(1, b.level))
        children.push(new Paragraph({ heading: lvl === 1 ? HeadingLevel.HEADING_1 : lvl === 2 ? HeadingLevel.HEADING_2 : lvl === 3 ? HeadingLevel.HEADING_3 : lvl === 4 ? HeadingLevel.HEADING_4 : lvl === 5 ? HeadingLevel.HEADING_5 : HeadingLevel.HEADING_6, children: runsToDocx(b.runs) }))
        break
      }
      case "paragraph":
        stats.paragraph++
        children.push(new Paragraph({ spacing: { after: 120, line: 276 }, children: runsToDocx(b.runs) }))
        break
      case "list": {
        const reference = `wps-list-${listIdx++}`
        numberingConfig.push({
          reference,
          levels: [0, 1, 2, 3].map((l) => ({
            level: l,
            format: b.ordered ? LevelFormat.DECIMAL : LevelFormat.BULLET,
            text: b.ordered ? `%${l + 1}.` : "•",
            alignment: AlignmentType.START,
            style: { paragraph: { indent: { left: 720 + l * 360, hanging: 360 } } },
          })),
        })
        for (const item of b.items) {
          stats.listItem++
          children.push(new Paragraph({ numbering: { reference, level: Math.min(3, item.level) }, spacing: { after: 60 }, children: runsToDocx(item.runs) }))
        }
        break
      }
      case "table": {
        stats.table++
        const nCols = Math.max(1, b.header.length)
        const aligns = (b.aligns ?? []).map((a) => (a && ALIGN[a] ? ALIGN[a] : undefined))
        const widths = b.widths && b.widths.length === nCols ? b.widths : undefined
        const rows = [b.header.map((h) => [{ text: h }]), ...b.rows.map((r) => r)]
        children.push(
          new Table({
            width: { size: 100, type: WidthType.PERCENTAGE },
            columnWidths: widths?.map((w) => Math.round((9360 * w) / 100)),
            rows: rows.map((cells, ri) =>
              new TableRow({
                tableHeader: ri === 0,
                children: Array.from({ length: nCols }, (_, ci) => {
                  const runs = cells[ci] ?? []
                  const headerRun: MdRun = { text: String(runs.map((r) => r.text).join("") || ""), bold: true }
                  return new TableCell({
                    width: widths ? { size: widths[ci], type: WidthType.PERCENTAGE } : undefined,
                    shading: ri === 0 ? { type: ShadingType.CLEAR, fill: "EEF2F8" } : undefined,
                    margins: { top: 60, bottom: 60, left: 100, right: 100 },
                    verticalAlign: VerticalAlign.CENTER,
                    children: [new Paragraph({ alignment: aligns[ci], spacing: { after: 0 }, children: runsToDocx(ri === 0 ? [headerRun] : runs) })],
                  })
                }),
              }),
            ),
          }),
        )
        // 相邻表格会被 Word 视觉合并，插入空段隔离
        children.push(new Paragraph({ spacing: { after: 60 }, children: [] }))
        break
      }
      case "image": {
        try {
          const { bytes, type } = await readImage(b.path, ctx)
          const size = fitImage(bytes, type, b.width, b.height)
          stats.image++
          children.push(new Paragraph({ alignment: AlignmentType.CENTER, spacing: { before: 120, after: 120 }, children: [new ImageRun({ type, data: bytes, transformation: size })] }))
        } catch (err) {
          warnings.push(`图片 ${b.path} 嵌入失败：${(err as Error).message}`)
        }
        break
      }
      case "code": {
        stats.code++
        const lines = b.text.split("\n")
        children.push(
          new Paragraph({
            shading: { type: ShadingType.CLEAR, fill: "F5F5F5" },
            spacing: { before: 120, after: 0 },
            children: lines.flatMap((ln, idx) => {
              const run = new TextRun({ text: ln || " ", font: "Consolas", size: Math.min(base.size, 10) * 2 })
              return idx === 0 ? [run] : [new TextRun({ break: 1 }), run]
            }),
          }),
        )
        children.push(new Paragraph({ spacing: { after: 120 }, children: [] }))
        break
      }
      case "quote":
        stats.quote++
        children.push(
          new Paragraph({
            indent: { left: 720 },
            border: { left: { style: BorderStyle.SINGLE, size: 12, color: "999999", space: 8 } },
            spacing: { before: 120, after: 120 },
            children: runsToDocx(b.runs.map((r) => ({ ...r, italic: true, color: r.color ?? "595959" }))),
          }),
        )
        break
      case "toc":
        stats.toc++
        children.push(new TableOfContents("目录", { hyperlink: true, headingStyleRange: "1-3" }))
        break
      case "pagebreak":
        stats.pagebreak++
        children.push(new Paragraph({ children: [new PageBreak()] }))
        break
    }
  }
  return { children, stats, warnings }
}

/** 页眉/页脚文本 → 段落（{page}/{pages} 占位替换为页码域）。 */
function headerFooterPara(text: string): Paragraph {
  const parts = text.split(/(\{page\}|\{pages\})/).filter((p) => p !== "")
  const children = parts.map((p) =>
    p === "{page}"
      ? new TextRun({ children: [PageNumber.CURRENT] })
      : p === "{pages}"
        ? new TextRun({ children: [PageNumber.TOTAL_PAGES] })
        : new TextRun(p),
  )
  return new Paragraph({ alignment: AlignmentType.CENTER, children })
}

export const wordCreateTool: Tool = {
  name: "word_create",
  description:
    "创建 .docx Word 文档（富排版：标题层级/表格/列表/图片/页眉页脚/目录/页面设置）。正文二选一：markdown 文本（推荐——支持 # 标题、段落、**粗** *斜* ~~删~~ `码` [链](url)、- / 1. 列表（缩进分级）、| 表格 |（首行表头）、> 引用、``` 代码块、![图](路径) 嵌入会话/项目内图片、<!--pagebreak--> 分页、<!--toc--> 目录）或 blocks JSON（结构化块数组，支持逐 run 样式与图片尺寸）。style 可调页面（A4 默认/横竖向/页边距 cm）与正文（字体/字号，默认微软雅黑 10.5pt）、页眉页脚（footer 支持 {page}/{pages} 页码占位）。目标已存在且本会话未读取过时拒绝（防盲覆盖，先 word_read）。旧版 .doc 不支持。",
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "输出路径（.docx；相对路径以会话工作目录为基准）" },
      markdown: { type: "string", description: "markdown 正文（与 blocks 二选一）" },
      blocks: { type: "array", description: "结构化块数组（与 markdown 二选一）：[{type:heading|paragraph|list|table|image|code|quote|pagebreak|toc, ...}]" },
      style: {
        type: "object",
        description: "文档级排版：{title, pageSize:a4|letter, orientation:portrait|landscape, margins:{top,right,bottom,left}(cm), baseFont, baseSize(pt), header, footer({page}/{pages} 页码)}",
      },
    },
    ["path"],
  ),
  safeMode: false,
  async execute(args, ctx) {
    const abs = ctx.resolvePath(String(args.path))
    const guardMsg = await writeGuards(abs, ctx)
    if (guardMsg) return { output: guardMsg }
    const blindMsg = await blindOverwriteGuard(abs, String(args.path), ctx)
    if (blindMsg) return { output: blindMsg }

    const style = normalizeStyle(args.style)
    const { blocks, warnings } = bodyInput(args)
    if (!blocks.length) return { output: "正文为空：请传 markdown 或 blocks 参数（至少一个内容块）。" }

    const numberingConfig: Array<{ reference: string; levels: unknown[] }> = []
    const { children, stats, warnings: buildWarnings } = await blocksToDocxChildren(blocks, ctx, style, numberingConfig)
    if (!children.length) return { output: "正文为空：解析后没有任何内容块。" }
    const allWarnings = [...warnings, ...buildWarnings]

    const size = PAGE_TWIPS[style.pageSize]
    const landscape = style.orientation === "landscape"
    const m = style.margins
    const cm = (v: number | undefined, def: number) => convertMillimetersToTwip((v ?? def) * 10)
    const doc = new Document({
      title: style.title,
      creator: "GEBAI",
      numbering: numberingConfig.length ? { config: numberingConfig as never } : undefined,
      styles: {
        default: {
          document: { run: { size: Math.round(style.baseSize * 2), font: { ascii: style.baseFont, eastAsia: style.baseFont, hAnsi: style.baseFont, cs: style.baseFont } } },
        },
      },
      sections: [
        {
          properties: {
            page: {
              size: { width: landscape ? size.h : size.w, height: landscape ? size.w : size.h, orientation: landscape ? "landscape" : undefined },
              margin: { top: cm(m?.top, 2.54), right: cm(m?.right, 3.18), bottom: cm(m?.bottom, 2.54), left: cm(m?.left, 3.18) },
            },
          },
          headers: style.header ? { default: new Header({ children: [headerFooterPara(style.header)] }) } : undefined,
          footers: style.footer ? { default: new Footer({ children: [headerFooterPara(style.footer)] }) } : undefined,
          children,
        },
      ],
    })
    const buf = await Packer.toBuffer(doc)
    await ctx.writeBinaryFile!(abs, new Uint8Array(buf))
    ctx.fileGuard?.markRead(abs)

    const statParts = Object.entries(stats)
      .filter(([, n]) => n > 0)
      .map(([k, n]) => `${labelOf(k)}×${n}`)
    const pageNote = `${style.pageSize.toUpperCase()} ${landscape ? "横向" : "纵向"}`
    const out =
      `已创建 ${args.path}（${statParts.join("、")}；${pageNote}，${style.baseFont} ${style.baseSize}pt）——前端文件面板可下载。` +
      (allWarnings.length ? `\n注意：\n- ${allWarnings.join("\n- ")}` : "")
    return { output: out, blocks: fileBlocks(abs, ctx) }
  },
}

function labelOf(k: string): string {
  const map: Record<string, string> = { heading: "标题", paragraph: "段落", listItem: "列表项", table: "表格", image: "图片", code: "代码块", quote: "引用", toc: "目录", pagebreak: "分页" }
  return map[k] ?? k
}

// ---------------------------------------------------------------------------
// word_read：解析回 markdown
// ---------------------------------------------------------------------------

export const wordReadTool: Tool = {
  name: "word_read",
  description:
    "读取 .docx 文档为 markdown（标题→#、列表→-/数字、表格→markdown 表格、内嵌图片→[图片] 占位；首行给出块数/图片数/页面摘要）。offset/limit 按块分页读长文档。旧版 .doc 不支持（请先另存为 .docx）。",
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "文档路径（.docx）" },
      offset: { type: "integer", description: "起始块序号（1 起始，默认 1）" },
      limit: { type: "integer", description: "读取块数（正数；默认全部——超长走截断保护）" },
    },
    ["path"],
  ),
  async execute(args, ctx) {
    const abs = ctx.resolvePath(String(args.path))
    let bytes: Uint8Array
    try {
      bytes = await ctx.readBinaryFile(abs)
    } catch {
      return { output: `word_read 失败：文件不存在或不可读（${args.path}）。请确认路径；目标在项目内时用 project 参数指定项目。` }
    }
    let read
    try {
      read = readDocx(unzipFiles(bytes))
    } catch (err) {
      return { output: `word_read 失败：${(err as Error).message}——可能是旧版 .doc 或损坏文件，请先在 Office/WPS 中另存为 .docx。` }
    }
    ctx.fileGuard?.markRead(abs)

    const total = read.blocks.length
    const offset = Math.max(1, Math.round(asNum(args.offset, 1)))
    const limit = args.limit == null ? undefined : Math.max(1, Math.round(asNum(args.limit, 1)))
    const slice = read.blocks.slice(offset - 1, limit ? offset - 1 + limit : undefined)

    const pageNote = read.page
      ? `${(read.page.widthTw / 567).toFixed(1)}×${(read.page.heightTw / 567).toFixed(1)}cm ${read.page.landscape ? "横向" : "纵向"}`
      : "页面设置未知"
    const head = `（${total} 块 · 图片 ${read.mediaCount} · ${pageNote}）`

    const lines: string[] = [head]
    let counter = 0
    let lastGroup = -1
    for (const b of slice) {
      if (b.kind === "heading") {
        lines.push(`${"#".repeat(Math.min(6, b.level))} ${b.text}`)
        counter = 0
      } else if (b.kind === "paragraph") {
        lines.push(b.text)
      } else if (b.kind === "listItem") {
        if (b.group !== lastGroup) counter = 0
        lastGroup = b.group
        counter++
        const marker = b.ordered ? `${counter}.` : "-"
        lines.push(`${"  ".repeat(Math.min(3, b.level))}${marker} ${b.text}`)
      } else if (b.kind === "table") {
        const esc = (s: string) => s.replace(/\|/g, "\\|").replace(/\n/g, " ")
        const [h, ...rest] = b.rows
        if (h) {
          lines.push(`| ${h.map(esc).join(" | ")} |`)
          lines.push(`| ${h.map(() => "---").join(" | ")} |`)
        }
        for (const r of rest) lines.push(`| ${r.map(esc).join(" | ")} |`)
        lines.push(`（表格 ${b.rows.length} 行 × ${h?.length ?? 0} 列）`)
      }
    }
    if (offset > 1 || (limit && offset - 1 + limit < total)) {
      lines.push(`（第 ${offset}–${Math.min(offset - 1 + slice.length, total)} 块，共 ${total} 块——调整 offset/limit 继续读）`)
    }
    const truncated = await truncate(lines.join("\n"), "word_read", ctx)
    return { ...truncated, blocks: fileBlocks(abs, ctx) }
  },
}

// ---------------------------------------------------------------------------
// word_append：原 XML 拼接（保留原文档格式，正文末尾追加）
// ---------------------------------------------------------------------------

const NS_R = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
const CONTENT_TYPES: Record<string, string> = { png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", bmp: "image/bmp" }
const HEADING_HALF_PT = [32, 28, 26, 24, 22, 21]

/** 追加片段生成：图片/超链接的关系登记 + 直接格式化 XML（不经样式表，新内容自带格式）。 */
function blocksToXml(
  blocks: MdBlock[],
  ctx: ToolContext,
  base: { font: string; size: number },
): Promise<{ xml: string; media: Array<{ name: string; bytes: Uint8Array }>; rels: Array<{ id: string; type: "image" | "hyperlink"; target: string; external?: boolean }>; stats: Record<string, number>; warnings: string[] }> {
  const media: Array<{ name: string; bytes: Uint8Array }> = []
  const rels: Array<{ id: string; type: "image" | "hyperlink"; target: string; external?: boolean }> = []
  const warnings: string[] = []
  const stats: Record<string, number> = {}
  const bump = (k: string) => (stats[k] = (stats[k] ?? 0) + 1)
  let nextRel = 1
  let nextMedia = 1
  let nextDocPr = 1000
  const relId = () => `rIdW${nextRel++}`
  const hrefIds = new Map<string, string>()

  const runXml = (r: MdRun): string => {
    let s = ""
    if (r.br) s += "<w:r><w:br/></w:r>"
    if (!r.text) return s
    const parts: string[] = []
    if (r.bold) parts.push("<w:b/>")
    if (r.italic) parts.push("<w:i/>")
    if (r.strike) parts.push("<w:strike/>")
    if (r.color) parts.push(`<w:color w:val="${r.color}"/>`)
    if (r.size) parts.push(`<w:sz w:val="${Math.round(r.size * 2)}"/><w:szCs w:val="${Math.round(r.size * 2)}"/>`)
    if (r.code) parts.push('<w:rFonts w:ascii="Consolas" w:eastAsia="Consolas" w:hAnsi="Consolas"/>')
    const pr = parts.length ? `<w:rPr>${parts.join("")}</w:rPr>` : ""
    if (r.href) {
      let id = hrefIds.get(r.href)
      if (!id) {
        id = relId()
        hrefIds.set(r.href, id)
        rels.push({ id, type: "hyperlink", target: r.href, external: true })
      }
      // 链接样式：蓝色下划线（结构化 run 自带 color 时优先）
      const linkPr = parts.filter((p) => !p.includes("<w:color")).join("") + (r.color ? `<w:color w:val="${r.color}"/>` : `<w:color w:val="0563C1"/><w:u w:val="single"/>`)
      return `${s}<w:hyperlink xmlns:r="${NS_R}" r:id="${id}"><w:r><w:rPr>${linkPr}</w:rPr><w:t xml:space="preserve">${escapeXml(r.text)}</w:t></w:r></w:hyperlink>`
    }
    return `${s}<w:r>${pr}<w:t xml:space="preserve">${escapeXml(r.text)}</w:t></w:r>`
  }

  const paraXml = (runs: MdRun[], pPr = '<w:spacing w:after="120" w:line="276" w:lineRule="auto"/>') =>
    `<w:p><w:pPr>${pPr}</w:pPr>${runs.map(runXml).join("")}</w:p>`

  const parts: string[] = []
  for (const b of blocks) {
    switch (b.kind) {
      case "heading": {
        bump("heading")
        const lvl = Math.min(6, Math.max(1, b.level))
        const hp = HEADING_HALF_PT[lvl - 1]
        parts.push(
          paraXml(
            b.runs.map((r) => ({ ...r, bold: true, size: (r.size ?? hp / 2) })),
            `<w:keepNext/><w:outlineLvl w:val="${lvl - 1}"/><w:spacing w:before="240" w:after="120"/>`,
          ),
        )
        break
      }
      case "paragraph":
        bump("paragraph")
        parts.push(paraXml(b.runs))
        break
      case "list": {
        let n = 0
        for (const item of b.items) {
          bump("listItem")
          n++
          const level = Math.min(3, item.level)
          const marker: MdRun = { text: b.ordered ? `${n}. ` : "• " }
          parts.push(paraXml([marker, ...item.runs], `<w:spacing w:after="60"/><w:ind w:left="${720 + level * 360}" w:hanging="360"/>`))
        }
        break
      }
      case "table": {
        bump("table")
        const nCols = Math.max(1, b.header.length)
        const colW = Math.floor(9360 / nCols)
        const borders = ["top", "left", "bottom", "right", "insideH", "insideV"].map((s) => `<w:${s} w:val="single" w:sz="4" w:space="0" w:color="A6A6A6"/>`).join("")
        const alignAttr = (ci: number) => {
          const a = b.aligns?.[ci]
          return a === "center" ? '<w:jc w:val="center"/>' : a === "right" ? '<w:jc w:val="right"/>' : ""
        }
        const rowsXml = [b.header.map((h) => [{ text: h, bold: true } as MdRun]), ...b.rows]
          .map((cells, ri) =>
            `<w:tr>${Array.from({ length: nCols }, (_, ci) => {
              const runs = cells[ci] ?? []
              const shd = ri === 0 ? '<w:shd w:val="clear" w:fill="EEF2F8"/>' : ""
              return `<w:tc><w:tcPr>${shd}<w:vAlign w:val="center"/></w:tcPr>${paraXml(runs, `<w:spacing w:after="40"/>${alignAttr(ci)}`)}</w:tc>`
            }).join("")}</w:tr>`,
          )
          .join("")
        parts.push(
          `<w:tbl><w:tblPr><w:tblW w:w="5000" w:type="pct"/><w:tblBorders>${borders}</w:tblBorders></w:tblPr><w:tblGrid>${`<w:gridCol w:w="${colW}"/>`.repeat(nCols)}</w:tblGrid>${rowsXml}</w:tbl><w:p/>`,
        )
        break
      }
      case "image": {
        // 异步图片读取同步化：外层用 await 逐块处理
        bump("image")
        parts.push(`__WPS_IMG__${b.path}__WPS_IMG__`)
        break
      }
      case "code": {
        bump("code")
        for (const ln of b.text.split("\n")) {
          parts.push(
            `<w:p><w:pPr><w:shd w:val="clear" w:fill="F5F5F5"/><w:spacing w:after="0"/></w:pPr><w:r><w:rPr><w:rFonts w:ascii="Consolas" w:eastAsia="Consolas" w:hAnsi="Consolas"/><w:sz w:val="${Math.min(base.size, 10) * 2}"/></w:rPr><w:t xml:space="preserve">${escapeXml(ln || " ")}</w:t></w:r></w:p>`,
          )
        }
        parts.push('<w:p><w:pPr><w:spacing w:after="120"/></w:pPr></w:p>')
        break
      }
      case "quote":
        bump("quote")
        parts.push(
          paraXml(
            b.runs.map((r) => ({ ...r, italic: true, color: r.color ?? "595959" })),
            '<w:ind w:left="720"/><w:pBdr><w:left w:val="single" w:sz="12" w:space="8" w:color="999999"/></w:pBdr><w:spacing w:before="120" w:after="120"/>',
          ),
        )
        break
      case "toc":
        bump("toc")
        parts.push(
          `<w:p><w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText xml:space="preserve"> TOC \\o "1-3" \\h </w:instrText></w:r><w:r><w:fldChar w:fldCharType="separate"/></w:r><w:r><w:t>（在 Word/WPS 中按 F9 或右键“更新域”生成目录）</w:t></w:r><w:r><w:fldChar w:fldCharType="end"/></w:r></w:p>`,
        )
        break
      case "pagebreak":
        bump("pagebreak")
        parts.push('<w:p><w:r><w:br w:type="page"/></w:r></w:p>')
        break
    }
  }

  // 第二遍：图片占位符替换为内嵌 drawing XML（需异步读文件）
  return (async () => {
    const out: string[] = []
    for (const p of parts) {
      const m = /^__WPS_IMG__(.*)__WPS_IMG__$/.exec(p)
      if (!m) {
        out.push(p)
        continue
      }
      const path = m[1]
      try {
        const { bytes, type } = await readImage(path, ctx)
        const size = fitImage(bytes, type, undefined, undefined)
        const ext = type === "jpg" ? "jpeg" : type
        const name = `wps-img-${nextMedia++}.${ext}`
        const id = relId()
        rels.push({ id, type: "image", target: `media/${name}` })
        media.push({ name: `word/media/${name}`, bytes })
        const emuW = Math.round(size.width * 9525)
        const emuH = Math.round(size.height * 9525)
        const docPr = nextDocPr++
        out.push(
          `<w:p><w:pPr><w:jc w:val="center"/><w:spacing w:before="120" w:after="120"/></w:pPr><w:r><w:drawing>` +
            `<wp:inline xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" distT="0" distB="0" distL="0" distR="0">` +
            `<wp:extent cx="${emuW}" cy="${emuH}"/><wp:effectExtent l="0" t="0" r="0" b="0"/><wp:docPr id="${docPr}" name="wps-image-${docPr}"/>` +
            `<wp:cNvGraphicFramePr><a:graphicFrameLocks xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" noChangeAspect="1"/></wp:cNvGraphicFramePr>` +
            `<a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">` +
            `<pic:pic xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"><pic:nvPicPr><pic:cNvPr id="${docPr}" name="wps-image-${docPr}"/><pic:cNvPicPr/></pic:nvPicPr>` +
            `<pic:blipFill><a:blip xmlns:r="${NS_R}" r:embed="${id}"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>` +
            `<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="${emuW}" cy="${emuH}"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr>` +
            `</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>`,
        )
      } catch (err) {
        warnings.push(`图片 ${path} 嵌入失败：${(err as Error).message}`)
      }
    }
    return { xml: out.join(""), media, rels, stats, warnings }
  })()
}

export const wordAppendTool: Tool = {
  name: "word_append",
  description:
    "向已有 .docx 末尾追加内容（原 XML 拼接：原文档格式/样式/图片**原样保留**，新内容直接格式化插入正文末尾）。正文 markdown 或 blocks（语法同 word_create：标题/段落/列表/表格/图片/引用/代码块/链接/分页/目录）。改中间内容或重排版请 word_read 后用 word_create 重建；Excel/表格类局部修改无对应工具（表格数据建议用 excel_* 维护）。",
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "目标文档路径（.docx，须已存在）" },
      markdown: { type: "string", description: "追加的 markdown 内容（与 blocks 二选一）" },
      blocks: { type: "array", description: "结构化块数组（与 markdown 二选一），块语法同 word_create" },
    },
    ["path"],
  ),
  safeMode: false,
  async execute(args, ctx) {
    const abs = ctx.resolvePath(String(args.path))
    const guardMsg = await writeGuards(abs, ctx)
    if (guardMsg) return { output: guardMsg }
    let bytes: Uint8Array
    try {
      bytes = await ctx.readBinaryFile(abs)
    } catch {
      return { output: `word_append 失败：文件不存在（${args.path}）。新建文档用 word_create。` }
    }
    let files: Record<string, Uint8Array>
    try {
      files = unzipFiles(bytes)
      if (!files["word/document.xml"]) throw new Error("缺少 word/document.xml")
    } catch (err) {
      return { output: `word_append 失败：${(err as Error).message}——不是有效的 .docx（旧版 .doc 请先另存为 .docx）。` }
    }

    const { blocks, warnings } = bodyInput(args)
    if (!blocks.length) return { output: "追加内容为空：请传 markdown 或 blocks 参数。" }
    const frag = await blocksToXml(blocks, ctx, { font: "Microsoft YaHei", size: 10.5 })
    const allWarnings = [...warnings, ...frag.warnings]
    if (!frag.xml) {
      return { output: `没有可追加的内容${allWarnings.length ? `：\n- ${allWarnings.join("\n- ")}` : "。"}` }
    }

    // document.xml：插入到 body 级 sectPr 之前（无则插到 </w:body> 前）
    const xml = xmlStr(files["word/document.xml"])
    const closeIdx = xml.lastIndexOf("</w:body>")
    const sectIdx = xml.lastIndexOf("<w:sectPr", closeIdx)
    const at = sectIdx >= 0 ? sectIdx : closeIdx
    const newXml = xml.slice(0, at) + frag.xml + xml.slice(at)

    const out: Record<string, Uint8Array> = { ...files, "word/document.xml": strToU8(newXml) }

    // 关系与媒体：图片/超链接登记 document.xml.rels，媒体文件入包，content types 补扩展名声明
    if (frag.rels.length) {
      const relKey = "word/_rels/document.xml.rels"
      let relsXml = files[relKey] ? xmlStr(files[relKey]) : `<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"></Relationships>`
      const add = frag.rels
        .map((r) => `<Relationship Id="${r.id}" Type="${NS_R}/${r.type}" Target="${escapeXml(r.target)}"${r.external ? ' TargetMode="External"' : ""}/>`)
        .join("")
      relsXml = relsXml.replace(/<\/Relationships>\s*$/, `${add}</Relationships>`)
      out[relKey] = strToU8(relsXml)
    }
    for (const m of frag.media) {
      out[m.name] = m.bytes
      const ext = /\.([a-z0-9]+)$/i.exec(m.name)?.[1].toLowerCase() ?? ""
      const ct = CONTENT_TYPES[ext]
      if (ct) {
        const ctXml = xmlStr(out["[Content_Types].xml"] ?? strToU8(`<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"></Types>`))
        const has = new RegExp(`Extension="${ext}"`, "i").test(ctXml)
        if (!has) out["[Content_Types].xml"] = strToU8(ctXml.replace(/<\/Types>\s*$/, `<Default Extension="${ext}" ContentType="${ct}"/></Types>`))
      }
    }

    const newBytes = zipFiles(out)
    // 写前校验：产物必须仍是可解析的 docx（防拼接损坏用户文档）
    try {
      const check = readDocx(unzipFiles(newBytes))
      if (check.blocks.length === 0) throw new Error("校验后正文为空")
    } catch (err) {
      return { output: `word_append 失败：追加后文档校验未通过（${(err as Error).message}），已放弃写入，原文件未改动。` }
    }
    await ctx.writeBinaryFile!(abs, newBytes)
    ctx.fileGuard?.markRead(abs)

    const statParts = Object.entries(frag.stats).map(([k, n]) => `${labelOf(k)}×${n}`)
    return {
      output: `已追加 ${statParts.join("、")} 至 ${args.path} 末尾（原文档格式保留）——前端文件面板可下载。${allWarnings.length ? `\n注意：\n- ${allWarnings.join("\n- ")}` : ""}`,
      blocks: fileBlocks(abs, ctx),
    }
  },
}
