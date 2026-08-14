import { deflateRawSync } from "node:zlib"

/** CRC32（ZIP 校验用，查表法）。 */
const CRC_TABLE = (() => {
  const table = new Uint32Array(256)
  for (let n = 0; n < 256; n++) {
    let c = n
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
    table[n] = c >>> 0
  }
  return table
})()

export function crc32(buf: Uint8Array): number {
  let c = 0xffffffff
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8)
  return (c ^ 0xffffffff) >>> 0
}

function dosDateTime(date: Date): { time: number; date: number } {
  const time = (date.getHours() << 11) | (date.getMinutes() << 5) | (date.getSeconds() >> 1)
  const d = ((Math.max(1980, date.getFullYear()) - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate()
  return { time, date: d }
}

/**
 * 构建 ZIP 归档（deflate 压缩，UTF-8 文件名，含中文路径）。
 * 零依赖实现，供会话临时文件多选打包下载（POST /files/download）。
 */
export function buildZip(files: Array<{ name: string; data: Uint8Array }>): Uint8Array {
  const encoder = new TextEncoder()
  const chunks: Uint8Array[] = []
  const central: Uint8Array[] = []
  let offset = 0
  const now = dosDateTime(new Date())

  for (const f of files) {
    const nameBytes = encoder.encode(f.name)
    const raw = f.data
    const deflated = deflateRawSync(raw)
    const method = deflated.length < raw.length ? 8 : 0
    const data = method === 8 ? deflated : raw
    const crc = crc32(raw)

    const local = new DataView(new ArrayBuffer(30))
    local.setUint32(0, 0x04034b50, true) // local file header signature
    local.setUint16(4, 20, true) // version needed to extract
    local.setUint16(6, 0x0800, true) // general purpose bit flag: UTF-8 names
    local.setUint16(8, method, true)
    local.setUint16(10, now.time, true)
    local.setUint16(12, now.date, true)
    local.setUint32(14, crc, true)
    local.setUint32(18, data.length, true)
    local.setUint32(22, raw.length, true)
    local.setUint16(26, nameBytes.length, true)
    local.setUint16(28, 0, true) // extra length
    chunks.push(new Uint8Array(local.buffer), nameBytes, data)

    const cd = new DataView(new ArrayBuffer(46))
    cd.setUint32(0, 0x02014b50, true) // central directory header signature
    cd.setUint16(4, 20, true) // version made by
    cd.setUint16(6, 20, true) // version needed
    cd.setUint16(8, 0x0800, true) // UTF-8 names
    cd.setUint16(10, method, true)
    cd.setUint16(12, now.time, true)
    cd.setUint16(14, now.date, true)
    cd.setUint32(16, crc, true)
    cd.setUint32(20, data.length, true)
    cd.setUint32(24, raw.length, true)
    cd.setUint16(28, nameBytes.length, true)
    cd.setUint16(30, 0, true) // extra length
    cd.setUint16(32, 0, true) // comment length
    cd.setUint32(42, offset, true) // local header offset
    central.push(new Uint8Array(cd.buffer), nameBytes)

    offset += 30 + nameBytes.length + data.length
  }

  const cdStart = offset
  const cdSize = central.reduce((n, c) => n + c.length, 0)
  const eocd = new DataView(new ArrayBuffer(22))
  eocd.setUint32(0, 0x06054b50, true) // end of central directory signature
  eocd.setUint16(8, files.length, true)
  eocd.setUint16(10, files.length, true)
  eocd.setUint32(12, cdSize, true)
  eocd.setUint32(16, cdStart, true)

  const out = new Uint8Array(offset + cdSize + 22)
  let pos = 0
  for (const c of [...chunks, ...central, new Uint8Array(eocd.buffer)]) {
    out.set(c, pos)
    pos += c.length
  }
  return out
}
