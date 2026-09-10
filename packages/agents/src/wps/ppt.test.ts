import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { pptCreateTool, pptReadTool } from "./ppt"
import { makeCtx, png1px } from "./test-ctx"

function setup(): string {
  const home = mkdtempSync(join(tmpdir(), "gebai-wps-ppt-"))
  const tmp = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(tmp, { recursive: true })
  writeFileSync(join(tmp, "dot.png"), png1px())
  return home
}

describe("ppt_create / ppt_read（pptx 往返）", () => {
  test("简式页 + 自由元素（图表/图片/表格/形状）+ 备注 + 背景，读回结构完整", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const created = await pptCreateTool.execute(
      {
        path: "deck.pptx",
        meta: { title: "测试演示" },
        slides: [
          { title: "封面标题", subtitle: "副标题", notes: "开场白", background: { color: "F5F7FA" } },
          {
            title: "数据页",
            bullets: ["要点一", "要点二"],
            elements: [
              { type: "chart", chart_type: "bar", data: [{ name: "销量", labels: ["Q1", "Q2"], values: [10, 20] }], x: 6.5, y: 1.8, w: 6, h: 4, title: "季度销量" },
              { type: "image", path: "dot.png", x: 1, y: 4, w: 1, h: 1 },
              { type: "table", rows: [["名称", "值"], ["甲", 1]], x: 1, y: 1.8, w: 4 },
              { type: "shape", shape: "roundRect", x: 5, y: 6, w: 2, h: 0.8, fill: "4472C4", text: "形状文本" },
            ],
            notes: "图表页备注",
          },
        ],
      },
      ctx,
    )
    expect(created.output).toContain("已创建")
    expect(created.output).toContain("图表×1")

    const read = await pptReadTool.execute({ path: "deck.pptx" }, ctx)
    expect(read.output).toContain("2 页")
    expect(read.output).toContain("【标题】封面标题")
    expect(read.output).toContain("副标题")
    expect(read.output).toContain("要点一")
    expect(read.output).toContain("表格 2×2")
    expect(read.output).toContain("形状文本")
    expect(read.output).toContain("图表")
    expect(read.output).toContain("备注：开场白")
    expect(read.output).toContain("备注：图表页备注")
    rmSync(home, { recursive: true, force: true })
  })

  test("4x3 布局与自定义尺寸；读回页面尺寸正确", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await pptCreateTool.execute({ path: "s43.pptx", slides: [{ title: "T" }], layout: "4x3" }, ctx)
    const r43 = await pptReadTool.execute({ path: "s43.pptx" }, ctx)
    expect(r43.output).toContain("10.00×7.50")

    await pptCreateTool.execute({ path: "cus.pptx", slides: [{ title: "T" }], layout: { width: 10, height: 6 } }, ctx)
    const rc = await pptReadTool.execute({ path: "cus.pptx" }, ctx)
    expect(rc.output).toContain("10.00×6.00")
    rmSync(home, { recursive: true, force: true })
  })

  test("输入容错：页级元素对象包裹 + 元素 type 缺失按字段推断（均回报提示）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const r = await pptCreateTool.execute(
      {
        path: "tol.pptx",
        slides: [
          // ① 页级直接传元素对象（带 type）→ 单元素页
          { type: "text", text: "页级文本", x: 1, y: 1, w: 8, h: 1 },
          // ② 元素漏 type：path → 推断 image；chart_type+data → 推断 chart；rows → 推断 table
          {
            title: "容错页",
            elements: [
              { path: "dot.png", x: 1, y: 2, w: 1, h: 1 },
              { chart_type: "bar", data: [{ name: "销量", labels: ["Q1", "Q2"], values: [1, 2] }], x: 4, y: 2, w: 5, h: 3 },
              { rows: [["列A"], ["1"]], x: 1, y: 4, w: 2 },
              { type: "shape", fill: "4472C4", text: "默认形状", x: 1, y: 6, w: 2, h: 0.6 },
            ],
          },
        ],
      },
      ctx,
    )
    expect(r.output).toContain("附加元素处理")
    expect(r.output).toContain("按字段推断为 image")
    expect(r.output).toContain("按字段推断为 chart")
    expect(r.output).toContain("按字段推断为 table")

    const read = await pptReadTool.execute({ path: "tol.pptx" }, ctx)
    expect(read.output).toContain("页级文本")
    expect(read.output).toContain("容错页")
    expect(read.output).toContain("列A")
    expect(read.output).toContain("图表")
    expect(read.output).toContain("2 页")
    rmSync(home, { recursive: true, force: true })
  })

  test("输入容错：页级 chart 字段未包 elements → 附加元素处理（图表不丢失 + 提示）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const r = await pptCreateTool.execute(
      {
        path: "t2.pptx",
        slides: [
          { title: "季度页", chart_type: "bar", data: [{ name: "销量", labels: ["Q1", "Q2"], values: [3, 6] }] },
          { title: "正常页", bullets: ["要点"] },
        ],
      },
      ctx,
    )
    expect(r.output).toContain("图表×1") // 图表未因漏包 elements 丢失
    expect(r.output).toContain("附加元素处理")
    expect(r.output).toContain("elements")
    const read = await pptReadTool.execute({ path: "t2.pptx" }, ctx)
    expect(read.output).toContain("季度页")
    expect(read.output).toContain("图表")
    rmSync(home, { recursive: true, force: true })
  })

  test("防盲覆盖 + 文件不存在/非法 pptx 错误 + slides 空数组", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await pptCreateTool.execute({ path: "g.pptx", slides: [{ title: "初版" }] }, ctx)
    const { ctx: fresh } = makeCtx(home)
    const rejected = await pptCreateTool.execute({ path: "g.pptx", slides: [{ title: "x" }] }, fresh)
    expect(rejected.output).toContain("防盲覆盖")
    await pptReadTool.execute({ path: "g.pptx" }, fresh)
    const allowed = await pptCreateTool.execute({ path: "g.pptx", slides: [{ title: "重制" }] }, fresh)
    expect(allowed.output).toContain("已创建")

    const missing = await pptReadTool.execute({ path: "none.pptx" }, ctx)
    expect(missing.output).toContain("不存在")
    writeFileSync(join(ctx.workdir, "bad.pptx"), "junk")
    const bad = await pptReadTool.execute({ path: "bad.pptx" }, ctx)
    expect(bad.output).toContain("ppt_read 失败")
    const empty = await pptCreateTool.execute({ path: "empty.pptx", slides: [] }, ctx)
    expect(empty.output).toContain("至少一页")
    rmSync(home, { recursive: true, force: true })
  })
})
