import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { wordCreateTool } from "./word"
import { excelWriteTool } from "./excel"
import { pptCreateTool } from "./ppt"
import { renderOfficeReadingView } from "./preview"
import { makeCtx, png1px } from "./test-ctx"

function setup(): string {
  const home = mkdtempSync(join(tmpdir(), "gebai-wps-preview-"))
  const tmp = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(tmp, { recursive: true })
  writeFileSync(join(tmp, "dot.png"), png1px())
  return home
}

describe("office 阅读视图渲染（files/preview?render=office 服务端取数）", () => {
  test("docx：标题/表格/列表 → 结构化 HTML，图片按出现顺序 data URI 内嵌", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await wordCreateTool.execute(
      {
        path: "r.docx",
        markdown: "# 报告标题\n\n段落 <b>含转义</b>。\n\n- 第一项\n- 第二项\n  - 嵌套项\n\n| 名称 | 数量 |\n| --- | --- |\n| 苹果 | 3 |\n\n![图](dot.png)",
      },
      ctx,
    )
    const html = await renderOfficeReadingView(join(ctx.workdir, "r.docx"))
    expect(html).toContain("<h1>")
    expect(html).toContain("报告标题")
    expect(html).toContain("&lt;b&gt;含转义&lt;/b&gt;") // 全量 HTML 转义
    expect(html).toContain("<li>第一项</li>")
    expect(html).toContain("<td>苹果</td>")
    expect(html).toContain("data:image/png;base64,")
    expect(html).toContain("阅读视图")
    rmSync(home, { recursive: true, force: true })
  })

  test("xlsx：工作表/单元格样式/合并跨列/公式回显", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await excelWriteTool.execute(
      {
        path: "d.xlsx",
        sheets: [
          {
            name: "销售",
            rows: [
              [{ value: "季度汇总", bold: true, fill: "EEF2F8" }, ""],
              ["华东", { value: "=B2+C2", bold: true }],
              ["华南", 95],
            ],
            merges: ["A1:B1"],
          },
        ],
      },
      ctx,
    )
    const html = await renderOfficeReadingView(join(ctx.workdir, "d.xlsx"))
    expect(html).toContain('class="sheet-name"')
    expect(html).toContain("销售")
    expect(html).toContain('colspan="2"') // A1:B1 合并
    expect(html).toContain("font-weight:700") // bold
    expect(html).toContain("background:#EEF2F8") // fill
    expect(html).toContain("=B2+C2") // 无缓存值公式回显
    rmSync(home, { recursive: true, force: true })
  })

  test("pptx：幻灯片大纲（页码/标题/表格/备注）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await pptCreateTool.execute(
      {
        path: "deck.pptx",
        slides: [
          { title: "封面", subtitle: "副题", notes: "开场白" },
          { title: "数据", bullets: ["要点一"], elements: [{ type: "table", rows: [["列A", "列B"], ["1", "2"]], x: 1, y: 2 }] },
        ],
      },
      ctx,
    )
    const html = await renderOfficeReadingView(join(ctx.workdir, "deck.pptx"))
    expect(html).toContain("第 1 页")
    expect(html).toContain("<h3>封面</h3>")
    expect(html).toContain("备注：开场白")
    expect(html).toContain("<td>列A</td>")
    expect(html).toContain("大纲视图")
    rmSync(home, { recursive: true, force: true })
  })

  test("非 office 扩展名拒绝", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await ctx.writeFile(join(ctx.workdir, "a.txt"), "hello")
    expect(renderOfficeReadingView(join(ctx.workdir, "a.txt"))).rejects.toThrow("不支持的预览类型")
    rmSync(home, { recursive: true, force: true })
  })
})
