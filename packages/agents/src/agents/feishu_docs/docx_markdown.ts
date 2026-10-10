/**
 * 块树 → Markdown 反向序列化（get_doc_text format=markdown 的纯函数核心）。
 *
 * 与 feishu_api.ts 的 `markdownToBlocks` 严格对称（round-trip）：读出什么、改什么、写回什么。
 * Markdown 表达力是 XML 的子集，故本模块覆盖 import_markdown 认识的全部语法；超出子集的
 * 块（高亮块配色/分栏/画板/表格列宽/题注等）只有 XML 通道能保真——Markdown 侧按「语法能表达的
 * 近似形式 + 丢失信息提示（notes）」处理，不静默丢内容。
 *
 * 零飞书 HTTP 依赖，可独立测试。
 */

/* ================= 行内元素 → Markdown ================= */

/** 行内可转义字符（与解析端 textElements 的转义面一致——单一数据源，两侧必须对齐）。 */
export const MD_ESCAPABLE = "\\`*_{}[]()#+-.!~$"

const ESCAPE_CLASS = MD_ESCAPABLE.replace(/[\]^-]/g, "\\$&")
const INLINE_ESCAPE_RE = new RegExp(`([${ESCAPE_CLASS}])`, "g")
const LINE_START_ESCAPE_RE = new RegExp(`^(\\s*)(#{1,9}\\s|[-*+]\\s|\\d{1,9}[.)]\\s|>\\s?|-{3,}\\s*$|\\*{3,}\\s*$|_{3,}\\s*$)`)

/**
 * 行内 Markdown 转义（防止读出的内容再写回时被当行内语法消费）。
 * 块级行首标记（标题/列表/引用/分割线）由 escapeMdLine 处理（仅在行首生效，与解析端一致）。
 */
export function escapeMd(text: string): string {
  return text.replace(INLINE_ESCAPE_RE, "\\$1")
}

/** 行首块级标记转义（标题/列表/引用/分割线——仅在行首才有语法含义）。 */
export function escapeMdLine(text: string): string {
  return text.replace(LINE_START_ESCAPE_RE, "$1\\$2")
}

/**
 * 还原 Markdown 转义（与 escapeMd 对称）。
 * `\X`（X 属于 MD_ESCAPABLE）→ 字面 X；其余反斜杠保留原样（Markdown 标准语义）。
 */
export function unescapeMd(text: string): string {
  return text.replace(new RegExp(`\\([${ESCAPE_CLASS}])`, "g"), "$1")
}

interface RunStyle {
  bold?: boolean
  italic?: boolean
  underline?: boolean
  strikethrough?: boolean
  inline_code?: boolean
  link?: { url?: string }
}

/** 单段文本 + 样式 → Markdown（与 textElements 的解析面反向对应：`***x***` / `**x**` / `*x*` / `~~x~~` / `` `x` `` / `[x](url)`）。 */
function wrapRun(text: string, st: RunStyle | undefined): string {
  if (!text) return ""
  if (st?.inline_code) return `\`${text.replace(/`/g, "\\`")}\``
  let s = escapeMd(text)
  if (st?.bold && st?.italic) s = `***${s}***`
  else if (st?.bold) s = `**${s}**`
  else if (st?.italic) s = `*${s}*`
  if (st?.strikethrough) s = `~~${s}~~`
  if (st?.link?.url) s = `[${s}](${st.link.url})`
  return s
}

/** 行内元素数组 → Markdown 串（text_run/mention/equation）。 */
export function inlineToMarkdown(elements: unknown[] | undefined): string {
  if (!Array.isArray(elements)) return ""
  const parts: string[] = []
  for (const el of elements) {
    const e = el as Record<string, unknown>
    const run = e.text_run as { content?: string; text_element_style?: RunStyle } | undefined
    if (run) {
      parts.push(wrapRun(String(run.content ?? ""), run.text_element_style))
      continue
    }
    const mu = e.mention_user as { user_id?: string } | undefined
    if (mu?.user_id) {
      parts.push(`@${mu.user_id}`) // @人在 Markdown 无语法，按可见文本落地（XML 通道可保真）
      continue
    }
    if (e.mention_doc) {
      parts.push("@文档")
      continue
    }
    if (e.mention_all) {
      parts.push("@所有人")
      continue
    }
    const eq = e.equation as { content?: string } | undefined
    if (eq?.content) parts.push(`$${eq.content}$`)
  }
  return parts.join("")
}

