/**
 * XML 排版语法（类 HTML）→ 飞书 docx 块描述。
 *
 * 纯转换层：输入 XML 文本，输出与 `add_blocks` 简化写法同构的**块描述**数组
 * （`{ block_type, [字段]: {...} }`），由 feishu_api 的块构造流程（normalizeBlockFields/buildGroup）
 * 落地为块组——本模块零飞书 HTTP 依赖，可独立测试。
 *
 * 为什么要有这一层：Markdown 只能表达飞书块模型的子集（自动编号、分栏、高亮块配色、表格列宽、
 * 题注都没有语法），文案「怎么修都排版不好看」的根因在此。XML 语法补齐这些表达：
 *   - `<h1 seq="auto">` 自动编号（1 / 1.1 / 1.1.1）
 *   - `<grid><column width-ratio="0.5">` 分栏（2~5 列）
 *   - `<callout emoji="💡" background-color="light-blue">` 高亮块配色
 *   - `<table><colgroup><col width="180"/>` 表格列宽 + `<th>` 表头行
 *   - `<img caption="题注">` / `<pre lang="go" caption="示例">` 题注
 *   - `<whiteboard type="mermaid">` 图示（服务端渲染为图片）
 *
 * `_` 前缀字段为本地元数据（图片图源、图表源码），发送飞书前由 feishu_api 的 stripLocalMeta 剥离。
 */

/** 行内文本样式（text_element_style 子集；仅设置出现的键）。 */
export interface InlineStyle {
  bold?: boolean
  italic?: boolean
  underline?: boolean
  strikethrough?: boolean
  inline_code?: boolean
  link?: { url: string }
  text_color?: number
  background_color?: number
}

export interface XmlText {
  text: string
}

export interface XmlElement {
  name: string
  attrs: Record<string, string>
  children: XmlNode[]
}

export type XmlNode = XmlElement | XmlText

/** 块描述（add_blocks 简化写法同构；`_` 前缀为本地元数据）。 */
export type BlockDesc = Record<string, unknown>

export interface XmlBlocksResult {
  blocks: BlockDesc[]
  /** 文档标题（`<title>`）；调用方可据此命名新建文档。 */
  title?: string
  /** 降级/忽略说明（不阻断导入，由工具汇总回报给模型）。 */
  notes: string[]
}

/** 无需闭合标签的元素（写成 `<br>` 也合法）。 */
const VOID_ELEMENTS = new Set(["br", "hr", "img", "col", "source"])

/** 颜色名 → 枚举（1~7 = 基础色相；背景的 medium-* 用 +7 偏移 → 8~14，实测平台接受该区间）。 */
const COLOR_ENUM: Record<string, number> = { red: 1, orange: 2, yellow: 3, green: 4, blue: 5, purple: 6, gray: 7 }

const ENTITIES: Record<string, string> = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " " }

/** 实体解码（&lt; &gt; &amp; &quot; &apos; &#NN; &#xNN;）。 */
export function decodeXmlText(s: string): string {
  return s.replace(/&(#[xX]?[0-9a-fA-F]+|[a-zA-Z]+);/g, (raw, body: string) => {
    if (body.startsWith("#")) {
      const cp = body[1] === "x" || body[1] === "X" ? Number.parseInt(body.slice(2), 16) : Number.parseInt(body.slice(1), 10)
      return Number.isFinite(cp) && cp > 0 ? String.fromCodePoint(cp) : raw
    }
    return ENTITIES[body.toLowerCase()] ?? raw
  })
}

/** 属性解析：`name="value"`（引号可选但推荐）；属性名小写归一。 */
function parseAttrs(src: string): Record<string, string> {
  const attrs: Record<string, string> = {}
  const re = /([\w.:-]+)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'>]+))/g
  let m: RegExpExecArray | null
  while ((m = re.exec(src))) attrs[m[1].toLowerCase()] = decodeXmlText(m[2] ?? m[3] ?? m[4] ?? "")
  return attrs
}

