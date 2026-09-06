/**
 * 本地识别三工具共享工厂（ocr/locate/locate_image）：core/cv 小模型推理（OCR + 模板匹配）
 * 的消费层。desktop（缺省图像源=现截宿主机屏幕、入口闸门=本地模式）与 playwright（缺省
 * 图像源=共享桥接截当前页视口、无闸门沙箱可用）注入各自的图像来源与文案，复用同一套
 * 识别、坐标映射与输出逻辑。全部只读操作（不点击/不输入）；坐标一律映射回缺省源的原始
 * 像素系（源偏移 offX/offY 回加），消费提示由 clickHint 注入（desktop=mouse_click，
 * playwright=evaluate elementFromPoint）。
 */
import type { Tool, ToolContext, ToolResult } from "../base/types"
import { getCvRunner } from "../cv/cv"
import { pairObjectsWithText } from "../cv/detect"
import { cropImage, decodePng, type RgbaImage } from "../cv/image"
import { matchTemplate, type TemplateMatch } from "../cv/template"
import { parseRegion, schema } from "./shared"
import { VISION_MAX_IMAGE_BYTES } from "./vision"

/** 识别图像源：img 为待识别图（region 场景为裁剪后），offX/offY 为坐标映射回原始像素系的偏移。 */
export interface CvSource {
  img: RgbaImage
  offX: number
  offY: number
  sourceDesc: string
}

/** 缺省图像源装载约定（image 参数省略时）。 */
export interface CvSourceLoader {
  /** 现取缺省识别源（现截屏幕/当前页截图等）；region 原样传入（已过格式校验），由实现自行解析 */
  captureDefault: (ctx: ToolContext, region: string) => Promise<CvSource | { error: string }>
  /** 非 PNG 报错的消费方尾巴（如「，或直接省略 image 现截屏幕」） */
  pngOnlyTail: string
}

/** 文案注入：描述/参数描述/输出提示按消费方命名空间与坐标系定制。 */
export interface CvAnalysisWording {
  descriptions: { ocr: string; locate: string; locateImage: string }
  imageParam: { ocr: string; locate: string; locateImage: string }
  regionParam: { ocr: string; locate: string; locateImage: string }
  templateRegionParam: string
  clickHint: (cx: number, cy: number) => string
  locateNotFound: string
  locateImageNotFound: string
}

export interface CvAnalysisOptions extends CvSourceLoader {
  /** 入口守卫（desktop 本地模式闸门；省略=无条件放行） */
  gate?: (ctx: ToolContext) => void
  wording: CvAnalysisWording
}

/** OCR 结果行数上限（超出截断提示用 find 过滤收窄）。 */
const OCR_LINE_LIMIT = 200

interface LineOut {
  text: string
  score: number
  x: number
  y: number
  w: number
  h: number
}

/** 载入待识别图像：image 参数优先（PNG），省略走 captureDefault 现取缺省源；
 *  坐标输出偏移=源偏移（image+region 为图片内区域原点，缺省源由装载方给定）。 */
export async function loadAnalysisSource(
  ctx: ToolContext,
  loader: CvSourceLoader,
  image: string,
  region: string,
): Promise<CvSource | { error: string }> {
  const parsed = region ? parseRegion(region) : null
  if (region && !parsed) {
    return { error: `region 格式错误: ${region}（应为 x,y,w,h）` }
  }
  if (image) {
    const path = ctx.resolvePath(image)
    if (!path.toLowerCase().endsWith(".png")) {
      return { error: `本地识别仅支持 PNG（当前 ${image}）——JPEG 等格式请先转换${loader.pngOnlyTail}` }
    }
    const bytes = await ctx.readBinaryFile(path)
    if (bytes.byteLength > VISION_MAX_IMAGE_BYTES) {
      return { error: `图片过大（${(bytes.byteLength / 1024 / 1024).toFixed(1)}MB，上限 8MB）` }
    }
    let img: RgbaImage
    try {
      img = decodePng(bytes)
    } catch (e) {
      return { error: `PNG 解码失败: ${e instanceof Error ? e.message : e}` }
    }
    if (parsed) {
      // image 是完整图：region 在图内裁剪（坐标为图片内坐标）
      return { img: cropImage(img, parsed), offX: parsed.x, offY: parsed.y, sourceDesc: `图像 ${image}（区域 ${region}）` }
    }
    return { img, offX: 0, offY: 0, sourceDesc: `图像 ${image}` }
  }
  return loader.captureDefault(ctx, region)
}

