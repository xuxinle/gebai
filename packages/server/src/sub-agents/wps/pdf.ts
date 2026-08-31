/**
 * PDF 工具（pdf-lib 生成/编辑 + pdf.js 文本提取，非 AI 依赖）：
 * - pdf_create：markdown/blocks → 排版 PDF（标题/段落/列表/表格/图片/代码/引用/分页/目录、页眉页脚页码）；
 *   CJK 字体自动发现（系统字体目录扫描，TTC 抽取子字体）+ 子集化嵌入，纯西文走标准 14 字体。
 * - pdf_read：逐页文本提取 + 元数据（pdf.js 经 unpdf 的 getDocumentProxy 低层 API——unpdf 的 extractText
 *   走 worker postMessage 在 Bun 下 DataCloneError，不可用）。
 * - pdf_merge / pdf_split / pdf_edit：合并、按区间/每 N 页/单页拆分、页面增删旋转移动 + 元数据 + 水印。
 */
import { PDFDocument, PDFFont, PDFImage, StandardFonts, degrees, rgb } from "pdf-lib"
import fontkit from "@pdf-lib/fontkit"
import { getDocumentProxy, getMeta } from "unpdf"
import { homedir } from "node:os"
import { readdirSync, readFileSync } from "node:fs"
import type { Tool, ToolContext } from "../../core/types"
import { truncate } from "../../core/tools"
import type { MdBlock, MdRun } from "./markdown"
import { bodyInput } from "./markdown"
import { asNum, blindOverwriteGuard, fileBlocks, normColor, readImage, schema, writeGuards } from "./shared"

// ---------------------------------------------------------------------------
// 基础：颜色 / 页码区间
// ---------------------------------------------------------------------------

function hexToRgb(hex: string) {
  const c = normColor(hex) ?? "000000"
  return rgb(parseInt(c.slice(0, 2), 16) / 255, parseInt(c.slice(2, 4), 16) / 255, parseInt(c.slice(4, 6), 16) / 255)
}

/** 页码区间解析："1-3,5,8-"（1 起始，开区间到末页）→ 升序去重 0 起始索引；非法描述抛错。 */
function parsePagesSpec(v: unknown, total: number): number[] {
  const specs: string[] = []
  if (typeof v === "number") specs.push(String(Math.round(v)))
  else if (typeof v === "string") specs.push(...v.split(/[,，;；\s]+/).filter(Boolean))
  else if (Array.isArray(v)) specs.push(...v.map((n) => String(Math.round(asNum(n, 0)))))
  else if (v == null) return Array.from({ length: total }, (_, i) => i)
  else throw new Error(`pages 参数须为区间字符串或数字数组`)
  const out = new Set<number>()
  for (const s of specs) {
    const m = /^(\d+)(?:\s*-\s*(\d+)?)?$/.exec(s.trim())
    if (!m) throw new Error(`页码区间「${s}」无法解析（示例："1-3,5,8-"）`)
    const from = Number(m[1])
    const to = m[2] != null && m[2] !== "" ? Number(m[2]) : m[2] === "" ? total : from
    if (from < 1 || to < from || to > total) throw new Error(`页码区间「${s}」超出范围（共 ${total} 页）`)
    for (let p = from; p <= to; p++) out.add(p - 1)
  }
  return [...out].sort((a, b) => a - b)
}

const CJK_RE = /[\u2E80-\u2FFF\u3000-\u303F\u31C0-\u31EF\u3200-\u4DBF\u4E00-\u9FFF\uF900-\uFAFF\uFE30-\uFE4F\uFF00-\uFFEF]/
const hasCjk = (s: string) => CJK_RE.test(s)

// ---------------------------------------------------------------------------
// CJK 字体：系统目录扫描 + TTC 子字体抽取（embedFont 只收字节，TTC 须重建为独立 TTF）
// ---------------------------------------------------------------------------

/** TTC → 独立 TTF：头部/表目录重建 + 表数据按新偏移重排（保持表序与校验和，fontkit 即可解析）。 */
function extractTtcSubfont(bytes: Uint8Array, index: number): Uint8Array {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const base = dv.getUint32(12 + 4 * index)
  const sfntVersion = dv.getUint32(base)
  const numTables = dv.getUint16(base + 4)
  const recs: Array<{ tag: Uint8Array; checksum: number; data: Uint8Array }> = []
  for (let i = 0; i < numTables; i++) {
    const rec = base + 12 + i * 16
    const offset = dv.getUint32(rec + 8)
    const length = dv.getUint32(rec + 12)
    recs.push({ tag: bytes.slice(rec, rec + 4), checksum: dv.getUint32(rec + 4), data: bytes.slice(offset, offset + length) })
  }
  const head = 12 + numTables * 16
  let total = head
  for (const r of recs) total += r.data.length + ((4 - (r.data.length % 4)) % 4)
  const res = new Uint8Array(total)
  const rdv = new DataView(res.buffer)
  rdv.setUint32(0, sfntVersion)
  rdv.setUint16(4, numTables)
  rdv.setUint16(6, dv.getUint16(base + 6))
  rdv.setUint16(8, dv.getUint16(base + 8))
  rdv.setUint16(10, dv.getUint16(base + 10))
  let cursor = head
  recs.forEach((r, i) => {
    res.set(r.tag, 12 + i * 16)
    rdv.setUint32(12 + i * 16 + 4, r.checksum)
    rdv.setUint32(12 + i * 16 + 8, cursor)
    rdv.setUint32(12 + i * 16 + 12, r.data.length)
    res.set(r.data, cursor)
    cursor += r.data.length + ((4 - (r.data.length % 4)) % 4)
  })
  return res
}

/** 解码 name 表字符串（UTF-16BE / Mac Roman，手工解码不依赖 TextDecoder 标签支持）。 */
function decodeNameString(b: Uint8Array, utf16: boolean): string {
  let s = ""
  if (utf16) for (let i = 0; i + 1 < b.length; i += 2) s += String.fromCharCode((b[i] << 8) | b[i + 1])
  else for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i])
  return s
}

/** TTC 各子字体的 PostScript 名与字重族名（name 表 nameID 6/2），用于挑选 Regular/Bold 变体。 */
function ttcSubfontNames(bytes: Uint8Array): Array<{ postscript: string; subfamily: string }> {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const numFonts = dv.getUint32(8)
  const out: Array<{ postscript: string; subfamily: string }> = []
  for (let f = 0; f < numFonts; f++) {
    const base = dv.getUint32(12 + 4 * f)
    const numTables = dv.getUint16(base + 4)
    let nameTable: Uint8Array | null = null
    for (let i = 0; i < numTables; i++) {
      const rec = base + 12 + i * 16
      if (String.fromCharCode(...bytes.slice(rec, rec + 4)) === "name") {
        const off = dv.getUint32(rec + 8)
        nameTable = bytes.slice(off, off + dv.getUint32(rec + 12))
        break
      }
    }
    let postscript = ""
    let subfamily = ""
    if (nameTable) {
      const ndv = new DataView(nameTable.buffer, nameTable.byteOffset, nameTable.byteLength)
      const count = ndv.getUint16(2)
      const storage = ndv.getUint16(4)
      for (let i = 0; i < count; i++) {
        const rec = 6 + i * 12
        const platform = ndv.getUint16(rec)
        const encoding = ndv.getUint16(rec + 2)
        const nameID = ndv.getUint16(rec + 6)
        const len = ndv.getUint16(rec + 8)
        const off = ndv.getUint16(rec + 10)
        if ((nameID !== 6 && nameID !== 2) || (platform !== 3 && platform !== 1)) continue
        const str = decodeNameString(nameTable.subarray(storage + off, storage + off + len), platform === 3 && encoding === 1)
        if (nameID === 6 && !postscript) postscript = str
        if (nameID === 2 && !subfamily) subfamily = str
      }
    }
    out.push({ postscript, subfamily })
  }
  return out
}

/** TTC 内挑选子字体：wantBold 匹配 subfamily/PostScript 名含 bold；否则优先 Regular/Book/Plain。 */
function pickTtcIndex(bytes: Uint8Array, wantBold: boolean): number {
  const names = ttcSubfontNames(bytes)
  const isBold = (n: { postscript: string; subfamily: string }) => /bold/i.test(n.subfamily) || /bold/i.test(n.postscript)
  if (wantBold) {
    const i = names.findIndex((n) => isBold(n))
    if (i >= 0) return i
  } else {
    const i = names.findIndex((n) => !isBold(n) && (/regular|book|plain|normal/i.test(n.subfamily) || /-regular$/i.test(n.postscript) || !n.subfamily))
    return i >= 0 ? i : 0
  }
  return 0
}

