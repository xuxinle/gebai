import { describe, expect, test } from "bun:test"
import { decodeXmlText, parseXml, xmlToBlocks, type BlockDesc } from "./docx_xml"

/** 取块字段（`{block_type:3, heading1:{...}}` → heading1 对象）。 */
const fieldOf = (b: BlockDesc, key: string): Record<string, unknown> => b[key] as Record<string, unknown>
/** 块字段的文本内容拼接。 */
const textOfBlock = (b: BlockDesc, key: string): string =>
  ((fieldOf(b, key).elements as Array<{ text_run?: { content?: string } }>) ?? []).map((e) => e.text_run?.content ?? "").join("")

describe("XML 解析器", () => {
  test("嵌套标签与文本节点", () => {
    const roots = parseXml("<p>前<b>粗</b>后</p>")
    expect(roots).toHaveLength(1)
    expect(roots[0].name).toBe("p")
    expect(roots[0].children).toHaveLength(3)
  })

  test("void 元素无需闭合（<br> 与 <br/> 等价）", () => {
    expect(parseXml("<p>a<br>b<br/>c</p>")[0].children.filter((c) => "name" in c)).toHaveLength(2)
  })

  test("实体解码（含数字实体）", () => {
    expect(decodeXmlText("A &amp; B &lt;tag&gt; &quot;q&quot; &#65;")).toBe('A & B <tag> "q" A')
  })

  test("属性：双引号/单引号/无引号均可，属性名小写归一", () => {
    const [el] = parseXml('<IMG HREF="http://x/y.png" caption=\'题注\' width=320/>')
    expect(el.attrs).toEqual({ href: "http://x/y.png", caption: "题注", width: "320" })
  })

  test("注释与 XML 声明被剔除", () => {
    const roots = parseXml('<?xml version="1.0"?><!-- 注释 --><p>正文</p>')
    expect(roots).toHaveLength(1)
    expect(roots[0].name).toBe("p")
    expect(xmlToBlocks('<?xml version="1.0"?><!-- 说明 --><p>正文</p>').blocks).toHaveLength(1)
  })

  test("标签未闭合 / 闭合错配时报错并指出标签", () => {
    expect(() => parseXml("<p>正文")).toThrow(/未闭合.*p/)
    expect(() => parseXml("<p>a</b>")).toThrow(/未正确闭合/)
  })
})

describe("XML → 块描述：结构与标题", () => {
  test("title 提取 + 首个同名 H1 去重", () => {
    const r = xmlToBlocks("<title>季度复盘</title>\n<h1>季度复盘</h1>\n<p>正文</p>")
    expect(r.title).toBe("季度复盘")
    expect(r.blocks.map((b) => b.block_type)).toEqual([2]) // H1 与标题重复已摘掉
  })

  test("标题层级与 seq=auto 自动编号（1 / 1.1 / 1.2 / 2）", () => {
    const r = xmlToBlocks('<h1 seq="auto">甲</h1><h2 seq="auto">乙</h2><h2 seq="auto">丙</h2><h1 seq="auto">丁</h1>')
    expect(r.blocks.map((b) => textOfBlock(b, `heading${Number(b.block_type) - 2}`))).toEqual(["1 甲", "1.1 乙", "1.2 丙", "2 丁"])
  })

  test("跳级时补 1 并提示", () => {
    const r = xmlToBlocks('<h1 seq="auto">一</h1><h3 seq="auto">三</h3>')
    expect(textOfBlock(r.blocks[1], "heading3")).toBe("1.1.1 三")
    expect(r.notes.join()).toContain("标题层级不连续")
  })

  test("段落对齐 align 落在块字段的 style 内", () => {
    const r = xmlToBlocks('<p align="center">居中</p>')
    expect(fieldOf(r.blocks[0], "text").style).toEqual({ align: 2 })
  })
})

