import { domToPng } from "modern-screenshot"

/** 捕获 html 上限（与服务端 PAGE_CAPTURE_HTML_LIMIT 对应）：超出截取首部，防 WS 传输过大。 */
export const CAPTURE_HTML_LIMIT = 300 * 1024
/** 截图输出长边上限（px）：超宽屏等比降采样，视觉模型分析足够。 */
export const CAPTURE_IMAGE_MAX_EDGE = 1600
/** 截图体积上限（字节）：超出降级 JPEG 重编码/缩放。 */
export const CAPTURE_IMAGE_MAX_BYTES = 2 * 1024 * 1024
/** 整页截图最大高度（px）：超出截取顶部（canvas 尺寸上限保护）。 */
export const CAPTURE_FULLPAGE_MAX_HEIGHT = 12000

export interface PageCapture {
  html: string
  /** 截图 data URL（png/jpeg）；截图失败或缺省时为 undefined。 */
  imageBase64?: string
}

/** 捕获当前页面：渲染后 DOM html（截断）+ 截图（体积受限）。截图失败不阻塞 html。delayMs 为捕获前等待（UI 操作/渲染完成后截图）。 */
export async function capturePage(opts: { fullPage?: boolean; delayMs?: number } = {}): Promise<PageCapture> {
  if (opts.delayMs) await new Promise((r) => setTimeout(r, Math.max(0, Math.min(10000, opts.delayMs!))))
  const html = document.documentElement.outerHTML.slice(0, CAPTURE_HTML_LIMIT)
  let imageBase64: string | undefined
  try {
    imageBase64 = await captureScreenshot(opts.fullPage ?? false)
  } catch (err) {
    // 截图失败（canvas 超限/浏览器限制）不阻塞 html 捕获：模型仍可 read 分析
    console.warn("[capture] screenshot failed:", err)
  }
  return { html, imageBase64 }
}

async function captureScreenshot(fullPage: boolean): Promise<string | undefined> {
  const root = document.documentElement
  const width = root.clientWidth
  const height = fullPage ? Math.min(root.scrollHeight, CAPTURE_FULLPAGE_MAX_HEIGHT) : root.clientHeight
  if (width <= 0 || height <= 0) return undefined
  const dataUrl = await domToPng(root, {
    width,
    height,
    // 输出长边 ≤ CAPTURE_IMAGE_MAX_EDGE（高分屏 2x 以内；超宽屏降采样）
    scale: Math.min(2, CAPTURE_IMAGE_MAX_EDGE / Math.max(width, height)),
    backgroundColor: getComputedStyle(root).backgroundColor || "#ffffff",
    style: fullPage ? { height: `${height}px`, overflow: "hidden" } : undefined,
  })
  return compressDataUrl(dataUrl)
}

/** 体积压缩：估算超限时先 JPEG 重编码，仍超限再等比缩放降质。 */
async function compressDataUrl(dataUrl: string): Promise<string> {
  if (dataUrl.length * 0.75 <= CAPTURE_IMAGE_MAX_BYTES) return dataUrl
  const img = await loadImage(dataUrl)
  let out = encodeJpeg(img, img.width, img.height, 0.85)
  if (out.length * 0.75 > CAPTURE_IMAGE_MAX_BYTES) {
    const scale = Math.min(1, (CAPTURE_IMAGE_MAX_BYTES / (out.length * 0.75)) ** 0.5)
    const w = Math.max(1, Math.round(img.width * scale))
    const h = Math.max(1, Math.round(img.height * scale))
    out = encodeJpeg(img, w, h, 0.8)
  }
  return out
}

function encodeJpeg(img: HTMLImageElement, w: number, h: number, quality: number): string {
  const canvas = document.createElement("canvas")
  canvas.width = w
  canvas.height = h
  const ctx = canvas.getContext("2d")
  if (!ctx) return img.src
  ctx.fillStyle = "#ffffff" // JPEG 无透明通道，白底兜底
  ctx.fillRect(0, 0, w, h)
  ctx.drawImage(img, 0, 0, w, h)
  return canvas.toDataURL("image/jpeg", quality)
}

function loadImage(src: string): Promise<HTMLImageElement> {
  return new Promise((resolve, reject) => {
    const img = new Image()
    img.onload = () => resolve(img)
    img.onerror = () => reject(new Error("截图重编码失败"))
    img.src = src
  })
}
