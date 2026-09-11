/** 文件工作台 · 写操作（DESIGN「文件工作台」P2）：保存 / 新建 / 改名 / 移动 / 复制 / 删除（软删除回收站）/ 上传。
 *
 *  安全与一致性要点：
 *  - **原子写**：同目录临时文件 + rename（断电/中断不会留下半截文件），保留原文件权限位；
 *  - **乐观锁**：`expectedEtag` 与服务端当前 etag 不符即 409（Agent 或他人同时改了同一文件——歌白特有场景），
 *    响应体回带磁盘当前内容供前端「对比合并」；
 *  - **编码回写**：UTF-8 / UTF-8-BOM / UTF-16LE/BE 用内建 TextEncoder 处理；GBK 系列走 iconv-lite
 *    （中文 Windows 老文件必须能保存，否则「能看不能存」等于没有编辑能力）；
 *  - **软删除**：默认移入 `{GEBAI_HOME}/users/{user}/.gebai-trash/<批次>/` + 清单（可恢复），
 *    物理删除需显式 `hard: true`（破坏性操作前端二次确认）。
 */
import { randomBytes } from "node:crypto"
import { chmodSync, existsSync, mkdirSync, readdirSync, statSync } from "node:fs"
import { cp, mkdir, readFile, rename, rm, writeFile } from "node:fs/promises"
import { dirname, join, resolve } from "node:path"
import iconv from "iconv-lite"
import { fsBadRequest, fsConflict, fsNotFound, fsTooLarge, resolveInRoot, type RootContext } from "./roots"
import { decodeBuffer, etagOf, looksBinary, readTextFile } from "./service"

/** 支持的编码（前端状态栏可选项）。 */
export const SUPPORTED_ENCODINGS = ["utf-8", "utf-8-bom", "utf-16le", "utf-16be", "gbk", "gb18030", "big5", "latin1"] as const
export type SupportedEncoding = (typeof SUPPORTED_ENCODINGS)[number]

export function normalizeEncoding(v: unknown): SupportedEncoding {
  const s = String(v ?? "utf-8").toLowerCase()
  if (s === "utf8") return "utf-8"
  if (s === "utf8bom" || s === "utf-8 bom") return "utf-8-bom"
  if (s === "gb2312" || s === "gb-2312" || s === "cp936") return "gbk"
  return (SUPPORTED_ENCODINGS as readonly string[]).includes(s) ? (s as SupportedEncoding) : "utf-8"
}

/** 文本 → 字节（按所选编码；BOM 按需写入）。 */
export function encodeText(content: string, encoding: SupportedEncoding): Uint8Array {
  if (encoding === "utf-8") return new TextEncoder().encode(content)
  if (encoding === "utf-8-bom") {
    const body = new TextEncoder().encode(content)
    const out = new Uint8Array(body.length + 3)
    out.set([0xef, 0xbb, 0xbf], 0)
    out.set(body, 3)
    return out
  }
  if (encoding === "utf-16le") {
    const out = new Uint8Array(content.length * 2 + 2)
    out[0] = 0xff
    out[1] = 0xfe
    for (let i = 0; i < content.length; i++) {
      const c = content.charCodeAt(i)
      out[2 + i * 2] = c & 0xff
      out[3 + i * 2] = c >> 8
    }
    return out
  }
  if (encoding === "utf-16be") {
    const out = new Uint8Array(content.length * 2 + 2)
    out[0] = 0xfe
    out[1] = 0xff
    for (let i = 0; i < content.length; i++) {
      const c = content.charCodeAt(i)
      out[2 + i * 2] = c >> 8
      out[3 + i * 2] = c & 0xff
    }
    return out
  }
  const iconvName = encoding === "big5" ? "big5" : encoding === "latin1" ? "latin1" : encoding
  return new Uint8Array(iconv.encode(content, iconvName))
}

/** 行尾归一：按目标风格重写（`mixed` 视为不改变原有混排，仅整文件统一）。 */
export function applyEol(content: string, eol: "lf" | "crlf" | "cr" | "mixed", target: "lf" | "crlf" | "cr" | "keep"): string {
  if (target === "keep") return content
  const unified = content.replace(/\r\n/g, "\n").replace(/\r/g, "\n")
  if (target === "lf") return unified
  if (target === "crlf") return unified.replace(/\n/g, "\r\n")
  return unified.replace(/\n/g, "\r")
  void eol
}

