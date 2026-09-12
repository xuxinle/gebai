/** 文件工作台 · 文件操作服务：目录列举 / 元信息 / 文本读取（编码与行尾探测）/ 二进制流（HTTP Range）/
 *  递归打包 / 内容搜索。所有函数接收**已解析并校验过的绝对路径**（边界校验在 roots.ts，本模块只管 IO 与语义）。
 *
 *  设计要点：
 *  - 只读与写入分离（本模块 P0 只读；写操作在 P2 补齐同文件）；
 *  - 大文件保护：读取上限（GEBAI_FS_MAX_READ）内返回内容并标 truncated，超限不读入内存；
 *  - 编码：BOM → UTF-8 严格校验 → GB18030/UTF-16 回退（中文 Windows 老文件刚需）；
 *  - Range：视频拖动 / PDF 跳页 / 断点续传的唯一前提（Bun.file().slice 原生支持，无需整文件入内存）。
 */
import { createHash } from "node:crypto"
import { existsSync, lstatSync, readdirSync, statSync } from "node:fs"
import { readFile, stat } from "node:fs/promises"
import { isAbsolute, join, relative, resolve, sep } from "node:path"
import { buildZip } from "../../zip"
import { WALK_SKIP_DIRS } from "../support/walk"
import { fsBadRequest, fsNotFound, fsTooLarge, isInside, isWindowsFs, resolveInRoot } from "./roots"
import { extOf, kindForPath, languageForPath, mimeForPath, type FileKind } from "./mime"

/** 目录条目（`fs/list` 返回；前端树按此渲染）。 */
export interface DirEntry {
  name: string
  /** root 内相对路径（POSIX 风格） */
  path: string
  type: "file" | "dir" | "symlink" | "other"
  size: number
  mtime: number
  kind?: FileKind
  language?: string
  editable?: boolean
}

export interface ListResult {
  entries: DirEntry[]
  truncated: boolean
  total: number
}

export interface FileStat {
  path: string
  type: "file" | "dir" | "symlink" | "other"
  size: number
  mtime: number
  ctime: number
  mode: number
  etag: string
  kind: FileKind
  language: string
  mime: string
  editable: boolean
  /** 是否符号链接（安全提示用） */
  symlink: boolean
}

export interface TextReadResult {
  content: string
  encoding: string
  eol: "lf" | "crlf" | "cr" | "mixed"
  size: number
  mtime: number
  etag: string
  truncated: boolean
  /** 探测为二进制（不可按文本编辑） */
  binary: boolean
  kind: FileKind
  language: string
}

/** 单层目录列举上限（超出标 truncated，前端提示并用过滤器缩小范围）。 */
export const LIST_MAX_ENTRIES = 5000
/** 默认读取上限（10MB）。 */
export const DEFAULT_MAX_READ = 10 * 1024 * 1024
/** 文本读取上限（超过即只给下载，避免 Monaco 卡死）。 */
export const DEFAULT_MAX_TEXT_READ = 10 * 1024 * 1024

/** ETag：mtime + size + 路径哈希（乐观锁与缓存校验共用；跨进程稳定，不读文件内容）。 */
export function etagOf(abs: string, size: number, mtimeMs: number): string {
  const h = createHash("sha1").update(`${abs}:${size}:${mtimeMs}`).digest("hex").slice(0, 16)
  return `W/"${Math.round(mtimeMs)}-${size}-${h}"`
}

/** 隐藏判定（Unix 点文件；Windows 隐藏属性不额外处理——attribute 读取需另调 API）。 */
export function isHiddenName(name: string): boolean {
  return name.startsWith(".")
}

function typeOfDirent(d: { isDirectory(): boolean; isFile(): boolean; isSymbolicLink(): boolean }): DirEntry["type"] {
  if (d.isSymbolicLink()) return "symlink"
  if (d.isDirectory()) return "dir"
  if (d.isFile()) return "file"
  return "other"
}

/** 自然排序比较（`a2` 在 `a10` 前；目录由调用方先行分组）。 */
export function naturalCompare(a: string, b: string): number {
  const re = /(\d+)|(\D+)/g
  const ax = a.toLowerCase().match(re) ?? []
  const bx = b.toLowerCase().match(re) ?? []
  for (let i = 0; i < Math.max(ax.length, bx.length); i++) {
    const x = ax[i]
    const y = bx[i]
    if (x === undefined) return -1
    if (y === undefined) return 1
    const nx = Number(x)
    const ny = Number(y)
    if (!Number.isNaN(nx) && !Number.isNaN(ny)) {
      if (nx !== ny) return nx - ny
    } else if (x !== y) {
      return x < y ? -1 : 1
    }
  }
  return 0
}

