import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, copyFileSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { PDFDocument } from "pdf-lib"
import { pdfCreateTool, pdfEditTool, pdfMergeTool, pdfReadTool, pdfSplitTool } from "./pdf"
import { makeCtx, png1px } from "./test-ctx"

/** 测试 CJK 字体 fixture：Noto Sans SC（Google Fonts TTF 版，TrueType 轮廓；SIL OFL 可再分发——许可证见 test-cjk-font.OFL.txt）经
 * 实例化常规字重 + pyftsubset 子集到测试字符集。pdf-lib 对 OTF/CFF 字体按 TrueType 位置嵌入致渲染空白，
 * 自定义字体仅支持 TrueType 轮廓（pdf.ts 检测 sfnt 版本拒绝 OTTO），fixture 须用 TTF 版。该类子集字体再经 pdf-lib 二次子集化会损坏字形（渲染空白、文本层完好），
 * 故测试统一 subsetFont:false 整字嵌入（该路径渲染/提取均正常，生产系统字体默认子集化亦经像素验证）。 */
const MD = `# 项目报告

这是**关键**段落内容，包含中文与 English 混排。

## 数据汇总

- 第一项
- 第二项
  - 嵌套项

1. 步骤一
2. 步骤二

| 名称 | 数量 |
| --- | :---: |
| 项目 | 3 |
| 合计 | 8 |

> 引用一段文字

\`\`\`ts
const x = 1
\`\`\`

![图](dot.png)

<!--pagebreak-->

## 结论

第二页内容。`

function setup(): string {
  const home = mkdtempSync(join(tmpdir(), "gebai-wps-pdf-"))
  const tmp = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(tmp, { recursive: true })
  writeFileSync(join(tmp, "dot.png"), png1px())
  copyFileSync(join(import.meta.dir, "test-cjk-font.ttf"), join(tmp, "cjk.ttf"))
  return home
}

