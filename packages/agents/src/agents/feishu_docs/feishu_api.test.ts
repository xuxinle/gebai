import { describe, expect, test } from "bun:test"
import { mkdirSync, mkdtempSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { sessionPath } from "@gebai/sdk/node"
import { createFeishuTools, markdownToBlocks, textElements, stripTableMergeInfo, blockText, blockTypeName, normalizeBlockFields, prepareGridColumns, widthRatioWeights, gridStructureError, expandAppendRange, extractBoardToken, findPlantUmlSource, collectBoardShapes, collectBoardEdges, extractBoardContent, extractOAuthCode, parseFolderToken, displayWidth, tableColumnWidths, tablePropertyOf, codeLangEnum, CODE_LANG_COUNT, dropDuplicateTitleHeading, xmlProfile, docOutline, replaceInBlocks, compactBlocks, TABLE_PAGE_WIDTH, TABLE_MIN_COLUMN_WIDTH, type FeishuDeps, type UserTokenEntry } from "./feishu_api"
import { def as feishuDef } from "./feishu_docs"

type Req = { url: string; init?: RequestInit }

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } })
}

/** 构造注入 mock fetch 的工具集；records 收集所有请求。用户令牌用内存 store（按 home|user|session 隔离）。 */
function makeTools(handler: (req: Req) => Response | Promise<Response>) {
  const records: Req[] = []
  const userTokens = new Map<string, UserTokenEntry | null>()
  const deps: FeishuDeps = {
    tokenCache: new Map(),
    fetchFn: (async (url: string | URL | Request, init?: RequestInit) => {
      const req: Req = { url: String(url), init }
      records.push(req)
      return await handler(req)
    }) as typeof fetch,
    userTokenStore: {
      get: async (c) => userTokens.get(`${c.home}|${c.user}|${c.sessionId}`) ?? null,
      set: async (c, entry) => {
        userTokens.set(`${c.home}|${c.user}|${c.sessionId}`, entry)
      },
      clear: async (c) => {
        userTokens.delete(`${c.home}|${c.user}|${c.sessionId}`)
      },
    },
  }
  const tools = createFeishuTools(deps)
  return { tools, records, userTokens }
}

/** 默认 handler：token 接口 + 其他接口返回空 data。 */
function defaultHandler(req: Req): Response {
  if (req.url.includes("/open-apis/auth/v3/tenant_access_token/internal")) {
    return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-test-token", expire: 7200 })
  }
  return jsonResponse({ code: 0, msg: "success", data: {} })
}

function ctx(overrides: Partial<ToolContext> = {}): ToolContext {
  const home = mkdtempSync(join(tmpdir(), "feishu-"))
  const sid = "0123456789abcdef0123456789abcdef" // 合法会话 id（32 位 hex）
  const tmp = join(home, "users", "default", "sessions", sid, "tmp")
  const base: ToolContext = {
    user: "default",
    sessionId: sid,
    workdir: tmp,
    home,
    env: { FEISHU_DOCS_APP_ID: "cli_test_app", FEISHU_DOCS_APP_SECRET: "secret_test" },
    sandboxed: false,
    resolvePath: (p) => join(tmp, p),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async () => {},
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (r) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => { throw new Error("未知预置项目") },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
  }
  return { ...base, ...overrides }
}

/* ================= Markdown 转换 ================= */

describe("textElements 行内解析", () => {
  test("纯文本", () => {
    expect(textElements("hello")).toEqual([{ text_run: { content: "hello" } }])
  })
  test("加粗/代码/链接/斜体", () => {
    const els = textElements("a **bold** `code` [link](https://x.com) *ita* tail")
    expect(els).toEqual([
      { text_run: { content: "a " } },
      { text_run: { content: "bold", text_element_style: { bold: true } } },
      { text_run: { content: " " } },
      { text_run: { content: "code", text_element_style: { inline_code: true } } },
      { text_run: { content: " " } },
      { text_run: { content: "link", text_element_style: { link: { url: "https://x.com" } } } },
      { text_run: { content: " " } },
      { text_run: { content: "ita", text_element_style: { italic: true } } },
      { text_run: { content: " tail" } },
    ])
  })
  test("空字符串返回空 run", () => {
    expect(textElements("")).toEqual([{ text_run: { content: "" } }])
  })
})

describe("markdownToBlocks", () => {
  test("标题与段落", () => {
    const groups = markdownToBlocks("# 标题1\n\n## 标题2\n\n正文段落")
    expect(groups.length).toBe(3)
    expect(groups[0].blocks[0]).toMatchObject({ block_id: groups[0].rootId, block_type: 3, heading1: { elements: [{ text_run: { content: "标题1" } }] }, children: [] })
    expect(groups[1].blocks[0].block_type).toBe(4)
    expect(groups[2].blocks[0]).toMatchObject({ block_type: 2, text: { elements: [{ text_run: { content: "正文段落" } }] } })
  })
  test("列表与任务", () => {
    const groups = markdownToBlocks("- 无序项\n1. 有序项\n- [x] 已完成\n- [ ] 未完成")
    expect(groups.map((g) => g.blocks[0].block_type)).toEqual([12, 13, 17, 17])
    expect(groups[2].blocks[0]).toMatchObject({ todo: { style: { done: true } } })
    expect(groups[3].blocks[0]).toMatchObject({ todo: { style: { done: false } } })
  })
  test("嵌套列表：缩进 2 空格一级，子项作为父块 children", () => {
    const groups = markdownToBlocks("- a\n  - a1\n    - a1x\n- b\n  1. b1\n  2. b2")
    // 两个顶层项各成一个块组（root + 嵌套子树）
    expect(groups.length).toBe(2)
    const rootA = groups[0].blocks.find((b) => b.block_id === groups[0].rootId)!
    const a1 = groups[0].blocks.find((b) => (b.children as string[])?.includes((rootA.children as string[])[0]))!
    expect(a1.block_type).toBe(12)
    const a1Child = groups[0].blocks.find((b) => (b.children as string[]).includes((a1.children as string[])[0]))
    expect(a1Child?.block_type).toBe(12) // 三级仍为 bullet（- a1x）
    const rootB = groups[1].blocks.find((b) => b.block_id === groups[1].rootId)!
    const bKids = rootB.children as string[]
    expect(bKids.length).toBe(2)
    const ordered = bKids.map((id) => groups[1].blocks.find((b) => b.block_id === id)!.block_type)
    expect(ordered).toEqual([13, 13]) // 嵌套有序列表
  })
  test("嵌套列表：跳级缩进归一为 +1 级；todo 可嵌套", () => {
    const groups = markdownToBlocks("- a\n        - deep\n- [x] 顶层\n  - [ ] 子待办")
    const rootA = groups[0].blocks.find((b) => b.block_id === groups[0].rootId)!
    expect((rootA.children as string[]).length).toBe(1) // 8 空格缩进归一为一级子项
    const rootTodo = groups[1].blocks.find((b) => b.block_id === groups[1].rootId)!
    expect(rootTodo.block_type).toBe(17)
    const subTodo = groups[1].blocks.find((b) => b.block_id === (rootTodo.children as string[])[0])!
    expect(subTodo).toMatchObject({ block_type: 17, todo: { style: { done: false } } })
  })
  test("GitHub 告示语法转 callout 高亮块（配色 + emoji + 标题粗体）", () => {
    const g = markdownToBlocks("> [!NOTE] 提示标题\n> 正文内容")
    expect(g.length).toBe(1)
    // 实测：样式为 callout 顶层字段，正文在 children 子块
    const callout = g[0].blocks.find((b) => b.block_id === g[0].rootId)!
    expect(callout).toMatchObject({ block_type: 19, callout: { background_color: 5, border_color: 5, emoji_id: "bulb" } })
    const kids = (callout.children as string[]).map((id) => g[0].blocks.find((b) => b.block_id === id)!)
    expect(kids).toHaveLength(1)
    expect((kids[0].text as { elements: unknown[] }).elements).toEqual([
      { text_run: { content: "提示标题", text_element_style: { bold: true } } },
      { text_run: { content: "\n" } },
      { text_run: { content: "正文内容" } },
    ])
    expect(markdownToBlocks("> [!CAUTION]\n> 危险操作")[0].blocks.find((b) => b.block_type === 19)).toMatchObject({ callout: { background_color: 1, border_color: 1, emoji_id: "pushpin" } })
    // 正文空行 → 多段子块（标题与首段同块，共 2 个子块）
    const multi = markdownToBlocks("> [!TIP] 标题\n>\n> 第一段\n>\n> 第二段")
    const c2 = multi[0].blocks.find((b) => b.block_id === multi[0].rootId)!
    expect((c2.children as string[]).length).toBe(2)
    const c2kids = (c2.children as string[]).map((id) => multi[0].blocks.find((b) => b.block_id === id)!)
    const firstEls = (c2kids[0].text as { elements: Array<{ text_run: { content: string } }> }).elements
    expect(firstEls.map((e) => e.text_run.content).join("")).toBe("标题\n第一段")
    expect((c2kids[1].text as { elements: Array<{ text_run: { content: string } }> }).elements[0].text_run.content).toBe("第二段")
    // 普通引用不误判
    expect(markdownToBlocks("> 普通引用")[0].blocks[0].block_type).toBe(15)
  })
  test("多级标题 7~9 与行内删除线/粗斜体", () => {
    const groups = markdownToBlocks("####### h7")
    expect(groups[0].blocks[0].block_type).toBe(9)
    expect(markdownToBlocks("######### h9")[0].blocks[0].block_type).toBe(11)
    const els = textElements("~~gone~~ ***both***")
    expect(els).toEqual([
      { text_run: { content: "gone", text_element_style: { strikethrough: true } } },
      { text_run: { content: " " } },
      { text_run: { content: "both", text_element_style: { bold: true, italic: true } } },
    ])
  })
  test("代码块与引用与分割线", () => {
    const groups = markdownToBlocks("```ts\nconst a = 1\n```\n\n> 引用内容\n\n---")
    expect(groups.length).toBe(3)
    // language 为数字枚举（官方枚举表：ts/TypeScript=63、js/JavaScript=30；未知→PlainText=1）
    expect(groups[0].blocks[0]).toMatchObject({ block_type: 14, code: { style: { language: 63 }, elements: [{ text_run: { content: "const a = 1" } }] } })
    expect(markdownToBlocks("```js\nx\n```")[0].blocks[0]).toMatchObject({ code: { style: { language: 30 } } })
    expect(markdownToBlocks("```unknownlang\nx\n```")[0].blocks[0]).toMatchObject({ code: { style: { language: 1 } } })
    expect(markdownToBlocks("```\nx\n```")[0].blocks[0]).toMatchObject({ code: { style: { language: 1 } } })
    expect(groups[1].blocks[0]).toMatchObject({ block_type: 15, quote: {} })
    expect(groups[2].blocks[0]).toMatchObject({ block_type: 22, divider: {} })
  })
  test("表格展开为 table + cell + 文本块（children 引用正确）", () => {
    const groups = markdownToBlocks("| 列A | 列B |\n|---|---|\n| a1 | b1 |")
    expect(groups.length).toBe(1)
    const blocks = groups[0].blocks
    const table = blocks.find((b) => b.block_type === 31)!
    expect(table).toMatchObject({ block_id: groups[0].rootId, block_type: 31, table: { property: { row_size: 2, column_size: 2 } } })
    const cellIds = table.children as string[]
    expect(cellIds.length).toBe(4)
    const cells = blocks.filter((b) => b.block_type === 32)
    expect(cells.length).toBe(4)
    const texts = blocks.filter((b) => b.block_type === 2)
    expect(texts.map((t) => (t.text as { elements?: Array<{ text_run?: { content?: string } }> })?.elements?.[0]?.text_run?.content)).toEqual(["列A", "列B", "a1", "b1"])
    for (const cell of cells) {
      expect((cell.children as string[]).length).toBe(1)
    }
  })
  test("每个块都有唯一 block_id 且 rootId 存在", () => {
    const groups = markdownToBlocks("# a\n\n- b\n\n| x |\n|---|\n| y |")
    const ids = groups.flatMap((g) => g.blocks.map((b) => b.block_id as string))
    expect(new Set(ids).size).toBe(ids.length)
    for (const g of groups) expect(ids).toContain(g.rootId)
  })
})

describe("表格列宽自适应", () => {
  test("displayWidth：汉字/全角计双宽", () => {
    expect(displayWidth("abc")).toBe(3)
    expect(displayWidth("中文")).toBe(4)
    expect(displayWidth("a中1")).toBe(4)
  })

  test("按内容分配：总宽为页面宽度、长内容列更宽、单列不低于下限", () => {
    const widths = tableColumnWidths([["序号", "说明"], ["1", "这是一段比较长的说明文字"]])
    expect(widths.reduce((a, b) => a + b, 0)).toBe(TABLE_PAGE_WIDTH)
    expect(widths[1]).toBeGreaterThan(widths[0])
    expect(Math.min(...widths)).toBeGreaterThanOrEqual(TABLE_MIN_COLUMN_WIDTH)
  })

  test("列数过多（最小宽度之和超页面）时全部取最小宽度", () => {
    expect(tableColumnWidths([Array.from({ length: 9 }, (_, i) => `列${i}`)])).toEqual(new Array(9).fill(TABLE_MIN_COLUMN_WIDTH))
  })

  test("totalWidth 可指定目标总宽", () => {
    expect(tableColumnWidths([["a", "b"]], 300).reduce((a, b) => a + b, 0)).toBe(300)
  })

  test("表格列宽自适应：单元格多段落按最长行计宽", () => {
    const widths = tableColumnWidths([["标题", "多段落"], ["a", "第一段\n第二段比较长的内容"]])
    expect(widths.reduce((a, b) => a + b, 0)).toBe(TABLE_PAGE_WIDTH)
    expect(widths[1]).toBeGreaterThan(widths[0])
  })

  test("tablePropertyOf：显式列宽原样使用，长度不符报错，header_row 透传", () => {
    expect(tablePropertyOf({ column_width: [200, 300] }, [["a", "b"]], 2)).toEqual({ column_width: [200, 300] })
    expect(tablePropertyOf({ property: { column_width: [120, 180] } }, [["a", "b"]], 2)).toEqual({ column_width: [120, 180] })
    expect(tablePropertyOf({ column_width: [200, 300], header_row: true }, [["a", "b"]], 2)).toEqual({ column_width: [200, 300], header_row: true })
    expect(() => tablePropertyOf({ column_width: [200] }, [["a", "b"]], 2)).toThrow("必须与列数")
    expect(() => tablePropertyOf({ column_width: [20, 300] }, [["a", "b"]], 2)).toThrow("不小于 50")
  })

  test("Markdown 表格：首行设为标题行且列宽自适应", () => {
    const table = markdownToBlocks("| 列A | 列B |\n|---|---|\n| a1 | b1 |")[0].blocks.find((b) => b.block_type === 31)!
    const prop = (table.table as { property: Record<string, unknown> }).property
    expect(prop.header_row).toBe(true)
    expect((prop.column_width as number[]).reduce((a, b) => a + b, 0)).toBe(TABLE_PAGE_WIDTH)
  })
})