describe("XML → 块描述：行内样式与列表", () => {
  test("行内样式：加粗/斜体/下划线/删除线/行内代码/链接", () => {
    const r = xmlToBlocks('<p><b>粗</b><em>斜</em><u>下划</u><del>删</del><code>code</code><a href="https://x">链接</a></p>')
    const els = fieldOf(r.blocks[0], "text").elements as Array<{ text_run: { content: string; text_element_style?: Record<string, unknown> } }>
    expect(els.map((e) => e.text_run.content)).toEqual(["粗", "斜", "下划", "删", "code", "链接"])
    expect(els[0].text_run.text_element_style).toEqual({ bold: true })
    expect(els[2].text_run.text_element_style).toEqual({ underline: true })
    expect(els[5].text_run.text_element_style).toEqual({ link: { url: "https://x" } })
  })

  test("span 的文字色与背景色映射为枚举（基础色相 / light / medium）", () => {
    const r = xmlToBlocks('<p><span text-color="red">红</span><span background-color="light-blue">浅蓝</span><span background-color="medium-gray">中灰</span></p>')
    const els = fieldOf(r.blocks[0], "text").elements as Array<{ text_run: { text_element_style?: Record<string, unknown> } }>
    expect(els[0].text_run.text_element_style).toEqual({ text_color: 1 })
    expect(els[1].text_run.text_element_style).toEqual({ background_color: 5 })
    expect(els[2].text_run.text_element_style).toEqual({ background_color: 14 })
  })

  test("无序列表与嵌套列表（子项挂在父项 children）", () => {
    const r = xmlToBlocks("<ul><li>父项<ul><li>子项</li></ul></li><li>第二项</li></ul>")
    expect(r.blocks).toHaveLength(2)
    expect(textOfBlock(r.blocks[0], "bullet")).toBe("父项")
    const kids = r.blocks[0].children as BlockDesc[]
    expect(kids).toHaveLength(1)
    expect(textOfBlock(kids[0], "bullet")).toBe("子项")
  })

  test("有序列表起始序号（seq=3）与默认自动编号", () => {
    const r = xmlToBlocks('<ol seq="3"><li>三</li><li>四</li></ol>')
    expect(fieldOf(r.blocks[0], "ordered").style).toEqual({ sequence: 3 })
    expect(fieldOf(r.blocks[1], "ordered").style).toBeUndefined()
  })

  test("待办 checkbox 的 done 映射到 todo.style.done", () => {
    const r = xmlToBlocks('<checkbox done="true">已完成</checkbox><checkbox>未完成</checkbox>')
    expect(fieldOf(r.blocks[0], "todo")).toMatchObject({ style: { done: true } })
    expect(fieldOf(r.blocks[1], "todo")).toMatchObject({ style: { done: false } })
  })

  test("引用与分割线", () => {
    const r = xmlToBlocks("<blockquote>引用内容</blockquote><hr/>")
    expect(r.blocks[0].block_type).toBe(15)
    expect(r.blocks[1]).toMatchObject({ block_type: 22, divider: {} })
  })

  test("代码块：lang 与 caption（题注落为斜体段落）", () => {
    const r = xmlToBlocks('<pre lang="go" caption="示例"><code>fmt.Println(&quot;hi&quot;)</code></pre>')
    expect((fieldOf(r.blocks[0], "code") as { style: Record<string, unknown> }).style).toMatchObject({ language: "go", wrap: true })
    expect(textOfBlock(r.blocks[1], "text")).toBe("示例")
  })

  test("代码块内标签按字面文本保留（pre 内不解析标记）", () => {
    const xml = '<pre lang="xml" caption="写法示例">\n<h1 seq="auto">结论</h1>\n<callout emoji="bulb"><p>提示</p></callout>\n</pre>'
    const r = xmlToBlocks(xml)
    const code = String(((fieldOf(r.blocks[0], "code") as { elements: Array<{ text_run?: { content?: string } }> }).elements[0].text_run?.content) ?? "")
    expect(code).toContain('<h1 seq="auto">结论</h1>')
    expect(code).toContain('<callout emoji="bulb">')
    // 内层标签不得变成真块（只应有代码块 + 题注段落）
    expect(r.blocks.map((b) => b.block_type)).toEqual([14, 2])
    expect(textOfBlock(r.blocks[1], "text")).toBe("写法示例")
    // 带 <code> 包裹的写法等价（内容同样保留标签字面量）
    const wrapped = xmlToBlocks('<pre lang="xml"><code><img href="x"/></code></pre>')
    const wrappedCode = String(((fieldOf(wrapped.blocks[0], "code") as { elements: Array<{ text_run?: { content?: string } }> }).elements[0].text_run?.content) ?? "")
    expect(wrappedCode).toContain('<img href="x"/>')
  })
})

