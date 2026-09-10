import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { strToU8, unzipSync, zipSync } from "fflate"
import { wordAppendTool, wordCreateTool, wordReadTool } from "./word"
import { parseXml } from "./ooxml"
import { makeCtx, png1px } from "./test-ctx"

const MD = `# 项目报告

这是**加粗**与*斜体*与\`code\`和[链接](https://example.com)的段落。

## 数据

- 第一项
- 第二项
  - 嵌套项

1. 步骤一
2. 步骤二

| 名称 | 数量 |
| --- | :---: |
| 苹果 | 3 |
| 香蕉 | 5 |

> 引用一段话

\`\`\`ts
const x = 1
\`\`\`

![图](dot.png)

<!--pagebreak-->

第二页内容`

function setup(): string {
  const home = mkdtempSync(join(tmpdir(), "gebai-wps-word-"))
  const tmp = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(tmp, { recursive: true })
  writeFileSync(join(tmp, "dot.png"), png1px())
  return home
}

describe("word_create / word_read（markdown → docx → markdown 往返）", () => {
  test("富排版创建：标题/行内样式/列表/表格/引用/代码块/图片/分页全部落盘，读回结构完整", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const created = await wordCreateTool.execute({ path: "report.docx", markdown: MD, style: { footer: "第 {page} 页 / 共 {pages} 页", title: "测试报告" } }, ctx)
    expect(created.output).toContain("已创建")
    expect(created.output).toContain("标题×2")
    expect(created.output).toContain("图片×1")
    expect(created.blocks?.length).toBeGreaterThan(0)

    const read = await wordReadTool.execute({ path: "report.docx" }, ctx)
    expect(read.output).toContain("# 项目报告")
    expect(read.output).toContain("## 数据")
    expect(read.output).toContain("加粗")
    expect(read.output).toContain("- 第一项")
    expect(read.output).toContain("1. 步骤一")
    expect(read.output).toContain("| 名称 | 数量 |")
    expect(read.output).toContain("苹果")
    expect(read.output).toContain("[图片]")
    expect(read.output).toMatch(/图片 1/)
    expect(read.output).toContain("纵向") // 页面摘要（21.0×29.7cm 纵向）
    rmSync(home, { recursive: true, force: true })
  })

  test("blocks JSON 结构化输入（逐 run 样式与图片尺寸）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const r = await wordCreateTool.execute(
      {
        path: "blocks.docx",
        blocks: [
          { type: "heading", text: "章节", level: 1 },
          { type: "paragraph", runs: [{ text: "红字", color: "FF0000", bold: true }, { text: "普通" }] },
          { type: "list", ordered: true, items: ["一", { text: "二", level: 1 }] },
          { type: "image", path: "dot.png", width: 200 },
          { type: "pagebreak" },
          { type: "unknown_kind" },
        ],
      },
      ctx,
    )
    expect(r.output).toContain("已创建")
    expect(r.output).toContain("未知块类型") // 未知块记 warning 不中断
    const read = await wordReadTool.execute({ path: "blocks.docx" }, ctx)
    expect(read.output).toContain("章节")
    expect(read.output).toContain("红字")
    rmSync(home, { recursive: true, force: true })
  })

  test("landscape/页边距/页码页脚选项", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await wordCreateTool.execute({ path: "land.docx", markdown: "# 横向\n\n正文", style: { orientation: "landscape", margins: { top: 1.5, left: 2 }, footer: "{page}/{pages}" } }, ctx)
    const read = await wordReadTool.execute({ path: "land.docx" }, ctx)
    expect(read.output).toContain("横向")
    rmSync(home, { recursive: true, force: true })
  })

  test("读分页（offset/limit 块序号）与文件不存在/非 docx 错误", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await wordCreateTool.execute({ path: "p.docx", markdown: "# 一\n\n甲\n\n乙\n\n# 二\n\n丙" }, ctx)
    const page = await wordReadTool.execute({ path: "p.docx", offset: 2, limit: 2 }, ctx)
    expect(page.output).toContain("甲")
    expect(page.output).not.toContain("丙")
    expect(page.output).toContain("共 5 块")

    const missing = await wordReadTool.execute({ path: "nope.docx" }, ctx)
    expect(missing.output).toContain("不存在")
    writeFileSync(join(ctx.workdir, "bad.docx"), "not a zip")
    const bad = await wordReadTool.execute({ path: "bad.docx" }, ctx)
    expect(bad.output).toContain("word_read 失败")
    rmSync(home, { recursive: true, force: true })
  })
})

