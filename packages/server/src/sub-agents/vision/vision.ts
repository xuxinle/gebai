/**
 * vision 视觉子代理：视觉能力的通用层收拢（图片输入型）——多模态语义分析（analyze）+
 * 本地小模型识别三件套（ocr/locate/locate_image）+ 本地目标检测（detect），全部只读、
 * 无环境闸门（沙箱可用）、零依赖。与其他子代理的复用边界（DESIGN「视觉能力分层与子代理复用边界」）：
 * - 通用图片能力（输入=图片路径，无环境耦合）收在此处——desktop/playwright/self_optimize 等
 *   经 dependencies: ["vision"] 依赖自动装载，以 vision_* 命名空间直接调用（复用方式一）；
 * - 环境耦合的缺省图像源便利（省略 image 现截屏幕/当前页视口、坐标映射回屏幕/视口像素系）
 *   留在 desktop/playwright 域内——自己的工具名（desktop_、playwright_ 前缀）+ cv-analysis 共享
 *   工厂注入各自缺省源/闸门/文案（复用方式二：实现共用、工具各归其域，语义不越界）。
 * 实现全部复用全局基建：analyze=makeVisionTool（视觉语义分析的唯一工具实现源，provider 解析经组装层
 * setVisionProviderGetter 注入）、ocr/locate/locate_image=createCvAnalysisTools、detect=createDetectTool
 * ——本子代理不重复实现，仅以 image 必填（无缺省源）形态提供通用入口；主会话 agent_load 装载，
 * 依赖方（dependencies 声明）装载/预加载时自动可用（新会话不继承全局工具也有视觉能力）。
 */
import type { SubAgentDef, ToolContext } from "../../core/base/types"
import { makeVisionTool, getVisionProvider } from "../../core/tools/vision"
import { createCvAnalysisTools, createDetectTool, type CvSource } from "../../core/tools/cv-analysis"
import { cropImage, decodePng, type RgbaImage } from "../../core/cv/image"
import { parseRegion } from "../../core/tools/shared"
import systemPromptBase from "./vision.md"

export const name = "vision"
export const description =
  "视觉能力（图片分析型，其他子代理可依赖）：把图片交给多模态模型语义分析（analyze——理解图像内容/布局含义/非文字元素），或本地小模型识别——OCR 读文字带精确坐标（ocr）、定位文字位置（locate）、图标/图形模板匹配（locate_image）、YOLO 目标检测（detect）。输入一律为图片文件路径（png；analyze 另支持 jpg/jpeg/gif/webp），无缺省图像源、不截图——需要现截屏幕/页面请用 desktop/playwright；全部只读免审批。"
export const systemPrompt = systemPromptBase

/** 图片加载（无缺省源：image 必填，region 在图片内裁剪、偏移回加——坐标即图片像素系）。 */
const loadImage = async (ctx: ToolContext, image: string, region: string): Promise<CvSource | { error: string }> => {
  let img: RgbaImage
  try {
    img = decodePng(await ctx.readBinaryFile(ctx.resolvePath(image)))
  } catch (e) {
    return { error: `图片读取/解码失败（${image}）: ${e instanceof Error ? e.message : e}——本工具仅支持 PNG 且 image 必填（无缺省图像源，需要现截屏幕/页面用 desktop/playwright 子代理）` }
  }
  const box = region ? parseRegion(region) : null
  if (region && !box) return { error: `region 格式错误: ${region}（应为 x,y,w,h）` }
  if (!box) return { img, offX: 0, offY: 0, sourceDesc: `图像 ${image}` }
  return { img: cropImage(img, box), offX: box.x, offY: box.y, sourceDesc: `图像 ${image}（区域 ${region}，坐标已映射回图片像素系）` }
}