function mapLines(lines: Array<{ text: string; score: number; box: { x: number; y: number; w: number; h: number } }>, offX: number, offY: number): LineOut[] {
  return lines.map((l) => ({
    text: l.text,
    score: l.score,
    x: Math.round(l.box.x + offX),
    y: Math.round(l.box.y + offY),
    w: Math.round(l.box.w),
    h: Math.round(l.box.h),
  }))
}

function formatLine(l: LineOut): string {
  const cx = Math.round(l.x + l.w / 2)
  const cy = Math.round(l.y + l.h / 2)
  return `${l.text}  [${l.x},${l.y},${l.w},${l.h}] → 中心 (${cx},${cy})  置信度 ${l.score.toFixed(2)}`
}

function normText(s: string): string {
  return s.trim().toLowerCase()
}

/** 匹配分级：2=完全相等，1=行包含目标，0=目标包含行（≥2 字），-1=不匹配。 */
function matchRank(line: string, target: string): number {
  const l = normText(line)
  const t = normText(target)
  if (!l || !t) return -1
  if (l === t) return 2
  if (l.includes(t)) return 1
  if (t.includes(l) && l.length >= 2) return 0
  return -1
}

/** detect 共享工厂注入文案：描述/参数描述与坐标语义按消费方（缺省源坐标系）定制。 */
export interface CvDetectWording {
  /** 工具描述（YOLO 自备模型/推理分层与通用说明不变，缺省源描述在此定制）。 */
  description: string
  /** image 参数描述（缺省源语义定制，如「省略则现截全屏」/「必填」）。 */
  imageParam: string
  /** 未检到时的建议尾巴（消费方坐标系与替代通道）。 */
  notFoundTail: string
}

/** detect 共享工厂入参：CvSourceLoader（缺省图像源）+ gate（入口守卫）+ wording（文案）。
 *  image 可选性由消费方定义：desktop=省略现截全屏，vision 子代理=image 必填。 */
export interface CvDetectOptions extends CvSourceLoader {
  gate?: (ctx: ToolContext) => void
  wording: CvDetectWording
}

/** 本地目标检测（YOLO ONNX）+ OCR 配对工具共享工厂：核心/cv 推理（getCvRunner().detect +
 *  pairObjectsWithText 配对补齐语义）、坐标偏移回加与输出格式化统一在此，消费方（desktop 域内
 *  缺省源=现截屏幕、vision 子代理通用图片）注入图像源/闸门/文案——识别能力复用，坐标语义各归其域。 */
