/**
 * 块树 → XML 排版语法（反向序列化）+ 选择性读取（scope）——fetch_doc 的纯函数核心。
 *
 * 与 docx_xml.ts 的 xmlToBlocks 互为逆操作（round-trip：fetch 出的 XML 可直接改后
 * 经 update_doc/import_xml 写回），零飞书 HTTP 依赖，可独立测试。
 *
 * 设计依据官方 lark-doc skill 的 +fetch：输出即排版语法、块带 block_id 可直接编辑、
 * 容器/表格节选用 <excerpt> 包裹并给出 top-block-id 回读锚点。
 */

/* ================= 行内元素 → XML ================= */

/** 文本转义：只转义文本内容，标签本身不转义（与官方 XML 规范一致）。 */
export function escapeXmlText(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
}

/** 属性值转义（引号内）。 */
function escapeAttr(s: string): string {
  return escapeXmlText(s).replace(/"/g, "&quot;")
}

/** 颜色枚举 → 色相名（1~7 基础 / 8~14 背景 medium-*）。 */
const COLOR_NAMES: Record<number, string> = { 1: "red", 2: "orange", 3: "yellow", 4: "green", 5: "blue", 6: "purple", 7: "gray" }

function colorName(n: unknown, medium = false): string | undefined {
  const v = Number(n)
  if (!Number.isFinite(v) || v < 1) return undefined
  if (v >= 8 && v <= 14) return `medium-${COLOR_NAMES[v - 7] ?? "gray"}`
  const base = COLOR_NAMES[v]
  return base ? (medium && v <= 7 ? `light-${base}` : base) : undefined
}

/** 对齐枚举 → 名称。 */
function alignName(n: unknown): string | undefined {
  const v = Number(n)
  return v === 2 ? "center" : v === 3 ? "right" : undefined
}

/** text_run 样式开闭标签（嵌套样式按 bold>italic>underline>strike>code 顺序包裹）。 */
interface RunStyle {
  bold?: boolean
  italic?: boolean
  underline?: boolean
  strikethrough?: boolean
  inline_code?: boolean
  link?: { url: string }
  text_color?: number
  background_color?: number
}

function wrapStyle(text: string, st: RunStyle): string {
  let s = escapeXmlText(text)
  if (st.text_color !== undefined || st.background_color !== undefined) {
    const attrs: string[] = []
    const tc = colorName(st.text_color)
    if (tc) attrs.push(`text-color="${tc}"`)
    const bg = colorName(st.background_color, true)
    if (bg) attrs.push(`background-color="${bg}"`)
    s = `<span ${attrs.join(" ")}>${s}</span>`
  }
  if (st.inline_code) {
    // 行内代码内容不再包其他样式（语义上代码即字面）
    return `<code>${s}</code>`
  }
  if (st.strikethrough) s = `<del>${s}</del>`
  if (st.underline) s = `<u>${s}</u>`
  if (st.italic) s = `<em>${s}</em>`
  if (st.bold) s = `<b>${s}</b>`
  if (st.link?.url) s = `<a href="${escapeAttr(st.link.url)}">${s}</a>`
  return s
}

/** 行内元素数组 → XML 串（text_run/mention_user/mention_doc/mention_all/equation）。 */
export function inlineToXml(elements: unknown[] | undefined): string {
  if (!Array.isArray(elements)) return ""
  const parts: string[] = []
  for (const el of elements) {
    const e = el as Record<string, unknown>
    const run = e.text_run as { content?: string; text_element_style?: RunStyle } | undefined
    if (run) {
      const content = String(run.content ?? "")
      const st = run.text_element_style
      if (content.includes("\n")) {
        // 换行拆段，各自套样式后用 <br/> 连接（样式保留在每个片段上）
        const segs = content.split("\n")
        parts.push(segs.map((seg) => (seg ? wrapStyle(seg, st ?? {}) : "")).join("<br/>"))
      } else if (content) {
        parts.push(wrapStyle(content, st ?? {}))
      }
      continue
    }
    const mu = e.mention_user as { user_id?: string } | undefined
    if (mu?.user_id) parts.push(`<cite type="user" user-id="${escapeAttr(mu.user_id)}"/>`)
    const ma = e.mention_all as Record<string, unknown> | undefined
    if (ma) parts.push(`<cite type="all"/>`)
    const md = e.mention_doc as { token?: string } | undefined
    if (md?.token) parts.push(`<cite type="doc" doc-id="${escapeAttr(md.token)}"/>`)
    const eq = e.equation as { content?: string } | undefined
    if (eq?.content) parts.push(`<latex>${escapeXmlText(eq.content)}</latex>`)
  }
  return parts.join("")
}

/* ================= 块树 → XML ================= */

export interface FetchXmlOptions {
  /** 块 id → 块对象映射（含 children 引用），用于子树展开。 */
  byId: Map<string, Record<string, unknown>>
  /** 是否输出 block_id（with-ids / full 模式）。 */
  ids?: boolean
  /** 是否输出行内样式与块属性（full 模式；simple 只保留结构文本）。 */
  styles?: boolean
  /** 表格瘦身：只输出表头 + 命中行（section/keyword 节选时）。 */
  tableRows?: number[] | undefined
  /** 允许的块类型集合（白名单过滤，未传不过滤）。 */
  typeFilter?: Set<number>
  /** 降级/说明收集。 */
  notes: Set<string>
}

/** 块 id 属性片段（ids 模式）。 */
function idAttr(b: Record<string, unknown>, opts: FetchXmlOptions): string {
  return opts.ids ? ` id="${escapeAttr(String(b.block_id ?? ""))}"` : ""
}

/** 块类型 → 字段名映射（与 docx BLOCK_TYPE_NAME 对齐）。 */
const BLOCK_FIELD: Record<number, string> = {
  2: "text", 3: "heading1", 4: "heading2", 5: "heading3", 6: "heading4", 7: "heading5", 8: "heading6",
  9: "heading7", 10: "heading8", 11: "heading9", 12: "bullet", 13: "ordered", 14: "code", 15: "quote",
  17: "todo", 19: "callout", 22: "divider", 24: "grid", 25: "grid_column", 27: "image", 31: "table",
  32: "table_cell", 35: "embed", 37: "file", 39: "sheet", 43: "mindnote", 44: "bitable",
}

/** 单块的字段对象（text/heading1/bullet/code/…）。 */
function fieldOf(b: Record<string, unknown>): Record<string, unknown> | undefined {
  const t = Number(b.block_type ?? 0)
  const name = BLOCK_FIELD[t]
  const v = name ? (b[name] as Record<string, unknown> | undefined) : undefined
  return v && typeof v === "object" ? v : undefined
}

/** 文本类块类型（有 elements 的）。 */
const TEXTUAL = new Set([2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17])

/** 子块 id 列表（children 引用，按序）。 */
function childIds(b: Record<string, unknown>): string[] {
  return Array.isArray(b.children) ? (b.children as unknown[]).map((c) => String(c)) : []
}

/** 对齐属性片段。 */
function alignAttr(field: Record<string, unknown> | undefined, opts: FetchXmlOptions): string {
  if (!opts.styles) return ""
  const align = alignName((field?.style as Record<string, unknown> | undefined)?.align)
  return align ? ` align="${align}"` : ""
}

/** 单块 → XML（容器递归展开）。返回 undefined 表示该块不可序列化（过滤或未知类型）。 */
export function blockToXml(blockId: string, opts: FetchXmlOptions, depth = 0): string | undefined {
  const { byId, notes } = opts
  const b = byId.get(blockId)
  if (!b) return undefined
  const t = Number(b.block_type ?? 0)
  if (opts.typeFilter && !opts.typeFilter.has(t)) return undefined
  if (depth > 12) {
    notes.add("块嵌套超过 12 层，深层内容已省略")
    return undefined
  }
  const field = fieldOf(b)
  const children = childIds(b)
  const kidsXml = (): string => children.map((id) => blockToXml(id, opts, depth + 1)).filter(Boolean).join("\n")

  // 文本类
  if (TEXTUAL.has(t)) {
    const elements = (field?.elements as unknown[] | undefined) ?? []
    const inner = opts.styles ? inlineToXml(elements) : inlinePlain(elements)
    if (t === 17) {
      const done = (field?.style as Record<string, unknown> | undefined)?.done === true
      return `<checkbox${done ? ' done="true"' : ""}${idAttr(b, opts)}${alignAttr(field, opts)}>${inner}</checkbox>`
    }
    if (t === 14) {
      // language 可能是枚举数字（服务端）或语言名（xmlToBlocks 简化写法，normalizeBlockFields 才转枚举）
      const rawLang = (field?.style as Record<string, unknown> | undefined)?.language
      const lang = typeof rawLang === "number" ? codeLangName(rawLang) : String(rawLang ?? "")
      const body = elements.map((e) => String((e as { text_run?: { content?: string } }).text_run?.content ?? "")).join("")
      const langAttr = opts.styles && lang ? ` lang="${escapeAttr(lang)}"` : ""
      return `<pre${langAttr}${idAttr(b, opts)}><code>${escapeXmlText(body)}</code></pre>`
    }
    if (t === 15) {
      // 引用块：飞书 quote 无子块，文本整体（含软换行）放标签内
      return `<blockquote${idAttr(b, opts)}>${inner}</blockquote>`
    }
    if (t >= 3 && t <= 11) {
      return `<h${t - 2}${idAttr(b, opts)}${alignAttr(field, opts)}>${inner}</h${t - 2}>`
    }
    if (t === 13) {
      const seq = opts.styles ? numberSeq((field?.style as Record<string, unknown> | undefined)?.sequence) : ""
      const nested = nestedListXml(children, opts, depth)
      return `<ol${seq}${idAttr(b, opts)}><li>${inner}${nested}</li></ol>`
    }
    if (t === 12) {
      const nested = nestedListXml(children, opts, depth)
      return `<ul${idAttr(b, opts)}><li>${inner}${nested}</li></ul>`
    }
    return `<p${idAttr(b, opts)}${alignAttr(field, opts)}>${inner}</p>`
  }

  // 容器与富块
  switch (t) {
    case 22:
      return `<hr${idAttr(b, opts)}/>`
    case 19: {
      const attrs: string[] = []
      if (opts.styles) {
        const emoji = field?.emoji_id
        if (typeof emoji === "string" && emoji) attrs.push(`emoji="${escapeAttr(emoji)}"`)
        const bg = colorName(field?.background_color, true)
        if (bg) attrs.push(`background-color="${bg}"`)
        const border = colorName(field?.border_color)
        if (border) attrs.push(`border-color="${border}"`)
        const txt = colorName(field?.text_color)
        if (txt) attrs.push(`text-color="${txt}"`)
      }
      const inner = kidsXml()
      return `<callout${attrs.length ? " " + attrs.join(" ") : ""}${idAttr(b, opts)}>${inner}</callout>`
    }
    case 24: {
      const cols = children.map((id) => blockToXml(id, opts, depth + 1)).filter(Boolean)
      return `<grid${idAttr(b, opts)}>${cols.join("\n")}</grid>`
    }
    case 25: {
      const ratio = Number(field?.width_ratio ?? 1)
      const inner = kidsXml()
      return `<column width-ratio="${Number.isFinite(ratio) && ratio > 0 ? ratio : 1}"${idAttr(b, opts)}>${inner}</column>`
    }
    case 31:
      return tableToXml(b, opts, depth)
    case 27: {
      const img = (b.image ?? {}) as Record<string, unknown>
      const token = typeof img.token === "string" ? img.token : ""
      const size = opts.styles && (img.width || img.height) ? ` width="${img.width ?? 0}" height="${img.height ?? 0}"` : ""
      return `<img src="${escapeAttr(token)}"${size}${idAttr(b, opts)}/>`
    }
    case 16:
      return `<latex${idAttr(b, opts)}>${escapeXmlText(String((field?.elements as unknown[] | undefined)?.map((e) => (e as { equation?: { content?: string } }).equation?.content ?? "").join("") ?? ""))}</latex>`
    case 35: {
      const url = String((field as Record<string, unknown> | undefined)?.url ?? "")
      return `<iframe embed-url="${escapeAttr(url)}"${idAttr(b, opts)}/>`
    }
    case 37:
    case 43:
    case 44:
    case 39: {
      const token = String((field as Record<string, unknown> | undefined)?.token ?? "")
      const tag = t === 37 ? "source" : t === 43 ? "whiteboard" : t === 44 ? "bitable" : "sheet"
      return `<${tag} token="${escapeAttr(token)}"${idAttr(b, opts)}/>`
    }
    default: {
      // 未知/暂不支持类型：给占位（保留 block_id 可定位）——不静默丢块
      notes.add(`块类型 ${t}（${String(b.block_id ?? "")}）暂不支持序列化，已用占位表示`)
      return `<!-- block_type=${t} id=${String(b.block_id ?? "")} 不支持序列化 -->`
    }
  }
}

/** 有序列表起始序号属性（sequence 存在且 ≠ 1 时输出 seq）。 */
function numberSeq(seq: unknown): string {
  const v = Number(seq)
  return Number.isFinite(v) && v !== 1 && v > 0 ? ` seq="${v}"` : ""
}

/** 列表项的嵌套子列表（bullet/ordered 子块）。 */
function nestedListXml(children: string[], opts: FetchXmlOptions, depth: number): string {
  return children
    .map((id) => blockToXml(id, opts, depth + 1))
    .filter(Boolean)
    .join("")
}

/** 表格 → XML：colgroup 列宽 + thead（header_row）+ tbody；tableRows 白名单为行下标（瘦身用）。
 *  兼容两种形态：服务端结构（children: [table_cell] + property）与 rows 简化写法（xmlToBlocks 产物）。 */
function tableToXml(b: Record<string, unknown>, opts: FetchXmlOptions, depth: number): string {
  const { byId, notes } = opts
  const table = (b.table ?? {}) as Record<string, unknown>
  // rows 简化写法（xmlToBlocks 产物，无 children/property）
  if (Array.isArray(table.rows) && !Array.isArray(b.children)) {
    const rows = table.rows as string[][]
    const headerRow = table.header_row === true
    const widths = Array.isArray(table.column_width) ? (table.column_width as unknown[]).map((x) => Number(x)) : []
    const lines: string[] = ["<table" + idAttr(b, opts) + ">"]
    if (opts.styles && widths.length === rows[0]?.length) {
      lines.push(`<colgroup>${widths.map((w) => `<col width="${Math.round(w)}"/>`).join("")}</colgroup>`)
    }
    rows.forEach((row, r) => {
      if (opts.tableRows && !opts.tableRows.includes(r) && !(headerRow && r === 0)) return
      const head = headerRow && r === 0
      const cells = row.map((cell) => `<${head ? "th" : "td"}>${escapeXmlText(String(cell ?? "")) || "（空）"}</${head ? "th" : "td"}>`).join("")
      lines.push(head ? `<thead><tr>${cells}</tr></thead>` : `<tr>${cells}</tr>`)
    })
    lines.push("</table>")
    return lines.join("\n")
  }
  const cells = childIds(b).map((id) => byId.get(id)).filter((x): x is Record<string, unknown> => !!x)
  // 飞书 docx 表格结构：table.children = [table_cell(0,0), table_cell(0,1), ..., table_cell(1,0), ...]（行优先扁平）；
  // 服务端返回 column_size/row_size/column_width/header_row 都在 table.property 下（实测）
  const property = (table.property as Record<string, unknown> | undefined) ?? {}
  const columnSize = Number(property.column_size ?? table.column_size ?? 0)
  const rowSize = Number(property.row_size ?? table.row_size ?? (columnSize ? Math.floor(cells.length / columnSize) : 0))
  const widths = Array.isArray(property.column_width) ? (property.column_width as unknown[]).map((x) => Number(x)) : []
  const headerRow = property.header_row === true
  const rowFilter = opts.tableRows

  const lines: string[] = ["<table" + idAttr(b, opts) + ">"]
  if (opts.styles && widths.length === columnSize && columnSize > 0) {
    lines.push(`<colgroup>${widths.map((w) => `<col width="${Math.round(w)}"/>`).join("")}</colgroup>`)
  }
  const cellXml = (cell: Record<string, unknown>, isHead: boolean): string => {
    const tag = isHead ? "th" : "td"
    let inner = childIds(cell)
      .map((id) => blockToXml(id, opts, depth + 2))
      .filter(Boolean)
      .join("")
    // 单段内容摘掉外层 <p>（单元格内段落冗余；多段/列表保留结构，<p> 与 <br/> 分隔）
    if (inner.startsWith("<p") && inner.endsWith("</p>") && !inner.slice(3).includes("<p")) inner = inner.replace(/^<p[^>]*>/, "").replace(/<\/p>$/, "")
    return `<${tag}${idAttr(cell, opts)}>${inner || "（空）"}</${tag}>`
  }
  for (let r = 0; r < rowSize; r++) {
    if (rowFilter && !rowFilter.includes(r) && !(headerRow && r === 0)) continue
    const head = headerRow && r === 0
    const rowCells: string[] = []
    for (let c = 0; c < columnSize; c++) {
      const cell = cells[r * columnSize + c]
      rowCells.push(cell ? cellXml(cell, head) : `<td/>`)
    }
    lines.push(head ? `<thead><tr>${rowCells.join("")}</tr></thead>` : `<tr>${rowCells.join("")}</tr>`)
  }
  if (lines.length <= 2) notes.add("空表格")
  lines.push("</table>")
  return lines.join("\n")
}

/** 行内纯文本（simple 模式：不带样式标签）。 */
function inlinePlain(elements: unknown[] | undefined): string {
  if (!Array.isArray(elements)) return ""
  return escapeXmlText(inlinePlainText(elements))
}

/** 行内纯文本拼接（含 mention 文本化）。 */
export function inlinePlainText(elements: unknown[] | undefined): string {
  if (!Array.isArray(elements)) return ""
  const parts: string[] = []
  for (const el of elements) {
    const e = el as Record<string, unknown>
    const run = e.text_run as { content?: string } | undefined
    if (run?.content) parts.push(run.content)
    else if (e.mention_user) parts.push("@用户")
    else if (e.mention_doc) parts.push("@文档")
    else if (e.mention_all) parts.push("@所有人")
    else if ((e as { equation?: { content?: string } }).equation?.content) parts.push(String((e as { equation?: { content?: string } }).equation?.content))
  }
  return parts.join("")
}

/** 代码块语言枚举 → 名称（feishu_api CODE_LANG_NAMES 的本地副本，避免循环依赖）。 */
const CODE_LANGS = [
  "PlainText", "ABAP", "Ada", "Apache", "Apex", "Assembly Language", "Bash", "CSharp", "C++", "C",
  "COBOL", "CSS", "CoffeeScript", "D", "Dart", "Delphi", "Django", "Dockerfile", "Erlang", "Fortran",
  "FoxPro", "Go", "Groovy", "HTML", "HTMLBars", "HTTP", "Haskell", "JSON", "Java", "JavaScript",
  "Julia", "Kotlin", "LateX", "Lisp", "Logo", "Lua", "MATLAB", "Makefile", "Markdown", "Nginx",
  "Objective-C", "OpenEdgeABL", "PHP", "Perl", "PostScript", "Power Shell", "Prolog", "ProtoBuf", "Python", "R",
  "RPG", "Ruby", "Rust", "SAS", "SCSS", "SQL", "Scala", "Scheme", "Scratch", "Shell",
  "Swift", "Thrift", "TypeScript", "VBScript", "Visual Basic", "XML", "YAML", "CMake", "Diff", "Gherkin",
  "GraphQL", "OpenGL Shading Language", "Properties", "Solidity", "TOML",
]

function codeLangName(lang: unknown): string {
  const n = Number(lang)
  if (!Number.isFinite(n) || n < 1 || n > CODE_LANGS.length) return ""
  return CODE_LANGS[n - 1]
}

/* ================= scope 选择性读取 ================= */

export interface ScopeResult {
  /** 输出 XML 段落（0~n 个顶层块/excerpt）。 */
  xml: string
  /** 命中的顶层块 id（excerpt 的 top-block-id 含在内）。 */
  topBlockIds: string[]
  /** 说明（模式、过滤、降级）。 */
  notes: string[]
}

export interface ScopeCommon {
  /** 顶层块 id 序列（page 的 children）。 */
  topIds: string[]
  byId: Map<string, Record<string, unknown>>
  /** 顶层块 id → 同级序号。 */
  topIndex: Map<string, number>
}

/** 构建顶层索引（rootId 未知时按 parent 推断顶层）。 */
export function buildScopeCommon(byId: Map<string, Record<string, unknown>>, rootId?: string): ScopeCommon {
  let topIds: string[]
  const root = rootId ? byId.get(rootId) : undefined
  if (root) {
    topIds = childIds(root)
  } else {
    topIds = [...byId.keys()].filter((id) => {
      const b = byId.get(id)
      const pid = String(b?.parent_id ?? "")
      return !pid || !byId.has(pid)
    })
  }
  const topIndex = new Map(topIds.map((id, i) => [id, i]))
  return { topIds, byId, topIndex }
}

/** 块（或其子树内任一块）的顶层祖先。 */
export function topAncestorOf(blockId: string, byId: Map<string, Record<string, unknown>>, topIndex: Map<string, number>): string | undefined {
  let cur = blockId
  for (let i = 0; i < 64; i++) {
    if (topIndex.has(cur)) return cur
    const b = byId.get(cur)
    const pid = b ? String(b.parent_id ?? "") : ""
    if (!pid || !byId.has(pid)) return undefined
    cur = pid
  }
  return undefined
}

/** 块自身或子树是否命中关键词（任一分支）。 */
export function blockMatches(blockId: string, byId: Map<string, Record<string, unknown>>, terms: string[], maxDepth = 12): boolean {
  if (!terms.length) return false
  const walk = (id: string, depth: number): boolean => {
    if (depth > maxDepth) return false
    const b = byId.get(id)
    if (!b) return false
    const field = fieldOf(b)
    const text = field?.elements ? inlinePlainText(field.elements as unknown[]) : ""
    if (terms.some((term) => text.toLowerCase().includes(term))) return true
    return childIds(b).some((cid) => walk(cid, depth + 1))
  }
  return walk(blockId, 0)
}

/** section：锚点块（顶层标题）到下一个同级或更高级标题前的全部顶层块；非标题锚点只返回该块。 */
export function sectionScope(common: ScopeCommon, anchorId: string, opts: FetchXmlOptions): ScopeResult {
  const { topIds, byId, topIndex } = common
  const notes: string[] = []
  const idx = topIndex.get(anchorId)
  if (idx === undefined) {
    // 锚点不在顶层：返回其所在顶层块（最小包容单元）
    const top = topAncestorOf(anchorId, byId, topIndex)
    if (!top) return { xml: "", topBlockIds: [], notes: [`块 ${anchorId} 不在文档顶层序列中`] }
    notes.push(`锚点不是顶层块，已返回其所在顶层块（${top}）`)
    const merged = new Set([...opts.notes])
    const xml = blockToXml(top, { ...opts, notes: merged }) ?? ""
    return { xml, topBlockIds: [top], notes: [...merged] }
  }
  const anchor = byId.get(anchorId)
  const anchorLevel = Number(anchor?.block_type ?? 0)
  const isHeading = anchorLevel >= 3 && anchorLevel <= 11
  const out: string[] = []
  const tops: string[] = []
  for (let i = idx; i < topIds.length; i++) {
    const id = topIds[i]
    const b = byId.get(id)
    const level = Number(b?.block_type ?? 0)
    if (i > idx && level >= 3 && level <= 11 && level <= anchorLevel) break
    if (!isHeading && i > idx) break
    const xml = blockToXml(id, opts)
    if (xml !== undefined) {
      out.push(xml)
      tops.push(id)
    }
  }
  return { xml: out.join("\n"), topBlockIds: tops, notes }
}

/** range：顶层区间 [startIdx, endIdx]（闭区间；端点给完整块）。start/end 缺省取文档首/末。 */
export function rangeScope(common: ScopeCommon, startId: string | undefined, endId: string | undefined, opts: FetchXmlOptions): ScopeResult {
  const { topIds, byId, topIndex } = common
  const notes: string[] = []
  const resolve = (id: string | undefined): number | undefined => {
    if (id === undefined) return undefined
    const direct = topIndex.get(id)
    if (direct !== undefined) return direct
    const top = topAncestorOf(id, byId, topIndex)
    return top ? topIndex.get(top) : undefined
  }
  let start = resolve(startId)
  let end = resolve(endId)
  if (start === undefined && end === undefined) return { xml: "", topBlockIds: [], notes: ["range 缺少有效端点"] }
  if (start === undefined) start = 0
  if (end === undefined) end = topIds.length - 1
  if (start > end) {
    notes.push(`区间端点顺序已自动纠正（${start} > ${end}）`)
    ;[start, end] = [end, start]
  }
  const out: string[] = []
  const tops: string[] = []
  for (let i = start; i <= end && i < topIds.length; i++) {
    const xml = blockToXml(topIds[i], opts)
    if (xml !== undefined) {
      out.push(xml)
      tops.push(topIds[i])
    }
  }
  return { xml: out.join("\n"), topBlockIds: tops, notes }
}

/** keyword：按关键词（| 分隔 OR）命中顶层块；命中输出完整块（容器整块返回，结构完整可改）。 */
export function keywordScope(common: ScopeCommon, keyword: string, contextBefore: number, contextAfter: number, opts: FetchXmlOptions): ScopeResult {
  const { topIds, byId } = common
  const notes: string[] = []
  const terms = keyword.split("|").map((s) => s.trim().toLowerCase()).filter(Boolean)
  if (!terms.length) return { xml: "", topBlockIds: [], notes: ["keyword 为空"] }
  const hits: number[] = []
  for (let i = 0; i < topIds.length; i++) {
    if (blockMatches(topIds[i], byId, terms)) hits.push(i)
  }
  if (!hits.length) return { xml: "", topBlockIds: [], notes: [`未命中关键词：${keyword}`] }
  const include = new Set<number>()
  for (const h of hits) {
    for (let i = Math.max(0, h - contextBefore); i <= Math.min(topIds.length - 1, h + contextAfter); i++) include.add(i)
  }
  const out: string[] = []
  const tops: string[] = []
  const hitSet = new Set(hits)
  for (const i of [...include].sort((a, b) => a - b)) {
    const id = topIds[i]
    const b = byId.get(id)
    const isHit = hitSet.has(i)
    const direct = b ? directTextHit(b, terms) : false
    const xml = blockToXml(id, opts)
    if (xml === undefined) continue
    if (isHit && direct) {
      out.push(xml)
    } else {
      // 命中在容器内或上下文行：excerpt 标注（top-block-id 可作回读锚点）
      out.push(`<excerpt top-block-id="${id}"${isHit ? "" : ' role="context"'}>${xml}</excerpt>`)
    }
    tops.push(id)
  }
  notes.push(`命中 ${hits.length} 个顶层块`)
  return { xml: out.join("\n"), topBlockIds: tops, notes }
}

/** 顶层块自身（非子树）文本是否直接命中。 */
function directTextHit(b: Record<string, unknown>, terms: string[]): boolean {
  const field = fieldOf(b)
  if (!field?.elements) return false
  const text = inlinePlainText(field.elements as unknown[]).toLowerCase()
  return terms.some((t) => text.includes(t))
}

/** outline：扁平标题列表（层级/文本/id/每节块数）。 */
export function outlineScope(common: ScopeCommon, maxDepth: number): ScopeResult {
  const { topIds, byId, topIndex } = common
  const notes: string[] = []
  const lines: string[] = []
  const tops: string[] = []
  for (const id of topIds) {
    const b = byId.get(id)
    if (!b) continue
    const t = Number(b.block_type ?? 0)
    if (t < 3 || t > 11) continue
    const level = t - 2
    if (level > maxDepth) continue
    const text = inlinePlainText((fieldOf(b)?.elements as unknown[]) ?? "") || "（无标题文本）"
    // 节内块数：该标题到下一个 ≤ 同级标题之间的顶层块数
    const idx = topIndex.get(id) ?? 0
    let end = topIds.length
    for (let j = idx + 1; j < topIds.length; j++) {
      const nb = byId.get(topIds[j])
      const nt = Number(nb?.block_type ?? 0)
      if (nt >= 3 && nt <= 11 && nt - 2 <= level) {
        end = j
        break
      }
    }
    lines.push(`${"  ".repeat(Math.max(0, level - 1))}<h${level} id="${id}">${escapeXmlText(text)}</h${level}> <!-- 节内 ${end - idx - 1} 个顶层块 -->`)
    tops.push(id)
  }
  if (!lines.length) notes.push("文档没有标题块")
  return { xml: lines.join("\n"), topBlockIds: tops, notes }
}
