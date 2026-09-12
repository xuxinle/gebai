import { domToPng } from "modern-screenshot"

/** 捕获 html 上限（与服务端 PAGE_CAPTURE_HTML_LIMIT 对应）：超出截取首部，防 WS 传输过大。 */
export const CAPTURE_HTML_LIMIT = 300 * 1024
/** 截图输出长边上限（px）：超宽屏等比降采样，视觉模型分析足够。 */
export const CAPTURE_IMAGE_MAX_EDGE = 1600
/** 截图体积上限（字节）：超出降级 JPEG 重编码/缩放。 */
export const CAPTURE_IMAGE_MAX_BYTES = 2 * 1024 * 1024
/** 整页截图最大高度（px）：超出截取顶部（canvas 尺寸上限保护）。 */
export const CAPTURE_FULLPAGE_MAX_HEIGHT = 12000
/**
 * 单次截图纳入的节点数预算：modern-screenshot 逐节点计算样式并克隆，成本约 0.5ms/节点——
 * 长会话页面（数千至上万节点）下无预算的整页截图会把主线程**阻塞数秒**（实测 1500 条消息/7508 节点
 * 阻塞 3.9s，用户表现为「一用就卡死」）。配合可见区域过滤后，典型截图只纳入数十~数百节点；
 * 预算只在极端页面（超长历史 + 整页模式）下兜底，避免把页面卡死。
 */
export const CAPTURE_MAX_NODES = 3000
/** 截图等待上限（ms）：超时按「截图失败」处理并返回 html（前端不再无限等待；服务端 waitForCapture 30s 兜底）。 */
export const CAPTURE_SCREENSHOT_TIMEOUT_MS = 15_000

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
    // 截图失败（canvas 超限/浏览器限制/超时）不阻塞 html 捕获：模型仍可 read 分析
    console.warn("[capture] screenshot failed:", err)
  }
  return { html, imageBase64 }
}

/**
 * 截图节点过滤器（导出供测试）：减少纳入渲染的节点数是避免长会话卡死的关键（成本 ∝ 纳入节点数）。
 * - **排除已滚出捕获区域的子树**（矩形完全在区域上方或下方）——「排除节点即排除其子树」，
 *   长历史里绝大多数消息因此不入渲染；
 * - **节点预算**（`budget.left`）用尽后一律排除：极端页面（整页模式 + 超长历史）下的硬兜底；
 * - 非元素节点（文本等）不参与几何判定，直接纳入；无布局盒的元素（display:contents 等）不据此排除。
 */
export function makeCaptureFilter(regionTop: number, regionBottom: number, budget: { left: number }): (el: Node) => boolean {
  return (el: Node): boolean => {
    if (budget.left <= 0) return false
    if (typeof Element === "undefined" || !(el instanceof Element)) return true
    const rect = typeof el.getBoundingClientRect === "function" ? el.getBoundingClientRect() : null
    // 只有「非退化布局盒」才据矩形排除：宽或高为 0 的元素（wrapper/display:contents 等）不排除自身，
    // 否则会连带丢掉其可见子树（宁多纳入不误排除，退化盒的渲染成本极低）
    if (rect && rect.width > 0 && rect.height > 0) {
      if (rect.bottom <= regionTop) return false
      if (rect.top >= regionBottom) return false
    }
    budget.left--
    return true
  }
}

/** 让出一帧：等浏览器完成待处理渲染，避免在掉帧时刻叠加长任务（截图前调用）。 */
function nextFrame(): Promise<void> {
  return new Promise((r) => (typeof requestAnimationFrame === "function" ? requestAnimationFrame(() => r()) : setTimeout(r, 0)))
}

/** 超时兜底：截图内部不可取消，超时至少让调用方（与工具）及时返回，不无限等待。 */
function withTimeout<T>(p: Promise<T>, ms: number): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`截图超时（${ms}ms，页面过大或渲染繁忙）`)), ms)
    p.then(
      (v) => {
        clearTimeout(timer)
        resolve(v)
      },
      (e) => {
        clearTimeout(timer)
        reject(e)
      },
    )
  })
}

async function captureScreenshot(fullPage: boolean): Promise<string | undefined> {
  const root = document.documentElement
  const width = root.clientWidth
  // 视口模式：捕获当前可见区域；整页模式：从文档顶到高度上限
  const regionTop = fullPage ? 0 : Math.max(0, root.scrollTop ?? 0)
  const height = fullPage ? Math.min(root.scrollHeight, CAPTURE_FULLPAGE_MAX_HEIGHT) - regionTop : root.clientHeight
  if (width <= 0 || height <= 0) return undefined
  await nextFrame()
  const budget = { left: CAPTURE_MAX_NODES }
  const dataUrl = await withTimeout(
    domToPng(root, {
      width,
      height,
      // 输出长边 ≤ CAPTURE_IMAGE_MAX_EDGE（高分屏 2x 以内；超宽屏降采样）
      scale: Math.min(2, CAPTURE_IMAGE_MAX_EDGE / Math.max(width, height)),
      backgroundColor: getComputedStyle(root).backgroundColor || "#ffffff",
      style: fullPage ? { height: `${height}px`, overflow: "hidden" } : undefined,
      filter: makeCaptureFilter(regionTop, regionTop + height, budget),
    }),
    CAPTURE_SCREENSHOT_TIMEOUT_MS,
  )
  if (budget.left <= 0) console.warn(`[capture] 截图达到节点预算上限（${CAPTURE_MAX_NODES}），区域外/超出部分未入图（html 仍是完整 DOM）`)
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
