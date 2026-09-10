import type { Tool } from "@gebai/sdk"
import { truncate } from "@gebai/sdk/node"
import { resizeForVision, resizeNote } from "./image-resize"

/** LLM 流式块（最小契约：视觉工具只需 text 块收集；与引擎 LLMProvider 结构兼容——
 *  provider 由引擎 compose 注入（setVisionProviderGetter），此处仅约束消费面形状）。 */
export interface LLMChunk {
  type: "text" | "done" | (string & {})
  text?: string
}
/** 最小 provider 契约（chat 方法）：视觉分析只需流式对话能力。 */
export interface VisionLLMProvider {
  chat(messages: Array<{ role: string; content: unknown }>, opts?: { signal?: AbortSignal }): AsyncIterable<LLMChunk>
}
/** 图片扩展名 → MIME（视觉工具白名单；与引擎 llm 模块同表）。 */
export const VISION_IMAGE_MIME: Record<string, string> = {
  png: "image/png",
  jpg: "image/jpeg",
  jpeg: "image/jpeg",
  gif: "image/gif",
  webp: "image/webp",
}
/** 组装多模态消息内容块（文本 + 图片 base64；与引擎 llm 模块同形态——provider 适配层处理各家协议）。 */
function imageMessageBlocks(text: string, mime: string, base64: string): Array<Record<string, unknown>> {
  return [{ type: "text", text }, { type: "image", mime, data: base64 }]
}

/** 图片附件 MIME 白名单（OpenAI 系与 Anthropic 均接受）；engine 上下文内联与压缩降级共用。 */
export const VISION_MIME_SET = new Set(["image/png", "image/jpeg", "image/gif", "image/webp"])

export const VISION_MAX_IMAGE_BYTES = 8 * 1024 * 1024

/** analyze 默认超时（秒）：外部多模态模型秒级延迟，超时引导改用本地视觉工具。 */
export const VISION_ANALYZE_TIMEOUT_SEC = 30
const VISION_ANALYZE_TIMEOUT_MIN = 1
const VISION_ANALYZE_TIMEOUT_MAX = 300

/**
 * 视觉 provider 提供者（组装层注册，见 boot/compose.ts）：让 vision 子代理 def 也能构造 analyze
 * 工具并使用同一 provider 解析逻辑（GEBAI_VISION_* 外挂模型 → 多模态主模型回落）。
 * 可选 env 参数：任务级 GEBAI_VISION 系列与 GEBAI_LLM_MULTIMODAL 覆盖（前端/会话配置的视觉模型在任务内生效）。
 */
let visionProviderGetter: ((env?: Record<string, string>) => VisionLLMProvider | null) | null = null
export function setVisionProviderGetter(getter: ((env?: Record<string, string>) => VisionLLMProvider | null) | null): void {
  visionProviderGetter = getter
}
export function getVisionProvider(env?: Record<string, string>): VisionLLMProvider | null {
  return visionProviderGetter ? visionProviderGetter(env) : null
}

/** 流式收集模型输出文本（视觉分析用）：异常原样上抛，无任何文本时抛中文错误。 */
export async function collectChatText(iter: AsyncIterable<LLMChunk>): Promise<string> {
  const parts: string[] = []
  for await (const c of iter) {
    if (c.type === "text" && c.text) parts.push(c.text)
  }
  const text = parts.join("")
  if (!text.trim()) throw new Error("模型未返回任何内容，请检查视觉模型配置")
  return text
}

/** analyze 超时返回文案：引导改用本地视觉工具（毫秒级、离线、不耗配额）。 */
export function analyzeTimeoutNote(sec: number): string {
  return `视觉分析超时（${sec} 秒，外部多模态模型未在时限内返回）。建议改用本地视觉工具（毫秒级、离线、不耗配额）：vision_ocr 读文字 / vision_locate 定位文字坐标 / vision_locate_image 图标模板匹配 / vision_detect 目标检测；需语义理解时可用更大 timeout 重试。`
}

/** 解析 analyze 超时参数（秒，缺省 30，钳制 1~300）。 */
export function parseTimeout(v: unknown): number {
  const n = Math.round(Number(v ?? VISION_ANALYZE_TIMEOUT_SEC))
  if (!Number.isFinite(n)) return VISION_ANALYZE_TIMEOUT_SEC
  return Math.max(VISION_ANALYZE_TIMEOUT_MIN, Math.min(VISION_ANALYZE_TIMEOUT_MAX, n))
}