/** 转义文本中的标签字符（保留已有实体：&amp;lt; 不会二次转义）。 */
function escapeTags(s: string): string {
  return s.replace(/</g, "&lt;").replace(/>/g, "&gt;")
}

/**
 * `<pre>` 内容保护：代码块内的标签是**字面文本**，不应按标记解析（与 HTML `<pre>` 语义一致），
 * 否则代码示例里的 `<h1>`、`<div>` 会被当块标签消费，内容与结构双失真。
 * 保留可选的 `<code>` 包裹标签，其内部与未包裹时的全部内容一律转义为字面文本。
 */
function protectPreContent(source: string): string {
  return source.replace(/<pre(\s[^>]*)?>([\s\S]*?)<\/pre>/gi, (_m, attrs: string | undefined, inner: string) => {
    const wrapped = /^\s*<code(?:\s[^>]*)?>([\s\S]*?)<\/code>\s*$/.exec(inner)
    const body = wrapped ? `<code>${escapeTags(wrapped[1])}</code>` : escapeTags(inner)
    return `<pre${attrs ?? ""}>${body}</pre>`
  })
}

/**
 * `<whiteboard type="svg">` 内容保护：内联 SVG 源码里的标签（`<svg>`、`<rect>`、`<text>`…）是**图形源码**，
 * 不是文档标签——整体转义为字面文本，否则会被解析器消费掉（内容丢失、甚至生成意外的块）。
 * 经解析后的文本节点实体回还原为原 SVG 源码。
 */