describe("排版细节（语言枚举/有序序号/单元格/图片）", () => {
  const cellContents = (md: string) =>
    markdownToBlocks(md)[0].blocks
      .filter((b) => b.block_type === 2)
      .map((b) => (b.text as { elements: Array<{ text_run: { content: string } }> }).elements[0].text_run.content)

  test("代码块语言枚举与官方枚举表一致（抽样）", () => {
    const cases: Array<[string | undefined, number]> = [
      ["plaintext", 1], ["bash", 7], ["sh", 7], ["csharp", 8], ["cpp", 9], ["c++", 9], ["c", 10], ["css", 12], ["dart", 15],
      ["dockerfile", 18], ["go", 22], ["html", 24], ["json", 28], ["java", 29], ["javascript", 30], ["js", 30], ["kotlin", 32],
      ["tex", 33], ["lua", 36], ["makefile", 38], ["markdown", 39], ["md", 39], ["nginx", 40], ["objective-c", 41], ["php", 43],
      ["perl", 44], ["powershell", 46], ["python", 49], ["py", 49], ["r", 50], ["ruby", 52], ["rust", 53], ["sql", 56],
      ["scala", 57], ["shell", 60], ["swift", 61], ["typescript", 63], ["ts", 63], ["xml", 66], ["yaml", 67], ["yml", 67],
      ["toml", 75], ["c#", 8], ["golang", 22], ["kt", 32], [undefined, 1], ["不存在的语言", 1],
    ]
    for (const [lang, expected] of cases) expect(`${lang}=${codeLangEnum(lang)}`).toBe(`${lang}=${expected}`)
    expect(CODE_LANG_COUNT).toBe(75)
  })

  test("代码块默认 wrap=true（长行自动换行不溢出）", () => {
    expect(markdownToBlocks("```ts\nx\n```")[0].blocks[0]).toMatchObject({ code: { style: { language: 63, wrap: true } } })
  })

  test("有序列表序号：段首项定起始、其后自增（CommonMark 语义）", () => {
    const seqs = (md: string) =>
      markdownToBlocks(md).map((g) => (g.blocks[0].ordered as { style?: { sequence?: string } } | undefined)?.style?.sequence)
    expect(seqs("3. 甲\n4. 乙")).toEqual(["3", "4"])
    expect(seqs("1. 甲\n1. 乙")).toEqual(["1", "2"])
    expect(seqs("5. 甲\n9. 乙")).toEqual(["5", "6"])
  })

  test("嵌套有序列表：子层独立编号；bullet 打断有序段", () => {
    const seqs = (md: string) =>
      markdownToBlocks(md)
        .flatMap((g) => g.blocks)
        .filter((b) => b.block_type === 13)
        .map((b) => (b.ordered as { style?: { sequence?: string } } | undefined)?.style?.sequence)
    expect(seqs("- 项\n  1. 子甲\n  1. 子乙")).toEqual(["1", "2"])
    expect(seqs("2. 甲\n- 中\n1. 乙")).toEqual(["2", "1"])
  })

  test("表格单元格：\\| 为字面竖线、<br> 转软换行且不错切列", () => {
    expect(cellContents("| 列A | 列B |\n|---|---|\n| a \\| b | c |")).toEqual(["列A", "列B", "a | b", "c"])
    expect(cellContents("| 表头 |\n|---|\n| 第一行<br>第二行 |")).toEqual(["表头", "第一行\n第二行"])
    const table = markdownToBlocks("| 列A | 列B |\n|---|---|\n| a \\| b | c |")[0].blocks.find((b) => b.block_type === 31)!
    expect((table.table as { property: { column_size: number } }).property.column_size).toBe(2)
  })

  test("表格单元格：反引号（行内代码）内的 | 不切列", () => {
    // 反引号内容会被解析为行内代码样式（反引号本身不进入文本）
    expect(cellContents("| A | B |\n|---|---|\n| `x|y` | z |")).toEqual(["A", "B", "x|y", "z"])
    const table = markdownToBlocks("| A | B |\n|---|---|\n| `x|y` | z |")[0].blocks.find((b) => b.block_type === 31)!
    expect((table.table as { property: { column_size: number } }).property.column_size).toBe(2)
    const blocks = markdownToBlocks("| A |\n|---|\n| `x|y` |")[0].blocks
    const cells = blocks.filter((b) => b.block_type === 32)
    const firstText = (cells[1].children as string[])[0] // 数据行单元格
    const inline = blocks.find((b) => b.block_id === firstText) as { text: { elements: Array<{ text_run: { text_element_style?: { inline_code?: boolean } } }> } }
    expect(inline.text.elements[0].text_run.text_element_style?.inline_code).toBe(true)
  })

  test("表格单元格：连续两个 <br> 转多段落（单元格可含多个子块）", () => {
    const blocks = markdownToBlocks("| A |\n|---|\n| 段一<br><br>段二 |")[0].blocks
    const cells = blocks.filter((b) => b.block_type === 32)
    expect((cells[0].children as string[]).length).toBe(1) // 表头单元格
    expect((cells[1].children as string[]).length).toBe(2) // 数据行：两段落
    const texts = blocks.filter((b) => b.block_type === 2).map((b) => (b.text as { elements: Array<{ text_run: { content: string } }> }).elements[0].text_run.content)
    expect(texts).toEqual(["A", "段一", "段二"])
  })

  test("引用块：引用内代码围栏转行内代码样式（quote 平台不支持子块）", () => {
    const quote = markdownToBlocks("> 说明\n> ```ts\n> const x = 1\n> ```\n> 后续")[0].blocks[0] as { block_type: number; quote: { elements: Array<{ text_run: { content: string; text_element_style?: { inline_code?: boolean } } }> } }
    expect(quote.block_type).toBe(15)
    const code = quote.quote.elements.find((e) => e.text_run.text_element_style?.inline_code)
    expect(code?.text_run.content).toBe("const x = 1")
    expect(quote.quote.elements.map((e) => e.text_run.content).join("")).toContain("说明")
    expect(quote.quote.elements.map((e) => e.text_run.content).join("")).toContain("后续")
  })

  test("引用块：多行文本合并为软换行（无代码围栏时不变）", () => {
    const quote = markdownToBlocks("> 甲\n> 乙")[0].blocks[0] as { quote: { elements: Array<{ text_run: { content: string } }> } }
    expect(quote.quote.elements.map((e) => e.text_run.content).join("")).toBe("甲\n乙")
  })

  test("段落首行缩进：行首两个全角空格 → onelevelindent", () => {
    const b = markdownToBlocks("\u3000\u3000缩进段落")[0].blocks[0] as { block_type: number; text: { style?: { indentation_level?: string }; elements: Array<{ text_run: { content: string } }> } }
    expect(b.text.style?.indentation_level).toBe("OneLevelIndent")
    expect(b.text.elements[0].text_run.content).toBe("缩进段落")
    // &emsp; 写法等价
    const amp = markdownToBlocks("&emsp;&emsp;缩进段落")[0].blocks[0] as { text: { style?: { indentation_level?: string } } }
    expect(amp.text.style?.indentation_level).toBe("OneLevelIndent")
    // 普通段落不缩进
    const plain = markdownToBlocks("普通段落")[0].blocks[0] as { text: { style?: unknown } }
    expect(plain.text.style).toBeUndefined()
  })

  test("dropDuplicateTitleHeading：新建文档时首行 H1 与标题重复则去掉", () => {
    expect(dropDuplicateTitleHeading("# 项目周报\n\n正文", "项目周报")).toBe("正文")
    expect(dropDuplicateTitleHeading("# 项目周报\n正文", "项目周报")).toBe("正文")
    // 标题不同 / 首行非 H1 → 原样保留
    expect(dropDuplicateTitleHeading("# 其他\n正文", "项目周报")).toBe("# 其他\n正文")
    expect(dropDuplicateTitleHeading("正文", "项目周报")).toBe("正文")
  })

  test("Markdown 图片生成占位块（图源记在本地字段）", () => {
    const g = markdownToBlocks("![架构图](./arch.png)")
    expect(g.length).toBe(1)
    expect(g[0].blocks[0]).toMatchObject({ block_type: 27, image: {}, _image_src: "./arch.png" })
  })

  test("import_markdown 图片回填：按 block_id_relations 上传素材并 replace_image", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) {
        const body = JSON.parse(String(req.init?.body)) as { descendants: Array<{ block_id: string; block_type: number }> }
        const img = body.descendants.find((b) => b.block_type === 27)!
        return jsonResponse({ code: 0, msg: "success", data: { block_id_relations: [{ block_id: "real_img", temporary_block_id: img.block_id }] } })
      }
      if (req.url.includes("/medias/upload_all")) return jsonResponse({ code: 0, msg: "success", data: { file_token: "media_tok" } })
      return jsonResponse({ code: 0, msg: "success", data: { image: { width: 240, height: 160 } } })
    })
    const c = ctx()
    mkdirSync(c.workdir, { recursive: true })
    await Bun.write(join(c.workdir, "pic.png"), new Uint8Array([137, 80, 78, 71, 1, 2, 3]))
    const r = await tools.import_markdown.execute({ document_id: "doxcn1", content: "![图](pic.png)" }, c)
    expect(r.output).toContain("图片: 1/1 已插入")
    // 占位块的本地字段不进请求体
    const desc = records.find((x) => x.url.includes("/descendant"))!
    expect(String(desc.init?.body)).not.toContain("_image_src")
    // 素材上传绑定到真实块 id，随后 replace_image 写入素材 token
    const upload = records.find((x) => x.url.includes("/medias/upload_all"))!
    expect((upload.init?.body as FormData).get("parent_node")).toBe("real_img")
    expect((upload.init?.body as FormData).get("parent_type")).toBe("docx_image")
    const patch = records.find((x) => x.init?.method === "PATCH")!
    expect(String(patch.init?.body)).toContain("media_tok")
  })

  test("import_markdown 图片失败只提示不中断导入", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const r = await tools.import_markdown.execute({ document_id: "doxcn1", content: "![缺失图](./nope.png)" }, ctx())
    expect(r.output).toContain("已导入 1 个顶层块")
    expect(r.output).toContain("未插入")
  })

  test("图表围栏识别为图表占位块（mermaid/plantuml/d2/echarts）", () => {
    const cases: Array<[string, string]> = [["mermaid", "mermaid"], ["mmd", "mermaid"], ["plantuml", "plantuml"], ["puml", "plantuml"], ["d2", "d2"], ["echarts", "echarts"], ["MERMAID", "mermaid"]]
    for (const [lang, fmt] of cases) {
      expect(`${lang}:${JSON.stringify(markdownToBlocks("```" + lang + "\nX\n```")[0].blocks[0])}`).toBe(
        `${lang}:${JSON.stringify({ block_id: markdownToBlocks("```" + lang + "\nX\n```")[0].blocks[0].block_id, block_type: 27, image: {}, _diagram_format: fmt, _diagram_code: "X", children: [] })}`,
      )
    }
    // 非图表语言与无语言围栏仍为代码块
    expect(markdownToBlocks("```ts\nx\n```")[0].blocks[0]).toMatchObject({ block_type: 14 })
    expect(markdownToBlocks("```\nx\n```")[0].blocks[0]).toMatchObject({ block_type: 14 })
  })

  test("import_markdown 图表围栏：服务端渲染 PNG 并上传素材回填", async () => {
    const png = new Uint8Array([137, 80, 78, 71, 9, 9])
    const rendered: string[] = []
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) {
        const body = JSON.parse(String(req.init?.body)) as { descendants: Array<{ block_id: string; block_type: number }> }
        const rel = body.descendants.filter((b) => b.block_type === 27).map((b, i) => ({ block_id: `real_img${i}`, temporary_block_id: b.block_id }))
        return jsonResponse({ code: 0, msg: "success", data: { block_id_relations: rel } })
      }
      if (req.url.includes("/medias/upload_all")) return jsonResponse({ code: 0, msg: "success", data: { file_token: "media_tok" } })
      return jsonResponse({ code: 0, msg: "success", data: { image: { width: 800, height: 600 } } })
    })
    const withRenderer = ctx({ renderDiagram: async (code: string, opts?: { format?: string }) => { rendered.push(`${opts?.format}:${code.split("\n")[0]}`); return png } } as unknown as Partial<ToolContext>)
    const r = await tools.import_markdown.execute(
      { document_id: "doxcn1", content: "```mermaid\ngraph TD\n  A-->B\n```\n\n```ts\nconst a = 1\n```" },
      withRenderer,
    )
    expect(rendered).toEqual(["mermaid:graph TD"]) // 仅图表围栏送渲染，代码块不渲染
    expect(r.output).toContain("图表: 1/1 已渲染为图片")
    const desc = JSON.parse(String(records.find((x) => x.url.includes("/descendant"))!.init?.body)) as { descendants: Array<Record<string, unknown>> }
    // 本地字段（_diagram_*）不随请求外发
    expect(JSON.stringify(desc)).not.toContain("_diagram")
    expect(JSON.stringify(desc)).not.toContain("_image_bytes")
    const img = desc.descendants.find((b) => b.block_type === 27)!
    expect(img.image).toEqual({})
    const tsBlock = desc.descendants.find((b) => b.block_type === 14)!
    expect((tsBlock.code as { style: { language: number } }).style.language).toBe(63)
    // 素材上传绑定真实块 id，随后 replace_image 写入素材 token
    const upload = (records.find((x) => x.url.includes("/medias/upload_all"))!.init?.body as FormData)
    expect(upload.get("parent_node")).toBe("real_img0")
    expect(upload.get("file_name")).toBe("mermaid.png")
    const patch = JSON.parse(String(records.find((x) => x.init?.method === "PATCH")!.init?.body)) as { replace_image: { token: string } }
    expect(patch.replace_image.token).toBe("media_tok")
  })

  test("图表渲染失败/渲染器缺失时降级为代码块（源码保留，导入不中断）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const failing = ctx({ renderDiagram: async () => { throw new Error("Mermaid 渲染错误：bad syntax") } } as unknown as Partial<ToolContext>)
    const r1 = await tools.import_markdown.execute({ document_id: "doxcn1", content: "```mermaid\ngraph TD\n  A-->B\n```" }, failing)
    expect(r1.output).toContain("0/1 已渲染为图片")
    expect(r1.output).toContain("bad syntax")
    const desc1 = JSON.parse(String(records.find((x) => x.url.includes("/descendant"))!.init?.body)) as { descendants: Array<Record<string, unknown>> }
    const code1 = desc1.descendants.find((b) => b.block_type === 14) as { code: { style: { language: number }; elements: Array<{ text_run: { content: string } }> } }
    expect(code1.code.elements[0].text_run.content).toBe("graph TD\n  A-->B")
    expect(code1.code.style.language).toBe(1) // 飞书无图表语言枚举：降级块按 PlainText 标注
    // 渲染器缺失（受限环境）：同样降级并注明原因
    const r2 = await tools.import_markdown.execute({ document_id: "doxcn1", content: "```plantuml\n@startuml\nA->B\n@enduml\n```" }, ctx())
    expect(r2.output).toContain("未提供服务端图表渲染")
  })

  test("diagram_source=keep 在图下追加源码代码块", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) {
        const body = JSON.parse(String(req.init?.body)) as { descendants: Array<{ block_id: string; block_type: number }> }
        const rel = body.descendants.filter((b) => b.block_type === 27).map((b) => ({ block_id: "real_img", temporary_block_id: b.block_id }))
        return jsonResponse({ code: 0, msg: "success", data: { block_id_relations: rel } })
      }
      if (req.url.includes("/medias/upload_all")) return jsonResponse({ code: 0, msg: "success", data: { file_token: "tok" } })
      return jsonResponse({ code: 0, msg: "success", data: { image: {} } })
    })
    const withRenderer = ctx({ renderDiagram: async () => new Uint8Array([1, 2, 3]) } as unknown as Partial<ToolContext>)
    const r = await tools.import_markdown.execute({ document_id: "doxcn1", content: "```mermaid\ngraph LR\n  X-->Y\n```", diagram_source: "keep" }, withRenderer)
    expect(r.output).toContain("图表: 1/1")
    const desc = JSON.parse(String(records.find((x) => x.url.includes("/descendant"))!.init?.body)) as { descendants: Array<Record<string, unknown>> }
    const kinds = desc.descendants.map((b) => b.block_type)
    expect(kinds).toEqual([27, 14]) // 图 + 源码代码块（图下）
    const src = desc.descendants[1] as { code: { style: { language: number }; elements: Array<{ text_run: { content: string } }> } }
    expect(src.code.elements[0].text_run.content).toBe("graph LR\n  X-->Y")
    expect(src.code.style.language).toBe(1) // 图表语言无对应枚举，落 PlainText
  })
})

describe("stripTableMergeInfo", () => {
  test("删除表格块只读字段 merge_info", () => {
    const block = { block_type: 31, table: { property: { row_size: 1, column_size: 1, merge_info: [{ row_span: 2 }] } } }
    const out = stripTableMergeInfo(block) as { block_type: number; table?: { property?: Record<string, unknown> } }
    expect(out.table?.property?.merge_info).toBeUndefined()
    expect(out.table?.property?.row_size).toBe(1)
  })
  test("非表格块原样返回", () => {
    const block = { block_type: 2, text: { elements: [] } }
    expect(stripTableMergeInfo(block)).toBe(block)
  })
})

describe("normalizeBlockFields 块字段自动映射", () => {
  test("text 字段按 block_type 映射为驼峰字段", () => {
    const el = { elements: [{ text_run: { content: "hi" } }] }
    expect(normalizeBlockFields({ block_type: 3, text: el })).toEqual({ block_type: 3, heading1: el })
    expect(normalizeBlockFields({ block_type: 12, text: el })).toEqual({ block_type: 12, bullet: el })
    expect(normalizeBlockFields({ block_type: 13, text: el })).toEqual({ block_type: 13, ordered: el })
    expect(normalizeBlockFields({ block_type: 15, text: el })).toEqual({ block_type: 15, quote: el })
    expect(normalizeBlockFields({ block_type: 17, text: el })).toEqual({ block_type: 17, todo: el })
    // equation（16）不可创建：不做字段映射（add_blocks 前置拦截报错）
    expect(normalizeBlockFields({ block_type: 16, text: el })).toEqual({ block_type: 16, text: el })
    // text 类型不改写
    expect(normalizeBlockFields({ block_type: 2, text: el })).toEqual({ block_type: 2, text: el })
  })
  test("已有驼峰字段不覆盖；不修改入参", () => {
    const block = { block_type: 3, heading1: { elements: [] }, text: { elements: [{ text_run: { content: "x" } }] } }
    const out = normalizeBlockFields(block)
    expect(out).toEqual({ block_type: 3, heading1: { elements: [] }, text: { elements: [{ text_run: { content: "x" } }] } })
    expect(block).toEqual({ block_type: 3, heading1: { elements: [] }, text: { elements: [{ text_run: { content: "x" } }] } })
  })
  test("code 块 language 字符串转数字枚举且不污染入参", () => {
    const input = { block_type: 14, code: { style: { language: "python" }, elements: [] } }
    const out = normalizeBlockFields(input) as { code: { style: { language: number } } }
    expect(out.code.style.language).toBe(49)
    // 深拷贝：入参不被改写
    expect((input.code as { style: { language: unknown } }).style.language).toBe("python")
  })
  test("code 块 text 字段映射为 code 字段；js 语言转枚举；缺 style 补默认", () => {
    const el = { elements: [{ text_run: { content: "x" } }] }
    expect(normalizeBlockFields({ block_type: 14, text: el })).toEqual({ block_type: 14, code: { ...el, style: { language: 1 } } })
    expect(normalizeBlockFields({ block_type: 14, code: { style: { language: "js" }, elements: [] } })).toEqual({
      block_type: 14,
      code: { style: { language: 30 }, elements: [] },
    })
  })
  test("divider 块自动补 divider 字段（实测缺字段 invalid param）", () => {
    expect(normalizeBlockFields({ block_type: 22 })).toEqual({ block_type: 22, divider: {} })
    expect(normalizeBlockFields({ block_type: 22, divider: { a: 1 } })).toEqual({ block_type: 22, divider: { a: 1 } })
  })
  test("code 块缺 style/language 补默认（实测缺字段 field validation failed）", () => {
    // 仅 elements（无 style）：补 style.language=1（PlainText）
    expect(normalizeBlockFields({ block_type: 14, code: { elements: [] } })).toEqual({
      block_type: 14,
      code: { elements: [], style: { language: 1 } },
    })
    // text 快捷写法：映射 code 字段 + 补默认 style
    const out = normalizeBlockFields({ block_type: 14, text: "x" }) as { code: { style: { language: number } } }
    expect(out.code.style.language).toBe(1)
    // 显式 language 字符串：转枚举且保留
    expect(normalizeBlockFields({ block_type: 14, code: { elements: [], style: { language: "python" } } })).toEqual({
      block_type: 14,
      code: { elements: [], style: { language: 49 } },
    })
  })
  test("widthRatioWeights：小数权重按比例换算为整数（接口只接受整数 width_ratio）", () => {
    expect(widthRatioWeights([1, 1])).toEqual([1, 1])
    expect(widthRatioWeights([6, 4])).toEqual([6, 4])
    expect(widthRatioWeights([0.5, 0.5])).toEqual([1, 1])
    expect(widthRatioWeights([0.6, 0.4])).toEqual([3, 2])
    expect(widthRatioWeights([0.7, 0.2, 0.1])).toEqual([7, 2, 1])
    expect(widthRatioWeights([0.25, 0.75])).toEqual([1, 3])
    // 非正数兜底：仍返回正整数权重（保证请求合法）
    expect(widthRatioWeights([0, 1]).every((v) => Number.isInteger(v) && v > 0)).toBe(true)
  })

  test("prepareGridColumns：每列补整数 width_ratio 与空子块（实测缺一即报 1770041 / 9499）", () => {
    const withContent = {
      block_type: 24,
      grid: { column_size: 2 },
      children: [
        { block_type: 25, grid_column: { width_ratio: 0.6 }, children: [{ block_type: 2, text: "内容" }] },
        { block_type: 25, grid_column: {} },
      ],
    }
    const r = prepareGridColumns(withContent)
    const cols = r.block.children as Array<Record<string, unknown>>
    expect(r.filledColumns).toBe(1)
    // 有内容的列：小数权重换算为整数、子块原样保留
    expect(cols[0].grid_column).toEqual({ width_ratio: 3 })
    expect(cols[0].children as unknown[]).toHaveLength(1)
    // 空列：补权重（与其他列保持相对比例：0.6 : 缺省 1 → 3 : 5）与一个空文本子块
    expect(cols[1].grid_column).toEqual({ width_ratio: 5 })
    expect((cols[1].children as Array<Record<string, unknown>>)[0]).toMatchObject({ block_type: 2 })
    // 不污染入参；非 grid 原样返回
    expect(withContent.children[1]).toEqual({ block_type: 25, grid_column: {} })
    expect(prepareGridColumns({ block_type: 2, text: "x" }).filledColumns).toBe(0)
  })

  test("gridStructureError：column_size 必须 2~5 且与 grid_column 子块数一致", () => {
    expect(gridStructureError({ block_type: 24, grid: { column_size: 2 }, children: [{ block_type: 25, grid_column: {} }, { block_type: 25, grid_column: {} }] })).toBeNull()
    expect(gridStructureError({ block_type: 24, grid: {} })).toContain("2~5")
    expect(gridStructureError({ block_type: 24, grid: { column_size: 6 }, children: [] })).toContain("2~5")
    expect(gridStructureError({ block_type: 24, grid: { column_size: 3 }, children: [{ block_type: 25, grid_column: {} }] })).toContain("不一致")
    expect(gridStructureError({ block_type: 2, text: "x" })).toBeNull()
  })
  test("callout：正文落 children 子块、颜色/emoji 归一为 callout 顶层字段（实测）", () => {
    // 实测：callout 正文在 children 子块（callout.elements 被服务端忽略），
    // 颜色/emoji 为 callout 顶层字段（callout.style 包裹被忽略），且至少一个子块（缺则 1770041）
    const out1 = normalizeBlockFields({ block_type: 19, text: "注意" }) as { block_type: number; callout: Record<string, unknown>; children: Array<Record<string, unknown>> }
    expect(out1.block_type).toBe(19)
    expect(out1.callout).toEqual({})
    expect(out1.children).toHaveLength(1)
    expect(out1.children[0]).toEqual({ block_type: 2, text: { elements: [{ text_run: { content: "注意" } }] } })
    // 显式 callout 对象 + text：text 转子块，颜色/emoji 留在 callout 顶层
    expect(normalizeBlockFields({ block_type: 19, callout: { background_color: 3, emoji_id: "bulb" }, text: "补充" })).toEqual({
      block_type: 19,
      callout: { background_color: 3, emoji_id: "bulb" },
      children: [{ block_type: 2, text: { elements: [{ text_run: { content: "补充" } }] } }],
    })
    // callout.style 包裹与块顶层误放的颜色/emoji 均收敛为 callout 顶层
    expect(normalizeBlockFields({ block_type: 19, callout: { style: { background_color: 5 } } })).toEqual({
      block_type: 19,
      callout: { background_color: 5 },
      children: [{ block_type: 2, text: { elements: [{ text_run: { content: "" } }] } }],
    })
    expect(normalizeBlockFields({ block_type: 19, background_color: 1, text: "x" })).toEqual({
      block_type: 19,
      callout: { background_color: 1 },
      children: [{ block_type: 2, text: { elements: [{ text_run: { content: "x" } }] } }],
    })
    // 旧写法 callout.elements → 转子块（不丢内容）
    const out2 = normalizeBlockFields({ block_type: 19, callout: { elements: [{ text_run: { content: "正文" } }] } }) as { callout: Record<string, unknown>; children: Array<Record<string, unknown>> }
    expect(out2.callout).toEqual({})
    expect(out2.children[0]).toEqual({ block_type: 2, text: { elements: [{ text_run: { content: "正文" } }] } })
    expect(normalizeBlockFields({ block_type: 19, callout: { elements: "正文" } }) ).toMatchObject({ children: [{ block_type: 2, text: { elements: [{ text_run: { content: "正文" } }] } }] })
    // 显式块对象子块保留（递归构建由 buildGroup 处理），callout.elements 内容追加为子块
    expect(normalizeBlockFields({ block_type: 19, callout: { elements: [{ text_run: { content: "追加" } }] }, children: [{ block_type: 2, text: "已存" }] })).toEqual({
      block_type: 19,
      callout: {},
      children: [
        { block_type: 2, text: "已存" },
        { block_type: 2, text: { elements: [{ text_run: { content: "追加" } }] } },
      ],
    })
    // 不污染入参
    const input = { block_type: 19, text: "注意" }
    normalizeBlockFields(input)
    expect(input).toEqual({ block_type: 19, text: "注意" })
  })
  test("add_blocks 简化写法自动规范化（修复 99992402）", () => {
    // 缺 block_type：默认 text(2)
    expect(normalizeBlockFields({ text: "hi" })).toEqual({ block_type: 2, text: { elements: [{ text_run: { content: "hi" } }] } })
    // 字段值为字符串：自动包装为元素数组
    expect(normalizeBlockFields({ block_type: 2, text: "hello" })).toEqual({ block_type: 2, text: { elements: [{ text_run: { content: "hello" } }] } })
    expect(normalizeBlockFields({ block_type: 12, bullet: "项" })).toEqual({ block_type: 12, bullet: { elements: [{ text_run: { content: "项" } }] } })
    expect(normalizeBlockFields({ block_type: 3, heading1: "标题" })).toEqual({ block_type: 3, heading1: { elements: [{ text_run: { content: "标题" } }] } })
    // text → heading 映射后来源为字符串：同样包装（heading 分支回归）
    expect(normalizeBlockFields({ block_type: 3, text: "标题" })).toEqual({ block_type: 3, heading1: { elements: [{ text_run: { content: "标题" } }] } })
    // 顶层 elements：自动包装为 text.elements
    expect(normalizeBlockFields({ block_type: 2, elements: [{ text_run: { content: "x" } }] })).toEqual({
      block_type: 2,
      text: { elements: [{ text_run: { content: "x" } }] },
    })
    // 完整结构原样保留
    const full = { block_type: 2, text: { elements: [{ text_run: { content: "ok" } }] } }
    expect(normalizeBlockFields(full)).toEqual(full)
  })

  test("todo 块显式 done 对象归一：done → todo.style.done，text 合并进 todo.elements（实测 99992402 修复）", () => {
    // done 顶层 → style.done；text 快捷参数合并进 elements；无多余 text 字段
    const out = normalizeBlockFields({ block_type: 17, todo: { done: true }, text: "任务" })
    expect(out).toEqual({ block_type: 17, todo: { elements: [{ text_run: { content: "任务" } }], style: { done: true } } })
    expect(out.text).toBeUndefined()
    // style 已含 done 时直接采用（顶层 done 不覆盖）
    expect(normalizeBlockFields({ block_type: 17, todo: { done: false, style: { done: true }, elements: [{ text_run: { content: "x" } }] } })).toEqual({
      block_type: 17,
      todo: { elements: [{ text_run: { content: "x" } }], style: { done: true } },
    })
    // 显式 todo.elements + style 完整结构原样保留
    const full = { block_type: 17, todo: { elements: [{ text_run: { content: "任务" } }], style: { done: true } } }
    expect(normalizeBlockFields(full)).toEqual(full)
  })

  test("非文本容器类型（table 等）不再被强制转成 text 字段（实测修复 invalid param）", () => {
    // table 块：保留 table 字段，顶层 elements/text 不注入 text 字段
    const table = { block_type: 31, table: { property: { row_size: 1, column_size: 1 } }, elements: [{ text_run: { content: "x" } }] }
    const out = normalizeBlockFields(table)
    expect(out.block_type).toBe(31)
    expect(out.table).toBeDefined()
    expect(out.text).toBeUndefined() // 不再注入 text 字段
    expect(out.elements).toEqual(table.elements) // elements 原样保留，不再被消费转 text
    // 纯 text 类型（2）的 elements 兜底不受影响
    expect(normalizeBlockFields({ block_type: 2, elements: [{ text_run: { content: "x" } }] }).text).toBeDefined()
  })
})

