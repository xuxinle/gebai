import { describe, expect, test } from "bun:test"
import { deflateSync } from "node:zlib"
import { cropImage, decodePng, resizeBilinear, rgbaToCHW, type RgbaImage } from "./image"

/* ---------- 测试用极简 PNG 编码器（CRC 写零——解码器不校验） ---------- */

function chunk(type: string, data: number[]): number[] {
  const len = [(data.length >>> 24) & 255, (data.length >>> 16) & 255, (data.length >>> 8) & 255, data.length & 255]
  const t = Array.from(type, (c) => c.charCodeAt(0))
  return [...len, ...t, ...data, 0, 0, 0, 0]
}

function ihdr(w: number, h: number, colorType: number): number[] {
  const be32 = (v: number) => [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255]
  return [...be32(w), ...be32(h), 8, colorType, 0, 0, 0]
}

/** 前向滤波（与解码端 unfilter 互逆）。bpp = 每像素字节数。 */
function forwardFilter(lines: number[][], bpp: number, filter: number): Uint8Array {
  const out: number[] = []
  for (let y = 0; y < lines.length; y++) {
    const cur = lines[y]
    const prev = y > 0 ? lines[y - 1] : null
    out.push(filter)
    for (let i = 0; i < cur.length; i++) {
      const left = i >= bpp ? cur[i - bpp] : 0
      const up = prev ? prev[i] : 0
      const upLeft = prev && i >= bpp ? prev[i - bpp] : 0
      let v = cur[i]
      if (filter === 1) v -= left
      else if (filter === 2) v -= up
      else if (filter === 3) v -= (left + up) >> 1
      else if (filter === 4) {
        const p = left + up - upLeft
        const pa = Math.abs(p - left)
        const pb = Math.abs(p - up)
        const pc = Math.abs(p - upLeft)
        v -= pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft
      }
      out.push(v & 0xff)
    }
  }
  return new Uint8Array(out)
}

function png(w: number, h: number, colorType: number, raw: Uint8Array, extra: number[] = []): Uint8Array {
  const sig = [137, 80, 78, 71, 13, 10, 26, 10]
  const parts = [...sig, ...chunk("IHDR", ihdr(w, h, colorType)), ...extra, ...chunk("IDAT", Array.from(deflateSync(Buffer.from(raw)))), ...chunk("IEND", [])]
  return new Uint8Array(parts)
}

/** RGBA 逐行数组（colorType 6）→ PNG。 */
function rgbaPng(w: number, h: number, rgba: number[], filter = 0): Uint8Array {
  const lines: number[][] = []
  for (let y = 0; y < h; y++) lines.push(rgba.slice(y * w * 4, (y + 1) * w * 4))
  return png(w, h, 6, forwardFilter(lines, 4, filter))
}

const W = 3
const H = 2
const RGBA = [
  255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
  10, 20, 30, 128, 200, 100, 50, 255, 1, 2, 3, 4,
]

describe("decodePng", () => {
  test("RGBA filter=0 解码回环", () => {
    const img = decodePng(rgbaPng(W, H, RGBA, 0))
    expect(img.width).toBe(W)
    expect(img.height).toBe(H)
    expect(Array.from(img.data)).toEqual(RGBA)
  })

  test("RGBA 全部五种滤波器解码回环", () => {
    for (const f of [0, 1, 2, 3, 4]) {
      const img = decodePng(rgbaPng(W, H, RGBA, f))
      expect(Array.from(img.data)).toEqual(RGBA)
    }
  })

  test("灰度 colorType 0 → RGBA", () => {
    const img = decodePng(png(2, 2, 0, new Uint8Array([0, 0, 100, 0, 200, 255])))
    expect(img.width).toBe(2)
    expect(Array.from(img.data)).toEqual([0, 0, 0, 255, 100, 100, 100, 255, 200, 200, 200, 255, 255, 255, 255, 255])
  })

  test("调色板 colorType 3（含 tRNS 透明度）", () => {
    const plte = chunk("PLTE", [255, 0, 0, 0, 0, 255, 30, 30, 30])
    const trns = chunk("tRNS", [255, 128])
    const img = decodePng(png(3, 1, 3, new Uint8Array([0, 0, 1, 2]), [...plte, ...trns]))
    expect(Array.from(img.data)).toEqual([255, 0, 0, 255, 0, 0, 255, 128, 30, 30, 30, 255])
  })

  test("灰度+alpha colorType 4", () => {
    const img = decodePng(png(2, 1, 4, new Uint8Array([0, 7, 7, 200, 255])))
    expect(Array.from(img.data)).toEqual([7, 7, 7, 7, 200, 200, 200, 255])
  })

  test("非 PNG / 16-bit / 隔行 明确报错", () => {
    expect(() => decodePng(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9]))).toThrow(/非 PNG/)
    const bad16 = png(2, 1, 6, new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7, 8]))
    bad16.set([16], 8 + 8 + 8) // IHDR bitDepth → 16
    expect(() => decodePng(bad16)).toThrow(/8-bit/)
    const badInterlace = png(2, 1, 6, new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7, 8]))
    badInterlace.set([1], 8 + 8 + 12) // IHDR interlace → 1
    expect(() => decodePng(badInterlace)).toThrow(/隔行/)
  })

  test("数据截断报错", () => {
    const full = rgbaPng(W, H, RGBA)
    expect(() => decodePng(full.subarray(0, full.length - 20))).toThrow(/截断|不完整/)
  })
})

describe("cropImage / resizeBilinear / rgbaToCHW", () => {
  const img: RgbaImage = { width: 4, height: 4, data: new Uint8ClampedArray(64) }
  for (let i = 0; i < 16; i++) img.data.set([i, i * 2, i * 3, 255], i * 4)

  test("cropImage 取子区域", () => {
    const c = cropImage(img, { x: 1, y: 1, w: 2, h: 2 })
    expect(c.width).toBe(2)
    expect(c.height).toBe(2)
    expect(Array.from(c.data.slice(0, 4))).toEqual([5, 10, 15, 255]) // 像素 (1,1)
  })

  test("resizeBilinear 恒等尺寸原样返回；2x2 → 1x1 为四点均值", () => {
    expect(resizeBilinear(img, 4, 4)).toBe(img)
    const sq: RgbaImage = { width: 2, height: 2, data: new Uint8ClampedArray([0, 0, 0, 255, 100, 0, 0, 255, 0, 100, 0, 255, 100, 100, 0, 255]) }
    const r = resizeBilinear(sq, 1, 1)
    expect(r.width).toBe(1)
    expect(Math.round(r.data[0])).toBe(50)
    expect(Math.round(r.data[1])).toBe(50)
  })

  test("rgbaToCHW CHW 布局与归一化", () => {
    const one: RgbaImage = { width: 2, height: 1, data: new Uint8ClampedArray([255, 0, 128, 255, 10, 20, 30, 255]) }
    const t = rgbaToCHW(one, [0.5, 0.5, 0.5], [0.5, 0.5, 0.5])
    expect(t.length).toBe(6)
    // R 通道平面：((255/255)-0.5)/0.5 = 1，((10/255)-0.5)/0.5 ≈ -0.92
    expect(t[0]).toBeCloseTo(1, 5)
    expect(t[1]).toBeCloseTo(((10 / 255 - 0.5) / 0.5), 5)
    // G 通道平面从下标 2 开始
    expect(t[2]).toBeCloseTo(-1, 5)
  })
})
