import { describe, expect, test } from "bun:test"
import { mkdtempSync, writeFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { excelEditTool, excelReadTool, excelWriteTool } from "./excel"
import { makeCtx } from "./test-ctx"

function setup(): string {
  return mkdtempSync(join(tmpdir(), "gebai-wps-excel-"))
}

describe("excel_write / excel_read（xlsx 往返）", () => {
  test("多表/公式/样式/列宽/冻结/筛选写入，概览与读取完整", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const written = await excelWriteTool.execute(
      {
        path: "data.xlsx",
        sheets: [
          {
            name: "销售",
            rows: [
              ["区域", "Q1", "Q2", { value: "合计", bold: true, fill: "EEF2F8" }],
              ["华东", 120, 150, { value: "=B2+C2", bold: true }],
              ["华南", 80, 95, { value: "=B3+C3" }],
            ],
            col_widths: [12, 10, 10, 12],
            freeze: "A2",
            autofilter: true,
          },
          { name: "说明", rows: [["备注"], ["示例"]] },
        ],
      },
      ctx,
    )
    expect(written.output).toContain("已创建")
    expect(written.output).toContain("销售")

    const overview = await excelReadTool.execute({ path: "data.xlsx" }, ctx)
    expect(overview.output).toContain("工作簿共 2 个工作表")
    expect(overview.output).toContain("销售")
    expect(overview.output).toContain("说明")

    const sheet = await excelReadTool.execute({ path: "data.xlsx", sheet: "销售" }, ctx)
    expect(sheet.output).toContain("华东")
    expect(sheet.output).toContain("合计")
    expect(sheet.output).toContain("=B2+C2（未计算）") // 无缓存值公式回显并标注，防误当字符串值
    const formulasMode = await excelReadTool.execute({ path: "data.xlsx", sheet: "销售", formulas: true }, ctx)
    expect(formulasMode.output).toContain("=B2+C2")
    expect(formulasMode.output).not.toContain("（未计算）")
    const byIndex = await excelReadTool.execute({ path: "data.xlsx", sheet: 2 }, ctx)
    expect(byIndex.output).toContain("示例")
    rmSync(home, { recursive: true, force: true })
  })

  test("range 区域裁剪 + formulas 公式模式 + max_rows 截断提示 + json 输出", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const rows = [["H1", "H2"], ...Array.from({ length: 5 }, (_, i) => [`a${i}`, `b${i}`])]
    await excelWriteTool.execute({ path: "r.xlsx", sheets: [{ name: "S", rows }] }, ctx)

    const clipped = await excelReadTool.execute({ path: "r.xlsx", sheet: "S", range: "A2:B3" }, ctx)
    expect(clipped.output).toContain("a0")
    expect(clipped.output).not.toContain("H1")

    const limited = await excelReadTool.execute({ path: "r.xlsx", sheet: "S", max_rows: 3 }, ctx)
    expect(limited.output).toContain("共 6 行")

    const json = await excelReadTool.execute({ path: "r.xlsx", sheet: "S", format: "json" }, ctx)
    const data = json.data as { rows: string[][] }
    expect(data.rows.length).toBe(6)
    expect(data.rows[1][0]).toBe("a0")
    rmSync(home, { recursive: true, force: true })
  })

  test("csv/tsv 读取与文件不存在/非法 xlsx 错误", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await ctx.writeFile(join(ctx.workdir, "t.csv"), 'a,b\n"x,1",2\n')
    const csv = await excelReadTool.execute({ path: "t.csv" }, ctx)
    expect(csv.output).toContain("x,1") // 引号内的分隔符不拆列
    await ctx.writeFile(join(ctx.workdir, "u.tsv"), "a\tb\n1\t2\n")
    const tsv = await excelReadTool.execute({ path: "u.tsv" }, ctx)
    expect(tsv.output).toContain("TSV")

    const missing = await excelReadTool.execute({ path: "none.xlsx" }, ctx)
    expect(missing.output).toContain("不存在")
    writeFileSync(join(ctx.workdir, "bad.xlsx"), "junk")
    const bad = await excelReadTool.execute({ path: "bad.xlsx", sheet: "S" }, ctx)
    expect(bad.output).toContain("excel_read 失败")
    rmSync(home, { recursive: true, force: true })
  })

  test("防盲覆盖：已存在未读取拒绝，读取后放行", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await excelWriteTool.execute({ path: "g.xlsx", sheets: [{ name: "S", rows: [[1]] }] }, ctx)
    const { ctx: fresh } = makeCtx(home)
    const rejected = await excelWriteTool.execute({ path: "g.xlsx", sheets: [{ name: "S", rows: [[2]] }] }, fresh)
    expect(rejected.output).toContain("防盲覆盖")
    await excelReadTool.execute({ path: "g.xlsx" }, fresh)
    const allowed = await excelWriteTool.execute({ path: "g.xlsx", sheets: [{ name: "S", rows: [[2]] }] }, fresh)
    expect(allowed.output).toContain("已创建")
    rmSync(home, { recursive: true, force: true })
  })
})

