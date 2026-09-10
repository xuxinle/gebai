/**
 * 迷你 Markdown 解析器（wps 子Agent 专用）：word_create / word_append / pdf_create 的正文输入形态，
 * 产出内部块模型（与 blocks JSON 参数共用同一模型）。支持子集：标题/段落/行内样式（粗斜删/
 * 等宽/链接）/有序无序列表（缩进分级）/表格（对齐）/引用/围栏代码块/块级图片/分页标记。
 * 自行实现而非引入 markdown 库：文档生成只需受控子集，行为可测、产物可控。
 * blocks JSON → 块模型的归一化（normalizeBlocks/bodyInput）也在此——三格式生成工具共用输入层。
 */
import { asNum, normColor } from "./shared"

export interface MdRun {
  text: string
  bold?: boolean
  italic?: boolean
  strike?: boolean
  code?: boolean
  href?: string
  /** 字号（pt；markdown 解析不产生，供 blocks JSON 结构化输入携带）。 */
  size?: number
  /** 字色（6 位 RRGGBB；markdown 解析不产生，供 blocks JSON 结构化输入携带）。 */
  color?: string
  /** 段内换行（软折行）：本 run 前插入换行。 */
  br?: boolean
}

export type MdBlock =
  | { kind: "heading"; level: number; runs: MdRun[] }
  | { kind: "paragraph"; runs: MdRun[] }
  | { kind: "list"; ordered: boolean; items: Array<{ runs: MdRun[]; level: number }> }
  | { kind: "table"; header: string[]; rows: MdRun[][][]; aligns?: Array<"left" | "center" | "right">; widths?: number[] }
  | { kind: "code"; text: string; lang?: string }
  | { kind: "quote"; runs: MdRun[] }
  | { kind: "image"; path: string; alt?: string; width?: number; height?: number }
  | { kind: "pagebreak" }
  | { kind: "toc" }

export function parseMarkdown(src: string): { blocks: MdBlock[]; warnings: string[] } {
  const warnings: string[] = []
  const lines = src.replace(/\r\n?/g, "\n").split("\n")
  const blocks: MdBlock[] = []
  let i = 0

  const isListLine = (s: string) => /^(\s*)([-*+]|\d+[.)])\s+(.*)$/.exec(s)
  const isTableSep = (s: string) => /^\s*\|?\s*:?-{1,}:?\s*(\|\s*:?-{1,}:?\s*)*\|?\s*$/.test(s) && s.includes("-")

  while (i < lines.length) {
    const line = lines[i]
    if (!line.trim()) {
      i++
      continue
    }
    // 围栏代码块
    const fence = /^\s*```([\w-]*)\s*$/.exec(line)
    if (fence) {
      const body: string[] = []
      i++
      while (i < lines.length && !/^\s*```\s*$/.test(lines[i])) body.push(lines[i++])
      if (i >= lines.length) warnings.push("代码块未闭合（缺 ``` 结尾），已按到文末处理")
      i++
      if (body.length) blocks.push({ kind: "code", text: body.join("\n"), lang: fence[1] || undefined })
      continue
    }
    // 标题
    const hm = /^(#{1,6})\s+(.+?)\s*#*\s*$/.exec(line)
    if (hm) {
      blocks.push({ kind: "heading", level: hm[1].length, runs: parseInline(hm[2]) })
      i++
      continue
    }
    // 分页标记
    if (/^\s*<!--\s*pagebreak\s*-->\s*$/i.test(line)) {
      blocks.push({ kind: "pagebreak" })
      i++
      continue
    }
    // 目录标记
    if (/^\s*<!--\s*toc\s*-->\s*$/i.test(line)) {
      blocks.push({ kind: "toc" })
      i++
      continue
    }
    // 水平线（文档场景无对应排版，跳过并提示）
    if (/^ {0,3}([-*_])\s*(?:\1\s*){2,}$/.test(line)) {
      warnings.push("已忽略水平分割线（---）：文档排版无对应样式，分页请用 <!--pagebreak-->")
      i++
      continue
    }
    // 块级图片
    const im = /^\s*!\[([^\]]*)\]\(\s*([^)\s]+)[^)]*\)\s*$/.exec(line)
    if (im) {
      blocks.push({ kind: "image", path: im[2], alt: im[1] || undefined })
      i++
      continue
    }
    // 引用
    if (/^\s*>/.test(line)) {
      const body: string[] = []
      while (i < lines.length && /^\s*>/.test(lines[i])) body.push(lines[i++].replace(/^\s*>\s?/, ""))
      blocks.push({ kind: "quote", runs: parseInline(body.join("\n")) })
      continue
    }
    // 表格：当前行含 | 且下一行是分隔行
    if (line.includes("|") && i + 1 < lines.length && isTableSep(lines[i + 1])) {
      const cells = (l: string) =>
        l.trim().replace(/^\|/, "").replace(/\|$/, "").split("|").map((c) => c.trim())
      const header = cells(line)
      const aligns = cells(lines[i + 1]).map((c) =>
        c.startsWith(":") && c.endsWith(":") ? "center" : c.endsWith(":") ? "right" : "left",
      )
      i += 2
      const rows: MdRun[][][] = []
      while (i < lines.length && lines[i].includes("|") && lines[i].trim()) {
        const row = cells(lines[i])
        while (row.length < header.length) row.push("")
        rows.push(row.slice(0, header.length + 2).map((c) => parseInline(c)))
        i++
      }
      blocks.push({ kind: "table", header, rows, aligns })
      continue
    }
    // 列表
    const lm = isListLine(line)
    if (lm) {
      type Raw = { runs: MdRun[]; level: number; ordered: boolean }
      const items: Raw[] = []
      while (i < lines.length) {
        const m = isListLine(lines[i])
        if (!m) {
          if (lines[i].trim() && i + 1 < lines.length && isListLine(lines[i + 1])) {
            items[items.length - 1].runs.push(...parseInline(lines[i]), { text: "", br: true } as MdRun)
            i++
            continue // 列表项的续行（后随列表项）
          }
          break
        }
        if (!lines[i].trim()) {
          if (i + 1 < lines.length && isListLine(lines[i + 1])) {
            i++
            continue // 列表项间的空行（仍是同一列表）
          }
          break
        }
        const indent = m[1].replace(/\t/g, "  ").length
        items.push({ runs: parseInline(m[3]), level: Math.min(3, Math.floor(indent / 2)), ordered: /^\d/.test(m[2]) })
        i++
      }
      // 有序/无序变化处分块（两类编号体系不同）
      let cur: Raw[] = []
      const flush = () => {
        if (cur.length) {
          blocks.push({ kind: "list", ordered: cur[0].ordered, items: cur.map(({ runs, level }) => ({ runs, level })) })
          cur = []
        }
      }
      for (const it of items) {
        if (cur.length && it.ordered !== cur[0].ordered) flush()
        cur.push(it)
      }
      flush()
      continue
    }
    // 段落：累积到空行或下一块级开始
    const para: string[] = []
    while (
      i < lines.length &&
      lines[i].trim() &&
      !/^\s*(#{1,6}\s|>|```|<!--|!\[)/.test(lines[i]) &&
      !isListLine(lines[i]) &&
      !(lines[i].includes("|") && i + 1 < lines.length && isTableSep(lines[i + 1]))
    ) {
      para.push(lines[i++])
    }
    if (para.length) {
      const runs: MdRun[] = []
      para.forEach((l, idx) => {
        const inline = parseInline(l)
        if (idx > 0 && inline.length) inline[0] = { ...inline[0], br: true }
        runs.push(...inline)
      })
      blocks.push({ kind: "paragraph", runs })
    }
  }
  return { blocks, warnings }
}

