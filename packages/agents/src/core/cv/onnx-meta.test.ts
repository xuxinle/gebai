import { describe, expect, test } from "bun:test"
import { parseOnnxInputSize, parseOnnxMetadata, ultralyticsMeta } from "./onnx-meta"

/* ---------- 微型 protobuf 编码器：手工构造 ModelProto 顶层字段 ---------- */

function varint(v: number): number[] {
  const out: number[] = []
  let n = v
  for (;;) {
    const b = n & 0x7f
    n = Math.floor(n / 128)
    out.push(n > 0 ? b | 0x80 : b)
    if (n === 0) return out
  }
}

function lenDelimited(field: number, payload: number[]): number[] {
  return [...varint((field << 3) | 2), ...varint(payload.length), ...payload]
}

function str(s: string): number[] {
  return Array.from(new TextEncoder().encode(s))
}

/** StringStringEntryProto {1: key, 2: value}，作为 ModelProto 顶层 field 14。 */
function metaEntry(key: string, value: string): number[] {
  return lenDelimited(14, [...lenDelimited(1, str(key)), ...lenDelimited(2, str(value))])
}

/** 模拟真实模型布局：ir_version(1, varint) + graph(7, 大消息按长度跳过) + metadata_props(14)。 */
function modelBytes(entries: string[]): Uint8Array {
  const parts: number[][] = []
  parts.push([...varint((1 << 3) | 0), ...varint(10)]) // ir_version = 10
  parts.push(lenDelimited(7, new Array(500).fill(1))) // graph 大消息（解析器只按长度跳过）
  parts.push(lenDelimited(8, [...varint(1), ...varint(20)])) // opset_import（嵌套消息）
  for (let i = 0; i + 1 < entries.length; i += 2) parts.push(metaEntry(entries[i], entries[i + 1]))
  const total = parts.reduce((n, p) => n + p.length, 0)
  const out = new Uint8Array(total)
  let off = 0
  for (const p of parts) {
    out.set(p, off)
    off += p.length
  }
  return out
}

describe("parseOnnxMetadata", () => {
  test("解析 metadata_props（跳过 graph 等大消息）", () => {
    const meta = parseOnnxMetadata(modelBytes(["imgsz", "[1280, 1280]", 'names', '{"0":"button"}']))
    expect(meta.imgsz).toBe("[1280, 1280]")
    expect(meta.names).toBe('{"0":"button"}')
  })

  test("无元数据 / 非 ONNX 字节 → 空表", () => {
    expect(Object.keys(parseOnnxMetadata(new Uint8Array(0))).length).toBe(0)
    expect(Object.keys(parseOnnxMetadata(new Uint8Array([9, 9, 9]))).length).toBe(0)
    expect(Object.keys(parseOnnxMetadata(modelBytes([]))).length).toBe(0)
  })
})

describe("ultralyticsMeta", () => {
  test("imgsz JSON 数组 → 边长；names JSON 对象按索引升序", () => {
    const m = ultralyticsMeta({ imgsz: "[1280, 1280]", names: '{"1":"icon","0":"button"}' })
    expect(m.imgsz).toBe(1280)
    expect(m.names).toEqual(["button", "icon"])
  })

  test("新版 ultralytics（8.4.x）names 为 Python repr 单引号字典——同样可解析", () => {
    const m = ultralyticsMeta({ imgsz: "[1280, 1280]", names: "{0: 'button', 1: 'icon', 2: \"input\"}" })
    expect(m.imgsz).toBe(1280)
    expect(m.names).toEqual(["button", "icon", "input"])
  })

  test("repr 键序乱序 / 混杂空值 → 按索引升序与过滤", () => {
    const m = ultralyticsMeta({ names: "{2: 'c', 0: 'a', 1: ''}" })
    expect(m.names).toEqual(["a", "c"])
  })

  test("names 数组形态 / imgsz 单值形态", () => {
    const m = ultralyticsMeta({ imgsz: "[960]", names: '["a","b","c"]' })
    expect(m.imgsz).toBe(960)
    expect(m.names).toEqual(["a", "b", "c"])
  })

  test("缺省 / 非法 / 越界（<320 或 >4096）→ null", () => {
    expect(ultralyticsMeta({})).toEqual({ imgsz: null, names: null })
    expect(ultralyticsMeta({ imgsz: "not-json", names: "oops" })).toEqual({ imgsz: null, names: null })
    expect(ultralyticsMeta({ imgsz: "[100, 100]" }).imgsz).toBeNull()
    expect(ultralyticsMeta({ imgsz: "[9999, 9999]" }).imgsz).toBeNull()
    expect(ultralyticsMeta({ names: '{}' }).names).toBeNull()
    expect(ultralyticsMeta({ names: '["", " "]' }).names).toBeNull()
  })
})

/* ---------- graph 首输入静态形状（parseOnnxInputSize） ---------- */

/** DimensionProto：dim_value(1) 静态维 / dim_param(2) 动态维。 */
function dim(entry: number | string): number[] {
  return typeof entry === "number" ? [...varint((1 << 3) | 0), ...varint(entry)] : lenDelimited(2, str(entry))
}

/** 模型布局：ir_version + graph(7, 首输入 ValueInfo 含静态形状) + opset(8) + metadata_props(14)。 */
function inputShapeModel(dims: (number | string)[], entries: string[] = []): Uint8Array {
  const shapeProto: number[] = []
  for (const d of dims) shapeProto.push(...lenDelimited(1, dim(d)))
  const tensorType = lenDelimited(1, lenDelimited(2, shapeProto)) // TypeProto.tensor_type(1).shape(2)
  const valueInfo = [...lenDelimited(1, str("images")), ...lenDelimited(2, tensorType)]
  const parts: number[][] = [
    [...varint((1 << 3) | 0), ...varint(10)],
    lenDelimited(7, lenDelimited(11, valueInfo)),
    lenDelimited(8, [...varint(1), ...varint(20)]),
  ]
  for (let i = 0; i + 1 < entries.length; i += 2) parts.push(metaEntry(entries[i], entries[i + 1]))
  const total = parts.reduce((n, p) => n + p.length, 0)
  const out = new Uint8Array(total)
  let off = 0
  for (const p of parts) {
    out.set(p, off)
    off += p.length
  }
  return out
}

describe("parseOnnxInputSize", () => {
  test("graph 首输入静态形状 [1,3,1280,1280] → 1280", () => {
    expect(parseOnnxInputSize(inputShapeModel([1, 3, 1280, 1280]))).toBe(1280)
  })

  test("动态 batch 维（dim_param）不参与判定 → 空间维 960", () => {
    expect(parseOnnxInputSize(inputShapeModel(["batch", 3, 960, 960]))).toBe(960)
  })

  test("空间维动态 / 不足两维 / 越界 → null", () => {
    expect(parseOnnxInputSize(inputShapeModel([1, 3, "h", 640]))).toBeNull()
    expect(parseOnnxInputSize(inputShapeModel([1, 3, 640]))).toBeNull()
    expect(parseOnnxInputSize(inputShapeModel([1, 3, 300, 300]))).toBeNull()
    expect(parseOnnxInputSize(inputShapeModel([1, 3, 8192, 8192]))).toBeNull()
  })

  test("无 graph / 垃圾字节 / graph 无输入 → null（元数据 fixture 的 graph 为不透明字节）", () => {
    expect(parseOnnxInputSize(new Uint8Array(0))).toBeNull()
    expect(parseOnnxInputSize(new Uint8Array([9, 9, 9]))).toBeNull()
    expect(parseOnnxInputSize(modelBytes(["imgsz", "[640, 640]"]))).toBeNull()
  })
})