interface FontCandidate {
  file: string
  boldFile?: string
  label: string
  aliases?: string[]
}

/** 常见 CJK 字体候选（按平台命中先后即优先级）。 */
const FONT_CANDIDATES: FontCandidate[] = [
  { file: "msyh.ttc", boldFile: "msyhbd.ttc", label: "微软雅黑", aliases: ["微软雅黑", "microsoft yahei", "yahei", "msyh"] },
  { file: "deng.ttf", boldFile: "dengb.ttf", label: "等线", aliases: ["等线", "dengxian", "deng"] },
  { file: "simhei.ttf", label: "黑体", aliases: ["黑体", "simhei"] },
  { file: "simsun.ttc", label: "宋体", aliases: ["宋体", "simsun"] },
  { file: "pingfang.ttc", label: "苹方", aliases: ["苹方", "pingfang"] },
  { file: "hiragino sans gb.ttc", label: "冬青黑体", aliases: ["hiragino"] },
  { file: "arial unicode.ttf", label: "Arial Unicode" },
  { file: "notosanscjk-regular.ttc", label: "Noto Sans CJK", aliases: ["noto", "noto sans cjk", "notosanscjk"] },
  { file: "notosanscjksc-regular.otf", label: "Noto Sans CJK SC", aliases: ["notosanscjksc"] },
  { file: "notosanscjksc-regular.ttf", label: "Noto Sans CJK SC" },
  { file: "wqy-microhei.ttc", label: "文泉驿微米黑", aliases: ["文泉驿", "wqy"] },
  { file: "wqy-zenhei.ttc", label: "文泉驿正黑" },
  { file: "droidsansfallbackfull.ttf", label: "Droid Sans Fallback" },
  { file: "sourcehansanssc-regular.otf", label: "思源黑体", aliases: ["思源黑体", "source han sans"] },
]

function fontDirs(): string[] {
  if (process.platform === "win32") {
    const dirs: string[] = []
    if (process.env.WINDIR) dirs.push(`${process.env.WINDIR}/Fonts`)
    if (process.env.LOCALAPPDATA) dirs.push(`${process.env.LOCALAPPDATA}/Microsoft/Windows/Fonts`)
    return dirs
  }
  if (process.platform === "darwin") {
    return ["/System/Library/Fonts", "/System/Library/Fonts/Supplemental", "/Library/Fonts", `${homedir()}/Library/Fonts`]
  }
  return ["/usr/share/fonts", "/usr/local/share/fonts", `${homedir()}/.fonts`, `${homedir()}/.local/share/fonts`]
}

let fontIndexCache: Map<string, string> | null = null

/** 系统字体目录索引：小写文件名 → 绝对路径（目录 + 一层子目录扫描，进程内缓存）。
 *  不用 readdirSync recursive:true——Windows Fonts 目录递归扫描触发 EPERM（特殊 ACL），逐层读取正常。 */
function systemFontIndex(): Map<string, string> {
  if (fontIndexCache) return fontIndexCache
  const map = new Map<string, string>()
  const addDir = (dir: string) => {
    let entries: Array<{ name: string; isDirectory(): boolean }> = []
    try {
      entries = readdirSync(dir, { withFileTypes: true }) as Array<{ name: string; isDirectory(): boolean }>
    } catch {
      return
    }
    for (const e of entries) {
      if (e.isDirectory()) {
        // Linux 字体常按厂家/格式分目录（opentype/noto 等），补一层子目录
        const sub = `${dir}/${e.name}`
        try {
          for (const f of readdirSync(sub, { withFileTypes: true }) as Array<{ name: string; isDirectory(): boolean }>) {
            if (f.isDirectory()) continue
            const name = f.name.toLowerCase()
            if (/\.(ttf|otf|ttc)$/.test(name) && !map.has(name)) map.set(name, `${sub}/${f.name}`)
          }
        } catch {
          /* 子目录不可读跳过 */
        }
      } else {
        const name = e.name.toLowerCase()
        if (/\.(ttf|otf|ttc)$/.test(name) && !map.has(name)) map.set(name, `${dir}/${e.name}`)
      }
    }
  }
  for (const dir of fontDirs()) addDir(dir)
  fontIndexCache = map
  return map
}

/** 字体文件读取：会话/项目内路径走 ctx 二进制通道（沙箱边界生效），系统目录兜底直读。 */
async function readFontBytes(path: string, ctx: ToolContext): Promise<Uint8Array> {
  try {
    return await ctx.readBinaryFile(path)
  } catch {
    return new Uint8Array(readFileSync(path))
  }
}

export interface CustomFont {
  regular: Uint8Array
  bold?: Uint8Array
  label: string
}

/** 轮廓检测：pdf-lib 将自定义字体一律按 TrueType 位置嵌入（FontFile2/CIDFontType2）——CFF 轮廓
 *  （OTTO 头的 OTF，含 Noto/思源/PingFang 官方发行版）在该结构下不规范，渲染器拒绝光栅化（文本层完好）。
 *  自定义字体仅支持 TrueType 轮廓（.ttf 或 TrueType 型 .ttc），CFF 在此拒绝并跳过。 */
function assertTrueType(bytes: Uint8Array, label: string): void {
  const magic = String.fromCharCode(...bytes.slice(0, 4))
  if (magic === "OTTO") throw new Error(`${label} 为 CFF 轮廓（OTF）字体——pdf-lib 嵌入后无法光栅化渲染，仅支持 TrueType 轮廓字体（.ttf / TrueType 型 .ttc，如微软雅黑/黑体/文泉驿）`)
  if (magic !== "\x00\x01\x00\x00" && magic !== "true" && magic !== "ttcf") throw new Error(`${label} 不是可识别的字体文件（sfnt 魔数 ${JSON.stringify(magic)}）`)
}

/**
 * 解析 CJK 自定义字体：styleFont 为字体文件路径（.ttf/.ttc，会话/项目内或系统绝对路径）或
 * 字体族别名；缺省扫描系统字体目录按候选优先级取首个命中。找不到返回 null（调用方决定降级或报错）。
 */
async function resolveCustomFont(styleFont: unknown, ctx: ToolContext, warnings: string[]): Promise<CustomFont | null> {
  const index = systemFontIndex()
  const raw = typeof styleFont === "string" ? styleFont.trim() : ""
  const want = raw.toLowerCase()

  const loadFile = async (path: string, wantBold: boolean, label: string): Promise<CustomFont> => {
    const bytes = await readFontBytes(path, ctx)
    const regular = /\.ttc$/i.test(path) ? extractTtcSubfont(bytes, pickTtcIndex(bytes, wantBold)) : bytes
    assertTrueType(regular, label)
    return { regular, label }
  }

  const fromCandidate = async (c: FontCandidate): Promise<CustomFont | null> => {
    const main = index.get(c.file)
    if (!main) return null
    let reg: CustomFont
    try {
      reg = await loadFile(main, false, c.label)
    } catch (err) {
      warnings.push(`字体候选 ${c.file} 不可用：${(err as Error).message}，已跳过`)
      return null
    }
    // 粗体伴随：同名 Bold 文件（msyhbd.ttc/Dengb.ttf）或单 TTC 内多字重子字体（mac/Linux 常见）
    if (c.boldFile) {
      const bp = index.get(c.boldFile)
      if (bp) {
        try {
          return { ...reg, bold: (await loadFile(bp, true, c.label)).regular }
        } catch {
          /* 粗体文件异常按仅常规处理 */
        }
      }
    }
    if (/\.ttc$/i.test(main)) {
      try {
        const rawBytes = await readFontBytes(main, ctx)
        const names = ttcSubfontNames(rawBytes)
        const bi = names.findIndex((n) => /bold/i.test(n.subfamily) || /bold/i.test(n.postscript))
        if (bi > 0 && bi !== pickTtcIndex(rawBytes, false)) {
          const bold = extractTtcSubfont(rawBytes, bi)
          assertTrueType(bold, c.label)
          return { ...reg, bold }
        }
      } catch {
        /* 子字体探测异常按仅常规处理 */
      }
    }
    return reg
  }

  // 显式字体文件路径（会话/项目内或系统绝对路径）
  if (want && /\.(ttf|otf|ttc)$/.test(want) && !FONT_CANDIDATES.some((c) => c.file === want || (c.aliases ?? []).includes(want))) {
    const abs = ctx.resolvePath(raw)
    const label = abs.split(/[\\/]/).pop() || raw
    try {
      const reg = await loadFile(abs, false, label)
      let bold: Uint8Array | undefined
      try {
        bold = (await loadFile(abs.replace(/(\.[^.]+)$/, "bd$1"), true, label)).regular
      } catch {
        /* 无粗体伴随文件 */
      }
      return { regular: reg.regular, bold, label }
    } catch (err) {
      warnings.push(`字体文件 ${styleFont} 加载失败：${(err as Error).message}，已回退默认字体`)
    }
  }

  // 字体族别名匹配 → 候选
  if (want) {
    const c = FONT_CANDIDATES.find((fc) => (fc.aliases ?? []).includes(want) || fc.file === want)
    if (!c) {
      warnings.push(`未识别字体族 ${styleFont}（支持别名：微软雅黑/等线/黑体/宋体/苹方/Noto 等，或传 .ttf/.ttc 文件路径），已回退默认字体`)
    } else {
      const got = await fromCandidate(c)
      if (got) return got
      warnings.push(`字体 ${styleFont} 在本机不可用（${c.file}），已回退默认字体`)
    }
  }

  for (const c of FONT_CANDIDATES) {
    const got = await fromCandidate(c)
    if (got) return got
  }
  return null
}