describe("XML → 块描述：表格 / 高亮块 / 分栏 / 媒体", () => {
  test("表格：thead 判表头、colgroup 定列宽、单元格行内语法还原", () => {
    const xml = '<table><colgroup><col width="180"/><col width="550"/></colgroup><thead><tr><th>列A</th><th>列B</th></tr></thead><tbody><tr><td>**粗**</td><td>普通</td></tr></tbody></table>'
    const r = xmlToBlocks(xml)
    const table = fieldOf(r.blocks[0], "table")
    expect(table.rows).toEqual([["列A", "列B"], ["**粗**", "普通"]])
    expect(table.header_row).toBe(true)
    expect(table.column_width).toEqual([180, 550])
  })

  test("colgroup 数量与列数不符时回落自适应并提示", () => {
    const r = xmlToBlocks('<table><colgroup><col width="180"/></colgroup><tr><th>a</th><th>b</th></tr></table>')
    expect(fieldOf(r.blocks[0], "table").column_width).toBeUndefined()
    expect(r.notes.join()).toContain("列宽数量")
  })

  test("高亮块：配色（light/medium 偏移）、emoji 与子块", () => {
    const r = xmlToBlocks('<callout emoji="bulb" background-color="medium-red" border-color="red" text-color="blue"><p>提示首段</p><ul><li>要点</li></ul></callout>')
    expect(r.blocks[0].callout).toEqual({ background_color: 8, border_color: 1, text_color: 5, emoji_id: "bulb" })
    expect((r.blocks[0].children as BlockDesc[]).map((b) => b.block_type)).toEqual([2, 12])
  })

  test("高亮块空内容补空段落（平台强制至少一个子块）", () => {
    const r = xmlToBlocks('<callout emoji="bulb"></callout>')
    expect((r.blocks[0].children as BlockDesc[])).toHaveLength(1)
  })

  test("高亮块内不支持表格：移出到高亮块之后并提示", () => {
    const r = xmlToBlocks('<callout emoji="bulb"><p>提示</p><table><tr><th>a</th></tr></table></callout>')
    expect(Number(r.blocks[1].block_type)).toBe(31)
    expect(r.notes.join()).toContain("高亮块内仅支持")
  })

  test("分栏：column_size 与 width_ratio；空列补空段落（实测缺一即报 1770041）", () => {
    const r = xmlToBlocks('<grid><column width-ratio="0.4"><p>左栏</p></column><column width-ratio="0.6"></column></grid>')
    expect(fieldOf(r.blocks[0], "grid")).toEqual({ column_size: 2 })
    const cols = r.blocks[0].children as BlockDesc[]
    expect(fieldOf(cols[0], "grid_column")).toEqual({ width_ratio: 0.4 })
    expect(cols[0].children as BlockDesc[]).toHaveLength(1)
    expect(cols[1].children as BlockDesc[]).toHaveLength(1)
  })

  test("分栏列数越界（1 列）时按普通段落落地", () => {
    const r = xmlToBlocks("<grid><column><p>单栏</p></column></grid>")
    expect(r.blocks[0].block_type).toBe(2)
    expect(r.notes.join()).toContain("需要 2~5 个")
  })

  test("图片：本地路径占位 + 题注；网络图不提示本地路径", () => {
    const r = xmlToBlocks('<img path="@./shot.png" caption="界面截图"/><img href="https://x/a.png"/>')
    expect(r.blocks[0]).toMatchObject({ block_type: 27, _image_src: "./shot.png" })
    expect(textOfBlock(r.blocks[1], "text")).toBe("界面截图")
    expect(r.notes.filter((n) => n.includes("本地路径"))).toHaveLength(1)
  })

  test("画板 mermaid 内联 → 图表占位（服务端渲染为图片）", () => {
    const r = xmlToBlocks('<whiteboard type="mermaid">graph TD; A-->B</whiteboard>')
    expect(r.blocks[0]).toMatchObject({ _diagram_format: "mermaid", _diagram_code: "graph TD; A-->B" })
  })

  test("不支持的标签/属性给出说明而不静默丢弃", () => {
    const r = xmlToBlocks('<p>正文</p><bookmark name="站点" href="https://x"></bookmark><latex>E=mc^2</latex>')
    expect(r.notes.join()).toContain("<bookmark>")
    expect(r.notes.join()).toContain("equation")
  })

  test("没有任何标签时报错；只有 title 也报错", () => {
    expect(() => xmlToBlocks("纯文本没有标签")).toThrow(/缺少标签/)
    expect(() => xmlToBlocks("<title>只有标题</title>")).toThrow(/没有可导入的块/)
  })
})

