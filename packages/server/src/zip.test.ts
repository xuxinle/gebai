import { describe, expect, test } from "bun:test"
import { inflateRawSync } from "node:zlib"
import { buildZip, crc32 } from "./zip"

/** 最小 ZIP 读取器：解析 EOCD + central directory + local header，返回 {name, content}[]。 */
function readZip(buf: Uint8Array): Array<{ name: string; content: Uint8Array }> {
  const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength)
  // 定位 EOCD（从末尾向前找 0x06054b50）
  let eocd = -1
  for (let i = buf.length - 22; i >= 0; i--) {
    if (view.getUint32(i, true) === 0x06054b50) {
      eocd = i
      break
    }
  }
  expect(eocd).toBeGreaterThanOrEqual(0)
  const cdCount = view.getUint16(eocd + 10, true)
  const cdOffset = view.getUint32(eocd + 16, true)
  const files: Array<{ name: string; content: Uint8Array }> = []
  let pos = cdOffset
  for (let i = 0; i < cdCount; i++) {
    expect(view.getUint32(pos, true)).toBe(0x02014b50) // central dir signature
    const method = view.getUint16(pos + 10, true)
    const crc = view.getUint32(pos + 16, true)
    const compSize = view.getUint32(pos + 20, true)
    const rawSize = view.getUint32(pos + 24, true)
    const nameLen = view.getUint16(pos + 28, true)
    const extraLen = view.getUint16(pos + 30, true)
    const commentLen = view.getUint16(pos + 32, true)
    const localOffset = view.getUint32(pos + 42, true)
    const name = new TextDecoder().decode(buf.subarray(pos + 46, pos + 46 + nameLen))
    // local header
    const lh = localOffset
    expect(view.getUint32(lh, true)).toBe(0x04034b50) // local header signature
    const lNameLen = view.getUint16(lh + 26, true)
    const lExtraLen = view.getUint16(lh + 28, true)
    const dataStart = lh + 30 + lNameLen + lExtraLen
    const raw = buf.subarray(dataStart, dataStart + compSize)
    const content = method === 8 ? inflateRawSync(raw) : raw
    expect(content.length).toBe(rawSize)
    expect(crc32(content)).toBe(crc)
    files.push({ name, content })
    pos += 46 + nameLen + extraLen + commentLen
  }
  return files
}

describe("buildZip", () => {
  test("packages multiple files with UTF-8 names and correct content", () => {
    const zip = buildZip([
      { name: "a.txt", data: new TextEncoder().encode("hello zip") },
      { name: "中文/目录/报告.md", data: new TextEncoder().encode("# 报告\n内容") },
      { name: "empty.bin", data: new Uint8Array(0) },
    ])
    const files = readZip(zip)
    expect(files).toHaveLength(3)
    expect(files[0].name).toBe("a.txt")
    expect(new TextDecoder().decode(files[0].content)).toBe("hello zip")
    expect(files[1].name).toBe("中文/目录/报告.md")
    expect(new TextDecoder().decode(files[1].content)).toBe("# 报告\n内容")
    expect(files[2].content.length).toBe(0)
  })

  test("empty archive is still valid (zero entries)", () => {
    const zip = buildZip([])
    expect(readZip(zip)).toHaveLength(0)
  })

  test("crc32 known vector", () => {
    expect(crc32(new TextEncoder().encode("123456789"))).toBe(0xcbf43926)
  })
})
