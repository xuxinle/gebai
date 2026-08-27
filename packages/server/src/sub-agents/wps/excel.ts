/**
 * Excel（.xlsx，CSV/TSV 读取兼容）工具：excel_read（读为 markdown/json/csv，overview 看结构）、
 * excel_write（全量建表：多工作表/公式/样式/列宽/合并/冻结/自动筛选）、excel_edit（对既有
 * 工作簿按 ops 批量修改：设值设样式/行列增删/工作表管理/合并/冻结）。读写均经 exceljs，
 * 路径与守卫与全局文件工具同规则（resolvePath 沙箱 + 防盲覆盖 + writeGuard）。
 */
import { Workbook } from "exceljs"
import type { Cell, Worksheet } from "exceljs"
import type { Tool, ToolContext } from "../../core/types"
import { truncate } from "../../core/tools"
import { asNum, blindOverwriteGuard, fileBlocks, normColor, schema, writeGuards } from "./shared"

// ---------------------------------------------------------------------------
// 基础：列号/区域解析、单元格赋值与样式
// ---------------------------------------------------------------------------

function colLetterToNum(s: string): number {
  let n = 0
  for (const c of s.toUpperCase()) {
    if (c < "A" || c > "Z") return 0
    n = n * 26 + (c.charCodeAt(0) - 64)
  }
  return n
}

function colNumToLetter(n: number): string {
  let s = ""
  while (n > 0) {
    const r = (n - 1) % 26
    s = String.fromCharCode(65 + r) + s
    n = Math.floor((n - 1) / 26)
  }
  return s
}

function parseCellRef(ref: string): { col: number; row: number } | null {
  const m = /^\s*([A-Za-z]+)(\d+)\s*$/.exec(ref)
  if (!m) return null
  return { col: colLetterToNum(m[1]), row: Number(m[2]) }
}

/** 区域解析："A1:D20"（矩形）或 "A:D"（整列，行界由调用方补全）。 */
function parseRange(range: string): { c1: number; r1: number | null; c2: number; r2: number | null } | null {
  const m = /^\s*([A-Za-z]+)(\d*)\s*:\s*([A-Za-z]+)(\d*)\s*$/.exec(range)
  if (!m) {
    const single = parseCellRef(range)
    return single ? { c1: single.col, r1: single.row, c2: single.col, r2: single.row } : null
  }
  const c1 = colLetterToNum(m[1])
  const c2 = colLetterToNum(m[3])
  if (!c1 || !c2 || (m[2] && !m[4]) || (!m[2] && m[4])) return null
  return { c1: Math.min(c1, c2), r1: m[2] ? Math.min(Number(m[2]), Number(m[4])) : null, c2: Math.max(c1, c2), r2: m[2] ? Math.max(Number(m[2]), Number(m[4])) : null }
}

const THIN_BORDER = { style: "thin" as const, color: { argb: "FFB0B0B0" } }

/** 单元格赋值：字符串 "=…" 识别为公式；{formula} 对象透传（可带 result 缓存值）。 */
function assignCellValue(cell: Cell, v: unknown): void {
  if (v == null || v === "") {
    cell.value = null
    return
  }
  if (typeof v === "string" && v.startsWith("=")) {
    cell.value = { formula: v.slice(1) }
    return
  }
  if (typeof v === "object" && v !== null && !(v instanceof Date) && "formula" in (v as Record<string, unknown>)) {
    const o = v as { formula: unknown; result?: unknown }
    // exceljs 对 result 的类型声明较窄（{} 不兼容 CellValue），运行时任意标量合法——断言绕过
    cell.value = { formula: String(o.formula).replace(/^=/, ""), ...(o.result !== undefined ? { result: o.result } : {}) } as Cell["value"]
    return
  }
  cell.value = v as string | number | boolean | Date
}