/** 视觉语义分析工具工厂（视觉能力统一经 vision 子代理——全局 vision 工具已移除）：
 *  vision 子代理 def 以 name="analyze" 复用本工厂产出 vision_analyze（同一 provider 解析与实现）。
 *  name 定制时错误消息中的自称同步替换（如 "analyze: 缺少图片路径"）。 */
export function makeVisionTool(deps: { vision: (env?: Record<string, string>) => VisionLLMProvider | null; name?: string }): Tool {
  const toolName = deps.name ?? "vision"
  const self = (s: string): string => (toolName === "vision" ? s : s.replace(/vision/g, toolName))
  return {
    name: toolName,
    description:
      "视觉分析：将图片文件交给多模态（视觉）模型分析（外部模型，秒级延迟、耗配额；读文字/找坐标等本地能力可答的问题优先用本地视觉工具 ocr/locate/locate_image/detect）。target 为分析目标（要查看/识别/描述的内容，如「图中有几个人」「识别屏幕上的报错信息」）；timeout 为超时秒数（默认 30，超时提示改用本地视觉工具）。",
    parameters: {
      type: "object",
      properties: {
        target: { type: "string", description: "分析目标：要查看/识别/描述的内容（必填）" },
        image: { type: "string", description: "图片文件路径（相对会话工作目录——tmp/ 前缀可省略——或绝对路径，沙箱限定会话内；png/jpg/jpeg/gif/webp）" },
        timeout: { type: "number", description: `可选：超时秒数（默认 ${VISION_ANALYZE_TIMEOUT_SEC}，范围 ${VISION_ANALYZE_TIMEOUT_MIN}~${VISION_ANALYZE_TIMEOUT_MAX}）；模型迟缓时可增大后重试` },
      },
      required: ["target", "image"],
    },
    async execute(args, ctx) {
      // 任务级 env 传入：会话/前端配置 GEBAI_VISION_* 时按任务解析视觉模型（缺省沿用启动配置）
      const provider = deps.vision(ctx.env)
      if (!provider) {
        return {
          output:
            "视觉能力不可用：未配置多模态（视觉）模型。请设置 GEBAI_VISION_MODEL 等 GEBAI_VISION_* 环境变量，或让主模型声明多模态能力（GEBAI_LLM_MULTIMODAL=true）。",
        }
      }
      const target = String(args.target ?? "")
      if (!target.trim()) return { output: self("vision: 缺少分析目标（target 参数）") }
      const image = String(args.image ?? "")
      if (!image) return { output: self("vision: 缺少图片文件路径（image 参数）") }
      const timeoutSec = parseTimeout(args.timeout)
      const path = ctx.resolvePath(image)
      const ext = path.split(".").pop()?.toLowerCase() ?? ""
      const mime = VISION_IMAGE_MIME[ext]
      if (!mime) return { output: self(`vision: 不支持的图片格式: ${image}（支持 png/jpg/jpeg/gif/webp）`) }
      const buf = await ctx.readBinaryFile(path)
      if (buf.byteLength > VISION_MAX_IMAGE_BYTES) {
        return { output: self(`vision: 图片过大（${(buf.byteLength / 1024 / 1024).toFixed(1)}MB，上限 8MB），请压缩后再试`) }
      }
      // 发送给大模型前才压缩（原图保留在会话 tmp/），并把原始/压缩尺寸随图片一起告知模型
      const rz = await resizeForVision(buf, mime)
      const base64 = rz.buf.toString("base64")
      const text = imageMessageBlocks(`${target}\n${resizeNote(rz)}`, mime, base64)
      // 手动定时器实现超时（正常返回后立即清除，不在事件循环留残定时器）；
      // 与用户取消信号合并（任一触发即中止上游请求，不继续消耗配额）
      const timeoutCtl = new AbortController()
      const timer = setTimeout(() => timeoutCtl.abort(new Error("vision_timeout")), timeoutSec * 1000)
      const signal = ctx.signal ? AbortSignal.any([ctx.signal, timeoutCtl.signal]) : timeoutCtl.signal
      try {
        const out = await collectChatText(provider.chat([{ role: "user", content: text }], { signal }))
        const truncated = await truncate(out, "vision", ctx)
        return { ...truncated, blocks: [{ type: "image", path: image, mime }] }
      } catch (err) {
        // 超时（手动定时器）或取消（用户/上游信号）皆以 AbortError 形态抵达
        if ((err as Error)?.name === "AbortError" || signal.aborted) {
          if (ctx.signal?.aborted) throw err // 用户主动取消：原样上抛
          return { output: self(analyzeTimeoutNote(timeoutSec)) }
        }
        throw err
      } finally {
        clearTimeout(timer)
      }
    },
  }
}