describe("expandAppendRange 追加 range 自动扩展", () => {
  test("单格扩展为覆盖 values 全尺寸的区域", () => {
    expect(expandAppendRange("Sheet1!A1", 3, 2)).toBe("Sheet1!A1:B3")
    expect(expandAppendRange("Sheet1!C5", 2, 3)).toBe("Sheet1!C5:E6")
    // 跨 Z 边界
    expect(expandAppendRange("Sheet1!Z1", 2, 2)).toBe("Sheet1!Z1:AA2")
  })
  test("已是区域或非单格形式原样返回", () => {
    expect(expandAppendRange("Sheet1!A1:C3", 5, 2)).toBe("Sheet1!A1:C3")
    expect(expandAppendRange("Sheet1!A1:B", 5, 2)).toBe("Sheet1!A1:B")
    expect(expandAppendRange("A1", 5, 2)).toBe("A1")
  })
})

/* ================= 工具（mock fetch） ================= */

/* ================= 目标文件夹配置 ================= */

/** 目标文件夹相关用例的 handler：token + 多维表格带 app_token（其余返回空 data）。 */
function folderHandler(req: Req): Response {
  if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
  if (req.url.endsWith("/open-apis/bitable/v1/apps")) return jsonResponse({ code: 0, msg: "ok", data: { app: { app_token: "bascn1", default_table_id: "tbl1" } } })
  return jsonResponse({ code: 0, msg: "success", data: {} })
}

/** 带目标文件夹配置的环境（子Agent 前缀）。 */
function folderEnv(folderUrl: string): Record<string, string> {
  return { FEISHU_DOCS_APP_ID: "cli_test_app", FEISHU_DOCS_APP_SECRET: "secret_test", FEISHU_DOCS_FOLDER_URL: folderUrl }
}

describe("目标文件夹配置", () => {
  test("parseFolderToken 解析文件夹 URL 与裸 token", () => {
    expect(parseFolderToken("https://xxx.feishu.cn/drive/folder/fldcnAbc123")).toBe("fldcnAbc123")
    expect(parseFolderToken("https://xxx.feishu.cn/drive/folder/fldcnAbc123?from=space")).toBe("fldcnAbc123")
    expect(parseFolderToken("https://xxx.feishu.cn/folder/fldcnAbc123#top")).toBe("fldcnAbc123")
    expect(parseFolderToken("  fldcnAbc123  ")).toBe("fldcnAbc123")
    expect(parseFolderToken("https://xxx.feishu.cn/wiki/wikcnAbc")).toBeNull()
    expect(parseFolderToken("https://xxx.feishu.cn/drive/folder/")).toBeNull()
    expect(parseFolderToken("   ")).toBeNull()
  })

  test("配置文件夹时 create_doc 缺省落位到该文件夹并附落位说明", async () => {
    const { tools, records } = makeTools(folderHandler)
    const c = ctx({ env: folderEnv("https://xxx.feishu.cn/drive/folder/fldUserFolder") })
    const result = await tools.create_doc.execute({ title: "测试" }, c)
    const createReq = records.find((r) => r.url.endsWith("/open-apis/docx/v1/documents"))
    expect(JSON.parse(String(createReq!.init?.body))).toEqual({ title: "测试", folder_token: "fldUserFolder" })
    expect(result.output).toContain("fldUserFolder")
  })

  test("显式 folder_token 优先于配置的目标文件夹", async () => {
    const { tools, records } = makeTools(folderHandler)
    const c = ctx({ env: folderEnv("fldUserFolder") })
    await tools.create_doc.execute({ title: "测试", folder_token: "fldExplicit" }, c)
    const createReq = records.find((r) => r.url.endsWith("/open-apis/docx/v1/documents"))
    expect(JSON.parse(String(createReq!.init?.body))).toEqual({ title: "测试", folder_token: "fldExplicit" })
  })

  test("未配置文件夹时创建在应用云空间并附配置引导", async () => {
    const { tools, records } = makeTools(folderHandler)
    const result = await tools.create_doc.execute({ title: "测试" }, ctx())
    const createReq = records.find((r) => r.url.endsWith("/open-apis/docx/v1/documents"))
    expect(JSON.parse(String(createReq!.init?.body))).toEqual({ title: "测试" })
    expect(result.output).toContain("未配置目标文件夹")
    expect(result.output).toContain("FEISHU_DOCS_FOLDER_URL")
  })

  test("表格/多维表格/文件夹/上传缺省落位到配置文件夹", async () => {
    const { tools, records } = makeTools(folderHandler)
    const c = ctx({ env: folderEnv("fldUserFolder") })
    await tools.create_sheet.execute({ title: "表格" }, c)
    await tools.create_bitable.execute({ name: "多维" }, c)
    await tools.create_folder.execute({ name: "子目录" }, c)
    await tools.upload_file.execute({ file_name: "a.txt", content: "hi" }, c)
    const sheet = records.find((r) => r.url.endsWith("/open-apis/sheets/v3/spreadsheets"))
    expect((JSON.parse(String(sheet!.init?.body)) as { folder_token?: string }).folder_token).toBe("fldUserFolder")
    const bitable = records.find((r) => r.url.endsWith("/open-apis/bitable/v1/apps") && r.init?.method === "POST")
    expect((JSON.parse(String(bitable!.init?.body)) as { folder_token?: string }).folder_token).toBe("fldUserFolder")
    const folder = records.find((r) => r.url.endsWith("/open-apis/drive/v1/files/create_folder"))
    expect((JSON.parse(String(folder!.init?.body)) as { folder_token?: string }).folder_token).toBe("fldUserFolder")
    const upload = records.find((r) => r.url.endsWith("/open-apis/drive/v1/files/upload_all"))
    expect((upload!.init?.body as FormData).get("parent_node")).toBe("fldUserFolder")
  })

  test("配置值无法识别时报错提示", async () => {
    const { tools } = makeTools(folderHandler)
    const c = ctx({ env: folderEnv("https://xxx.feishu.cn/wiki/wikcnAbc") })
    const result = await tools.create_doc.execute({ title: "测试" }, c)
    expect(result.output).toContain("目标文件夹配置无效")
  })

  test("全局 GEBAI_FEISHU_FOLDER_URL 兜底", async () => {
    const { tools, records } = makeTools(folderHandler)
    const c = ctx({ env: { FEISHU_DOCS_APP_ID: "cli_test_app", FEISHU_DOCS_APP_SECRET: "secret_test", GEBAI_FEISHU_FOLDER_URL: "fldGlobal" } })
    await tools.create_doc.execute({ title: "测试" }, c)
    const createReq = records.find((r) => r.url.endsWith("/open-apis/docx/v1/documents"))
    expect((JSON.parse(String(createReq!.init?.body)) as { folder_token?: string }).folder_token).toBe("fldGlobal")
  })

  test("add_permission 权限受限时附文件夹方案引导", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/permissions/")) return jsonResponse({ code: 99991672, msg: "no permission" }, 403)
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.add_permission.execute({ token: "d1", type: "docx", member_type: "email", member_id: "a@b.com", perm: "edit" }, ctx())
    expect(result.output).toContain("99991672")
    expect(result.output).toContain("文件夹方案")
  })

  test("envVars 声明目标文件夹变量", () => {
    expect(feishuDef.envVars?.map((v) => v.name)).toContain("FEISHU_DOCS_FOLDER_URL")
  })
})

describe("认证与请求", () => {
  test("create_doc 发送正确请求并携带 token", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/open-apis/docx/v1/documents") && req.init?.method === "POST") {
        return jsonResponse({ code: 0, msg: "success", data: { document: { document_id: "doxcn1", title: "测试", url: "https://feishu.cn/docx/doxcn1" } } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.create_doc.execute({ title: "测试", folder_token: "fld1" }, ctx())
    expect(result.output).toContain("doxcn1")
    const tokenReq = records.find((r) => r.url.includes("/auth/v3/tenant_access_token"))
    expect(tokenReq).toBeTruthy()
    expect(JSON.parse(String(tokenReq!.init?.body))).toEqual({ app_id: "cli_test_app", app_secret: "secret_test" })
    const createReq = records.find((r) => r.url.endsWith("/open-apis/docx/v1/documents"))
    expect(createReq!.init?.method).toBe("POST")
    expect((createReq!.init?.headers as Record<string, string>).Authorization).toBe("Bearer t-abc")
    expect(JSON.parse(String(createReq!.init?.body))).toEqual({ title: "测试", folder_token: "fld1" })
  })

  test("token 缓存：两次调用只请求一次 token", async () => {
    let tokenCalls = 0
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) {
        tokenCalls++
        return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-cached", expire: 7200 })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    await tools.get_doc_meta.execute({ document_id: "doxcn1" }, ctx())
    await tools.get_doc_meta.execute({ document_id: "doxcn1" }, ctx())
    expect(tokenCalls).toBe(1)
  })

  test("缺少凭证时报错提示", async () => {
    const { tools } = makeTools(defaultHandler)
    const c = ctx({ env: {} })
    const result = await tools.create_doc.execute({ title: "x" }, c)
    expect(result.output).toContain("FEISHU_DOCS_APP_ID")
  })

  test("全局 GEBAI_FEISHU_* 凭证兜底", async () => {
    const { tools, records } = makeTools(defaultHandler)
    const c = ctx({ env: { GEBAI_FEISHU_APP_ID: "cli_global", GEBAI_FEISHU_APP_SECRET: "secret_global" } })
    await tools.get_doc_meta.execute({ document_id: "doxcn1" }, c)
    const tokenReq = records.find((r) => r.url.includes("/auth/v3/tenant_access_token"))
    expect(tokenReq).toBeTruthy()
    expect(JSON.parse(String(tokenReq!.init?.body))).toEqual({ app_id: "cli_global", app_secret: "secret_global" })
  })

  test("子Agent 前缀优先于全局凭证", async () => {
    const { tools, records } = makeTools(defaultHandler)
    const c = ctx({ env: { FEISHU_DOCS_APP_ID: "cli_agent", FEISHU_DOCS_APP_SECRET: "secret_agent", GEBAI_FEISHU_APP_ID: "cli_global", GEBAI_FEISHU_APP_SECRET: "secret_global" } })
    await tools.get_doc_meta.execute({ document_id: "doxcn1" }, c)
    const tokenReq = records.find((r) => r.url.includes("/auth/v3/tenant_access_token"))
    expect(JSON.parse(String(tokenReq!.init?.body))).toEqual({ app_id: "cli_agent", app_secret: "secret_agent" })
  })

  test("业务错误码返回可读失败信息（含 method 与 path）", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 99991400, msg: "frequency limit" }, 400)
    })
    const result = await tools.get_doc_meta.execute({ document_id: "doxcn1" }, ctx())
    expect(result.output).toContain("99991400")
    expect(result.output).toContain("frequency limit")
    expect(result.output).toContain("GET")
    expect(result.output).toContain("/open-apis/docx/v1/documents/doxcn1")
  })

  test("1770001 附带参数不合法提示", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 1770001, msg: "invalid param" }, 400)
    })
    const result = await tools.get_doc_meta.execute({ document_id: "doxcn1" }, ctx())
    expect(result.output).toContain("1770001")
    expect(result.output).toContain("参数不合法")
  })

  test("auth_status 不输出密钥明文", async () => {
    const { tools } = makeTools(defaultHandler)
    const result = await tools.auth_status.execute({}, ctx())
    expect(result.output).toContain("✓")
    expect(result.output).not.toContain("secret_test")
  })
})

/* ================= 用户授权（user_access_token） ================= */

/** 标准 OAuth token 端点 mock：authorization_code 兑换返回可配置的 access_token。 */
function oauthHandler(accessToken = "uat-user-1", refreshToken = "urt-1", expiresIn = 7200, scopes = "docx:document offline_access"): (req: Req) => Response {
  return (req: Req) => {
    if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
    if (req.url.includes("/oauth/v3/token")) {
      const body = JSON.parse(String(req.init?.body)) as Record<string, unknown>
      if (body.grant_type === "refresh_token") {
        if (body.refresh_token === "urt-bad") return jsonResponse({ code: 20037, msg: "expired", data: {} }, 400)
        return jsonResponse({
          code: 0,
          access_token: "uat-refreshed",
          expires_in: 7200,
          refresh_token: "urt-2",
          refresh_token_expires_in: 604800,
          scope: scopes,
          token_type: "Bearer",
        })
      }
      return jsonResponse({
        code: 0,
        access_token: accessToken,
        expires_in: expiresIn,
        refresh_token: refreshToken,
        refresh_token_expires_in: 604800,
        scope: scopes,
        token_type: "Bearer",
      })
    }
    if (req.url.includes("/authen/v1/user_info")) {
      return jsonResponse({ code: 0, msg: "ok", data: { name: "张三", open_id: "ou_zhangsan" } })
    }
    if (req.url.endsWith("/open-apis/docx/v1/documents") && req.init?.method === "POST") {
      return jsonResponse({ code: 0, msg: "success", data: { document: { document_id: "doxcn_user1", title: "用户文档", url: "https://feishu.cn/docx/doxcn_user1" } } })
    }
    return jsonResponse({ code: 0, msg: "success", data: {} })
  }
}

describe("extractOAuthCode 纯函数", () => {
  test("整段回调地址提取 code（含 URL 编码）", () => {
    expect(extractOAuthCode("https://localhost:5173/?code=abc%2D123&state=s1")).toBe("abc-123")
    expect(extractOAuthCode("https://example.com/callback?code=CODE_xyz")).toBe("CODE_xyz")
    expect(extractOAuthCode("2Wd5g337vo5BZXUz-3W5KECsWUmIzJ_FJ1eFD59fD1AJIibIZljTu3OLK-HP_UI1")).toBe("2Wd5g337vo5BZXUz-3W5KECsWUmIzJ_FJ1eFD59fD1AJIibIZljTu3OLK-HP_UI1")
  })
})

describe("auth_user_authorize 授权链接", () => {
  test("生成带 client_id/scope/state/prompt 与默认回调地址的授权链接（不输出密钥）", async () => {
    const { tools } = makeTools(oauthHandler())
    const result = await tools.auth_user_authorize.execute({}, ctx())
    expect(result.output).toContain("https://accounts.feishu.cn/open-apis/authen/v1/authorize")
    expect(result.output).toContain("client_id=cli_test_app")
    expect(result.output).toContain("response_type=code")
    expect(result.output).toContain("prompt=consent")
    expect(result.output).toContain("state=")
    expect(result.output).toContain("docx:document")
    expect(result.output).toContain("offline_access")
    expect(result.output).not.toContain("secret_test")
    expect(result.output).toContain("回到会话说「已授权」")
    // 默认自动回调：localhost:{GEBAI_PORT|3000} + 内置回调端点
    expect(result.output).toContain("redirect_uri=http%3A%2F%2Flocalhost%3A3000%2Fapi%2Fv1%2Foauth%2Ffeishu%2Fcallback")
    expect(result.output).toContain("自动跳回歌白")
  })

  test("GEBAI_PUBLIC_URL 覆盖默认回调地址", async () => {
    const { tools } = makeTools(oauthHandler())
    const c = ctx({ env: { FEISHU_DOCS_APP_ID: "cli_test_app", FEISHU_DOCS_APP_SECRET: "secret_test", GEBAI_PUBLIC_URL: "https://example.com/gebai" } })
    const result = await tools.auth_user_authorize.execute({}, c)
    expect(result.output).toContain("redirect_uri=https%3A%2F%2Fexample.com%2Fgebai%2Fapi%2Fv1%2Foauth%2Ffeishu%2Fcallback")
  })

  test("自定义 scopes 与 redirect_uri（覆盖自动回调）", async () => {
    const { tools } = makeTools(oauthHandler())
    const result = await tools.auth_user_authorize.execute({ scopes: "docx:document drive:drive", redirect_uri: "http://localhost:5173" }, ctx())
    expect(result.output).toContain("redirect_uri=http%3A%2F%2Flocalhost%3A5173")
    expect(result.output).toContain("drive:drive")
    // 自定义回调时提示手动粘贴路径（不走自动回调提示）
    expect(result.output).not.toContain("自动跳回歌白")
  })

  test("非法 scopes 报错", async () => {
    const { tools } = makeTools(oauthHandler())
    const result = await tools.auth_user_authorize.execute({ scopes: "docx document," }, ctx())
    expect(result.output).toContain("scopes 格式非法")
  })
})

describe("auth_user_token 兑换", () => {
  test("授权码兑换并保存（输出不含令牌明文）", async () => {
    const { tools, records } = makeTools(oauthHandler())
    const result = await tools.auth_user_token.execute({ code: "2Wd5g337vo5BZXUz-3W5KECsWUmIzJ_FJ1eFD59fD1AJIibIZljTu3OLK-HP_UI1" }, ctx())
    expect(result.output).toContain("已配置 user_access_token")
    expect(result.output).toContain("张三")
    expect(result.output).toContain("docx:document")
    expect(result.output).not.toContain("uat-user-1")
    expect(result.output).not.toContain("urt-1")
    const tokenReq = records.find((r) => r.url.includes("/oauth/v3/token"))
    expect(tokenReq).toBeTruthy()
    const body = JSON.parse(String(tokenReq!.init?.body)) as Record<string, unknown>
    expect(body).toMatchObject({ grant_type: "authorization_code", client_id: "cli_test_app", client_secret: "secret_test", code: "2Wd5g337vo5BZXUz-3W5KECsWUmIzJ_FJ1eFD59fD1AJIibIZljTu3OLK-HP_UI1" })
  })

  test("传完整回调地址自动提取 code，state 匹配通过", async () => {
    const { tools } = makeTools(oauthHandler())
    const c = ctx()
    const auth = await tools.auth_user_authorize.execute({}, c)
    const state = auth.output!.match(/state=([a-f0-9]+)/)![1]
    const result = await tools.auth_user_token.execute({ code: `http://localhost:5173/?code=abc123&state=${state}` }, c)
    expect(result.output).toContain("已配置")
  })

  test("回调地址 state 不匹配时拒绝", async () => {
    const { tools } = makeTools(oauthHandler())
    const c = ctx()
    await tools.auth_user_authorize.execute({}, c)
    const result = await tools.auth_user_token.execute({ code: "http://localhost:5173/?code=abc123&state=wrongstate" }, c)
    expect(result.output).toContain("state 不匹配")
  })

  test("OAuth 错误码附可读提示（授权码已使用）", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/oauth/v3/token")) return jsonResponse({ code: 20065, msg: "used", data: {} }, 400)
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.auth_user_token.execute({ code: "abc" }, ctx())
    expect(result.output).toContain("20065")
    expect(result.output).toContain("一次性")
  })
})