/** 单元格样式：bold/italic/fontSize/color（字色）/fill（底色）/align/valign/wrap/numberFormat/border。 */
function applyCellStyle(cell: Cell, s: Record<string, unknown>): void {
  const font: Record<string, unknown> = {}
  if (s.bold === true) font.bold = true
  if (s.italic === true) font.italic = true
  if (s.fontSize != null) font.size = asNum(s.fontSize, 11)
  const color = normColor(s.color)
  if (color) font.color = { argb: `FF${color}` }
  if (Object.keys(font).length) cell.font = font
  const fill = normColor(s.fill)
  if (fill) cell.fill = { type: "pattern", pattern: "solid", fgColor: { argb: `FF${fill}` } }
  const align: Record<string, unknown> = {}
  if (s.align === "left" || s.align === "center" || s.align === "right") align.horizontal = s.align
  if (s.valign === "top" || s.valign === "middle" || s.valign === "bottom") align.vertical = s.valign
  if (s.wrap === true) align.wrapText = true
  if (Object.keys(align).length) cell.alignment = align
  if (typeof s.numberFormat === "string" && s.numberFormat) cell.numFmt = s.numberFormat
  if (s.border === true) cell.border = { top: THIN_BORDER, left: THIN_BORDER, bottom: THIN_BORDER, right: THIN_BORDER }
}