export type SortKey = "name" | "mtime" | "size" | "type"

/**
 * 单层目录列举（root 内相对路径 → 条目清单）。
 * 符号链接不跟随（仅标注类型，防越界与循环）；隐藏文件按 showHidden 过滤。
 */
export async function listDirectory(
  rootAbs: string,
  relPath: string,
  opts: { showHidden?: boolean; sort?: SortKey; dirsFirst?: boolean; limit?: number } = {},
): Promise<ListResult> {
  const absDir = resolveInRoot(rootAbs, relPath)
  let st
  try {
    st = statSync(absDir)
  } catch {
    throw fsNotFound(`目录不存在: ${relPath || "/"}`)
  }
  if (!st.isDirectory()) throw fsBadRequest(`不是目录: ${relPath || "/"}`)
  let raw
  try {
    raw = readdirSync(absDir, { withFileTypes: true })
  } catch (err) {
    throw fsBadRequest(`目录不可读: ${relPath || "/"}（${(err as Error).message}）`)
  }
  const showHidden = opts.showHidden === true
  const entries: DirEntry[] = []
  for (const d of raw) {
    if (!showHidden && isHiddenName(d.name)) continue
    const full = join(absDir, d.name)
    const type = typeOfDirent(d)
    let size = 0
    let mtime = 0
    try {
      const s = lstatSync(full)
      size = s.size
      mtime = s.mtimeMs
    } catch {
      continue // 竞态删除：跳过
    }
    const rel = relOf(rootAbs, full)
    const kind = type === "file" ? kindForPath(d.name) : undefined
    entries.push({
      name: d.name,
      path: rel,
      type,
      size,
      mtime,
      ...(kind ? { kind, language: languageForPath(d.name), editable: kind === "text" || kind === "diagram" } : {}),
    })
  }
  const sort = opts.sort ?? "name"
  const dirsFirst = opts.dirsFirst !== false
  entries.sort((a, b) => {
    if (dirsFirst) {
      const da = a.type === "dir" ? 0 : 1
      const db = b.type === "dir" ? 0 : 1
      if (da !== db) return da - db
    }
    if (sort === "mtime") return b.mtime - a.mtime || naturalCompare(a.name, b.name)
    if (sort === "size") return b.size - a.size || naturalCompare(a.name, b.name)
    if (sort === "type") return (extOf(a.name) || "").localeCompare(extOf(b.name) || "") || naturalCompare(a.name, b.name)
    return naturalCompare(a.name, b.name)
  })
  const limit = opts.limit ?? LIST_MAX_ENTRIES
  const total = entries.length
  return { entries: entries.slice(0, limit), truncated: total > limit, total }
}

/** root 内相对路径（POSIX 风格）。 */
export function relOf(rootAbs: string, abs: string): string {
  const rel = relative(rootAbs, abs).replace(/\\/g, "/")
  return rel === "." ? "" : rel
}

/** 单路径元信息（含 kind/language/etag，供前端决定 Viewer 与编码提示）。 */
export async function statPath(rootAbs: string, relPath: string): Promise<FileStat> {
  const abs = resolveInRoot(rootAbs, relPath, { allowAbsolute: true })
  let ls
  try {
    ls = lstatSync(abs)
  } catch {
    throw fsNotFound(`文件不存在: ${relPath}`)
  }
  const symlink = ls.isSymbolicLink()
  let target = ls
  if (symlink) {
    try {
      target = statSync(abs)
    } catch {
      throw fsNotFound(`符号链接目标不存在: ${relPath}`)
    }
  }
  const type: FileStat["type"] = symlink ? "symlink" : target.isDirectory() ? "dir" : target.isFile() ? "file" : "other"
  const name = relPath.replace(/\\/g, "/").split("/").pop() ?? relPath
  const kind = kindForPath(name)
  return {
    path: relOf(rootAbs, abs),
    type,
    size: target.size,
    mtime: target.mtimeMs,
    ctime: target.ctimeMs,
    mode: target.mode,
    etag: etagOf(abs, target.size, target.mtimeMs),
    kind,
    language: languageForPath(name),
    mime: mimeForPath(name),
    editable: kind === "text" || kind === "diagram",
    symlink,
  }
}