// ---------------------------------------------------------------------------
// 排版引擎：runs → 行（CJK 逐字断行、拉丁按词）→ 绘制指令
// ---------------------------------------------------------------------------

interface FontSet {
  regular: PDFFont
  bold: PDFFont
  italic: PDFFont
  boldItalic: PDFFont
  code: PDFFont
  custom: boolean
  label: string
  of(bold: boolean, italic: boolean): PDFFont
}

async function embedFontSet(doc: PDFDocument, custom: CustomFont | null, subset = true): Promise<FontSet> {
  if (!custom) {
    const [r, b, i, bi, code] = await Promise.all([
      doc.embedFont(StandardFonts.Helvetica),
      doc.embedFont(StandardFonts.HelveticaBold),
      doc.embedFont(StandardFonts.HelveticaOblique),
      doc.embedFont(StandardFonts.HelveticaBoldOblique),
      doc.embedFont(StandardFonts.Courier),
    ])
    const f = { regular: r, bold: b, italic: i, boldItalic: bi, code, custom: false, label: "Helvetica" }
    return { ...f, of: (bo: boolean, it: boolean) => (bo && it ? bi : bo ? b : it ? i : r) }
  }
  doc.registerFontkit(fontkit)
  const r = await doc.embedFont(custom.regular, { subset })
  const b = custom.bold ? await doc.embedFont(custom.bold, { subset }) : r
  const f = { regular: r, bold: b, italic: r, boldItalic: b, code: r, custom: true, label: custom.label }
  return { ...f, of: (bo: boolean) => (bo ? b : r) }
}

interface Seg {
  text: string
  size: number
  color: string
  bold: boolean
  italic: boolean
  strike: boolean
  code: boolean
  href?: string
}
interface Line {
  segs: Seg[]
  size: number
}

