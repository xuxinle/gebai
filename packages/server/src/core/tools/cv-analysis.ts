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
      try {
        const raw = await getCvRunner().ocr(loaded.img, { env: ctx.env })
        lines = mapLines(raw, loaded.offX, loaded.offY)
      } catch (e) {
        return { output: `本地识别失败: ${e instanceof Error ? e.message : e}` }
      }
      if (find) lines = lines.filter((l) => l.text.toLowerCase().includes(find))
      const capped = lines.length > OCR_LINE_LIMIT
      const shown = lines.slice(0, OCR_LINE_LIMIT)
      const head = `识别到 ${lines.length} 行（${loaded.sourceDesc}，坐标相对其原点）${find ? `，过滤「${args.find}」` : ""}：`
      const body = shown.map(formatLine).join("\n")
      const tail = capped ? `\n…（共 ${lines.length} 行，仅显示前 ${OCR_LINE_LIMIT} 行；可用 find 参数过滤收窄）` : ""
      return {
        output: lines.length ? `${head}\n${body}${tail}` : `${head}\n（无${find ? "匹配" : "识别到"}文本——可能是图形界面无文字、分辨率过低，或需用 vision 工具做语义分析）`,
        data: { source: loaded.sourceDesc, find: args.find ?? null, lines },
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
        lines = mapLines(await getCvRunner().ocr(loaded.img, { env: ctx.env }), loaded.offX, loaded.offY)
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
