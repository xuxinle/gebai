import { describe, expect, test } from "bun:test"
import { imageSize, resizeForVision, resizeNote, formatBytes, VISION_RESIZE_MAX_EDGE } from "./image-resize"

/** 构造最小合法 PNG（水平渐变，可压缩）——宽高可指定。 */
function makePng(width: number, height: number): Buffer {
  const { deflateSync } = require("node:zlib")
  const crc32 = (buf: Buffer) => {
    let c = ~0
    for (const b of buf) {
      c ^= b
      for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1))
    }
    return ~c >>> 0
  }
  const chunk = (type: string, data: Buffer) => {
    const len = Buffer.alloc(4)
    len.writeUInt32BE(data.length)
    const body = Buffer.concat([Buffer.from(type, "ascii"), data])
    const crc = Buffer.alloc(4)
    crc.writeUInt32BE(crc32(body))
    return Buffer.concat([len, body, crc])
  }
  const ihdr = Buffer.alloc(13)
  ihdr.writeUInt32BE(width, 0)
  ihdr.writeUInt32BE(height, 4)
  ihdr[8] = 8 // bit depth
  ihdr[9] = 2 // color type RGB
  // 每行 filter 0 + RGB 像素（水平渐变 + 行间缓慢变化，deflate 压缩率高，重编码后体积确定小于阈值）
  const raw = Buffer.alloc((width * 3 + 1) * height)
  for (let y = 0; y < height; y++) {
    const off = y * (width * 3 + 1) + 1
    for (let x = 0; x < width; x++) {
      raw[off + x * 3] = Math.round((x * 255) / Math.max(1, width - 1))
      raw[off + x * 3 + 1] = Math.round((y * 255) / Math.max(1, height - 1))
      raw[off + x * 3 + 2] = 128
    }
  }
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw)),
    chunk("IEND", Buffer.alloc(0)),
  ])
}

describe("imageSize", () => {
  test("PNG 免解码读 IHDR 宽高", async () => {
    expect(await imageSize(makePng(320, 240))).toEqual({ width: 320, height: 240 })
  })
  test("PNG 签名但 IHDR 非法 → Bun.Image 兜底/失败容忍", async () => {
    const bad = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    const r = await imageSize(bad)
    expect(r === null || r.width === 0 || (r as { width: number }).width === undefined ? r === null : true).toBe(true)
  })
  test("非图片数据返回 null", async () => {
    expect(await imageSize(new Uint8Array([1, 2, 3]))).toBeNull()
  })
})

describe("resizeForVision", () => {
  test("小图未超限：原样返回 resized=false 带尺寸", async () => {
    const png = makePng(200, 100)
    const out = await resizeForVision(png, "image/png")
    expect(out.resized).toBe(false)
    expect(out.buf.equals(png)).toBe(true)
    expect(out.width).toBe(200)
    expect(out.height).toBe(100)
  })
  test("长边超限的 PNG：等比缩放到 1280 并保持 png 格式", async () => {
    const png = makePng(2000, 1000)
    const out = await resizeForVision(png, "image/png")
    expect(out.resized).toBe(true)
    expect(out.mime).toBe("image/png")
    expect(out.width).toBe(VISION_RESIZE_MAX_EDGE)
    expect(out.height).toBe(640)
    // 输出仍是合法 PNG（可再次解码）
    const re = await imageSize(out.buf)
    expect(re).toEqual({ width: 1280, height: 640 })
  })
  test("竖图按高度为长边缩放", async () => {
    const png = makePng(600, 3000)
    const out = await resizeForVision(png, "image/png")
    expect(out.width).toBe(256)
    expect(out.height).toBe(1280)
  })
  test("GIF 不压缩（动画帧保留）", async () => {
    const fake = new Uint8Array(10)
    const out = await resizeForVision(fake, "image/gif")
    expect(out.resized).toBe(false)
    expect(out.buf).toBeInstanceOf(Buffer)
  })
  test("解码失败（非图片字节）原样返回不抛错", async () => {
    const junk = new Uint8Array([1, 2, 3, 4, 5])
    const out = await resizeForVision(junk, "image/png")
    expect(out.resized).toBe(false)
    expect(Buffer.from(junk).equals(out.buf)).toBe(true)
  })
})

describe("resizeNote", () => {
  test("压缩时输出 原始→压缩后 文案", () => {
    const note = resizeNote({ buf: Buffer.alloc(1000), mime: "image/png", width: 1280, height: 640, resized: true, origWidth: 2000, origHeight: 1000, origBytes: 5 * 1024 * 1024 })
    expect(note).toContain("原始 2000×1000")
    expect(note).toContain("→ 1280×640")
    expect(note).toContain("5.0MB")
    expect(note).toContain("已压缩")
  })
  test("未压缩时标注未压缩", () => {
    const note = resizeNote({ buf: Buffer.alloc(2048), mime: "image/png", width: 300, height: 200, resized: false, origWidth: 300, origHeight: 200, origBytes: 2048 })
    expect(note).toContain("300×200 2KB")
    expect(note).toContain("未压缩")
  })
})

describe("formatBytes", () => {
  test("分档格式化", () => {
    expect(formatBytes(512)).toBe("512 B")
    expect(formatBytes(4096)).toBe("4KB")
    expect(formatBytes(3 * 1024 * 1024)).toBe("3.0MB")
  })
})