/** 块行内纯文本（无样式，用于摘要/代码/单元格等）。 */
export function inlinePlain(elements: unknown[] | undefined): string {
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

/* ================= 块树 → Markdown ================= */

export interface MdOptions {
  byId: Map<string, Record<string, unknown>>
  /** 收集丢失的表达力（Markdown 子集外的块属性）。 */
  notes: Set<string>
}

/** 块字段名映射（与 import_markdown / markdownToBlocks 产出对齐）。 */
const BLOCK_FIELD: Record<number, string> = {
  2: "text", 3: "heading1", 4: "heading2", 5: "heading3", 6: "heading4", 7: "heading5", 8: "heading6",
  9: "heading7", 10: "heading8", 11: "heading9", 12: "bullet", 13: "ordered", 14: "code", 15: "quote",
  17: "todo", 19: "callout", 24: "grid", 25: "grid_column", 27: "image", 31: "table",
  32: "table_cell", 35: "embed", 37: "file", 39: "sheet", 43: "mindnote", 44: "bitable",
}

function fieldOf(b: Record<string, unknown>): Record<string, unknown> | undefined {
  const t = Number(b.block_type ?? 0)
  const name = BLOCK_FIELD[t]
  const v = name ? (b[name] as Record<string, unknown> | undefined) : undefined
  return v && typeof v === "object" ? v : undefined
}

function childIds(b: Record<string, unknown>): string[] {
  return Array.isArray(b.children) ? (b.children as unknown[]).map((c) => String(c)) : []
}

/** 单个列表项行前缀（有序递增/无序/待办勾选态）。 */
function listMarker(t: number, counter: number, b: Record<string, unknown>): string {
  if (t === 13) return `${counter + 1}. `
  if (t === 17) {
    const done = (fieldOf(b)?.style as Record<string, unknown> | undefined)?.done === true
    return `- [${done ? "x" : " "}] `
  }
  return "- "
}

/**
 * 块序列 → 块级 Markdown 片段（**统一递归入口**）。
 * 每个顶层块（含容器）产出独立片段，片段间以空行分隔——与 markdownToBlocks 的块识别边界一致
 * （缺空行会让相邻块被合并/误判：列表被并入段落、表格与上段粘连等）。
 * 连续同类列表项在片段内聚合（有序编号递增、子项缩进一级）。
 */
function renderSequence(ids: string[], opts: MdOptions, indent: string): string[] {
  const blocks: string[] = []
  let i = 0
  while (i < ids.length) {
    const b = opts.byId.get(ids[i])
    if (!b) {
      i++
      continue
    }
    const t = Number(b.block_type ?? 0)

    // page 根块：内容即其子序列（片段直接下放，不额外包裹）
    if (t === 1) {
      blocks.push(...renderSequence(childIds(b), opts, indent))
      i++
      continue
    }

    // 连续同类列表项：聚合为同一列表片段（编号递增；子项缩进一级）
    if (t === 12 || t === 13 || t === 17) {
      const lines: string[] = []
      let counter = 0
      while (i < ids.length && Number(opts.byId.get(ids[i])?.block_type ?? 0) === t) {
        const node = opts.byId.get(ids[i])!
        const els = fieldOf(node)?.elements as unknown[] | undefined
        lines.push(`${indent}${listMarker(t, counter, node)}${inlineToMarkdown(els)}`)
        counter++
        const kids = childIds(node)
        if (kids.length) {
          // 嵌套子列表：缩进一级，子块片段去掉空行（保持列表紧凑）
          for (const seg of renderSequence(kids, opts, `${indent}  `)) {
            for (const l of seg.split("\n")) lines.push(l)
          }
        }
        i++
      }
      blocks.push(lines.join("\n"))
      continue
    }

    const seg = renderBlock(b, opts, indent)
    if (seg !== undefined && seg !== "") blocks.push(seg)
    i++
  }
  return blocks
}

/** 单块 → 块级 Markdown 片段（非列表块；容器递归经 renderSequence）。 */
function renderBlock(b: Record<string, unknown>, opts: MdOptions, indent: string): string | undefined {
  const t = Number(b.block_type ?? 0)
  const field = fieldOf(b)
  const kids = childIds(b)
  const els = field?.elements as unknown[] | undefined

  // 标题 3~11（文本内转义，标记自身不转义）
  if (t >= 3 && t <= 11) {
    return `${"#".repeat(t - 2)} ${inlineToMarkdown(els)}`
  }

  switch (t) {
    case 2: {
      // 段落：首行缩进（OneLevelIndent）→ 全角空格前缀（与解析端一致）
      const indentation = (field?.style as Record<string, unknown> | undefined)?.indentation_level
      const prefix = indentation === "OneLevelIndent" ? "\u3000\u3000" : ""
      return escapeMdLine(prefix + inlineToMarkdown(els))
    }
    case 14: {
      const lang = (field?.style as Record<string, unknown> | undefined)?.language
      const langName = typeof lang === "string" ? lang : codeLangName(lang)
      const body = (els ?? []).map((e) => String((e as { text_run?: { content?: string } }).text_run?.content ?? "")).join("")
      return ["```" + langFence(langName), body, "```"].join("\n")
    }
    case 15: {
      const text = inlineToMarkdown(els).replace(/\n/g, "\n> ")
      return `> ${escapeMdLine(text).replace(/\n> /g, "\n> ")}`
    }
    case 22:
      return "---"
    case 19: {
      // 高亮块 → GitHub 告示语法（与解析端 ALERT_RE 对齐；emoji_id 映射到告示类型）
      const kind = calloutAlertKind(field?.emoji_id === undefined ? undefined : String(field.emoji_id))
      const inner = renderSequence(kids, opts, "")
      const lines = [`> [!${kind}]`]
      for (const seg of inner) for (const l of seg.split("\n")) lines.push(l === "" ? ">" : `> ${l}`)
      return lines.join("\n")
    }
    case 24: {
      // 分栏 → 顺序输出各列内容（Markdown 无分栏语法）
      opts.notes.add("分栏（grid）在 Markdown 中按列顺序展开——需保留分栏请用 XML 通道（fetch_doc detail=full）")
      return renderSequence(kids, opts, indent).join("\n\n")
    }
    case 25:
      return renderSequence(kids, opts, indent).join("\n\n")
    case 31: {
      const rows = tableRowsOf(b, opts)
      if (!rows.length) {
        opts.notes.add(`表格块 ${String(b.block_id ?? "")} 无可读行列（空表格）`)
        return undefined
      }
      const colSize = Math.max(...rows.map((r) => r.length))
      const pad = (r: string[]): string[] => [...r, ...Array.from({ length: colSize - r.length }, () => "")]
      const cell = (c: string): string => c.replace(/\|/g, "\\|") || " "
      const lines = [`| ${pad(rows[0]).map(cell).join(" | ")} |`, `| ${Array.from({ length: colSize }, () => "---").join(" | ")} |`]
      for (const r of rows.slice(1)) lines.push(`| ${pad(r).map(cell).join(" | ")} |`)
      return lines.join("\n")
    }
    case 27: {
      const token = String((b.image as Record<string, unknown> | undefined)?.token ?? "")
      opts.notes.add("图片以 token 占位（feishu-image:<token>）——Markdown 无飞书图片素材语法；下载图片用 download_file 传 extra={document_id, block_id}")
      return token ? `![图片](feishu-image:${token})` : "![图片]()"
    }
    case 43:
    case 44:
    case 39:
    case 35:
    case 37: {
      const name = t === 43 ? "画板/思维导图" : t === 44 ? "多维表格" : t === 39 ? "电子表格" : t === 35 ? "嵌入内容" : "附件"
      opts.notes.add(`${name}（块类型 ${t}）在 Markdown 中无对应语法，已用占位标记——需保真请用 XML 通道`)
      return `[${name}](feishu-block:${String(b.block_id ?? "")})`
    }
    default:
      // 未知类型：有文本则按段落落地，否则占位（不静默丢块）
      if (els) return escapeMdLine(inlineToMarkdown(els))
      opts.notes.add(`块类型 ${t} 在 Markdown 中无对应语法，已用注释占位`)
      return `<!-- feishu-block:${String(b.block_id ?? "")} type=${t} -->`
  }
}

/** 代码语言枚举值 → 枚举名（供围栏标识）。 */
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
  if (!Number.isFinite(n) || n < 1) return ""
  return CODE_LANGS[n - 1] ?? ""
}