describe("excel_write 输入容错（缺陷回归：对象形式整行/超链接）", () => {
  test("单格行以对象/标量直传：收敛为单格行写入并提示，不再静默丢弃", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const r = await excelWriteTool.execute(
      {
        path: "mini.xlsx",
        sheets: [{ name: "说明", rows: [{ value: "季度统计", bold: true, font_size: 14 }, ["区域", "值"], ["华东", 100]] }],
      },
      ctx,
    )
    expect(r.output).toContain("第 1 行不是数组")
    expect(r.output).toContain("单格行处理")
    const read = await excelReadTool.execute({ path: "mini.xlsx", sheet: "说明" }, ctx)
    expect(read.output).toContain("季度统计")
    expect(read.output).toContain("华东")
    rmSync(home, { recursive: true, force: true })
  })

  test("单元格 hyperlink 写入（值形态 {text, hyperlink} 落盘可读回）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await excelWriteTool.execute(
      { path: "link.xlsx", sheets: [{ name: "S", rows: [["官网", { value: "GEBAI", hyperlink: "https://gebai.dev" }]] }] },
      ctx,
    )
    const read = await excelReadTool.execute({ path: "link.xlsx", sheet: "S" }, ctx)
    expect(read.output).toContain("GEBAI")
    // 直接验证包内超链接部件存在（缺陷 ② 的回归口径：此前 hyperlink 静默丢失）
    const { unzipFiles } = await import("./ooxml")
    const files = unzipFiles(new Uint8Array(await Bun.file(join(ctx.workdir, "link.xlsx")).arrayBuffer()))
    const relsEntry = Object.entries(files).find(([k, v]) => k.includes("worksheets/_rels") && !k.endsWith("/") && v.length > 0)
    expect(relsEntry).toBeDefined()
    expect(Buffer.from(relsEntry![1]).toString("utf-8")).toContain("hyperlink")
    expect(Buffer.from(relsEntry![1]).toString("utf-8")).toContain("gebai.dev")
    rmSync(home, { recursive: true, force: true })
  })
})

describe("excel_edit（ops 批量修改）", () => {
  test("set/add_sheet/rename/delete/行列增删/合并/列宽全套", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await excelWriteTool.execute(
      {
        path: "e.xlsx",
        sheets: [{ name: "主表", rows: [["A", "B"], ["x", "y"], ["z", "w"]] }],
      },
      ctx,
    )
    const r = await excelEditTool.execute(
      {
        path: "e.xlsx",
        edits: [
          { op: "set", sheet: "主表", cells: [{ ref: "C1", value: "C 列", bold: true, fill: "FFEEAA" }, { ref: "C2", value: "=A2&B2" }, { ref: "B3", value: 666, number_format: "#,##0" }] },
          { op: "add_sheet", name: "新表", rows: [["k", "v"], ["a", 1]] },
          { op: "rename_sheet", from: "新表", to: "改名表" },
          { op: "insert_rows", sheet: "主表", start: 2, count: 1 },
          { op: "delete_cols", sheet: "主表", start: 3, count: 1 }, // 删掉 C 列（含 set 的样式列）
          { op: "col_width", sheet: "主表", columns: [{ col: "A", width: 20 }] },
        ],
      },
      ctx,
    )
    expect(r.output).toContain("已应用 6 项")
    expect(r.output).not.toContain("未成功")

    // insert_rows 后原第 2 行下移：B4=666；delete_cols 后 C 列消失
    const read = await excelReadTool.execute({ path: "e.xlsx", sheet: "主表" }, ctx)
    expect(read.output).toContain("666")
    expect(read.output).toContain("z")
    const overview = await excelReadTool.execute({ path: "e.xlsx" }, ctx)
    expect(overview.output).toContain("改名表")

    // 删除工作表
    const del = await excelEditTool.execute({ path: "e.xlsx", edits: [{ op: "delete_sheet", name: "改名表" }] }, ctx)
    expect(del.output).toContain("delete_sheet")
    const after = await excelReadTool.execute({ path: "e.xlsx" }, ctx)
    expect(after.output).not.toContain("改名表")
    rmSync(home, { recursive: true, force: true })
  })

  test("merge/unmerge + 未知 op 报告 + 目标不存在错误", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await excelWriteTool.execute({ path: "m.xlsx", sheets: [{ name: "S", rows: [["标题", ""], ["a", "b"]] }] }, ctx)
    const merged = await excelEditTool.execute({ path: "m.xlsx", edits: [{ op: "merge", ranges: ["A1:B1"] }] }, ctx)
    expect(merged.output).toContain("merge")
    const unmerged = await excelEditTool.execute({ path: "m.xlsx", edits: [{ op: "unmerge", ranges: ["A1:B1"] }] }, ctx)
    expect(unmerged.output).toContain("unmerge")

    const unknown = await excelEditTool.execute({ path: "m.xlsx", edits: [{ op: "wat" }] }, ctx)
    expect(unknown.output).toContain("未知 op")

    const missing = await excelEditTool.execute({ path: "none.xlsx", edits: [{ op: "merge", ranges: ["A1:B1"] }] }, ctx)
    expect(missing.output).toContain("excel_write")
    rmSync(home, { recursive: true, force: true })
  })
})
