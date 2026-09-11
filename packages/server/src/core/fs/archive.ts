/** 文件工作台 · ZIP 读取（不解压落盘即可浏览条目）：EOCD + 中央目录解析，stored/deflate 两种方法。
 *  与写侧 `server/src/zip.ts`（buildZip）配套——读用中央目录、写用本地头，两侧都不引入依赖。
 *  `.tar/.gz/.7z/.rar` 不在本模块范围（P5 可扩展；前端对这类扩展名只提供下载）。
 */
import { inflateRawSync } from "node:zlib"
import { fsBadRequest, fsNotFound } from "./roots"

export interface ZipEntry {
  name: string
  size: number
  compressedSize: number
  method: number
  crc32: number
  isDir: boolean
  mtime: number
  /** 本地头偏移（读取条目内容用） */
  offset: number
}

/** 找出 EOCD（End of Central Directory）：从尾部回扫签名 0x06054b50（注释最长 64KB）。 */
function findEocd(buf: Uint8Array): number {
  const min = Math.max(0, buf.length - 22 - 0xffff)
  for (let i = buf.length - 22; i >= min; i--) {
    if (buf[i] === 0x50 && buf[i + 1] === 0x4b && buf[i + 2] === 0x05 && buf[i + 3] === 0x06) return i
  }
  return -1
}

function dosToMs(date: number, time: number): number {
  const y = 1980 + ((date >> 9) & 0x7f)
  const mo = ((date >> 5) & 0x0f) - 1
  const d = date & 0x1f
  const h = (time >> 11) & 0x1f
  const mi = (time >> 5) & 0x3f
  const s = (time & 0x1f) * 2
  return Date.UTC(y, Math.max(0, mo), Math.max(1, d), h, mi, s)
}

/** 解析 ZIP 中央目录（ZIP64 的条目用扩展字段可忽略；超大归档只列条目不解压）。 */
export function listZip(buf: Uint8Array): ZipEntry[] {
  const eocd = findEocd(buf)
  if (eocd < 0) throw fsBadRequest("不是有效的 ZIP 归档（未找到中央目录）")
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength)
  const total = dv.getUint16(eocd + 10, true)
  let off = dv.getUint32(eocd + 16, true)
  const out: ZipEntry[] = []
  for (let i = 0; i < total; i++) {
    if (off + 46 > buf.length) break
    if (dv.getUint32(off, true) !== 0x02014b50) break
    const method = dv.getUint16(off + 10, true)
    const time = dv.getUint16(off + 12, true)
    const date = dv.getUint16(off + 14, true)
    const crc = dv.getUint32(off + 16, true)
    const compressedSize = dv.getUint32(off + 20, true)
    const size = dv.getUint32(off + 24, true)
    const nameLen = dv.getUint16(off + 28, true)
    const extraLen = dv.getUint16(off + 30, true)
    const commentLen = dv.getUint16(off + 32, true)
    const localOffset = dv.getUint32(off + 42, true)
    const name = new TextDecoder("utf-8").decode(buf.subarray(off + 46, off + 46 + nameLen))
    out.push({
      name,
      size,
      compressedSize,
      method,
      crc32: crc,
      isDir: name.endsWith("/") || (name.endsWith("\\") && size === 0),
      mtime: dosToMs(date, time),
      offset: localOffset,
    })
    off += 46 + nameLen + extraLen + commentLen
  }
  return out
}

/** 读取单个条目内容（stored / deflate；加密条目抛 422）。 */
export function readZipEntry(buf: Uint8Array, entry: ZipEntry): Uint8Array {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength)
  const off = entry.offset
  if (off + 30 > buf.length || dv.getUint32(off, true) !== 0x04034b50) throw fsNotFound(`条目数据损坏: ${entry.name}`)
  const flags = dv.getUint16(off + 6, true)
  if (flags & 0x1) throw fsBadRequest("加密 ZIP 条目暂不支持读取")
  const nameLen = dv.getUint16(off + 26, true)
  const extraLen = dv.getUint16(off + 28, true)
  const start = off + 30 + nameLen + extraLen
  const raw = buf.subarray(start, start + entry.compressedSize)
  if (entry.method === 0) return raw
  if (entry.method === 8) {
    const out = inflateRawSync(raw)
    return new Uint8Array(out.buffer, out.byteOffset, out.byteLength)
  }
  throw fsBadRequest(`不支持的压缩方法: ${entry.method}（仅 stored/deflate）`)
}

/** 安全解压：拒绝条目名穿越（`../`、绝对路径）——写盘前必须过此校验。 */
export function safeEntryName(name: string): string {
  const norm = name.replace(/\\/g, "/")
  if (norm.startsWith("/") || /^[a-zA-Z]:/.test(norm)) throw fsBadRequest(`归档条目为绝对路径，拒绝解压: ${name}`)
  const segs = norm.split("/").filter(Boolean)
  for (const s of segs) {
    if (s === "..") throw fsBadRequest(`归档条目含路径穿越，拒绝解压: ${name}`)
    if (/[\x00-\x1f]/.test(s)) throw fsBadRequest(`归档条目名含控制字符: ${name}`)
  }
  return segs.join("/")
}
