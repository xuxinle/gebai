import { describe, expect, test } from "bun:test"
import type { RgbaImage } from "./image"
import { matchTemplate } from "./template"

function mulberry32(seed: number): () => number {
  let a = seed
  return () => {
    a |= 0
    a = (a + 0x6d2b79f5) | 0
    let t = Math.imul(a ^ (a >>> 15), 1 | a)
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
}

/** 背景渐变 + 伪随机扰动的合成图（避免大面积纯色导致窗口零方差）。 */
function makeSearch(w: number, h: number, seed = 42): RgbaImage {
  const rnd = mulberry32(seed)
  const data = new Uint8ClampedArray(w * h * 4)
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const base = 60 + (x * 120) / w + (y * 40) / h + rnd() * 8
      const o = (y * w + x) * 4
      data[o] = base
      data[o + 1] = base * 0.8 + rnd() * 6
      data[o + 2] = 160 - base * 0.5
      data[o + 3] = 255
    }
  }
  return { width: w, height: h, data }
}

/** 伪随机纹理模板（确定性，含结构）。 */
function makePatch(size: number, seed: number): RgbaImage {
  const rnd = mulberry32(seed)
  const data = new Uint8ClampedArray(size * size * 4)
  for (let i = 0; i < size * size; i++) {
    const v = 40 + rnd() * 200
    data[i * 4] = v
    data[i * 4 + 1] = 200 - v * 0.7
    data[i * 4 + 2] = v * 0.5 + 30
    data[i * 4 + 3] = 255
  }
  return { width: size, height: size, data }
}

function paste(dst: RgbaImage, patch: RgbaImage, x0: number, y0: number, jitter = 0): void {
  const rnd = mulberry32(7)
  for (let y = 0; y < patch.height; y++) {
    for (let x = 0; x < patch.width; x++) {
      const d = (y0 + y) * dst.width + x0 + x
      const s = y * patch.width + x
      const j = jitter ? (rnd() - 0.5) * jitter : 0
      dst.data[d * 4] = patch.data[s * 4] + j
      dst.data[d * 4 + 1] = patch.data[s * 4 + 1] + j
      dst.data[d * 4 + 2] = patch.data[s * 4 + 2] + j
    }
  }
}

describe("matchTemplate", () => {
  test("在搜索图中精确命中嵌入模板的位置", () => {
    const search = makeSearch(200, 100)
    const patch = makePatch(24, 99)
    paste(search, patch, 60, 40)
    const matches = matchTemplate(search, patch, { threshold: 0.85 })
    expect(matches.length).toBeGreaterThan(0)
    expect(matches[0].x).toBe(60)
    expect(matches[0].y).toBe(40)
    expect(matches[0].score).toBeGreaterThan(0.95)
  })

  test("48px 模板走降采样粗扫路径仍精确命中", () => {
    const search = makeSearch(320, 200, 7)
    const patch = makePatch(48, 123)
    paste(search, patch, 130, 66)
    const matches = matchTemplate(search, patch, { threshold: 0.85 })
    expect(matches[0].x).toBe(130)
    expect(matches[0].y).toBe(66)
    expect(matches[0].score).toBeGreaterThan(0.95)
  })

  test("轻微噪声扰动下仍命中（阈值 0.8）", () => {
    const search = makeSearch(200, 100)
    const patch = makePatch(24, 55)
    paste(search, patch, 30, 20, 6)
    const matches = matchTemplate(search, patch, { threshold: 0.8 })
    expect(matches.length).toBeGreaterThan(0)
    expect(matches[0].x).toBe(30)
    expect(matches[0].y).toBe(20)
  })

  test("未出现的目标不产生匹配", () => {
    const search = makeSearch(200, 100)
    const absent = makePatch(24, 777)
    expect(matchTemplate(search, absent, { threshold: 0.8 })).toEqual([])
  })

  test("同一模板多处出现全部返回（按分数降序）", () => {
    const search = makeSearch(300, 150)
    const patch = makePatch(20, 321)
    paste(search, patch, 20, 20)
    paste(search, patch, 200, 90)
    const matches = matchTemplate(search, patch, { threshold: 0.85 })
    expect(matches.length).toBe(2)
    expect(matches.map((m) => `${m.x},${m.y}`).sort()).toEqual(["20,20", "200,90"])
    expect(matches[0].score).toBeGreaterThanOrEqual(matches[1].score)
  })

  test("模板大于搜索图抛中文错误", () => {
    const search = makeSearch(50, 50)
    const big = makePatch(64, 1)
    expect(() => matchTemplate(search, big)).toThrow(/大于搜索图/)
  })

  test("纯色模板抛中文错误", () => {
    const search = makeSearch(100, 100)
    const flat: RgbaImage = {
      width: 16,
      height: 16,
      data: new Uint8ClampedArray(16 * 16 * 4).fill(255),
    }
    expect(() => matchTemplate(search, flat)).toThrow(/纯色/)
  })
})
