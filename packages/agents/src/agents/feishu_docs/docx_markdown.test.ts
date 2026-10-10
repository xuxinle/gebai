import { describe, expect, test } from "bun:test"
import { blocksToMarkdown, escapeMd, inlineToMarkdown, type MdOptions } from "./docx_markdown"
import { codeLangEnum, markdownToBlocks } from "./feishu_api"

/* ================= 测试素材：把 markdownToBlocks 的产出铺平成 byId ================= */

/** 把 markdownToBlocks 的块组铺平成 { byId, topIds }（模拟飞书返回的块树）。 */
function flatten(md: string): { byId: Map<string, Record<string, unknown>>; topIds: string[] } {
  const groups = markdownToBlocks(md)
  const byId = new Map<string, Record<string, unknown>>()
  const topIds: string[] = []
  for (const g of groups) {
    for (const b of g.blocks) byId.set(String(b.block_id), b)
    topIds.push(g.rootId)
  }
  return { byId, topIds }
}

function roundTrip(md: string): { markdown: string; notes: string[]; byId: Map<string, Record<string, unknown>> } {
  const { byId, topIds } = flatten(md)
  const notes = new Set<string>()
  const opts: MdOptions = { byId, notes }
  const r = blocksToMarkdown(topIds, opts)
  return { markdown: r.markdown, notes: r.notes, byId }
}

/** 再次导入后的根块类型序列（markdownToBlocks 把容器块放在组的末尾，须按 rootId 取根块）。 */
function rootTypes(md: string): number[] {
  const groups = markdownToBlocks(md)
  const byId = new Map<string, Record<string, unknown>>()
  for (const g of groups) for (const b of g.blocks) byId.set(String((b as any).block_id), b as any)
  return groups.map((g) => Number(byId.get(g.rootId)?.block_type ?? 0))
}

/** 再次导入后的根块数组。 */
function rootBlocks(md: string): Array<Record<string, unknown>> {
  const groups = markdownToBlocks(md)
  const byId = new Map<string, Record<string, unknown>>()
  for (const g of groups) for (const b of g.blocks) byId.set(String((b as any).block_id), b as any)
  return groups.map((g) => byId.get(g.rootId)!)
}

/* ================= 行内 ================= */

describe("inlineToMarkdown", () => {
  const el = (content: string, style?: Record<string, unknown>) => (style ? { text_run: { content, text_element_style: style } } : { text_run: { content } })

  test("纯文本", () => {
    expect(inlineToMarkdown([el("你好")])).toBe("你好")
  })
  test("加粗/斜体/粗斜体", () => {
    expect(inlineToMarkdown([el("重点", { bold: true })])).toBe("**重点**")
    expect(inlineToMarkdown([el("斜", { italic: true })])).toBe("*斜*")
    expect(inlineToMarkdown([el("都", { bold: true, italic: true })])).toBe("***都***")
  })
  test("删除线与行内代码", () => {
    expect(inlineToMarkdown([el("旧", { strikethrough: true })])).toBe("~~旧~~")
    expect(inlineToMarkdown([el("npm i", { inline_code: true })])).toBe("`npm i`")
  })
  test("链接", () => {
    expect(inlineToMarkdown([el("飞书", { link: { url: "https://feishu.cn" } })])).toBe("[飞书](https://feishu.cn)")
  })
  test("特殊字符转义（防止再次写入时被当语法）", () => {
    expect(inlineToMarkdown([el("a*b")])).toBe("a\\*b")
    expect(inlineToMarkdown([el("~~x~~")])).toBe("\\~\\~x\\~\\~")
  })
  test("@人文本化（Markdown 无 @ 语法）", () => {
    expect(inlineToMarkdown([{ mention_user: { user_id: "ou_1" } }])).toBe("@ou_1")
  })
  test("换行保持", () => {
    expect(inlineToMarkdown([el("第一行\n第二行")])).toBe("第一行\n第二行")
  })
})

describe("escapeMd", () => {
  test("转义 Markdown 元字符", () => {
    expect(escapeMd("*a*_b_`c`~d~[e]")).toBe("\\*a\\*\\_b\\_\\`c\\`\\~d\\~\\[e\\]")
  })
  test("反斜杠自身转义", () => {
    // 反斜杠是转义字符，自身被转义（与不可转义字符组合时保持原义）
    expect(escapeMd("C:\\Users\\*bin")).toBe("C:\\Users\\\\*bin")
  })
})

/* ================= 块级 round-trip（读出的 Markdown 再次导入应得到等价块） ================= */