/** 行内原子化：拉丁词/空白/其余单字符（CJK 逐字可断）；br → "\n" 标记原子。 */
function atomize(runs: MdRun[], base: { size: number; color: string }): Seg[] {
  const atoms: Seg[] = []
  const push = (t: string, src: Partial<Seg>) => {
    if (!t) return
    atoms.push({ text: t, size: base.size, color: base.color, bold: false, italic: false, strike: false, code: false, ...src })
  }
  for (const r of runs) {
    if (r.br) push("\n", {})
    const style: Partial<Seg> = {
      bold: r.bold === true,
      italic: r.italic === true,
      strike: r.strike === true,
      code: r.code === true,
      href: r.href,
      size: r.size ? Math.max(4, Math.min(72, r.size)) : base.size,
      color: r.href ? r.color ?? "0563C1" : r.color ?? base.color,
    }
    const re = /[A-Za-z0-9][A-Za-z0-9'’\-_.@%#:/+=]*|\s+|[^\s]/gu
    let m: RegExpExecArray | null
    while ((m = re.exec(r.text))) push(m[0], style)
  }
  return atoms
}

function segWidth(s: Seg, fonts: FontSet): number {
  const f = s.code ? fonts.code : fonts.of(s.bold, s.italic)
  try {
    return f.widthOfTextAtSize(s.text, s.size)
  } catch {
    return s.text.length * s.size * 0.6
  }
}

/** 折行：拉丁词不可断、CJK 逐字断；超宽原子（长 URL 等）按字符硬切。 */
function wrapAtoms(atoms: Seg[], fonts: FontSet, maxWidth: number): Line[] {
  const lines: Line[] = []
  let cur: Seg[] = []
  let curW = 0
  let curSize = 0
  const flush = () => {
    while (cur.length && !cur[cur.length - 1].text.trim()) cur.pop() // 行尾空白不占宽
    if (cur.length) lines.push({ segs: cur, size: curSize })
    cur = []
    curW = 0
    curSize = 0
  }
  const append = (a: Seg) => {
    cur.push(a)
    curW += segWidth(a, fonts)
    curSize = Math.max(curSize, a.size)
  }
  const hardSplit = (a: Seg) => {
    let rest = a.text
    while (rest.length) {
      let take = rest.length
      while (take > 1 && fonts.of(a.bold, a.italic).widthOfTextAtSize(rest.slice(0, take), a.size) > maxWidth) take--
      append({ ...a, text: rest.slice(0, take) })
      rest = rest.slice(take)
      if (rest.length) flush()
    }
  }
  for (const a of atoms) {
    if (a.text === "\n") {
      flush()
      continue
    }
    if (!a.text.trim()) {
      if (cur.length) append(a)
      continue
    }
    const w = segWidth(a, fonts)
    if (w <= maxWidth && (curW + w <= maxWidth || !cur.length)) {
      append(a)
      continue
    }
    if (cur.length) flush()
    if (w <= maxWidth) append(a)
    else hardSplit(a)
  }
  flush()
  return lines
}

function wrapRuns(runs: MdRun[], fonts: FontSet, maxWidth: number, base: { size: number; color: string }): Line[] {
  return wrapAtoms(atomize(runs, base), fonts, maxWidth)
}

// ---------------------------------------------------------------------------
// 页面布局（绘制指令）
// ---------------------------------------------------------------------------

type DrawCmd =
  | { k: "t"; x: number; y: number; s: Seg }
  | { k: "r"; x: number; y: number; w: number; h: number; color: string }
  | { k: "l"; x1: number; y1: number; x2: number; y2: number; color: string; width: number; dash?: boolean }
  | { k: "img"; x: number; y: number; w: number; h: number; path: string }

interface PageGeom {
  w: number
  h: number
  mL: number
  mR: number
  mT: number
  mB: number
}

interface TocEntry {
  level: number
  text: string
  page: number // 最终 1 起始页码
}

const HEAD_SIZE = [17, 14.5, 12.5, 11.5, 11, 10.5]
const HEAD_COLOR = ["1F3864", "1F3864", "262626", "262626", "262626", "262626"]
const BODY_COLOR = "262626"
const LH = 1.6

class Layout {
  pages: DrawCmd[][] = [[]]
  y = 0
  headings: Array<{ level: number; text: string; page: number; afterToc: boolean }> = []
  tocSeen = false
  stats: Record<string, number> = {}
  readonly geom: PageGeom
  readonly fonts: FontSet
  readonly images: Map<string, { w: number; h: number }>
  readonly warnings: string[]
  readonly baseSize: number

  constructor(geom: PageGeom, fonts: FontSet, images: Map<string, { w: number; h: number }>, warnings: string[], baseSize: number) {
    this.geom = geom
    this.fonts = fonts
    this.images = images
    this.warnings = warnings
    this.baseSize = baseSize
    this.y = geom.h - geom.mT
  }

  get contentW(): number {
    return this.geom.w - this.geom.mL - this.geom.mR
  }
  get bottom(): number {
    return this.geom.mB
  }
  get pageIdx(): number {
    return this.pages.length - 1
  }
  private bump(k: string) {
    this.stats[k] = (this.stats[k] ?? 0) + 1
  }
  private push(c: DrawCmd) {
    this.pages[this.pages.length - 1].push(c)
  }
  newPage() {
    this.pages.push([])
    this.y = this.geom.h - this.geom.mT
  }
  ensure(h: number) {
    if (this.y - h < this.bottom) this.newPage()
  }

  drawLineAt(l: Line, x: number, baseline: number) {
    let cx = x
    for (const s of l.segs) {
      const w = segWidth(s, this.fonts)
      this.push({ k: "t", x: cx, y: baseline, s })
      if (s.href) this.push({ k: "l", x1: cx, y1: baseline - 1.5, x2: cx + w, y2: baseline - 1.5, color: "0563C1", width: 0.6 })
      if (s.strike) this.push({ k: "l", x1: cx, y1: baseline + s.size * 0.3, x2: cx + w, y2: baseline + s.size * 0.3, color: s.color, width: 0.6 })
      cx += w
    }
  }

  heading(level: number, runs: MdRun[], record = true) {
    const lvl = Math.min(6, Math.max(1, level))
    const size = HEAD_SIZE[lvl - 1]
    const color = HEAD_COLOR[lvl - 1]
    const lines = wrapRuns(runs.map((r) => ({ ...r, bold: true })), this.fonts, this.contentW, { size, color })
    this.ensure(size * LH + size) // keepNext：标题 + 至少一行同页
    if (record) {
      this.bump("heading")
      this.headings.push({ level: lvl, text: runs.map((r) => r.text).join(""), page: this.pageIdx, afterToc: this.tocSeen })
    }
    if (this.y < this.geom.h - this.geom.mT) this.y -= lvl <= 2 ? 10 : 6
    for (const l of lines) {
      this.ensure(l.size * LH)
      this.y -= l.size * LH
      this.drawLineAt(l, this.geom.mL, this.y + l.size * 0.25)
    }
    this.y -= 3
  }

  paragraph(runs: MdRun[]) {
    this.bump("paragraph")
    const lines = wrapRuns(runs, this.fonts, this.contentW, { size: this.baseSize, color: BODY_COLOR })
    for (const l of lines) {
      this.ensure(l.size * LH)
      this.y -= l.size * LH
      this.drawLineAt(l, this.geom.mL, this.y + l.size * 0.25)
    }
    this.y -= 4
  }

  list(ordered: boolean, items: Array<{ runs: MdRun[]; level: number }>) {
    this.bump("list")
    let n = 0
    for (const it of items) {
      this.bump("listItem")
      n++
      const indent = it.level * 16
      const marker: Seg = { text: ordered ? `${n}. ` : "• ", size: this.baseSize, color: BODY_COLOR, bold: false, italic: false, strike: false, code: false }
      const hang = segWidth(marker, this.fonts)
      const lines = wrapRuns(it.runs, this.fonts, this.contentW - indent - hang, { size: this.baseSize, color: BODY_COLOR })
      lines.forEach((l, i) => {
        this.ensure(l.size * LH)
        this.y -= l.size * LH
        if (i === 0) this.push({ k: "t", x: this.geom.mL + indent, y: this.y + l.size * 0.25, s: marker })
        this.drawLineAt(l, this.geom.mL + indent + hang, this.y + l.size * 0.25)
      })
      this.y -= 2
    }
    this.y -= 3
  }

  quote(runs: MdRun[]) {
    this.bump("quote")
    const x = this.geom.mL + 14
    const lines = wrapRuns(runs.map((r) => ({ ...r, color: r.color ?? "595959" })), this.fonts, this.contentW - 18, { size: this.baseSize, color: "595959" })
    // 竖线逐段绘制（引用跨页时每页一段）
    let segTop = this.y
    let segPage = this.pageIdx
    const closeBar = (endY: number) => {
      if (endY < segTop) this.push({ k: "l", x1: this.geom.mL + 4, y1: segTop - 2, x2: this.geom.mL + 4, y2: endY, color: "999999", width: 2 })
    }
    for (const l of lines) {
      this.ensure(l.size * LH)
      if (this.pageIdx !== segPage) {
        closeBar(this.geom.h - this.geom.mT)
        segTop = this.geom.h - this.geom.mT
        segPage = this.pageIdx
      }
      this.y -= l.size * LH
      this.drawLineAt(l, x, this.y + l.size * 0.25)
    }
    closeBar(this.y)
    this.y -= 6
  }

  code(text: string) {
    this.bump("code")
    const size = Math.min(this.baseSize, 9.5)
    const lh = size * 1.45
    const lines = text.split("\n").map((ln) => ({ segs: [{ text: ln || " ", size, color: "333333", bold: false, italic: false, strike: false, code: true } as Seg], size }))
    let i = 0
    while (i < lines.length) {
      const avail = this.y - this.bottom - 6
      const take = Math.min(lines.length - i, Math.max(1, Math.floor(avail / lh)))
      const block = lines.slice(i, i + take)
      const h = block.length * lh + 6
      if (this.y - h < this.bottom && this.pages[this.pages.length - 1].length) {
        this.newPage()
        continue
      }
      const top = this.y
      this.push({ k: "r", x: this.geom.mL, y: top - h, w: this.contentW, h, color: "F5F5F5" })
      let y = top - 3
      for (const l of block) {
        y -= lh
        this.drawLineAt(l, this.geom.mL + 6, y + size * 0.25)
      }
      this.y = top - h
      i += take
    }
    this.y -= 6
  }

  image(b: Extract<MdBlock, { kind: "image" }>) {
    const meta = this.images.get(b.path)
    if (!meta) return // 预嵌入失败已记 warning
    this.bump("image")
    const maxH = this.geom.h - this.geom.mT - this.geom.mB
    let w = (b.width ?? 0) * 0.75
    let h = (b.height ?? 0) * 0.75
    if (!w || !h) {
      w = meta.w * 0.75
      h = meta.h * 0.75
    }
    const scale = Math.min(1, this.contentW / w, maxH / h)
    w *= scale
    h *= scale
    this.ensure(h + 8)
    this.y -= h
    this.push({ k: "img", x: this.geom.mL + (this.contentW - w) / 2, y: this.y, w, h, path: b.path })
    this.y -= 6
  }

  table(b: Extract<MdBlock, { kind: "table" }>) {
    this.bump("table")
    const nCols = Math.max(1, b.header.length)
    const size = Math.min(this.baseSize, 9.5)
    const lh = size * 1.4
    const pad = 3
    const colW = b.widths && b.widths.length === nCols ? b.widths.map((p) => (this.contentW * p) / 100) : Array.from({ length: nCols }, () => this.contentW / nCols)
    const aligns = b.aligns ?? []
    const cellLines = (cells: MdRun[][]) => Array.from({ length: nCols }, (_, ci) => wrapRuns(cells[ci] ?? [], this.fonts, (colW[ci] ?? colW[nCols - 1]) - 2 * pad, { size, color: BODY_COLOR }))

    const rowsSpec: Array<{ cells: MdRun[][]; header: boolean }> = [
      { cells: b.header.map((h) => [{ text: h, bold: true }]), header: true },
      ...b.rows.map((r) => ({ cells: r, header: false })),
    ]
    const headerLines = cellLines(rowsSpec[0].cells)

    const drawRow = (spec: { cells: MdRun[][]; header: boolean }, linesPerCell: Line[][]) => {
      const top = this.y
      const rowH = Math.max(...linesPerCell.map((ls) => ls.length)) * lh + 2 * pad
      if (spec.header) this.push({ k: "r", x: this.geom.mL, y: top - rowH, w: this.contentW, h: rowH, color: "EEF2F8" })
      let x = this.geom.mL
      linesPerCell.forEach((lines, ci) => {
        const w = colW[ci] ?? colW[nCols - 1]
        const align = aligns[ci]
        lines.forEach((l, li) => {
          const lw = l.segs.reduce((n, s) => n + segWidth(s, this.fonts), 0)
          const ox = align === "center" ? (w - lw) / 2 : align === "right" ? w - lw - pad : pad
          this.drawLineAt(l, x + ox, top - pad - (li + 0.85) * lh)
        })
        this.push({ k: "l", x1: x, y1: top, x2: x, y2: top - rowH, color: "A6A6A6", width: 0.5 })
        x += w
      })
      this.push({ k: "l", x1: x, y1: top, x2: x, y2: top - rowH, color: "A6A6A6", width: 0.5 })
      this.push({ k: "l", x1: this.geom.mL, y1: top, x2: this.geom.mL + this.contentW, y2: top, color: "A6A6A6", width: 0.5 })
      this.push({ k: "l", x1: this.geom.mL, y1: top - rowH, x2: this.geom.mL + this.contentW, y2: top - rowH, color: "A6A6A6", width: 0.5 })
      this.y = top - rowH
    }

    for (const spec of rowsSpec) {
      const linesPerCell = spec === rowsSpec[0] ? headerLines : cellLines(spec.cells)
      const rowH = Math.max(...linesPerCell.map((ls) => ls.length)) * lh + 2 * pad
      if (this.y - rowH < this.bottom && this.pages[this.pages.length - 1].length) {
        this.newPage()
        drawRow(rowsSpec[0], headerLines) // 跨页重绘表头
      }
      drawRow(spec, linesPerCell)
    }
    this.y -= 8
  }

  /** 目录：entries 为 null 时仅登记出现位置（pass1 虚拟布局）；否则渲染条目（真实页码）并独占页。 */
  toc(entries: TocEntry[] | null) {
    this.bump("toc")
    if (!entries) {
      this.tocSeen = true
      return
    }
    this.heading(1, [{ text: "目录", bold: true }], false)
    const size = 10
    for (const e of entries) {
      const indent = (Math.min(4, e.level) - 1) * 14
      const lines = wrapAtoms([{ text: e.text, size, color: BODY_COLOR, bold: false, italic: false, strike: false, code: false }], this.fonts, this.contentW - indent - 40)
      lines.forEach((l, li) => {
        this.ensure(size * LH)
        this.y -= size * LH
        this.drawLineAt(l, this.geom.mL + indent, this.y + size * 0.25)
        if (li === 0) {
          const num: Seg = { text: String(e.page), size, color: BODY_COLOR, bold: false, italic: false, strike: false, code: false }
          const numW = segWidth(num, this.fonts)
          const textW = l.segs.reduce((n, s) => n + segWidth(s, this.fonts), 0)
          this.drawLineAt({ segs: [num], size }, this.geom.mL + this.contentW - numW, this.y + size * 0.25)
          const dashFrom = this.geom.mL + indent + textW + 4
          const dashTo = this.geom.mL + this.contentW - numW - 4
          if (dashTo > dashFrom) this.push({ k: "l", x1: dashFrom, y1: this.y + size * 0.15, x2: dashTo, y2: this.y + size * 0.15, color: "BBBBBB", width: 0.5, dash: true })
        }
      })
      this.y -= 2
    }
    this.newPage() // 目录独占页：其后正文自新页起（页码平移以此为界）
  }
}

/** 渲染绘制指令到 PDF 页（图片经预嵌入映射取 PDFImage）。 */
async function renderCmds(doc: PDFDocument, pages: DrawCmd[][], geom: PageGeom, fonts: FontSet, images: Map<string, PDFImage>): Promise<void> {
  for (const cmds of pages) {
    const page = doc.addPage([geom.w, geom.h])
    for (const c of cmds) {
      if (c.k === "t") {
        page.drawText(c.s.text, { x: c.x, y: c.y, size: c.s.size, font: c.s.code ? fonts.code : fonts.of(c.s.bold, c.s.italic), color: hexToRgb(c.s.color) })
      } else if (c.k === "r") {
        page.drawRectangle({ x: c.x, y: c.y, width: c.w, height: c.h, color: hexToRgb(c.color) })
      } else if (c.k === "l") {
        page.drawLine({ start: { x: c.x1, y: c.y1 }, end: { x: c.x2, y: c.y2 }, thickness: c.width, color: hexToRgb(c.color), ...(c.dash ? { dashArray: [1, 3] } : {}) })
      } else {
        const img = images.get(c.path)
        if (img) page.drawImage(img, { x: c.x, y: c.y, width: c.w, height: c.h })
      }
    }
  }
}

/** 页眉/页脚：居中 9pt，{page}/{pages} 逐页替换（每页独立测量居中）。 */
function drawHeaderFooter(doc: PDFDocument, geom: PageGeom, fonts: FontSet, header?: string, footer?: string): void {
  if (!header && !footer) return
  const pages = doc.getPages()
  const size = 9
  pages.forEach((page, pi) => {
    const slots: Array<[string | undefined, number]> = [
      [header, geom.h - geom.mT / 2 - 2],
      [footer, geom.mB / 2 - 2],
    ]
    for (const [text, y] of slots) {
      if (!text) continue
      const parts = text.split(/(\{page\}|\{pages\})/).filter((p) => p !== "")
      const subst = (p: string) => (p === "{page}" ? String(pi + 1) : p === "{pages}" ? String(pages.length) : p)
      const widths = parts.map((p) => fonts.regular.widthOfTextAtSize(subst(p), size))
      let x = (geom.w - widths.reduce((a, b) => a + b, 0)) / 2
      parts.forEach((p, i) => {
        page.drawText(subst(p), { x, y, size, font: fonts.regular, color: hexToRgb("595959") })
        x += widths[i]
      })
    }
  })
}

// ---------------------------------------------------------------------------
// pdf_create
// ---------------------------------------------------------------------------

interface PdfStyle {
  title?: string
  pageSize: "a4" | "letter"
  orientation: "portrait" | "landscape"
  margins: { top: number; right: number; bottom: number; left: number }
  baseFontRaw: string | undefined
  baseSize: number
  /** 字体子集化开关（默认开：仅嵌入用到字形，产物小；个别字体结构与 pdf-lib 子集器不兼容时关闭整字嵌入）。 */
  subsetFont: boolean
  header?: string
  footer?: string
}

function normalizePdfStyle(v: unknown): PdfStyle {
  const o = v && typeof v === "object" ? (v as Record<string, unknown>) : {}
  const s: PdfStyle = {
    pageSize: o.pageSize === "letter" ? "letter" : "a4",
    orientation: o.orientation === "landscape" ? "landscape" : "portrait",
    margins: { top: 2.54, right: 3.18, bottom: 2.54, left: 3.18 },
    baseFontRaw: typeof o.baseFont === "string" && o.baseFont.trim() ? o.baseFont.trim() : undefined,
    baseSize: asNum(o.baseSize, 10.5),
    subsetFont: o.subsetFont !== false,
  }
  if (typeof o.title === "string" && o.title.trim()) s.title = o.title.trim()
  if (typeof o.header === "string" && o.header.trim()) s.header = o.header.trim()
  if (typeof o.footer === "string" && o.footer.trim()) s.footer = o.footer.trim()
  if (o.margins && typeof o.margins === "object") {
    const m = o.margins as Record<string, unknown>
    for (const k of ["top", "right", "bottom", "left"] as const) {
      const n = asNum(m[k], NaN)
      if (Number.isFinite(n) && n >= 0 && n <= 10) s.margins[k] = n
    }
  }
  return s
}

const PAGE_PT = { a4: [595.28, 841.89] as const, letter: [612, 792] as const }

const PDF_LABEL: Record<string, string> = { heading: "标题", paragraph: "段落", listItem: "列表项", table: "表格", image: "图片", code: "代码块", quote: "引用", toc: "目录", pagebreak: "分页" }

export const pdfCreateTool: Tool = {
  name: "pdf_create",
  description:
    "创建 PDF 文档（富排版：标题层级/段落/列表/表格/图片/代码块/引用/分页/目录/页眉页脚页码）。正文二选一：markdown 文本（语法同 word_create：# 标题、**粗** *斜* ~~删~~ `码` [链](url)、- / 1. 列表、| 表格 |、> 引用、``` 代码块、![图](路径) 嵌入 png/jpg、<!--pagebreak--> 分页、<!--toc--> 目录（自动汇总标题与真实页码））或 blocks JSON。style：pageSize a4/letter、orientation、margins(cm)、baseFont（字体族名：微软雅黑/等线/黑体/宋体/苹方/Noto，或 .ttf/.ttc 文件路径——须 TrueType 轮廓，OTF/CFF 不受支持）、baseSize(pt)、subsetFont（默认 true 子集化嵌入，字体与子集器不兼容致字形异常时关闭）、header/footer（支持 {page}/{pages} 页码占位）。中文自动嵌入系统 CJK 字体（子集化，产物小）；纯西文走标准字体。目标已存在且本会话未读取过时拒绝（防盲覆盖）。",
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "输出路径（.pdf；相对路径以会话工作目录为基准）" },
      markdown: { type: "string", description: "markdown 正文（与 blocks 二选一）" },
      blocks: { type: "array", description: "结构化块数组（与 markdown 二选一），块语法同 word_create" },
      style: {
        type: "object",
        description: "排版：{title, pageSize:a4|letter, orientation:portrait|landscape, margins:{top,right,bottom,left}(cm), baseFont, baseSize(pt), subsetFont(默认true), header, footer({page}/{pages})}",
      },
    },
    ["path"],
  ),
  safeMode: false,
  async execute(args, ctx) {
    const abs = ctx.resolvePath(String(args.path))
    const guardMsg = await writeGuards(abs, ctx)
    if (guardMsg) return { output: guardMsg }
    const blindMsg = await blindOverwriteGuard(abs, String(args.path), ctx)
    if (blindMsg) return { output: blindMsg }

    const style = normalizePdfStyle(args.style)
    const { blocks, warnings } = bodyInput(args)
    if (!blocks.length) return { output: "正文为空：请传 markdown 或 blocks 参数（至少一个内容块）。" }

    // 全文收集：CJK/可编码判定
    const allText = blocks
      .map((b) => {
        if (b.kind === "heading" || b.kind === "paragraph" || b.kind === "quote") return b.runs.map((r) => r.text).join("")
        if (b.kind === "list") return b.items.map((i) => i.runs.map((r) => r.text).join("")).join("")
        if (b.kind === "table") return [...b.header, ...b.rows.flat().map((c) => c.map((r) => r.text).join(""))].join("")
        if (b.kind === "code") return b.text
        return ""
      })
      .join("")

    const doc = await PDFDocument.create()
    doc.setCreator("GEBAI")
    doc.setProducer("GEBAI (pdf-lib)")
    if (style.title) doc.setTitle(style.title)

    // 字体：显式指定或含 CJK → 系统字体解析嵌入；否则标准字体试编码，覆盖不了再回退自定义
    let custom: CustomFont | null = null
    if (style.baseFontRaw || hasCjk(allText)) {
      custom = await resolveCustomFont(style.baseFontRaw, ctx, warnings)
      if (!custom) {
        return {
          output:
            `未找到可用的 CJK 字体（已扫描系统字体目录），无法生成含中文的 PDF。请安装常见中文字体（微软雅黑/思源黑体/Noto CJK 等），或在 style.baseFont 传字体文件路径（.ttf/.otf/.ttc，会话/项目内路径均可）。`,
        }
      }
    } else {
      const probe = await doc.embedFont(StandardFonts.Helvetica)
      try {
        probe.encodeText(allText)
      } catch {
        custom = await resolveCustomFont(undefined, ctx, warnings)
        if (!custom) return { output: `正文含标准字体无法编码的字符（非 WinAnsi 范围）且未找到可嵌入的系统字体——可在 style.baseFont 指定字体文件路径。` }
      }
    }
    const fonts = await embedFontSet(doc, custom, style.subsetFont)

    // 图片预嵌入（pdf-lib 仅 png/jpg；gif/bmp 记 warning 跳过）
    const imageSizes = new Map<string, { w: number; h: number }>()
    const embedded = new Map<string, PDFImage>()
    for (const b of blocks) {
      if (b.kind !== "image") continue
      try {
        const { bytes, type } = await readImage(b.path, ctx)
        if (type !== "png" && type !== "jpg") throw new Error(`图片格式 ${type} 不受 PDF 支持（仅 png/jpg）`)
        const img = type === "png" ? await doc.embedPng(bytes) : await doc.embedJpg(bytes)
        embedded.set(b.path, img)
        imageSizes.set(b.path, { w: img.width, h: img.height })
      } catch (err) {
        warnings.push(`图片 ${b.path} 嵌入失败：${(err as Error).message}`)
      }
    }

    const base = PAGE_PT[style.pageSize]
    const geom: PageGeom = {
      w: style.orientation === "landscape" ? base[1] : base[0],
      h: style.orientation === "landscape" ? base[0] : base[1],
      mL: style.margins.left * 28.3465,
      mR: style.margins.right * 28.3465,
      mT: style.margins.top * 28.3465,
      mB: style.margins.bottom * 28.3465,
    }

    const layoutBlocks = (tocEntries: TocEntry[] | null) => {
      const lay = new Layout(geom, fonts, imageSizes, warnings, style.baseSize)
      for (const b of blocks) {
        switch (b.kind) {
          case "heading":
            lay.heading(b.level, b.runs)
            break
          case "paragraph":
            lay.paragraph(b.runs)
            break
          case "list":
            lay.list(b.ordered, b.items)
            break
          case "table":
            lay.table(b)
            break
          case "code":
            lay.code(b.text)
            break
          case "quote":
            lay.quote(b.runs)
            break
          case "image":
            lay.image(b)
            break
          case "pagebreak":
            lay.newPage()
            break
          case "toc":
            lay.toc(tocEntries)
            break
        }
      }
      return lay
    }

    // 两遍法：pass1 虚拟布局（目录占 0 页）记录标题页码；按条目实际折行估算目录页数 k，
    // pass2 带真实页码条目实排（目录之后的标题页码 = 虚拟页码 + k）
    const pass1 = layoutBlocks(null)
    let finalLay = pass1
    if (pass1.tocSeen && pass1.headings.some((h) => h.afterToc)) {
      const after = pass1.headings.filter((h) => h.afterToc)
      const dummy = new Layout(geom, fonts, imageSizes, warnings, style.baseSize)
      dummy.toc(after.map((h) => ({ level: h.level, text: h.text, page: 1 })))
      const k = dummy.pages.length - 1 // 目录独占页数（末尾 newPage 的空白续页由其后正文复用）
      const entries: TocEntry[] = after.map((h) => ({ level: h.level, text: h.text, page: h.page + k + 1 }))
      finalLay = layoutBlocks(entries)
    }

    await renderCmds(doc, finalLay.pages, geom, fonts, embedded)
    drawHeaderFooter(doc, geom, fonts, style.header, style.footer)

    const bytes = await doc.save()
    await ctx.writeBinaryFile!(abs, bytes)
    ctx.fileGuard?.markRead(abs)

    const statParts = Object.entries(finalLay.stats)
      .filter(([, n]) => n > 0)
      .map(([k, n]) => `${PDF_LABEL[k] ?? k}×${n}`)
    const fontNote = fonts.custom ? fonts.label : "标准西文字体"
    const out =
      `已创建 ${args.path}（${finalLay.pages.length} 页 · ${statParts.join("、")}；${style.pageSize.toUpperCase()} ${style.orientation === "landscape" ? "横向" : "纵向"}，${fontNote} ${style.baseSize}pt）——前端文件面板可预览/下载。` +
      (warnings.length ? `\n注意：\n- ${warnings.join("\n- ")}` : "")
    return { output: out, blocks: fileBlocks(abs, ctx) }
  },
}