function fmtScalar(v: unknown): string {
  if (v == null) return ""
  if (v instanceof Date) {
    const d = v
    const hasTime = d.getHours() || d.getMinutes() || d.getSeconds()
    const date = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`
    return hasTime ? `${date} ${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}` : date
  }
  if (typeof v === "number") return Number.isInteger(v) ? String(v) : String(Number(v.toFixed(10)))
  return String(v)
}

/** 单元格值 → 展示文本（读取与阅读视图预览共用：公式默认显示计算值、无缓存值回显公式原文）。 */
export function cellText(cell: Cell, formulas: boolean): string {
  const v: unknown = cell.value
  if (v == null || v === "") return ""
  if (v instanceof Date) return fmtScalar(v)
  if (typeof v === "object") {
    const o = v as Record<string, unknown>
    // 计算值模式：有公式无缓存值（本工具写入的公式在 Office 打开重算前）→ 回显公式原文并标注，
    // 防调用方误当字符串值（写入侧不计算——错误缓存值比无缓存值更误导）
    if ("formula" in o && o.formula) return formulas ? `=${o.formula}` : o.result != null ? fmtScalar(o.result) : `=${o.formula}（未计算）`
    if ("sharedFormula" in o && o.sharedFormula) return formulas ? `=${o.sharedFormula}` : o.result != null ? fmtScalar(o.result) : `=${o.sharedFormula}（未计算）`
    if ("richText" in o && Array.isArray(o.richText)) return o.richText.map((t) => (t as { text: string }).text).join("")
    if ("hyperlink" in o) return String(o.text ?? o.hyperlink ?? "")
    if ("error" in o) return String(o.error)
  }
  return fmtScalar(v)
}

/** 单元格规格归一：标量直接是值；对象 {value, ...样式}（hasValue 须在清理键位前判定）。 */
function cellSpec(v: unknown): { hasValue: boolean; value: unknown; style: Record<string, unknown> } {
  if (v !== null && typeof v === "object" && !Array.isArray(v) && !(v instanceof Date)) {
    const o = { ...(v as Record<string, unknown>) }
    const hasValue = "value" in o || "formula" in o
    const value = hasValue ? o.value ?? (o.formula != null ? `=${String(o.formula).replace(/^=/, "")}` : null) : o
    const style = o
    delete style.value
    delete style.formula
    return { hasValue, value, style }
  }
  return { hasValue: true, value: v, style: {} }
}

function sheetByNameOrIndex(wb: Workbook, key: unknown): Worksheet | null {
  if (key == null) return wb.worksheets[0] ?? null
  if (typeof key === "number" && Number.isInteger(key)) return wb.worksheets[key - 1] ?? null
  const name = String(key)
  return wb.worksheets.find((ws) => ws.name === name) ?? null
}

async function writeWorkbookBytes(wb: Workbook, abs: string, ctx: ToolContext): Promise<void> {
  const buf = await wb.xlsx.writeBuffer()
  const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf as ArrayBuffer)
  await ctx.writeBinaryFile!(abs, u8)
}

// ---------------------------------------------------------------------------
// excel_read
// ---------------------------------------------------------------------------

/** CSV/TSV 解析（引号转义感知）。 */
function parseDelimited(text: string, delim: string): string[][] {
  const rows: string[][] = []
  let row: string[] = []
  let cell = ""
  let quoted = false
  const src = text.replace(/^\uFEFF/, "")
  for (let i = 0; i < src.length; i++) {
    const c = src[i]
    if (quoted) {
      if (c === '"') {
        if (src[i + 1] === '"') {
          cell += '"'
          i++
        } else quoted = false
      } else cell += c
    } else if (c === '"') quoted = true
    else if (c === delim) {
      row.push(cell)
      cell = ""
    } else if (c === "\n") {
      row.push(cell)
      rows.push(row)
      row = []
      cell = ""
    } else if (c !== "\r") cell += c
  }
  if (cell || row.length) {
    row.push(cell)
    rows.push(row)
  }
  return rows
}

export const excelReadTool: Tool = {
  name: "excel_read",
  description:
    "读取 Excel 为 markdown 表格（.xlsx/.xlsm；.csv/.tsv 文本表格也支持）。不传 sheet 返回工作簿概览（各表名/行列数/前 3 行预览）；传 sheet（表名或 1 起始序号）读取该表。range（A1:D20 或 A:D 列区间）截取区域，maxRows（默认 200）限行，formulas:true 显示公式原文，format: markdown|json|csv。默认显示计算值——本工具/excel_write 写入的公式不带缓存计算值（Office 打开重算前），此时回显公式原文并标注（未计算），不会误当字符串值。空单元格为空串。",
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "表格路径（.xlsx/.csv/.tsv）" },
      sheet: { type: "string", description: "工作表名或 1 起始序号（缺省返回概览）" },
      range: { type: "string", description: "区域裁剪：A1:D20 或 A:D（列区间）" },
      maxRows: { type: "integer", description: "最多读取行数（默认 200，截断时尾部有提示）" },
      formulas: { type: "boolean", description: "true 显示公式原文（=SUM(...)），默认显示计算值" },
      format: { type: "string", description: "输出格式 markdown（默认）/json（rows 数组入 data，供 flow 编排）/csv" },
    },
    ["path"],
  ),
  async execute(args, ctx) {
    const abs = ctx.resolvePath(String(args.path))
    let bytes: Uint8Array
    try {
      bytes = await ctx.readBinaryFile(abs)
    } catch {
      return { output: `excel_read 失败：文件不存在或不可读（${args.path}）。目标在项目内时用 project 参数指定项目。` }
    }
    const ext = /\.([a-z0-9]+)$/i.exec(abs)?.[1].toLowerCase() ?? ""
    const format = args.format === "json" || args.format === "csv" ? args.format : "markdown"
    const formulas = args.formulas === true
    const maxRows = Math.max(1, Math.round(asNum(args.maxRows, 200)))

    if (ext === "csv" || ext === "tsv") {
      const text = new TextDecoder("utf-8").decode(bytes)
      const rows = parseDelimited(text, ext === "tsv" ? "\t" : ",")
      ctx.fileGuard?.markRead(abs)
      return renderRows(rows, format, { sheet: ext.toUpperCase(), total: rows.length, shown: rows.length, cols: Math.max(0, ...rows.map((r) => r.length)) }, abs, ctx)
    }

    const wb = new Workbook()
    try {
      await wb.xlsx.load(Buffer.from(bytes) as never)
    } catch (err) {
      return { output: `excel_read 失败：不是有效的 .xlsx（${(err as Error).message}）——旧版 .xls 请先另存为 .xlsx。` }
    }
    ctx.fileGuard?.markRead(abs)

    // sheet 未传 → 工作簿概览；传名/序号 → 解析目标表
    const ws = args.sheet == null ? null : sheetByNameOrIndex(wb, args.sheet)
    if (!ws) {
      const names = wb.worksheets.map((w, i) => `${i + 1}. ${w.name}（${w.actualRowCount} 行 × ${w.actualColumnCount} 列）`)
      return {
        output: `工作簿共 ${wb.worksheets.length} 个工作表：\n${names.join("\n")}\n传 sheet 参数（表名或序号）读取具体工作表。`,
        data: { sheets: wb.worksheets.map((w) => ({ name: w.name, rowCount: w.actualRowCount, colCount: w.actualColumnCount })) },
      }
    }

    // 区域裁剪 + 行列界：行跨度用 rowCount（actualRowCount 是非空行计数，有空行时小于最大行号）
    let c1 = 1
    let r1 = 1
    let r2 = Math.max(1, ws.rowCount || ws.actualRowCount || 1)
    if (typeof args.range === "string" && args.range.trim()) {
      const rg = parseRange(args.range)
      if (!rg) return { output: `range 参数无效（${args.range}）：应为 A1:D20 或 A:D 形态。` }
      c1 = rg.c1
      r1 = rg.r1 ?? 1
      if (rg.r2 != null) r2 = rg.r2
    }
    const totalRows = r2 - r1 + 1
    const shown = Math.min(totalRows, maxRows)
    let c2 = 0
    if (typeof args.range === "string" && args.range.trim()) {
      const rg = parseRange(args.range)!
      c2 = rg.c2
    } else {
      for (let r = r1; r < r1 + shown; r++) c2 = Math.max(c2, ws.getRow(r).cellCount)
      if (!c2) c2 = ws.actualColumnCount || 1
    }
    const rows: string[][] = []
    for (let r = r1; r < r1 + shown; r++) {
      const line: string[] = []
      for (let c = c1; c <= c2; c++) line.push(cellText(ws.getRow(r).getCell(c), formulas))
      rows.push(line)
    }
    return renderRows(rows, format, { sheet: ws.name, total: totalRows, shown, cols: c2 - c1 + 1, startRow: r1 }, abs, ctx)
  },
}

async function renderRows(
  rows: string[][],
  format: string,
  info: { sheet: string; total: number; shown: number; cols: number; startRow?: number },
  abs: string,
  ctx: ToolContext,
) {
  const capNote = info.shown < info.total ? `（第 ${info.startRow ?? 1}–${(info.startRow ?? 1) + info.shown - 1} 行，共 ${info.total} 行——range/maxRows 翻页继续）` : ""
  let output: string
  if (format === "json") {
    output = JSON.stringify(rows)
  } else if (format === "csv") {
    output = rows.map((r) => r.map((c) => (/[",\n]/.test(c) ? `"${c.replace(/"/g, '""')}"` : c)).join(",")).join("\n")
  } else {
    const esc = (s: string) => s.replace(/\|/g, "\\|").replace(/\n/g, " ")
    const [head, ...body] = rows
    const lines: string[] = []
    if (head) {
      lines.push(`| ${head.map(esc).join(" | ")} |`)
      lines.push(`| ${head.map(() => "---").join(" | ")} |`)
    }
    for (const r of body) lines.push(`| ${r.map(esc).join(" | ")} |`)
    lines.push(`（${info.sheet}：${info.total} 行 × ${info.cols} 列）${capNote}`)
    output = lines.join("\n")
  }
  const truncated = await truncate(output, "excel_read", ctx)
  return { ...truncated, blocks: fileBlocks(abs, ctx), data: format === "json" ? { sheet: info.sheet, rows } : undefined }
}

