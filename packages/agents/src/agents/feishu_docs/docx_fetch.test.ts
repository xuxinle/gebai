import { describe, expect, test } from "bun:test"
import {
  blockMatches,
  blockToXml,
  buildScopeCommon,
  escapeXmlText,
  inlinePlainText,
  inlineToXml,
  keywordScope,
  outlineScope,
  rangeScope,
  sectionScope,
  seqToXml,
  type FetchXmlOptions,
} from "./docx_fetch"
import { xmlToBlocks } from "./docx_xml"

/* ================= 测试素材构造 ================= */

function el(content: string, style?: Record<string, unknown>): Record<string, unknown> {
  return style ? { text_run: { content, text_element_style: style } } : { text_run: { content } }
}

function textBlock(id: string, text: string, parent: string, type = 2, extra: Record<string, unknown> = {}): Record<string, unknown> {
  const field = { elements: [el(text)], ...extra }
  const fieldName = type === 2 ? "text" : type >= 3 && type <= 11 ? `heading${type - 2}` : type === 12 ? "bullet" : type === 13 ? "ordered" : "text"
  return { block_id: id, block_type: type, parent_id: parent, [fieldName]: field, ...(("children" in {}) ? {} : {}) }
}

function makeDoc(blocks: Array<Record<string, unknown>>): Map<string, Record<string, unknown>> {
  const byId = new Map<string, Record<string, unknown>>()
  for (const b of blocks) byId.set(String(b.block_id), b)
  // page 根块：children 按传入顺序
  byId.set("root", { block_id: "root", block_type: 1, parent_id: "", children: blocks.map((b) => String(b.block_id)) })
  return byId
}

/* ================= 行内序列化 ================= */

describe("inlineToXml", () => {
  test("纯文本无样式", () => {
    expect(inlineToXml([el("你好")])).toBe("你好")
  })
  test("加粗+斜体嵌套", () => {
    expect(inlineToXml([{ text_run: { content: "重点", text_element_style: { bold: true, italic: true } } }])).toBe("<b><em>重点</em></b>")
  })
  test("链接", () => {
    expect(inlineToXml([{ text_run: { content: "飞书", text_element_style: { link: { url: "https://feishu.cn" } } } }])).toBe('<a href="https://feishu.cn">飞书</a>')
  })
  test("行内代码", () => {
    expect(inlineToXml([{ text_run: { content: "npm install", text_element_style: { inline_code: true } } }])).toBe("<code>npm install</code>")
  })
  test("颜色", () => {
    expect(inlineToXml([{ text_run: { content: "警告", text_element_style: { text_color: 5 } } }])).toBe('<span text-color="blue">警告</span>')
    expect(inlineToXml([{ text_run: { content: "高亮", text_element_style: { background_color: 12 } } }])).toBe('<span background-color="medium-blue">高亮</span>')
  })
  test("@人 mention", () => {
    expect(inlineToXml([{ mention_user: { user_id: "ou_123" } }])).toBe('<cite type="user" user-id="ou_123"/>')
  })
  test("换行拆 <br/>", () => {
    expect(inlineToXml([el("第一行\n第二行")])).toBe("第一行<br/>第二行")
  })
  test("转义 < > &", () => {
    expect(inlineToXml([el("a < b & c > d")])).toBe("a &lt; b &amp; c &gt; d")
  })
})

describe("escapeXmlText / inlinePlainText", () => {
  test("只转义文本", () => {
    expect(escapeXmlText("<p>A & B</p>")).toBe("&lt;p&gt;A &amp; B&lt;/p&gt;")
  })
  test("mention 文本化", () => {
    expect(inlinePlainText([el("hi "), { mention_user: { user_id: "ou_x" } }])).toBe("hi @用户")
  })
})

/* ================= 块序列化 ================= */