/** BOM 探测结果。 */
function bomOf(buf: Uint8Array): { encoding: "utf-8" | "utf-16le" | "utf-16be"; bomLen: number } | null {
  if (buf.length >= 3 && buf[0] === 0xef && buf[1] === 0xbb && buf[2] === 0xbf) return { encoding: "utf-8", bomLen: 3 }
  if (buf.length >= 2 && buf[0] === 0xff && buf[1] === 0xfe) return { encoding: "utf-16le", bomLen: 2 }
  if (buf.length >= 2 && buf[0] === 0xfe && buf[1] === 0xff) return { encoding: "utf-16be", bomLen: 2 }
  return null
}

/** 二进制启发判定：前 8KB 内出现 NUL，或控制字符占比过高。 */
export function looksBinary(buf: Uint8Array): boolean {
  const n = Math.min(buf.length, 8192)
  if (!n) return false
  let ctrl = 0
  for (let i = 0; i < n; i++) {
    const b = buf[i]
    if (b === 0) return true
    if (b < 9 || (b > 13 && b < 32)) ctrl++
  }
  return ctrl / n > 0.3
}

/**
 * 编码探测 + 解码：
 * BOM → UTF-8 严格校验（fatal）→ GB18030 回退（中文 Windows 老文件）→ UTF-16 启发。
 * 返回文本与编码标签（前端状态栏展示、保存时按同编码回写）。
 */
export function decodeBuffer(buf: Uint8Array): { text: string; encoding: string; eol: "lf" | "crlf" | "cr" | "mixed" } {
  const bom = bomOf(buf)
  let text: string
  let encoding: string
  if (bom) {
    text = new TextDecoder(bom.encoding).decode(buf.subarray(bom.bomLen))
    encoding = bom.encoding
  } else {
    try {
      text = new TextDecoder("utf-8", { fatal: true }).decode(buf)
      encoding = "utf-8"
    } catch {
      // UTF-16 启发：无 BOM 但奇偶位大量 NUL
      const n = Math.min(buf.length, 4096)
      let evenNul = 0
      let oddNul = 0
      for (let i = 0; i + 1 < n; i += 2) {
        if (buf[i] === 0) evenNul++
        if (buf[i + 1] === 0) oddNul++
      }
      if (oddNul > n / 8 && evenNul === 0) {
        text = new TextDecoder("utf-16le").decode(buf)
        encoding = "utf-16le"
      } else if (evenNul > n / 8 && oddNul === 0) {
        text = new TextDecoder("utf-16be").decode(buf)
        encoding = "utf-16be"
      } else {
        // GB18030 是 GBK/GB2312 的超集，解码覆盖面最广
        text = new TextDecoder("gb18030").decode(buf)
        encoding = "gbk"
      }
    }
  }
  return { text, encoding, eol: detectEol(text) }
}

function detectEol(text: string): "lf" | "crlf" | "cr" | "mixed" {
  let crlf = 0
  let lf = 0
  let cr = 0
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i)
    if (c === 13) {
      if (text.charCodeAt(i + 1) === 10) {
        crlf++
        i++
      } else cr++
    } else if (c === 10) lf++
  }
  const kinds = [crlf > 0, lf > 0, cr > 0].filter(Boolean).length
  if (kinds > 1) return "mixed"
  if (crlf) return "crlf"
  if (cr) return "cr"
  return "lf"
}

/**
 * 文本读取（编码/行尾/etag 一并返回）。truncated 时只返回前 maxBytes 字节（前端提示只读片段）。
 * 二进制文件不按文本返回（binary: true，前端走 hex / 下载）。
 */