// ---------------------------------------------------------------------------
// excel_write
// ---------------------------------------------------------------------------

export const excelWriteTool: Tool = {
  name: "excel_write",
  description:
    "创建 .xlsx 工作簿（覆盖整簿——已有文件本会话未读取过时拒绝，防盲覆盖；改局部用 excel_edit）。sheets 数组每项：{name, rows, colWidths, merges, freeze, autofilter}；单元格为标量或 {value, bold, italic, color(字色 RRGGBB), fill(底色), fontSize, align, valign, wrap, numberFormat, border, hyperlink}；字符串 = 开头自动按公式写入（如 =SUM(B2:B10)，公式不含缓存计算值——Office 打开时自动重算）。单格行可直接传对象/标量（不包数组，输出会提示）。常用 numberFormat：#,##0 / 0.00% / yyyy-mm-dd。freeze 传 \"A2\" 冻结首行；autofilter:true 自动筛选（或直接传区域）。",
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "输出路径（.xlsx）" },
      sheets: {
        type: "array",
        description: "工作表数组：[{name, rows: [[单元格,…],…], colWidths: [20,12], merges: [\"A1:B1\"], freeze: \"A2\", autofilter: true}]；单元格=标量或 {value, …样式}",
      },
    },
    ["path", "sheets"],
  ),
  safeMode: false,
  async execute(args, ctx) {
    const abs = ctx.resolvePath(String(args.path))
    const guardMsg = await writeGuards(abs, ctx)
    if (guardMsg) return { output: guardMsg }
    const blindMsg = await blindOverwriteGuard(abs, String(args.path), ctx)
    if (blindMsg) return { output: blindMsg }
    if (!Array.isArray(args.sheets) || !args.sheets.length) return { output: "sheets 参数须为非空数组（至少一个工作表）。" }

    const wb = new Workbook()
    wb.creator = "GEBAI"
    const used = new Set<string>()
    const parts: string[] = []
    const warnings: string[] = []
    for (const raw of args.sheets) {
      if (!raw || typeof raw !== "object") continue
      const s = raw as Record<string, unknown>
      let name = typeof s.name === "string" && s.name.trim() ? s.name.trim() : `Sheet${used.size + 1}`
      name = name.replace(/[\\/*?:[\]]/g, "_").slice(0, 31)
      while (used.has(name)) name = `${name.slice(0, 29)}_${used.size + 1}`
      used.add(name)
      const ws = wb.addWorksheet(name)
      const rows = normalizeRows(s.rows, name, warnings)
      const rowCount = rows.length
      let maxCol = 0
      rows.forEach((row, ri) => {
        row.forEach((cv, ci) => {
          writeCell(ws.getCell(ri + 1, ci + 1), cv)
        })
        maxCol = Math.max(maxCol, row.length)
      })
      if (Array.isArray(s.colWidths)) s.colWidths.forEach((w, i) => (ws.getColumn(i + 1).width = asNum(w, 0) || undefined))
      if (Array.isArray(s.merges)) for (const mr of s.merges) if (typeof mr === "string") ws.mergeCells(mr)
      if (typeof s.freeze === "string") {
        const ref = parseCellRef(s.freeze)
        if (ref) ws.views = [{ state: "frozen", xSplit: ref.col - 1, ySplit: ref.row - 1 }]
      }
      if (s.autofilter != null) {
        const last = `${colNumToLetter(Math.max(1, maxCol))}${Math.max(1, rowCount)}`
        ws.autoFilter = s.autofilter === true ? `A1:${last}` : typeof s.autofilter === "string" ? s.autofilter : undefined
      }
      parts.push(`${name}（${rowCount} 行 × ${maxCol} 列）`)
    }
    if (!wb.worksheets.length) return { output: "sheets 参数中没有有效工作表。" }
    await writeWorkbookBytes(wb, abs, ctx)
    ctx.fileGuard?.markRead(abs)
    return {
      output: `已创建 ${args.path}：${parts.join("、")}——前端文件面板可下载。${warnings.length ? `\n注意：\n- ${warnings.join("\n- ")}` : ""}`,
      blocks: fileBlocks(abs, ctx),
    }
  },
}

/** 单元格写入（excel_write / excel_edit add_sheet 共用）：值 + 超链接 + 样式一次落位。
 *  hyperlink 为值形态（exceljs {text, hyperlink}），混在样式对象里——在此统一取出处理。 */
function writeCell(cell: Cell, cv: unknown): void {
  const spec = cellSpec(cv)
  const link = typeof spec.style.hyperlink === "string" ? spec.style.hyperlink : null
  if (link) {
    cell.value = { text: spec.value != null && spec.value !== "" ? String(spec.value) : link, hyperlink: link }
  } else if (spec.hasValue) {
    assignCellValue(cell, spec.value)
  }
  applyCellStyle(cell, spec.style)
}

/** rows 数组归一：非数组行（单格对象/标量——模型常见写法）收敛为单格行并回报 warning，不静默丢弃。 */
function normalizeRows(rows: unknown, sheetName: string, warnings: string[]): unknown[][] {
  if (!Array.isArray(rows)) return []
  return rows.map((row, ri) => {
    if (Array.isArray(row)) return row
    warnings.push(`${sheetName} 第 ${ri + 1} 行不是数组（对象/标量按单格行处理——整行多格请用数组）`)
    return [row]
  })
}

// ---------------------------------------------------------------------------
// excel_edit
// ---------------------------------------------------------------------------

export const excelEditTool: Tool = {
  name: "excel_edit",
  description:
    "编辑既有 .xlsx（按 ops 批量原子应用，一次调用多处修改）。op 类型：set（cells: [{ref, value, …样式}]，样式字段同 excel_write）/ add_sheet（name, 可选 rows）/ rename_sheet（from,to）/ delete_sheet（name）/ insert_rows|delete_rows|insert_cols|delete_cols（start 1 起始, count）/ col_width（columns: [{col, width}]）/ row_height（rows: [{row, height}]）/ merge|unmerge（ranges）/ freeze（at: \"A2\"）/ autofilter（range 或 true/false）。ops 各项可带 sheet（表名或序号，缺省第一个表）。改完自动回读校验，失败不落盘。",
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "目标工作簿路径（.xlsx，须已存在）" },
      edits: { type: "array", description: "操作数组：[{op: set|add_sheet|rename_sheet|delete_sheet|insert_rows|delete_rows|insert_cols|delete_cols|col_width|row_height|merge|unmerge|freeze|autofilter, sheet?, …}]" },
    },
    ["path", "edits"],
  ),
  safeMode: false,
  async execute(args, ctx) {
    const abs = ctx.resolvePath(String(args.path))
    const guardMsg = await writeGuards(abs, ctx)
    if (guardMsg) return { output: guardMsg }
    let bytes: Uint8Array
    try {
      bytes = await ctx.readBinaryFile(abs)
    } catch {
      return { output: `excel_edit 失败：文件不存在（${args.path}）。新建工作簿用 excel_write。` }
    }
    const wb = new Workbook()
    try {
      await wb.xlsx.load(Buffer.from(bytes) as never)
    } catch (err) {
      return { output: `excel_edit 失败：不是有效的 .xlsx（${(err as Error).message}）。` }
    }
    if (!Array.isArray(args.edits) || !args.edits.length) return { output: "edits 参数须为非空数组。" }

    const applied: string[] = []
    const failed: string[] = []
    const rowWarnings: string[] = []
    for (const raw of args.edits) {
      if (!raw || typeof raw !== "object") continue
      const op = raw as Record<string, unknown>
      const kind = String(op.op ?? "")
      const ws = sheetByNameOrIndex(wb, op.sheet)
      try {
        switch (kind) {
          case "set": {
            if (!ws) throw new Error("工作表不存在")
            let n = 0
            for (const c of Array.isArray(op.cells) ? op.cells : []) {
              if (!c || typeof c !== "object") continue
              const spec = c as Record<string, unknown>
              const ref = parseCellRef(String(spec.ref ?? ""))
              if (!ref) {
                failed.push(`set：无效单元格引用 ${String(spec.ref)}`)
                continue
              }
              writeCell(ws.getCell(ref.row, ref.col), spec)
              n++
            }
            applied.push(`set ${n} 格`)
            break
          }
          case "add_sheet": {
            const name = String(op.name ?? `Sheet${wb.worksheets.length + 1}`).replace(/[\\/*?:[\]]/g, "_").slice(0, 31)
            const nws = wb.addWorksheet(name)
            const rows = normalizeRows(op.rows, name, rowWarnings)
            const rowCount = rows.length
            let maxCol = 0
            rows.forEach((row: unknown[], ri: number) => {
              row.forEach((cv, ci) => {
                writeCell(nws.getCell(ri + 1, ci + 1), cv)
              })
              maxCol = Math.max(maxCol, row.length)
            })
            applied.push(`add_sheet ${name}（${rowCount} 行 × ${maxCol} 列）`)
            break
          }
          case "rename_sheet": {
            const target = (typeof op.from === "string" ? wb.worksheets.find((w) => w.name === op.from) : null) ?? ws
            if (!target) throw new Error(`工作表不存在: ${String(op.from ?? op.sheet)}`)
            target.name = String(op.to)
            applied.push(`rename_sheet → ${String(op.to)}`)
            break
          }
          case "delete_sheet": {
            const target = wb.worksheets.find((w) => w.name === String(op.name ?? op.sheet)) ?? ws
            if (!target) throw new Error("工作表不存在")
            wb.removeWorksheet(target.id)
            applied.push(`delete_sheet ${target.name}`)
            break
          }
          case "insert_rows":
          case "delete_rows": {
            if (!ws) throw new Error("工作表不存在")
            const start = Math.max(1, Math.round(asNum(op.start, 1)))
            const count = Math.max(1, Math.round(asNum(op.count, 1)))
            if (kind === "insert_rows") ws.spliceRows(start, 0, ...Array.from({ length: count }, () => []))
            else ws.spliceRows(start, count)
            applied.push(`${kind} @${start} ×${count}`)
            break
          }
          case "insert_cols":
          case "delete_cols": {
            if (!ws) throw new Error("工作表不存在")
            const start = Math.max(1, Math.round(asNum(op.start, 1)))
            const count = Math.max(1, Math.round(asNum(op.count, 1)))
            if (kind === "insert_cols") ws.spliceColumns(start, 0, ...Array.from({ length: count }, () => []))
            else ws.spliceColumns(start, count)
            applied.push(`${kind} @${start} ×${count}`)
            break
          }
          case "col_width": {
            if (!ws) throw new Error("工作表不存在")
            for (const c of Array.isArray(op.columns) ? op.columns : []) {
              if (!c || typeof c !== "object") continue
              const spec = c as Record<string, unknown>
              const num = typeof spec.col === "number" ? spec.col : colLetterToNum(String(spec.col ?? ""))
              if (num >= 1) ws.getColumn(num).width = asNum(spec.width, 0) || undefined
            }
            applied.push("col_width")
            break
          }
          case "row_height": {
            if (!ws) throw new Error("工作表不存在")
            for (const r of Array.isArray(op.rows) ? op.rows : []) {
              if (!r || typeof r !== "object") continue
              const spec = r as Record<string, unknown>
              const row = Math.round(asNum(spec.row, 0))
              const height = asNum(spec.height, 0)
              if (row >= 1 && height > 0) ws.getRow(row).height = height
            }
            applied.push("row_height")
            break
          }
          case "merge":
          case "unmerge": {
            if (!ws) throw new Error("工作表不存在")
            for (const r of Array.isArray(op.ranges) ? op.ranges : []) {
              if (typeof r !== "string") continue
              if (kind === "merge") ws.mergeCells(r)
              else ws.unMergeCells(r)
            }
            applied.push(kind)
            break
          }
          case "freeze": {
            if (!ws) throw new Error("工作表不存在")
            const ref = parseCellRef(String(op.at ?? "A2"))
            if (ref) ws.views = [{ state: "frozen", xSplit: ref.col - 1, ySplit: ref.row - 1 }]
            applied.push(`freeze @${String(op.at ?? "A2")}`)
            break
          }
          case "autofilter": {
            if (!ws) throw new Error("工作表不存在")
            if (op.range === false || op.value === false) ws.autoFilter = undefined
            else if (typeof op.range === "string") ws.autoFilter = op.range
            else {
              const rows = Math.max(1, ws.actualRowCount || 1)
              const cols = Math.max(1, ws.actualColumnCount || 1)
              ws.autoFilter = `A1:${colNumToLetter(cols)}${rows}`
            }
            applied.push("autofilter")
            break
          }
          default:
            failed.push(`未知 op: ${kind || "(空)"}`)
        }
      } catch (err) {
        failed.push(`${kind}: ${(err as Error).message}`)
      }
    }
    if (!applied.length) return { output: `没有成功应用的操作${failed.length ? `：\n- ${failed.join("\n- ")}` : "。"}` }

    const buf = await wb.xlsx.writeBuffer()
    const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf as ArrayBuffer)
    try {
      const check = new Workbook()
      await check.xlsx.load(Buffer.from(u8) as never)
    } catch {
      return { output: "excel_edit 失败：修改后回读校验未通过，已放弃写入，原文件未改动。" }
    }
    await ctx.writeBinaryFile!(abs, u8)
    ctx.fileGuard?.markRead(abs)
    return {
      output: `已应用 ${applied.length} 项修改至 ${args.path}：${applied.join("、")}${failed.length ? `\n未成功：\n- ${failed.join("\n- ")}` : ""}${rowWarnings.length ? `\n注意：\n- ${rowWarnings.join("\n- ")}` : ""}`,
      blocks: fileBlocks(abs, ctx),
    }
  },
}
