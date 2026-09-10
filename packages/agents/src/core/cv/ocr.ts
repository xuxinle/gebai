/**
 * PP-OCR（det/rec ONNX）前后处理纯函数：det 前处理（等比缩放 + 32 倍数零填充）、
 * DB 后处理（阈值→膨胀→连通域→外接框→unclip 外扩→阅读序）、rec 前处理（48 高等比缩放 + 320 宽零填充）、
 * CTC 贪心解码。前后处理约定对齐 RapidOCR 的 PP-OCR ONNX 部署实现
 * （det 归一化 ImageNet mean/std；rec 归一化 0.5/0.5；CTC blank = index 0，字典尾追加空格）。
 */
import { type Box, type RgbaImage, resizeBilinear, rgbaToCHW } from "./image"

/** det 输入长边上限（对齐 32 的倍数，wasm CPU 上亚秒级）。 */
export const DET_MAX_SIDE = 960
/** DBNet 网络要求输入宽高为 32 的倍数。 */
const DET_ALIGN = 32
/** DB 概率图二值化阈值。 */
export const DB_THRESHOLD = 0.3
/** DB 文本框平均置信度下限。 */
const DB_BOX_THRESHOLD = 0.5
/** DB unclip 外扩系数（热力图收缩的逆过程，按面积/周长外扩）。 */
const DB_UNCLIP_RATIO = 1.6
/** rec 输入高度（PP-OCR mobile 系列）。 */
export const REC_HEIGHT = 48
/** rec 输入宽度（等比缩放后零填充到该宽度，CTC 忽略尾部填充时间步）。 */
export const REC_WIDTH = 320
/** CTC blank 类别下标（PP-OCR ONNX 导出约定；个别导出在末位，届时改此常量）。 */
export const CTC_BLANK_INDEX = 0

const DET_MEAN: [number, number, number] = [0.485, 0.456, 0.406]
const DET_STD: [number, number, number] = [0.229, 0.224, 0.225]
const REC_MEAN: [number, number, number] = [0.5, 0.5, 0.5]
const REC_STD: [number, number, number] = [0.5, 0.5, 0.5]

export interface OcrLine {
  text: string
  score: number
  /** 相对输入图像（裁剪/缩放后的那份）的像素坐标 */
  box: Box
}

/** det 前处理：等比缩放（长边 ≤ maxSide）+ 右/下零填充至 32 倍数。scale 供坐标还原。 */
export function detPreprocess(
  img: RgbaImage,
  maxSide = DET_MAX_SIDE,
): { data: Float32Array; width: number; height: number; scale: number } {
  const scale = Math.min(1, maxSide / Math.max(img.width, img.height))
  const w = Math.max(1, Math.round(img.width * scale))
  const h = Math.max(1, Math.round(img.height * scale))
  const pw = align32(w)
  const ph = align32(h)
  const resized = resizeBilinear(img, w, h)
  const canvas: RgbaImage = { width: pw, height: ph, data: new Uint8ClampedArray(pw * ph * 4) }
  for (let row = 0; row < h; row++) {
    canvas.data.set(resized.data.subarray(row * w * 4, (row + 1) * w * 4), row * pw * 4)
  }
  return { data: rgbaToCHW(canvas, DET_MEAN, DET_STD), width: pw, height: ph, scale }
}

function align32(v: number): number {
  return Math.max(DET_ALIGN, Math.ceil(v / DET_ALIGN) * DET_ALIGN)
}

/** det 后处理产出：外接框 + 框内平均置信度。 */
export interface DbBox extends Box {
  score: number
}

