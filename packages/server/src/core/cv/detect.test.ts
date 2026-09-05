import { describe, expect, test } from "bun:test"
import { DETECT_SIZE, letterbox, yoloPostprocess } from "./detect"
import type { RgbaImage } from "./image"

function solidImage(w: number, h: number, fill: [number, number, number]): RgbaImage {
  const data = new Uint8ClampedArray(w * h * 4)
  for (let i = 0; i < w * h; i++) data.set([...fill, 255], i * 4)
  return { width: w, height: h, data }
}

describe("letterbox", () => {
  test("等比缩放 + 居中 padding 参数", () => {
    const r = letterbox(solidImage(100, 50, [255, 0, 0]), 200)
    expect(r.size).toBe(200)
    expect(r.scale).toBeCloseTo(2, 5)
    expect(r.padX).toBe(0)
    expect(r.padY).toBe(50)
    expect(r.data.length).toBe(200 * 200 * 3)
  })

  test("填充区为 114 灰（px/255 归一化）", () => {
    const r = letterbox(solidImage(100, 50, [255, 0, 0]), 200)
    // 顶部 padding 第一个像素（y=0, x=0）
    expect(r.data[0]).toBeCloseTo(114 / 255, 5)
  })

  test("内容区首像素来自原图", () => {
    const r = letterbox(solidImage(100, 50, [200, 100, 50]), 200)
    // 内容起点 (x=0, y=50)，CHW 布局 R 平面
    const idx = 50 * 200 + 0
    expect(r.data[idx]).toBeCloseTo(200 / 255, 5)
  })
})

describe("yoloPostprocess", () => {
  const labels = ["cat", "dog"]
  const params = { srcW: 100, srcH: 100, scale: 2, padX: 10, padY: 0, conf: 0.25 }
  const N = 8 // 候选数须显著大于通道数（真实 v8 输出 8400），否则形状启发式误判

  test("v8 形态 [1, 4+nc, N]：坐标还原 letterbox", () => {
    const out = new Float32Array((4 + 2) * N)
    // 候选 0：letterbox 坐标 cx=110 cy=20 w=20 h=10，类别 1 高分
    // 原图坐标：x1=(110-10-10)/2=45, y1=(20-5-0)/2=7.5, w=20/2=10, h=10/2=5
    out[0 * N + 0] = 110
    out[1 * N + 0] = 20
    out[2 * N + 0] = 20
    out[3 * N + 0] = 10
    out[(4 + 1) * N + 0] = 0.9
    const objs = yoloPostprocess(out, [1, 6, N], labels, params)
    expect(objs.length).toBe(1)
    expect(objs[0].label).toBe("dog")
    expect(objs[0].score).toBeCloseTo(0.9, 5)
    expect(objs[0].x).toBeCloseTo(45, 5)
    expect(objs[0].y).toBeCloseTo(7.5, 5)
    expect(objs[0].w).toBeCloseTo(10, 5)
    expect(objs[0].h).toBeCloseTo(5, 5)
  })

  test("v5 形态 [1, N, 5+nc]：obj×cls 得分", () => {
    const per = 7 // 5 + 2 类
    const out = new Float32Array(N * per)
    out[0] = 110
    out[1] = 20
    out[2] = 20
    out[3] = 10
    out[4] = 0.9 // obj
    out[5 + 1] = 0.9 // dog
    const objs = yoloPostprocess(out, [1, N, per], labels, params)
    expect(objs.length).toBe(1)
    expect(objs[0].label).toBe("dog")
    expect(objs[0].score).toBeCloseTo(0.81, 5)
  })

  test("低于 conf 的候选被过滤", () => {
    const out = new Float32Array(6 * N)
    out[(4 + 0) * N] = 0.1
    expect(yoloPostprocess(out, [1, 6, N], labels, params).length).toBe(0)
  })

  test("同类重叠 NMS 去重、异类保留", () => {
    const out = new Float32Array(6 * N)
    const set = (i: number, cx: number, cy: number, w: number, h: number, cls: number, s: number) => {
      out[0 * N + i] = cx
      out[1 * N + i] = cy
      out[2 * N + i] = w
      out[3 * N + i] = h
      out[(4 + cls) * N + i] = s
    }
    set(0, 100, 50, 40, 40, 0, 0.9) // cat 高分
    set(1, 102, 52, 40, 40, 0, 0.8) // cat 重叠（IoU 高）→ 抑制
    set(2, 100, 50, 40, 40, 1, 0.7) // dog 同位置异类 → 保留
    const objs = yoloPostprocess(out, [1, 6, N], labels, params)
    expect(objs.length).toBe(2)
    expect(objs.map((o) => o.label).sort()).toEqual(["cat", "dog"])
  })

  test("类别数超过标签行数报错", () => {
    const out = new Float32Array(6 * N)
    expect(() => yoloPostprocess(out, [1, 6, N], ["only-one"], params)).toThrow(/标签文件/)
  })
})

describe("DETECT_SIZE 常量", () => {
  test("缺省 640", () => {
    expect(DETECT_SIZE).toBe(640)
  })
})