/** 行内样式扫描：**粗** *斜* ~~删~~ `码` [字](链接)，反斜杠转义字面量；未闭合标记按字面量保留。 */
export function parseInline(src: string): MdRun[] {
  const runs: MdRun[] = []
  let buf = ""
  let bold = false
  let italic = false
  let strike = false

  const flush = () => {
    if (buf) {
      const r: MdRun = { text: buf }
      if (bold) r.bold = true
      if (italic) r.italic = true
      if (strike) r.strike = true
      runs.push(r)
      buf = ""
    }
  }

  let i = 0
  while (i < src.length) {
    const c = src[i]
    if (c === "\\" && i + 1 < src.length && /[*~`[\]()\\]/.test(src[i + 1])) {
      buf += src[i + 1]
      i += 2
      continue
    }
    if (c === "`") {
      const close = src.indexOf("`", i + 1)
      if (close > i) {
        flush()
        runs.push({ text: src.slice(i + 1, close), code: true })
        i = close + 1
        continue
      }
    }
    if (c === "*" && src[i + 1] === "*") {
      flush()
      bold = !bold
      i += 2
      continue
    }
    if (c === "*") {
      flush()
      italic = !italic
      i += 1
      continue
    }
    if (c === "~" && src[i + 1] === "~") {
      flush()
      strike = !strike
      i += 2
      continue
    }
    if (c === "[") {
      const m = /^\[([^\]]*)\]\(\s*([^)\s]+)\s*\)/.exec(src.slice(i))
      if (m) {
        flush()
        const r: MdRun = { text: m[1] || m[2], href: m[2] }
        if (bold) r.bold = true
        if (italic) r.italic = true
        runs.push(r)
        i += m[0].length
        continue
      }
    }
    buf += c
    i++
  }
  flush()
  return mergeRuns(runs)
}

/** 合并相邻同样式 run（扫描器产生的碎片收敛，产物更干净）。 */
function mergeRuns(runs: MdRun[]): MdRun[] {
  const out: MdRun[] = []
  for (const r of runs) {
    const prev = out[out.length - 1]
    if (
      prev &&
      !prev.br &&
      !!prev.bold === !!r.bold &&
      !!prev.italic === !!r.italic &&
      !!prev.strike === !!r.strike &&
      !!prev.code === !!r.code &&
      prev.href === r.href
    ) {
      prev.text += r.text
    } else out.push({ ...r })
  }
  return out.filter((r) => r.text !== "" || r.br)
}

// ---------------------------------------------------------------------------
// blocks JSON → MdBlock[]（word_create/word_append/pdf_create 共用的结构化输入归一化）
// ---------------------------------------------------------------------------

export function toRuns(v: unknown): MdRun[] {
  if (typeof v === "string") return v ? [{ text: v }] : []
  if (v && typeof v === "object" && !Array.isArray(v)) return toRuns([v])
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

/** blocks JSON → MdBlock[]：type/kind 均接受，字段宽松归一；未知类型记 warning 跳过。 */
export function normalizeBlocks(input: unknown): { blocks: MdBlock[]; warnings: string[] } {
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

/** 工具入参 → 块模型：markdown 文本优先，否则 blocks JSON。 */
export function bodyInput(args: Record<string, unknown>): { blocks: MdBlock[]; warnings: string[] } {
  if (typeof args.markdown === "string" && args.markdown.trim()) return parseMarkdown(args.markdown)
  if (args.blocks != null) return normalizeBlocks(args.blocks)
  return { blocks: [], warnings: [] }
}