const cv = createCvAnalysisTools({
  captureDefault: (ctx, region) => loadImage(ctx, "", region),
  pngOnlyTail: "",
  wording: {
    descriptions: {
      ocr: "本地 OCR 识别图片文字（PP-OCR 中英文小模型，离线运行、快、带图片像素坐标、不耗模型配额）。image 为 PNG 图片路径（必填）；region 限定区域（'x,y,w,h'，图片内坐标）；find 关键词过滤。返回每行文字与图片像素坐标、置信度。",
      locate: "在图片中定位目标文字的精确像素坐标（本地 OCR）。target 为要找的文字；image 为 PNG 图片路径（必填）；region 限定搜索区域。返回最佳匹配的中心坐标（图片像素系）与全部候选。",
      locateImage: "在图片中按模板定位图标/图形元素（本地模板匹配，零训练）。template 为模板 PNG 路径，或 template_region 从搜索图坐标内取模板区域；同尺寸匹配（模板需与目标显示尺寸一致）。image 为搜索 PNG 图片路径（必填）；region 限定搜索区域。",
    },
    imageParam: {
      ocr: "PNG 图片路径（相对会话工作目录，必填——本工具无缺省图像源）",
      locate: "PNG 图片路径（相对会话工作目录，必填）",
      locateImage: "PNG 搜索图路径（相对会话工作目录，必填）",
    },
    regionParam: {
      ocr: "可选：识别区域 'x,y,w,h'（图片内像素坐标）",
      locate: "可选：搜索区域 'x,y,w,h'（图片内像素坐标）",
      locateImage: "可选：搜索区域 'x,y,w,h'（图片内像素坐标）",
    },
    templateRegionParam: "可选：'x,y,w,h' 在搜索图坐标系内取模板区域（与 template 二选一；坐标系同 vision_ocr 对同一 image/region 的输出）",
    clickHint: (cx, cy) => `坐标 (${cx}, ${cy}) 为图片像素系——消费方自行映射到目标环境（屏幕经 desktop 子代理加窗口原点、页面经 playwright elementFromPoint）`,
    locateNotFound:
      "1) 用 vision_ocr 读取图片全部文字确认实际措辞；2) 文字可能是图标/图形，改用 vision_locate_image（模板匹配）；3) 需语义理解（图像内容/布局含义）改用 vision_analyze。",
    locateImageNotFound:
      "2) 确认模板与目标同尺寸（本工具不做缩放匹配，模板宜从同一环境截图裁剪）；3) 目标可能是文字——改用 vision_locate；4) 用 vision_analyze 对图片做语义分析。",
  },
})

const detect = createDetectTool({
  captureDefault: async () => ({ error: "本工具无缺省图像源——image 参数必填" }),
  pngOnlyTail: "",
  wording: {
    description:
      "本地目标检测（自备 YOLO ONNX 模型：GEBAI_CV_DETECT_MODEL 指定，或放入 {GEBAI_HOME}/models/detect/ 唯一 .onnx 自动生效；ultralytics 导出的 ONNX 自动读取内嵌输入尺寸与类别，免标签配置）。返回检测对象类别与图片像素坐标，并默认与 OCR 配对输出每框文本（pair_text 可关）。image 为 PNG 图片路径（必填——本工具无缺省图像源，需要现截屏幕/页面用 desktop/playwright 子代理）；conf 置信度阈值（默认 0.25）；iou NMS 阈值（默认 0.45，密集小控件场景建议 0.1）。推理分层：GPU sidecar（GEBAI_CV_DETECT_BACKEND，Windows DirectML/任意 DX12 显卡、CUDA、macOS CoreML）不可用时自动回落 wasm CPU。",
    imageParam: "PNG 图片路径（相对会话工作目录，必填——本工具无缺省图像源）",
    notFoundTail: "可尝试降低 conf 阈值，或确认模型/标签与场景匹配；需语义理解改用 vision_analyze。",
  },
})

export const tools = { analyze: makeVisionTool({ vision: getVisionProvider, name: "analyze" }), ...cv, detect }
export const requiresApproval = {}
export const preload = false

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
