/**
 * ONNX 模型元数据解析（core/cv，纯函数）：从模型文件字节直接解析 ModelProto 的
 * metadata_props（field 14，StringStringEntryProto {key,value}），不依赖 ort 运行时——
 * wasm（onnxruntime-web）与 GPU sidecar（onnxruntime-node）两条推理通道同一解析口径。
 *
 * 主要消费 ultralytics 导出约定（YOLO11/v8 ONNX 自带）：`imgsz`（如 "[1280, 1280]"，
 * letterbox 尺寸来源——官方常按 1280 训练导出，固定 640 会形状不匹配/掉精度）与
 * `names`（如 {"0":"button","1":"icon"}，类别表来源——省去单独的标签文件）。
 * 只需顶层字段遍历（graph 等大消息按长度跳过），不构造完整 protobuf 树。
 */

/** 读 varint（返回值与下一位置）；越界返回 null。 */
function readVarint(bytes: Uint8Array, p: number): [number, number] | null {
  let v = 0
  let shift = 0
  for (;;) {
    if (p >= bytes.length) return null
    const b = bytes[p++]
    v |= (b & 0x7f) * 2 ** shift
    if ((b & 0x80) === 0) return [v, p]
    shift += 7
    if (shift > 56) return null
  }
}

/** 解析单个 StringStringEntryProto：{1: key, 2: value} → [key, value]。 */
function parseEntry(entry: Uint8Array): [string, string] | null {
  let key = ""
  let value = ""
  let p = 0
  while (p < entry.length) {
    const head = readVarint(entry, p)
    if (!head) return null
    const [tag, q] = head
    p = q
    const field = tag >>> 3
    const wt = tag & 7
    if (wt === 0) {
      const v = readVarint(entry, p)
      if (!v) return null
      p = v[1]
    } else if (wt === 2) {
      const len = readVarint(entry, p)
      if (!len || len[1] + len[0] > entry.length) return null
      const text = new TextDecoder().decode(entry.subarray(len[1], len[1] + len[0]))
      if (field === 1) key = text
      else if (field === 2) value = text
      p = len[1] + len[0]
    } else if (wt === 1) p += 8
    else if (wt === 5) p += 4
    else return null
  }
  return key ? [key, value] : null
}

/** 解析 ONNX 顶层 metadata_props → 键值表（无元数据/解析失败返回空表）。 */
export function parseOnnxMetadata(bytes: Uint8Array): Record<string, string> {
  const out: Record<string, string> = {}
  let p = 0
  while (p < bytes.length) {
    const head = readVarint(bytes, p)
    if (!head) break
    const [tag, q] = head
    p = q
    const field = tag >>> 3
    const wt = tag & 7
    if (wt === 0) {
      const v = readVarint(bytes, p)
      if (!v) break
      p = v[1]
    } else if (wt === 2) {
      const len = readVarint(bytes, p)
      if (!len || len[1] + len[0] > bytes.length) break
      if (field === 14) {
        const kv = parseEntry(bytes.subarray(len[1], len[1] + len[0]))
        if (kv) out[kv[0]] = kv[1]
      }
      p = len[1] + len[0]
    } else if (wt === 1) p += 8
    else if (wt === 5) p += 4
    else break
  }
  return out
}

export interface UltralyticsMeta {
  /** 训练/导出输入边长（正方形 letterbox 目标尺寸）。 */
  imgsz: number | null
  /** 类别表（按索引升序）。 */
  names: string[] | null
}

/** Python repr 字典解析（新版 ultralytics 导出 names 为单引号 repr 而非 JSON）：{0: 'a', 1: "b"}。 */
function parsePyDict(s: string): string[] | null {
  let p = 0
  const skipWs = () => {
    while (p < s.length && /\s/.test(s[p])) p++
  }
  const readString = (): string | null => {
    const q = s[p]
    if (q !== "'" && q !== '"') return null
    const end = s.indexOf(q, p + 1)
    if (end < 0) return null
    const text = s.slice(p + 1, end)
    p = end + 1
    return text
  }
  skipWs()
  if (s[p] !== "{") return null
  p++
  const entries = new Map<number, string>()
  for (;;) {
    skipWs()
    if (s[p] === "}") {
      p++
      break
    }
    const key = readString() ?? (() => {
      const m = /^\d+/.exec(s.slice(p))
      if (!m) return null
      p += m[0].length
      return m[0]
    })()
    if (key === null) return null
    skipWs()
    if (s[p] !== ":") return null
    p++
    skipWs()
    const val = readString() ?? (() => {
      const m = /^[^,}]+/.exec(s.slice(p))
      if (!m) return null
      p += m[0].length
      return m[0].trim()
    })()
    if (val === null) return null
    const idx = Number(key)
    if (Number.isInteger(idx) && idx >= 0 && val.trim()) entries.set(idx, val.trim()) // 空值跳过（不整体失败）
    skipWs()
    if (s[p] === ",") {
      p++
      continue
    }
    if (s[p] === "}") {
      p++
      break
    }
    return null
  }
  if (!entries.size) return null
  return [...entries.entries()].sort((a, b) => a[0] - b[0]).map(([, v]) => v)
}

/** 从元数据表提取 ultralytics 约定（imgsz JSON 数组 / names JSON 或 Python repr 字典）；无效或缺失返回 null 项。 */
export function ultralyticsMeta(meta: Record<string, string>): UltralyticsMeta {
  let imgsz: number | null = null
  let names: string[] | null = null
  try {
    const size = JSON.parse(meta.imgsz ?? "")
    const first = Array.isArray(size) ? Number(size[0]) : Number(size)
    if (Number.isFinite(first) && first >= 320 && first <= 4096) imgsz = Math.round(first)
  } catch { /* 无 imgsz 或非数组形态 */ }
  for (const parse of [JSON.parse, parsePyDict]) {
    try {
      const parsed = parse(meta.names ?? "")
      if (Array.isArray(parsed)) {
        const list = parsed.map((v) => String(v).trim()).filter(Boolean)
        if (list.length) {
          names = list
          break
        }
      } else if (parsed && typeof parsed === "object") {
        const entries = Object.entries(parsed as Record<string, unknown>)
          .map(([k, v]) => [Number(k), String(v).trim()] as const)
          .filter(([k, v]) => Number.isInteger(k) && k >= 0 && v)
          .sort((a, b) => a[0] - b[0])
        if (entries.length) {
          names = entries.map(([, v]) => v)
          break
        }
      }
    } catch { /* 该形态解析失败——尝试下一种 */ }
  }
  return { imgsz, names }
}