// ---------------------------------------------------------------------------
// pdf_read
// ---------------------------------------------------------------------------

export const pdfReadTool: Tool = {
  name: "pdf_read",
  description:
    '读取 PDF 文本：逐页提取文本层（`## 第 N 页` 分节）+ 首行元数据摘要（页数/标题/作者/生成器）。pages 选页（"1-3,5" 区间语法），maxPages 上限（默认 30，防超长输出）；文本层为空的页标注（可能为扫描件/图片型 PDF——引导截图或转图后视觉读取）。加密 PDF 传 password。内容经截断保护。',
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "PDF 路径" },
      pages: { type: "string", description: '页码区间（"1-3,5,8-"，默认全部）' },
      maxPages: { type: "integer", description: "最多读取页数（默认 30）" },
      password: { type: "string", description: "加密 PDF 的打开密码" },
    },
    ["path"],
  ),
  async execute(args, ctx) {
    const abs = ctx.resolvePath(String(args.path))
    let bytes: Uint8Array
    try {
      bytes = await ctx.readBinaryFile(abs)
    } catch {
      return { output: `pdf_read 失败：文件不存在或不可读（${args.path}）。目标在项目内时用 project 参数指定项目。` }
    }
    ctx.fileGuard?.markRead(abs)

    let pdf
    try {
      pdf = await getDocumentProxy(bytes, args.password != null ? { password: String(args.password) } : undefined)
    } catch (err) {
      const msg = (err as Error).message || ""
      if (/password/i.test(msg)) return { output: `pdf_read 失败：PDF 已加密——用 password 参数传打开密码。` }
      return { output: `pdf_read 失败：${msg || "无法解析"}——可能不是有效 PDF 文件。` }
    }

    const total = pdf.numPages
    let selected: number[]
    try {
      selected = parsePagesSpec(args.pages, total)
    } catch (err) {
      return { output: `pdf_read 失败：${(err as Error).message}` }
    }
    const maxPages = Math.max(1, Math.round(asNum(args.maxPages, 30)))
    const capped = selected.slice(0, maxPages)

    let metaNote = ""
    try {
      const meta = await getMeta(pdf)
      const info = (meta.info ?? {}) as Record<string, unknown>
      const bits = [info.Title, info.Author, info.Producer].filter((v) => typeof v === "string" && v).map(String)
      if (bits.length) metaNote = ` · ${bits.join(" / ")}`
    } catch {
      /* 元数据缺失不影响读取 */
    }

    const lines: string[] = [`（共 ${total} 页${metaNote}）`]
    for (const idx of capped) {
      const page = await pdf.getPage(idx + 1)
      const tc = await page.getTextContent()
      let text = tc.items
        .map((it: unknown) => {
          const o = it as { str?: string; hasEOL?: boolean }
          return (o.str ?? "") + (o.hasEOL ? "\n" : "")
        })
        .join("")
      text = text
        .split("\n")
        .map((l) => l.replace(/\s+$/, ""))
        .join("\n")
        .replace(/\n{3,}/g, "\n\n")
        .trim()
      lines.push("", `## 第 ${idx + 1} 页`)
      lines.push(text || "（空白页——文本层为空，可能为扫描件/图片型 PDF；可用截图工具转图后视觉读取）")
    }
    if (selected.length > capped.length) lines.push(`（已读前 ${capped.length} 页，共选中 ${selected.length} 页——maxPages 调大继续）`)
    const truncated = await truncate(lines.join("\n"), "pdf_read", ctx)
    return { ...truncated, blocks: fileBlocks(abs, ctx) }
  },
}