export interface SaveResult {
  etag: string
  size: number
  mtime: number
  encoding: SupportedEncoding
  eol: string
}

export interface SaveOptions {
  content: string
  encoding?: unknown
  /** 目标行尾：keep 保持内容原样（默认） */
  eol?: "keep" | "lf" | "crlf" | "cr"
  expectedEtag?: string
  createDirs?: boolean
  maxBytes: number
}

/**
 * 保存文本（原子写 + 乐观锁 + 编码/行尾）。
 * `expectedEtag` 不符 → 409 且响应体携带磁盘当前文本（前端「覆盖 / 对比合并 / 放弃」三选）。
 */
export async function saveText(abs: string, opts: SaveOptions): Promise<SaveResult> {
  const encoding = normalizeEncoding(opts.encoding)
  const exists = existsSync(abs)
  let prevEtag = ""
  let prevMode = 0
  let prevStat: { size: number; mtimeMs: number } | null = null
  if (exists) {
    const st = statSync(abs)
    if (st.isDirectory()) throw fsBadRequest("目标是目录，不能写入")
    prevEtag = etagOf(abs, st.size, st.mtimeMs)
    prevMode = st.mode
    prevStat = { size: st.size, mtimeMs: st.mtimeMs }
  } else if (!opts.createDirs) {
    // 允许新建（前端「新建文件」直接保存）；createDirs=false 仅表示不额外建父目录
    const parent = dirname(abs)
    if (!existsSync(parent)) throw fsNotFound(`父目录不存在: ${parent}`)
  }
  if (opts.expectedEtag && prevEtag && opts.expectedEtag !== prevEtag) {
    const current = await readTextFile(abs, { maxBytes: 512 * 1024, forceText: true }).catch(() => null)
    throw Object.assign(
      new Error("文件已被外部修改（磁盘内容与编辑器基线不一致）"),
      { status: 409, code: "etag_mismatch", current: current ? { content: current.content, etag: current.etag, encoding: current.encoding, eol: current.eol } : null },
    )
  }
  const text = applyEol(opts.content, "mixed", opts.eol ?? "keep")
  const bytes = encodeText(text, encoding)
  if (bytes.length > opts.maxBytes) throw fsTooLarge(`内容超过写入上限（${Math.round(opts.maxBytes / 1024 / 1024)}MB）`)
  if (opts.createDirs && !existsSync(dirname(abs))) await mkdir(dirname(abs), { recursive: true })
  const tmp = `${abs}.gebai-tmp-${randomBytes(4).toString("hex")}`
  try {
    await writeFile(tmp, bytes)
    if (prevMode) {
      try {
        chmodSync(tmp, prevMode)
      } catch {
        /* Windows 上 chmod 语义有限：忽略 */
      }
    }
    await rename(tmp, abs)
  } catch (err) {
    await rm(tmp, { force: true }).catch(() => {})
    throw err
  }
  const st = statSync(abs)
  return { etag: etagOf(abs, st.size, st.mtimeMs), size: st.size, mtime: st.mtimeMs, encoding, eol: opts.eol ?? "keep" }
  void prevStat
}

/** 保存冲突错误判定（路由层据此回带磁盘内容）。 */
export function isSaveConflict(err: unknown): err is Error & { code: string; current: unknown } {
  return !!err && typeof err === "object" && (err as { code?: string }).code === "etag_mismatch"
}

/* ---------------- 目录与文件基本操作 ---------------- */

export async function createDirectory(abs: string, parents = true): Promise<void> {
  if (existsSync(abs)) {
    if (!statSync(abs).isDirectory()) throw fsConflict("同名文件已存在")
    return
  }
  await mkdir(abs, { recursive: parents })
}

export async function renameEntry(abs: string, newName: string): Promise<string> {
  if (!existsSync(abs)) throw fsNotFound("目标不存在")
  const parent = dirname(abs)
  const dest = join(parent, newName)
  if (resolve(dest) === resolve(abs)) return dest
  if (existsSync(dest)) throw fsConflict(`同名条目已存在: ${newName}`)
  await rename(abs, dest)
  return dest
}