describe("blocksToMarkdown round-trip", () => {
  test("标题层级", () => {
    const { markdown } = roundTrip("# 一级\n\n## 二级\n\n### 三级\n")
    expect(markdown).toBe("# 一级\n\n## 二级\n\n### 三级")
    // 再导入：层级不漂移
    expect(rootTypes(markdown)).toEqual([3, 4, 5])
  })

  test("段落与行内样式", () => {
    const { markdown } = roundTrip("这是**加粗**与`代码`的段落\n")
    expect(markdown).toBe("这是**加粗**与`代码`的段落")
    const els = (rootBlocks(markdown)[0] as any).text.elements as any[]
    expect(els.some((e) => e.text_run?.text_element_style?.bold)).toBe(true)
    expect(els.some((e) => e.text_run?.text_element_style?.inline_code)).toBe(true)
  })

  test("无序列表（含嵌套）", () => {
    const { markdown } = roundTrip("- 项A\n  - 子项\n- 项B\n")
    expect(markdown).toBe("- 项A\n  - 子项\n- 项B")
    const again = rootBlocks(markdown)
    expect(again.length).toBe(2) // 项A（含子项）、项B
    expect((again[0] as any).children?.length).toBe(1) // 子项挂在项A 下
  })

  test("有序列表（编号递增与起始序号）", () => {
    const { markdown } = roundTrip("1. 第一步\n2. 第二步\n3. 第三步\n")
    expect(markdown).toBe("1. 第一步\n2. 第二步\n3. 第三步")
    const again = rootBlocks(markdown)
    expect(again.length).toBe(3)
    expect(Number((again[0] as any).ordered.style.sequence)).toBe(1)
  })

  test("待办列表（勾选态）", () => {
    const { markdown } = roundTrip("- [x] 已完成\n- [ ] 未完成\n")
    expect(markdown).toBe("- [x] 已完成\n- [ ] 未完成")
    const again = rootBlocks(markdown)
    expect(again.length).toBe(2)
    expect((again[0] as any).todo.style.done).toBe(true)
    expect((again[1] as any).todo.style.done).toBe(false)
  })

  test("代码块（带语言）", () => {
    const { markdown } = roundTrip("```ts\nconst a = 1\n```\n")
    expect(markdown).toBe("```typescript\nconst a = 1\n```")
    // 语言名再导入映射回同一枚举（ts=63）
    expect(Number((rootBlocks(markdown)[0] as any).code.style.language)).toBe(codeLangEnum("ts"))
  })

  test("引用块", () => {
    const { markdown } = roundTrip("> 引用内容\n")
    expect(markdown).toBe("> 引用内容")
    expect(rootTypes(markdown)).toEqual([15])
  })

  test("分割线", () => {
    const { markdown } = roundTrip("---\n")
    expect(markdown).toBe("---")
    expect(rootTypes(markdown)).toEqual([22])
  })

  test("表格（表头 + 分隔行 + 数据行）", () => {
    const { markdown } = roundTrip("| 名 | 值 |\n| --- | --- |\n| a | 1 |\n| b | 2 |\n")
    expect(markdown).toBe("| 名 | 值 |\n| --- | --- |\n| a | 1 |\n| b | 2 |")
    expect(rootTypes(markdown)).toEqual([31])
  })

  test("高亮块 → GitHub 告示语法", () => {
    const { markdown } = roundTrip("> [!NOTE]\n> 提示内容\n")
    expect(markdown).toContain("> [!NOTE]")
    expect(markdown).toContain("> 提示内容")
  })

  test("综合文档：结构与顺序保持", () => {
    const src = "# 标题\n\n段落内容。\n\n- 项一\n- 项二\n\n```go\nfmt.Println(1)\n```\n\n| a | b |\n| --- | --- |\n| 1 | 2 |\n\n> 引用\n"
    const { markdown } = roundTrip(src)
    expect(rootTypes(markdown)).toEqual([3, 2, 12, 12, 14, 31, 15])
  })

  test("空文档 → 空字符串", () => {
    const { markdown } = blocksToMarkdown([], { byId: new Map(), notes: new Set() })
    expect(markdown).toBe("")
  })
})

/* ================= 超出 Markdown 表达力的块：降级 + 提示（不静默丢） ================= */

describe("Markdown 子集外的块", () => {
  test("分栏 grid → 按列顺序展开并提示", () => {
    const byId = new Map<string, Record<string, unknown>>([
      ["g", { block_id: "g", block_type: 24, children: ["c1", "c2"], grid: { column_size: 2 } }],
      ["c1", { block_id: "c1", block_type: 25, parent_id: "g", children: ["p1"], grid_column: { width_ratio: 1 } }],
      ["c2", { block_id: "c2", block_type: 25, parent_id: "g", children: ["p2"], grid_column: { width_ratio: 1 } }],
      ["p1", { block_id: "p1", block_type: 2, parent_id: "c1", text: { elements: [{ text_run: { content: "左" } }] } }],
      ["p2", { block_id: "p2", block_type: 2, parent_id: "c2", text: { elements: [{ text_run: { content: "右" } }] } }],
    ])
    const notes = new Set<string>()
    const r = blocksToMarkdown(["g"], { byId, notes })
    expect(r.markdown).toContain("左")
    expect(r.markdown).toContain("右")
    expect(r.notes.join(" ")).toContain("分栏")
  })

  test("图片 → token 占位并提示走 download_file", () => {
    const byId = new Map<string, Record<string, unknown>>([
      ["i", { block_id: "i", block_type: 27, image: { token: "boxcnABC" } }],
    ])
    const r = blocksToMarkdown(["i"], { byId, notes: new Set() })
    expect(r.markdown).toContain("feishu-image:boxcnABC")
    expect(r.notes.join(" ")).toContain("download_file")
  })

  test("画板/多维表格 → 占位并提示用 XML 通道保真", () => {
    const byId = new Map<string, Record<string, unknown>>([
      ["b", { block_id: "b", block_type: 43, mindnote: { token: "brdX" } }],
    ])
    const r = blocksToMarkdown(["b"], { byId, notes: new Set() })
    expect(r.markdown).toContain("画板")
    expect(r.notes.join(" ")).toContain("XML 通道")
  })
})
