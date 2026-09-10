/**
 * 面向视觉模型传输的图片压缩（Bun.Image 原生编解码，零外部依赖）：图片按原图保存在会话 tmp/
 * （DESIGN「多模态支持」），仅在发送给大模型前压缩——多模态内联（引擎附件/工具结果图片）与
 * vision 子代理 analyze 共用。超出阈值（长边/体积）才压缩，未超限/不支持的输入原样返回
 * （resized=false），调用方直用原 buf。GIF 为动画帧格式，重编码会丢帧，一律不压缩。
 */

/** 压缩阈值：长边上限（px）与体积上限（字节），JPEG/WebP 重编码质量。 */
export const VISION_RESIZE_MAX_EDGE = 1280
export const VISION_RESIZE_MAX_BYTES = 2 * 1024 * 1024
export const VISION_RESIZE_QUALITY = 0.85

export interface ResizedImage {
  buf: Buffer
  mime: string
  /** 压缩后尺寸；未压缩时即原图尺寸（解码失败时为 0）。 */
  width: number
  height: number
  resized: boolean
  /** 原始尺寸与体积（解码失败时尺寸为 0）。 */
  origWidth: number
  origHeight: number
  origBytes: number
}

/** 读取图像宽高（PNG 免解码读 IHDR；其余格式经 Bun.Image 元数据，失败返回 null）。 */
export async function imageSize(bytes: Uint8Array): Promise<{ width: number; height: number } | null> {
  if (bytes.length >= 24 && bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47) {
    const width = ((bytes[16] << 24) | (bytes[17] << 16) | (bytes[18] << 8) | bytes[19]) >>> 0
    const height = ((bytes[20] << 24) | (bytes[21] << 16) | (bytes[22] << 8) | bytes[23]) >>> 0
    if (width > 0 && height > 0) return { width, height }
  }
  try {
    const meta = await new Bun.Image(bytes).metadata()
    return meta.width > 0 && meta.height > 0 ? { width: meta.width, height: meta.height } : null
  } catch {
    return null
  }
}

export function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(0)}KB`
  return `${(n / 1024 / 1024).toFixed(1)}MB`
}

/** 压缩说明文案（原始 → 压缩后，含未压缩标注；尺寸未知时省略宽高）：附在图片块旁供模型感知精度与坐标缩放比。 */
export function resizeNote(r: ResizedImage): string {
  const origSize = formatBytes(r.origBytes)
  const origDims = r.origWidth ? `${r.origWidth}×${r.origHeight} ` : ""
  if (!r.resized) return `[图片 ${origDims}${origSize}，未压缩]`
  return `[图片已压缩: 原始 ${r.origWidth}×${r.origHeight} ${origSize} → ${r.width}×${r.height} ${formatBytes(r.buf.length)}（模型看到的图片为压缩后尺寸，坐标/细节按比例对应原图）]`
}

/** 按格式重编码（同格式输出；质量仅对 jpeg/webp 生效——png 无损压缩无质量参数）。 */
async function encodeByMime(resized: Bun.Image, mime: string): Promise<Buffer> {
  if (mime === "image/jpeg") return resized.jpeg({ quality: Math.round(VISION_RESIZE_QUALITY * 100) }).toBuffer()
  if (mime === "image/webp") return resized.webp({ quality: Math.round(VISION_RESIZE_QUALITY * 100) }).toBuffer()
  return resized.png().toBuffer()
}

/**
 * 视觉传输压缩：非 GIF 且（长边 > MAX_EDGE 或体积 > MAX_BYTES）时等比缩放（长边 = MAX_EDGE，
 * 体积仍超限时按 0.8 倍迭代缩小，下限 0.25 倍）后按原格式重编码；其余原样返回（resized=false，
 * 尺寸已解析时携带）。解码/编码失败不抛错（返回原图，调用方沿用既有 8MB 硬限逻辑兜底）。
 */
export async function resizeForVision(bytes: Uint8Array, mime: string): Promise<ResizedImage> {
  const asBuf = Buffer.from(bytes)
  let width = 0
  let height = 0
  let img: Bun.Image
  try {
    img = new Bun.Image(asBuf)
    const meta = await img.metadata()
    width = meta.width
    height = meta.height
  } catch {
    return { buf: asBuf, mime, width: 0, height: 0, resized: false, origWidth: 0, origHeight: 0, origBytes: asBuf.length }
  }
  if (!width || !height) return { buf: asBuf, mime, width: 0, height: 0, resized: false, origWidth: 0, origHeight: 0, origBytes: asBuf.length }
  const longEdge = Math.max(width, height)
  if (mime === "image/gif" || (longEdge <= VISION_RESIZE_MAX_EDGE && asBuf.length <= VISION_RESIZE_MAX_BYTES)) {
    return { buf: asBuf, mime, width, height, resized: false, origWidth: width, origHeight: height, origBytes: asBuf.length }
  }
  let scale = Math.min(1, VISION_RESIZE_MAX_EDGE / longEdge)
  let last: { buf: Buffer; target: number } | null = null
  for (let i = 0; i < 6 && scale >= 0.25 - 1e-9; i++) {
    try {
      const target = Math.max(1, Math.round(longEdge * scale))
      const buf = await encodeByMime(img.resize(target), mime)
      last = { buf, target }
      if (buf.length <= VISION_RESIZE_MAX_BYTES) break
    } catch {
      return { buf: asBuf, mime, width, height, resized: false, origWidth: width, origHeight: height, origBytes: asBuf.length }
    }
    scale *= 0.8
  }
  if (!last) return { buf: asBuf, mime, width, height, resized: false, origWidth: width, origHeight: height, origBytes: asBuf.length }
  const ratio = last.target / longEdge
  return {
    buf: last.buf,
    mime,
    width: Math.max(1, Math.round(width * ratio)),
    height: Math.max(1, Math.round(height * ratio)),
    resized: true,
    origWidth: width,
    origHeight: height,
    origBytes: asBuf.length,
  }
}