function protectSvgWhiteboards(source: string): string {
  return source.replace(/<whiteboard(\s[^>]*)?>([\s\S]*?)<\/whiteboard>/gi, (m, attrs: string | undefined, inner: string) => {
    const a = attrs ?? ""
    if (!/type\s*=\s*["']svg["']/i.test(a)) return m
    return `<whiteboard${a}>${escapeTags(inner)}</whiteboard>`
  })
}

/**
 * XML → 元素树（宽松解析：注释/声明/CDATA 去除、无引号属性接受、void 元素免闭合）。
 * 标签未闭合或闭合错配时抛错并指出标签名——供模型据此局部修正。
 */
export function parseXml(source: string): XmlElement[] {
  const clean = protectSvgWhiteboards(protectPreContent(source))
    .replace(/<!--[\s\S]*?-->/g, "")
    .replace(/<\?[\s\S]*?\?>/g, "")
    .replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, (_, inner: string) => escapeTags(inner))
  const roots: XmlElement[] = []
  const stack: XmlElement[] = []
  const push = (node: XmlNode): void => {
    const parent = stack[stack.length - 1]
    ;(parent ? parent.children : roots).push(node)
  }
  const tagRe = /<(\/?)([A-Za-z][\w.:-]*)((?:"[^"]*"|'[^']*'|[^>"'])*?)(\/?)>/g
  let last = 0
  let m: RegExpExecArray | null
  while ((m = tagRe.exec(clean))) {
    const raw = clean.slice(last, m.index)
    last = tagRe.lastIndex
    if (raw) push({ text: decodeXmlText(raw) })
    const name = m[2].toLowerCase()
    if (m[1] === "/") {
      const top = stack.pop()
      if (!top || top.name !== name) throw new Error(`XML 标签未正确闭合：</${name}> 与 <${top?.name ?? "文档根"}> 不匹配`)
      continue
    }
    const el: XmlElement = { name, attrs: parseAttrs(m[3]), children: [] }
    push(el)
    if (!VOID_ELEMENTS.has(name) && m[4] !== "/") stack.push(el)
  }
  const tail = clean.slice(last)
  if (tail) push({ text: decodeXmlText(tail) })
  if (stack.length) throw new Error(`XML 标签未闭合：<${stack[stack.length - 1].name}>`)
  return roots
}

/** 颜色属性 → 枚举：`red`/`light-blue`/`medium-gray`；无法识别返回 undefined（并记 note）。 */
function colorEnum(raw: string | undefined, notes: Set<string>, allowMedium: boolean): number | undefined {
  if (!raw) return undefined
  const v = raw.trim().toLowerCase()
  if (v in COLOR_ENUM) return COLOR_ENUM[v]
  const prefixed = /^(light|medium)-(.+)$/.exec(v)
  if (prefixed && prefixed[2] in COLOR_ENUM) {
    const base = COLOR_ENUM[prefixed[2]]
    if (prefixed[1] === "light") return base
    if (allowMedium) return base + 7
    notes.add(`颜色 "medium-${prefixed[2]}" 仅高亮块背景支持（边框/文字色只有基础色相），已按基础色 ${prefixed[2]} 处理`)
    return base
  }
  notes.add(`未识别的颜色 "${raw}"（可用：red/orange/yellow/green/blue/purple/gray 及 light-*/medium-* 前缀），已忽略`)
  return undefined
}

/** 行内节点 → text_run 元素数组（样式键只保留实际出现的）。 */
function inlineElements(nodes: XmlNode[], base: InlineStyle, notes: Set<string>): Record<string, unknown>[] {
  const out: Record<string, unknown>[] = []
  const push = (content: string, style: InlineStyle): void => {
    if (!content) return
    const st = Object.fromEntries(Object.entries(style).filter(([, v]) => v !== undefined && v !== false)) as InlineStyle
    out.push(Object.keys(st).length ? { text_run: { content, text_element_style: st } } : { text_run: { content } })
  }
  const walk = (list: XmlNode[], style: InlineStyle): void => {
    for (const node of list) {
      if (!("name" in node)) {
        push(node.text.replace(/[ \t]*\n[ \t]*/g, "\n"), style)
        continue
      }
      const merged = (extra: InlineStyle): InlineStyle => ({ ...style, ...extra })
      switch (node.name) {
        case "br":
          push("\n", style)
          break
        case "b":
        case "strong":
          walk(node.children, merged({ bold: true }))
          break
        case "em":
        case "i":
          walk(node.children, merged({ italic: true }))
          break
        case "u":
          walk(node.children, merged({ underline: true }))
          break
        case "del":
        case "s":
        case "strike":
          walk(node.children, merged({ strikethrough: true }))
          break
        case "code":
          push(textOf(node), merged({ inline_code: true }))
          break
        case "a": {
          const url = node.attrs.href ?? ""
          if (node.attrs.type === "url-preview") notes.add("链接预览卡片（type=\"url-preview\"）暂不支持，已按普通链接处理")
          if (!url) {
            walk(node.children, style)
            notes.add("链接缺少 href 属性，已按普通文本处理")
            break
          }
          push(textOf(node) || url, merged({ link: { url } }))
          break
        }
        case "span": {
          const extra: InlineStyle = {}
          const tc = colorEnum(node.attrs["text-color"], notes, false)
          const bg = colorEnum(node.attrs["background-color"], notes, true)
          if (tc !== undefined) extra.text_color = tc
          if (bg !== undefined) extra.background_color = bg
          walk(node.children, merged(extra))
          break
        }
        case "cite": {
          // @人：飞书文本元素 mention_user（实测可用）；@文档：mention_doc 实测报 1770038，降级为链接
          const kind = (node.attrs.type ?? "").toLowerCase()
          const userId = node.attrs["user-id"] ?? ""
          if (kind === "user") {
            if (!userId) {
              notes.add('<cite type="user"> 缺少 user-id，已忽略（open_id 可用通讯录接口或群成员列表获取）')
              break
            }
            out.push({ mention_user: { user_id: userId } })
            break
          }
          if (kind === "doc") {
            notes.add('<cite type="doc"> 暂不支持（实测飞书返回 1770038 resource not found）——请改用普通链接 <a href="…">文档标题</a>')
            break
          }
          notes.add(`未识别的 <cite type="${kind || "未指定"}">，已忽略`)
          break
        }
        case "latex":
          // 飞书 equation 块不可经 API 创建（官方创建接口枚举不含 16），按文本落地
          push(textOf(node), style)
          notes.add("行内公式（<latex>）落地为文本：飞书 equation 块不可经 API 创建，需要公式请手动插入公式块")
          break
        case "sub":
        case "sup":
        case "mark":
          walk(node.children, style)
          break
        default:
          walk(node.children, style)
      }
    }
  }
  walk(nodes, base)
  return out
}

/** 元素的纯文本内容（保留原始换行，实体已解码）。 */
function textOf(el: XmlElement): string {
  let s = ""
  for (const child of el.children) s += "name" in child ? textOf(child) : child.text
  return s
}

/** 段落内联内容：折叠排版空白（换行/多空格压成单空格，两端去白）。 */
function inlineText(nodes: XmlNode[], style: InlineStyle, notes: Set<string>): Record<string, unknown>[] {
  const flat: XmlNode[] = nodes.map((n) => ("name" in n ? n : { text: n.text.replace(/\s+/g, " ") }))
  const els = inlineElements(flat, style, notes)
  const first = (els[0] as { text_run?: { content?: string } } | undefined)?.text_run
  if (first && typeof first.content === "string") first.content = first.content.replace(/^\s+/, "")
  const last = (els[els.length - 1] as { text_run?: { content?: string } } | undefined)?.text_run
  if (last && typeof last.content === "string") last.content = last.content.replace(/\s+$/, "")
  // 空文本 run 剔除；@人/@文档等非 text_run 元素（mention_*）必须保留
  return els.filter((e) => {
    const tr = (e as { text_run?: { content?: string } }).text_run
    return tr ? String(tr.content ?? "") !== "" : true
  })
}

/** 表格单元格 / 代码块等场景的行内内容 → Markdown 串（表格走 rows 简化写法，单元格内容经 Markdown 行内语法还原）。 */
function inlineMarkdown(nodes: XmlNode[], notes: Set<string>): string {
  let s = ""
  const walk = (list: XmlNode[], wrap: (s: string) => string): void => {
    for (const node of list) {
      if (!("name" in node)) {
        s += wrap(node.text)
        continue
      }
      switch (node.name) {
        case "br":
          s += "<br>"
          break
        case "b":
        case "strong":
          walk(node.children, (t) => `**${t}**`)
          break
        case "em":
        case "i":
          walk(node.children, (t) => `*${t}*`)
          break
        case "del":
        case "s":
        case "strike":
          walk(node.children, (t) => `~~${t}~~`)
          break
        case "code":
          s += `\`${textOf(node)}\``
          break
        case "a":
          s += node.attrs.href ? `[${textOf(node) || node.attrs.href}](${node.attrs.href})` : textOf(node)
          break
        case "u":
          walk(node.children, (t) => t)
          notes.add("表格单元格内不支持下划线（rows 简化写法），已按普通文本处理")
          break
        default:
          walk(node.children, wrap)
      }
    }
  }
  walk(nodes, (t) => t.replace(/\s+/g, " "))
  return s.trim()
}

/** 文档标题与编号计数（跨块共享的遍历状态）。 */
interface WalkState {
  notes: Set<string>
  counters: number[]
  title?: string
  /** 本地文件读取（`<whiteboard path>` 引用用；未提供时按「读取失败」提示）。 */
  readFile?: (path: string) => string | undefined
}

/** 标题自动编号（`seq="auto"`）：1 / 1.1 / 1.1.1；跳级时把缺失层级补 1 并提示。 */
function headingNumber(state: WalkState, level: number, notes: Set<string>): string {
  let jumped = false
  // 缺失的中间层级（如 h1 直跳 h3）补 1，使编号仍为 1.1.1 而不是 1.1
  for (let i = 0; i < level - 1; i++) if (!state.counters[i]) { state.counters[i] = 1; jumped = true }
  if (jumped) notes.add("标题层级不连续（跳级），自动编号已按缺失层级补 1——建议补上中间层级标题")
  state.counters[level - 1] = (state.counters[level - 1] ?? 0) + 1
  for (let i = level; i < state.counters.length; i++) state.counters[i] = 0
  return state.counters.slice(0, level).join(".")
}

function alignStyle(el: XmlElement, notes: Set<string>): Record<string, unknown> | undefined {
  const raw = (el.attrs.align ?? "").toLowerCase()
  if (!raw) return undefined
  const align = raw === "center" ? 2 : raw === "right" ? 3 : raw === "left" ? 1 : undefined
  if (align === undefined) {
    notes.add(`未识别的对齐值 align="${el.attrs.align}"（可用 left/center/right），已忽略`)
    return undefined
  }
  return { align }
}

/** 带样式的文本块描述（style 落在块字段对象内：飞书结构为 { elements, style }）。 */
function textDesc(blockType: number, field: string, elements: Record<string, unknown>[], style?: Record<string, unknown>): BlockDesc {
  return { block_type: blockType, [field]: { ...(style ? { style } : {}), elements } }
}

/** 块级遍历：XML 元素 → 块描述数组。 */
function walkBlocks(nodes: XmlNode[], out: BlockDesc[], state: WalkState): void {
  const { notes } = state
  const readFile = state.readFile
  for (const node of nodes) {
    if (!("name" in node)) {
      // 块级裸文本（作者忘写 <p>）：非空白则按段落落地，避免内容静默丢失
      if (node.text.trim()) out.push(textDesc(2, "text", inlineText([{ text: node.text }], {}, notes)))
      continue
    }
    const { name } = node
    const heading = /^h([1-9])$/.exec(name)
    if (heading) {
      const level = Number(heading[1])
      let els = inlineText(node.children, {}, notes)
      if (node.attrs.seq === "auto") {
        const num = headingNumber(state, level, notes)
        els = [{ text_run: { content: `${num} ` } }, ...els]
      }
      out.push(textDesc(2 + level, `heading${level}`, els, alignStyle(node, notes)))
      continue
    }
    switch (name) {
      case "title":
        state.title = textOf(node).trim()
        break
      case "p":
        out.push(textDesc(2, "text", inlineText(node.children, {}, notes), alignStyle(node, notes)))
        break
      case "hr":
        out.push({ block_type: 22, divider: {} })
        break
      case "blockquote": {
        // 引用块平台不支持子块：内部块级内容压成软换行文本
        const lines = node.children.map((c) => ("name" in c ? textOf(c).trim() : c.text.trim())).filter(Boolean)
        if (lines.length && node.children.some((c) => "name" in c && /^(p|ul|ol|checkbox)$/.test(c.name))) {
          notes.add("引用块内不支持子块（平台限制），内容已按软换行合并")
        }
        out.push(textDesc(15, "quote", inlineElements([{ text: lines.join("\n") }], {}, notes)))
        break
      }
      case "ul":
      case "ol": {
        const items = node.children.filter((c): c is XmlElement => "name" in c && c.name === "li")
        if (!items.length) {
          notes.add(`<${name}> 内没有 <li> 列表项，已忽略`)
          break
        }
        const ordered = name === "ol"
        const start = ordered && node.attrs.seq && node.attrs.seq !== "auto" ? Number(node.attrs.seq) : undefined
        items.forEach((li, idx) => {
          const els = inlineText(li.children.filter((c) => !("name" in c) || !/^(ul|ol)$/.test(c.name)), {}, notes)
          const field = ordered ? "ordered" : "bullet"
          const desc = textDesc(ordered ? 13 : 12, field, els)
          // 有序列表起始序号：段内首项决定（与 Markdown 一致）
          if (ordered && start !== undefined && idx === 0) desc[field] = { ...(desc[field] as Record<string, unknown>), style: { sequence: start } }
          const nested = li.children.filter((c): c is XmlElement => "name" in c && /^(ul|ol)$/.test(c.name))
          if (nested.length) {
            const kids: BlockDesc[] = []
            walkBlocks(nested, kids, state)
            desc.children = kids
          }
          out.push(desc)
        })
        break
      }
      case "checkbox":
        out.push(textDesc(17, "todo", inlineText(node.children, {}, notes), { done: (node.attrs.done ?? "").toLowerCase() === "true" }))
        break
      case "pre": {
        // 代码内容一律按字面文本处理（protectPreContent 已把内部标签转义，此处不再区分是否有 <code> 包裹）
        const code = node.children.find((c): c is XmlElement => "name" in c && c.name === "code")
        const body = code ? textOf(code) : textOf(node)
        const lang = node.attrs.lang || node.attrs.language || ""
        out.push({ block_type: 14, code: { style: { language: lang || undefined, wrap: node.attrs.wrap !== "false" }, elements: [{ text_run: { content: body.replace(/\n$/, "") } }] } })
        if (node.attrs.caption) out.push(textDesc(2, "text", [{ text_run: { content: node.attrs.caption, text_element_style: { italic: true } } }]))
        break
      }
      case "table":
        pushTable(node, out, notes)
        break
      case "callout": {
        const callout: Record<string, unknown> = {}
        const bg = colorEnum(node.attrs["background-color"], notes, true)
        const border = colorEnum(node.attrs["border-color"], notes, false)
        const text = colorEnum(node.attrs["text-color"], notes, false)
        if (bg !== undefined) callout.background_color = bg
        if (border !== undefined) callout.border_color = border
        if (text !== undefined) callout.text_color = text
        const emoji = node.attrs.emoji?.trim()
        if (emoji) callout.emoji_id = emoji
        const kids: BlockDesc[] = []
        walkBlocks(node.children, kids, state)
        const allowed = kids.filter((k) => [2, 12, 13, 17, 3, 4, 5, 6, 7, 8, 9, 10, 11].includes(Number(k.block_type)))
        if (allowed.length !== kids.length) notes.add("高亮块内仅支持段落/列表/待办（平台限制），其余块已移出高亮块")
        // 至少一个子块（平台强制）：空高亮块补空段落
        const kept: BlockDesc[] = allowed.length ? allowed : [textDesc(2, "text", [{ text_run: { content: "" } }])]
        out.push({ block_type: 19, callout, children: kept })
        out.push(...kids.filter((k) => !allowed.includes(k)))
        break
      }
      case "grid": {
        const cols = node.children.filter((c): c is XmlElement => "name" in c && c.name === "column")
        if (cols.length < 2 || cols.length > 5) {
          notes.add(`分栏 <grid> 需要 2~5 个 <column>（收到 ${cols.length}），已按普通段落落地`)
          walkBlocks(node.children, out, state)
          break
        }
        out.push({
          block_type: 24,
          grid: { column_size: cols.length },
          children: cols.map((col) => {
            const kids: BlockDesc[] = []
            walkBlocks(col.children, kids, state)
            // 平台校验：每列至少一个子块、且必须带 width_ratio（实测空列/缺 width_ratio 报 1770041）
            return { block_type: 25, grid_column: { width_ratio: Number(col.attrs["width-ratio"] ?? 1) || 1 }, children: kids.length ? kids : [textDesc(2, "text", [{ text_run: { content: "" } }])] }
          }),
        })
        break
      }
      case "img": {
        const src = node.attrs.path ? node.attrs.path.replace(/^@/, "") : node.attrs.href || node.attrs.src || ""
        if (!src) {
          notes.add("<img> 缺少 path/href/src，已忽略")
          break
        }
        if (!/^https?:/i.test(src)) notes.add(`图片使用本地路径 ${src}（相对当前工作目录解析）`)
        out.push({ block_type: 27, image: {}, _image_src: src })
        if (node.attrs.width || node.attrs.height) notes.add("图片宽高（width/height）暂不接受设置，图片按原始尺寸插入")
        if (node.attrs.caption) out.push(textDesc(2, "text", [{ text_run: { content: node.attrs.caption, text_element_style: { italic: true } } }]))
        break
      }
      case "whiteboard": {
        const type = (node.attrs.type ?? "").toLowerCase()
        const inline = textOf(node).trim()
        const path = node.attrs.path?.replace(/^@/, "") ?? ""
        const format: Record<string, string> = { mermaid: "mermaid", plantuml: "plantuml", d2: "d2", echarts: "echarts" }
        if (type === "blank") {
          notes.add("空白画板（type=\"blank\"）不支持创建，已忽略")
          break
        }
        if (type === "svg") {
          // SVG：直接作为图片上传（保真显示；飞书画板无法从 SVG 创建，故不可编辑）——实测媒体接受 image/svg+xml
          if (path) out.push({ block_type: 27, image: {}, _image_src: path, _image_name: path.split("/").pop() || "diagram.svg" })
          else out.push({ block_type: 27, image: {}, _svg_inline: inline })
          notes.add("SVG 以图片形式插入（保真显示；画板不可从 SVG 创建，因此不可编辑）")
          if (node.attrs.caption) out.push(textDesc(2, "text", [{ text_run: { content: node.attrs.caption, text_element_style: { italic: true } } }]))
          break
        }
        if (!format[type]) {
          notes.add(`画板类型 "${type || "未指定"}" 暂不支持（可用 mermaid/plantuml/d2/echarts/svg），已忽略`)
          break
        }
        if (!inline && !path) {
          notes.add("<whiteboard> 既无内联内容也无 path，已忽略")
          break
        }
        if (!inline) {
          // 图表源码可由本地文件给出（<whiteboard type="mermaid" path="@./a.mmd">）
          const content = path ? readFile?.(path) : undefined
          if (path && content === undefined) {
            notes.add(`图表文件读取失败（${path}）——路径不存在或当前环境未提供文件读取，已忽略该图`)
            break
          }
          out.push({ block_type: 27, image: {}, _diagram_format: format[type], _diagram_code: (content ?? "").trim() })
        } else {
          out.push({ block_type: 27, image: {}, _diagram_format: format[type], _diagram_code: inline })
        }
        if (node.attrs.caption) out.push(textDesc(2, "text", [{ text_run: { content: node.attrs.caption, text_element_style: { italic: true } } }]))
        break
      }
      case "bookmark":
      case "button":
      case "time":
      case "task":
      case "sheet":
      case "source":
      case "html5-block":
      case "okr":
        notes.add(`标签 <${name}> 暂不支持，已忽略其内容（可用文本+链接替代）`)
        break
      case "latex":
        out.push(textDesc(2, "text", [{ text_run: { content: textOf(node) } }]))
        notes.add("独立公式（<latex>）落地为文本：飞书 equation 块不可经 API 创建")
        break
      default: {
        // 未知容器（div/section 等）透明穿透；未知行内标签按段落落地
        const hasBlockChild = node.children.some((c) => "name" in c && /^(p|h[1-9]|ul|ol|li|pre|table|callout|grid|blockquote|checkbox|hr|img|whiteboard)$/.test(c.name))
        if (hasBlockChild) {
          walkBlocks(node.children, out, state)
        } else if (textOf(node).trim()) {
          notes.add(`未知标签 <${name}> 已按段落处理`)
          out.push(textDesc(2, "text", inlineText(node.children, {}, notes)))
        }
      }
    }
  }
}

/** 表格：`<colgroup><col width>` → 列宽；`<thead>` → 表头行；单元格内容走 rows 简化写法（列宽自适应）。 */
function pushTable(node: XmlElement, out: BlockDesc[], notes: Set<string>): void {
  const rows: string[][] = []
  let hasHead = false
  const walkRows = (nodes: XmlNode[], isHead: boolean): void => {
    for (const n of nodes) {
      if (!("name" in n)) continue
      if (n.name === "tr") {
        const cells = n.children.filter((c): c is XmlElement => "name" in c && (c.name === "td" || c.name === "th"))
        if (!cells.length) continue
        // 表头行判定：来自 <thead> 或单元格用 <th>
        if (isHead || cells.some((c) => c.name === "th")) hasHead = true
        rows.push(cells.map((c) => inlineMarkdown(c.children, notes)))
        continue
      }
      if (n.name === "thead" || n.name === "tbody" || n.name === "tfoot") walkRows(n.children, n.name === "thead")
    }
  }
  walkRows(node.children, false)
  if (!rows.length) {
    notes.add("<table> 内没有可识别的行（<tr><td>），已忽略")
    return
  }
  const columnSize = Math.max(...rows.map((r) => r.length))
  const padded = rows.map((r) => [...r, ...Array.from({ length: columnSize - r.length }, () => "")])
  const table: Record<string, unknown> = { rows: padded, header_row: hasHead }
  const widths = parseColWidths(node, columnSize, notes)
  if (widths) table.column_width = widths
  out.push({ block_type: 31, table })
}

/** `<colgroup><col width="180" span="2"/>` → 列宽数组（长度须等于列数，否则忽略并提示）。 */
function parseColWidths(table: XmlElement, columnSize: number, notes: Set<string>): number[] | undefined {
  const group = table.children.find((c): c is XmlElement => "name" in c && c.name === "colgroup")
  if (!group) return undefined
  const widths: number[] = []
  for (const col of group.children) {
    if (!("name" in col) || col.name !== "col") continue
    const w = Number(col.attrs.width ?? 0)
    const span = Math.max(1, Number(col.attrs.span ?? 1) || 1)
    if (!Number.isFinite(w) || w <= 0) continue
    for (let i = 0; i < span; i++) widths.push(Math.round(w))
  }
  if (!widths.length) return undefined
  if (widths.length !== columnSize) {
    notes.add(`<colgroup> 列宽数量（${widths.length}）与表格列数（${columnSize}）不一致，已改用内容自适应列宽`)
    return undefined
  }
  return widths
}

/**
 * XML 排版语法 → 块描述数组。
 * `title` 选项用于新建文档时的标题去重（`<title>` 优先，其次用该选项；首行 H1 与标题相同则摘掉避免层级重复）。
 */
export function xmlToBlocks(xml: string, opts: { title?: string; readFile?: (path: string) => string | undefined } = {}): XmlBlocksResult {
  const notes = new Set<string>()
  const state: WalkState = { notes, counters: [], readFile: opts.readFile }
  if (!/<[A-Za-z]/.test(xml)) throw new Error("XML 内容为空或缺少标签——整篇文档请以 <title> 或 <h1> 开头")
  const roots = parseXml(xml)
  const blocks: BlockDesc[] = []
  walkBlocks(roots, blocks, state)
  const title = state.title?.trim() || opts.title?.trim()
  // 标题去重：文档已有 title 字段时，首个 H1 若与标题字符串相同则摘掉（避免标题层级重复）
  if (title) {
    const first = blocks.find((b) => Number(b.block_type) === 3) as { heading1?: { elements?: Array<{ text_run?: { content?: string } }> } } | undefined
    const text = (first?.heading1?.elements ?? []).map((e) => e.text_run?.content ?? "").join("").trim()
    if (text && text === title) blocks.splice(blocks.indexOf(first as BlockDesc), 1)
  }
  if (!blocks.length) throw new Error("XML 中没有可导入的块（是否只写了 <title>？）")
  return { blocks, title, notes: [...notes] }
}