describe("blockToXml", () => {
  test("段落/标题带 id", () => {
    const byId = makeDoc([textBlock("b1", "概述", "root", 3)])
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    expect(blockToXml("b1", opts)).toBe('<h1 id="b1">概述</h1>')
  })
  test("simple 模式去样式去 id", () => {
    const byId = makeDoc([textBlock("b1", "概述", "root", 3)])
    const opts: FetchXmlOptions = { byId, notes: new Set() }
    expect(blockToXml("b1", opts)).toBe("<h1>概述</h1>")
  })
  test("todo done", () => {
    const byId = new Map([["b1", { block_id: "b1", block_type: 17, parent_id: "root", todo: { style: { done: true }, elements: [el("任务")] } }]])
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    expect(blockToXml("b1", opts)).toBe('<checkbox done="true" id="b1">任务</checkbox>')
  })
  test("代码块 lang + 转义", () => {
    const byId = new Map([["b1", { block_id: "b1", block_type: 14, parent_id: "root", code: { style: { language: 49 }, elements: [el('fmt.Println("<hi>")]')] } }]])
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    expect(blockToXml("b1", opts)).toBe('<pre lang="Python" id="b1"><code>fmt.Println("&lt;hi&gt;")]</code></pre>')
  })
  test("callout 配色与子块", () => {
    const byId = new Map<string, Record<string, unknown>>([
      ["c1", { block_id: "c1", block_type: 19, parent_id: "root", callout: { background_color: 5, border_color: 5 }, children: ["c1p1"] }],
      ["c1p1", { block_id: "c1p1", block_type: 2, parent_id: "c1", text: { elements: [el("提示内容")] } }],
    ])
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    const xml = blockToXml("c1", opts)
    expect(xml).toBe('<callout background-color="light-blue" border-color="blue" id="c1"><p id="c1p1">提示内容</p></callout>')
  })
  test("grid 分栏", () => {
    const byId = new Map<string, Record<string, unknown>>([
      ["g1", { block_id: "g1", block_type: 24, parent_id: "root", grid: { column_size: 2 }, children: ["gc1", "gc2"] }],
      ["gc1", { block_id: "gc1", block_type: 25, parent_id: "g1", grid_column: { width_ratio: 1 }, children: ["gc1p"] }],
      ["gc2", { block_id: "gc2", block_type: 25, parent_id: "g1", grid_column: { width_ratio: 1 }, children: ["gc2p"] }],
      ["gc1p", { block_id: "gc1p", block_type: 2, parent_id: "gc1", text: { elements: [el("左")] } }],
      ["gc2p", { block_id: "gc2p", block_type: 2, parent_id: "gc2", text: { elements: [el("右")] } }],
    ])
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    expect(blockToXml("g1", opts)).toBe(
      '<grid id="g1"><column width-ratio="1" id="gc1"><p id="gc1p">左</p></column>\n<column width-ratio="1" id="gc2"><p id="gc2p">右</p></column></grid>',
    )
  })
  test("表格：colgroup 列宽 + thead", () => {
    const mk = (id: string, parent: string, kids: string[]): [string, Record<string, unknown>] => [id, { block_id: id, block_type: 32, parent_id: parent, children: kids }]
    const p = (id: string, parent: string, text: string): [string, Record<string, unknown>] => [id, { block_id: id, block_type: 2, parent_id: parent, text: { elements: [el(text)] } }]
    const byId = new Map<string, Record<string, unknown>>([
      ["t1", { block_id: "t1", block_type: 31, parent_id: "root", table: { property: { column_size: 2, row_size: 2, column_width: [180, 550], header_row: true } }, children: ["tc00", "tc01", "tc10", "tc11"] } as Record<string, unknown>],
      mk("tc00", "t1", ["t00p"]), mk("tc01", "t1", ["t01p"]), mk("tc10", "t1", ["t10p"]), mk("tc11", "t1", ["t11p"]),
      p("t00p", "tc00", "列A"), p("t01p", "tc01", "列B"), p("t10p", "tc10", "a1"), p("t11p", "tc11", "b1"),
    ])
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    const xml = blockToXml("t1", opts)
    expect(xml).toBe(
      '<table id="t1">\n<colgroup><col width="180"/><col width="550"/></colgroup>\n<thead><tr><th id="tc00">列A</th><th id="tc01">列B</th></tr></thead>\n<tr><td id="tc10">a1</td><td id="tc11">b1</td></tr>\n</table>',
    )
  })
  test("图片 token 与宽高", () => {
    const byId = new Map([["i1", { block_id: "i1", block_type: 27, parent_id: "root", image: { token: "boxcnABC", width: 800, height: 600 } }]])
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    expect(blockToXml("i1", opts)).toBe('<img src="boxcnABC" width="800" height="600" id="i1"/>')
  })
  test("未知块类型给占位不静默丢失", () => {
    const byId = new Map([["x1", { block_id: "x1", block_type: 46, parent_id: "root" }]])
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    expect(blockToXml("x1", opts)).toBe("<!-- block_type=46 id=x1 不支持序列化 -->")
  })
  test("嵌套列表", () => {
    const byId = new Map<string, Record<string, unknown>>([
      ["l1", { block_id: "l1", block_type: 12, parent_id: "root", bullet: { elements: [el("项一")] }, children: ["l1a"] }],
      ["l1a", { block_id: "l1a", block_type: 12, parent_id: "l1", bullet: { elements: [el("子项")] } }],
    ])
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    expect(blockToXml("l1", opts)).toBe('<ul id="l1"><li>项一<ul id="l1a"><li>子项</li></ul></li></ul>')
  })
  test("seqToXml：连续同类列表项聚合成一个 ul/ol（round-trip 对称）", () => {
    const byId = new Map<string, Record<string, unknown>>([
      ["o1", { block_id: "o1", block_type: 13, parent_id: "root", ordered: { elements: [el("第一步")], style: { sequence: 3 } } }],
      ["o2", { block_id: "o2", block_type: 13, parent_id: "root", ordered: { elements: [el("第二步")] } }],
      ["b1", { block_id: "b1", block_type: 12, parent_id: "root", bullet: { elements: [el("项一")] } }],
      ["b2", { block_id: "b2", block_type: 12, parent_id: "root", bullet: { elements: [el("项二")] } }],
      ["p1", { block_id: "p1", block_type: 2, parent_id: "root", text: { elements: [el("段落")] } }],
    ])
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    const xml = seqToXml(["o1", "o2", "p1", "b1", "b2"], opts, 0)
    expect(xml).toBe('<ol seq="3" id="o1"><li>第一步</li><li id="o2">第二步</li></ol>\n<p id="p1">段落</p>\n<ul id="b1"><li>项一</li><li id="b2">项二</li></ul>')
    // 再导入：两个列表段分别得到 2 项，不膨胀
    const again = xmlToBlocks(xml.replace(/ id="[^"]*"/g, "").replace(/ seq="3"/g, ""))
    expect(again.blocks.filter((b) => Number(b.block_type) === 13)).toHaveLength(2)
    expect(again.blocks.filter((b) => Number(b.block_type) === 12)).toHaveLength(2)
  })
  test("seqToXml：单块调用不聚合（blockToXml 直调仍为单块语义）", () => {
    const byId = new Map([["o1", { block_id: "o1", block_type: 13, parent_id: "root", ordered: { elements: [el("唯一步骤")] } }]])
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    expect(blockToXml("o1", opts)).toBe('<ol id="o1"><li>唯一步骤</li></ol>')
  })
})

/* ================= round-trip：xmlToBlocks → blocksToXml 结构等价 ================= */

describe("round-trip XML ↔ blocks", () => {
  const src = [
    '<title>测试文档</title>',
    '<h1 seq="auto">背景</h1>',
    "<p>这是<b>加粗</b>与<code>code</code>的段落</p>",
    '<ul><li>项A<ul><li>子项</li></ul></li><li>项B</li></ul>',
    '<callout emoji="bulb" background-color="light-blue"><p>提示</p></callout>',
    '<table><colgroup><col width="200"/><col width="530"/></colgroup><thead><tr><th>名</th><th>值</th></tr></thead><tbody><tr><td>a</td><td>1</td></tr></tbody></table>',
    '<pre lang="go" caption=""><code>x := 1</code></pre>',
  ].join("\n")

  test("导入再序列化：文本与结构保持", () => {
    const parsed = xmlToBlocks(src)
    // 构造 byId（模拟飞书返回：块带 children）
    const byId = new Map<string, Record<string, unknown>>()
    const flat: Array<Record<string, unknown>> = []
    let seq = 0
    const walk = (descs: Array<Record<string, unknown>>, parentId: string): string[] => {
      const ids: string[] = []
      for (const d of descs) {
        const id = `b${++seq}`
        const t = Number(d.block_type)
        const kids = Array.isArray(d.children) ? walk(d.children as Array<Record<string, unknown>>, id) : []
        const block: Record<string, unknown> = { block_id: id, block_type: t, parent_id: parentId }
        const fieldName = t === 31 ? "table" : t === 19 ? "callout" : t === 14 ? "code" : t === 17 ? "todo" : t === 12 ? "bullet" : t === 13 ? "ordered" : t === 2 ? "text" : t >= 3 && t <= 11 ? `heading${t - 2}` : undefined
        if (fieldName && (d as Record<string, unknown>)[fieldName] !== undefined) block[fieldName] = (d as Record<string, unknown>)[fieldName]
        if (kids.length) block.children = kids
        byId.set(id, block)
        flat.push(block)
        ids.push(id)
      }
      return ids
    }
    const topIds = walk(parsed.blocks, "root")
    byId.set("root", { block_id: "root", block_type: 1, parent_id: "", children: topIds })
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    const xml = topIds.map((id) => blockToXml(id, opts)).filter(Boolean).join("\n")
    // 断言关键结构回归
    expect(xml).toContain(">1 背景</h1>")
    expect(xml).toContain("这是<b>加粗</b>与<code>code</code>的段落")
    expect(xml).toContain("<li>项A<ul id=\"b4\"><li>子项</li></ul></li>")
    expect(xml).toContain('background-color="light-blue"')
    expect(xml).toContain('<col width="200"/>')
    expect(xml).toContain("<th>名</th>")
    expect(xml).toContain("<pre lang=\"go\" id=\"b9\"><code>x := 1</code></pre>")
    // 再次导入 round-trip 后块数不膨胀
    const again = xmlToBlocks(xml.replace(/ id="b\d+"/g, ""))
    expect(again.blocks.length).toBe(parsed.blocks.length)
  })
})

/* ================= scope ================= */

function sampleDoc(): { common: ReturnType<typeof buildScopeCommon>; byId: Map<string, Record<string, unknown>> } {
  const byId = new Map<string, Record<string, unknown>>([
    ["h1a", { block_id: "h1a", block_type: 3, parent_id: "root", heading1: { elements: [el("一、总述")] } }],
    ["p1", { block_id: "p1", block_type: 2, parent_id: "root", text: { elements: [el("总述内容，含 部署 关键词")] } }],
    ["h2a", { block_id: "h2a", block_type: 4, parent_id: "root", heading2: { elements: [el("1.1 部署步骤")] } }],
    ["p2", { block_id: "p2", block_type: 2, parent_id: "root", text: { elements: [el("先安装依赖再执行部署")] } }],
    ["h1b", { block_id: "h1b", block_type: 3, parent_id: "root", heading1: { elements: [el("二、总结")] } }],
    ["p3", { block_id: "p3", block_type: 2, parent_id: "root", text: { elements: [el("收尾")] } }],
  ])
  byId.set("root", { block_id: "root", block_type: 1, parent_id: "", children: ["h1a", "p1", "h2a", "p2", "h1b", "p3"] })
  return { common: buildScopeCommon(byId, "root"), byId }
}

describe("scopes", () => {
  test("outline：层级缩进与节内块数", () => {
    const { common } = sampleDoc()
    const r = outlineScope(common, 3)
    expect(r.xml).toContain('<h1 id="h1a">一、总述</h1> <!-- 节内 3 个顶层块 -->')
    expect(r.xml).toContain('  <h2 id="h2a">1.1 部署步骤</h2> <!-- 节内 1 个顶层块 -->')
    expect(r.topBlockIds).toEqual(["h1a", "h2a", "h1b"])
  })
  test("section：h2 到下一个同级前", () => {
    const { common, byId } = sampleDoc()
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    const r = sectionScope(common, "h2a", opts)
    expect(r.topBlockIds).toEqual(["h2a", "p2"])
  })
  test("section：h1 覆盖到下一个 h1 前", () => {
    const { common, byId } = sampleDoc()
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    const r = sectionScope(common, "h1a", opts)
    expect(r.topBlockIds).toEqual(["h1a", "p1", "h2a", "p2"])
  })
  test("range：区间含端点", () => {
    const { common, byId } = sampleDoc()
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    const r = rangeScope(common, "p1", "p2", opts)
    expect(r.topBlockIds).toEqual(["p1", "h2a", "p2"])
  })
  test("range：容器内块归属顶层祖先区间", () => {
    const byId = new Map<string, Record<string, unknown>>([
      ["c1", { block_id: "c1", block_type: 19, parent_id: "root", callout: {}, children: ["c1p"] }],
      ["c1p", { block_id: "c1p", block_type: 2, parent_id: "c1", text: { elements: [el("callout 内")] } }],
      ["p9", { block_id: "p9", block_type: 2, parent_id: "root", text: { elements: [el("尾")] } }],
    ])
    byId.set("root", { block_id: "root", block_type: 1, parent_id: "", children: ["c1", "p9"] })
    const common = buildScopeCommon(byId, "root")
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    const r = rangeScope(common, "c1p", undefined, opts)
    expect(r.topBlockIds).toEqual(["c1", "p9"])
  })
  test("keyword：命中 + 上下文 excerpt", () => {
    const { common, byId } = sampleDoc()
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    const r = keywordScope(common, "部署", 1, 0, opts)
    expect(r.topBlockIds).toContain("p1")
    expect(r.topBlockIds).toContain("h2a")
    expect(r.xml).toContain("excerpt")
    expect(r.xml).toContain('top-block-id="h1a"')
  })
  test("keyword：多词 OR", () => {
    const { common, byId } = sampleDoc()
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    const r = keywordScope(common, "收尾|总述", 0, 0, opts)
    expect(r.topBlockIds).toEqual(["h1a", "p1", "p3"])
  })
  test("keyword 未命中", () => {
    const { common, byId } = sampleDoc()
    const opts: FetchXmlOptions = { byId, ids: true, styles: true, notes: new Set() }
    const r = keywordScope(common, "不存在", 0, 0, opts)
    expect(r.xml).toBe("")
    expect(r.notes[0]).toContain("未命中")
  })
  test("blockMatches：容器内子块命中", () => {
    const byId = new Map<string, Record<string, unknown>>([
      ["c1", { block_id: "c1", block_type: 19, parent_id: "root", callout: {}, children: ["c1p"] }],
      ["c1p", { block_id: "c1p", block_type: 2, parent_id: "c1", text: { elements: [el("藏在 callout 里")] } }],
    ])
    expect(blockMatches("c1", byId, ["藏在"])).toBe(true)
    expect(blockMatches("c1", byId, ["没有的词"])).toBe(false)
  })
})