export function createDetectTool(opts: CvDetectOptions): Tool {
  const { gate, wording } = opts
  const requireImage = !wording.imageParam.includes("可选")
  return {
    name: "detect",
    description: wording.description,
    card: { titleParams: ["image", "region"], args: "none" },
    parameters: schema(
      {
        image: { type: "string", description: wording.imageParam },
        region: { type: "string", description: "可选：检测区域 'x,y,w,h'（像素，相对 image 图片/缺省源坐标系）" },
        conf: { type: "number", description: "可选：置信度阈值（默认 0.25）" },
        iou: { type: "number", description: "可选：NMS IoU 阈值（默认 0.45；密集小控件/重叠元素场景调低至 0.1）" },
        pair_text: { type: "boolean", description: "可选：检测框与 OCR 文本配对输出「类别+文本」（默认 true；纯检测提速可关）" },
      },
      requireImage ? ["image"] : [],
    ),
    async execute(args, ctx): Promise<ToolResult> {
      gate?.(ctx)
      const image = String(args.image ?? "").trim()
      if (requireImage && !image) return { output: "缺少 image 参数（待检测图片路径，PNG——本工具无缺省图像源，需显式指定）" }
      const loaded = await loadAnalysisSource(ctx, opts, image, String(args.region ?? "").trim())
      if ("error" in loaded) return { output: loaded.error }
      const conf = Number(args.conf)
      const iou = Number(args.iou)
      let outcome: { objects: Array<{ label: string; score: number; x: number; y: number; w: number; h: number }>; backend: string }
      try {
        outcome = await getCvRunner().detect(loaded.img, {
          env: ctx.env,
          conf: Number.isFinite(conf) && conf > 0 && conf < 1 ? conf : 0.25,
          iou: Number.isFinite(iou) && iou > 0 && iou < 1 ? iou : undefined,
        })
      } catch (e) {
        return { output: `目标检测失败: ${e instanceof Error ? e.message : e}` }
      }
      // 检测框 × OCR 行配对（配对在图像像素系进行，再统一加偏移）：检测只给组件类别，
      // 配对文本补齐「哪一个按钮/输入框」的语义（完整屏幕解析的本地拼装）
      let pairedTexts: Array<string | undefined> = outcome.objects.map(() => undefined)
      if (args.pair_text !== false && outcome.objects.length) {
        try {
          const lines = (await getCvRunner().ocr(loaded.img, { env: ctx.env })).lines
          pairedTexts = pairObjectsWithText(outcome.objects, lines.map((l) => ({ text: l.text, ...l.box }))).map((o) => o.text)
        } catch { /* OCR 模型未配置等——跳过配对，仅输出检测框 */ }
      }
      const objects = outcome.objects.map((o, i) => ({
        label: o.label,
        score: o.score,
        x: Math.round(o.x + loaded.offX),
        y: Math.round(o.y + loaded.offY),
        w: Math.round(o.w),
        h: Math.round(o.h),
        text: pairedTexts[i],
      }))
      if (!objects.length) {
        return {
          output: `未检测到目标对象（${loaded.sourceDesc}）。${wording.notFoundTail}`,
          data: { objects, backend: outcome.backend },
        }
      }
      const body = objects
        .map((o) => {
          const cx = Math.round(o.x + o.w / 2)
          const cy = Math.round(o.y + o.h / 2)
          return `${o.label}  [${o.x},${o.y},${o.w},${o.h}] → 中心 (${cx},${cy})  置信度 ${o.score.toFixed(2)}${o.text ? `  文本: ${o.text}` : ""}`
        })
        .join("\n")
      return {
        output: `检测到 ${objects.length} 个对象（${loaded.sourceDesc}，坐标相对其原点；后端 ${outcome.backend}）：\n${body}`,
        data: { source: loaded.sourceDesc, backend: outcome.backend, objects },
      }
    },
  }
}

