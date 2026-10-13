import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync, mkdirSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { excelBatchTool } from "./batch"
import { makeCtx } from "./test-ctx"

function setup(): string {
  const home = mkdtempSync(join(tmpdir(), "gebai-wps-batch-"))
  mkdirSync(join(home, "data"), { recursive: true })
  return home
}

/** 手工构造 GBK 字节（测试数据小，直接映射——避免测试自身依赖被测编码器）。 */

describe("excel_batch：源装载与多文件合并", () => {
  test("csv+xlsx glob 多源合并（表头对齐、列序不同、xlsx 公式取缓存值）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await ctx.writeFile(join(ctx.workdir, "a.csv"), "区域,金额\n华东,100\n华南,50\n")
    await ctx.writeFile(join(ctx.workdir, "b.csv"), "金额,区域,备注\n30,华东,Q1\n70,华南,Q2\n") // 列序相反
    // xlsx：经 excel_batch 自身 xlsx 输出构造
    await excelBatchTool.execute({ rows: [["区域", "金额"], ["华北", 20]], actions: [], outputs: [{ path: "c.xlsx", format: "xlsx" }] }, ctx)

    const r = await excelBatchTool.execute(
      {
        files: ["a.csv", "b.csv", "c.xlsx"],
        actions: [{ op: "group", by: "区域", aggs: [{ col: "金额", fn: "sum", as: "合计" }] }],
        outputs: [{ path: "汇总.xlsx", format: "xlsx" }],
      },
      ctx,
    )
    expect(r.output).toContain("excel_batch 完成")
    expect(r.output).toContain("合并 3 个来源")
    expect(r.output).toContain("→ 3 列") // 区域+金额+备注（b.csv 独有列并入）
    const data = r.data as { headers: string[]; rows: number; inputs: string[] }
    expect(data.rows).toBe(3) // 华东/华南/华北
    expect(data.inputs.length).toBe(3)
    // 合并正确性：b.csv 列序相反但金额已归位（华东 100+30、华南 50+70）
    expect(r.output).toContain("| 华东 | 130 |")
    expect(r.output).toContain("| 华南 | 120 |")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("files 显式未命中报错（不静默回退 rows——模式写错应早失败）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const r = await excelBatchTool.execute(
      { files: ["none/*.csv"], rows: [["a"], [1]], outputs: [{ path: "inline.csv", format: "csv", encoding: "utf-8" }] },
      ctx,
    )
    expect(r.output).toContain("没有可导入的文件")
    expect(r.output).toContain("none/*.csv 未匹配")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("GBK csv 自动探测导入；导出 GBK csv 可回读（编码回环）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    // 「区域」GBK = D8 F4 / C7 F8；「金额」= BD F0 / B6 EE；华东 = BB AA / B6 AB；用 TextEncoder 造不出，逐字节写
    const gbkHeader = Uint8Array.from([0xd8, 0xf4, 0xc7, 0xf8, 0x2c, 0xbd, 0xf0, 0xb6, 0xee, 0x0a])
    const gbkRow = Uint8Array.from([0xbb, 0xaa, 0xb6, 0xab, 0x2c, 0x31, 0x30, 0x30, 0x0a])
    const bytes = new Uint8Array(gbkHeader.length + gbkRow.length)
    bytes.set(gbkHeader)
    bytes.set(gbkRow, gbkHeader.length)
    await ctx.writeBinaryFile!(join(ctx.workdir, "gbk.csv"), bytes)
    const r = await excelBatchTool.execute(
      {
        files: ["gbk.csv"],
        outputs: [
          { path: "out-gbk.csv", format: "csv", encoding: "gbk" },
          { path: "out-utf8.csv", format: "csv", encoding: "utf-8" },
        ],
      },
      ctx,
    )
    expect(r.output).toContain("华东") // 解码正确
    expect(r.output).not.toContain("已跳过")
    // GBK 回读：字节应与源数据等价（表头+数据行重编码）
    const round = new Uint8Array(await Bun.file(join(ctx.workdir, "out-gbk.csv")).arrayBuffer())
    // 找「华东,100」那行的 GBK 字节
    const expectRow = Uint8Array.from([0xbb, 0xaa, 0xb6, 0xab, 0x2c, 0x31, 0x30, 0x30])
    const rowStr = Buffer.from(round).toString("binary")
    expect(rowStr.includes(Buffer.from(expectRow).toString("binary"))).toBe(true)
    // utf-8 导出可直接文本验证
    const utf8 = await Bun.file(join(ctx.workdir, "out-utf8.csv")).text()
    expect(utf8).toContain("华东")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("数值保守转换：前导零/手机号保文本，千分位/百分比归数", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await ctx.writeFile(join(ctx.workdir, "t.csv"), "编号,手机号,金额,占比\n0012,13800138000,\"1,234\",20%\n")
    const r = await excelBatchTool.execute({ files: ["t.csv"], outputs: [{ path: "o.json", format: "json" }] }, ctx)
    const text = await Bun.file(join(ctx.workdir, "o.json")).text()
    const obj = JSON.parse(text)
    expect(obj[0]["编号"]).toBe("0012")
    expect(obj[0]["手机号"]).toBe("13800138000")
    expect(obj[0]["金额"]).toBe(1234)
    expect(obj[0]["占比"]).toBe(0.2)
    expect(r.output).toContain("excel_batch 完成")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)
})

describe("excel_batch：动作管道", () => {
  const SALES = [
    ["区域", "产品", "数量", "单价"],
    ["华东", "A", 10, 5.5],
    ["华东", "B", 3, 20],
    ["华南", "A", 7, 5.5],
    ["华南", "C", 2, 99],
    ["华北", "B", 1, 20],
  ]

  test("filter 多操作符 + sort + top", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const r = await excelBatchTool.execute(
      {
        rows: SALES,
        actions: [
          { op: "filter", where: { 数量: { gt: 2 } } },
          { op: "sort", by: "数量", order: "desc" },
          { op: "top", n: 2 },
        ],
        outputs: [{ path: "f.xlsx", format: "xlsx" }],
      },
      ctx,
    )
    const data = r.data as { rows: number }
    expect(data.rows).toBe(2)
    expect(r.output).toContain("filter（数量{\"gt\":2}）：5 → 3 行")
    expect(r.output).toContain("top（前 2 行）：3 → 2 行")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("compute 表达式（列引用/IF/ROUND/字符串函数）+ fill + dedupe", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await excelBatchTool.execute(
      {
        rows: [
          ["区域", "数量", "单价", "备注"],
          ["华东", 10, 5.5, ""],
          ["华东", 10, 5.5, "重复行"],
          ["华南", 3, 20, ""],
        ],
        actions: [
          { op: "fill", col: "备注", value: "—" },
          { op: "compute", cols: [
            { name: "金额", expr: "ROUND({数量}*{单价}, 2)" },
            { name: "档位", expr: 'IF({金额} >= 60, "大单", "小单")' },
            { name: "区域码", expr: 'CONCAT(UPPER(LEFT({区域},1)), "区")' },
          ] },
          { op: "dedupe", cols: ["区域", "数量", "单价"] },
        ],
        outputs: [{ path: "c.json", format: "json" }],
      },
      ctx,
    )
    const text = await Bun.file(join(ctx.workdir, "c.json")).text()
    const arr = JSON.parse(text)
    expect(arr.length).toBe(2) // 去重掉一行
    expect(arr[0]["金额"]).toBe(55)
    expect(arr[0]["档位"]).toBe("小单")
    expect(arr[1]["档位"]).toBe("大单")
    expect(arr[0]["区域码"]).toBe("华区")
    expect(arr[0]["备注"]).toBe("—")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("group 多列分组多聚合 + aggregate 汇总行", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await excelBatchTool.execute(
      {
        rows: SALES,
        actions: [{ op: "group", by: ["区域", "产品"], aggs: [{ col: "数量", fn: "sum", as: "总数量" }, { col: "单价", fn: "avg", as: "均价" }] }],
        outputs: [{ path: "g.json", format: "json" }],
      },
      ctx,
    )
    const arr = JSON.parse(await Bun.file(join(ctx.workdir, "g.json")).text())
    expect(arr.length).toBe(5)
    const hd = arr.find((x: Record<string, unknown>) => x["区域"] === "华东" && x["产品"] === "A")
    expect(hd["总数量"]).toBe(10)
    expect(hd["均价"]).toBe(5.5)

        const agg = await excelBatchTool.execute(
      { rows: SALES, actions: [{ op: "aggregate", cols: [{ col: "数量", fn: "sum", as: "总量" }, { col: "单价", fn: "max", as: "最高价" }] }], outputs: [{ path: "a.json", format: "json" }] },
      ctx,
    )
    const aggArr = JSON.parse(await Bun.file(join(ctx.workdir, "a.json")).text())
    expect(aggArr[0]["总量"]).toBe(23)
    expect(aggArr[0]["最高价"]).toBe(99)
    expect(agg.output).toContain("aggregate")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("join（left/inner，键列不重复带出、重名列加后缀）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await ctx.writeFile(join(ctx.workdir, "targets.csv"), "区域,目标\n华东,500\n华南,400\n西部,300\n")
    const left = await excelBatchTool.execute(
      {
        rows: SALES,
        actions: [{ op: "join", path: "targets.csv", on: "区域", type: "left" }],
        outputs: [{ path: "j.json", format: "json" }],
      },
      ctx,
    )
    expect(left.output).toContain("join")
    const arr = JSON.parse(await Bun.file(join(ctx.workdir, "j.json")).text())
    expect(arr.length).toBe(5) // left join 保留全部左行
    expect(arr.find((x: Record<string, unknown>) => x["区域"] === "华北")["目标"]).toBeNull() // 未匹配补空
    expect(arr[0]["目标"]).toBe(500)

    const inner = await excelBatchTool.execute(
      {
        rows: SALES,
        actions: [{ op: "join", path: "targets.csv", on: "区域", type: "inner" }],
        outputs: [{ path: "j2.json", format: "json" }],
      },
      ctx,
    )
    expect(inner.output).toContain("join")
    const arr2 = JSON.parse(await Bun.file(join(ctx.workdir, "j2.json")).text())
    expect(arr2.length).toBe(4) // 华北无目标被过滤
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("pivot 行列透视 + 合计行", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const r = await excelBatchTool.execute(
      {
        rows: SALES,
        actions: [
          { op: "compute", cols: [{ name: "金额", expr: "{数量}*{单价}" }] },
          { op: "pivot", rows: "区域", cols: "产品", values: "金额", agg: "sum" },
        ],
        outputs: [{ path: "p.json", format: "json" }],
      },
      ctx,
    )
    void r
    const arr = JSON.parse(await Bun.file(join(ctx.workdir, "p.json")).text())
    expect(arr.length).toBe(4) // 3 区域 + 合计
    const hd = arr.find((x: Record<string, unknown>) => x["区域"] === "华东")
    expect(hd["A"]).toBe(55)
    expect(hd["B"]).toBe(60)
    expect(hd["C"]).toBeNull()
    const total = arr.find((x: Record<string, unknown>) => x["区域"] === "合计")
    expect(total["A"]).toBe(93.5)
    expect(r.output).toContain("pivot")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("select_cols/rename_cols/slice + 列序号/字母引用", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await excelBatchTool.execute(
      {
        rows: SALES,
        actions: [
          { op: "select_cols", cols: [1, "D"] }, // 序号 1（区域）+ 字母 D（单价）
          { op: "rename_cols", map: { 区域: "地区" } },
          { op: "slice", from: 1, to: 2 },
        ],
        outputs: [{ path: "s.json", format: "json" }],
      },
      ctx,
    )
    const arr = JSON.parse(await Bun.file(join(ctx.workdir, "s.json")).text())
    expect(arr.length).toBe(2)
    expect(arr[0]["地区"]).toBe("华东")
    expect(arr[0]["单价"]).toBe(5.5)
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("filter 等值：日期/型号类文本值不做表达式误判（回归：旧启发式把 2024-01-01 算成 2022 全通过）；expr 操作符行级求值", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    // A. 日期字符串等值：只应命中 1 行
    const A = await excelBatchTool.execute(
      {
        rows: [["日期", "v"], ["2024-01-01", 1], ["2024-01-02", 2], ["1999-01-01", 3]],
        actions: [{ op: "filter", where: { 日期: "2024-01-01" } }],
        outputs: [{ path: "ra.json", format: "json" }],
      },
      ctx,
    )
    const aArr = JSON.parse(await Bun.file(join(ctx.workdir, "ra.json")).text())
    expect(aArr.length).toBe(1)
    expect(aArr[0]["v"]).toBe(1)
    expect(A.output).toContain("filter（日期2024-01-01）：3 → 1 行")

    // B. expr 操作符：行级表达式跨列引用
    const B = await excelBatchTool.execute(
      {
        rows: [["x", "v"], ["a", 1], ["b", 2], ["c", 3]],
        actions: [{ op: "filter", where: { x: { expr: "{v} > 1" } } }],
        outputs: [{ path: "rb.json", format: "json" }],
      },
      ctx,
    )
    void B
    const bArr = JSON.parse(await Bun.file(join(ctx.workdir, "rb.json")).text())
    expect(bArr.length).toBe(2)
    expect(bArr.map((r: Record<string, unknown>) => r["x"])).toEqual(["b", "c"])

    // C. 含列引用的真表达式串仍按表达式求值
    const C = await excelBatchTool.execute(
      {
        rows: [["x", "v"], ["a", 1], ["b", 2]],
        actions: [{ op: "filter", where: { x: "{v} >= 2" } }],
        outputs: [{ path: "rc.json", format: "json" }],
      },
      ctx,
    )
    void C
    const cArr = JSON.parse(await Bun.file(join(ctx.workdir, "rc.json")).text())
    expect(cArr.length).toBe(1)
    expect(cArr[0]["x"]).toBe("b")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("compute 除零：NaN/Infinity 归空（xlsx 可落盘、json/csv 可序列化），不报错不脏值", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const r = await excelBatchTool.execute(
      {
        rows: [["a", "b"], [10, 2], [10, 0]],
        actions: [{ op: "compute", cols: [{ name: "ratio", expr: "{a}/{b}" }] }],
        outputs: [
          { path: "dz.xlsx", format: "xlsx" },
          { path: "dz.json", format: "json" },
          { path: "dz.csv", format: "csv", encoding: "utf-8" },
        ],
      },
      ctx,
    )
    expect(r.output).toContain("excel_batch 完成")
    const arr = JSON.parse(await Bun.file(join(ctx.workdir, "dz.json")).text())
    expect(arr[0]["ratio"]).toBe(5)
    expect(arr[1]["ratio"]).toBeNull() // 除零 → 空（非 NaN/Infinity）
    const csv = await Bun.file(join(ctx.workdir, "dz.csv")).text()
    expect(csv).not.toContain("NaN")
    // xlsx 可被 excel_read 读回（NaN 落盘会导致 exceljs 写入报错）
    const { excelReadTool } = await import("./excel")
    const back = await excelReadTool.execute({ path: "dz.xlsx", sheet: 1 }, ctx)
    expect(back.output).toContain("ratio")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("未知动作与列不存在：原子失败（不写任何输出）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const bad = await excelBatchTool.execute({ rows: SALES, actions: [{ op: "wat" }], outputs: [{ path: "x.xlsx", format: "xlsx" }] }, ctx)
    expect(bad.output).toContain("未知动作")
    expect(bad.output).toContain("未写出任何输出")
    const badCol = await excelBatchTool.execute({ rows: SALES, actions: [{ op: "filter", where: { 不存在的列: 1 } }], outputs: [{ path: "x.xlsx", format: "xlsx" }] }, ctx)
    expect(badCol.output).toContain("不存在")
    expect(await Bun.file(join(ctx.workdir, "x.xlsx")).exists()).toBe(false)
    rmSync(home, { recursive: true, force: true })
  }, 30_000)
})

describe("excel_batch：输出与守卫", () => {
  test("xlsx 输出可被 excel_read 读回（表头/冻结/数值类型）", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await excelBatchTool.execute({ rows: [["名称", "值"], ["甲", 1], ["乙", 2]], outputs: [{ path: "o.xlsx", format: "xlsx" }] }, ctx)
    const { excelReadTool } = await import("./excel")
    const read = await excelReadTool.execute({ path: "o.xlsx", sheet: 1 }, ctx)
    expect(read.output).toContain("名称")
    expect(read.output).toContain("甲")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("防盲覆盖：已存在未读取拒绝；读取后放行", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    await excelBatchTool.execute({ rows: [["a"], [1]], outputs: [{ path: "g.csv", format: "csv", encoding: "utf-8" }] }, ctx)
    const { ctx: fresh } = makeCtx(home)
    const rejected = await excelBatchTool.execute({ rows: [["a"], [2]], outputs: [{ path: "g.csv", format: "csv", encoding: "utf-8" }] }, fresh)
    expect(rejected.output).toContain("防盲覆盖")
    // 读取后放行（excel_read 读回即计入已读）
    const { excelReadTool } = await import("./excel")
    await excelReadTool.execute({ path: "g.csv" }, fresh)
    const allowed = await excelBatchTool.execute({ rows: [["a"], [3]], outputs: [{ path: "g.csv", format: "csv", encoding: "utf-8" }] }, fresh)
    expect(allowed.output).toContain("g.csv")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)

  test("glob 未命中报错清晰；空 outputs 拒绝", async () => {
    const home = setup()
    const { ctx } = makeCtx(home)
    const miss = await excelBatchTool.execute({ files: ["none/*.csv"], outputs: [{ path: "o.csv", format: "csv" }] }, ctx)
    expect(miss.output).toContain("没有可导入的文件")
    const noOut = await excelBatchTool.execute({ rows: [["a"], [1]], outputs: [] }, ctx)
    expect(noOut.output).toContain("outputs")
    rmSync(home, { recursive: true, force: true })
  }, 30_000)
})