// ---------------------------------------------------------------------------
// pdf_merge
// ---------------------------------------------------------------------------

async function loadPdfDoc(bytes: Uint8Array, display: string): Promise<PDFDocument> {
  try {
    return await PDFDocument.load(bytes)
  } catch (err) {
    const msg = (err as Error).message || ""
    if (/encrypt/i.test(msg)) throw new Error(`${display} 已加密，pdf-lib 无法处理加密 PDF（请先解密另存）`)
    throw new Error(`${display} 无法解析（${msg}）`)
  }
}

/** pdf-lib 的 removePage 不失效 pageCache（仅 insertPage 失效）——删除后 getPage/getPages 返回
 *  幽灵页，混合 delete/rotate/move 时会取错页。补失效（私有字段防御式访问，失败不阻塞）。 */
function invalidatePageCache(doc: PDFDocument): void {
  ;(doc as unknown as { pageCache?: { invalidate?: () => void } }).pageCache?.invalidate?.()
}

export const pdfMergeTool: Tool = {
  name: "pdf_merge",
  description:
    '合并多个 PDF 为一个：inputs 数组每项为路径字符串或 {path, pages}（pages "1-3,5" 选页——抽取部分页合并）。按数组顺序拼接，输出页数为各输入选中页数之和。目标已存在且本会话未读取过时拒绝（防盲覆盖）。',
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "输出路径（.pdf）" },
      inputs: {
        type: "array",
        description: '输入 PDF 数组：["a.pdf", {path: "b.pdf", pages: "2-5"}]，路径相对会话工作目录',
      },
    },
    ["path", "inputs"],
  ),
  safeMode: false,
  async execute(args, ctx) {
    const abs = ctx.resolvePath(String(args.path))
    const guardMsg = await writeGuards(abs, ctx)
    if (guardMsg) return { output: guardMsg }
    const blindMsg = await blindOverwriteGuard(abs, String(args.path), ctx)
    if (blindMsg) return { output: blindMsg }
    const inputs = Array.isArray(args.inputs) ? args.inputs : []
    if (inputs.length < 2) return { output: "pdf_merge 需要 inputs 数组至少两个输入（单文件的页面重排用 pdf_edit 的 move op）。" }

    const out = await PDFDocument.create()
    out.setCreator("GEBAI")
    const parts: string[] = []
    let total = 0
    for (const raw of inputs) {
      const item = typeof raw === "string" ? { path: raw } : raw && typeof raw === "object" ? (raw as Record<string, unknown>) : null
      if (!item || typeof item.path !== "string") {
        return { output: `inputs 项格式错误：应为路径字符串或 {path, pages}（收到 ${JSON.stringify(raw)?.slice(0, 60)}）。` }
      }
      let bytes: Uint8Array
      try {
        bytes = await ctx.readBinaryFile(ctx.resolvePath(item.path))
      } catch {
        return { output: `pdf_merge 失败：输入文件不存在或不可读（${item.path}）。` }
      }
      let src: PDFDocument
      try {
        src = await loadPdfDoc(bytes, item.path)
      } catch (err) {
        return { output: `pdf_merge 失败：${(err as Error).message}` }
      }
      let indices: number[]
      try {
        indices = parsePagesSpec(item.pages, src.getPageCount())
      } catch (err) {
        return { output: `pdf_merge 失败：${item.path} ${(err as Error).message}` }
      }
      const copied = await out.copyPages(src, indices)
      for (const p of copied) out.addPage(p)
      total += indices.length
      parts.push(`${item.path}（${indices.length} 页）`)
      ctx.fileGuard?.markRead(ctx.resolvePath(item.path))
    }

    const bytes = await out.save()
    await ctx.writeBinaryFile!(abs, bytes)
    ctx.fileGuard?.markRead(abs)
    return {
      output: `已合并 ${inputs.length} 个文件 → ${args.path}（共 ${total} 页：${parts.join(" + ")}）——前端文件面板可预览/下载。`,
      blocks: fileBlocks(abs, ctx),
    }
  },
}