describe("user_access_token 自动生效", () => {
  test("回调端点写回令牌后工具自动拾取（无缓存穿透）", async () => {
    const { tools, records, userTokens } = makeTools(oauthHandler())
    const c = ctx()
    // 先查询一次（缓存空）→ 模拟 REST 回调端点直接写回会话令牌文件 → 再次操作应自动拾取
    const before = await tools.auth_user_status.execute({}, c)
    expect(before.output).toContain("未配置")
    userTokens.set(`${c.home}|default|0123456789abcdef0123456789abcdef`, {
      accessToken: "uat-callback-1",
      expireAt: Date.now() + 7200_000,
      refreshToken: "urt-callback-1",
      refreshExpireAt: Date.now() + 604800_000,
      scopes: ["docx:document", "offline_access"],
      name: "李四",
      openId: "ou_lisi",
    })
    const result = await tools.create_doc.execute({ title: "回调创建" }, c)
    expect(result.output).toContain("doxcn_user1")
    const createReq = records.find((r) => r.url.endsWith("/open-apis/docx/v1/documents"))
    expect((createReq!.init?.headers as Record<string, string>).Authorization).toBe("Bearer uat-callback-1")
    const status = await tools.auth_user_status.execute({}, c)
    expect(status.output).toContain("李四")
  })

  test("配置后 create_doc 自动以用户身份调用（不请求 tenant token）", async () => {
    const { tools, records } = makeTools(oauthHandler())
    const c = ctx()
    await tools.auth_user_token.execute({ code: "code-ok" }, c)
    const result = await tools.create_doc.execute({ title: "用户文档" }, c)
    expect(result.output).toContain("doxcn_user1")
    const createReq = records.find((r) => r.url.endsWith("/open-apis/docx/v1/documents"))
    expect((createReq!.init?.headers as Record<string, string>).Authorization).toBe("Bearer uat-user-1")
    expect(records.some((r) => r.url.includes("/auth/v3/tenant_access_token"))).toBe(false)
  })

  test("access 过期自动用 refresh_token 刷新后续调", async () => {
    // expires_in=1 → 兑换即视为过期，下次调用触发刷新（refresh 返回 uat-refreshed）
    const { tools, records } = makeTools(oauthHandler("uat-old", "urt-1", 1))
    const c = ctx()
    await tools.auth_user_token.execute({ code: "code-ok" }, c)
    const result = await tools.create_doc.execute({ title: "t" }, c)
    expect(result.output).toContain("doxcn_user1")
    const refreshReq = records.find((r) => r.url.includes("/oauth/v3/token") && String(r.init?.body).includes("refresh_token"))
    expect(refreshReq).toBeTruthy()
    const body = JSON.parse(String(refreshReq!.init?.body)) as Record<string, unknown>
    expect(body).toMatchObject({ grant_type: "refresh_token", refresh_token: "urt-1" })
    const createReq = records.find((r) => r.url.endsWith("/open-apis/docx/v1/documents"))
    expect((createReq!.init?.headers as Record<string, string>).Authorization).toBe("Bearer uat-refreshed")
  })

  test("刷新失败（refresh_token 过期）回退应用身份继续", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/oauth/v3/token")) {
        const body = JSON.parse(String(req.init?.body)) as Record<string, unknown>
        if (body.grant_type === "refresh_token") return jsonResponse({ code: 20037, msg: "refresh expired", data: {} }, 400)
        return jsonResponse({ code: 0, access_token: "uat-old", expires_in: 1, refresh_token: "urt-1", refresh_token_expires_in: 604800, scope: "docx:document", token_type: "Bearer" })
      }
      if (req.url.endsWith("/open-apis/docx/v1/documents") && req.init?.method === "POST") {
        return jsonResponse({ code: 0, msg: "success", data: { document: { document_id: "doxcn_app1" } } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const c = ctx()
    await tools.auth_user_token.execute({ code: "code-ok" }, c)
    const result = await tools.create_doc.execute({ title: "t" }, c)
    expect(result.output).toContain("doxcn_app1")
    const createReq = records.find((r) => r.url.endsWith("/open-apis/docx/v1/documents"))
    expect((createReq!.init?.headers as Record<string, string>).Authorization).toBe("Bearer t-abc")
  })

  test("服务端判定用户令牌失效（99991663）时刷新一次重试", async () => {
    let docCalls = 0
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/oauth/v3/token")) {
        const body = JSON.parse(String(req.init?.body)) as Record<string, unknown>
        if (body.grant_type === "refresh_token") {
          return jsonResponse({ code: 0, access_token: "uat-refreshed", expires_in: 7200, refresh_token: "urt-2", refresh_token_expires_in: 604800, scope: "docx:document", token_type: "Bearer" })
        }
        return jsonResponse({ code: 0, access_token: "uat-user-1", expires_in: 7200, refresh_token: "urt-1", refresh_token_expires_in: 604800, scope: "docx:document", token_type: "Bearer" })
      }
      if (req.url.endsWith("/open-apis/docx/v1/documents") && req.init?.method === "POST") {
        docCalls++
        const auth = (req.init?.headers as Record<string, string>).Authorization
        if (docCalls === 1 && auth === "Bearer uat-user-1") return jsonResponse({ code: 99991663, msg: "token expired", data: {} }, 400)
        return jsonResponse({ code: 0, msg: "success", data: { document: { document_id: "doxcn_retry" } } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const c = ctx()
    await tools.auth_user_token.execute({ code: "code-ok" }, c)
    const result = await tools.create_doc.execute({ title: "t" }, c)
    expect(result.output).toContain("doxcn_retry")
    expect(docCalls).toBe(2)
    expect(records.some((r) => String(r.init?.body).includes('"grant_type":"refresh_token"'))).toBe(true)
  })

  test("99991679 用户令牌缺权限附缺失 scope 引导", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/oauth/v3/token")) {
        return jsonResponse({ code: 0, access_token: "uat-user-1", expires_in: 7200, refresh_token: "urt-1", refresh_token_expires_in: 604800, scope: "docx:document", token_type: "Bearer" })
      }
      if (req.url.endsWith("/docx/v1/documents/doxcn1")) {
        return jsonResponse(
          {
            code: 99991679,
            msg: "Unauthorized",
            error: { permission_violations: [{ subject: "wiki:wiki:read", type: "action_privilege_required" }] },
            data: {},
          },
          400,
        )
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const c = ctx()
    await tools.auth_user_token.execute({ code: "code-ok" }, c)
    const result = await tools.get_doc_meta.execute({ document_id: "doxcn1" }, c)
    expect(result.output).toContain("99991679")
    expect(result.output).toContain("wiki:wiki:read")
    expect(result.output).toContain("auth_user_authorize")
  })
})

describe("auth_user_status / auth_user_clear", () => {
  test("未配置时提示当前为应用身份并给配置流程", async () => {
    const { tools } = makeTools(oauthHandler())
    const result = await tools.auth_user_status.execute({}, ctx())
    expect(result.output).toContain("未配置 user_access_token")
    expect(result.output).toContain("auth_user_authorize")
  })

  test("配置后展示绑定用户/scope/有效期", async () => {
    const { tools } = makeTools(oauthHandler())
    const c = ctx()
    await tools.auth_user_token.execute({ code: "code-ok" }, c)
    const result = await tools.auth_user_status.execute({}, c)
    expect(result.output).toContain("已配置")
    expect(result.output).toContain("张三")
    expect(result.output).toContain("docx:document")
    expect(result.output).toContain("offline_access")
    expect(result.output).not.toContain("uat-user-1")
  })

  test("clear 后回退应用身份", async () => {
    const { tools, records } = makeTools(oauthHandler())
    const c = ctx()
    await tools.auth_user_token.execute({ code: "code-ok" }, c)
    const cleared = await tools.auth_user_clear.execute({}, c)
    expect(cleared.output).toContain("已清除")
    const status = await tools.auth_user_status.execute({}, c)
    expect(status.output).toContain("未配置")
    await tools.create_doc.execute({ title: "t" }, c)
    const createReq = records.find((r) => r.url.endsWith("/open-apis/docx/v1/documents"))
    expect((createReq!.init?.headers as Record<string, string>).Authorization).toBe("Bearer t-abc")
  })
})

describe("api_call 兜底", () => {
  test("拒绝非 /open-apis/ 路径", async () => {
    const { tools } = makeTools(defaultHandler)
    const result = await tools.api_call.execute({ method: "GET", path: "https://evil.com" }, ctx())
    expect(result.output).toContain("/open-apis/")
  })
  test("拒绝非法 method", async () => {
    const { tools } = makeTools(defaultHandler)
    const result = await tools.api_call.execute({ method: "TRACE", path: "/open-apis/docx/v1/documents" }, ctx())
    expect(result.output).toContain("TRACE")
  })
  test("正常调用携带 token", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 0, msg: "success", data: { items: [1] } })
    })
    const result = await tools.api_call.execute({ method: "GET", path: "/open-apis/docx/v1/documents/doxcn1/blocks", query: { page_size: 5 } }, ctx())
    expect(result.output).toContain("items")
    const req = records.find((r) => r.url.includes("/blocks"))
    expect(req!.url).toContain("page_size=5")
    expect((req!.init?.headers as Record<string, string>).Authorization).toBe("Bearer t-abc")
  })
})

describe("import_markdown", () => {
  test("新建文档 + 本地转换 + descendant 插入", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/open-apis/docx/v1/documents") && req.init?.method === "POST") {
        return jsonResponse({ code: 0, msg: "success", data: { document: { document_id: "doxcn_new", title: "t" } } })
      }
      if (req.url.includes("/blocks?page_size=1")) {
        return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      }
      if (req.url.includes("/descendant")) {
        return jsonResponse({ code: 0, msg: "success", data: { children: [] } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.import_markdown.execute({ title: "报告", content: "# 标题\n\n正文" }, ctx())
    expect(result.output).toContain("doxcn_new")
    const insert = records.find((r) => r.url.includes("/descendant"))
    expect(insert).toBeTruthy()
    const body = JSON.parse(String(insert!.init?.body)) as { children_id: string[]; descendants: Array<{ block_type: number }> }
    expect(body.children_id.length).toBe(2)
    expect(body.descendants.map((d) => d.block_type)).toEqual([3, 2])
  })

  test("追加到已有文档（document_id 指定，不新建）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.import_markdown.execute({ document_id: "doxcn_exist", content: "追加内容" }, ctx())
    expect(result.output).toContain("doxcn_exist")
    expect(records.some((r) => r.url.endsWith("/open-apis/docx/v1/documents") && r.init?.method === "POST")).toBe(false)
  })

  test("official 引擎调用官方 convert 并剥离 merge_info", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/blocks/convert")) {
        return jsonResponse({
          code: 0,
          msg: "success",
          data: {
            first_level_block_ids: ["root1"],
            blocks: [
              { block_id: "root1", parent_id: "", block_type: 2, text: { elements: [] } },
              { block_id: "tbl1", parent_id: "root1", block_type: 31, table: { property: { row_size: 1, column_size: 1, merge_info: [] } }, children: [] },
            ],
          },
        })
      }
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) {
        const body = JSON.parse(String(req.init?.body)) as { descendants: Array<{ block_type: number; table?: { property?: Record<string, unknown> } }> }
        const tbl = body.descendants.find((d) => d.block_type === 31)
        expect(tbl?.table?.property?.merge_info).toBeUndefined()
        return jsonResponse({ code: 0, msg: "success", data: {} })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.import_markdown.execute({ document_id: "doxcn_exist", content: "|a|\n|-|\n|b|", engine: "official" }, ctx())
    expect(result.output).toContain("✓")
    expect(records.some((r) => r.url.endsWith("/blocks/convert"))).toBe(true)
  })

  test("official 引擎转换失败（1770041）自动回退本地转换", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/blocks/convert")) {
        // B5：official 引擎对代码块+表格组合报 schema mismatch（1770041）
        return jsonResponse({ code: 1770041, msg: "schema mismatch", data: {} }, 400)
      }
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.import_markdown.execute({ document_id: "doxcn_exist", content: "```ts\nx\n```", engine: "official" }, ctx())
    expect(result.output).toContain("✓ 已导入 1 个顶层块")
    expect(result.output).toContain("已自动回退本地转换")
    // 回退后走 descendant 插入，且本地转换的代码块 language 为数字枚举
    const desc = records.find((r) => r.url.includes("/descendant"))
    expect(desc).toBeDefined()
    const body = JSON.parse(String(desc!.init?.body)) as { descendants: Array<{ block_type: number; code?: { style?: { language: number } } }> }
    const code = body.descendants.find((d) => d.block_type === 14)
    expect(code?.code?.style?.language).toBe(63) // ts → TypeScript（官方枚举）
  })
})

describe("add_blocks", () => {
  test("text 快捷参数与根块自动定位", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/children")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", text: "hello" }, ctx())
    expect(result.output).toContain("1 个块")
    const req = records.find((r) => r.url.includes("/children"))
    expect(req!.url).toContain("/blocks/page_root/children")
    const body = JSON.parse(String(req!.init?.body)) as { children: Array<{ block_type: number; text: { elements: unknown[] } }> }
    expect(body.children[0]).toMatchObject({ block_type: 2 })
  })

  test("含 todo 块时改走创建嵌套块接口（children 不支持 todo）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // B6：todo 块（17）经 children 接口创建报 99992402，须走 descendant；统一传 text 字段自动映射为 todo 驼峰字段
    const todo = { block_type: 17, text: { style: { done: false }, elements: [{ text_run: { content: "任务" } }] } }
    const mixed = { block_type: 2, text: { elements: [{ text_run: { content: "说明" } }] } }
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([todo, mixed]) }, ctx())
    expect(result.output).toContain("已添加 2 个顶层块")
    expect(result.output).toContain("嵌套块接口")
    const desc = records.find((r) => r.url.includes("/descendant"))
    expect(desc).toBeDefined()
    const body = JSON.parse(String(desc!.init?.body)) as { children_id: string[]; descendants: Array<{ block_id: string; block_type: number; todo?: unknown; text?: unknown }> }
    expect(body.descendants.map((d) => d.block_type)).toEqual([17, 2])
    expect(body.descendants.every((d) => typeof d.block_id === "string" && d.block_id.length > 0)).toBe(true)
    expect(body.descendants[0].todo).toBeDefined()
    expect(body.descendants[0].text).toBeUndefined()
    // descendant 接口不支持 index：请求体不携带（追加到末尾）
    expect("index" in body).toBe(false)
    // 无 todo 时仍走 children 接口（不受影响）
    const { tools: t2, records: r2 } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/children")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    await t2.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([{ block_type: 2, text: { elements: [] } }]) }, ctx())
    expect(r2.some((r) => r.url.includes("/children"))).toBe(true)
    expect(r2.some((r) => r.url.includes("/descendant"))).toBe(false)
  })

  test("todo 显式 done 对象：归一为 todo.style.done + text 合并进 elements", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // B6：todo 显式 done 对象（仅含 todo 未含 text）报 99992402——归一：done → todo.style.done，text 合并进 todo.elements，无多余 text 字段
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([{ block_type: 17, todo: { done: true }, text: "任务" }]) }, ctx())
    expect(result.output).toContain("走创建嵌套块接口")
    const desc = records.find((r) => r.url.includes("/descendant"))
    const body = JSON.parse(String(desc!.init?.body)) as { descendants: Array<{ block_type: number; todo?: { elements: Array<{ text_run: { content: string } }>; style: { done: boolean } }; text?: unknown }> }
    const todo = body.descendants.find((d) => d.block_type === 17)?.todo
    expect(todo).toEqual({ elements: [{ text_run: { content: "任务" } }], style: { done: true } })
    expect(body.descendants.find((d) => d.block_type === 17)?.text).toBeUndefined()
  })

  test("超过 50 块自动分批", async () => {
    let childCalls = 0
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/children")) {
        childCalls++
        return jsonResponse({ code: 0, msg: "success", data: {} })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const blocks = Array.from({ length: 120 }, (_, i) => ({ block_type: 2, text: { elements: [{ text_run: { content: `b${i}` } }] } }))
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify(blocks) }, ctx())
    expect(result.output).toContain("120 个块")
    expect(result.output).toContain("3 批")
    expect(childCalls).toBe(3)
  })

  test("grid_column 自动补 width_ratio 与空子块（实测缺一即报 1770041），列内容随分栏一次创建", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const grid = {
      block_type: 24,
      grid: { column_size: 2 },
      children: [
        { block_type: 25, grid_column: { width_ratio: 50 }, children: [{ block_type: 2, text: { elements: [{ text_run: { content: "左列" } }] } }] },
        { block_type: 25, grid_column: { width_ratio: 50 } },
      ],
    }
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([grid]) }, ctx())
    expect(result.output).toContain("走创建嵌套块接口")
    expect(result.output).toContain("分栏空列已补空段落")
    const desc = records.find((r) => r.url.includes("/descendant"))
    const body = JSON.parse(String(desc!.init?.body)) as { descendants: Array<{ block_id: string; block_type: number; children: string[]; grid_column?: Record<string, unknown>; text?: unknown }> }
    const cols = body.descendants.filter((d) => d.block_type === 25)
    expect(cols).toHaveLength(2)
    // width_ratio 原值保留（缺省时补 1——每列必须带，否则 1770041）
    expect(cols.map((c) => c.grid_column)).toEqual([{ width_ratio: 50 }, { width_ratio: 50 }])
    // 每列至少一个子块；列内内容随分栏一次创建（不再剥离）
    expect(cols.every((c) => c.children.length >= 1)).toBe(true)
    const texts = body.descendants.filter((d) => d.block_type === 2).map((d) => JSON.stringify(d.text))
    expect(texts.join()).toContain("左列")
  })

  test("grid 结构前置校验：column_size 与列数不一致 / equation 不可创建均报可读错误", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const mismatch = await tools.add_blocks.execute(
      { document_id: "doxcn1", blocks: [{ block_type: 24, grid: { column_size: 3 }, children: [{ block_type: 25, grid_column: {} }] }] },
      ctx(),
    )
    expect(mismatch.output).toContain("不一致")
    expect(records.some((r) => r.url.includes("/descendant"))).toBe(false)
    const equation = await tools.add_blocks.execute(
      { document_id: "doxcn1", blocks: [{ block_type: 16, equation: { elements: [{ text_run: { content: "E=mc^2" } }] } }] },
      ctx(),
    )
    expect(equation.output).toContain("不可通过 API 创建")
    // 嵌套子树中的 equation 同样被拦截
    const nested = await tools.add_blocks.execute(
      { document_id: "doxcn1", blocks: [{ block_type: 2, text: "x", children: [{ block_type: 16, text: "y" }] }] },
      ctx(),
    )
    expect(nested.output).toContain("不可通过 API 创建")
  })

  test("blocks 参数数组直传（非字符串，避免转义/截断——修复「不是合法 JSON」根因）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/children")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // 数组参数：LLM 工具调用直接生成对象数组（无字符串转义层）
    const result = await tools.add_blocks.execute(
      { document_id: "doxcn1", blocks: [{ block_type: 2, text: { elements: [{ text_run: { content: "数组直传" } }] } }] },
      ctx(),
    )
    expect(result.output).toContain("1 个块")
    const body = JSON.parse(String(records.find((r) => r.url.includes("/children"))!.init?.body)) as { children: unknown[] }
    expect(body.children[0]).toMatchObject({ block_type: 2 })
  })

  test("长 JSON 截断报错带分批/简化写法引导", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: '{"block_type":2,"text":{"elements":[{text_run' }, ctx())
    expect(result.output).toContain("不是合法 JSON")
    expect(result.output).toContain("分批提交") // 截断引导
  })

  test("表格简化写法 table.rows 一次走嵌套块接口创建完整表格（不再逐格 N 次调用）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const table = { block_type: 31, table: { rows: [["列A", "列B"], ["a1", "b1"]] } }
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([table]) }, ctx())
    expect(result.output).toContain("已添加 1 个顶层块")
    const desc = records.find((r) => r.url.includes("/descendant"))
    expect(desc).toBeDefined()
    const body = JSON.parse(String(desc!.init?.body)) as {
      children_id: string[]
      descendants: Array<{ block_id: string; block_type: number; children?: string[]; table?: { property?: { row_size: number; column_size: number; column_width?: number[] } }; table_cell?: unknown; text?: unknown }>
    }
    // 单次请求包含 table + 2 行 × 2 列 table_cell + 4 个 text（一次创建完整表格）
    const types = body.descendants.map((d) => d.block_type)
    expect(types).toEqual([2, 32, 2, 32, 2, 32, 2, 32, 31])
    expect(body.descendants[8].table?.property).toEqual({ row_size: 2, column_size: 2, column_width: [365, 365] })
    // table 块 children 引用 4 个 cell；cell 的 children 引用其内 text
    expect(body.descendants[8].children).toHaveLength(4)
    expect(body.descendants[1].children).toEqual([body.descendants[0].block_id])
    expect(body.descendants.every((d) => typeof d.block_id === "string" && d.block_id.length > 0)).toBe(true)
    // 只调了一次写入接口
    expect(records.filter((r) => r.url.includes("/descendant"))).toHaveLength(1)
  })

  test("嵌套 children 结构自动走嵌套块接口（递归补 block_id 引用）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // 模型从 find_blocks 复制的块 JSON 可能带真实 block_id——应强制重生成（防引用冲突）
    const nested = {
      block_id: "old_root",
      block_type: 2,
      text: { elements: [{ text_run: { content: "容器" } }] },
      children: [{ block_type: 2, text: "子内容" }],
    }
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([nested]) }, ctx())
    expect(result.output).toContain("走创建嵌套块接口")
    const desc = records.find((r) => r.url.includes("/descendant"))
    const body = JSON.parse(String(desc!.init?.body)) as { children_id: string[]; descendants: Array<{ block_id: string; block_type: number; children?: string[]; text?: unknown }> }
    expect(body.descendants.map((d) => d.block_type)).toEqual([2, 2])
    // 旧 block_id 被强制替换为全局唯一新 id；顶层块 children 引用新生成的子块 id
    expect(body.descendants[1].block_id).not.toBe("old_root")
    expect(body.descendants[1].children).toEqual([body.descendants[0].block_id])
    // 子块简化写法（text 字符串）也被自动映射为 text.elements
    expect(body.descendants[0].text).toEqual({ elements: [{ text_run: { content: "子内容" } }] })
    expect(body.children_id).toEqual([body.descendants[1].block_id])
  })

  test("callout/grid/grid_column 正确枚举（19/24/25）走 descendant 接口创建", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // callout（实测）：callout=19、grid=24、grid_column=25（原 40/33/34 有误）；容器块不能经 children 接口创建，须走 descendant；
    // callout 正文在 children 子块（elements 被忽略）、颜色/emoji 为 callout 顶层字段、至少一个子块（缺则 1770041）；
    // grid_column 带 children 报 field validation failed、width_ratio 报 9499（均为旧实现误判：正确写法是**列带 width_ratio 且非空**）
    const blocks = [
      { block_type: 19, callout: { background_color: 3, elements: [{ text_run: { content: "提示内容" } }] } },
      {
        block_type: 24,
        grid: { column_size: 2 },
        children: [
          { block_type: 25, grid_column: { width_ratio: 50 }, children: [{ block_type: 2, text: "左" }] },
          { block_type: 25, grid_column: { width_ratio: 50 }, children: [{ block_type: 2, text: "右" }] },
        ],
      },
    ]
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify(blocks) }, ctx())
    expect(result.output).toContain("走创建嵌套块接口")
    expect(result.output).not.toContain("update_block 填充")
    const desc = records.find((r) => r.url.includes("/descendant"))
    const body = JSON.parse(String(desc!.init?.body)) as {
      children_id: string[]
      descendants: Array<{ block_id: string; block_type: number; children?: string[]; callout?: { background_color?: number; border_color?: number; emoji_id?: string }; grid?: { column_size: number }; grid_column?: Record<string, unknown>; text?: unknown }>
    }
    // 展开顺序：子块先于父块入组——callout 正文(2) → callout(19)；每列文本(2) → grid_column(25)；最后 grid(24)
    expect(body.descendants.map((d) => d.block_type)).toEqual([2, 19, 2, 25, 2, 25, 24])
    const callout = body.descendants.find((d) => d.block_type === 19)!
    // 实测：颜色/emoji 为 callout 顶层字段；正文在 children 子块（elements 被忽略）
    expect(callout.callout).toEqual({ background_color: 3 })
    expect(callout.children).toHaveLength(1)
    const calloutBody = body.descendants.find((d) => d.block_id === (callout.children as string[])[0])!
    expect(calloutBody.text).toEqual({ elements: [{ text_run: { content: "提示内容" } }] })
    const grid = body.descendants.find((d) => d.block_type === 24)!
    expect(grid.grid).toEqual({ column_size: 2 })
    expect(grid.children).toHaveLength(2)
    const cols = body.descendants.filter((d) => d.block_type === 25)
    expect(cols).toHaveLength(2)
    // 每列带 width_ratio 且至少一个子块（实测：缺一即报 1770041）
    expect(cols.every((c) => Number(c.grid_column?.width_ratio) > 0)).toBe(true)
    expect(cols.every((c) => (c.children?.length ?? 0) >= 1)).toBe(true)
    // 容器块不注入多余 text 字段
    expect(body.descendants.find((d) => d.block_type === 19)?.text).toBeUndefined()
    expect(body.descendants.find((d) => d.block_type === 24)?.text).toBeUndefined()
    expect(records.some((r) => r.url.includes("/descendant"))).toBe(true)
    expect(records.some((r) => r.url.includes("/children"))).toBe(false)
  })

  test("index 参数对嵌套块接口不生效（提示追加到末尾）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([{ block_type: 17, todo: { elements: [{ text_run: { content: "任务" } }] } }]), index: 2 }, ctx())
    expect(result.output).toContain("index 参数对嵌套块接口不生效")
    const desc = records.find((r) => r.url.includes("/descendant"))
    const body = JSON.parse(String(desc!.init?.body)) as { index?: number }
    expect("index" in body).toBe(false)
  })

  test("blocks 含 null 元素不抛错（归一为默认 text）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/children")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([null, { block_type: 2, text: "x" }]) }, ctx())
    expect(result.output).toContain("已添加 2 个块")
    expect(records.some((r) => r.url.includes("/children"))).toBe(true)
  })

  test("children 字符串 id 引用明确报错（防悬空引用）", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // tool() 包装把错误转为 output（❌ 前缀）；用 text 块作载体（callout 会剥离 children 不再触发）
    const bad = { block_type: 2, text: { elements: [] }, children: ["abc123"] }
    const r = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([bad]) }, ctx())
    expect(r.output).toContain("children 元素必须是块对象")
  })

  test("超大表格（单组超 1000 块）明确报错提示拆分", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // 100 行 × 5 列 = 500 单元格 → 1(table) + 500(cell) + 500(text) = 1001 块 > 1000
    const rows = Array.from({ length: 100 }, () => ["a", "b", "c", "d", "e"])
    const table = { block_type: 31, table: { rows } }
    const r = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([table]) }, ctx())
    expect(r.output).toContain("超过接口上限 1000")
  })

  test("table.rows 简化写法下发自适应列宽（显式覆盖生效、header_row 不默认打开）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const auto = { block_type: 31, table: { rows: [["序号", "说明"], ["1", "这是一段比较长的说明文字"]] } }
    const fixed = { block_type: 31, table: { rows: [["a", "b"]], column_width: [200, 300], header_row: true } }
    const r = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([auto, fixed]) }, ctx())
    expect(r.output).toContain("已添加 2 个顶层块")
    const body = JSON.parse(String(records.find((x) => x.url.includes("/descendant"))!.init?.body)) as { descendants: Array<{ block_type: number; table?: { property: Record<string, unknown> } }> }
    const props = body.descendants.filter((b) => b.block_type === 31).map((b) => b.table!.property)
    const autoWidths = props[0].column_width as number[]
    expect(autoWidths.reduce((a, b) => a + b, 0)).toBe(TABLE_PAGE_WIDTH)
    expect(autoWidths[1]).toBeGreaterThan(autoWidths[0])
    expect(props[0].header_row).toBeUndefined()
    expect(props[1]).toEqual({ row_size: 1, column_size: 2, column_width: [200, 300], header_row: true })
  })
})