export async function readTextFile(abs: string, opts: { maxBytes?: number; forceText?: boolean } = {}): Promise<TextReadResult> {
  const max = opts.maxBytes && opts.maxBytes > 0 ? opts.maxBytes : DEFAULT_MAX_TEXT_READ
  let st
  try {
    st = statSync(abs)
  } catch {
    throw fsNotFound(`文件不存在: ${abs}`)
  }
  if (st.isDirectory()) throw fsBadRequest("目标是目录")
  const etag = etagOf(abs, st.size, st.mtimeMs)
  const name = abs.replace(/\\/g, "/").split("/").pop() ?? abs
  const kind = kindForPath(name)
  const language = languageForPath(name)
  if (st.size === 0) return { content: "", encoding: "utf-8", eol: "lf", size: 0, mtime: st.mtimeMs, etag, truncated: false, binary: false, kind, language }
  const readBytes = Math.min(st.size, max)
  const handle = await readFile(abs)
  const buf = readBytes >= handle.length ? handle : handle.subarray(0, readBytes)
  const binary = !opts.forceText && looksBinary(buf)
  if (binary) {
    return { content: "", encoding: "binary", eol: "lf", size: st.size, mtime: st.mtimeMs, etag, truncated: st.size > readBytes, binary: true, kind, language }
  }
  const decoded = decodeBuffer(buf)
  return {
    content: decoded.text,
    encoding: decoded.encoding,
    eol: decoded.eol,
    size: st.size,
    mtime: st.mtimeMs,
    etag,
    truncated: st.size > readBytes,
    binary: false,
    kind,
    language,
  }
}

/* ---------------- HTTP Range（媒体/PDF/大文件流） ---------------- */

export interface ResolvedRange {
  start: number
  end: number
  /** 是否 206 部分响应 */
  partial: boolean
}

/**
 * 解析 `Range: bytes=` 头（单区间；多区间取首个——浏览器播放器与 PDF.js 只用单区间）。
 * 非法/不可满足返回 null 表示「按完整响应处理」（RFC 7233 允许忽略不可解析的 Range）。
 */
export function parseRange(header: string | null | undefined, size: number): ResolvedRange | "unsatisfiable" | null {
  if (!header) return null
  const m = /^\s*bytes\s*=\s*(.+)$/i.exec(header)
  if (!m) return null
  const firstSpec = m[1].split(",")[0].trim()
  const dm = /^(\d*)-(\d*)$/.exec(firstSpec)
  if (!dm) return null
  const [, rawStart, rawEnd] = dm
  if (rawStart === "" && rawEnd === "") return null
  let start: number
  let end: number
  if (rawStart === "") {
    const suffix = Number(rawEnd)
    if (!Number.isFinite(suffix) || suffix <= 0) return null
    start = Math.max(0, size - suffix)
    end = size - 1
  } else {
    start = Number(rawStart)
    end = rawEnd === "" ? size - 1 : Number(rawEnd)
  }
  if (!Number.isFinite(start) || !Number.isFinite(end)) return null
  if (start >= size) return "unsatisfiable"
  if (end >= size) end = size - 1
  if (end < start) return null
  return { start, end, partial: !(start === 0 && end === size - 1) }
}

/** 二进制/原样文件响应（含 Range、ETag 条件请求、Content-Disposition 可选）。 */
export async function rawResponse(
  abs: string,
  opts: { rangeHeader?: string | null; ifNoneMatch?: string | null; download?: boolean; name?: string; forceMime?: string } = {},
): Promise<Response> {
  let st
  try {
    st = statSync(abs)
  } catch {
    throw fsNotFound(`文件不存在: ${abs}`)
  }
  if (st.isDirectory()) throw fsBadRequest("目标是目录（目录请用打包下载）")
  const name = opts.name ?? abs.replace(/\\/g, "/").split("/").pop() ?? "file"
  const etag = etagOf(abs, st.size, st.mtimeMs)
  if (opts.ifNoneMatch && opts.ifNoneMatch === etag) {
    return new Response(null, { status: 304, headers: { ETag: etag } })
  }
  const mime = opts.forceMime ?? mimeForPath(name)
  const base: Record<string, string> = {
    "Content-Type": mime,
    ETag: etag,
    "Last-Modified": new Date(st.mtimeMs).toUTCString(),
    "Accept-Ranges": "bytes",
    "Cache-Control": "no-cache",
  }
  if (opts.download) base["Content-Disposition"] = `attachment; filename*=UTF-8''${encodeURIComponent(name)}`
  const parsed = parseRange(opts.rangeHeader, st.size)
  if (parsed === "unsatisfiable") {
    return new Response(null, { status: 416, headers: { ...base, "Content-Range": `bytes */${st.size}` } })
  }
  const file = Bun.file(abs)
  if (!parsed) {
    return new Response(file, { headers: { ...base, "Content-Length": String(st.size) } })
  }
  const slice = file.slice(parsed.start, parsed.end + 1)
  return new Response(slice, {
    status: 206,
    headers: { ...base, "Content-Range": `bytes ${parsed.start}-${parsed.end}/${st.size}`, "Content-Length": String(parsed.end - parsed.start + 1) },
  })
}