/** 高亮块 emoji → GitHub 告示类型（与解析端 ALERT_RE 的可选值域一致）。 */
function calloutAlertKind(emoji: string | undefined): string {
  switch (String(emoji ?? "").toLowerCase()) {
    case "warning":
    case "caution":
      return "WARNING"
    case "pushpin":
      return "IMPORTANT"
    case "check":
      return "TIP"
    case "x":
      return "CAUTION"
    default:
      return "NOTE"
  }
}

/** 表格块 → 单元格文本二维数组（按 column_size 分行；单元格取子树纯文本）。 */
function tableRowsOf(table: Record<string, unknown>, opts: MdOptions): string[][] {
  const prop = ((table.table ?? {}) as Record<string, unknown>).property as Record<string, unknown> | undefined
  const colSize = Number(prop?.column_size ?? 0)
  const cells = childIds(table)
  if (!colSize) return []
  const rows: string[][] = []
  for (let i = 0; i < cells.length; i += colSize) {
    const row: string[] = []
    for (let c = 0; c < colSize; c++) {
      const cell = opts.byId.get(cells[i + c])
      if (!cell) {
        row.push("")
        continue
      }
      // 单元格内多段：<br> 拼接（与解析端「连续两个换行拆段」对应）
      const parts = childIds(cell)
        .map((id) => {
          const cb = opts.byId.get(id)
          if (!cb) return ""
          return inlinePlain(fieldOf(cb)?.elements as unknown[] | undefined)
        })
        .filter((x) => x !== "")
      row.push(parts.join("<br>"))
    }
    rows.push(row)
  }
  return rows
}

/**
 * 文档块序列 → Markdown（顶层入口）。
 *
 * `ids` 为顶层块 id 序列；返回 Markdown 文本与丢失表达力的说明（notes）。
 * 连续同类列表项按飞书模型各自成块，序列化时聚合为同一列表（同级缩进、有序编号递增）。
 */
export function blocksToMarkdown(ids: string[], opts: MdOptions): { markdown: string; notes: string[] } {
  const blocks = renderSequence(ids, opts, "")
  const md = blocks.join("\n\n").replace(/\n{3,}/g, "\n\n").trim()
  return { markdown: md, notes: [...opts.notes] }
}

/** 代码语言枚举名 → 围栏标识（小写、去空格/连字符；再导入时由导入端映射回枚举）。 */
function langFence(name: string): string {
  return name.toLowerCase().replace(/[\s-]+/g, "")
}
