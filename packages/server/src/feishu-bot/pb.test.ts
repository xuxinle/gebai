import { describe, expect, test } from "bun:test"
import { decodeFrame, decodeVarint, encodeFrame, encodeHeader, encodeVarint, type PbFrame } from "./pb"

describe("varint", () => {
  test("编码/解码 roundtrip", () => {
    for (const v of [0, 1, 127, 128, 300, 16383, 16384, 2 ** 20, 2 ** 32, Number.MAX_SAFE_INTEGER]) {
      const bytes = Uint8Array.from(encodeVarint(v))
      expect(decodeVarint(bytes, 0).value).toBe(v)
      expect(decodeVarint(bytes, 0).size).toBe(bytes.length)
    }
  })

  test("多字节 varint 连续解码（128 = 0x80 0x01）", () => {
    const bytes = Uint8Array.from([0x80, 0x01])
    const r = decodeVarint(bytes, 0)
    expect(r.value).toBe(128)
    expect(r.size).toBe(2)
  })

  test("负数/非整数按 0 处理", () => {
    expect(Uint8Array.from(encodeVarint(-5))[0]).toBe(0)
    expect(Uint8Array.from(encodeVarint(1.9))[0]).toBe(1)
  })
})

describe("Header 编码", () => {
  test("key/value roundtrip（手工解码 length-delimited 字段）", () => {
    const hb = encodeHeader({ key: "type", value: "event" })
    // Header: key=1 (len), value=2 (len)
    const dec = new TextDecoder()
    let offset = 0
    const fields: string[] = []
    const dv = (b: Uint8Array, o: number) => {
      let v = 0
      let s = 0
      for (let i = o; i < b.length; i++) {
        v += (b[i] & 0x7f) * 2 ** s
        if (!(b[i] & 0x80)) return { value: v, size: i - o + 1 }
        s += 7
      }
      throw new Error("bad varint")
    }
    while (offset < hb.length) {
      const tag = dv(hb, offset)
      offset += tag.size
      expect(tag.value & 7).toBe(2) // length-delimited
      const len = dv(hb, offset)
      offset += len.size
      fields.push(dec.decode(hb.slice(offset, offset + len.value)))
      offset += len.value
    }
    expect(fields).toEqual(["type", "event"])
  })
})

describe("Frame 编码/解码", () => {
  const sample: PbFrame = {
    SeqID: 0,
    LogID: 0,
    service: 123,
    method: 1,
    headers: [
      { key: "type", value: "event" },
      { key: "message_id", value: "om_abc" },
      { key: "sum", value: "1" },
    ],
    payload: new TextEncoder().encode('{"schema":"2.0"}'),
  }

  test("roundtrip 全部字段", () => {
    const buf = encodeFrame(sample)
    const dec = decodeFrame(buf)
    expect(dec.SeqID).toBe(0)
    expect(dec.LogID).toBe(0)
    expect(dec.service).toBe(123)
    expect(dec.method).toBe(1)
    expect(dec.headers).toEqual(sample.headers)
    expect(new TextDecoder().decode(dec.payload)).toBe('{"schema":"2.0"}')
  })

  test("可选字段 payload_encoding/payload_type/LogIDNew", () => {
    const buf = encodeFrame({ ...sample, payload_encoding: "json", payload_type: "event", LogIDNew: "x" })
    const dec = decodeFrame(buf)
    expect(dec.payload_encoding).toBe("json")
    expect(dec.payload_type).toBe("event")
    expect(dec.LogIDNew).toBe("x")
  })

  test("空 payload 不编码字段 8", () => {
    const buf = encodeFrame({ SeqID: 1, LogID: 2, service: 3, method: 0, headers: [] })
    const dec = decodeFrame(buf)
    expect(dec.payload).toBeUndefined()
  })

  test("大 header 值（中文）", () => {
    const buf = encodeFrame({ ...sample, headers: [{ key: "k", value: "飞书中文值" }] })
    expect(decodeFrame(buf).headers).toEqual([{ key: "k", value: "飞书中文值" }])
  })

  test("未知字段跳过、未知 wire type 报错", () => {
    // 手工构造：字段 99 varint + 合法帧
    const buf = Uint8Array.from([...encodeVarint((99 << 3) | 0), 0x01])
    const dec = decodeFrame(buf)
    expect(dec.method).toBe(0)
    // wire type 3（group）未支持 → 抛错
    expect(() => decodeFrame(Uint8Array.from([(1 << 3) | 3]))).toThrow()
  })

  test("截断字节抛错或合法（varint 未终止抛错）", () => {
    const buf = encodeFrame(sample)
    // 前 5 字节截断在 service varint 中途 → varint 越界
    expect(() => decodeFrame(buf.slice(0, 5))).toThrow()
    expect(() => decodeFrame(Uint8Array.from([0x0a, 0x05, 0x61]))).toThrow() // 声明长度超出
  })
})