export async function moveEntry(src: string, dest: string, opts: { overwrite?: boolean } = {}): Promise<void> {
  if (!existsSync(src)) throw fsNotFound("源目标不存在")
  if (existsSync(dest)) {
    if (!opts.overwrite) throw fsConflict("目标已存在（可勾选覆盖）")
    await rm(dest, { recursive: true, force: true })
  }
  await mkdir(dirname(dest), { recursive: true })
  try {
    await rename(src, dest)
  } catch {
    // 跨设备：退化复制 + 删除
    await cp(src, dest, { recursive: true, force: true })
    await rm(src, { recursive: true, force: true })
  }
}

export async function copyEntry(src: string, dest: string, opts: { overwrite?: boolean } = {}): Promise<void> {
  if (!existsSync(src)) throw fsNotFound("源目标不存在")
  if (resolve(src) === resolve(dest)) throw fsBadRequest("源与目标相同")
  if (existsSync(dest)) {
    if (!opts.overwrite) throw fsConflict("目标已存在（可勾选覆盖）")
    await rm(dest, { recursive: true, force: true })
  }
  await mkdir(dirname(dest), { recursive: true })
  await cp(src, dest, { recursive: true, force: true, errorOnExist: false })
}

/* ---------------- 软删除（回收站）与恢复 ---------------- */

export interface TrashManifest {
  createdAt: number
  user: string
  items: Array<{ root: string; path: string; trashName: string; type: "file" | "dir" }>
}

function trashBase(ctx: RootContext): string {
  return join(ctx.home, "users", ctx.user, ".gebai-trash")
}

/** 批量删除：默认软删除（回收站可恢复），`hard: true` 物理删除。 */
export async function deleteEntries(
  ctx: RootContext,
  items: Array<{ rootId: string; rootAbs: string; abs: string; rel: string }>,
  opts: { hard?: boolean } = {},
): Promise<{ trashed: number; deleted: number; batch?: string }> {
  if (!items.length) throw fsBadRequest("未指定要删除的条目")
  if (opts.hard) {
    for (const it of items) await rm(it.abs, { recursive: true, force: true })
    return { trashed: 0, deleted: items.length }
  }
  const batch = `${Date.now()}-${randomBytes(3).toString("hex")}`
  const dir = join(trashBase(ctx), batch)
  await mkdir(dir, { recursive: true })
  const manifest: TrashManifest = { createdAt: Date.now(), user: ctx.user, items: [] }
  let i = 0
  for (const it of items) {
    if (!existsSync(it.abs)) continue
    const isDir = statSync(it.abs).isDirectory()
    const trashName = `${String(i++).padStart(3, "0")}-${it.rel.split("/").pop() || (isDir ? "dir" : "file")}`
    await rename(it.abs, join(dir, trashName)).catch(async () => {
      await cp(it.abs, join(dir, trashName), { recursive: true, force: true })
      await rm(it.abs, { recursive: true, force: true })
    })
    manifest.items.push({ root: it.rootId, path: it.rel, trashName, type: isDir ? "dir" : "file" })
  }
  if (!manifest.items.length) {
    await rm(dir, { recursive: true, force: true })
    throw fsNotFound("没有可删除的条目")
  }
  await writeFile(join(dir, "manifest.json"), JSON.stringify(manifest, null, 2), "utf8")
  return { trashed: manifest.items.length, deleted: 0, batch }
}

export interface TrashEntry {
  batch: string
  createdAt: number
  items: Array<{ root: string; path: string; type: "file" | "dir"; size: number }>
}

/** 回收站清单（按时间倒序）。 */
export async function listTrash(ctx: RootContext, limit = 100): Promise<TrashEntry[]> {
  const base = trashBase(ctx)
  if (!existsSync(base)) return []
  const out: TrashEntry[] = []
  for (const name of readdirSync(base)) {
    const dir = join(base, name)
    let stat
    try {
      stat = statSync(dir)
    } catch {
      continue
    }
    if (!stat.isDirectory()) continue
    let manifest: TrashManifest
    try {
      manifest = JSON.parse(await readFile(join(dir, "manifest.json"), "utf8")) as TrashManifest
    } catch {
      continue
    }
    out.push({
      batch: name,
      createdAt: manifest.createdAt ?? stat.mtimeMs,
      items: (manifest.items ?? []).map((it) => {
        let size = 0
        try {
          const s = statSync(join(dir, it.trashName))
          size = s.isDirectory() ? 0 : s.size
        } catch {
          /* 条目已丢失 */
        }
        return { root: it.root, path: it.path, type: it.type, size }
      }),
    })
  }
  out.sort((a, b) => b.createdAt - a.createdAt)
  return out.slice(0, limit)
}

