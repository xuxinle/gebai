/**
 * 极简 protobuf wire 编解码（飞书长连接 pbbp2.Frame 专用）。
 * 仅实现所需类型：varint（int32/uint64/枚举）与 length-delimited（string/bytes/嵌套 message）。
 * 参照 lark_oapi 官方 SDK 的 pbbp2.proto（proto2）：
 *
 *   message Header { required string key = 1; required string value = 2; }
 *   message Frame {
 *     required uint64 SeqID = 1;
 *     required uint64 LogID = 2;
 *     required int32  service = 3;
 *     required int32  method = 4;
 *     repeated Header headers = 5;
 *     optional string payload_encoding = 6;
 *     optional string payload_type = 7;
 *     optional bytes  payload = 8;
 *     optional string LogIDNew = 9;
 *   }
 *
 * 注意：uint64 以 JS number 承载，超出 Number.MAX_SAFE_INTEGER 时精度截断
 * （SeqID/LogID 实际为服务端下发的序号/时间戳，均在安全范围内）。
 */

export interface PbHeader {
  key: string
  value: string
}

export interface PbFrame {
  SeqID: number
  LogID: number
  service: number
  method: number
  headers: PbHeader[]
  payload_encoding?: string
  payload_type?: string
  /** bytes（无则为空）。 */
  payload?: Uint8Array
  LogIDNew?: string
}

const WIRE_VARINT = 0
const WIRE_LEN = 2

/** 将无符号整数编码为 protobuf varint（支持到 2^53）。 */
export function encodeVarint(value: number): number[] {
  const out: number[] = []
  let v = Math.max(0, Math.floor(value))
  do {
    let b = v & 0x7f
    v = Math.floor(v / 128)
    if (v > 0) b |= 0x80
    out.push(b)
  } while (v > 0)
  return out
}

/** 从 offset 处解码 varint；返回 { value, size }。 */
export function decodeVarint(buf: Uint8Array, offset: number): { value: number; size: number } {
  let value = 0
  let shift = 0
  for (let i = offset; i < buf.length; i++) {
    const b = buf[i]
    value += (b & 0x7f) * 2 ** shift
    if (!(b & 0x80)) return { value, size: i - offset + 1 }
    shift += 7
    if (shift > 63) break
  }
  throw new Error("protobuf: varint 越界")
}

function encodeTag(field: number, wireType: number): number[] {
  return encodeVarint((field << 3) | wireType)
}

/** 编码 Header。 */
export function encodeHeader(h: PbHeader): Uint8Array {
  const keyBytes = new TextEncoder().encode(h.key)
  const valBytes = new TextEncoder().encode(h.value)
  const keyPart = [...encodeTag(1, WIRE_LEN), ...encodeVarint(keyBytes.length), ...keyBytes]
  const valPart = [...encodeTag(2, WIRE_LEN), ...encodeVarint(valBytes.length), ...valBytes]
  return Uint8Array.from([...keyPart, ...valPart])
}

function encodeLenField(field: number, bytes: Uint8Array): Uint8Array {
  return Uint8Array.from([...encodeTag(field, WIRE_LEN), ...encodeVarint(bytes.length), ...bytes])
}

/** 编码 Frame。 */
export function encodeFrame(f: PbFrame): Uint8Array {
  const parts: number[] = []
  parts.push(...encodeTag(1, WIRE_VARINT), ...encodeVarint(f.SeqID))
  parts.push(...encodeTag(2, WIRE_VARINT), ...encodeVarint(f.LogID))
  parts.push(...encodeTag(3, WIRE_VARINT), ...encodeVarint(f.service))
  parts.push(...encodeTag(4, WIRE_VARINT), ...encodeVarint(f.method))
  for (const h of f.headers) {
    const hb = encodeHeader(h)
    parts.push(...encodeTag(5, WIRE_LEN), ...encodeVarint(hb.length), ...hb)
  }
  const chunks: Uint8Array[] = [Uint8Array.from(parts)]
  if (f.payload_encoding !== undefined) chunks.push(encodeLenField(6, new TextEncoder().encode(f.payload_encoding)))
  if (f.payload_type !== undefined) chunks.push(encodeLenField(7, new TextEncoder().encode(f.payload_type)))
  if (f.payload && f.payload.length > 0) chunks.push(encodeLenField(8, f.payload))
  if (f.LogIDNew !== undefined) chunks.push(encodeLenField(9, new TextEncoder().encode(f.LogIDNew)))
  if (chunks.length === 1) return chunks[0]
  const total = chunks.reduce((n, c) => n + c.length, 0)
  const out = new Uint8Array(total)
  let off = 0
  for (const c of chunks) {
    out.set(c, off)
    off += c.length
  }
  return out
}

/** 解码 Frame（未知字段跳过；proto2 required 缺失时以 0/空兜底，容忍服务端变体）。 */
export function decodeFrame(buf: Uint8Array): PbFrame {
  const frame: PbFrame = { SeqID: 0, LogID: 0, service: 0, method: 0, headers: [] }
  let offset = 0
  while (offset < buf.length) {
    const { value: tag, size: tagSize } = decodeVarint(buf, offset)
    offset += tagSize
    const field = tag >>> 3
    const wireType = tag & 7
    if (wireType === WIRE_VARINT) {
      const { value, size } = decodeVarint(buf, offset)
      offset += size
      switch (field) {
        case 1: frame.SeqID = value; break
        case 2: frame.LogID = value; break
        case 3: frame.service = value; break
        case 4: frame.method = value; break
      }
    } else if (wireType === WIRE_LEN) {
      const { value: len, size } = decodeVarint(buf, offset)
      offset += size
      const end = offset + len
      if (end > buf.length) throw new Error("protobuf: length-delimited 越界")
      const slice = buf.slice(offset, end)
      offset = end
      switch (field) {
        case 5: {
          // 嵌套 Header：key=1, value=2（均 length-delimited）
          const h: PbHeader = { key: "", value: "" }
          let o = 0
          while (o < slice.length) {
            const t = decodeVarint(slice, o)
            o += t.size
            const f = t.value >>> 3
            const wt = t.value & 7
            if (wt === WIRE_LEN) {
              const l = decodeVarint(slice, o)
              o += l.size
              const v = new TextDecoder().decode(slice.slice(o, o + l.value))
              o += l.value
              if (f === 1) h.key = v
              else if (f === 2) h.value = v
            } else if (wt === WIRE_VARINT) {
              const v = decodeVarint(slice, o)
              o += v.size
            } else {
              break
            }
          }
          frame.headers.push(h)
          break
        }
        case 6: frame.payload_encoding = new TextDecoder().decode(slice); break
        case 7: frame.payload_type = new TextDecoder().decode(slice); break
        case 8: frame.payload = slice; break
        case 9: frame.LogIDNew = new TextDecoder().decode(slice); break
      }
    } else {
      // wire type 1/5（fixed32/64）未使用：按字节跳过
      const skip = wireType === 1 ? 8 : wireType === 5 ? 4 : 0
      if (!skip) throw new Error(`protobuf: 未知 wire type ${wireType}`)
      offset += skip
    }
  }
  return frame
}
