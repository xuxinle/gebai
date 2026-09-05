/**
 * 图像纯函数层（core/cv）：PNG 解码（node:zlib inflate + 逆滤波）、裁剪、双线性缩放、
 * RGBA→CHW float 张量。全部纯函数、零外部依赖，供本地 CV 推理（OCR/YOLO）前处理使用。
 * 覆盖截图场景的 PNG 子集：8-bit、非隔行、灰度/RGB/调色板/带 alpha 变体。
 */
import { inflateSync } from "node:zlib"

export interface RgbaImage {
  width: number
  height: number
  /** RGBA 逐像素交错，长度 = width * height * 4 */
  data: Uint8ClampedArray
}

export interface Box {
  x: number
  y: number
  w: number
  h: number
}

const PNG_SIGNATURE = [137, 80, 78, 71, 13, 10, 26, 10]

/** 解码 PNG → RGBA。不支持的形态（16-bit/隔行/截断/非 PNG）抛中文错误。 */
export function decodePng(bytes: Uint8Array): RgbaImage {
  if (bytes.length < 8 || !PNG_SIGNATURE.every((b, i) => bytes[i] === b)) {
    throw new Error("非 PNG 图像（当前本地识别仅支持 PNG，可用 desktop_screenshot 重新截取）")
  }
  let off = 8
  let width = 0
  let height = 0
  let bitDepth = 0
  let colorType = -1
  let interlace = 0
  let palette: Uint8Array | null = null
  let paletteAlpha: Uint8Array | null = null
  const idat: Uint8Array[] = []
  while (off + 8 <= bytes.length) {
    const len = readU32(bytes, off)
    const type = String.fromCharCode(bytes[off + 4], bytes[off + 5], bytes[off + 6], bytes[off + 7])
    const data = bytes.subarray(off + 8, off + 8 + len)
    if (off + 12 + len > bytes.length) throw new Error("PNG 数据截断（chunk 不完整）")
    if (type === "IHDR") {
      width = readU32(data, 0)
      height = readU32(data, 4)
      bitDepth = data[8]
      colorType = data[9]
      interlace = data[12]
    } else if (type === "PLTE") {
      palette = data.slice()
    } else if (type === "tRNS") {
      paletteAlpha = data.slice()
    } else if (type === "IDAT") {
      idat.push(data)
    } else if (type === "IEND") {
      break
    }
    off += 12 + len
  }
  if (!width || !height) throw new Error("PNG 缺少 IHDR")
  if (bitDepth !== 8) throw new Error(`PNG 仅支持 8-bit 位深（当前 ${bitDepth}）`)
  if (interlace !== 0) throw new Error("PNG 仅支持非隔行扫描（Adam7 隔行不支持）")
  const channels = CHANNELS[colorType]
  if (!channels) throw new Error(`PNG 不支持的颜色类型: ${colorType}`)
  if (colorType === 3 && !palette) throw new Error("PNG 调色板缺失（PLTE）")
  const raw = inflateSync(Buffer.concat(idat.map((c) => Buffer.from(c))))
  const stride = width * channels
  const expected = (stride + 1) * height
  if (raw.length < expected) throw new Error(`PNG 像素数据不完整（${raw.length} < ${expected}）`)
  unfilter(raw, width, height, channels)
  const out = new Uint8ClampedArray(width * height * 4)
  for (let y = 0; y < height; y++) {
    const lineOff = y * (stride + 1) + 1 // 跳过行首滤波字节
    for (let x = 0; x < width; x++) {
      const p = lineOff + x * channels
      const o = (y * width + x) * 4
    switch (colorType) {
      case 0: {
        const v = raw[p]
        out[o] = out[o + 1] = out[o + 2] = v
        out[o + 3] = 255
        break
      }
      case 2:
        out[o] = raw[p]
        out[o + 1] = raw[p + 1]
        out[o + 2] = raw[p + 2]
        out[o + 3] = 255
        break
      case 3: {
        const idx = raw[p] * 3
        out[o] = palette![idx]
        out[o + 1] = palette![idx + 1]
        out[o + 2] = palette![idx + 2]
        out[o + 3] = paletteAlpha && raw[p] < paletteAlpha.length ? paletteAlpha[raw[p]] : 255
        break
      }
      case 4: {
        const v = raw[p]
        out[o] = out[o + 1] = out[o + 2] = v
        out[o + 3] = raw[p + 1]
        break
      }
      case 6:
        out[o] = raw[p]
        out[o + 1] = raw[p + 1]
        out[o + 2] = raw[p + 2]
        out[o + 3] = raw[p + 3]
        break
    }
    }
  }
  return { width, height, data: out }
}