describe("set_table_width", () => {
  const cell = (id: string, child: string) => ({ block_id: id, block_type: 32, table_cell: {}, children: [child] })
  const txt = (id: string, content: string) => ({ block_id: id, block_type: 2, text: { elements: [{ text_run: { content } }] } })
  const items = [
    { block_id: "tbl1", block_type: 31, table: { property: { row_size: 2, column_size: 2, column_width: [100, 100] } }, children: ["c1", "c2", "c3", "c4"] },
    cell("c1", "t1"),
    cell("c2", "t2"),
    cell("c3", "t3"),
    cell("c4", "t4"),
    txt("t1", "序号"),
    txt("t2", "说明"),
    txt("t3", "1"),
    txt("t4", "这是一段比较长的说明文字"),
  ]
  function handler(req: Req): Response {
    if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
    if (req.init?.method === "PATCH") return jsonResponse({ code: 0, msg: "success", data: {} })
    return jsonResponse({ code: 0, msg: "success", data: { items, has_more: false } })
  }
  const patchesOf = (records: Req[]) =>
    records.filter((x) => x.init?.method === "PATCH").map((x) => JSON.parse(String(x.init?.body)) as { update_table_property: Record<string, number | boolean> })

  test("缺省按内容自适应，逐列 PATCH update_table_property", async () => {
    const { tools, records } = makeTools(handler)
    const r = await tools.set_table_width.execute({ document_id: "doxcn1", block_id: "tbl1" }, ctx())
    expect(r.output).toContain("列宽已设为")
    const patches = patchesOf(records)
    expect(patches.map((p) => p.update_table_property.column_index)).toEqual([0, 1])
    const widths = patches.map((p) => Number(p.update_table_property.column_width))
    expect(widths.reduce((a, b) => a + b, 0)).toBe(TABLE_PAGE_WIDTH)
    expect(widths[1]).toBeGreaterThan(widths[0])
  })

  test("显式 columns 逐列下发 + header_row 单独提交", async () => {
    const { tools, records } = makeTools(handler)
    const r = await tools.set_table_width.execute({ document_id: "doxcn1", block_id: "tbl1", columns: [220, 510], header_row: true }, ctx())
    expect(r.output).toContain("总宽 730px")
    expect(patchesOf(records).map((p) => p.update_table_property)).toEqual([{ column_index: 0, column_width: 220 }, { column_index: 1, column_width: 510 }, { header_row: true }])
  })

  test("显式列宽长度不符 / 非表格块 / 块不存在均报错", async () => {
    const { tools, records } = makeTools(handler)
    expect((await tools.set_table_width.execute({ document_id: "doxcn1", block_id: "tbl1", columns: [200] }, ctx())).output).toContain("必须与表格列数")
    expect(patchesOf(records)).toHaveLength(0)
    const { tools: tools2 } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "blk", block_type: 2, text: { elements: [] } }], has_more: false } })
    })
    expect((await tools2.set_table_width.execute({ document_id: "doxcn1", block_id: "blk" }, ctx())).output).toContain("不是表格块")
    expect((await tools2.set_table_width.execute({ document_id: "doxcn1", block_id: "nope" }, ctx())).output).toContain("不存在块")
  })
})

/* ================= 错误诊断（docxCall 本地探测） ================= */

describe("docx 操作失败本地诊断", () => {
  test("add_blocks 失败时诊断出 block_id 不存在", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/documents/doxcn1")) return jsonResponse({ code: 0, msg: "success", data: { document: { document_id: "doxcn1" } } })
      if (req.url.includes("/blocks/doxcnBAD/children")) return jsonResponse({ code: 1770001, msg: "invalid param" }, 400)
      if (req.url.includes("/blocks/doxcnBAD")) return jsonResponse({ code: 1061001, msg: "block not found" }, 404)
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", block_id: "doxcnBAD", text: "hi" }, ctx())
    expect(result.output).toContain("1770001")
    expect(result.output).toContain("本地诊断")
    expect(result.output).toContain("不存在")
  })

  test("add_blocks 失败时诊断出叶子块不支持子块", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/documents/doxcn1")) return jsonResponse({ code: 0, msg: "success", data: {} })
      if (req.url.includes("/blocks/doxcnLEAF/children")) return jsonResponse({ code: 1770001, msg: "invalid param" }, 400)
      if (req.url.includes("/blocks/doxcnLEAF")) return jsonResponse({ code: 0, msg: "success", data: { block_id: "doxcnLEAF", block_type: 22 } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", block_id: "doxcnLEAF", text: "hi" }, ctx())
    expect(result.output).toContain("不支持子块")
    expect(result.output).toContain("divider")
  })

  test("document_id 无效时提示文档问题", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/documents/doxcnBAD")) return jsonResponse({ code: 1061002, msg: "document not found" }, 404)
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 1770001, msg: "invalid param" }, 400)
      if (req.url.includes("/children")) return jsonResponse({ code: 1770001, msg: "invalid param" }, 400)
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.add_blocks.execute({ document_id: "doxcnBAD", text: "hi" }, ctx())
    expect(result.output).toContain("本地诊断")
    expect(result.output).toContain("document_id")
  })
})

/* ================= find_blocks 按文本反查 ================= */

function blocksHandler(req: Req): Response {
  if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
  if (req.url.includes("page_token=p2")) {
    return jsonResponse({
      code: 0,
      msg: "success",
      data: { items: [{ block_id: "t1", block_type: 2, parent_id: "h1", children: [], text: { elements: [{ text_run: { content: "本周进度 50%" } }] } }], has_more: false },
    })
  }
  if (req.url.includes("/blocks?page_size=500")) {
    return jsonResponse({
      code: 0,
      msg: "success",
      data: {
        items: [
          { block_id: "page_root", block_type: 1, parent_id: "", children: ["h1"] },
          { block_id: "h1", block_type: 3, parent_id: "page_root", children: ["t1"], heading1: { elements: [{ text_run: { content: "周报概述" } }] } },
        ],
        has_more: true,
        page_token: "p2",
      },
    })
  }
  return jsonResponse({ code: 0, msg: "success", data: {} })
}

describe("find_blocks", () => {
  test("按标题文本反查 block_id（含类型标注与路径）", async () => {
    const { tools } = makeTools(blocksHandler)
    const result = await tools.find_blocks.execute({ document_id: "doxcn1", query: "周报" }, ctx())
    expect(result.output).toContain("h1")
    expect(result.output).toContain("heading1")
    expect(result.output).toContain("周报概述")
    expect(result.output).toContain("路径: 根")
  })

  test("匹配正文块并带父级路径", async () => {
    const { tools } = makeTools(blocksHandler)
    const result = await tools.find_blocks.execute({ document_id: "doxcn1", query: "进度" }, ctx())
    expect(result.output).toContain("t1")
    expect(result.output).toContain("周报概述")
  })

  test("block_type 过滤只匹配指定类型", async () => {
    const { tools } = makeTools(blocksHandler)
    const result = await tools.find_blocks.execute({ document_id: "doxcn1", query: "进度", block_type: "heading" }, ctx())
    expect(result.output).toContain("未找到")
    const heading = await tools.find_blocks.execute({ document_id: "doxcn1", query: "周报", block_type: "heading" }, ctx())
    expect(heading.output).toContain("h1")
  })

  test("非法 block_type 报错", async () => {
    const { tools } = makeTools(blocksHandler)
    const result = await tools.find_blocks.execute({ document_id: "doxcn1", query: "x", block_type: "bogus" }, ctx())
    expect(result.output).toContain("无法识别的块类型")
  })

  test("空 query 报错", async () => {
    const { tools } = makeTools(blocksHandler)
    const result = await tools.find_blocks.execute({ document_id: "doxcn1", query: "" }, ctx())
    expect(result.output).toContain("query 不能为空")
  })
})

/* ================= get_doc_text 小节读取 ================= */

describe("get_doc_text 小节读取", () => {
  test("block_id 指定时组合子树文本（标题层级）", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks/h1/descendant")) {
        return jsonResponse({
          code: 0,
          msg: "success",
          data: {
            items: [
              { block_id: "t1", block_type: 2, parent_id: "h1", children: [], text: { elements: [{ text_run: { content: "正文一" } }] } },
              { block_id: "t2", block_type: 2, parent_id: "h1", children: [], text: { elements: [{ text_run: { content: "正文二" } }] } },
            ],
            has_more: false,
          },
        })
      }
      if (req.url.endsWith("/blocks/h1")) {
        return jsonResponse({ code: 0, msg: "success", data: { block_id: "h1", block_type: 3, parent_id: "page_root", children: ["t1", "t2"], heading1: { elements: [{ text_run: { content: "周报概述" } }] } } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.get_doc_text.execute({ document_id: "doxcn1", block_id: "h1" }, ctx())
    expect(result.output).toContain("# 周报概述")
    expect(result.output).toContain("正文一")
    expect(result.output).toContain("正文二")
  })

  test("表格子树组合为行文本", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks/tbl/descendant")) {
        const cells = ["c1", "c2", "c3", "c4"].map((id, i) => ({ block_id: id, block_type: 32, parent_id: "tbl", children: [`tx${i + 1}`], table_cell: {} }))
        const texts = ["tx1", "tx2", "tx3", "tx4"].map((id, i) => ({ block_id: id, block_type: 2, parent_id: id === "tx1" ? "c1" : `c${i}`, children: [], text: { elements: [{ text_run: { content: ["a", "b", "c", "d"][i] } }] } }))
        return jsonResponse({ code: 0, msg: "success", data: { items: [...cells, ...texts], has_more: false } })
      }
      if (req.url.endsWith("/blocks/tbl")) {
        return jsonResponse({
          code: 0,
          msg: "success",
          data: { block_id: "tbl", block_type: 31, parent_id: "page_root", children: ["c1", "c2", "c3", "c4"], table: { property: { row_size: 2, column_size: 2 } } },
        })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.get_doc_text.execute({ document_id: "doxcn1", block_id: "tbl" }, ctx())
    expect(result.output).toContain("[表格]")
    expect(result.output).toContain("| a | b |")
    expect(result.output).toContain("| c | d |")
  })
})

/* ================= page_all 自动翻页 ================= */

describe("page_all 自动翻页", () => {
  test("get_doc_blocks page_all 汇总多页并标注 type_name", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("page_token=p2")) {
        return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "b2", block_type: 22, children: [] }], has_more: false } })
      }
      if (req.url.includes("/blocks?page_size=100")) {
        return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "b1", block_type: 2, children: [], text: { elements: [] } }], has_more: true, page_token: "p2" } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.get_doc_blocks.execute({ document_id: "doxcn1", page_all: true }, ctx())
    expect(result.output).toContain("b1")
    expect(result.output).toContain("b2")
    expect(result.output).toContain("divider")
    expect(result.output).toContain("type_name")
    expect(result.output).toContain("total")
  })

  test("list_blocks page_all 汇总子块", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("page_token=p2")) {
        return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "b2", block_type: 2, children: [], text: { elements: [] } }], has_more: false } })
      }
      if (req.url.includes("/blocks/page_root/children")) {
        return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "b1", block_type: 2, children: [], text: { elements: [] } }], has_more: true, page_token: "p2" } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.list_blocks.execute({ document_id: "doxcn1", page_all: true }, ctx())
    expect(result.output).toContain("b1")
    expect(result.output).toContain("b2")
    expect(result.output).toContain("全部子块")
  })
})

/* ================= 纯函数：块文本/类型标注 ================= */

describe("blockText / blockTypeName 纯函数", () => {
  test("blockText 提取各类块文本", () => {
    expect(blockText({ block_type: 3, heading1: { elements: [{ text_run: { content: "标题" } }] } })).toBe("标题")
    expect(blockText({ block_type: 2, text: { elements: [{ text_run: { content: "a" } }, { text_run: { content: "b" } }] } })).toBe("ab")
    expect(blockText({ block_type: 14, code: { elements: [{ text_run: { content: "const" } }] } })).toBe("const")
    expect(blockText({ block_type: 22, divider: {} })).toBe("")
    expect(blockText({ block_type: 31, table: {}, children: ["c1"] })).toBe("")
  })
  test("blockTypeName 标注类型", () => {
    expect(blockTypeName(3)).toBe("heading1")
    expect(blockTypeName(12)).toBe("bullet")
    expect(blockTypeName(31)).toBe("table")
    expect(blockTypeName(99)).toBe("未知(99)")
    // 实测修正：图片块为 27（原 43 误标 image——43 为 mindnote）；quote=15
    expect(blockTypeName(27)).toBe("image")
    expect(blockTypeName(43)).toBe("mindnote")
    expect(blockTypeName(15)).toBe("quote")
    // 实测修正：容器块枚举 19=callout、24=grid、25=grid_column；33=view、34=quote_container、40=add_ons
    expect(blockTypeName(19)).toBe("callout")
    expect(blockTypeName(24)).toBe("grid")
    expect(blockTypeName(25)).toBe("grid_column")
    expect(blockTypeName(33)).toBe("view")
    expect(blockTypeName(34)).toBe("quote_container")
    expect(blockTypeName(40)).toBe("add_ons")
  })
})

describe("delete_blocks", () => {
  test("发送 batch_delete 请求", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/batch_delete")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.delete_blocks.execute({ document_id: "doxcn1", start_index: 2, end_index: 4 }, ctx())
    expect(result.output).toContain("删除成功")
    const req = records.find((r) => r.url.includes("/batch_delete"))
    expect(req!.init?.method).toBe("DELETE")
    expect(JSON.parse(String(req!.init?.body))).toEqual({ start_index: 2, end_index: 4 })
  })

  test("单块删除：end_index 缺省/相同自动扩展为 start+1（半开区间）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/batch_delete")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // B6：end_index 与 start_index 相同 → 自动 end=start+1（API 要求 end > start）
    await tools.delete_blocks.execute({ document_id: "doxcn1", start_index: 2, end_index: 2 }, ctx())
    await tools.delete_blocks.execute({ document_id: "doxcn1", start_index: 5 }, ctx())
    const reqs = records.filter((r) => r.url.includes("/batch_delete"))
    expect(JSON.parse(String(reqs[0].init?.body))).toEqual({ start_index: 2, end_index: 3 })
    expect(JSON.parse(String(reqs[1].init?.body))).toEqual({ start_index: 5, end_index: 6 })
    // 反向区间：明确报错提示
    const bad = await tools.delete_blocks.execute({ document_id: "doxcn1", start_index: 4, end_index: 2 }, ctx())
    expect(bad.output).toContain("end_index（2）不能小于 start_index（4）")
  })
})

describe("read_sheet / write_sheet", () => {
  test("read_sheet 指定 range 走 v2 values 接口", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/values/")) return jsonResponse({ code: 0, msg: "success", data: { valueRange: { values: [["1", "2"]] } } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.read_sheet.execute({ spreadsheet_token: "sht1", range: "Sheet1!A1:B1" }, ctx())
    expect(result.output).toContain("1")
    const req = records.find((r) => r.url.includes("/values/"))
    expect(req!.url).toContain("valueRenderOption=ToString")
  })

  test("write_sheet 发送 valueRange", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.write_sheet.execute({ spreadsheet_token: "sht1", range: "Sheet1!A1", values: JSON.stringify([["x", "y"]]) }, ctx())
    expect(result.output).toContain("写入成功")
    const req = records.find((r) => r.url.endsWith("/values"))
    expect(JSON.parse(String(req!.init?.body))).toEqual({ valueRange: { range: "Sheet1!A1", values: [["x", "y"]] } })
  })

  test("write_sheet 非法 values 报错", async () => {
    const { tools } = makeTools(defaultHandler)
    const result = await tools.write_sheet.execute({ spreadsheet_token: "sht1", range: "Sheet1!A1", values: "not-json" }, ctx())
    expect(result.output).toContain("JSON")
  })
})

describe("bitable 记录操作", () => {
  test("add_bitable_records 自动包装 fields", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 0, msg: "success", data: { records: [] } })
    })
    const result = await tools.add_bitable_records.execute(
      { app_token: "app1", table_id: "tbl1", records: JSON.stringify([{ 姓名: "张三" }, { fields: { 姓名: "李四" } }]) },
      ctx(),
    )
    expect(result.output).toContain("新增成功")
    const req = records.find((r) => r.url.includes("/records/batch_create"))
    const body = JSON.parse(String(req!.init?.body)) as { records: unknown[] }
    expect(body.records).toEqual([{ fields: { 姓名: "张三" } }, { fields: { 姓名: "李四" } }])
  })
})