export function createCvAnalysisTools(opts: CvAnalysisOptions): { ocr: Tool; locate: Tool; locate_image: Tool } {
  const { gate, wording } = opts
  const load = (ctx: ToolContext, image: string, region: string) => loadAnalysisSource(ctx, opts, image, region)

  const ocr: Tool = {
    name: "ocr",
    description: wording.descriptions.ocr,
    card: { titleParams: ["find", "region"], args: "none" },
    parameters: schema({
      image: { type: "string", description: wording.imageParam.ocr },
      region: { type: "string", description: wording.regionParam.ocr },
      find: { type: "string", description: "可选：关键词过滤，只返回包含该关键词的文本行" },
    }),
    async execute(args, ctx): Promise<ToolResult> {
      gate?.(ctx)
      const loaded = await load(ctx, String(args.image ?? "").trim(), String(args.region ?? "").trim())
      if ("error" in loaded) return { output: loaded.error }
      const find = String(args.find ?? "").trim().toLowerCase()
      let lines: LineOut[]
      let backend = ""
      try {
        const raw = await getCvRunner().ocr(loaded.img, { env: ctx.env })
        lines = mapLines(raw.lines, loaded.offX, loaded.offY)
        backend = raw.backend
      } catch (e) {
        return { output: `本地识别失败: ${e instanceof Error ? e.message : e}` }
      }
      if (find) lines = lines.filter((l) => l.text.toLowerCase().includes(find))
      const capped = lines.length > OCR_LINE_LIMIT
      const shown = lines.slice(0, OCR_LINE_LIMIT)
      const head = `识别到 ${lines.length} 行（${loaded.sourceDesc}，坐标相对其原点；后端 ${backend}）${find ? `，过滤「${args.find}」` : ""}：`
      const body = shown.map(formatLine).join("\n")
      const tail = capped ? `\n…（共 ${lines.length} 行，仅显示前 ${OCR_LINE_LIMIT} 行；可用 find 参数过滤收窄）` : ""
      return {
        output: lines.length ? `${head}\n${body}${tail}` : `${head}\n（无${find ? "匹配" : "识别到"}文本——可能是图形界面无文字、分辨率过低，或需用视觉子代理（vision_analyze）做语义分析）`,
        data: { source: loaded.sourceDesc, backend, find: args.find ?? null, lines },
      }
    },
  }

  const locate: Tool = {
    name: "locate",
    description: wording.descriptions.locate,
    card: { titleParams: ["target", "region"], args: "none" },
    parameters: schema(
      {
        target: { type: "string", description: "要定位的目标文字（如「保存」「确定」）" },
        image: { type: "string", description: wording.imageParam.locate },
        region: { type: "string", description: wording.regionParam.locate },
      },
      ["target"],
    ),
    async execute(args, ctx): Promise<ToolResult> {
      gate?.(ctx)
      const target = String(args.target ?? "").trim()
      if (!target) return { output: "target 不能为空" }
      const loaded = await load(ctx, String(args.image ?? "").trim(), String(args.region ?? "").trim())
      if ("error" in loaded) return { output: loaded.error }
      let lines: LineOut[]
      try {
        lines = mapLines((await getCvRunner().ocr(loaded.img, { env: ctx.env })).lines, loaded.offX, loaded.offY)
      } catch (e) {
        return { output: `本地识别失败: ${e instanceof Error ? e.message : e}` }
      }
      const candidates = lines
        .map((l) => ({ line: l, rank: matchRank(l.text, target) }))
        .filter((c) => c.rank >= 0)
        .sort((a, b) => b.rank - a.rank || b.line.score - a.line.score)
        .map((c) => c.line)
      if (!candidates.length) {
        return {
          output: `未找到目标文字「${target}」（${loaded.sourceDesc}）。建议：${wording.locateNotFound}`,
          data: { target, found: false },
        }
      }
      const best = candidates[0]
      const cx = Math.round(best.x + best.w / 2)
      const cy = Math.round(best.y + best.h / 2)
      const rest = candidates.slice(1, 11).map(formatLine)
      const output =
        `找到「${best.text}」（${loaded.sourceDesc}）：中心 (${cx},${cy})，框 [${best.x},${best.y},${best.w},${best.h}]，置信度 ${best.score.toFixed(2)}\n` +
        `${wording.clickHint(cx, cy)}。${rest.length ? `\n其余候选：\n${rest.join("\n")}` : ""}`
      return { output, data: { target, found: true, best: { ...best, center: [cx, cy] }, candidates } }
    },
  }

  const locate_image: Tool = {
    name: "locate_image",
    description: wording.descriptions.locateImage,
    card: { titleParams: ["template", "template_region", "region"], args: "none" },
    parameters: schema(
      {
        template: { type: "string", description: "可选：模板 PNG 路径（相对会话工作目录）" },
        template_region: { type: "string", description: wording.templateRegionParam },
        image: { type: "string", description: wording.imageParam.locateImage },
        region: { type: "string", description: wording.regionParam.locateImage },
        threshold: { type: "number", description: "可选：相似度阈值 0-1（默认 0.8，降低可放宽匹配）" },
      },
      [],
    ),
    async execute(args, ctx): Promise<ToolResult> {
      gate?.(ctx)
      const templatePath = String(args.template ?? "").trim()
      const templateRegionRaw = String(args.template_region ?? "").trim()
      if (!templatePath && !templateRegionRaw) {
        return { output: "请提供 template（模板 PNG 路径）或 template_region（搜索图内模板区域）二者之一" }
      }
      const loaded = await load(ctx, String(args.image ?? "").trim(), String(args.region ?? "").trim())
      if ("error" in loaded) return { output: loaded.error }
      let tpl: RgbaImage
      if (templatePath) {
        const path = ctx.resolvePath(templatePath)
        if (!path.toLowerCase().endsWith(".png")) {
          return { output: `模板仅支持 PNG（当前 ${templatePath}）` }
        }
        const bytes = await ctx.readBinaryFile(path)
        if (bytes.byteLength > VISION_MAX_IMAGE_BYTES) {
          return { output: `模板过大（${(bytes.byteLength / 1024 / 1024).toFixed(1)}MB，上限 8MB）` }
        }
        try {
          tpl = decodePng(bytes)
        } catch (e) {
          return { output: `模板 PNG 解码失败: ${e instanceof Error ? e.message : e}` }
        }
      } else {
        const box = parseRegion(templateRegionRaw)
        if (!box) return { output: `template_region 格式错误: ${templateRegionRaw}（应为 x,y,w,h）` }
        if (box.x < 0 || box.y < 0 || box.x + box.w > loaded.img.width || box.y + box.h > loaded.img.height) {
          return { output: `template_region 超出搜索图范围（图 ${loaded.img.width}x${loaded.img.height}，区域 ${templateRegionRaw}）` }
        }
        tpl = cropImage(loaded.img, box)
      }
      const threshold = Number(args.threshold)
      let matches: TemplateMatch[]
      try {
        matches = matchTemplate(loaded.img, tpl, {
          threshold: Number.isFinite(threshold) && threshold > 0 && threshold < 1 ? threshold : 0.8,
        })
      } catch (e) {
        return { output: `模板匹配失败: ${e instanceof Error ? e.message : e}` }
      }
      if (!matches.length) {
        return {
          output: `未找到匹配的模板图形（${loaded.sourceDesc}）。建议：1) 降低 threshold（如 0.7）放宽匹配；${wording.locateImageNotFound}`,
          data: { found: false },
        }
      }
      const withOffset = (m: TemplateMatch) => ({
        ...m,
        x: m.x + loaded.offX,
        y: m.y + loaded.offY,
        center: [Math.round(m.x + loaded.offX + m.w / 2), Math.round(m.y + loaded.offY + m.h / 2)] as [number, number],
      })
      const best = withOffset(matches[0])
      const rest = matches.slice(1, 6).map((m) => {
        const o = withOffset(m)
        return `相似度 ${o.score.toFixed(2)}  [${o.x},${o.y},${o.w},${o.h}] → 中心 (${o.center[0]},${o.center[1]})`
      })
      return {
        output:
          `找到模板匹配（${loaded.sourceDesc}）：中心 (${best.center[0]},${best.center[1]})，框 [${best.x},${best.y},${best.w},${best.h}]，相似度 ${best.score.toFixed(2)}\n` +
          `${wording.clickHint(best.center[0], best.center[1])}。${rest.length ? `\n其余候选：\n${rest.join("\n")}` : ""}`,
        data: { found: true, best, candidates: matches.slice(1, 6).map(withOffset) },
      }
    },
  }

  return { ocr, locate, locate_image }
}