// ---------------------------------------------------------------------------
// pdf_split
// ---------------------------------------------------------------------------

export const pdfSplitTool: Tool = {
  name: "pdf_split",
  description:
    '拆分 PDF 为多个文件：mode=ranges 按 ranges 拆（每区间一个文件，如 ["1-3","4-6"]）；mode=every 每 N 页一个文件；mode=single 每页一个文件（缺省按已传参数推断）。输出到 outdir（默认会话工作目录），命名 {prefix}-p{起}-{止}.pdf。',
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "源 PDF 路径" },
      mode: { type: "string", description: '拆分模式：ranges / every / single（缺省按参数推断：有 ranges→ranges，有 every→every，否则 single）' },
      ranges: { type: "array", description: 'ranges 模式：区间数组 ["1-3", "4-6", "7"]' },
      every: { type: "integer", description: "every 模式：每 N 页一个文件" },
      outdir: { type: "string", description: "输出目录（默认会话工作目录）" },
      prefix: { type: "string", description: "输出文件名前缀（默认源文件名去扩展名）" },
    },
    ["path"],
  ),
  safeMode: false,
  async execute(args, ctx) {
    const srcAbs = ctx.resolvePath(String(args.path))
    let bytes: Uint8Array
    try {
      bytes = await ctx.readBinaryFile(srcAbs)
    } catch {
      return { output: `pdf_split 失败：文件不存在或不可读（${args.path}）。` }
    }
    ctx.fileGuard?.markRead(srcAbs)
    let src: PDFDocument
    try {
      src = await loadPdfDoc(bytes, String(args.path))
    } catch (err) {
      return { output: `pdf_split 失败：${(err as Error).message}` }
    }
    const total = src.getPageCount()

    let mode = typeof args.mode === "string" ? args.mode.toLowerCase() : ""
    if (!["ranges", "every", "single"].includes(mode)) mode = args.ranges != null ? "ranges" : args.every != null ? "every" : "single"

    const groups: Array<{ indices: number[]; label: string }> = []
    try {
      if (mode === "ranges") {
        const specs = Array.isArray(args.ranges) ? args.ranges.map(String) : typeof args.ranges === "string" ? args.ranges.split(/[,，;；\s]+/).filter(Boolean) : []
        if (!specs.length) return { output: 'ranges 模式需传 ranges 数组（如 ["1-3","4-6"]）。' }
        for (const s of specs) {
          const indices = parsePagesSpec(s, total)
          if (!indices.length) continue
          groups.push({ indices, label: `p${indices[0] + 1}${indices.length > 1 ? `-${indices[indices.length - 1] + 1}` : ""}` })
        }
      } else if (mode === "every") {
        const n = Math.max(1, Math.round(asNum(args.every, 1)))
        for (let i = 0; i < total; i += n) {
          const indices = Array.from({ length: Math.min(n, total - i) }, (_, j) => i + j)
          groups.push({ indices, label: `p${i + 1}${indices.length > 1 ? `-${i + indices.length}` : ""}` })
        }
      } else {
        for (let i = 0; i < total; i++) groups.push({ indices: [i], label: `p${i + 1}` })
      }
    } catch (err) {
      return { output: `pdf_split 失败：${(err as Error).message}` }
    }
    if (!groups.length) return { output: "pdf_split 失败：没有可拆分的区间。" }

    const prefix = typeof args.prefix === "string" && args.prefix.trim() ? args.prefix.trim().replace(/\.pdf$/i, "") : String(args.path).replace(/[\\/]/g, "/").split("/").pop()!.replace(/\.pdf$/i, "")
    const outDirAbs = ctx.resolvePath(typeof args.outdir === "string" && args.outdir.trim() ? args.outdir.trim() : ".")

    const outputs: Array<{ abs: string; rel: string; pages: number }> = []
    for (const g of groups) {
      const out = await PDFDocument.create()
      const copied = await out.copyPages(src, g.indices)
      for (const p of copied) out.addPage(p)
      const rel = `${prefix}-${g.label}.pdf`
      const abs = `${outDirAbs}/${rel}`.replaceAll("\\", "/")
      const guard = await writeGuards(abs, ctx)
      if (guard) return { output: `pdf_split 失败（写入 ${rel} 前被拦截）：${guard}` }
      const blind = await blindOverwriteGuard(abs, rel, ctx)
      if (blind) return { output: blind }
      await ctx.writeBinaryFile!(abs, await out.save())
      ctx.fileGuard?.markRead(abs)
      outputs.push({ abs, rel, pages: g.indices.length })
    }

    const dirNote = outDirAbs.startsWith(ctx.workdir) ? outDirAbs.slice(ctx.workdir.length).replace(/^[/\\]/, "") : outDirAbs
    return {
      output: `已将 ${args.path}（${total} 页）拆分为 ${outputs.length} 个文件（${dirNote ? `输出目录 ${dirNote}` : "输出目录为会话工作目录"}）：\n${outputs.map((o) => `- ${o.rel}（${o.pages} 页）`).join("\n")}`,
      blocks: outputs.flatMap((o) => fileBlocks(o.abs, ctx)),
    }
  },
}