describe("export_doc / upload_file / download_file", () => {
  test("export_doc 创建任务并轮询到结果（查询只带 token query，实测多余参数报 field validation failed）", async () => {
    let ticketCalls = 0
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/export_tasks") && req.init?.method === "POST") return jsonResponse({ code: 0, msg: "success", data: { ticket: "ticket1" } })
      if (req.url.includes("/export_tasks/ticket1")) {
        ticketCalls++
        return ticketCalls >= 2 ? jsonResponse({ code: 0, msg: "success", data: { result: { file_token: "file1", file_name: "a.docx", file_size: 10, job_status: 0 } } }) : jsonResponse({ code: 0, msg: "success", data: { result: { job_status: 2 } } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.export_doc.execute({ token: "doxcn1", type: "docx", file_extension: "pdf" }, ctx())
    expect(result.output).toContain("file1")
    expect(result.output).toContain("10 字节")
    const req = records.find((r) => r.url.endsWith("/export_tasks"))
    expect(JSON.parse(String(req!.init?.body))).toEqual({ token: "doxcn1", type: "docx", file_extension: "pdf", file_name: undefined, sub_id: undefined })
    // 查询任务状态只携带 token query（实测多余 file_extension/type 报 field validation failed）
    const pollReq = records.find((r) => r.url.includes("/export_tasks/ticket1"))
    expect(pollReq!.url).toContain("token=doxcn1")
    expect(pollReq!.url).not.toContain("file_extension")
    expect(pollReq!.url).not.toContain("type=")
  })

  test("export_doc 任务终态失败提前返回 job_error_msg，不空等到超时", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/export_tasks") && req.init?.method === "POST") return jsonResponse({ code: 0, msg: "success", data: { ticket: "ticket1" } })
      if (req.url.includes("/export_tasks/ticket1")) return jsonResponse({ code: 0, msg: "success", data: { result: { job_status: 3, job_error_msg: "内部错误" } } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.export_doc.execute({ token: "doxcn1", type: "docx", file_extension: "pdf" }, ctx())
    expect(result.output).toContain("导出任务失败")
    expect(result.output).toContain("内部错误")
    expect(records.filter((r) => r.url.includes("/export_tasks/ticket1"))).toHaveLength(1) // 不继续轮询
  })

  test("export_doc bitable 直接传 app_token（修复 1069914 file token invalid），csv 必须带 sub_id", async () => {
    let exportCalls = 0
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/export_tasks") && req.init?.method === "POST") {
        exportCalls++
        const body = JSON.parse(String(req.init?.body)) as { token: string; sub_id?: string }
        // 实测 1069914 file token invalid：token 传数据表 table_id（tbl 开头）报错——必须是 app_token
        if (body.token !== "bascn1") return jsonResponse({ code: 1069914, msg: "file token invalid", data: {} }, 404)
        return jsonResponse({ code: 0, msg: "success", data: { ticket: "ticket1" } })
      }
      if (req.url.includes("/export_tasks/ticket1")) return jsonResponse({ code: 0, msg: "success", data: { result: { file_token: "file1", file_name: "a.xlsx", file_size: 5, job_status: 0 } } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // xlsx：直接 app_token，无需解析数据表（修复：此前解析 table token 报 1069914）
    const result = await tools.export_doc.execute({ token: "bascn1", type: "bitable", file_extension: "xlsx" }, ctx())
    expect(result.output).toContain("file1")
    expect(records.some((r) => r.url.includes("/bitable/v1/apps"))).toBe(false) // 不再解析数据表列表
    expect(exportCalls).toBe(1)
    const exportReq = records.find((r) => r.url.endsWith("/export_tasks") && r.init?.method === "POST")
    expect(JSON.parse(String(exportReq!.init?.body))).toEqual({ token: "bascn1", type: "bitable", file_extension: "xlsx", file_name: undefined, sub_id: undefined })
    // csv：官方接口要求 sub_id（数据表 ID），缺失时前置拦截引导
    const { tools: t3, records: r3 } = makeTools(defaultHandler)
    const csv = await t3.export_doc.execute({ token: "bascn1", type: "bitable", file_extension: "csv" }, ctx())
    expect(csv.output).toContain("sub_id")
    expect(r3.some((r) => r.url.includes("/export_tasks"))).toBe(false)
    // csv + sub_id：作为 sub_id 传参创建任务
    const { tools: t2, records: r2 } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/export_tasks") && req.init?.method === "POST") return jsonResponse({ code: 0, msg: "success", data: { ticket: "ticket2" } })
      if (req.url.includes("/export_tasks/ticket2")) return jsonResponse({ code: 0, msg: "success", data: { result: { file_token: "file2" } } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    await t2.export_doc.execute({ token: "bascn1", type: "bitable", file_extension: "csv", sub_id: "tbl9" }, ctx())
    const exportReq2 = r2.find((r) => r.url.endsWith("/export_tasks") && r.init?.method === "POST")
    expect(JSON.parse(String(exportReq2!.init?.body))).toEqual({ token: "bascn1", type: "bitable", file_extension: "csv", file_name: undefined, sub_id: "tbl9" })
  })

  test("upload_file 发送 multipart form", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/files/upload_all")) return jsonResponse({ code: 0, msg: "success", data: { file_token: "box1" } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.upload_file.execute({ file_name: "a.txt", content: "hello" }, ctx())
    expect(result.output).toContain("box1")
    const req = records.find((r) => r.url.includes("/upload_all"))
    expect(req!.init?.method).toBe("POST")
    expect(req!.init?.body).toBeInstanceOf(FormData)
    const fd = req!.init!.body as FormData
    expect(String(fd.get("file_name"))).toBe("a.txt")
    expect(String(fd.get("parent_type"))).toBe("explorer")
    expect(String(fd.get("size"))).toBe("5")
  })

  test("upload_file base64 解码", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/files/upload_all")) return jsonResponse({ code: 0, msg: "success", data: { file_token: "box2" } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const b64 = Buffer.from("png-bytes").toString("base64")
    const result = await tools.upload_file.execute({ file_name: "a.png", content: b64, encoding: "base64" }, ctx())
    expect(result.output).toContain("box2")
    const req = records.find((r) => r.url.includes("/upload_all"))
    const fd = req!.init!.body as FormData
    expect(String(fd.get("size"))).toBe("9")
  })

  test("download_file 保存到会话目录", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/files/box1/download")) {
        return new Response("file-content", { status: 200, headers: { "content-type": "text/plain" } })
      }
      if (req.url.includes("/drive/v1/metas/batch_query")) return jsonResponse({ code: 0, msg: "success", data: { metas: [{ doc_token: "box1", title: "data.txt" }] } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const c = ctx()
    const result = await tools.download_file.execute({ file_token: "box1" }, c)
    expect(result.output).toContain("预览: file-content")
    expect(result.output).toContain("data.txt")
    // 实测：导出文件下载必须带 Range: bytes=0-（否则 403）
    const dl = records.find((r) => r.url.includes("/files/box1/download"))
    expect((dl!.init?.headers as Record<string, string>).Range).toBe("bytes=0-")
  })
})

describe("wiki / permission / 搜索", () => {
  test("create_wiki_node 创建节点并写入 Markdown", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/wiki/v2/spaces/sp1/nodes")) return jsonResponse({ code: 0, msg: "success", data: { node: { node_token: "wikcn1", obj_token: "doxcn_wiki", title: "知识库页" } } })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/descendant")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.create_wiki_node.execute({ space_id: "sp1", title: "知识库页", markdown: "# 标题" }, ctx())
    expect(result.output).toContain("wikcn1")
    expect(result.output).toContain("doxcn_wiki")
    const nodeReq = records.find((r) => r.url.includes("/wiki/v2/spaces/sp1/nodes"))
    expect(JSON.parse(String(nodeReq!.init?.body))).toEqual({ obj_type: "docx", title: "知识库页" })
    expect(records.some((r) => r.url.includes("/descendant"))).toBe(true)
  })

  test("list_wiki_spaces 与 get_wiki_node", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/wiki/v2/spaces/get_node")) return jsonResponse({ code: 0, msg: "success", data: { node: { node_token: "wikcn2" } } })
      return jsonResponse({ code: 0, msg: "success", data: { items: [{ space_id: "sp1" }] } })
    })
    const list = await tools.list_wiki_spaces.execute({}, ctx())
    expect(list.output).toContain("sp1")
    const get = await tools.get_wiki_node.execute({ token: "wikcn2" }, ctx())
    expect(get.output).toContain("wikcn2")
  })

  test("add_permission 发送协作者请求", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 0, msg: "success", data: { member: {} } })
    })
    const result = await tools.add_permission.execute({ token: "doxcn1", type: "docx", member_type: "openid", member_id: "ou_1", perm: "view" }, ctx())
    expect(result.output).toContain("授权成功")
    const req = records.find((r) => r.url.includes("/permissions/doxcn1/members"))
    expect(req!.url).toContain("type=docx")
    expect(JSON.parse(String(req!.init?.body))).toEqual({ member_type: "openid", member_id: "ou_1", perm: "view" })
  })

  test("search 携带搜索词", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 0, msg: "success", data: { docs_entities: [] } })
    })
    const result = await tools.search.execute({ query: "周报", count: 5, docs_types: JSON.stringify(["docx", "sheet"]) }, ctx())
    expect(result.output).toContain("docs_entities")
    const req = records.find((r) => r.url.includes("/search/object"))
    const body = JSON.parse(String(req!.init?.body)) as { search_key: string; count: number; offset?: number; docs_types: string[] }
    expect(body).toEqual({ search_key: "周报", count: 5, offset: 0, docs_types: ["docx", "sheet"] })
  })
})

describe("bitable / sheets 其余操作", () => {
  test("delete_bitable_records 逗号分隔转数组", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.delete_bitable_records.execute({ app_token: "app1", table_id: "tbl1", record_ids: "rec1,rec2" }, ctx())
    expect(result.output).toContain("删除成功")
    // B3/实测：改用 POST batch_delete 且 records 为字符串数组（对象数组实测报错）
    const req = records.find((r) => r.url.includes("/records/batch_delete"))
    expect(req!.init?.method).toBe("POST")
    expect(JSON.parse(String(req!.init?.body))).toEqual({ records: ["rec1", "rec2"] })
    // 超过 500 条自动分批
    const many = Array.from({ length: 501 }, (_, i) => `rec${i}`).join(",")
    const { tools: t2, records: r2 } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    await t2.delete_bitable_records.execute({ app_token: "app1", table_id: "tbl1", record_ids: many }, ctx())
    const delReqs = r2.filter((r) => r.url.includes("/records/batch_delete"))
    expect(delReqs).toHaveLength(2)
    expect(JSON.parse(String(delReqs[0].init?.body))).toEqual({ records: many.split(",").slice(0, 500) })
    expect(JSON.parse(String(delReqs[1].init?.body))).toEqual({ records: ["rec500"] })
    // 容错：record_ids 传对象数组 {record_id} 时自动提取为字符串
    const { tools: t3, records: r3 } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    await t3.delete_bitable_records.execute({ app_token: "app1", table_id: "tbl1", record_ids: JSON.stringify([{ record_id: "recA" }, { record_id: "recB" }]) }, ctx())
    const objReq = r3.find((r) => r.url.includes("/records/batch_delete"))
    expect(JSON.parse(String(objReq!.init?.body))).toEqual({ records: ["recA", "recB"] })
  })

  test("update_bitable_record 发送 fields", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 0, msg: "success", data: { record: {} } })
    })
    const result = await tools.update_bitable_record.execute({ app_token: "app1", table_id: "tbl1", record_id: "rec1", fields: JSON.stringify({ 状态: "完成" }) }, ctx())
    expect(result.output).toContain("更新成功")
    const req = records.find((r) => r.url.includes("/records/rec1"))
    expect(JSON.parse(String(req!.init?.body))).toEqual({ fields: { 状态: "完成" } })
  })

  test("get_sheet_meta 与 read_sheet 缺省 range", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/sheets/query")) {
        return jsonResponse({ code: 0, msg: "success", data: { sheets: [{ sheet_id: "oVs1", title: "Sheet1", index: 0 }] } })
      }
      return jsonResponse({ code: 0, msg: "success", data: { spreadsheet: { title: "S" } } })
    })
    const meta = await tools.get_sheet_meta.execute({ spreadsheet_token: "sht1" }, ctx())
    expect(meta.output).toContain("S")
    // B6：get_sheet_meta 补调 sheets/query，输出含工作表列表
    expect(meta.output).toContain("oVs1")
    const read = await tools.read_sheet.execute({ spreadsheet_token: "sht1" }, ctx())
    expect(read.output).toContain("sheet_id")
    expect(records.filter((r) => r.url.includes("/sheets/v3/spreadsheets/sht1")).length).toBe(3)
  })

  test("append_sheet 发送 values_append", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/sheets/query")) {
        return jsonResponse({ code: 0, msg: "success", data: { sheets: [{ sheet_id: "oVsAj0001", title: "Sheet1" }] } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.append_sheet.execute({ spreadsheet_token: "sht1", range: "Sheet1!A1", values: JSON.stringify([["a", "b"], ["c", "d"], ["e", "f"]]) }, ctx())
    expect(result.output).toContain("追加成功")
    const req = records.find((r) => r.url.includes("/values_append"))
    expect(req!.url).toContain("insertDataOption=INSERT_ROWS")
    const body = JSON.parse(String(req!.init?.body)) as { valueRange: { range: string; values: unknown[] } }
    // 名称→sheet_id 解析 + 单格 range 自动扩展为覆盖全部数据行的区域（实测 wrong range）
    expect(body.valueRange.range).toBe("oVsAj0001!A1:B3")
    expect(body.valueRange.values).toHaveLength(3)
  })

  test("create_bitable / list_bitable_tables / list_bitable_records", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/bitable/v1/apps") && req.init?.method === "POST") return jsonResponse({ code: 0, msg: "success", data: { app: { app_token: "app1" } } })
      return jsonResponse({ code: 0, msg: "success", data: { items: [{ table_id: "tbl1" }] } })
    })
    const created = await tools.create_bitable.execute({ name: "项目表" }, ctx())
    expect(created.output).toContain("app1")
    const tables = await tools.list_bitable_tables.execute({ app_token: "app1" }, ctx())
    expect(tables.output).toContain("tbl1")
    const recs = await tools.list_bitable_records.execute({ app_token: "app1", table_id: "tbl1", filter: JSON.stringify({ conjunction: "and", conditions: [] }) }, ctx())
    expect(recs.output).toContain("tbl1")
    // B4：filter 走 POST /records/search 带 body filter（GET /records?filter= 报 InvalidFilter）
    const searchReq = records.find((r) => r.url.includes("/records/search"))
    expect(searchReq!.init?.method).toBe("POST")
    expect(JSON.parse(String(searchReq!.init?.body))).toEqual({ filter: { conjunction: "and", conditions: [] } })
  })

  test("create_bitable fields 参数自动创建自定义字段（修复 FieldNameNotFound）", async () => {
    const fieldReqs: Req[] = []
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/bitable/v1/apps") && req.init?.method === "POST") {
        return jsonResponse({ code: 0, msg: "success", data: { app: { app_token: "bascn1", default_table_id: "tbl1" } } })
      }
      if (req.url.includes("/tables/tbl1/fields")) {
        fieldReqs.push(req)
        return jsonResponse({ code: 0, msg: "success", data: { field: { field_id: "fld1" } } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const fields = [
      { name: "名称", type: 1 },
      { name: "状态", type: 3, property: { options: [{ name: "进行中" }, { name: "已完成" }] } },
    ]
    const created = await tools.create_bitable.execute({ name: "项目表", fields: JSON.stringify(fields) }, ctx())
    expect(created.output).toContain("bascn1")
    expect(created.output).toContain("默认数据表: tbl1")
    expect(created.output).toContain("已创建 2 个字段: 名称 / 状态")
    expect(fieldReqs).toHaveLength(2)
    expect(JSON.parse(String(fieldReqs[0].init?.body))).toEqual({ field_name: "名称", type: 1, property: undefined })
    expect(JSON.parse(String(fieldReqs[1].init?.body))).toEqual({
      field_name: "状态",
      type: 3,
      property: { options: [{ name: "进行中" }, { name: "已完成" }] },
    })
  })

  test("get_file_meta 走 metas/batch_query", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/drive/v1/metas/batch_query")) {
        return jsonResponse({ code: 0, msg: "success", data: { metas: [{ doc_token: "doxcn1", doc_type: "docx", title: "报告", url: "https://feishu.cn/docx/doxcn1" }] } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // B2：files/{token} 对 docx 404，必须走 batch_query
    const withType = await tools.get_file_meta.execute({ file_token: "doxcn1", type: "docx" }, ctx())
    expect(withType.output).toContain("报告")
    await tools.get_file_meta.execute({ file_token: "doxcn1" }, ctx())
    const reqs = records.filter((r) => r.url.includes("/drive/v1/metas/batch_query"))
    expect(reqs).toHaveLength(2)
    expect(reqs[0].init?.method).toBe("POST")
    expect(JSON.parse(String(reqs[0].init?.body))).toEqual({ request_docs: [{ doc_token: "doxcn1", doc_type: "docx" }] })
    // 未传 type：不带 doc_type（服务端自动识别）
    expect(JSON.parse(String(reqs[1].init?.body))).toEqual({ request_docs: [{ doc_token: "doxcn1" }] })
    // 不遗留旧接口调用
    expect(records.some((r) => r.url.includes("/drive/v1/files/doxcn1") && !r.url.includes("/download"))).toBe(false)
  })

  test("get_file_meta 缺省 type 自动识别失败时补 file 类型查询", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/drive/v1/metas/batch_query")) {
        const body = JSON.parse(String((req as unknown as { init?: { body?: string } }).init?.body ?? "{}")) as { request_docs?: Array<{ doc_type?: string }> }
        if (body.request_docs?.[0]?.doc_type === "file") {
          return jsonResponse({ code: 0, msg: "success", data: { metas: [{ doc_token: "box1", doc_type: "file", title: "a.txt" }] } })
        }
        // 无 doc_type 时普通 file 识别不出（metas 空）
        return jsonResponse({ code: 0, msg: "success", data: { metas: [] } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // 实测：file 类型缺省自动识别失败，需显式 type=file——工具内部自动补查
    const result = await tools.get_file_meta.execute({ file_token: "box1" }, ctx())
    expect(result.output).toContain("a.txt")
    const reqs = records.filter((r) => r.url.includes("/drive/v1/metas/batch_query"))
    expect(reqs).toHaveLength(2)
    expect(JSON.parse(String(reqs[1].init?.body))).toEqual({ request_docs: [{ doc_token: "box1", doc_type: "file" }] })
  })

  test("get_file_meta 缺省 type 时 file 补查仍无结果自动再试 docx（970005 修复）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/drive/v1/metas/batch_query")) {
        const body = JSON.parse(String((req as unknown as { init?: { body?: string } }).init?.body ?? "{}")) as { request_docs?: Array<{ doc_type?: string }> }
        const docType = body.request_docs?.[0]?.doc_type
        if (docType === "docx") {
          return jsonResponse({ code: 0, msg: "success", data: { metas: [{ doc_token: "doxcn2", doc_type: "docx", title: "会议纪要", url: "https://feishu.cn/docx/doxcn2" }] } })
        }
        if (docType === "file") {
          // file 补查也无结果（模拟 docx 缺省识别失败场景）
          return jsonResponse({ code: 0, msg: "success", data: { metas: [] } })
        }
        // 无 doc_type 时自动识别失败（metas 空）
        return jsonResponse({ code: 0, msg: "success", data: { metas: [] } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // 实测：docx 缺省自动识别报 970005，file 补查也查不到——工具应自动再试 docx
    const result = await tools.get_file_meta.execute({ file_token: "doxcn2" }, ctx())
    expect(result.output).toContain("会议纪要")
    const reqs = records.filter((r) => r.url.includes("/drive/v1/metas/batch_query"))
    expect(reqs).toHaveLength(3)
    expect(JSON.parse(String(reqs[1].init?.body))).toEqual({ request_docs: [{ doc_token: "doxcn2", doc_type: "file" }] })
    expect(JSON.parse(String(reqs[2].init?.body))).toEqual({ request_docs: [{ doc_token: "doxcn2", doc_type: "docx" }] })
  })

  test("read_sheet range 前缀为工作表名称时自动解析为 sheet_id", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/sheets/query")) {
        return jsonResponse({ code: 0, msg: "success", data: { sheets: [{ sheet_id: "oVsAj1", title: "Sheet1" }, { sheet_id: "oVsAj2", title: "数据" }, { sheet_id: "oVsAj3", title: "order" }, { sheet_id: "oVsAj4", title: "2024" }] } })
      }
      return jsonResponse({ code: 0, msg: "success", data: { valueRange: { values: [[1]] } } })
    })
    // B6：名称 → sheet_id 自动解析（用名称报 90215）；range 冒号经 encodeURIComponent 编码
    await tools.read_sheet.execute({ spreadsheet_token: "sht1", range: "Sheet1!A1:C3" }, ctx())
    const readReq = records.find((r) => r.url.includes("/values/"))
    expect(readReq!.url).toContain("oVsAj1!A1")
    // o 开头/数字表名：名称匹配优先（不误判为 sheet_id）
    await tools.read_sheet.execute({ spreadsheet_token: "sht1", range: "order!A1" }, ctx())
    await tools.read_sheet.execute({ spreadsheet_token: "sht1", range: "2024!B2" }, ctx())
    const orderReq = records.filter((r) => r.url.includes("/values/"))[1]
    const numReq = records.filter((r) => r.url.includes("/values/"))[2]
    expect(orderReq!.url).toContain("oVsAj3!A1")
    expect(numReq!.url).toContain("oVsAj4!B2")
    // 已是 sheet_id 前缀（oVs 开头长 token）：不触发解析请求（range 在 body 中）
    await tools.write_sheet.execute({ spreadsheet_token: "sht1", range: "oVsAj0022!A1:B2", values: JSON.stringify([["a"]]) }, ctx())
    const writeReq = records.find((r) => r.url.includes("/values") && r.init?.method === "PUT")
    expect((JSON.parse(String(writeReq!.init?.body)) as { valueRange: { range: string } }).valueRange.range).toBe("oVsAj0022!A1:B2")
    // 三次名称解析触发 3 次 sheets/query；oVs 长前缀（write）不触发
    const queryCount = records.filter((r) => r.url.includes("/sheets/query")).length
    expect(queryCount).toBe(3)
  })

  test("read_sheet range 前缀未命中任何工作表名/sheet_id 时报可用清单（替代 90215 盲错）；新格式 sheet_id 按 sheet_id 字段透传", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/sheets/query")) {
        return jsonResponse({ code: 0, msg: "success", data: { sheets: [{ sheet_id: "oVsAj1", title: "Sheet1" }, { sheet_id: "Mm1gs7Ur", title: "数据" }] } })
      }
      return jsonResponse({ code: 0, msg: "success", data: { valueRange: { values: [[1]] } } })
    })
    // 常见误因：用了表格标题作 range 前缀——直接报可用工作表清单，不发起 values 请求（旧实现交由接口报 90215 盲错）
    const bad = await tools.read_sheet.execute({ spreadsheet_token: "sht1", range: "表格标题!A1:C3" }, ctx())
    expect(bad.output).toContain("不是本表格的工作表")
    expect(bad.output).toContain("Sheet1、数据")
    expect(records.some((r) => r.url.includes("/values/"))).toBe(false)
    // 新版 sheet_id（无 oVs 前缀）：按 sheets/query 返回的 sheet_id 字段透传，不误报
    const ok = await tools.read_sheet.execute({ spreadsheet_token: "sht1", range: "Mm1gs7Ur!A1" }, ctx())
    expect(ok.output).not.toContain("不是本表格的工作表")
    expect(records.some((r) => r.url.includes("/values/") && decodeURIComponent(r.url).includes("Mm1gs7Ur!A1"))).toBe(true)
  })
})

