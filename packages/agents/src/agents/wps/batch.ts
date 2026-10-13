/**
 * excel_batch：批量表格数据管道（导入→清洗→统计→导出，一站式声明式工具）。
 * 解决的问题：此前批量处理（多文件合并/筛选/分组统计/透视/格式转换）只能靠 py+pandas 写脚本——
 * 环境依赖重、编排成本高；本工具把高频数据动作收编为声明式管道，模型只传文件路径与动作描述，
 * 数据全程走 文件→文件，不进上下文、不依赖 Python。
 *
 * 组成：
 * - 源解析：glob 多文件（xlsx/xlsm/csv/tsv，按表头名对齐合并）或 rows 内联小表；
 * - 动作管道：filter/select_cols/rename_cols/fill/sort/dedupe/top/slice/compute/aggregate/
 *   join/group/pivot（列按表头名/序号/字母引用，数值感知比较）；
 * - 输出：xlsx（自动表头美化+数值类型）/csv（utf-8/utf-8-sig/gbk——GBK 编码器运行时反向构建，
 *   不依赖 iconv/Python）/json；每阶段行数统计 + 头部预览。
 * 守卫与 excel_write 同语义（writeGuards + 防盲覆盖）；路径解析经 ctx.resolvePath/projectAware。
 */
import type { Workbook, Worksheet } from "exceljs"
import type { Tool, ToolContext } from "@gebai/sdk"
import { asNum, blindOverwriteGuard, fileBlocks, schema, writeGuards } from "./shared"
import { loadWorkbook } from "./excel"

// ---------------------------------------------------------------------------
// 基础类型与工具函数
// ---------------------------------------------------------------------------

/** 中间表：首行为表头的二维数组（列头唯一化后）+ 原始行号（错误定位）。 */
type Frame = { headers: string[]; rows: unknown[][] }

/** 源文件 glob 展开：走 walkDirFiles（与全局 glob 工具同一实现，跳过 node_modules/.git 等大目录）。
 *  模式直接相对会话工作目录/项目根解析（batch.ts 不感知 project 参数差异——projectAware 已替换 resolvePath）。 */