// ---------------------------------------------------------------------------
// pdf_edit
// ---------------------------------------------------------------------------

export const pdfEditTool: Tool = {
  name: "pdf_edit",
  description:
    '编辑已有 PDF（ops 按序执行，页码均指当前状态）：delete 删页（pages "2,5-6"）、rotate 旋转（pages + degrees 90/180/270）、move 移动页序（from/to 1 起始位置）、metadata 设元数据（title/author/subject/keywords/creator）、watermark 水印（text，fontSize/color/opacity/angle/pages 可调，中文自动嵌字体）。编辑产物自校验后落盘，失败不改原文件。',
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "目标 PDF 路径（原地修改）" },
      ops: {
        type: "array",
        description:
          '操作数组：[{op:"delete", pages:"2,5-6"}, {op:"rotate", pages:"1-3", degrees:90}, {op:"move", from:5, to:2}, {op:"metadata", title:"…", author:"…"}, {op:"watermark", text:"草稿", fontSize:52, color:"888888", opacity:0.15, angle:45, pages:"1-3"}]',
      },
    },
    ["path", "ops"],
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
      return { output: `pdf_edit 失败：文件不存在或不可读（${args.path}）。` }
    }
    let doc: PDFDocument
    try {
      doc = await loadPdfDoc(bytes, String(args.path))
    } catch (err) {
      return { output: `pdf_edit 失败：${(err as Error).message}` }
    }

    const ops = Array.isArray(args.ops) ? args.ops : []
    if (!ops.length) return { output: "ops 参数须为非空数组（delete/rotate/move/metadata/watermark）。" }
    const applied: string[] = []

    for (const raw of ops) {
      if (!raw || typeof raw !== "object") continue
      const op = raw as Record<string, unknown>
      const kind = String(op.op ?? op.type ?? "")
      try {
        if (kind === "delete") {
          const indices = parsePagesSpec(op.pages, doc.getPageCount())
          for (const i of [...indices].sort((a, b) => b - a)) doc.removePage(i)
          invalidatePageCache(doc)
          applied.push(`删除 ${indices.length} 页`)
        } else if (kind === "rotate") {
          const deg = Math.round(asNum(op.degrees, 90))
          if (![90, 180, 270, -90, -180, -270].includes(deg)) throw new Error(`degrees 须为 ±90/±180/±270（收到 ${deg}）`)
          const indices = parsePagesSpec(op.pages, doc.getPageCount())
          for (const i of indices) {
            const page = doc.getPage(i)
            page.setRotation(degrees((((page.getRotation().angle + deg) % 360) + 360) % 360))
          }
          applied.push(`旋转 ${indices.length} 页 ${deg}°`)
        } else if (kind === "move") {
          const from = Math.round(asNum(op.from, NaN))
          const to = Math.round(asNum(op.to, NaN))
          const count = doc.getPageCount()
          if (!(from >= 1 && from <= count && to >= 1 && to <= count)) throw new Error(`from/to 须在 1..${count}（收到 ${from}→${to}）`)
          if (from !== to) {
            const page = doc.getPage(from - 1)
            doc.removePage(from - 1)
            doc.insertPage(Math.min(Math.max(to - 1, 0), doc.getPageCount()), page)
            applied.push(`第 ${from} 页移至第 ${to} 页`)
          }
        } else if (kind === "metadata") {
          if (typeof op.title === "string" && op.title.trim()) doc.setTitle(op.title.trim())
          if (typeof op.author === "string" && op.author.trim()) doc.setAuthor(op.author.trim())
          if (typeof op.subject === "string" && op.subject.trim()) doc.setSubject(op.subject.trim())
          if (typeof op.keywords === "string" && op.keywords.trim()) doc.setKeywords(op.keywords.trim().split(/[,，;；\s]+/).filter(Boolean))
          if (typeof op.creator === "string" && op.creator.trim()) doc.setCreator(op.creator.trim())
          applied.push("设置元数据")
        } else if (kind === "watermark") {
          const text = typeof op.text === "string" ? op.text.trim() : ""
          if (!text) throw new Error("watermark 需要 text")
          const size = Math.max(8, Math.min(200, asNum(op.fontSize, 52)))
          const color = normColor(op.color) ?? "888888"
          const opacity = Math.max(0.02, Math.min(1, asNum(op.opacity, 0.15)))
          const angle = asNum(op.angle, 45)
          const indices = parsePagesSpec(op.pages, doc.getPageCount())
          let font: PDFFont
          if (hasCjk(text)) {
            const custom = await resolveCustomFont(undefined, ctx, [])
            if (!custom) throw new Error("水印含中文但未找到系统 CJK 字体——可先安装中文字体，或改用西文水印文本")
            doc.registerFontkit(fontkit)
            font = await doc.embedFont(custom.regular, { subset: true })
          } else {
            font = await doc.embedFont(StandardFonts.HelveticaBold)
          }
          const tw = font.widthOfTextAtSize(text, size)
          const rad = (angle * Math.PI) / 180
          for (const i of indices) {
            const page = doc.getPage(i)
            const { width, height } = page.getSize()
            page.drawText(text, {
              x: width / 2 - (tw / 2) * Math.cos(rad),
              y: height / 2 - (tw / 2) * Math.sin(rad),
              size,
              font,
              color: hexToRgb(color),
              opacity,
              rotate: degrees(angle),
            })
          }
          applied.push(`水印「${text}」× ${indices.length} 页`)
        } else {
          throw new Error(`未知 op: ${kind || "(空)"}（支持 delete/rotate/move/metadata/watermark）`)
        }
      } catch (err) {
        return { output: `pdf_edit 失败：op ${JSON.stringify(op).slice(0, 80)} 执行出错——${(err as Error).message}。已放弃写入，原文件未改动。` }
      }
    }

    const outBytes = await doc.save()
    try {
      await PDFDocument.load(outBytes) // 写前自校验：产物须可重新解析
    } catch (err) {
      return { output: `pdf_edit 失败：编辑后产物校验未通过（${(err as Error).message}），已放弃写入，原文件未改动。` }
    }
    await ctx.writeBinaryFile!(abs, outBytes)
    ctx.fileGuard?.markRead(abs)

    return {
      output: `已对 ${args.path} 应用 ${applied.length} 项编辑（${applied.join("；")}），现共 ${doc.getPageCount()} 页——前端文件面板可预览/下载。`,
      blocks: fileBlocks(abs, ctx),
    }
  },
}