describe("update_block / 下载 / 分享（新修复）", () => {
  test("insert_text 失败自动降级为读块拼接整体替换", async () => {
    let patchCalls = 0
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      // 首次 insert_text PATCH：飞书参数校验失败（99991400 invalid param）
      if (req.url.includes(`/blocks/block1`) && req.init?.method === "PATCH") {
        patchCalls++
        if (patchCalls === 1) return jsonResponse({ code: 99991400, msg: "invalid param", data: {} }, 400)
        return jsonResponse({ code: 0, msg: "success", data: {} })
      }
      // 降级路径：读块原文（"你好世界"）——真实飞书响应为 data.block 包裹（未解包会读到空原文）
      if (req.url.endsWith("/blocks/block1") && req.init?.method === "GET") {
        return jsonResponse({ code: 0, msg: "success", data: { block: { block_id: "block1", block_type: 2, text: { elements: [{ text_run: { content: "你好世界" } }] } } } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.update_block.execute({ document_id: "doxcn1", block_id: "block1", insert_text: "XY", insert_index: 1 }, ctx())
    expect(result.output).toContain("降级为整体替换")
    const patches = records.filter((r) => r.url.includes("/blocks/block1") && r.init?.method === "PATCH")
    expect(patches).toHaveLength(2)
    // 降级 PATCH：原文本在 index=1 处插入 "XY" → "你XY好世界"
    const body = JSON.parse(String(patches[1].init?.body)) as { update_text_elements: { elements: Array<{ text_run: { content: string } }> } }
    expect(body.update_text_elements.elements.map((e) => e.text_run.content).join("")).toBe("你XY好世界")
    // 负例：非参数校验类错误（如 12345 业务错误）不触发降级
    let badCalls = 0
    const { tools: t2, records: r2 } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes(`/blocks/block1`) && req.init?.method === "PATCH") {
        badCalls++
        return jsonResponse({ code: 12345, msg: "some business error", data: {} }, 400)
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const bad = await t2.update_block.execute({ document_id: "doxcn1", block_id: "block1", insert_text: "XY" }, ctx())
    expect(bad.output).toContain("12345")
    expect(badCalls).toBe(1) // 未走降级重试
    expect(r2.filter((r) => r.url.includes(`/blocks/block1`) && r.init?.method === "PATCH")).toHaveLength(1)
  })

  test("download_file 导出产物（media token）files 403 时回退 medias 接口", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/drive/v1/metas/batch_query")) return jsonResponse({ code: 0, msg: "success", data: { metas: [{ doc_token: "box1", title: "a.pdf" }] } })
      if (req.url.includes("/files/box1/download")) return new Response("denied", { status: 403 })
      if (req.url.includes("/medias/box1/download")) return new Response("pdf-bytes", { status: 200, headers: { "content-type": "application/pdf" } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.download_file.execute({ file_token: "box1" }, ctx())
    expect(result.output).toContain("已保存 9 字节")
    const filesReq = records.find((r) => r.url.includes("/files/box1/download"))
    const mediasReq = records.find((r) => r.url.includes("/medias/box1/download"))
    expect(filesReq).toBeDefined()
    expect(mediasReq).toBeDefined()
    expect((mediasReq!.init?.headers as Record<string, string>).Range).toBe("bytes=0-")
  })

  test("set_link_share 发送 PATCH public 请求（实测修正：方法为 PATCH 非 PUT，PUT 404）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 0, msg: "success", data: { link_share_entity: "tenant_readable" } })
    })
    const result = await tools.set_link_share.execute({ token: "doxcn1", type: "docx" }, ctx())
    expect(result.output).toContain("分享设置成功")
    const req = records.find((r) => r.url.includes("/permissions/doxcn1/public"))
    expect(req!.init?.method).toBe("PATCH")
    expect(req!.url).toContain("type=docx")
    expect(JSON.parse(String(req!.init?.body))).toEqual({ link_share_entity: "tenant_readable" })
    // 负例：非法 link_share_entity 枚举明确报错（实测修正：枚举为 tenant/anyone 系列，anyone_can_view 属 security_entity）
    const bad = await tools.set_link_share.execute({ token: "doxcn1", type: "docx", link_share_entity: "anyone_can_view" }, ctx())
    expect(bad.output).toContain("link_share_entity 非法")
    expect(bad.output).toContain("tenant_readable")
  })

  test("set_link_share 404 附权限诊断引导（实测：GET 正常 PUT 404 = 写权限 scope 缺失）", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return new Response("not found", { status: 404 })
    })
    const result = await tools.set_link_share.execute({ token: "doxcn1", type: "docx" }, ctx())
    expect(result.output).toContain("404")
    expect(result.output).toContain("drive:drive") // 权限诊断引导
  })

  test("add_blocks blocks 数组简化写法自动规范化（99992402 修复）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      if (req.url.includes("/children")) return jsonResponse({ code: 0, msg: "success", data: {} })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    // 用户实测失败写法：text 为字符串、无完整包装 → 工具自动规范化后请求体合法
    const result = await tools.add_blocks.execute({ document_id: "doxcn1", blocks: JSON.stringify([{ block_type: 2, text: "hi" }, { block_type: 12, text: "item" }]) }, ctx())
    expect(result.output).toContain("已添加 2 个块")
    const req = records.find((r) => r.url.includes("/children"))
    const body = JSON.parse(String(req!.init?.body)) as { children: Array<Record<string, unknown>> }
    expect(body.children[0]).toEqual({ block_type: 2, text: { elements: [{ text_run: { content: "hi" } }] } })
    expect(body.children[1]).toEqual({ block_type: 12, bullet: { elements: [{ text_run: { content: "item" } }] } })
  })

  test("insert_image 三步流程（空 image 块 → media 上传 → replace_image）", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      // 步骤 1：创建空 image 块（image:{} 不传 token）
      if (req.url.includes("/blocks/page_root/children") && req.init?.method === "POST") {
        return jsonResponse({ code: 0, msg: "success", data: { children: [{ block_id: "img_block1", block_type: 27 }] } })
      }
      // 步骤 2：media 上传（multipart）
      if (req.url.includes("/drive/v1/medias/upload_all")) {
        const fd = req.init!.body as FormData
        expect(String(fd.get("parent_type"))).toBe("docx_image")
        expect(String(fd.get("parent_node"))).toBe("img_block1")
        expect(String(fd.get("size"))).toBe("4")
        return jsonResponse({ code: 0, msg: "success", data: { file_token: "media_tok1" } })
      }
      // 步骤 3：PATCH replace_image
      if (req.url.includes("/blocks/img_block1") && req.init?.method === "PATCH") {
        return jsonResponse({ code: 0, msg: "success", data: { image: { width: 320, height: 180 } } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.insert_image.execute({ document_id: "doxcn1", image: Buffer.from("png!").toString("base64"), encoding: "base64" }, ctx())
    expect(result.output).toContain("已在文档插入图片（320×180）")
    expect(result.output).toContain("img_block1")
    // 步骤 1 创建请求体：image:{} 无 token
    const createReq = records.find((r) => r.url.includes("/blocks/page_root/children"))
    expect(JSON.parse(String(createReq!.init?.body))).toEqual({ children: [{ block_type: 27, image: {} }] })
    // 步骤 3 PATCH：replace_image 携带 media token
    const patchReq = records.find((r) => r.url.includes("/blocks/img_block1") && r.init?.method === "PATCH")
    expect(JSON.parse(String(patchReq!.init?.body))).toEqual({ block_id: "img_block1", replace_image: { token: "media_tok1" } })
  })
})

/* ================= board 画板读取 ================= */

describe("board 提取纯函数", () => {
  test("extractBoardToken 从 mindnote 块提取 board.token（含嵌套）", () => {
    expect(extractBoardToken({ block_id: "b1", block_type: 43, board: { token: "bord_abc" } })).toBe("bord_abc")
    expect(extractBoardToken({ block_id: "b1", block_type: 43, mindnote: { board: { token: "bord_nested" } } })).toBe("bord_nested")
    expect(extractBoardToken({ block_id: "b1", block_type: 43, board_token: "bord_direct" })).toBe("bord_direct")
    expect(extractBoardToken({ block_id: "b1", block_type: 43, whiteboard_id: "bord_wb" })).toBe("bord_wb")
    expect(extractBoardToken({ block_id: "b1", block_type: 2, text: { elements: [] } })).toBe("")
    expect(extractBoardToken(null)).toBe("")
  })

  test("findPlantUmlSource 优先 syntax.code 且识别 PlantUML 特征", () => {
    const nodes = [{ node_id: "n1", syntax: { code: "@startuml\nA --> B\n@enduml" } }]
    expect(findPlantUmlSource(nodes)).toContain("@startuml")
    // 非 syntax 但含箭头语法的 code 也能识别
    expect(findPlantUmlSource([{ node_id: "n1", code: "A -> B" }])).toBe("A -> B")
    // 无 PlantUML 特征时取第一个 code
    expect(findPlantUmlSource([{ node_id: "n1", code: "hello" }])).toBe("hello")
    // 无 code 返回 undefined
    expect(findPlantUmlSource([{ node_id: "n1", text: "x" }])).toBeUndefined()
  })

  test("collectBoardShapes 收集 node_id+文本（含 content.text 嵌套）", () => {
    const nodes = [
      { node_id: "n1", props: { text: "步骤A" } },
      { node_id: "n2", content: { text: "步骤B" } },
      { node_id: "n3" },
    ]
    const shapes = collectBoardShapes(nodes)
    expect(shapes.map((s) => [s.id, s.text])).toEqual([["n1", "步骤A"], ["n2", "步骤B"]])
  })

  test("collectBoardEdges 收集连接线（from/to/source/target/start/end 引用）", () => {
    const nodes = [
      { node_id: "c1", from: "n1", to: "n2", text: "是" },
      { node_id: "c2", source: { node_id: "n2" }, target: { id: "n3" }, label: "否" },
      { node_id: "c3", start_node_id: "n1", end_node_id: "n4" },
      { node_id: "c4", from: "n1", to: "n1" },
    ]
    const edges = collectBoardEdges(nodes)
    expect(edges).toEqual([
      { from: "n1", to: "n2", label: "是" },
      { from: "n2", to: "n3", label: "否" },
      { from: "n1", to: "n4", label: undefined },
    ])
  })

  test("extractBoardContent 无源码时重建流程描述", () => {
    const nodes = [
      { node_id: "n1", props: { text: "步骤A" } },
      { node_id: "n2", props: { text: "步骤B" } },
      { node_id: "c1", from: "n1", to: "n2", text: "是" },
    ]
    const out = extractBoardContent(nodes)
    expect(out).toContain("步骤A ->(是) 步骤B")
    expect(out).toContain("连接关系")
  })

  test("extractBoardContent 有 syntax.code 时返回 PlantUML 源码", () => {
    const nodes = [{ node_id: "n1", syntax: { code: "@startuml\nA --> B\n@enduml" }, props: { text: "x" } }]
    const out = extractBoardContent(nodes)
    expect(out).toContain("[画板 PlantUML 源码]")
    expect(out).toContain("@startuml")
    expect(out).not.toContain("连接关系")
  })

  test("extractBoardContent 仅形状无连接线时输出节点文本；全无则回退原始 JSON", () => {
    expect(extractBoardContent([{ node_id: "n1", text: "甲" }, { node_id: "n2", text: "乙" }])).toContain("[画板节点文本]\n甲\n乙")
    expect(extractBoardContent([{ node_id: "n1", props: { whatever: 1 } }])).toContain("[画板节点原始结构]")
  })
})

describe("get_board 工具", () => {
  const boardNodesHandler = (req: Req): Response => {
    if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
    if (req.url.endsWith("/blocks/b_mind")) {
      return jsonResponse({ code: 0, msg: "success", data: { block_id: "b_mind", block_type: 43, board: { token: "bord_tok1" } } })
    }
    if (req.url.includes("/board/v1/whiteboards/bord_tok1/nodes")) {
      return jsonResponse({
        code: 0,
        msg: "success",
        data: { items: [{ node_id: "n1", syntax: { code: "@startuml\nA --> B\n@enduml" } }], has_more: false },
      })
    }
    return jsonResponse({ code: 0, msg: "success", data: {} })
  }

  test("board_token 直读返回结构化提取结果（PlantUML 优先）", async () => {
    const { tools, records } = makeTools(boardNodesHandler)
    const result = await tools.get_board.execute({ board_token: "bord_tok1" }, ctx())
    expect(result.output).toContain("@startuml")
    const req = records.find((r) => r.url.includes("/board/v1/whiteboards/bord_tok1/nodes"))
    expect(req).toBeTruthy()
    expect(req!.url).toContain("page_size=100")
  })

  test("document_id+block_id 自动提取画板 token 后读取", async () => {
    const { tools, records } = makeTools(boardNodesHandler)
    const result = await tools.get_board.execute({ document_id: "doxcn1", block_id: "b_mind" }, ctx())
    expect(result.output).toContain("A --> B")
    const blockReq = records.find((r) => r.url.endsWith("/blocks/b_mind"))
    expect(blockReq).toBeTruthy()
    expect(records.some((r) => r.url.includes("/board/v1/whiteboards/bord_tok1/nodes"))).toBe(true)
  })

  test("block 不含画板 token 时明确报错（附块类型）", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/blocks/b_text")) return jsonResponse({ code: 0, msg: "success", data: { block_id: "b_text", block_type: 2, text: { elements: [] } } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.get_board.execute({ document_id: "doxcn1", block_id: "b_text" }, ctx())
    expect(result.output).toContain("不含画板 token")
    expect(result.output).toContain("text")
    expect(result.output).toContain("mindnote")
  })

  test("board_token 与 document_id+block_id 均缺时报错", async () => {
    const { tools } = makeTools(defaultHandler)
    const result = await tools.get_board.execute({}, ctx())
    expect(result.output).toContain("board_token")
  })

  test("多页分页收集节点（连接线重建流程）", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("page_token=p2")) {
        return jsonResponse({
          code: 0,
          msg: "success",
          data: { items: [{ node_id: "n3", props: { text: "步骤C" } }, { node_id: "c2", from: "n2", to: "n3", text: "否" }], has_more: false },
        })
      }
      if (req.url.includes("/board/v1/whiteboards/bord_multi/nodes")) {
        return jsonResponse({
          code: 0,
          msg: "success",
          data: {
            items: [
              { node_id: "n1", props: { text: "步骤A" } },
              { node_id: "n2", props: { text: "步骤B" } },
              { node_id: "c1", from: "n1", to: "n2", text: "是" },
            ],
            has_more: true,
            page_token: "p2",
          },
        })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.get_board.execute({ board_token: "bord_multi" }, ctx())
    expect(result.output).toContain("步骤A ->(是) 步骤B")
    expect(result.output).toContain("步骤B ->(否) 步骤C")
  })

  test("空画板返回提示", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/board/v1/whiteboards/bord_empty/nodes")) return jsonResponse({ code: 0, msg: "success", data: { items: [], has_more: false } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.get_board.execute({ board_token: "bord_empty" }, ctx())
    expect(result.output).toContain("画板为空")
  })
})

/* ================= 权限类错误码引导 ================= */

describe("权限类错误码引导", () => {
  test("99991672（导出权限缺失）附带所需 scope 与授权链接", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 99991672, msg: "permission denied", data: {} }, 400)
    })
    const result = await tools.export_doc.execute({ token: "doxcn1", type: "docx", file_extension: "pdf" }, ctx())
    expect(result.output).toContain("99991672")
    expect(result.output).toContain("docs:document:export")
    expect(result.output).toContain("drive:export:readonly")
    expect(result.output).toContain("https://open.feishu.cn/app/cli_test_app/auth?q=docs%3Adocument%3Aexport")
  })

  test("99991668 权限类错误附资源授权与开通引导", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 99991668, msg: "no permission", data: {} }, 400)
    })
    const result = await tools.get_doc_meta.execute({ document_id: "doxcn1" }, ctx())
    expect(result.output).toContain("99991668")
    expect(result.output).toContain("docx:document")
    expect(result.output).toContain("授权链接")
  })

  test("未覆盖的权限码（9999167x 区间）也统一附引导，不再裸报错", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 99991677, msg: "scope missing", data: {} }, 400)
    })
    const result = await tools.get_doc_meta.execute({ document_id: "doxcn1" }, ctx())
    expect(result.output).toContain("99991677")
    expect(result.output).toContain("docx:document")
  })

  test("非权限类错误不加授权链接", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      return jsonResponse({ code: 1770001, msg: "invalid param", data: {} }, 400)
    })
    const result = await tools.get_doc_meta.execute({ document_id: "doxcn1" }, ctx())
    expect(result.output).toContain("参数不合法")
    expect(result.output).not.toContain("open.feishu.cn/app")
  })
})

/* ================= page_all 上限提示 ================= */

describe("page_all 达到上限提示", () => {
  test("get_doc_blocks page_all 达到 2000 块上限且还有更多时提示", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=100")) {
        const items = Array.from({ length: 2000 }, (_, i) => ({ block_id: `b${i}`, block_type: 2, children: [], text: { elements: [] } }))
        return jsonResponse({ code: 0, msg: "success", data: { items, has_more: true, page_token: "p2" } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const c = ctx()
    const result = await tools.get_doc_blocks.execute({ document_id: "doxcn1", page_all: true }, c)
    // 上限提示放在尾部（截断保留 tail 行）
    expect(result.output).toContain("已达 2000 块读取上限")
    expect(result.output).toContain('"items"')
    // total 在 JSON 中部被截断，完整内容落盘可查（相对会话工作目录的路径 → 会话 tmp/ 拼接）
    const m = result.output?.match(/文件: (truncated\/[\w.]+)/)
    expect(m).toBeTruthy()
    const file = await Bun.file(join(sessionPath(c.home, "default", "0123456789abcdef0123456789abcdef"), "tmp", m![1])).text()
    expect(file).toContain('"total":2000')
  })

  test("未达上限不提示", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks?page_size=100")) {
        return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "b1", block_type: 2, children: [] }], has_more: false } })
      }
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const result = await tools.get_doc_blocks.execute({ document_id: "doxcn1", page_all: true }, ctx())
    expect(result.output).not.toContain("已达")
  })
})