describe("pdf_create / pdf_read（markdown → PDF → 文本提取往返）", () => {
  test("中文富排版创建：标题/行内样式/列表/表格/引用/代码块/图片/分页全部落盘，读回文本完整", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const created = await pdfCreateTool.execute(
      { path: "report.pdf", markdown: MD, style: { baseFont: "cjk.ttf", subsetFont: false, footer: "第 {page} 页 / 共 {pages} 页", title: "测试报告" } },
      ctx,
    )
    expect(created.output).toContain("已创建")
    expect(created.output).toContain("标题×3")
    expect(created.output).toContain("表格×1")
    expect(created.output).toContain("图片×1")
    expect(created.output).toContain("cjk.ttf") // 字体标签为 fixture 文件名
    expect(created.blocks?.length).toBeGreaterThan(0)

    const read = await pdfReadTool.execute({ path: "report.pdf" }, ctx)
    expect(read.output).toContain("共 2 页")
    expect(read.output).toContain("项目报告")
    expect(read.output).toContain("段落内容")
    expect(read.output).toContain("第一项")
    expect(read.output).toContain("步骤一")
    expect(read.output).toContain("合计")
    expect(read.output).toContain("引用一段文字")
    expect(read.output).toContain("const x = 1")
    expect(read.output).toContain("第二页内容")
    expect(read.output).toContain("第 1 页") // 页脚页码占位逐页替换
    expect(read.output).toContain("共 2 页")
    rmSync(home, { recursive: true, force: true })
  })

  test("目录：<!--toc--> 汇总其后标题与真实页码（两遍布局）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const md = `<!--toc-->

# 概述

概述内容。

<!--pagebreak-->

# 数据汇总

汇总内容。`
    const created = await pdfCreateTool.execute({ path: "toc.pdf", markdown: md, style: { baseFont: "cjk.ttf", subsetFont: false } }, ctx)
    expect(created.output).toContain("已创建")
    expect(created.output).toContain("目录×1")

    const read = await pdfReadTool.execute({ path: "toc.pdf" }, ctx)
    expect(read.output).toContain("共 3 页") // 目录 1 页 + 正文 2 页
    expect(read.output).toContain("目录")
    expect(read.output).toContain("概述")
    expect(read.output).toContain("数据汇总")
    // 目录页（第 1 页）应含条目页码 2/3
    const page1 = read.output.split("## 第 1 页")[1]?.split("## 第 2 页")[0] ?? ""
    expect(page1).toContain("2")
    expect(page1).toContain("3")
    rmSync(home, { recursive: true, force: true })
  })

  test("blocks JSON 输入：结构化块数组（表格对齐/列宽/图片尺寸）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const created = await pdfCreateTool.execute(
      {
        path: "blocks.pdf",
        blocks: [
          { type: "heading", level: 1, text: "结构报告" },
          { type: "paragraph", runs: [{ text: "混合", bold: true }, { text: "内容" }] },
          { type: "table", header: ["名称", "数值"], rows: [[{ text: "合计" }], [{ text: "金额", color: "FF0000" }]], aligns: ["left", "center"], widths: [60, 40] },
          { type: "image", path: "dot.png", width: 100, height: 60 },
        ],
        style: { baseFont: "cjk.ttf", subsetFont: false },
      },
      ctx,
    )
    expect(created.output).toContain("已创建")
    expect(created.output).toContain("表格×1")
    expect(created.output).toContain("图片×1")
    const read = await pdfReadTool.execute({ path: "blocks.pdf" }, ctx)
    expect(read.output).toContain("结构报告")
    expect(read.output).toContain("合计")
    rmSync(home, { recursive: true, force: true })
  })

  test("纯西文走标准字体（无需 CJK 字体），回读完整", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const created = await pdfCreateTool.execute({ path: "latin.pdf", markdown: "# Hello Report\n\nSome **bold** text and `code` here." }, ctx)
    expect(created.output).toContain("已创建")
    expect(created.output).toContain("标准西文字体")
    const read = await pdfReadTool.execute({ path: "latin.pdf" }, ctx)
    expect(read.output).toContain("Hello Report")
    expect(read.output).toContain("bold")
    rmSync(home, { recursive: true, force: true })
  })

  test("防盲覆盖：已存在未读取时拒绝，读后放行", async () => {
    const home = setup()
    const { ctx, readSet } = makeCtx(home)
    await pdfCreateTool.execute({ path: "guard.pdf", markdown: "# 标题内容", style: { baseFont: "cjk.ttf", subsetFont: false } }, ctx)
    readSet.clear() // 写入成功的 markRead 视为「已读」，须清空后才能验证盲覆盖拦截
    const blind = await pdfCreateTool.execute({ path: "guard.pdf", markdown: "# 覆盖内容", style: { baseFont: "cjk.ttf", subsetFont: false } }, ctx)
    expect(blind.output).toContain("防盲覆盖")
    await pdfReadTool.execute({ path: "guard.pdf" }, ctx)
    const ok = await pdfCreateTool.execute({ path: "guard.pdf", markdown: "# 覆盖内容", style: { baseFont: "cjk.ttf", subsetFont: false } }, ctx)
    expect(ok.output).toContain("已创建")
    const read = await pdfReadTool.execute({ path: "guard.pdf" }, ctx)
    expect(read.output).toContain("覆盖内容")
    rmSync(home, { recursive: true, force: true })
  })

  test("空正文与非 CJK 字体缺失引导", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const empty = await pdfCreateTool.execute({ path: "empty.pdf", style: { baseFont: "cjk.ttf", subsetFont: false } }, ctx)
    expect(empty.output).toContain("正文为空")
    const css = await pdfCreateTool.execute({ path: "x.pdf", markdown: "# 标题" }, ctx)
    // 有系统 CJK 字体则创建成功；无字体环境返回安装引导（均合法，不崩溃）
    expect(["已创建", "未找到可用的 CJK 字体"].some((t) => css.output.includes(t))).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })
})

describe("pdf_read（选页/上限/错误形态）", () => {
  test("pages 区间选页与 maxPages 截断提示", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await pdfCreateTool.execute({ path: "multi.pdf", markdown: "# 第一页\n\n<!--pagebreak-->\n\n第二页内容。\n\n<!--pagebreak-->\n\n第三页内容。", style: { baseFont: "cjk.ttf", subsetFont: false } }, ctx)
    const p2 = await pdfReadTool.execute({ path: "multi.pdf", pages: "2" }, ctx)
    expect(p2.output).toContain("## 第 2 页")
    expect(p2.output).not.toContain("## 第 1 页")
    expect(p2.output).toContain("第二页内容")
    const cap = await pdfReadTool.execute({ path: "multi.pdf", maxPages: 2 }, ctx)
    expect(cap.output).toContain("已读前 2 页")
    const bad = await pdfReadTool.execute({ path: "multi.pdf", pages: "1-9" }, ctx)
    expect(bad.output).toContain("超出范围")
    const missing = await pdfReadTool.execute({ path: "nope.pdf" }, ctx)
    expect(missing.output).toContain("不存在")
    rmSync(home, { recursive: true, force: true })
  })

  test("非 PDF 文件报错", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    writeFileSync(join(home, "users", "default", "sessions", "s1", "tmp", "fake.pdf"), "not a pdf")
    const res = await pdfReadTool.execute({ path: "fake.pdf" }, ctx)
    expect(res.output).toContain("pdf_read 失败")
    rmSync(home, { recursive: true, force: true })
  })
})

describe("pdf_merge / pdf_split", () => {
  async function make3(ctx: ReturnType<typeof makeCtx>["ctx"], path: string): Promise<void> {
    await pdfCreateTool.execute(
      { path, markdown: "# 第一页\n\n<!--pagebreak-->\n\n第二页内容。\n\n<!--pagebreak-->\n\n第三页内容。", style: { baseFont: "cjk.ttf", subsetFont: false } },
      ctx,
    )
  }

  test("合并：全量 + 分页选取，输出页数为各输入页数之和", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await make3(ctx, "a.pdf")
    await make3(ctx, "b.pdf")
    const merged = await pdfMergeTool.execute({ path: "merged.pdf", inputs: ["a.pdf", { path: "b.pdf", pages: "1,3" }] }, ctx)
    expect(merged.output).toContain("已合并")
    expect(merged.output).toContain("共 5 页")
    const read = await pdfReadTool.execute({ path: "merged.pdf" }, ctx)
    expect(read.output).toContain("共 5 页")
    expect(read.output.match(/## 第 \d+ 页/g)?.length).toBe(5)
    const few = await pdfMergeTool.execute({ path: "x.pdf", inputs: ["a.pdf"] }, ctx)
    expect(few.output).toContain("至少两个")
    const missing = await pdfMergeTool.execute({ path: "x.pdf", inputs: ["a.pdf", "zz.pdf"] }, ctx)
    expect(missing.output).toContain("不存在")
    rmSync(home, { recursive: true, force: true })
  })

  test("拆分：ranges / every / single 三模式，产物页数正确", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await make3(ctx, "src.pdf")
    const split = await pdfSplitTool.execute({ path: "src.pdf", mode: "ranges", ranges: ["1-2", "3"], outdir: "out" }, ctx)
    expect(split.output).toContain("已将")
    expect(split.output).toContain("src-p1-2.pdf（2 页）")
    expect(split.output).toContain("src-p3.pdf（1 页）")
    const r1 = await pdfReadTool.execute({ path: "out/src-p1-2.pdf" }, ctx)
    expect(r1.output).toContain("共 2 页")
    const r2 = await pdfReadTool.execute({ path: "out/src-p3.pdf" }, ctx)
    expect(r2.output).toContain("第三页内容")

    const every = await pdfSplitTool.execute({ path: "src.pdf", every: 2, outdir: "ev" }, ctx)
    expect(every.output).toContain("src-p1-2.pdf（2 页）")
    expect(every.output).toContain("src-p3.pdf（1 页）")

    const single = await pdfSplitTool.execute({ path: "src.pdf" }, ctx)
    expect(single.output).toContain("src-p2.pdf（1 页）")
    rmSync(home, { recursive: true, force: true })
  })
})

describe("pdf_edit（页面操作/元数据/水印）", () => {
  async function make3(ctx: ReturnType<typeof makeCtx>["ctx"], path: string): Promise<void> {
    await pdfCreateTool.execute(
      { path, markdown: "# 第一页\n\n<!--pagebreak-->\n\n第二页内容。\n\n<!--pagebreak-->\n\n第三页内容。", style: { baseFont: "cjk.ttf", subsetFont: false } },
      ctx,
    )
  }

  test("delete + rotate + move：页数/旋转角/页序逐项验证", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await make3(ctx, "edit.pdf")
    const res = await pdfEditTool.execute(
      { path: "edit.pdf", ops: [{ op: "delete", pages: "2" }, { op: "rotate", pages: "1", degrees: 90 }, { op: "move", from: 2, to: 1 }] },
      ctx,
    )
    expect(res.output).toContain("删除 1 页")
    expect(res.output).toContain("旋转 1 页 90°")
    expect(res.output).toContain("现共 2 页") // 删 1 页后 2 页
    expect(res.output).toContain("第 2 页移至第 1 页")

    const doc = await PDFDocument.load(readFileSync(join(home, "users", "default", "sessions", "s1", "tmp", "edit.pdf")))
    expect(doc.getPageCount()).toBe(2)
    // move 后页序为 [原第3页（未旋转）, 原第1页（90°）]
    expect(doc.getPage(0).getRotation().angle).toBe(0)
    expect(doc.getPage(1).getRotation().angle).toBe(90)
    // move 后第 1 页应为原第 3 页内容（原第 1 页已旋转）
    const read = await pdfReadTool.execute({ path: "edit.pdf" }, ctx)
    const p1 = read.output.split("## 第 1 页")[1]?.split("## 第 2 页")[0] ?? ""
    expect(p1).toContain("第三页内容")
    rmSync(home, { recursive: true, force: true })
  })

  test("metadata + 水印：元数据可读回，水印文本落页", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await make3(ctx, "meta.pdf")
    const res = await pdfEditTool.execute(
      { path: "meta.pdf", ops: [{ op: "metadata", title: "测试标题", author: "作者名称" }, { op: "watermark", text: "DRAFT", pages: "1-2", opacity: 0.2 }] },
      ctx,
    )
    expect(res.output).toContain("设置元数据")
    expect(res.output).toContain("DRAFT")
    const read = await pdfReadTool.execute({ path: "meta.pdf" }, ctx)
    expect(read.output).toContain("测试标题") // 元数据摘要行
    expect(read.output).toContain("作者名称")
    const p1 = read.output.split("## 第 1 页")[1]?.split("## 第 2 页")[0] ?? ""
    const p3 = read.output.split("## 第 3 页")[1] ?? ""
    expect(p1).toContain("DRAFT")
    expect(p3).not.toContain("DRAFT")
    rmSync(home, { recursive: true, force: true })
  })

  test("错误形态：未知 op / 页码越界 / 文件不存在，原文件不动", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await make3(ctx, "safe.pdf")
    const before = readFileSync(join(home, "users", "default", "sessions", "s1", "tmp", "safe.pdf"))
    const unknown = await pdfEditTool.execute({ path: "safe.pdf", ops: [{ op: "burn", pages: "1" }] }, ctx)
    expect(unknown.output).toContain("未知 op")
    expect(unknown.output).toContain("原文件未改动")
    const range = await pdfEditTool.execute({ path: "safe.pdf", ops: [{ op: "delete", pages: "9" }] }, ctx)
    expect(range.output).toContain("超出范围")
    const missing = await pdfEditTool.execute({ path: "zz.pdf", ops: [{ op: "metadata", title: "t" }] }, ctx)
    expect(missing.output).toContain("不存在")
    const after = readFileSync(join(home, "users", "default", "sessions", "s1", "tmp", "safe.pdf"))
    expect(Buffer.compare(Buffer.from(before), Buffer.from(after))).toBe(0)
    rmSync(home, { recursive: true, force: true })
  })

  test("中文水印走系统字体嵌入（本机存在 CJK 字体时）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await make3(ctx, "wm.pdf")
    const res = await pdfEditTool.execute({ path: "wm.pdf", ops: [{ op: "watermark", text: "测试水印" }] }, ctx)
    const read = await pdfReadTool.execute({ path: "wm.pdf" }, ctx)
    // 有系统 CJK 字体则水印落页；无字体环境为报错分支（原文件不动），两者都不崩溃
    expect(res.output === read.output || res.output.includes("已对") || res.output.includes("CJK 字体")).toBe(true)
    if (res.output.includes("已对")) expect(read.output).toContain("测试水印")
    rmSync(home, { recursive: true, force: true })
  })
})