/* ---------------- 递归收集与打包 ---------------- */

export interface CollectedFile {
  /** 归档内相对路径（POSIX） */
  name: string
  abs: string
  size: number
}

/** 递归收集文件（跳过重目录；深度与总量上限保护）。 */
export function collectFiles(rootAbs: string, absPath: string, opts: { maxFiles?: number; maxBytes?: number; skipDirs?: boolean; depth?: number } = {}): { files: CollectedFile[]; skipped: number; totalBytes: number } {
  const maxFiles = opts.maxFiles ?? 20000
  const maxBytes = opts.maxBytes ?? 500 * 1024 * 1024
  const maxDepth = opts.depth ?? 20
  const out: CollectedFile[] = []
  let totalBytes = 0
  let skipped = 0
  const walk = (dir: string, depth: number): void => {
    if (out.length >= maxFiles || totalBytes >= maxBytes || depth > maxDepth) return
    let entries
    try {
      entries = readdirSync(dir, { withFileTypes: true })
    } catch {
      skipped++
      return
    }
    for (const e of entries) {
      if (out.length >= maxFiles || totalBytes >= maxBytes) return
      const full = join(dir, e.name)
      if (e.isDirectory()) {
        if (opts.skipDirs !== false && WALK_SKIP_DIRS.has(e.name)) {
          skipped++
          continue
        }
        walk(full, depth + 1)
      } else if (e.isFile()) {
        let size = 0
        try {
          size = statSync(full).size
        } catch {
          skipped++
          continue
        }
        if (totalBytes + size > maxBytes) {
          skipped++
          continue
        }
        totalBytes += size
        out.push({ name: relOf(rootAbs, full), abs: full, size })
      } else {
        skipped++
      }
    }
  }
  let st
  try {
    st = statSync(absPath)
  } catch {
    return { files: out, skipped, totalBytes }
  }
  if (st.isFile()) {
    out.push({ name: absPath.replace(/\\/g, "/").split("/").pop() ?? "file", abs: absPath, size: st.size })
    return { files: out, skipped, totalBytes: st.size }
  }
  walk(absPath, 0)
  return { files: out, skipped, totalBytes }
}

/** 打包 zip（复用零依赖 buildZip；超限抛 413 由路由层转用户可读提示）。 */
export async function zipPaths(rootAbs: string, paths: string[], maxBytes: number): Promise<Uint8Array> {
  const files: Array<{ name: string; data: Uint8Array }> = []
  let total = 0
  for (const p of paths) {
    const abs = resolveInRoot(rootAbs, p, { allowAbsolute: true })
    if (!existsSync(abs)) throw fsNotFound(`文件不存在: ${p}`)
    const collected = collectFiles(rootAbs, abs, { maxBytes: Math.max(0, maxBytes - total) })
    for (const f of collected.files) {
      total += f.size
      if (total > maxBytes) throw fsTooLarge(`打包内容超过上限（${Math.round(maxBytes / 1024 / 1024)}MB），请分批下载`)
      const buf = await Bun.file(f.abs).arrayBuffer()
      files.push({ name: f.name || "file", data: new Uint8Array(buf) })
    }
  }
  if (!files.length) throw fsNotFound("没有可打包的文件")
  return buildZip(files)
}

/* ---------------- 内容搜索 ---------------- */

export interface SearchHit {
  path: string
  line: number
  column: number
  lineText: string
  /** 文件级命中（name 模式） */
  fileOnly?: boolean
  size: number
  mtime: number
}

export interface SearchResult {
  hits: SearchHit[]
  truncated: boolean
  engine: "ripgrep" | "builtin"
}

/** ripgrep 可用性（首次探测后缓存）。 */
let rgAvailable: boolean | null = null
function hasRipgrep(): boolean {
  if (rgAvailable !== null) return rgAvailable
  try {
    const p = Bun.which("rg")
    rgAvailable = !!p
  } catch {
    rgAvailable = false
  }
  return rgAvailable
}