describe("子Agent 定义", () => {  test("feishu_docs 定义完整且工具命名合法", () => {    expect(feishuDef.name).toBe("feishu_docs")
    expect(feishuDef.description.length).toBeGreaterThan(20)
    expect(feishuDef.systemPrompt).toContain("FEISHU_DOCS_APP_ID")
    const tools = Object.keys(feishuDef.tools ?? {})
    expect(tools.length).toBeGreaterThanOrEqual(30)
    for (const t of tools) {
      expect(/^[a-zA-Z0-9_]+$/.test(t)).toBe(true)
      expect(t.length).toBeLessThanOrEqual(40 - "feishu_docs".length - 1)
    }
    // 写操作全部需审批
    for (const w of ["create_doc", "import_markdown", "import_xml", "delete_blocks", "upload_file", "add_permission", "api_call", "set_table_width"]) {
      expect(feishuDef.requiresApproval?.[w]).toBe(true)
    }
    // 读操作/会话配置/只读体检不审批
    for (const r of ["get_doc_text", "list_files", "read_sheet", "get_board", "auth_user_authorize", "auth_user_token", "auth_user_status", "auth_user_clear", "lint_doc", "style_guide"]) {
      expect(feishuDef.requiresApproval?.[r]).toBeUndefined()
    }
    expect(feishuDef.preload).toBe(false)
  })

  test("add_blocks 描述覆盖全部可创建块类型提示", () => {
    const desc = String(feishuDef.tools?.["add_blocks"]?.description ?? "")
    // 文本类
    for (const s of ["heading1~9", "bullet", "ordered", "code", "quote", "equation", "todo", "divider"]) {
      expect(desc).toContain(s)
    }
    // 容器类（嵌套接口）
    for (const s of ["callout", "grid", "grid_column", "table.rows"]) {
      expect(desc).toContain(s)
    }
    // 引用型（需资源 token / 外部地址）
    for (const s of ["embed.url", "file.token", "sheet.token", "mindnote.token", "bitable.token", "diagram.diagram_type"]) {
      expect(desc).toContain(s)
    }
    // 限制提示：image 走 insert_image、table_cell 不可单独创建
    expect(desc).toContain("insert_image")
    expect(desc).toContain("table_cell 不可单独创建")
    // 实测缺陷提示：equation 不可创建、callout 正文放子块/颜色emoji 为顶层字段、grid 每列须带 width_ratio 且非空
    expect(desc).toContain("不可经 API 创建")
    expect(desc).toContain("正文放 children 子块")
    expect(desc).toContain("callout 顶层字段")
    expect(desc).toContain("width_ratio")
    // md-only 细节已并入 add_blocks 描述：todo.style.done、grid 列宽 api_call 调整
    expect(desc).toContain("todo.style.done")
    expect(desc).toContain("update_grid_column_width_ratio")
    // 块类型知识单源于 add_blocks 描述：系统提示词只留指针，不重复整表
    expect(feishuDef.systemPrompt).toContain("add_blocks 工具描述")
    expect(feishuDef.systemPrompt).not.toContain("## 块类型速查")
  })
})

describe("排版 SKILL 与 XML 导入", () => {
  test("style_guide：无 name 列出全部文档，有 name 返回全文，未知名给出可用清单", async () => {
    const { tools } = makeTools(() => jsonResponse({ code: 0, msg: "ok" }))
    const list = await tools.style_guide.execute({}, ctx())
    for (const n of ["style", "xml", "weekly-report", "technical-doc", "formal-doc"]) expect(list.output).toContain(n)
    const xml = await tools.style_guide.execute({ name: "xml" }, ctx())
    expect(xml.output).toContain("width-ratio")
    expect(xml.output).toContain("import_xml")
    const style = await tools.style_guide.execute({ name: "style" }, ctx())
    expect(style.output).toContain("工作流")
    const bad = await tools.style_guide.execute({ name: "nope" }, ctx())
    expect(bad.output).toContain("未知规范文档")
    expect(bad.output).toContain("weekly-report")
  })

  test("import_xml：XML 一次成型——标题自动编号/高亮块配色/分栏/表格列宽落到 descendant 请求", async () => {
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.endsWith("/docx/v1/documents")) return jsonResponse({ code: 0, msg: "success", data: { document: { document_id: "doc_1" } } })
      if (req.url.includes("/blocks?page_size=1")) return jsonResponse({ code: 0, msg: "success", data: { items: [{ block_id: "page_root", block_type: 1 }] } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const xml = [
      "<title>季度复盘</title>",
      '<h1 seq="auto">结论</h1>',
      "<p>本期交付按时完成。</p>",
      '<callout emoji="bulb" background-color="medium-red"><p>风险：依赖尚未确认。</p></callout>',
      "<table><colgroup><col width=\"200\"/><col width=\"530\"/></colgroup><thead><tr><th>项</th><th>值</th></tr></thead><tbody><tr><td>a</td><td>b</td></tr></tbody></table>",
      '<grid><column width-ratio="0.5"><p>左栏</p></column><column width-ratio="0.5"><p>右栏</p></column></grid>',
    ].join("\n")
    const result = await tools.import_xml.execute({ content: xml }, ctx())
    expect(result.output).toContain("已导入")
    expect(result.output).toContain("doc_1")
    const desc = records.find((r) => r.url.includes("/descendant"))
    const body = JSON.parse(String(desc!.init?.body)) as { descendants: Array<Record<string, unknown>> }
    const blocks = body.descendants
    // 标题自动编号（1 结论），未手写序号
    const h1 = blocks.find((b) => b.block_type === 3) as { heading1: { elements: Array<{ text_run: { content: string } }> } }
    expect(h1.heading1.elements.map((e) => e.text_run.content).join("")).toBe("1 结论")
    // 高亮块：medium-red → 8；子块在 children；无 local 字段泄漏
    const callout = blocks.find((b) => b.block_type === 19) as { callout: Record<string, unknown>; children: string[] }
    expect(callout.callout).toMatchObject({ background_color: 8, emoji_id: "bulb" })
    expect(callout.children).toHaveLength(1)
    // 表格：rows 展开为 table + table_cell，列宽来自 colgroup、首行标题行
    const table = blocks.find((b) => b.block_type === 31) as { table: { property: Record<string, unknown> } }
    expect(table.table.property).toMatchObject({ column_width: [200, 530], header_row: true, row_size: 2, column_size: 2 })
    expect(blocks.filter((b) => b.block_type === 32)).toHaveLength(4)
    // 分栏：每列带整数 width_ratio 且非空（0.5/0.5 → 1/1）
    const cols = blocks.filter((b) => b.block_type === 25) as Array<{ grid_column: { width_ratio: number }; children: string[] }>
    expect(cols.map((c) => c.grid_column.width_ratio)).toEqual([1, 1])
    expect(cols.every((c) => c.children.length >= 1)).toBe(true)
    // 本地元数据（_ 前缀）不随请求外发
    expect(JSON.stringify(body)).not.toContain("_image")
    expect(JSON.stringify(body)).not.toContain("_diagram")
  })

  test("import_xml：标签未闭合报可读错误；只写 title 报无可导入块", async () => {
    const { tools, records } = makeTools(() => jsonResponse({ code: 0, msg: "success", data: {} }))
    const unclosed = await tools.import_xml.execute({ content: "<p>正文" }, ctx())
    expect(unclosed.output).toContain("❌")
    expect(unclosed.output).toContain("未闭合")
    const onlyTitle = await tools.import_xml.execute({ content: "<title>只有标题</title>" }, ctx())
    expect(onlyTitle.output).toContain("没有可导入的块")
    expect(records.some((r) => r.url.includes("/descendant"))).toBe(false)
  })

  test("lint_doc：报告层级跳级/连续标题/长段落/表格列宽/代码语言/空段落，并附块 id 与修复建议", async () => {
    const blocks = [
      { block_id: "page", block_type: 1, children: ["h1", "h3", "h3b", "p1", "p2", "tbl", "code1", "empty"] },
      { block_id: "h1", block_type: 3, parent_id: "page", heading1: { elements: [{ text_run: { content: "结论" } }] } },
      { block_id: "h3", block_type: 5, parent_id: "page", heading3: { elements: [{ text_run: { content: "细节" } }] } },
      { block_id: "h3b", block_type: 5, parent_id: "page", heading3: { elements: [{ text_run: { content: "另一节" } }] } },
      { block_id: "p1", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "长".repeat(320) } }] } },
      { block_id: "p2", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "正常段落" } }] } },
      { block_id: "tbl", block_type: 31, parent_id: "page", children: [], table: { property: { column_size: 2, column_width: [60, 670], header_row: false } } },
      { block_id: "code1", block_type: 14, parent_id: "page", code: { style: { language: 1 }, elements: [{ text_run: { content: "x" } }] } },
      { block_id: "empty", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "" } }] } },
    ]
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks")) return jsonResponse({ code: 0, msg: "success", data: { items: blocks, has_more: false } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const r = await tools.lint_doc.execute({ document_id: "doc_1" }, ctx())
    expect(r.output).toContain("排版体检")
    for (const rule of ["标题层级跳级", "连续标题", "段落过长", "表格列宽过窄", "表格缺表头", "代码块缺语言", "空段落"]) {
      expect(r.output).toContain(rule)
    }
    expect(r.output).toContain("tbl") // 报告带 block_id，便于直接精修
    expect(r.output).toContain("set_table_width")
    expect(r.output).toContain("warn")
  })

  test("lint_doc：文档无问题时给出明确结论", async () => {
    const blocks = [
      { block_id: "page", block_type: 1, children: ["h1", "p1", "tbl"] },
      { block_id: "h1", block_type: 3, parent_id: "page", heading1: { elements: [{ text_run: { content: "结论" } }] } },
      { block_id: "p1", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "正文" } }] } },
      { block_id: "tbl", block_type: 31, parent_id: "page", children: [], table: { property: { column_size: 2, column_width: [365, 365], header_row: true } } },
    ]
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks")) return jsonResponse({ code: 0, msg: "success", data: { items: blocks, has_more: false } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const r = await tools.lint_doc.execute({ document_id: "doc_1" }, ctx())
    expect(r.output).toContain("未发现排版问题")
  })
})

describe("预检 / 大纲 / 跨块替换（纯函数）", () => {
  test("xmlProfile：顶层块、总块（含子树）、字数与类型分布", () => {
    const p = xmlProfile([
      { block_type: 3, heading1: { elements: [{ text_run: { content: "标题" } }] } },
      { block_type: 19, callout: { background_color: 5 }, children: [{ block_type: 2, text: { elements: [{ text_run: { content: "提示" } }] } }] },
    ])
    expect(p.topLevel).toBe(2)
    expect(p.total).toBe(3)
    expect(p.chars).toBe(4)
    expect(p.counts).toMatchObject({ heading1: 1, callout: 1, text: 1 })
  })

  test("docOutline：按文档流抽标题并统计每节顶层块数", () => {
    const items = [
      { block_id: "page", block_type: 1, children: ["h1", "p1", "h2", "p2", "p3"] },
      { block_id: "h1", block_type: 3, parent_id: "page", heading1: { elements: [{ text_run: { content: "1 结论" } }] } },
      { block_id: "p1", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "正文" } }] } },
      { block_id: "h2", block_type: 4, parent_id: "page", heading2: { elements: [{ text_run: { content: "1.1 细节" } }] } },
      { block_id: "p2", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "a" } }] } },
      { block_id: "p3", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "b" } }] } },
    ]
    const { entries, hasPage } = docOutline(items)
    expect(hasPage).toBe(true)
    expect(entries).toEqual([
      { level: 1, text: "1 结论", blockId: "h1", sectionBlocks: 1 },
      { level: 2, text: "1.1 细节", blockId: "h2", sectionBlocks: 2 },
    ])
    expect(docOutline([{ block_id: "p", block_type: 2 }]).entries).toEqual([])
  })

  test("replaceInBlocks：逐 run 替换保留样式，跨 run 命中单列", () => {
    const items = [
      { block_id: "b1", block_type: 2, text: { elements: [{ text_run: { content: "旧术词出现两次：旧术词" } }] } },
      { block_id: "b2", block_type: 2, text: { elements: [{ text_run: { content: "前缀旧", text_element_style: { bold: true } } }, { text_run: { content: "术词后" } }] } },
      { block_id: "b3", block_type: 2, text: { elements: [{ text_run: { content: "无关内容" } }] } },
    ]
    const plan = replaceInBlocks(items, { pattern: "旧术词", replacement: "新术词" })
    expect(plan.total).toBe(3)
    expect(plan.updates.map((u) => u.blockId)).toEqual(["b1"]) // b2 跨 run，无法逐段替换
    expect(plan.hits[0].count).toBe(2)
    expect(plan.crossRun.map((h) => h.blockId)).toEqual(["b2"])
    // 样式保留：仅 content 变
    const els = plan.updates[0].elements as Array<{ text_run: { content: string } }>
    expect(els[0].text_run.content).toBe("新术词出现两次：新术词")
  })

  test("replaceInBlocks：limit 截断与 blockIds 限定、正则模式、非法正则报错", () => {
    const items = ["a1", "a2", "a3"].map((id, i) => ({ block_id: id, block_type: 2, text: { elements: [{ text_run: { content: `X${i}` } }] } }))
    const limited = replaceInBlocks(items, { pattern: "X", replacement: "Y", limit: 2 })
    expect(limited.updates).toHaveLength(2)
    expect(limited.skipped).toBe(1)
    const scoped = replaceInBlocks(items, { pattern: "X", replacement: "Y", blockIds: ["a2"] })
    expect(scoped.updates.map((u) => u.blockId)).toEqual(["a2"])
    const regex = replaceInBlocks(items, { pattern: "X\\d", replacement: "Z", regex: true })
    expect(regex.updates).toHaveLength(3)
    expect(() => replaceInBlocks(items, { pattern: "(", replacement: "x", regex: true })).toThrow(/不合法/)
    // 字面模式：正则元字符不当正则用
    const literal = replaceInBlocks([{ block_id: "b", block_type: 2, text: { elements: [{ text_run: { content: "a.b" } }] } }], { pattern: "a.b", replacement: "c" })
    expect(literal.updates).toHaveLength(1)
  })

  test("replace_text 工具：dry_run 预览不写入；正式执行走 batch_update", async () => {
    const blocks = [
      { block_id: "page", block_type: 1, children: ["t1"] },
      { block_id: "t1", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "旧称：旧称方案" } }] } },
    ]
    const { tools, records } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks")) return jsonResponse({ code: 0, msg: "success", data: { items: blocks, has_more: false } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const preview = await tools.replace_text.execute({ document_id: "doc_1", pattern: "旧称", replacement: "新称", dry_run: true }, ctx())
    expect(preview.output).toContain("预演")
    expect(preview.output).toContain("t1")
    expect(preview.output).toContain("共命中 2 处")
    expect(records.some((r) => r.init?.method === "PATCH")).toBe(false) // 预演零写入

    const done = await tools.replace_text.execute({ document_id: "doc_1", pattern: "旧称", replacement: "新称" }, ctx())
    expect(done.output).toContain("已替换 1/1 个块")
    const patch = records.find((r) => r.url.includes("/blocks/batch_update"))
    const body = JSON.parse(String(patch!.init?.body)) as { requests: Array<{ block_id: string; update_text_elements: { elements: Array<{ text_run: { content: string } }> } }> }
    expect(body.requests[0].block_id).toBe("t1")
    expect(body.requests[0].update_text_elements.elements[0].text_run.content).toBe("新称：新称方案")
  })

  test("import_xml dry_run：出画像与问题清单，且不创建文档、不写入块", async () => {
    const { tools, records } = makeTools(() => jsonResponse({ code: 0, msg: "success", data: {} }))
    const xml = '<title>预检</title><h1 seq="auto">结论</h1><p>正文</p><img path="@./missing.png"/>'
    const r = await tools.import_xml.execute({ content: xml, dry_run: true }, ctx())
    expect(r.output).toContain("预检（dry_run，未写入任何内容）")
    expect(r.output).toContain("字数约")
    expect(r.output).toContain("图片文件不存在")
    expect(records.length).toBe(0) // 零请求：既不建文档也不写块
  })

  test("get_doc_blocks outline：只返回大纲（标题 + block_id + 节内块数）", async () => {
    const blocks = [
      { block_id: "page", block_type: 1, children: ["h1", "p1"] },
      { block_id: "h1", block_type: 3, parent_id: "page", heading1: { elements: [{ text_run: { content: "1 甲" } }] } },
      { block_id: "p1", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "正文" } }] } },
    ]
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks")) return jsonResponse({ code: 0, msg: "success", data: { items: blocks, has_more: false } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const r = await tools.get_doc_blocks.execute({ document_id: "doc_1", outline: true }, ctx())
    expect(r.output).toContain("文档大纲（1 个标题）")
    expect(r.output).toContain("- h1 1 甲  [h1] 节内块 1")
    expect(r.output).toContain("get_doc_text")
  })
})

describe("紧凑块视图与命中上下文（读-改闭环）", () => {
  const docBlocks = [
    { block_id: "page", block_type: 1, children: ["h1", "p1", "tbl", "cal"] },
    { block_id: "h1", block_type: 3, parent_id: "page", heading1: { elements: [{ text_run: { content: "1 部署" } }] } },
    { block_id: "p1", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "先决条件：" + "很长的一段说明文字".repeat(20) } }] } },
    { block_id: "tbl", block_type: 31, parent_id: "page", children: ["c1"], table: { property: { row_size: 2, column_size: 3, header_row: true } } },
    { block_id: "c1", block_type: 32, parent_id: "tbl", children: ["t1"], table_cell: {} },
    { block_id: "t1", block_type: 2, parent_id: "c1", text: { elements: [{ text_run: { content: "单元格内文本不应出现在紧凑视图" } }] } },
    { block_id: "cal", block_type: 19, parent_id: "page", children: ["t2"], callout: { background_color: 3, emoji_id: "bulb" } },
    { block_id: "t2", block_type: 2, parent_id: "cal", text: { elements: [{ text_run: { content: "高亮块内容" } }] } },
  ]

  test("compactBlocks：一行一块（带 id 与缩进），表格不展开、page 不占行、长文本截断", () => {
    const v = compactBlocks(docBlocks, { textLimit: 20 })
    expect(v.total).toBe(5) // 不含 page，不含表格单元格子树（c1/t1）
    expect(v.lines[0]).toBe("- heading1 [h1] 1 部署")
    expect(v.lines[1]).toMatch(/^- text \[p1\] 先决条件：.*…$/)
    expect(v.lines[2]).toBe("- table [tbl] table 2×3 表头行")
    expect(v.lines[3]).toBe("- callout [cal] callout bulb bg=3")
    expect(v.lines[4]).toBe("  - text [t2] 高亮块内容") // 高亮块子块缩进一层
    expect(v.lines.join("\n")).not.toContain("单元格内文本")
  })

  test("compactBlocks：maxLines 截断并标记", () => {
    const v = compactBlocks(docBlocks, { maxLines: 2 })
    expect(v.lines).toHaveLength(2)
    expect(v.truncated).toBe(true)
    expect(v.total).toBe(5) // 总数仍如实统计
  })

  test("get_doc_blocks detail=compact：头部计数 + 行格式 + 取 id 提示", async () => {
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks")) return jsonResponse({ code: 0, msg: "success", data: { items: docBlocks, has_more: false } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const r = await tools.get_doc_blocks.execute({ document_id: "doc_1", detail: "compact" }, ctx())
    expect(r.output).toContain("紧凑块视图（5 个块")
    expect(r.output).toContain("- heading1 [h1] 1 部署")
    expect(r.output).toContain("- table [tbl] table 2×3 表头行")
    expect(r.output).not.toContain("单元格内文本")
  })

  test("find_blocks：默认只列命中块并提示上下文参数；传参后展开同父相邻块（▶ 命中 / · 上下文）", async () => {
    const blocks = [
      { block_id: "page", block_type: 1, children: ["h1", "p0", "p1", "p2", "p3"] },
      { block_id: "h1", block_type: 3, parent_id: "page", heading1: { elements: [{ text_run: { content: "部署流程" } }] } },
      { block_id: "p0", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "前置说明" } }] } },
      { block_id: "p1", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "命中行：部署需要审批" } }] } },
      { block_id: "p2", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "后续步骤" } }] } },
      { block_id: "p3", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "无关" } }] } },
    ]
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks")) return jsonResponse({ code: 0, msg: "success", data: { items: blocks, has_more: false } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const plain = await tools.find_blocks.execute({ document_id: "doc_1", query: "审批" }, ctx())
    expect(plain.output).toContain("找到 1 个包含「审批」的块")
    expect(plain.output).toContain("p1")
    expect(plain.output).toContain("context_before")
    expect(plain.output).not.toContain("▶")

    const withCtx = await tools.find_blocks.execute({ document_id: "doc_1", query: "审批", context_before: 1, context_after: 1 }, ctx())
    expect(withCtx.output).toContain("· [text] p0 前置说明")
    expect(withCtx.output).toContain("▶ [text] p1 命中行：部署需要审批")
    expect(withCtx.output).toContain("· [text] p2 后续步骤")
    expect(withCtx.output.indexOf("p0")).toBeLessThan(withCtx.output.indexOf("▶"))
    expect(withCtx.output.indexOf("▶")).toBeLessThan(withCtx.output.indexOf("p2"))
    expect(withCtx.output).not.toContain("p3") // 超出上下文窗口
  })

  test("blockText：@人与公式等非文本元素取可读占位（不凭空消失）", () => {
    const b = { block_id: "p", block_type: 2, text: { elements: [
      { text_run: { content: "请 " } },
      { mention_user: { user_id: "ou_x" } },
      { text_run: { content: " 与 " } },
      { mention_doc: { token: "t" } },
      { text_run: { content: " 对照" } },
    ] } }
    expect(blockText(b)).toBe("请 @用户 与 @文档 对照")
  })

  test("find_blocks / 紧凑视图：容器块（callout/表格）在上下文行里给出摘要", async () => {
    const blocks = [
      { block_id: "page", block_type: 1, children: ["cal", "p1"] },
      { block_id: "cal", block_type: 19, parent_id: "page", children: ["ct"], callout: { background_color: 3, emoji_id: "bulb" } },
      { block_id: "ct", block_type: 2, parent_id: "cal", text: { elements: [{ text_run: { content: "高亮块正文" } }] } },
      { block_id: "p1", block_type: 2, parent_id: "page", text: { elements: [{ text_run: { content: "命中：审批" } }] } },
    ]
    const { tools } = makeTools((req) => {
      if (req.url.includes("/auth/v3/tenant_access_token")) return jsonResponse({ code: 0, msg: "ok", tenant_access_token: "t-abc", expire: 7200 })
      if (req.url.includes("/blocks")) return jsonResponse({ code: 0, msg: "success", data: { items: blocks, has_more: false } })
      return jsonResponse({ code: 0, msg: "success", data: {} })
    })
    const r = await tools.find_blocks.execute({ document_id: "doc_1", query: "审批", context_before: 1 }, ctx())
    expect(r.output).toContain("· [callout] cal callout bulb bg=3") // 容器块给出摘要而非空行
  })
})