/** DB 后处理：概率图（det 模型输出，尺度与 det 输入一致）→ 原图坐标文本行框，阅读序排序。 */
export function dbPostprocess(
  prob: Float32Array,
  mapW: number,
  mapH: number,
  srcW: number,
  srcH: number,
  scale: number,
): DbBox[] {
  const mask = new Uint8Array(mapW * mapH)
  for (let i = 0; i < mask.length; i++) mask[i] = prob[i] > DB_THRESHOLD ? 1 : 0
  const dil = dilate3x3(mask, mapW, mapH)
  const visited = new Uint8Array(mapW * mapH)
  const stack: number[] = []
  const boxes: DbBox[] = []
  for (let start = 0; start < dil.length; start++) {
    if (!dil[start] || visited[start]) continue
    // 8-连通洪泛（显式栈防深图递归溢出）
    stack.length = 0
    stack.push(start)
    visited[start] = 1
    let minx = mapW
    let miny = mapH
    let maxx = -1
    let maxy = -1
    let count = 0
    let scoreSum = 0
    while (stack.length) {
      const p = stack.pop()!
      const x = p % mapW
      const y = (p / mapW) | 0
      if (x < minx) minx = x
      if (y < miny) miny = y
      if (x > maxx) maxx = x
      if (y > maxy) maxy = y
      // 置信度只在核心像素（膨胀前的二值命中）上取均值——膨胀桥接的低值像素不计入
      if (mask[p]) {
        count++
        scoreSum += prob[p]
      }
      for (let dy = -1; dy <= 1; dy++) {
        for (let dx = -1; dx <= 1; dx++) {
          const nx = x + dx
          const ny = y + dy
          if (nx < 0 || ny < 0 || nx >= mapW || ny >= mapH) continue
          const np = ny * mapW + nx
          if (dil[np] && !visited[np]) {
            visited[np] = 1
            stack.push(np)
          }
        }
      }
    }
    const score = count ? scoreSum / count : 0
    if (score < DB_BOX_THRESHOLD) continue
    const w = maxx - minx + 1
    const h = maxy - miny + 1
    if (w < 3 || h < 3) continue
    // unclip：按 面积×系数/周长 外扩（轴对称近似，UI 文本为水平排布，够用）
    const d = (w * h * DB_UNCLIP_RATIO) / (2 * (w + h))
    const x0 = clamp((minx - d) / scale, 0, srcW)
    const y0 = clamp((miny - d) / scale, 0, srcH)
    const x1 = clamp((maxx + 1 + d) / scale, 0, srcW)
    const y1 = clamp((maxy + 1 + d) / scale, 0, srcH)
    if (x1 - x0 < 3 || y1 - y0 < 3) continue
    boxes.push({ x: x0, y: y0, w: x1 - x0, h: y1 - y0, score })
  }
  return sortReadingOrder(boxes)
}

/** 3x3 膨胀（一次）：DB 热力图是收缩过的，膨胀合并碎片防同一文本行拆散。返回膨胀后副本。 */
function dilate3x3(mask: Uint8Array, w: number, h: number): Uint8Array {
  const out = new Uint8Array(mask)
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      if (mask[y * w + x]) continue
      let hit = 0
      for (let dy = -1; dy <= 1 && !hit; dy++) {
        for (let dx = -1; dx <= 1; dx++) {
          const nx = x + dx
          const ny = y + dy
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue
          if (mask[ny * w + nx]) {
            hit = 1
            break
          }
        }
      }
      if (hit) out[y * w + x] = 1
    }
  }
  return out
}

/** 阅读序排序：按垂直中心聚类成行（容差 = 0.6×中位行高），行内按 x 排序。 */
export function sortReadingOrder<T extends Box>(boxes: T[]): T[] {
  if (boxes.length < 2) return boxes.slice()
  const heights = boxes.map((b) => b.h).sort((a, b) => a - b)
  const tol = Math.max(8, heights[heights.length >> 1] * 0.6)
  const keyed = boxes.map((b) => ({ b, row: Math.round((b.y + b.h / 2) / tol) }))
  keyed.sort((p, q) => p.row - q.row || p.b.x - q.b.x)
  return keyed.map((k) => k.b)
}

/** rec 前处理：等比缩放到 48 高（宽钳制 [16, 320]）+ 右侧零填充至 320 宽。 */
export function recPreprocess(crop: RgbaImage): { data: Float32Array; width: number } {
  const w = Math.min(REC_WIDTH, Math.max(16, Math.round((crop.width / crop.height) * REC_HEIGHT)))
  const resized = resizeBilinear(crop, w, REC_HEIGHT)
  const canvas: RgbaImage = { width: REC_WIDTH, height: REC_HEIGHT, data: new Uint8ClampedArray(REC_WIDTH * REC_HEIGHT * 4) }
  for (let row = 0; row < REC_HEIGHT; row++) {
    canvas.data.set(resized.data.subarray(row * w * 4, (row + 1) * w * 4), row * REC_WIDTH * 4)
  }
  return { data: rgbaToCHW(canvas, REC_MEAN, REC_STD), width: REC_WIDTH }
}

/** CTC 贪心解码：逐时间步 argmax，合并连续相同后去 blank；返回文本与命中步平均置信度。 */
export function ctcDecode(
  probs: Float32Array,
  steps: number,
  classes: number,
  chars: string[],
): { text: string; score: number } {
  let text = ""
  let scoreSum = 0
  let hits = 0
  let prev = -1
  for (let t = 0; t < steps; t++) {
    const off = t * classes
    let best = 0
    let bestP = -1
    for (let c = 0; c < classes; c++) {
      const p = probs[off + c]
      if (p > bestP) {
        bestP = p
        best = c
      }
    }
    if (best !== CTC_BLANK_INDEX && best !== prev && best < chars.length) {
      text += chars[best]
      scoreSum += bestP
      hits++
    }
    prev = best
  }
  return { text, score: hits ? scoreSum / hits : 0 }
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v
}
