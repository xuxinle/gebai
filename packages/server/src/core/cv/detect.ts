/**
 * YOLO（ONNX 导出）前后处理纯函数：letterbox 前处理（等比缩放 + 居中填充）与
 * NMS 后处理。兼容 v8 系输出 [1, C, N]（框坐标 + 逐类得分，无 obj）与 v5 系输出
 * [1, N, 5+C]（x,y,w,h,obj + 逐类得分）两种形态，按形状自动识别。
 */
import { type Box, type RgbaImage, resizeBilinear, rgbaToCHW } from "./image"

/** letterbox 目标边长（YOLO 导出缺省 640）。 */
export const DETECT_SIZE = 640
/** letterbox 填充灰度（114/255，ultralytics 约定）。 */
const PAD_GRAY = 114
const DETECT_MEAN: [number, number, number] = [0, 0, 0]
const DETECT_STD: [number, number, number] = [1, 1, 1]
/** NMS IoU 阈值。 */
export const NMS_IOU = 0.45
/** 置信度缺省下限。 */
export const DETECT_CONF = 0.25

export interface DetectObject extends Box {
  label: string
  score: number
}

/** letterbox 前处理：等比缩放至 size×size 画布居中放置。scale/pad 供坐标还原。 */
export function letterbox(
  img: RgbaImage,
  size = DETECT_SIZE,
): { data: Float32Array; size: number; scale: number; padX: number; padY: number } {
  const scale = Math.min(size / img.width, size / img.height)
  const w = Math.max(1, Math.round(img.width * scale))
  const h = Math.max(1, Math.round(img.height * scale))
  const padX = ((size - w) / 2) | 0
  const padY = ((size - h) / 2) | 0
  const resized = resizeBilinear(img, w, h)
  const canvas: RgbaImage = { width: size, height: size, data: new Uint8ClampedArray(size * size * 4) }
  const g = PAD_GRAY
  for (let i = 0; i < size * size; i++) {
    canvas.data[i * 4] = canvas.data[i * 4 + 1] = canvas.data[i * 4 + 2] = g
    canvas.data[i * 4 + 3] = 255
  }
  for (let row = 0; row < h; row++) {
    canvas.data.set(resized.data.subarray(row * w * 4, (row + 1) * w * 4), ((padY + row) * size + padX) * 4)
  }
  return { data: rgbaToCHW(canvas, DETECT_MEAN, DETECT_STD), size, scale, padX, padY }
}

/**
 * YOLO 后处理：output 展平数组，dims 为模型输出形状 [1, a, b]。
 * v8 系 [1, 4+nc, N]（列为主序）；v5 系 [1, N, 5+nc]（行为主序，含 obj 分支）。
 * 返回坐标已还原到原图像素系的检测结果（按得分降序）。
 */
export function yoloPostprocess(
  output: Float32Array,
  dims: readonly number[],
  labels: string[],
  params: { srcW: number; srcH: number; scale: number; padX: number; padY: number; conf: number },
): DetectObject[] {
  const [a, b] = [dims[1] ?? 0, dims[2] ?? 0]
  if (a < 1 || b < 1) throw new Error(`YOLO 输出形状非法: [${dims.join(",")}]`)
  // v8：a = 4+nc（小），b = 候选数（大）；v5：a = 候选数（大），b = 5+nc（小）
  const isV8 = a < b
  const nc = isV8 ? a - 4 : b - 5
  if (nc < 1) throw new Error(`YOLO 类别数异常: ${nc}`)
  if (nc > labels.length) throw new Error(`YOLO 模型类别数 ${nc} 超过标签文件 ${labels.length} 行`)
  const candidates: Array<{ box: Box; label: string; score: number }> = []
  const n = isV8 ? b : a
  for (let i = 0; i < n; i++) {
    let cx: number
    let cy: number
    let w: number
    let h: number
    let bestCls = -1
    let bestScore = 0
    if (isV8) {
      cx = output[0 * n + i]
      cy = output[1 * n + i]
      w = output[2 * n + i]
      h = output[3 * n + i]
      for (let c = 0; c < nc; c++) {
        const s = output[(4 + c) * n + i]
        if (s > bestScore) {
          bestScore = s
          bestCls = c
        }
      }
    } else {
      const row = i * b
      cx = output[row]
      cy = output[row + 1]
      w = output[row + 2]
      h = output[row + 3]
      const obj = output[row + 4]
      for (let c = 0; c < nc; c++) {
        const s = obj * output[row + 5 + c]
        if (s > bestScore) {
          bestScore = s
          bestCls = c
        }
      }
    }
    if (bestScore < params.conf) continue
    // letterbox 坐标 → 原图坐标（去 padding、除缩放）
    const x1 = clamp((cx - w / 2 - params.padX) / params.scale, 0, params.srcW)
    const y1 = clamp((cy - h / 2 - params.padY) / params.scale, 0, params.srcH)
    const x2 = clamp((cx + w / 2 - params.padX) / params.scale, 0, params.srcW)
    const y2 = clamp((cy + h / 2 - params.padY) / params.scale, 0, params.srcH)
    if (x2 - x1 < 1 || y2 - y1 < 1) continue
    candidates.push({ box: { x: x1, y: y1, w: x2 - x1, h: y2 - y1 }, label: labels[bestCls] ?? `class_${bestCls}`, score: bestScore })
  }
  candidates.sort((p, q) => q.score - p.score)
  // 按类别分组 NMS（同类重叠去重，跨类保留）
  const out: DetectObject[] = []
  for (let i = 0; i < candidates.length; i++) {
    const c = candidates[i]
    let suppressed = false
    for (const kept of out) {
      if (kept.label === c.label && iou(kept, c.box) > NMS_IOU) {
        suppressed = true
        break
      }
    }
    if (!suppressed) out.push({ ...c, ...c.box })
  }
  return out
}

/** IoU（两轴对齐框交并比）。 */
export function iou(a: Box, b: Box): number {
  const x1 = Math.max(a.x, b.x)
  const y1 = Math.max(a.y, b.y)
  const x2 = Math.min(a.x + a.w, b.x + b.w)
  const y2 = Math.min(a.y + a.h, b.y + b.h)
  const inter = Math.max(0, x2 - x1) * Math.max(0, y2 - y1)
  const union = a.w * a.h + b.w * b.h - inter
  return union > 0 ? inter / union : 0
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v
}
