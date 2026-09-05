import type { ToolContext, ToolSet } from "../../core/base/types"
import { createLazyBridge, withSessionLock, type BridgeLike } from "../../core/browser/bridge"
import { cropImage, decodePng, type RgbaImage } from "../../core/cv/image"
import { createCvAnalysisTools, type CvSource } from "../../core/tools/cv-analysis"
import { parseRegion } from "../../core/tools/shared"

/**
 * playwright 子Agent 本地识别工具集（ocr/locate/locate_image）：复用 desktop 同款 core/cv
 * 小模型识别基建（共享工厂 core/tools/cv-analysis），缺省图像源改走共享桥接截取当前页面
 * 视口（固定文件复用）——坐标即视口像素系，可经 evaluate 的 document.elementFromPoint
 * 定位元素。浏览器是隔离环境：无 desktop 的本地模式闸门，服务端部署（沙箱）可用；
 * 定位 DOM 读不到的内容（canvas/WebGL 渲染文字、图片化文字/验证码、图形模板）时
 * 作为 content/选择器通道的兜底。
 */

function fail(err: unknown): string {
  return err instanceof Error ? err.message : String(err)
}

export function createPlaywrightCvTools(deps: { bridge?: BridgeLike } = {}): ToolSet {
  const bridge: BridgeLike = deps.bridge ?? createLazyBridge()
  const request = (sessionId: string, op: string, args: Record<string, unknown>): Promise<unknown> =>
    withSessionLock(sessionId, () => bridge.request(op, { sessionId, ...args }))

  /** 缺省识别源：当前页视口截图（与 playwright 工具同一桥接单例与会话锁）；region 在截图内
   *  裁剪、偏移回加——坐标一律映射回视口像素系（elementFromPoint 直接可用）。 */
  const capturePage = async (ctx: ToolContext, region: string): Promise<CvSource | { error: string }> => {
    const abs = ctx.resolvePath("tmp/pw_cv_capture.png")
    try {
      await request(ctx.sessionId, "screenshot", { path: abs, fullPage: false, selector: "" })
    } catch (err) {
      return { error: `页面截图失败: ${fail(err)}（先 open 打开页面后重试）` }
    }
    let img: RgbaImage
    try {
      img = decodePng(await ctx.readBinaryFile(abs))
    } catch (e) {
      return { error: `截图解码失败: ${fail(e)}` }
    }
    const box = region ? parseRegion(region) : null
    if (!box) return { img, offX: 0, offY: 0, sourceDesc: "当前页面截图（视口，坐标即视口像素）" }
    return { img: cropImage(img, box), offX: box.x, offY: box.y, sourceDesc: `当前页面截图（区域 ${region}，坐标已映射回视口像素系）` }
  }

  return createCvAnalysisTools({
    captureDefault: capturePage,
    pngOnlyTail: "，或直接省略 image 截取当前页面",
    wording: {
      descriptions: {
        ocr: "本地 OCR 识别当前页面/图片文字（PP-OCR 中英文小模型，离线运行、快、带精确视口坐标、不耗模型配额）。image 省略则截取当前页面（视口）；region 限定区域（'x,y,w,h'）；find 关键词过滤。canvas/WebGL 渲染文字、图片化文字/验证码等 DOM 读不到的内容用本工具（普通文本优先 content）；需整页时先 screenshot full_page 再传 image。",
        locate: "在页面截图/图片中定位目标文字的精确像素坐标（本地 OCR——canvas 等无 DOM 文本、选择器够不到时的兜底）。target 为要找的文字；返回最佳匹配的中心坐标（视口像素系）与全部候选。image 省略则截取当前页面（视口）；region 限定搜索区域。",
        locateImage: "在页面截图/图片中按模板定位图标/图形元素（本地模板匹配，零训练——验证图标/logo 是否真实渲染等）。template 为模板 PNG 路径，或 template_region 从搜索图坐标内取模板区域；返回最佳匹配中心坐标（视口像素系）与候选。同尺寸匹配（模板需与目标显示尺寸一致——宜从同一页面环境截图裁剪），threshold 相似度阈值默认 0.8。image 省略则截取当前页面（视口）；region 限定搜索区域。",
      },
      imageParam: {
        ocr: "可选：PNG 图片路径（相对会话工作目录，省略则截取当前页面）",
        locate: "可选：PNG 图片路径（省略则截取当前页面）",
        locateImage: "可选：PNG 搜索图路径（省略则截取当前页面）",
      },
      regionParam: {
        ocr: "可选：识别区域 'x,y,w,h'（像素；视口截图内/image 图片内坐标）",
        locate: "可选：搜索区域 'x,y,w,h'（像素）",
        locateImage: "可选：搜索区域 'x,y,w,h'（像素）",
      },
      templateRegionParam: "可选：'x,y,w,h' 在搜索图坐标系内取模板区域（与 template 二选一；坐标系同 playwright_ocr 对同一 image/region 的输出）",
      clickHint: (cx, cy) => `可经 evaluate 用 document.elementFromPoint(${cx}, ${cy}) 定位元素后操作（坐标为视口像素）`,
      locateNotFound:
        "1) 用 playwright_ocr 读取页面截图全部文字确认实际措辞；2) DOM 可达时优先 content/evaluate 按选择器或文本定位（更可靠）；3) 文字可能是图标/图形，改用 playwright_locate_image（模板匹配）或 vision 工具语义分析。",
      locateImageNotFound:
        "2) 确认模板与目标同尺寸（本工具不做缩放匹配，模板宜从同一页面环境截图裁剪）；3) 目标可能是文字——改用 playwright_locate；4) 用 vision 工具对截图做语义分析。",
    },
  })
}