describe("XML → 块描述：@人与图示文件引用", () => {
  test("cite type=user 生成 mention_user 元素（夹在文本中间也保留）", () => {
    const r = xmlToBlocks('<p>请 <cite type="user" user-id="ou_abc"/> 复核</p>')
    const els = fieldOf(r.blocks[0], "text").elements as Array<Record<string, unknown>>
    expect(els.map((e) => (e.mention_user ? "mention" : (e.text_run as { content?: string })?.content)).join("|")).toBe("请 |mention| 复核")
    expect((els[1].mention_user as { user_id: string }).user_id).toBe("ou_abc")
  })

  test("cite 缺 user-id / cite type=doc 均给出说明并降级", () => {
    expect(xmlToBlocks('<p>请 <cite type="user"/> 看</p>').notes.join()).toContain("缺少 user-id")
    expect(xmlToBlocks('<p>见 <cite type="doc" doc-id="docx1"/></p>').notes.join()).toContain("1770038")
  })

  test("whiteboard type=svg：path 直传图片、内联转 _svg_inline", () => {
    const p = xmlToBlocks('<whiteboard type="svg" path="@./pic.svg" caption="图"/>')
    expect(p.blocks[0]).toMatchObject({ block_type: 27, _image_src: "./pic.svg", _image_name: "pic.svg" })
    expect(p.notes.join()).toContain("SVG")
    const inline = xmlToBlocks('<whiteboard type="svg"><svg viewBox="0 0 1 1"></svg></whiteboard>')
    expect(typeof inline.blocks[0]._svg_inline).toBe("string")
    expect(String(inline.blocks[0]._svg_inline)).toContain("<svg")
  })

  test("whiteboard path 引用本地图表源码（注入 readFile）；读取失败时提示", () => {
    const files: Record<string, string> = { "./a.mmd": "graph TD;\n  A-->B" }
    const ok = xmlToBlocks('<whiteboard type="mermaid" path="@./a.mmd"/>', { readFile: (p) => files[p] })
    expect(ok.blocks[0]).toMatchObject({ _diagram_format: "mermaid", _diagram_code: "graph TD;\n  A-->B" })
    // 读取失败不阻断整篇（其他块照常导入），只是该图被跳过并记说明
    const fail = xmlToBlocks('<p>正文</p><whiteboard type="mermaid" path="@./none.mmd"/>', { readFile: (p) => files[p] })
    expect(fail.blocks).toHaveLength(1)
    expect(fail.notes.join()).toContain("图表文件读取失败")
    expect(xmlToBlocks('<p>正文</p><whiteboard type="mermaid" path="@./a.mmd"/>').notes.join()).toContain("图表文件读取失败")
  })
})