/** 从回收站恢复（目标已存在且未 overwrite → 409）。 */
export async function restoreTrash(
  ctx: RootContext,
  batch: string,
  opts: { overwrite?: boolean; resolveRootAbs: (rootId: string) => string } = { resolveRootAbs: () => "" },
): Promise<{ restored: number; skipped: string[] }> {
  const dir = join(trashBase(ctx), batch)
  if (!existsSync(dir)) throw fsNotFound(`回收站批次不存在: ${batch}`)
  let manifest: TrashManifest
  try {
    manifest = JSON.parse(await readFile(join(dir, "manifest.json"), "utf8")) as TrashManifest
  } catch {
    throw fsNotFound("回收站清单损坏")
  }
  let restored = 0
  const skipped: string[] = []
  for (const it of manifest.items ?? []) {
    const from = join(dir, it.trashName)
    if (!existsSync(from)) {
      skipped.push(it.path)
      continue
    }
    let dest: string
    try {
      const rootAbs = opts.resolveRootAbs(it.root)
      dest = resolveInRoot(rootAbs, it.path)
    } catch {
      skipped.push(it.path)
      continue
    }
    if (existsSync(dest) && !opts.overwrite) {
      skipped.push(it.path)
      continue
    }
    if (existsSync(dest)) await rm(dest, { recursive: true, force: true })
    await mkdir(dirname(dest), { recursive: true })
    await rename(from, dest).catch(async () => {
      await cp(from, dest, { recursive: true, force: true })
    })
    restored++
  }
  await rm(dir, { recursive: true, force: true })
  return { restored, skipped }
}

/** 彻底清空回收站（或指定批次）。 */
export async function purgeTrash(ctx: RootContext, batch?: string): Promise<number> {
  const base = trashBase(ctx)
  if (!existsSync(base)) return 0
  const targets = batch ? [join(base, batch)] : readdirSync(base).map((n) => join(base, n))
  let n = 0
  for (const t of targets) {
    if (!existsSync(t)) continue
    await rm(t, { recursive: true, force: true })
    n++
  }
  return n
}

/* ---------------- 上传 ---------------- */

export interface UploadItem {
  /** 目标相对路径（含文件名；可带子目录 → 自动建目录） */
  path: string
  data: Uint8Array
}

/** 上传落盘（路径穿越与保留名已由 resolveInRoot 校验；覆盖策略由前端确认后传入 overwrite）。 */
export async function uploadFiles(rootAbs: string, items: UploadItem[], opts: { overwrite?: boolean; maxBytes: number }): Promise<{ saved: Array<{ path: string; size: number }>; skipped: string[] }> {
  const saved: Array<{ path: string; size: number }> = []
  const skipped: string[] = []
  for (const it of items) {
    if (it.data.length > opts.maxBytes) {
      skipped.push(`${it.path}（超过单文件上限）`)
      continue
    }
    const abs = resolveInRoot(rootAbs, it.path)
    if (existsSync(abs) && !opts.overwrite) {
      skipped.push(it.path)
      continue
    }
    await mkdir(dirname(abs), { recursive: true })
    await writeFile(abs, it.data)
    const st = statSync(abs)
    saved.push({ path: it.path, size: st.size })
  }
  return { saved, skipped }
}

/** 二进制内容是否可安全按文本处理（上传/读取前的兜底判定）。 */
export function isBinaryContent(data: Uint8Array): boolean {
  return looksBinary(data)
}

/** 解码字节为文本（上传的文本预览/导入用）。 */
export function bytesToText(data: Uint8Array): { text: string; encoding: string; eol: string } {
  const d = decodeBuffer(data)
  return { text: d.text, encoding: d.encoding, eol: d.eol }
}

/** 创建父目录（内部用）。 */
export async function ensureParent(p: string): Promise<void> {
  const parent = dirname(p)
  if (!existsSync(parent)) mkdirSync(parent, { recursive: true })
}
