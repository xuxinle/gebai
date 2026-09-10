import { describe, expect, test } from "bun:test"
import { ctcDecode, dbPostprocess, detPreprocess, recPreprocess, sortReadingOrder } from "./ocr"
import type { RgbaImage } from "./image"

function solidImage(w: number, h: number, fill: [number, number, number]): RgbaImage {
  const data = new Uint8ClampedArray(w * h * 4)
  for (let i = 0; i < w * h; i++) data.set([...fill, 255], i * 4)
  return { width: w, height: h, data }
}

describe("detPreprocess", () => {
  test("小图不缩放，右/下零填充至 32 倍数", () => {
    const { data, width, height, scale } = detPreprocess(solidImage(100, 40, [255, 255, 255]))
    expect(scale).toBe(1)
    expect(width).toBe(128)
    expect(height).toBe(64)
    expect(data.length).toBe(128 * 64 * 3)
  })

  test("大图等比缩放到长边 960", () => {
    const { width, height, scale } = detPreprocess(solidImage(1920, 1080, [0, 0, 0]))
    expect(scale).toBeCloseTo(0.5, 5)
    expect(width).toBe(960)
    expect(height).toBe(544) // 540 → 32 倍数
  })
})

describe("dbPostprocess", () => {
  function blobProb(mapW: number, mapH: number, blobs: Array<{ x: number; y: number; w: number; h: number }>): Float32Array {
    const p = new Float32Array(mapW * mapH).fill(0.05)
    for (const b of blobs) {
      for (let y = b.y; y < b.y + b.h; y++) {
        for (let x = b.x; x < b.x + b.w; x++) p[y * mapW + x] = 0.9
      }
    }
    return p
  }

  test("单一连通域 → 一个框，坐标按 scale 还原到原图", () => {
    const prob = blobProb(32, 32, [{ x: 10, y: 10, w: 8, h: 4 }])
    const boxes = dbPostprocess(prob, 32, 32, 64, 64, 0.5)
    expect(boxes.length).toBe(1)
    const b = boxes[0]
    expect(b.score).toBeGreaterThan(0.5)
    // 核心区 x∈[10,17] 膨胀后 [9,18]，d=(10×6×1.6)/(2×16)=3：原图坐标 x0=(9-3)/0.5=12，w=(19+3-9+3)/0.5=32
    expect(b.x).toBeCloseTo(12, 5)
    expect(b.w).toBeCloseTo(32, 5)
    expect(b.y).toBeCloseTo(12, 5)
    expect(b.h).toBeCloseTo(24, 5)
  })

  test("同行双块按 x 排序、双行按 y 排序（阅读序）", () => {
    const prob = blobProb(64, 64, [
      { x: 40, y: 10, w: 8, h: 4 }, // 右块（构造顺序在前）
      { x: 10, y: 10, w: 8, h: 4 }, // 左块
      { x: 10, y: 40, w: 8, h: 4 }, // 下一行
    ])
    const boxes = dbPostprocess(prob, 64, 64, 64, 64, 1)
    expect(boxes.length).toBe(3)
    expect(boxes[0].x).toBeLessThan(boxes[1].x)
    expect(boxes[0].y).toBeLessThan(boxes[2].y)
  })

  test("低置信度连通域被过滤", () => {
    const p = new Float32Array(32 * 32).fill(0.31) // 全图 0.31：均值不足 0.5
    expect(dbPostprocess(p, 32, 32, 32, 32, 1).length).toBe(0)
  })

  test("相邻小块被膨胀合并为单框", () => {
    // 两块间隔 1px：无膨胀会是两个连通域，膨胀后合并
    const prob = blobProb(32, 32, [
      { x: 10, y: 10, w: 4, h: 4 },
      { x: 15, y: 10, w: 4, h: 4 },
    ])
    expect(dbPostprocess(prob, 32, 32, 32, 32, 1).length).toBe(1)
  })
})

describe("sortReadingOrder", () => {
  test("空/单元素安全", () => {
    expect(sortReadingOrder([])).toEqual([])
    const one = [{ x: 1, y: 2, w: 3, h: 4 }]
    expect(sortReadingOrder(one)).toEqual(one)
  })
})

describe("recPreprocess", () => {
  test("48 高等比缩放 + 320 宽零填充", () => {
    const { data, width } = recPreprocess(solidImage(100, 20, [255, 255, 255]))
    expect(width).toBe(320)
    expect(data.length).toBe(320 * 48 * 3)
    // 内容区（缩放后宽 240）第一个像素：白色归一化 = (1-0.5)/0.5 = 1
    expect(data[0]).toBeCloseTo(1, 5)
    // 填充区（x=250 行 0）：黑 0 归一化 = -1
    expect(data[250]).toBeCloseTo(-1, 5)
  })

  test("过窄裁剪钳制最小宽 16", () => {
    const { data } = recPreprocess(solidImage(4, 48, [255, 0, 0]))
    expect(data.length).toBe(320 * 48 * 3)
    // 缩放后 R 通道内容区从 0 到 16*48
    expect(data[0]).toBeCloseTo(1, 5)
    expect(data[16 * 48]).toBeCloseTo(-1, 5)
  })
})

describe("ctcDecode", () => {
  const chars = ["·", "a", "b", "c"] // index 0 = blank 占位
  function probsFromSequence(seq: number[], classes = 4): Float32Array {
    const out = new Float32Array(seq.length * classes)
    for (let t = 0; t < seq.length; t++) {
      out[t * classes + seq[t]] = 0.9
      out[t * classes + (seq[t] + 1) % classes] = 0.05
    }
    return out
  }

  test("合并重复、去 blank", () => {
    const { text, score } = ctcDecode(probsFromSequence([1, 1, 0, 2, 2, 0, 1]), 7, 4, chars)
    expect(text).toBe("aba")
    expect(score).toBeGreaterThan(0.8)
  })

  test("全 blank 输出空文本零分", () => {
    const { text, score } = ctcDecode(probsFromSequence([0, 0, 0]), 3, 4, chars)
    expect(text).toBe("")
    expect(score).toBe(0)
  })

  test("越界类别下标安全跳过", () => {
    const p = new Float32Array([0.1, 0.0, 0.9]) // classes=3，argmax=2 越界（chars 长度 2）
    const { text } = ctcDecode(p, 1, 3, ["·", "a"])
    expect(text).toBe("")
  })
})