describe("第三方 docx 兼容性（python-docx 生成的单引号 XML 声明）", () => {
  test("parseXml 对单/双引号 XML 声明均可正确解析为 XML（不降级为 HTML）", () => {
    const xmlBody = '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p/></w:body></w:document>'
    for (const quote of ["'", '"']) {
      const doc = parseXml(`<?xml version=${quote}1.0${quote} encoding=${quote}UTF-8${quote} standalone=${quote}yes${quote}?>${xmlBody}`)
      expect(doc.getElementsByTagName("w:body").length).toBe(1)
    }
  })

  test("word_read 可直接读取 python-docx 生成的文件（曾误报「缺少 w:body」）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    // 先生成一份基座 docx，再把 document.xml 换成 python-docx 形态（单引号声明 + 无缓存值占位标签）
    await wordCreateTool.execute({ path: "ref.docx", markdown: "# 占位" }, ctx)
    const fxml = '<?xml version=\'1.0\' encoding=\'UTF-8\' standalone=\'yes\'?>'
    const documentXml = `${fxml}<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>`
      + `<w:p><w:r><w:t>第三方库生成的标题段落</w:t></w:r></w:p>`
      + `<w:sectPr><w:pgSz w:w="11906" w:h="16838"/></w:sectPr></w:body></w:document>`
    const files = unzipSync(readFileSync(join(ctx.workdir, "ref.docx")))
    files["word/document.xml"] = strToU8(documentXml)
    writeFileSync(join(ctx.workdir, "ref.docx"), zipSync(files))
    const read = await wordReadTool.execute({ path: "ref.docx" }, ctx)
    expect(read.output).toContain("第三方库生成的标题段落")
    expect(read.output).not.toContain("缺少 w:body")
    rmSync(home, { recursive: true, force: true })
  })
})

describe("word_append（原 XML 拼接，保留原文档格式）", () => {
  test("追加标题/段落/超链接/图片：原文与新文共存，媒体计数累加", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await wordCreateTool.execute({ path: "a.docx", markdown: "# 原文标题\n\n| 甲 | 乙 |\n| --- | --- |\n| 1 | 2 |\n\n![原图](dot.png)" }, ctx)
    const appended = await wordAppendTool.execute({ path: "a.docx", markdown: "## 附录\n\n追加段落，含[外链](https://gebai.dev)。\n\n![补图](dot.png)" }, ctx)
    expect(appended.output).toContain("已追加")

    const read = await wordReadTool.execute({ path: "a.docx" }, ctx)
    expect(read.output).toContain("原文标题") // 原文保留
    expect(read.output).toContain("| 甲 | 乙 |")
    expect(read.output).toContain("附录") // 新文可见
    expect(read.output).toContain("外链")
    expect(read.output).toMatch(/图片 2/) // 媒体累加
    rmSync(home, { recursive: true, force: true })
  })

  test("追加列表/表格/分页；目标不存在时报错引导 word_create", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await wordCreateTool.execute({ path: "b.docx", markdown: "# 标题" }, ctx)
    const r = await wordAppendTool.execute({ path: "b.docx", markdown: "- 项A\n- 项B\n\n| X | Y |\n| --- | --- |\n| 1 | 2 |\n\n<!--pagebreak-->" }, ctx)
    expect(r.output).toContain("表格×1")
    const read = await wordReadTool.execute({ path: "b.docx" }, ctx)
    expect(read.output).toContain("项A")
    expect(read.output).toContain("| X | Y |")

    const missing = await wordAppendTool.execute({ path: "none.docx", markdown: "x" }, ctx)
    expect(missing.output).toContain("word_create")
    rmSync(home, { recursive: true, force: true })
  })
})

describe("wps 写工具守卫（防盲覆盖，与全局 write 同语义）", () => {
  test("已存在未读取 → 拒绝；读取后 → 放行", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await wordCreateTool.execute({ path: "g.docx", markdown: "# 初版" }, ctx)
    // 新上下文（未读追踪）模拟另一会话视角：创建过/读过的文件视为未读
    const { ctx: fresh } = makeCtx(home)
    const rejected = await wordCreateTool.execute({ path: "g.docx", markdown: "# 覆盖" }, fresh)
    expect(rejected.output).toContain("防盲覆盖")

    await wordReadTool.execute({ path: "g.docx" }, fresh)
    const allowed = await wordCreateTool.execute({ path: "g.docx", markdown: "# 覆盖" }, fresh)
    expect(allowed.output).toContain("已创建")
    const read = await wordReadTool.execute({ path: "g.docx" }, fresh)
    expect(read.output).toContain("覆盖")
    rmSync(home, { recursive: true, force: true })
  })
})