const CHANNELS: Record<number, number> = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }

function readU32(b: Uint8Array, off: number): number {
  return ((b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]) >>> 0
}

/** 逐行逆滤波（None/Sub/Up/Average/Paeth），原地写回 raw。 */
function unfilter(raw: Buffer, width: number, height: number, channels: number): void {
  const stride = width * channels
  let prev = 0 // 上一行首字节偏移（0 = 首行，无上邻）
  for (let y = 0; y < height; y++) {
    const line = y * (stride + 1)
    const filter = raw[line]
    const start = line + 1
    for (let i = 0; i < stride; i++) {
      const cur = start + i
      const left = i >= channels ? raw[cur - channels] : 0
      const up = prev ? raw[prev + i] : 0
      const upLeft = prev && i >= channels ? raw[prev + i - channels] : 0
      let v = raw[cur]
      if (filter === 1) v += left
      else if (filter === 2) v += up
      else if (filter === 3) v += (left + up) >> 1
      else if (filter === 4) v += paeth(left, up, upLeft)
      else if (filter !== 0) throw new Error(`PNG 未知滤波类型: ${filter}`)
      raw[cur] = v & 0xff
    }
    prev = start
  }
}

function paeth(a: number, b: number, c: number): number {
  const p = a + b - c
  const pa = Math.abs(p - a)
  const pb = Math.abs(p - b)
  const pc = Math.abs(p - c)
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c
}

/** 裁剪（越界部分被钳制到图像边界，w/h 钳制非负）。 */
export function cropImage(img: RgbaImage, box: Box): RgbaImage {
  const x = Math.max(0, Math.min(Math.round(box.x), img.width - 1))
  const y = Math.max(0, Math.min(Math.round(box.y), img.height - 1))
  const w = Math.max(1, Math.min(Math.round(box.w), img.width - x))
  const h = Math.max(1, Math.min(Math.round(box.h), img.height - y))
  const data = new Uint8ClampedArray(w * h * 4)
  for (let row = 0; row < h; row++) {
    const src = ((y + row) * img.width + x) * 4
    data.set(img.data.subarray(src, src + w * 4), row * w * 4)
  }
  return { width: w, height: h, data }
}

/** 双线性缩放（RGBA 逐通道独立插值）。 */
export function resizeBilinear(img: RgbaImage, width: number, height: number): RgbaImage {
  if (width < 1 || height < 1) throw new Error("缩放目标尺寸非法")
  if (img.width === width && img.height === height) return img
  const out = new Uint8ClampedArray(width * height * 4)
  const sx = img.width / width
  const sy = img.height / height
  for (let y = 0; y < height; y++) {
    const fy = Math.min((y + 0.5) * sy - 0.5, img.height - 1)
    const y0 = Math.max(0, Math.floor(fy))
    const y1 = Math.min(y0 + 1, img.height - 1)
    const wy = fy - y0
    for (let x = 0; x < width; x++) {
      const fx = Math.min((x + 0.5) * sx - 0.5, img.width - 1)
      const x0 = Math.max(0, Math.floor(fx))
      const x1 = Math.min(x0 + 1, img.width - 1)
      const wx = fx - x0
      const o = (y * width + x) * 4
      for (let c = 0; c < 4; c++) {
        const p00 = img.data[(y0 * img.width + x0) * 4 + c]
        const p01 = img.data[(y0 * img.width + x1) * 4 + c]
        const p10 = img.data[(y1 * img.width + x0) * 4 + c]
        const p11 = img.data[(y1 * img.width + x1) * 4 + c]
        out[o + c] = p00 * (1 - wx) * (1 - wy) + p01 * wx * (1 - wy) + p10 * (1 - wx) * wy + p11 * wx * wy
      }
    }
  }
  return { width, height, data: out }
}

/**
 * RGBA → NCHW float32 张量（RGB 三通道，丢弃 alpha），归一化 (px/255 - mean) / std。
 * mean/std 长度 3，输出长度 = width * height * 3，布局 [c][y][x]。
 */
export function rgbaToCHW(
  img: RgbaImage,
  mean: [number, number, number],
  std: [number, number, number],
): Float32Array {
  const n = img.width * img.height
  const out = new Float32Array(n * 3)
  for (let c = 0; c < 3; c++) {
    const plane = c * n
    for (let i = 0; i < n; i++) {
      out[plane + i] = (img.data[i * 4 + c] / 255 - mean[c]) / std[c]
    }
  }
  return out
}