async function expandFilePatterns(patterns: string[], ctx: ToolContext): Promise<{ files: string[]; missing: string[] }> {
  const { walkDirFiles } = await import("@gebai/sdk/node")
  const root = ctx.resolvePath(".")
  const all = await ctx.listFiles().catch(() => [] as Awaited<ReturnType<typeof ctx.listFiles>>)
  const listed = new Map(all.map((f) => [f.path.replace(/\\/g, "/"), f]))
  const files: string[] = []
  const missing: string[] = []
  for (const raw of patterns) {
    const pattern = String(raw).replace(/\\/g, "/").trim()
    if (!pattern) continue
    // 含通配符 → 模式匹配（会话列表 + 范围外实际遍历）；无通配符 → 直接 resolvePath 定文件（项目内路径唯一入口）
    if (/[*?{]/.test(pattern)) {
      const re = globToRegExp(pattern)
      let matched = [...listed.keys()].filter((p) => re.test(p) || re.test(p.split("/").pop() ?? ""))
      if (!matched.length) {
        // 会话列表未命中：可能是 tmp/ 外（项目根外绝对/相对）路径——按目录前缀实际遍历（本地模式）
        const base = pattern.slice(0, pattern.lastIndexOf("/"))
        if (base) {
          const absBase = ctx.resolvePath(base)
          const rel = relOutside(base, root, ctx)
          if (rel !== null) {
            const walked = await walkDirFiles(absBase, `${rel}/`)
            matched = walked.filter((f) => !f.isDir).map((f) => f.path).filter((p) => re.test(stripPrefix(p, `${rel}/`)))
          }
        }
      }
      if (!matched.length) missing.push(pattern)
      else files.push(...matched)
    } else {
      const abs = ctx.resolvePath(pattern)
      try {
        await ctx.readBinaryFile(abs)
        files.push(pattern)
      } catch {
        missing.push(pattern)
      }
    }
  }
  return { files: [...new Set(files)], missing }
}

/** glob 模式 → 正则（递归通配、单段 * 、? 单字符、{a,b} 交替；与全局 glob 工具同语义的精简版）。 */
function globToRegExp(pattern: string): RegExp {
  const alts = expandBraces(pattern)
  const parts = alts.map((v) => {
    let out = ""
    for (let i = 0; i < v.length; i++) {
      const c = v[i]
      if (c === "*") {
        if (v[i + 1] === "*" && v[i + 2] === "/") {
          out += "(?:.*/)?"
          i += 2
        } else {
          out += "[^/]*"
          if (v[i + 1] === "*") i++
        }
      } else if (c === "?") out += "[^/]"
      else out += c.replace(/[.+^${}()|[\]\\]/g, "\\$&")
    }
    return out
  })
  return new RegExp(`^(?:${parts.join("|")})$`)
}

/** 花括号交替展开：a{b,c}d → [abd, acd]（支持嵌套一层）。 */
function expandBraces(pattern: string): string[] {
  const m = /\{([^{}]*)\}/.exec(pattern)
  if (!m) return [pattern]
  const out: string[] = []
  for (const alt of m[1].split(",")) out.push(...expandBraces(pattern.slice(0, m.index) + alt.trim() + pattern.slice(m.index + m[0].length)))
  return out.length ? out : [pattern]
}

/** 基准外相对前缀（resolvePath 指向 root 外时给遍历结果的路径前缀），同基准内返回 null。 */
function relOutside(p: string, root: string, _ctx: ToolContext): string | null {
  const norm = (s: string) => s.replace(/\\/g, "/").replace(/\/+$/, "")
  const np = norm(p)
  const nr = norm(root)
  if (!np) return null
  // 前缀归属判断：np 以 nr 开头 → 基准内（null）；否则把首段 .. 逐级剥到基准外的锚（本地模式上级路径）
  const relSteps: string[] = []
  let cur = np
  while (cur && !prefixDir(cur, nr)) {
    relSteps.unshift(cur.slice(cur.lastIndexOf("/") + 1) || cur)
    const idx = cur.lastIndexOf("/")
    cur = idx > 0 ? cur.slice(0, idx) : ""
    if (!cur) break
  }
  if (!cur) return null // 无法锚定（含盘符差异等）——交由上层按无匹配处理
  if (!relSteps.length) return null // 基准内
  const joined = relSteps.join("/")
  return joined.startsWith("../") ? null : joined
}

/** 目录前缀归属（含相等）：a/b 是 a 的内部。 */
function prefixDir(p: string, dir: string): boolean {
  return p === dir || p.startsWith(`${dir}/`)
}

function stripPrefix(p: string, prefix: string): string {
  return p.startsWith(prefix) ? p.slice(prefix.length) : p
}

// ---------------------------------------------------------------------------
// 文本/编码
// ---------------------------------------------------------------------------

/** GBK 编码器：以运行时 TextDecoder("gbk") 反向构建映射（256×256 双字节区解码 → 查表编码），
 *  无需第三方库/iconv/Python；构建一次缓存。U+FFFD（解不出）与 BMP 外字符替换为 "？"。 */
let gbkEncodeTable: Map<number, number[]> | null = null
async function gbkEncoder(): Promise<Map<number, number[]> | null> {
  if (gbkEncodeTable) return gbkEncodeTable
  const table = new Map<number, number[]>()
  try {
    const decoder = new TextDecoder("gbk" as never)
    const buf = new Uint8Array(2)
    const dv = new DataView(buf.buffer)
    for (let hi = 0x81; hi <= 0xfe; hi++) {
      for (let lo = 0x40; lo <= 0xfe; lo++) {
        if (lo === 0x7f) continue
        dv.setUint8(0, hi)
        dv.setUint8(1, lo)
        const s = decoder.decode(buf)
        if (!s || s.includes("\uFFFD")) continue
        const cp = s.codePointAt(0)!
        if (!table.has(cp)) table.set(cp, [hi, lo]) // 同码位多映射取首个（GBK 兼容 GB2312 主区）
      }
    }
    // ASCII 直通
    for (let b = 0x20; b < 0x7f; b++) table.set(b, [b])
    gbkEncodeTable = table
    return table
  } catch {
    return null // 运行时不支持 gbk（极少见）——调用方降级报错
  }
}

async function encodeGbk(text: string): Promise<{ bytes: Uint8Array; failed: string[] } | null> {
  const table = await gbkEncoder()
  if (!table) return null
  const out: number[] = []
  const failed = new Set<string>()
  for (const ch of text) {
    const cp = ch.codePointAt(0)!
    if (cp > 0xffff) {
      failed.add(ch)
      out.push(0x3f)
      continue
    }
    const enc = table.get(cp)
    if (enc) out.push(...enc)
    else {
      failed.add(ch)
      out.push(0x3f)
    }
  }
  return { bytes: Uint8Array.from(out), failed: [...failed] }
}

/** csv/tsv 文本解码：BOM 优先 → UTF-8 严格 → GBK 兜底（中文 CSV 导入最常见坑）；显式 encoding 直用。 */
function decodeDelimited(bytes: Uint8Array, encoding?: string): { text: string; encoding: string } | null {
  const stripBom = (s: string) => (s.charCodeAt(0) === 0xfeff ? s.slice(1) : s)
  if (encoding && encoding !== "auto") {
    try {
      return { text: stripBom(new TextDecoder(encoding as never, { fatal: true }).decode(bytes)), encoding }
    } catch {
      return null
    }
  }
  if (bytes.length >= 3 && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) {
    return { text: stripBom(new TextDecoder("utf-8").decode(bytes)), encoding: "utf-8-sig" }
  }
  try {
    return { text: new TextDecoder("utf-8", { fatal: true }).decode(bytes), encoding: "utf-8" }
  } catch {
    /* 落 GBK */
  }
  try {
    const text = new TextDecoder("gbk" as never, { fatal: true }).decode(bytes)
    if (text.includes("\uFFFD")) return null
    return { text, encoding: "gbk" }
  } catch {
    return null
  }
}

/** CSV/TSV 解析（引号转义感知；excel.ts parseDelimited 同源逻辑，这里独立轻实现避免导出扰动）。 */
function parseDelimited(text: string, delim: string): string[][] {
  const rows: string[][] = []
  let row: string[] = []
  let cell = ""
  let quoted = false
  for (let i = 0; i < text.length; i++) {
    const c = text[i]
    if (quoted) {
      if (c === '"') {
        if (text[i + 1] === '"') {
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
  // 尾部空行剔除（CSV 常见结尾多换行）
  while (rows.length && rows[rows.length - 1].every((c) => c === "")) rows.pop()
  return rows
}

// ---------------------------------------------------------------------------
// 值归一：类型推断（数值/日期/文本保留原样，防 "0012" 编号被误转数字）
// ---------------------------------------------------------------------------

/** CSV 单元格文本 → 值：纯整数且无前导零（≥3 位）/前导零保护；千分位/百分号/负数/小数归数；ISO 日期归日期。
 *  设计取向：**保守转数**——可转可不转的一律保文本（编号/编码列误转数字是批量导入最高频事故）。 */
function coerceCsvValue(s: string): unknown {
  if (s === "") return null
  const t = s.trim()
  if (t !== s) return s // 有首尾空格按原文本（用户数据有意为之）
  if (/^-?\d{1,15}$/.test(t)) {
    // 前导零（"0012"）与超长数字（手机号/证件号等）保文本；其余转整数
    if (/^(-?)0\d/.test(t) || t.replace("-", "").length >= 11) return s
    return Number(t)
  }
  // 千分位整数/小数（至少一个逗号分组）：1,234 / 12,345.67
  if (/^-?\d{1,3}(,\d{3})+(\.\d+)?$/.test(t)) return Number(t.replace(/,/g, ""))
  if (/^-?\d+\.\d+$/.test(t)) return Number(t)
  if (/^-?\d+(\.\d+)?%$/.test(t)) return Number(t.slice(0, -1)) / 100 // 百分比归数值（20% → 0.2）
  if (/^\d{4}-\d{2}-\d{2}$/.test(t)) {
    const d = new Date(`${t}T00:00:00`)
    if (!Number.isNaN(d.getTime())) return d
  }
  if (/^\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}(:\d{2})?$/.test(t)) {
    const d = new Date(t.replace(" ", "T"))
    if (!Number.isNaN(d.getTime())) return d
  }
  return s
}

/** 输出侧单元格值渲染（csv/json）：null → 空、日期 ISO 化、其余字符串化（与 excel_read cellText 口径接近）。 */
function renderValue(v: unknown, csv: boolean): string {
  if (v == null) return ""
  if (typeof v === "number" && !Number.isFinite(v)) return "" // NaN/Infinity（除零等）→ 空（compute 已拦截，此处兑底源数据携带）
  if (v instanceof Date) {
    const d = v
    const date = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`
    const hasTime = d.getHours() || d.getMinutes() || d.getSeconds()
    return hasTime ? `${date} ${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}:${String(d.getSeconds()).padStart(2, "0")}` : date
  }
  if (typeof v === "number") return Number.isInteger(v) ? String(v) : String(Number(v.toFixed(10)))
  if (typeof v === "boolean") return v ? "TRUE" : "FALSE"
  const s = String(v)
  if (csv && /^-?\d+(\.\d+)?e[+-]?\d+$/i.test(s)) return s // 防科学计数法串扰
  return s
}

/** 数值感知比较（排序/筛选共用）：数字按数值比、日期按时间戳、其余字符串比较（数值字符串也按数值比）。 */
function compareValues(a: unknown, b: unknown): number {
  const na = numericOf(a)
  const nb = numericOf(b)
  if (na != null && nb != null) return na - nb
  if (a instanceof Date || b instanceof Date) {
    const ta = a instanceof Date ? a.getTime() : Number.NaN
    const tb = b instanceof Date ? b.getTime() : Number.NaN
    if (!Number.isNaN(ta) && !Number.isNaN(tb)) return ta - tb
  }
  const sa = a == null ? "" : String(a)
  const sb = b == null ? "" : String(b)
  return sa < sb ? -1 : sa > sb ? 1 : 0
}

function numericOf(v: unknown): number | null {
  if (typeof v === "number") return Number.isFinite(v) ? v : null
  if (typeof v === "string" && v.trim() !== "") {
    const n = Number(v)
    return Number.isFinite(n) ? n : null
  }
  return null
}

// ---------------------------------------------------------------------------
// 表达式引擎（compute/join 映射键等）：安全递归下降求值，无 eval
// 语法：列引用 {列名} 或 [列名]；运算 + - * / % ^ 与括号；函数 IF(cond,a,b) ROUND(x,n) ABS MIN MAX
//        INT FLOOR CEIL SQRT LEN CONCAT UPPER LOWER TRIM LEFT(s,n) RIGHT(s,n) TEXT(v) NUMBER(v)
//        AND OR NOT ISBLANK；比较 = <> != > >= < <=；逻辑真值：非空非零非"false"
// ---------------------------------------------------------------------------

type EvalCtx = { row: Record<string, unknown>; headers: string[] }

/** 分词：标识符/数字/字符串/运算符。 */
function tokenizeExpr(src: string): string[] {
  const tokens: string[] = []
  let i = 0
  while (i < src.length) {
    const c = src[i]
    if (/\s/.test(c)) {
      i++
      continue
    }
    if (c === '"' || c === "'") {
      const quote = c
      let j = i + 1
      let s = ""
      while (j < src.length && src[j] !== quote) {
        if (src[j] === "\\" && j + 1 < src.length) {
          s += src[j + 1]
          j += 2
        } else s += src[j++]
      }
      tokens.push(`«str:${s}»`)
      i = j + 1
      continue
    }
    if (c === "{" || c === "[") {
      const close = c === "{" ? "}" : "]"
      let j = i + 1
      while (j < src.length && src[j] !== close) j++
      tokens.push(`«col:${src.slice(i + 1, j)}»`)
      i = j + 1
      continue
    }
    if (/[0-9]/.test(c) || (c === "." && /[0-9]/.test(src[i + 1] ?? ""))) {
      let j = i
      while (j < src.length && /[0-9.]/.test(src[j])) j++
      tokens.push(src.slice(i, j))
      i = j
      continue
    }
    if (/[A-Za-z_\u4e00-\u9fff]/.test(c)) {
      let j = i
      while (j < src.length && /[A-Za-z0-9_\u4e00-\u9fff]/.test(src[j])) j++
      tokens.push(src.slice(i, j))
      i = j
      continue
    }
    const two = src.slice(i, i + 2)
    if (["<=", ">=", "<>", "!=", "=="].includes(two)) {
      tokens.push(two)
      i += 2
      continue
    }
    tokens.push(c)
    i++
  }
  return tokens
}

/** 求值器（递归下降：expr → or → and → cmp → add → mul → pow → unary → atom）。 */
function evalExpression(src: string, ec: EvalCtx): unknown {
  const tokens = tokenizeExpr(src)
  let pos = 0
  const peek = () => tokens[pos]
  const next = () => tokens[pos++]

  const truthy = (v: unknown): boolean => {
    if (v == null || v === "") return false
    if (typeof v === "number") return v !== 0
    if (typeof v === "boolean") return v
    const s = String(v).trim().toLowerCase()
    return s !== "" && s !== "false" && s !== "0"
  }
  const num = (v: unknown): number => {
    const n = numericOf(v)
    if (n == null) throw new Error(`「${String(v)}」不是数字`)
    return n
  }

  function parseAtom(): unknown {
    const t = next()
    if (t === undefined) throw new Error("表达式意外结束")
    if (t.startsWith("«str:")) return t.slice(5, -1)
    if (t.startsWith("«col:")) {
      const name = t.slice(5, -1)
      const key = resolveHeader(name, ec.headers)
      if (!key) throw new Error(`列「${name}」不存在（可用列：${ec.headers.slice(0, 8).join("、")}${ec.headers.length > 8 ? "…" : ""}）`)
      return ec.row[key]
    }
    if (t === "(") {
      const v = parseOr()
      if (next() !== ")") throw new Error("缺右括号")
      return v
    }
    if (t === "-") return -num(parseAtom())
    if (t === "+") return num(parseAtom())
    if (/^[0-9.]+$/.test(t)) return Number(t)
    if (/^[A-Za-z_\u4e00-\u9fff][\w\u4e00-\u9fff]*$/.test(t)) {
      const fn = t.toUpperCase()
      if (next() !== "(") throw new Error(`「${t}」后应为 (`)
      const args: unknown[] = []
      if (peek() !== ")") {
        for (;;) {
          args.push(parseOr())
          const sep = next()
          if (sep === ")") break
          if (sep !== ",") throw new Error(`函数参数分隔异常：${String(sep)}`)
        }
      } else next()
      return applyFunction(fn, args)
    }
    throw new Error(`无法解析的记号：${t}`)
  }

  function applyFunction(fn: string, a: unknown[]): unknown {
    const s = (i: number) => String(a[i] ?? "")
    switch (fn) {
      case "IF":
        return truthy(a[0]) ? a[1] ?? null : a[2] ?? null
      case "ROUND":
        return Math.round(num(a[0]) * 10 ** Math.trunc(numOr(a[1], 0))) / 10 ** Math.trunc(numOr(a[1], 0))
      case "ABS":
        return Math.abs(num(a[0]))
      case "MIN":
        return Math.min(...a.map(num))
      case "MAX":
        return Math.max(...a.map(num))
      case "INT":
      case "FLOOR":
        return Math.floor(num(a[0]))
      case "CEIL":
      case "CEILING":
        return Math.ceil(num(a[0]))
      case "SQRT":
        return Math.sqrt(num(a[0]))
      case "LEN":
        return s(0).length
      case "CONCAT":
      case "CONCATENATE":
        return a.map((v) => renderValue(v, false)).join("")
      case "UPPER":
        return s(0).toUpperCase()
      case "LOWER":
        return s(0).toLowerCase()
      case "TRIM":
        return s(0).trim()
      case "LEFT":
        return s(0).slice(0, Math.trunc(numOr(a[1], 1)))
      case "RIGHT": {
        const n = Math.trunc(numOr(a[1], 1))
        const str = s(0)
        return n >= str.length ? str : str.slice(str.length - n)
      }
      case "TEXT":
        return renderValue(a[0] ?? null, false)
      case "NUMBER":
        return num(a[0])
      case "AND":
        return a.every(truthy)
      case "OR":
        return a.some(truthy)
      case "NOT":
        return !truthy(a[0])
      case "ISBLANK":
        return a[0] == null || a[0] === ""
      default:
        throw new Error(`不支持的函数：${fn}（支持 IF/ROUND/ABS/MIN/MAX/INT/FLOOR/CEIL/SQRT/LEN/CONCAT/UPPER/LOWER/TRIM/LEFT/RIGHT/TEXT/NUMBER/AND/OR/NOT/ISBLANK）`)
    }
  }

  const numOr = (v: unknown, d: number): number => {
    const n = numericOf(v)
    return n == null ? d : n
  }

  function parsePow(): unknown {
    const base = parseAtom()
    if (peek() === "^") {
      next()
      return num(base) ** num(parsePow())
    }
    return base
  }

  function parseUnary(): unknown {
    return parsePow()
  }

  function parseMul(): unknown {
    let left = parseUnary()
    while (peek() === "*" || peek() === "/" || peek() === "%") {
      const op = next()
      const right = parseUnary()
      const l = num(left)
      const r = num(right)
      left = op === "*" ? l * r : op === "/" ? (r === 0 ? Number.NaN : l / r) : (r === 0 ? Number.NaN : l % r)
    }
    return left
  }

  function parseAdd(): unknown {
    let left = parseMul()
    while (peek() === "+" || peek() === "-") {
      const op = next()
      const right = parseMul()
      if (op === "+") {
        // 数字+数字 → 数；任一为文本 → 拼接（Excel 同语义）
        if (numericOf(left) != null && numericOf(right) != null) left = num(left) + num(right)
        else left = renderValue(left, false) + renderValue(right, false)
      } else left = num(left) - num(right)
    }
    return left
  }

  function parseCmp(): unknown {
    const left = parseAdd()
    const op = peek()
    if (op === "=" || op === "==" || op === "<>" || op === "!=" || op === ">" || op === ">=" || op === "<" || op === "<=") {
      next()
      const right = parseAdd()
      switch (op) {
        case "=":
        case "==": {
          // 等值：数值感知（"1.0" = 1）；两空相等
          const ln = numericOf(left)
          const rn = numericOf(right)
          if (ln != null && rn != null) return ln === rn
          return renderValue(left, false).trim() === renderValue(right, false).trim()
        }
        case "<>":
        case "!=": {
          const ln = numericOf(left)
          const rn = numericOf(right)
          if (ln != null && rn != null) return ln !== rn
          return renderValue(left, false).trim() !== renderValue(right, false).trim()
        }
        default:
          return cmpOp(op, compareValues(left, right))
      }
    }
    return left
  }

  function parseAnd(): unknown {
    let left = parseCmp()
    while (peek() && /^[A-Za-z_\u4e00-\u9fff]+$/.test(peek()!) && peek()!.toUpperCase() === "AND") {
      next()
      const right = parseCmp()
      left = truthy(left) && truthy(right)
    }
    return left
  }

  function parseOr(): unknown {
    let left = parseAnd()
    while (peek() && /^[A-Za-z_\u4e00-\u9fff]+$/.test(peek()!) && peek()!.toUpperCase() === "OR") {
      next()
      const right = parseAnd()
      left = truthy(left) || truthy(right)
    }
    return left
  }

  const v = parseOr()
  if (pos < tokens.length) throw new Error(`表达式尾部有无法解析的内容：${tokens.slice(pos).join(" ")}`)
  return v
}

function cmpOp(op: string, cmp: number): boolean {
  switch (op) {
    case ">":
      return cmp > 0
    case ">=":
      return cmp >= 0
    case "<":
      return cmp < 0
    case "<=":
      return cmp <= 0
    default:
      throw new Error(`不支持的操作符 ${op}`)
  }
}

/** 列引用解析：精确 → 大小写/空白不敏感 → 序号（1 起）→ 字母（A/BC）→ 带引号。返回 headers 中的实际键。 */
function resolveHeader(ref: unknown, headers: string[]): string | null {
  if (ref == null) return null
  if (typeof ref === "number" && Number.isInteger(ref)) return headers[ref - 1] ?? null
  const s = String(ref).trim()
  if (!s) return null
  const unquote = (t: string) => (t.length >= 2 && ((t[0] === '"' && t.endsWith('"')) || (t[0] === "'" && t.endsWith("'"))) ? t.slice(1, -1) : t)
  const t = unquote(s)
  if (headers.includes(t)) return t
  const lowered = new Map(headers.map((h) => [h.trim().toLowerCase(), h]))
  const hit = lowered.get(t.trim().toLowerCase())
  if (hit != null) return hit
  // 字母列号（A → 1）
  if (/^[A-Za-z]{1,3}$/.test(t)) {
    let n = 0
    for (const c of t.toUpperCase()) n = n * 26 + (c.charCodeAt(0) - 64)
    if (n >= 1 && n <= headers.length) return headers[n - 1]
  }
  return null
}

/** 表头唯一化：空列名补「列N」，重名追加 .2/.3…。 */
function uniquifyHeaders(raw: string[]): string[] {
  const used = new Map<string, number>()
  return raw.map((h, i) => {
    let name = h != null && String(h).trim() !== "" ? String(h).trim() : `列${i + 1}`
    const n = used.get(name) ?? 0
    used.set(name, n + 1)
    if (n > 0) name = `${name}.${n + 1}`
    return name
  })
}

// ---------------------------------------------------------------------------
// 源装载：xlsx / csv / tsv → Frame（多文件按表头名对齐合并）
// ---------------------------------------------------------------------------

async function loadXlsxFrame(path: string, sheetKey: unknown, ctx: ToolContext): Promise<{ frames: Array<{ sheet: string; frame: Frame }>; error?: string }> {
  const abs = ctx.resolvePath(path)
  let bytes: Uint8Array
  try {
    bytes = await ctx.readBinaryFile(abs)
  } catch {
    return { frames: [], error: `文件不存在或不可读` }
  }
  const Workbook = await loadWorkbook()
  const wb = new Workbook()
  try {
    await wb.xlsx.load(Buffer.from(bytes) as never)
  } catch (err) {
    return { frames: [], error: `不是有效的 .xlsx（${(err as Error).message}）` }
  }
  const targets: Worksheet[] = sheetKey == null ? wb.worksheets : [sheetByNameOrIndex(wb, sheetKey)].filter((w): w is Worksheet => w != null)
  if (!targets.length) return { frames: [], error: `工作表不存在：${String(sheetKey)}` }
  const frames = targets.map((ws) => {
    const maxRow = ws.rowCount || 0
    const maxCol = ws.actualColumnCount || 0
    const grid: unknown[][] = []
    for (let r = 1; r <= maxRow; r++) {
      const line: unknown[] = []
      for (let c = 1; c <= maxCol; c++) {
        const cell = ws.getRow(r).getCell(c)
        const v: unknown = cell.value
        if (v != null && typeof v === "object" && !(v instanceof Date) && "result" in (v as Record<string, unknown>)) {
          // 公式取缓存结果（批量数据管道关心值，不关心公式）
          const res = (v as Record<string, unknown>).result
          line.push(res === undefined ? null : res)
        } else if (v != null && typeof v === "object" && "richText" in (v as Record<string, unknown>)) {
          line.push(((v as { richText: Array<{ text: string }> }).richText ?? []).map((t) => t.text).join(""))
        } else if (v != null && typeof v === "object" && "text" in (v as Record<string, unknown>)) {
          line.push((v as { text: unknown }).text ?? null)
        } else line.push(v)
      }
      grid.push(line)
    }
    // 掐头去尾空行、右侧空列
    trimGrid(grid)
    const headers = uniquifyHeaders((grid.shift() ?? []).map((h) => (h instanceof Date ? renderValue(h, false) : h == null ? "" : String(h))))
    return { sheet: ws.name, frame: { headers, rows: grid } }
  })
  return { frames }
}

function sheetByNameOrIndex(wb: Workbook, key: unknown): Worksheet | null {
  if (key == null) return null
  if (typeof key === "number" && Number.isInteger(key)) return wb.worksheets[key - 1] ?? null
  const name = String(key)
  return wb.worksheets.find((ws) => ws.name === name) ?? null
}

function trimGrid(grid: unknown[][]): void {
  while (grid.length && grid[0].every((v) => v == null || v === "")) grid.shift()
  while (grid.length && grid[grid.length - 1].every((v) => v == null || v === "")) grid.pop()
}

async function loadDelimitedFrame(path: string, encoding: unknown, ctx: ToolContext): Promise<{ frame: Frame; error?: string; encoding: string }> {
  const abs = ctx.resolvePath(path)
  let bytes: Uint8Array
  try {
    bytes = await ctx.readBinaryFile(abs)
  } catch {
    return { frame: { headers: [], rows: [] }, error: "文件不存在或不可读", encoding: "" }
  }
  const ext = /\.([a-z0-9]+)$/i.exec(abs)?.[1].toLowerCase() ?? "csv"
  const decoded = decodeDelimited(bytes, typeof encoding === "string" && encoding ? encoding : undefined)
  if (!decoded) return { frame: { headers: [], rows: [] }, error: "编码无法识别（可显式传 encoding，如 utf-8/gbk/utf-16le）", encoding: "" }
  const raw = parseDelimited(decoded.text, ext === "tsv" ? "\t" : ",")
  if (!raw.length) return { frame: { headers: [], rows: [] }, error: "内容为空", encoding: decoded.encoding }
  const headers = uniquifyHeaders(raw[0].map((h) => h.trim()))
  const rows = raw.slice(1).map((r) => r.map(coerceCsvValue))
  return { frame: { headers, rows }, encoding: decoded.encoding }
}

/** 多 Frame 合并：按表头名对齐（列序不同无碍；新列补空；类型冲突按宽化：数字+文本 → 文本）。 */
function mergeFrames(frames: Array<{ src: string; frame: Frame }>): { frame: Frame; mergedCols: string[]; droppedCols: string[] } {
  const ordered: string[] = []
  for (const { frame } of frames) for (const h of frame.headers) if (!ordered.includes(h)) ordered.push(h)
  const droppedCols = frames.flatMap(({ src, frame }) => frame.headers.filter((h) => !ordered.includes(h)).map((h) => `${src}:${h}`))
  const rows: unknown[][] = []
  for (const { frame } of frames) {
    const idx = frame.headers.map((h) => ordered.indexOf(h))
    for (const r of frame.rows) rows.push(idx.map((i) => (i >= 0 ? r[i] ?? null : null)))
  }
  return { frame: { headers: ordered, rows }, mergedCols: ordered, droppedCols }
}

// ---------------------------------------------------------------------------
// 动作管道
// ---------------------------------------------------------------------------

type StageStat = { action: string; detail: string; rowsIn: number; rowsOut: number }

function rowToRecord(headers: string[], row: unknown[]): Record<string, unknown> {
  const rec: Record<string, unknown> = {}
  headers.forEach((h, i) => (rec[h] = row[i] ?? null))
  return rec
}

function applyFilter(frame: Frame, cond: Record<string, unknown>): Frame {
  const headers = frame.headers
  const rows = frame.rows.filter((row) => {
    const rec = rowToRecord(headers, row)
    return Object.entries(cond).every(([ref, ruleRaw]) => {
      const key = resolveHeader(ref, headers)
      if (!key) throw new Error(`filter：列「${ref}」不存在（可用列：${headers.join("、")}）`)
      const v = rec[key]
      const fail = (why: string): false => {
        throw new Error(`filter ${ref}: ${why}`)
      }
      // 规则：单操作符对象 {gt: x} / 多操作符 AND；或直接值（等值）/ 数组（in）/ 表达式串（"expr"）
      if (ruleRaw != null && typeof ruleRaw === "object" && !Array.isArray(ruleRaw)) {
        const rule = ruleRaw as Record<string, unknown>
        return Object.entries(rule).every(([op, expected]) => matchOp(v, op, expected, fail, { row: rec, headers }))
      }
      if (Array.isArray(ruleRaw)) return ruleRaw.some((e) => compareValues(v, e) === 0 || renderValue(v, false) === renderValue(e, false))
      if (typeof ruleRaw === "string") {
        // 字符串值：含比较/逻辑操作符且含列引用才按表达式求值——纯文本（日期 "2024-01-01"、型号 "A-1 x2"）
        // 恒等值比较（旧启发式含算术符即求值，日期串被算成 2022 → 全行通过的真缺陷）
        const s = ruleRaw
        if (/\{[^}]+\}|\[[^\]]+\]/.test(s) && /[<>!=]=?|\bAND\b|\bOR\b/i.test(s)) {
          try {
            return truthyResult(evalExpression(s, { row: rec, headers }))
          } catch {
            /* 列引用失效等 → 回退等值比较 */
          }
        }
        return renderValue(v, false).trim() === s.trim() || compareValues(v, s) === 0
      }
      return compareValues(v, ruleRaw) === 0
    })
  })
  return { headers: [...headers], rows }
}

function truthyResult(v: unknown): boolean {
  if (v == null || v === "") return false
  if (typeof v === "number") return v !== 0
  if (typeof v === "boolean") return v
  const s = String(v).trim().toLowerCase()
  return s !== "" && s !== "false" && s !== "0"
}

function matchOp(v: unknown, op: string, expected: unknown, fail: (why: string) => false, rowCtx?: { row: Record<string, unknown>; headers: string[] }): boolean {
  switch (op) {
    case "eq":
      return Array.isArray(expected) ? expected.some((e) => compareValues(v, e) === 0) : compareValues(v, expected) === 0 || renderValue(v, false) === renderValue(expected, false)
    case "ne":
      return !(Array.isArray(expected) ? expected.some((e) => compareValues(v, e) === 0) : compareValues(v, expected) === 0 || renderValue(v, false) === renderValue(expected, false))
    case "gt":
      return compareValues(v, expected) > 0
    case "gte":
      return compareValues(v, expected) >= 0
    case "lt":
      return compareValues(v, expected) < 0
    case "lte":
      return compareValues(v, expected) <= 0
    case "between": {
      if (!Array.isArray(expected)) fail("between 须传 [min, max]")
      const pair = expected as unknown[]
      return compareValues(v, pair[0]) >= 0 && compareValues(v, pair[1]) <= 0
    }
    case "in":
      return (Array.isArray(expected) ? expected : [expected]).some((e) => compareValues(v, e) === 0 || renderValue(v, false) === renderValue(e, false))
    case "not_in":
      return !(Array.isArray(expected) ? expected : [expected]).some((e) => compareValues(v, e) === 0 || renderValue(v, false) === renderValue(e, false))
    case "contains":
      return renderValue(v, false).includes(String(expected))
    case "not_contains":
      return !renderValue(v, false).includes(String(expected))
    case "starts":
    case "starts_with":
      return renderValue(v, false).startsWith(String(expected))
    case "ends":
    case "ends_with":
      return renderValue(v, false).endsWith(String(expected))
    case "empty":
      return expected === true ? v == null || v === "" : v != null && v !== ""
    case "not_empty":
      return expected === true ? v != null && v !== "" : v == null || v === ""
    case "expr": {
      // 行级表达式（规则值即表达式源码，如 {expr: "{数量} > 5 AND {单价} < 10"}）——依赖整行上下文求值
      if (!rowCtx) return fail("expr 需要行上下文（内部错误）")
      if (typeof expected !== "string" || !expected.trim()) return fail("expr 须为表达式字符串")
      try {
        return truthyResult(evalExpression(expected, rowCtx))
      } catch (err) {
        return fail(`expr 求值失败（${expected}）：${(err as Error).message}`)
      }
    }
    default:
      return fail(`未知操作符 ${op}（支持 eq/ne/gt/gte/lt/lte/between/in/not_in/contains/not_contains/starts/ends/empty/not_empty）`)
  }
}

/** 管道动作应用总入口：返回新 Frame + 阶段统计；未知动作抛错（外层统一捕获进 failed 清单）。 */
async function applyActions(frame: Frame, actions: unknown[], joinLoader: (path: string, sheet: unknown, ctx: ToolContext) => Promise<{ frame: Frame; error?: string }>): Promise<{ frame: Frame; stats: StageStat[] }> {
  const stats: StageStat[] = []
  const step = async (action: string, detail: string, fn: () => Frame | Promise<Frame>) => {
    const rowsIn = frame.rows.length
    frame = await fn()
    stats.push({ action, detail, rowsIn, rowsOut: frame.rows.length })
  }
  for (const raw of actions) {
    if (!raw || typeof raw !== "object") continue
    const a = raw as Record<string, unknown>
    const kind = String(a.op ?? a.action ?? "")
    const rowsIn = frame.rows.length
    switch (kind) {
      case "filter":
        await step("filter", briefCond(a.where ?? a.cond ?? a.condition, frame), () => applyFilter(frame, (a.where ?? a.cond ?? a.condition) as Record<string, unknown>))
        break
      case "select_cols": {
        const cols = (a.cols ?? a.columns) as unknown
        const list = Array.isArray(cols) ? cols : [cols]
        const keys = list.map((c) => resolveHeader(c, frame.headers)).map((k, i) => k ?? failCol(String(list[i])))
        const idx = keys.map((k) => frame.headers.indexOf(k!))
        frame = { headers: keys as string[], rows: frame.rows.map((r) => idx.map((i) => r[i] ?? null)) }
        stats.push({ action: kind, detail: `${(keys as string[]).length} 列`, rowsIn, rowsOut: frame.rows.length })
        break
      }
      case "rename_cols": {
        const map = (a.map ?? a.mapping) as Record<string, unknown>
        const headers = frame.headers.map((h) => {
          const from = typeof h === "string" ? h : String(h)
          for (const [k, v] of Object.entries(map ?? {})) {
            // 引用可为目标名本身（已改列名）或原列名（精确/忽略大小写）
            if (from === k || from.toLowerCase() === k.toLowerCase().trim()) return String(v)
          }
          return h
        })
        frame = { headers: uniquifyHeaders(headers), rows: frame.rows }
        stats.push({ action: kind, detail: `${Object.keys(map ?? {}).length} 项`, rowsIn, rowsOut: frame.rows.length })
        break
      }
      case "fill": {
        const col = resolveHeader(a.col ?? a.column, frame.headers)
        if (!col) throw new Error(`fill：列「${String(a.col ?? a.column)}」不存在`)
        const value = a.value
        let n = 0
        const ci = frame.headers.indexOf(col)
        frame.rows.forEach((r) => {
          if (r[ci] == null || r[ci] === "") {
            r[ci] = typeof value === "string" ? value : value
            n++
          }
        })
        stats.push({ action: kind, detail: `${col} 补 ${n} 格`, rowsIn, rowsOut: frame.rows.length })
        break
      }
      case "sort": {
        const by = a.by ?? a.keys
        const specs = (Array.isArray(by) ? by : [by]).map((b) => (typeof b === "object" && b !== null ? b : { col: b }) as Record<string, unknown>)
        const dirs = (Array.isArray(a.order) ? a.order : typeof a.order === "string" ? a.order.split(",") : specs.map(() => a.order)).map((d: unknown) => String(d ?? "asc").toLowerCase())
        const keyIdx = specs.map((s) => {
          const key = resolveHeader(s.col ?? s.column ?? s.key, frame.headers)
          if (!key) throw new Error(`sort：列「${String(s.col ?? s.key)}」不存在`)
          return frame.headers.indexOf(key)
        })
        const indexed = frame.rows.map((r, i) => ({ r, i }))
        indexed.sort((x, y) => {
          for (let k = 0; k < keyIdx.length; k++) {
            const desc = dirs[k] === "desc"
            const c = compareValues(x.r[keyIdx[k]], y.r[keyIdx[k]])
            if (c !== 0) return desc ? -c : c
          }
          return x.i - y.i // 稳定排序
        })
        frame = { headers: [...frame.headers], rows: indexed.map((x) => x.r) }
        stats.push({ action: kind, detail: specs.map((s, i) => `${String(s.col ?? s.key)} ${dirs[i] === "desc" ? "↓" : "↑"}`).join("、"), rowsIn, rowsOut: frame.rows.length })
        break
      }
      case "dedupe": {
        const cols = a.cols ?? a.columns ?? a.by
        const seen = new Set<string>()
        const idx = cols ? (Array.isArray(cols) ? cols : [cols]).map((c) => frame.headers.indexOf(resolveHeader(c, frame.headers) ?? failCol(String(c)))) : frame.headers.map((_, i) => i)
        const rows = frame.rows.filter((r) => {
          const key = idx.map((i) => renderValue(r[i], false)).join("｜")
          if (seen.has(key)) return false
          seen.add(key)
          return true
        })
        const removed = frame.rows.length - rows.length
        frame = { headers: [...frame.headers], rows }
        stats.push({ action: kind, detail: `按 ${cols ? String(Array.isArray(cols) ? cols.join("+") : cols) : "全列"} 去重 ${removed} 行`, rowsIn, rowsOut: frame.rows.length })
        break
      }
      case "top":
      case "limit": {
        const n = Math.max(0, Math.round(asNum(a.n ?? a.count, 10)))
        frame = { headers: [...frame.headers], rows: frame.rows.slice(0, n) }
        stats.push({ action: "top", detail: `前 ${n} 行`, rowsIn, rowsOut: frame.rows.length })
        break
      }
      case "slice": {
        const from = Math.max(1, Math.round(asNum(a.from, 1)))
        const to = Math.min(frame.rows.length, Math.round(asNum(a.to, frame.rows.length)))
        frame = { headers: [...frame.headers], rows: frame.rows.slice(from - 1, to) }
        stats.push({ action: kind, detail: `第 ${from}–${to} 行`, rowsIn, rowsOut: frame.rows.length })
        break
      }
      case "compute": {
        const cols = (a.cols ?? a.columns) as Array<Record<string, unknown>>
        if (!Array.isArray(cols) || !cols.length) throw new Error("compute：cols 须为 [{name, expr}] 数组")
        const headers = [...frame.headers]
        for (const c of cols) {
          const name = String(c.name ?? "")
          if (!name) throw new Error("compute：列 name 不能为空")
          if (!headers.includes(name)) headers.push(name)
        }
        const rows = frame.rows.map((r) => {
          // 累进求值：同一 compute 内后列可引用前列（{金额} 在下一表达式即已可见）
          const rec = rowToRecord(frame.headers, r)
          const out = [...r]
          for (const c of cols) {
            const name = String(c.name ?? "")
            const expr = String(c.expr ?? c.formula ?? "")
            try {
              const v = evalExpression(expr, { row: rec, headers })
              // 非有限数值（除零/溢出 → NaN/Infinity）序列化为空：json/csv 可序列化，xlsx 不落脏值（Exceljs 写 NaN 报错）
              const safe = typeof v === "number" && !Number.isFinite(v) ? null : v instanceof Date ? v : typeof v === "number" ? v : v == null || typeof v !== "object" ? v : renderValue(v, false)
              out[headers.indexOf(name)] = safe
              rec[name] = safe
            } catch (err) {
              throw new Error(`compute ${name}（${expr}）：${(err as Error).message}`)
            }
          }
          return out
        })
        frame = { headers, rows }
        stats.push({ action: kind, detail: `${cols.length} 列（${cols.map((c) => String(c.name)).join("、")}）`, rowsIn, rowsOut: frame.rows.length })
        break
      }
      case "aggregate": {
        const specs = (Array.isArray(a.cols) ? a.cols : []) as Array<Record<string, unknown>>
        const out: unknown[] = a.label != null ? [String(a.label)] : []
        const outHeaders = a.label != null ? [String(a.label)] : []
        const targets: Array<Record<string, unknown>> = specs.length ? specs : frame.headers.map((h) => ({ col: h, fn: "sum" }))
        for (const t of targets) {
          const key = resolveHeader(t.col ?? t.column, frame.headers)
          if (!key) throw new Error(`aggregate：列「${String(t.col)}」不存在`)
          const fn = String(t.fn ?? t.func ?? "sum").toLowerCase()
          const ci = frame.headers.indexOf(key)
          const col = frame.rows.map((r) => r[ci])
          const label = String(t.as ?? `${aggLabel(fn)}(${key})`)
          outHeaders.push(label)
          out.push(aggregateValues(col, fn))
        }
        frame = { headers: outHeaders, rows: [out] }
        stats.push({ action: kind, detail: `${targets.length} 项`, rowsIn, rowsOut: 1 })
        break
      }
      case "join": {
        const loaded = await joinLoader(String(a.path ?? ""), a.sheet, undefined as never)
        if (loaded.error || !loaded.frame.headers.length) throw new Error(`join 装载 ${String(a.path)} 失败：${loaded.error ?? "空表"}`)
        frame = joinFrames(frame, loaded.frame, a)
        stats.push({ action: kind, detail: `${String(a.type ?? "left")} join ${String(a.path)} on ${joinKeyDesc(a)}`, rowsIn, rowsOut: frame.rows.length })
        break
      }
      case "group": {
        const by = a.by ?? a.keys
        const byKeys = (Array.isArray(by) ? by : [by]).map((c) => resolveHeader(c, frame.headers) ?? failCol(String(c)))
        const aggs = Array.isArray(a.aggs) ? (a.aggs as Array<Record<string, unknown>>) : Object.entries((a.agg ?? a.aggregations ?? {}) as Record<string, unknown>).map(([k, v]) => ({ col: k.split(":")[0], fn: k.split(":")[1] ?? String(v), as: typeof v === "string" && !["sum", "avg", "count", "min", "max", "distinct"].includes(String(v)) ? String(v) : undefined }))
        const groups = new Map<string, { key: unknown[]; rows: unknown[][] }>()
        const byIdx = byKeys.map((k) => frame.headers.indexOf(k))
        for (const r of frame.rows) {
          const gk = byIdx.map((i) => renderValue(r[i], false)).join("｜")
          let g = groups.get(gk)
          if (!g) groups.set(gk, { key: byIdx.map((i) => r[i]), rows: [] })
          g = groups.get(gk)!
          g.rows.push(r)
        }
        const outHeaders = [...byKeys]
        const aggResolved: Array<Record<string, unknown> | null> = aggs.length ? aggs : byKeys.map(() => null)
        const aggCols: Array<{ label: string; ci: number; fn: string } | null> = aggResolved.map((spec) => {
          if (!spec) return null
          const key = resolveHeader(spec.col ?? spec.column, frame.headers)
          if (!key) throw new Error(`group：聚合列「${String(spec.col)}」不存在`)
          const fn = String(spec.fn ?? spec.func ?? "sum").toLowerCase()
          const label = String(spec.as ?? `${aggLabel(fn)}(${key})`)
          if (!outHeaders.includes(label)) outHeaders.push(label)
          return { label, ci: frame.headers.indexOf(key), fn }
        })
        const aggFiller = aggResolved.map((spec, i) => aggCols[i] ?? { label: String(spec?.as ?? "计数"), ci: -1, fn: "count" })
        const rows = [...groups.values()].map((g) => {
          const out: unknown[] = [...g.key]
          for (const spec of aggFiller) out.push(aggregateValues(spec.ci >= 0 ? g.rows.map((r) => r[spec.ci]) : g.rows.map(() => 1), spec.fn))
          return out
        })
        frame = { headers: outHeaders, rows }
        stats.push({ action: kind, detail: `${byKeys.join("+")} × ${aggFiller.length} 聚合 → ${rows.length} 组`, rowsIn, rowsOut: frame.rows.length })
        break
      }
      case "pivot": {
        frame = pivotFrame(frame, a)
        stats.push({ action: kind, detail: `行 ${String(a.rows)} × 列 ${String(a.cols)} × 值 ${String(a.values)}（${String(a.agg ?? "sum")}）`, rowsIn, rowsOut: frame.rows.length })
        break
      }
      default:
        throw new Error(`未知动作 ${kind || "(空)"}（支持 filter/select_cols/rename_cols/fill/sort/dedupe/top/slice/compute/aggregate/join/group/pivot）`)
    }
  }
  return { frame, stats }
}

function failCol(ref: string): string {
  throw new Error(`列「${ref}」不存在（可用列：…见上方错误信息）`)
}

function briefCond(cond: unknown, frame: Frame): string {
  if (!cond || typeof cond !== "object") return String(cond)
  try {
    return Object.entries(cond as Record<string, unknown>).map(([k, v]) => `${k}${typeof v === "object" && v ? JSON.stringify(v) : `${typeof v === "string" ? "" : ""}${String(v)}`}`).join(" & ")
  } catch {
    return `${frame.headers.length} 列条件`
  }
}

function aggLabel(fn: string): string {
  switch (fn) {
    case "avg":
    case "mean":
      return "平均"
    case "count":
      return "计数"
    case "min":
      return "最小"
    case "max":
      return "最大"
    case "distinct":
    case "nunique":
      return "去重数"
    default:
      return "合计"
  }
}

function aggregateValues(values: unknown[], fn: string): unknown {
  const nonNull = values.filter((v) => v != null && v !== "")
  switch (fn) {
    case "count":
      return nonNull.length
    case "distinct":
    case "nunique":
      return new Set(nonNull.map((v) => renderValue(v, false))).size
    case "min":
      return nonNull.length ? nonNull.reduce((m, v) => (compareValues(v, m) < 0 ? v : m)) : null
    case "max":
      return nonNull.length ? nonNull.reduce((m, v) => (compareValues(v, m) > 0 ? v : m)) : null
    case "avg":
    case "mean": {
      const nums = nonNull.map(numericOf).filter((n): n is number => n != null)
      return nums.length ? Number((nums.reduce((a, b) => a + b, 0) / nums.length).toFixed(10)) : null
    }
    case "first":
      return nonNull[0] ?? null
    case "last":
      return nonNull[nonNull.length - 1] ?? null
    default: {
      // sum（默认）：数值求和；无任何数值时计数兜底（同 Excel 语义偏差——sum 文本返回 0）
      const nums = nonNull.map(numericOf).filter((n): n is number => n != null)
      return nums.length ? Number(nums.reduce((a, b) => a + b, 0).toFixed(10)) : 0
    }
  }
}

function joinKeyDesc(a: Record<string, unknown>): string {
  const on = a.on ?? a.keys
  if (on != null) return String(Array.isArray(on) ? on.join("+") : on)
  return `左 ${String(Array.isArray(a.left_on) ? a.left_on.join("+") : a.left_on)} = 右 ${String(Array.isArray(a.right_on) ? a.right_on.join("+") : a.right_on)}`
}

function joinFrames(left: Frame, right: Frame, a: Record<string, unknown>): Frame {
  const type = String(a.type ?? "left").toLowerCase()
  const on = a.on ?? a.keys
  let leftKeys: string[]
  let rightKeys: string[]
  if (on != null) {
    const list = Array.isArray(on) ? on : [on]
    leftKeys = list.map((c) => resolveHeader(c, left.headers) ?? failCol(`左表 ${String(c)}`))
    rightKeys = list.map((c) => resolveHeader(c, right.headers) ?? failCol(`右表 ${String(c)}`))
  } else {
    const lo = Array.isArray(a.left_on) ? a.left_on : [a.left_on]
    const ro = Array.isArray(a.right_on) ? a.right_on : [a.right_on]
    leftKeys = lo.map((c) => resolveHeader(c, left.headers) ?? failCol(`左表 ${String(c)}`))
    rightKeys = ro.map((c) => resolveHeader(c, right.headers) ?? failCol(`右表 ${String(c)}`))
  }
  // 右表列并入（键列不重复带出；重名列加后缀）
  const rightCarry: Array<{ label: string; ci: number }> = []
  for (let i = 0; i < right.headers.length; i++) {
    if (rightKeys.includes(right.headers[i])) continue
    let label = right.headers[i]
    while (left.headers.includes(label) || rightCarry.some((c) => c.label === label)) label = `${label}_右`
    rightCarry.push({ label, ci: i })
  }
  const headers = [...left.headers, ...rightCarry.map((c) => c.label)]
  const rightIdx = new Map<string, unknown[]>()
  right.rows.forEach((r) => {
    const k = rightKeys.map((h) => renderValue(r[right.headers.indexOf(h)], false)).join("｜")
    if (!rightIdx.has(k)) rightIdx.set(k, r) // 多匹配取首个（与 VLOOKUP 同语义）
  })
  const rows: unknown[][] = []
  const matchedRight = new Set<string>()
  for (const lr of left.rows) {
    const k = leftKeys.map((h) => renderValue(lr[left.headers.indexOf(h)], false)).join("｜")
    const rr = rightIdx.get(k)
    if (rr) matchedRight.add(k)
    if (!rr && type === "inner") continue
    rows.push([...lr, ...rightCarry.map((c) => (rr ? rr[c.ci] ?? null : null))])
  }
  if (type === "outer" || type === "full") {
    right.rows.forEach((r) => {
      const k = rightKeys.map((h) => renderValue(r[right.headers.indexOf(h)], false)).join("｜")
      if (!matchedRight.has(k)) rows.push([...leftKeys.map(() => null), ...left.headers.slice(leftKeys.length).map(() => null), ...rightCarry.map((c) => r[c.ci] ?? null)])
    })
  }
  return { headers, rows }
}

function pivotFrame(frame: Frame, a: Record<string, unknown>): Frame {
  const rowBy = a.rows ?? a.row ?? a.index
  const colBy = a.cols ?? a.col ?? a.columns
  const valBy = a.values ?? a.value
  if (rowBy == null) throw new Error("pivot：rows（行维度列）必填")
  const rowKey = resolveHeader(rowBy, frame.headers) ?? failCol(String(rowBy))
  const rowCi = frame.headers.indexOf(rowKey)
  const agg = String(a.agg ?? a.fn ?? "sum").toLowerCase()
  const valKeys = valBy != null ? (Array.isArray(valBy) ? valBy : [valBy]) : null
  const valCis = valKeys ? valKeys.map((v) => frame.headers.indexOf(resolveHeader(v, frame.headers) ?? failCol(String(v)))) : null

  const colCi = colBy != null ? frame.headers.indexOf(resolveHeader(colBy, frame.headers) ?? failCol(String(colBy))) : null
  const buckets = new Map<string, { rowKey: unknown[]; colKey: string; values: unknown[][] }>()
  for (const r of frame.rows) {
    const rk = renderValue(r[rowCi], false)
    const ck = colCi != null ? renderValue(r[colCi], false) : "值"
    const gk = `${rk}｜${ck}`
    let b = buckets.get(gk)
    if (!b) buckets.set(gk, (b = { rowKey: [r[rowCi]], colKey: ck, values: [] }))
    if (valCis) b.values.push(valCis.map((i) => r[i] ?? null))
    else b.values.push([1])
  }
  // 列头展开顺序：首见顺序（去重）
  const colKeys: string[] = []
  for (const b of buckets.values()) if (!colKeys.includes(b.colKey)) colKeys.push(b.colKey)
  const valLabels = valCis ? valKeys!.map((v) => String(v)) : [aggLabel(agg)]
  const headers = [rowKey, ...colKeys.flatMap((ck) => (valCis && valCis.length > 1 ? valLabels.map((vl) => `${ck}·${vl}`) : [ck]))]
  // 行分组（保持首见顺序）
  const rowOrder: string[] = []
  const rowGroups = new Map<string, unknown[]>()
  for (const r of frame.rows) {
    const rk = renderValue(r[rowCi], false)
    if (!rowGroups.has(rk)) {
      rowGroups.set(rk, r)
      rowOrder.push(rk)
    }
  }
  const getCell = (rk: string, ck: string, vi: number): unknown => {
    const b = buckets.get(`${rk}｜${ck}`)
    if (!b) return null
    return aggregateValues(b.values.map((vs) => vs[vi]), agg)
  }
  const rows = rowOrder.map((rk) => {
    const out: unknown[] = [rowGroups.get(rk)![rowCi]]
    for (const ck of colKeys) {
      if (valCis && valCis.length > 1) for (let vi = 0; vi < valCis.length; vi++) out.push(getCell(rk, ck, vi))
      else out.push(getCell(rk, ck, 0))
    }
    return out
  })
  const withTotal = a.totals !== false && rows.length
  if (withTotal) {
    // 合计行
    const totalRow: unknown[] = ["合计"]
    for (let ci = 1; ci < headers.length; ci++) totalRow.push(aggregateValues(rows.map((r) => r[ci]), agg))
    rows.push(totalRow)
    // 合计列（多值透视只加首个值列组——简化：每列独立合计已在合计行；列合计略）
  }
  return { headers: uniquifyHeaders(headers), rows }
}

// ---------------------------------------------------------------------------
// 输出器：xlsx / csv / json
// ---------------------------------------------------------------------------

/** Frame → xlsx 落盘：表头加粗+底色+冻结+自动筛选+自适应列宽；数值/日期原生类型；
 *  "0012" 这类文本值（导入侧已保护）按文本写入。 */
async function writeFrameXlsx(frame: Frame, abs: string, ctx: ToolContext): Promise<{ rows: number; cols: number }> {
  const Workbook = await loadWorkbook()
  const wb = new Workbook()
  wb.creator = "GEBAI"
  const ws = wb.addWorksheet("数据")
  ws.addRow(frame.headers)
  frame.headers.forEach(() => {})
  const headRow = ws.getRow(1)
  headRow.font = { bold: true, color: { argb: "FF1F3864" } }
  headRow.fill = { type: "pattern", pattern: "solid", fgColor: { argb: "FFEEF2F8" } }
  for (const r of frame.rows) ws.addRow(r.map((v) => (v === undefined ? null : v)))
  // 自适应列宽（按表头与首 200 行内容长度估宽，CJK 字符按 2 计）
  const widths = frame.headers.map((h, ci) => {
    let w = cjkWidth(h)
    for (let i = 0; i < Math.min(frame.rows.length, 200); i++) w = Math.max(w, cjkWidth(renderValue(frame.rows[i][ci] ?? null, false)))
    return Math.min(40, Math.max(8, w + 2))
  })
  widths.forEach((w, i) => (ws.getColumn(i + 1).width = w))
  if (frame.rows.length && frame.headers.length) {
    ws.views = [{ state: "frozen", ySplit: 1 }]
    ws.autoFilter = `A1:${colLetterOf(frame.headers.length)}${frame.rows.length + 1}`
  }
  const buf = await wb.xlsx.writeBuffer()
  const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf as ArrayBuffer)
  await ctx.writeBinaryFile!(abs, u8)
  return { rows: frame.rows.length, cols: frame.headers.length }
}

function colLetterOf(n: number): string {
  let s = ""
  while (n > 0) {
    const r = (n - 1) % 26
    s = String.fromCharCode(65 + r) + s
    n = Math.floor((n - 1) / 26)
  }
  return s
}

function cjkWidth(s: string): number {
  let w = 0
  for (const c of s) w += /[\u3000-\u9fff\uff00-\uffef]/.test(c) ? 2 : 1
  return w
}

function frameToCsv(frame: Frame): string {
  const esc = (s: string) => (/[",\n\r]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s)
  return [frame.headers.map(esc).join(","), ...frame.rows.map((r) => r.map((v) => esc(renderValue(v ?? null, true))).join(","))].join("\n")
}

// ---------------------------------------------------------------------------
// excel_batch 工具主体
// ---------------------------------------------------------------------------

export const excelBatchTool: Tool = {
  name: "excel_batch",
  description:
    "批量表格数据管道：多文件导入→清洗→统计→多格式导出，一次声明式完成（替代写 py/pandas 脚本）。源（files/rows 二选一）：files 为 glob 模式或路径数组（xlsx/xlsm/csv/tsv，多文件按表头名自动对齐合并；csv 编码自动探测 BOM/UTF-8/GBK，可传 encoding 指定）；rows 内联小表（首行表头）。actions 依次执行：filter（where: {列: 值|{gt/gte/lt/lte/ne/eq/between/in/not_in/contains/starts/ends/empty/not_empty/expr}}）、select_cols、rename_cols（map）、fill、sort（by+order）、dedupe（cols 可省）、top/limit(n)、slice(from,to)、compute（cols: [{name, expr}]——微型表达式，列引用 {列名}，支持 + - * / ^ IF ROUND ABS MIN MAX LEN CONCAT TRIM LEFT RIGHT TEXT NUMBER AND OR NOT ISBLANK）、aggregate（cols: [{col, fn, as}]）、join（path+on 或 left_on/right_on，type: inner/left/outer）、group（by + aggs）、pivot（rows/cols/values/agg）。列引用支持表头名/序号/字母。outputs 数组同时导多目标：{path, format: xlsx|csv|json, encoding?: utf-8|utf-8-sig|gbk}——xlsx 自动表头美化+冻结+筛选+列宽，编号类文本（0012/手机号）不被误转数字。返回各阶段行数统计与结果预览。目标已存在且本会话未读取过时拒绝覆盖（防盲覆盖）。",
  card: { titleParams: ["files", "rows", "path"], args: "json" },
  parameters: schema(
    {
      files: { type: "array", items: { type: "string" }, description: "输入文件：glob 模式（data/*.csv）或路径数组（xlsx/xlsm/csv/tsv）；多文件按表头名合并" },
      encoding: { type: "string", description: "csv 输入编码（缺省自动探测 BOM/UTF-8/GBK）" },
      rows: { type: "array", description: "内联小表：[[表头…], [行…]…]（与 files 二选一）" },
      sheet: { type: "string", description: "xlsx 输入的工作表（名或 1 起始序号，缺省全部工作表依次合并）" },
      sort_files: { type: "string", description: "多文件合并顺序：mtime(新→旧)/name/size，缺省按给定顺序" },
      limit_files: { type: "integer", description: "glob 命中文件数上限（默认 200）" },
      actions: { type: "array", description: "动作管道（依次应用）：[{op: filter|select_cols|rename_cols|fill|sort|dedupe|top|slice|compute|aggregate|join|group|pivot, …}]" },
      outputs: { type: "array", items: { type: "object" }, description: "输出目标：[{path, format: xlsx|csv|json, encoding?: utf-8|utf-8-sig|gbk}]" },
      preview: { type: "integer", description: "返回预览行数（默认 5，0 关闭）" },
    },
    ["outputs"],
  ),
  outputSchema: schema(
    {
      headers: { type: "array", items: { type: "string" }, description: "结果列头" },
      rows: { type: "integer", description: "结果行数" },
      cols: { type: "integer", description: "结果列数" },
      inputs: { type: "array", items: { type: "string" }, description: "实际装载的输入文件" },
      outputs: { type: "array", items: { type: "object" }, description: "各输出目标 {path, rows, format}" },
      stats: { type: "array", items: { type: "object" }, description: "各动作行数变化统计" },
    },
    ["headers", "rows", "cols"],
  ),
  safeMode: false,
  async execute(args, ctx) {
    // ---- 0. 输出目标预检（防盲覆盖在装载源之前统一做——部分执行中途撞守卫体验最差）----
    if (!Array.isArray(args.outputs) || !args.outputs.length) return { output: "excel_batch 失败：outputs 须为非空数组（[{path, format}]）。" }
    const outputs: Array<{ path: string; format: string; encoding?: string; abs: string }> = []
    for (const raw of args.outputs) {
      if (!raw || typeof raw !== "object") continue
      const o = raw as Record<string, unknown>
      const path = String(o.path ?? "")
      if (!path) return { output: "excel_batch 失败：outputs 某项缺 path。" }
      const ext = /\.([a-z0-9]+)$/i.exec(path)?.[1].toLowerCase() ?? ""
      const format = String(o.format ?? (ext === "csv" ? "csv" : ext === "json" ? "json" : "xlsx")).toLowerCase()
      if (!["xlsx", "csv", "json"].includes(format)) return { output: `excel_batch 失败：不支持的输出格式 ${format}（xlsx/csv/json）。` }
      const abs = ctx.resolvePath(path)
      const guardMsg = await writeGuards(abs, ctx)
      if (guardMsg) return { output: `excel_batch 失败（${path}）：${guardMsg}` }
      const blindMsg = await blindOverwriteGuard(abs, path, ctx)
      if (blindMsg) return { output: `excel_batch 失败（${path}）：${blindMsg}` }
      outputs.push({ path, format, encoding: typeof o.encoding === "string" ? o.encoding : undefined, abs })
    }
    if (!outputs.length) return { output: "excel_batch 失败：outputs 中没有有效输出目标。" }

    // ---- 1. 源装载 ----
    const notes: string[] = []
    let frame: Frame | null = null
    const inputFiles: string[] = []
    if (Array.isArray(args.files) && args.files.length) {
      const { files, missing } = await expandFilePatterns(args.files as string[], ctx)
      if (missing.length) notes.push(`未匹配到文件：${missing.join("、")}`)
      // 排序与上限
      let list = files
      const sortMode = String(args.sort_files ?? "").toLowerCase()
      if (sortMode) {
        const meta = await Promise.all(list.map(async (p) => ({ p, st: await statSafe(ctx.resolvePath(p)) })))
        if (sortMode === "mtime") meta.sort((a, b) => (b.st?.mtimeMs ?? 0) - (a.st?.mtimeMs ?? 0))
        else if (sortMode === "size") meta.sort((a, b) => (b.st?.size ?? 0) - (a.st?.size ?? 0))
        else if (sortMode === "name") meta.sort((a, b) => (a.p < b.p ? -1 : 1))
        list = meta.map((m) => m.p)
      }
      const limit = Math.max(1, Math.round(asNum(args.limit_files, 200)))
      if (list.length > limit) {
        notes.push(`命中 ${list.length} 个文件，按 limit_files=${limit} 截断（sort_files 调整取舍）`)
        list = list.slice(0, limit)
      }
      if (!list.length) return { output: `excel_batch 失败：没有可导入的文件${missing.length ? `（${missing.join("、")} 未匹配）` : ""}。` }
      const frames: Array<{ src: string; frame: Frame }> = []
      for (const p of list) {
        const ext = /\.([a-z0-9]+)$/i.exec(p)?.[1].toLowerCase() ?? ""
        if (ext === "xlsx" || ext === "xlsm") {
          const r = await loadXlsxFrame(p, args.sheet, ctx)
          if (r.error || !r.frames.length) {
            notes.push(`${p}：${r.error ?? "无工作表"}，已跳过`)
            continue
          }
          for (const f of r.frames) frames.push({ src: `${p}#${f.sheet}`, frame: f.frame })
          inputFiles.push(p)
        } else if (ext === "csv" || ext === "tsv") {
          const r = await loadDelimitedFrame(p, args.encoding, ctx)
          if (r.error) {
            notes.push(`${p}：${r.error}，已跳过`)
            continue
          }
          frames.push({ src: p, frame: r.frame })
          inputFiles.push(p)
        } else notes.push(`${p}：不支持的扩展名 .${ext}（xlsx/xlsm/csv/tsv），已跳过`)
      }
      if (!frames.length) return { output: `excel_batch 失败：文件均无法装载。${notes.join("；")}` }
      if (frames.length === 1) frame = frames[0].frame
      else {
        const merged = mergeFrames(frames)
        frame = merged.frame
        notes.push(`合并 ${frames.length} 个来源（${frames.map((f) => f.src).join("、")}）→ ${merged.mergedCols.length} 列`)
      }
    } else if (Array.isArray(args.rows) && args.rows.length) {
      const grid = args.rows.map((r) => (Array.isArray(r) ? [...r] : [r])) as unknown[][]
      const headers = uniquifyHeaders((grid.shift() ?? []).map((h) => (h == null ? "" : String(h))))
      frame = { headers, rows: grid }
      inputFiles.push("(inline rows)")
    } else {
      // 纯预览形态：无数据源时 outputs 必填（上游已校验非空）——此分支不可达，仅类型收敛
      return { output: "excel_batch 失败：须提供 files（文件/模式数组）或 rows（内联表）之一（纯预览也要有数据源）。" }
    }
    const srcRows = frame.rows.length
    const srcCols = frame.headers.length
    if (!srcCols) return { output: "excel_batch 失败：源表没有列（表头为空）。" }

    // ---- 2. 动作管道 ----
    const stats: StageStat[] = []
    if (Array.isArray(args.actions) && args.actions.length) {
      const joinLoader = async (path: string, sheet: unknown): Promise<{ frame: Frame; error?: string }> => {
        const ext = /\.([a-z0-9]+)$/i.exec(path)?.[1].toLowerCase() ?? ""
        if (ext === "xlsx" || ext === "xlsm") {
          const r = await loadXlsxFrame(path, sheet ?? 1, ctx)
          if (r.error || !r.frames.length) return { frame: { headers: [], rows: [] }, error: r.error ?? "无工作表" }
          return { frame: r.frames[0].frame }
        }
        if (ext === "csv" || ext === "tsv") return await loadDelimitedFrame(path, undefined, ctx)
        return { frame: { headers: [], rows: [] }, error: `不支持的 join 源 .${ext}` }
      }
      try {
        const r = await applyActions(frame, args.actions, joinLoader)
        frame = r.frame
        stats.push(...r.stats)
      } catch (err) {
        return {
          output: `excel_batch 失败：${(err as Error).message}\n已中止（未写出任何输出文件——管道原子性：要么全部输出成功，要么不动文件）。`,
        }
      }
    }

    // ---- 3. 输出 ----
    const outInfos: Array<Record<string, unknown>> = []
    const failedOut: string[] = []
    for (const o of outputs) {
      try {
        if (o.format === "xlsx") {
          const r = await writeFrameXlsx(frame, o.abs, ctx)
          outInfos.push({ path: o.path, format: "xlsx", rows: r.rows, cols: r.cols })
        } else if (o.format === "csv") {
          const text = frameToCsv(frame)
          const enc = (o.encoding ?? "utf-8-sig").toLowerCase()
          if (enc === "gbk") {
            const g = await encodeGbk(text)
            if (!g) throw new Error("当前运行时不支持 GBK 编码（TextDecoder gbk 不可用）")
            if (g.failed.length) notes.push(`csv(gbk) 有 ${new Set(g.failed).size} 类字符无法编码（已替换为 ？）：${[...new Set(g.failed)].slice(0, 5).join(" ")}`)
            await ctx.writeBinaryFile!(o.abs, g.bytes)
          } else if (enc === "utf-8-sig" || enc === "utf8-sig" || enc === "utf-8-bom") {
            await ctx.writeBinaryFile!(o.abs, new Uint8Array([0xef, 0xbb, 0xbf, ...new TextEncoder().encode(text)]))
          } else {
            await ctx.writeBinaryFile!(o.abs, new TextEncoder().encode(text))
          }
          outInfos.push({ path: o.path, format: "csv", encoding: enc, rows: frame.rows.length, cols: frame.headers.length })
        } else {
          const objects = frame.rows.map((r) => Object.fromEntries(frame!.headers.map((h, i) => [h, r[i] instanceof Date ? renderValue(r[i], false) : r[i] ?? null])))
          const json = JSON.stringify(objects, null, 2)
          await ctx.writeBinaryFile!(o.abs, new TextEncoder().encode(json))
          outInfos.push({ path: o.path, format: "json", rows: frame.rows.length, cols: frame.headers.length })
        }
        ctx.fileGuard?.markRead(o.abs)
      } catch (err) {
        failedOut.push(`${o.path}: ${(err as Error).message}`)
      }
    }
    if (!outInfos.length) return { output: `excel_batch 失败：所有输出均未成功。\n- ${failedOut.join("\n- ")}` }

    // ---- 4. 结果报告 ----
    const previewN = args.preview === 0 ? 0 : Math.max(1, Math.round(asNum(args.preview, 5)))
    let preview = ""
    if (previewN) {
      const esc = (s: string) => s.replace(/\|/g, "\\|").replace(/\n/g, " ")
      const lines = [`| ${frame.headers.map(esc).join(" | ")} |`, `| ${frame.headers.map(() => "---").join(" | ")} |`]
      for (const r of frame.rows.slice(0, previewN)) lines.push(`| ${r.map((v) => esc(renderValue(v ?? null, false))).join(" | ")} |`)
      preview = lines.join("\n")
    }
    const statLines = stats.map((s) => `${s.action}（${s.detail}）：${s.rowsIn} → ${s.rowsOut} 行`)
    const outLines = outInfos.map((o) => `${o.path}（${o.format}${o.encoding ? `/${o.encoding}` : ""}，${o.rows} 行 × ${o.cols} 列）`)
    const output = [
      `excel_batch 完成：${inputFiles.length > 1 || inputFiles[0] !== "(inline rows)" ? `${inputFiles.length} 个来源 ${srcRows} 行` : `内联 ${srcRows} 行`} → 处理后 ${frame.rows.length} 行 × ${frame.headers.length} 列。`,
      statLines.length ? `管道：\n- ${statLines.join("\n- ")}` : "",
      `输出：\n- ${outLines.join("\n- ")}`,
      notes.length ? `注意：\n- ${notes.join("\n- ")}` : "",
      failedOut.length ? `部分输出失败：\n- ${failedOut.join("\n- ")}` : "",
      preview ? `结果预览（前 ${Math.min(previewN, frame.rows.length)} 行）：\n${preview}` : "",
    ]
      .filter(Boolean)
      .join("\n")
    return {
      output,
      data: { headers: frame.headers, rows: frame.rows.length, cols: frame.headers.length, inputs: inputFiles, outputs: outInfos, stats },
      blocks: outputs.map((o) => fileBlocks(o.abs, ctx)).flat(),
    }
  },
}

async function statSafe(p: string): Promise<{ size: number; mtimeMs: number } | null> {
  const { stat } = await import("node:fs/promises")
  return await stat(p).catch(() => null)
}