/** 简易 glob → 正则（`*` 单层、`**` 跨层、`?` 单字符、`{a,b}` 交替）。 */
export function globToRegExp(glob: string): RegExp {
  let re = ""
  for (let i = 0; i < glob.length; i++) {
    const c = glob[i]
    if (c === "*") {
      if (glob[i + 1] === "*") {
        // `**/` 或 `**`
        if (glob[i + 2] === "/") {
          re += "(?:.*/)?"
          i += 2
        } else {
          re += ".*"
          i += 1
        }
      } else re += "[^/]*"
    } else if (c === "?") re += "[^/]"
    else if (c === "{") {
      const close = glob.indexOf("}", i)
      if (close > i) {
        re += `(?:${glob
          .slice(i + 1, close)
          .split(",")
          .map((s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
          .join("|")})`
        i = close
      } else re += "\\{"
    } else re += c.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
  }
  return new RegExp(`^${re}$`, isWindowsFs() ? "i" : "")
}

function escapeRegExp(s: string): string {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
}

/**
 * 搜索：name 模式按文件名匹配（glob 支持）；content 模式按内容匹配。
 * content 优先 ripgrep（快、尊重 .gitignore）；缺失回退内置遍历（跳过重目录，逐行正则）。
 */
export async function searchInRoot(
  rootAbs: string,
  opts: { query: string; mode?: "name" | "content"; glob?: string; ignoreCase?: boolean; maxResults?: number; regex?: boolean; maxFileSize?: number },
): Promise<SearchResult> {
  const query = String(opts.query ?? "")
  if (!query.trim()) throw fsBadRequest("搜索关键字不能为空")
  const mode = opts.mode ?? "content"
  const max = opts.maxResults && opts.maxResults > 0 ? Math.min(opts.maxResults, 2000) : 500
  const ignoreCase = opts.ignoreCase !== false
  if (mode === "name") {
    const re = new RegExp(opts.regex ? query : escapeRegExp(query), ignoreCase ? "i" : "")
    const globRe = opts.glob ? globToRegExp(opts.glob) : null
    const hits: SearchHit[] = []
    let truncated = false
    const walk = (dir: string, depth: number): void => {
      if (hits.length >= max || depth > 12) {
        if (hits.length >= max) truncated = true
        return
      }
      let entries
      try {
        entries = readdirSync(dir, { withFileTypes: true })
      } catch {
        return
      }
      for (const e of entries) {
        if (hits.length >= max) {
          truncated = true
          return
        }
        const rel = relOf(rootAbs, join(dir, e.name))
        if (e.isDirectory()) {
          if (WALK_SKIP_DIRS.has(e.name)) continue
          walk(join(dir, e.name), depth + 1)
        } else if (e.isFile()) {
          if (globRe && !globRe.test(rel)) continue
          if (!re.test(e.name)) continue
          let size = 0
          let mtime = 0
          try {
            const s = statSync(join(dir, e.name))
            size = s.size
            mtime = s.mtimeMs
          } catch {
            continue
          }
          hits.push({ path: rel, line: 0, column: 0, lineText: "", fileOnly: true, size, mtime })
        }
      }
    }
    walk(rootAbs, 0)
    return { hits, truncated, engine: "builtin" }
  }

  const maxFileSize = opts.maxFileSize ?? 2 * 1024 * 1024
  if (hasRipgrep()) {
    const args = ["--json", "--line-number", "--column", "--no-heading", "--max-filesize", String(maxFileSize), "--max-count", "50", "-m", "50"]
    if (ignoreCase) args.push("-i")
    if (!opts.regex) args.push("-F")
    if (opts.glob) args.push("-g", opts.glob)
    for (const d of WALK_SKIP_DIRS) args.push("-g", `!${d}`)
    args.push("-e", query, ".")
    const proc = Bun.spawnSync(["rg", ...args], { cwd: rootAbs, stdout: "pipe", stderr: "ignore" })
    const text = new TextDecoder().decode(proc.stdout ?? new Uint8Array())
    const hits: SearchHit[] = []
    let truncated = false
    for (const line of text.split("\n")) {
      if (!line.trim()) continue
      if (hits.length >= max) {
        truncated = true
        break
      }
      let obj: { type?: string; data?: Record<string, unknown> }
      try {
        obj = JSON.parse(line)
      } catch {
        continue
      }
      if (obj.type !== "match" || !obj.data) continue
      const data = obj.data as {
        path?: { text?: string }
        lines?: { text?: string }
        line_number?: number
        submatches?: Array<{ start?: number }>
      }
      // rg 在 Windows 上以本地分隔符输出路径（`.\b.md`）：先归一为 POSIX 分隔符再剥 `./` 前缀，
      // 否则剥离失败会留下 `./b.md`（前端拿去解析与内置回退引擎的 `b.md` 形态不一致）
      const p = (data.path?.text ?? "").replace(/\\/g, "/").replace(/^\.\//, "")
      if (!p) continue
      let size = 0
      let mtime = 0
      try {
        const s = statSync(join(rootAbs, p))
        size = s.size
        mtime = s.mtimeMs
      } catch {
        /* 竞态忽略 */
      }
      hits.push({
        path: p,
        line: data.line_number ?? 0,
        column: (data.submatches?.[0]?.start ?? 0) + 1,
        lineText: (data.lines?.text ?? "").replace(/\r?\n$/, "").slice(0, 400),
        size,
        mtime,
      })
    }
    return { hits, truncated, engine: "ripgrep" }
  }

  // 内置回退：逐文件逐行正则
  const flags = (ignoreCase ? "i" : "") + "g"
  let re: RegExp
  try {
    re = new RegExp(opts.regex ? query : escapeRegExp(query), flags)
  } catch {
    throw fsBadRequest(`非法正则: ${query}`)
  }
  const globRe = opts.glob ? globToRegExp(opts.glob) : null
  const hits: SearchHit[] = []
  let truncated = false
  const walk = async (dir: string, depth: number): Promise<void> => {
    if (hits.length >= max || depth > 12) {
      if (hits.length >= max) truncated = true
      return
    }
    let entries
    try {
      entries = readdirSync(dir, { withFileTypes: true })
    } catch {
      return
    }
    for (const e of entries) {
      if (hits.length >= max) {
        truncated = true
        return
      }
      const full = join(dir, e.name)
      if (e.isDirectory()) {
        if (WALK_SKIP_DIRS.has(e.name)) continue
        await walk(full, depth + 1)
      } else if (e.isFile()) {
        const rel = relOf(rootAbs, full)
        if (globRe && !globRe.test(rel)) continue
        let s
        try {
          s = statSync(full)
        } catch {
          continue
        }
        if (s.size > maxFileSize) continue
        const bytes = new Uint8Array(await Bun.file(full).arrayBuffer())
        if (looksBinary(bytes)) continue
        const { text } = decodeBuffer(bytes)
        const lines = text.split(/\r\n|\r|\n/)
        for (let i = 0; i < lines.length; i++) {
          re.lastIndex = 0
          const m = re.exec(lines[i])
          if (!m) continue
          hits.push({ path: rel, line: i + 1, column: m.index + 1, lineText: lines[i].slice(0, 400), size: s.size, mtime: s.mtimeMs })
          if (hits.length >= max) {
            truncated = true
            break
          }
        }
      }
    }
  }
  await walk(rootAbs, 0)
  return { hits, truncated, engine: "builtin" }
}

/** root 内某路径是否位于给定目录（前端「在树中定位」用；两参数均为绝对路径）。 */
export function containsPath(dir: string, target: string): boolean {
  return isInside(dir, target)
}

/** 供审计与展示：绝对路径转 root 内逻辑路径（越界返回绝对路径本身）。 */
export function displayPath(rootAbs: string, abs: string): string {
  return isInside(rootAbs, abs) ? relOf(rootAbs, abs) : abs
}

/** 归一化用户传入的排序键。 */
export function asSortKey(v: unknown): SortKey {
  return v === "mtime" || v === "size" || v === "type" ? v : "name"
}

/** 路径是否为绝对（Windows 形态在前端也需识别，统一走本函数）。 */
export function looksAbsolute(p: string): boolean {
  const s = String(p ?? "").replace(/\\/g, "/")
  return isAbsolute(s) || /^[a-zA-Z]:\//.test(s)
}

/** 拼接 root 内相对路径（供「新建文件」预填与父子导航）。 */
export function joinRel(...parts: string[]): string {
  return parts
    .map((p) => String(p ?? "").replace(/\\/g, "/").replace(/^\/+|\/+$/g, ""))
    .filter(Boolean)
    .join("/")
}

/** 解析 `stat` 结果中的 sep（仅供测试断言用）。 */
export const PATH_SEP = sep

/** 触发一次真实的路径存在性检查（供路由层给出一致的 404 语义）。 */
export async function requireFile(abs: string): Promise<void> {
  const st = await stat(abs).catch(() => null)
  if (!st || !st.isFile()) throw fsNotFound(`文件不存在: ${abs}`)
}

/** 未使用占位：保持 resolve 导入（部分分支路径需要绝对化）。 */
void resolve
