import type { LLMChunk, LLMProvider } from "./llm"
import { imageMessageBlocks, VISION_IMAGE_MIME } from "./llm"
import { truncate } from "./tools"
import type { Tool } from "./types"

/** vision 工具单张图片大小上限（8MB，超出提示压缩后重试）。 */
export const VISION_MAX_IMAGE_BYTES = 8 * 1024 * 1024

/**
 * 视觉 provider 提供者（组装层注册，见 index.ts）：让子Agent 定义（如 self_optimize）也能
 * 构造 vision 工具且与主 Agent 使用同一 provider 解析逻辑（GEBAI_VISION_* 外挂模型 → 多模态主模型回落）。
 * 可选 env 参数：任务级 GEBAI_VISION 系列与 GEBAI_LLM_MULTIMODAL 覆盖（前端/会话配置的视觉模型在任务内生效）。
 */
let visionProviderGetter: ((env?: Record<string, string>) => LLMProvider | null) | null = null
export function setVisionProviderGetter(getter: ((env?: Record<string, string>) => LLMProvider | null) | null): void {
  visionProviderGetter = getter
}
export function getVisionProvider(env?: Record<string, string>): LLMProvider | null {
  return visionProviderGetter ? visionProviderGetter(env) : null
}

/** 流式收集模型输出文本（vision 工具用）：异常原样上抛，无任何文本时抛中文错误。 */
export async function collectChatText(iter: AsyncIterable<LLMChunk>): Promise<string> {
  const parts: string[] = []
  for await (const c of iter) {
    if (c.type === "text" && c.text) parts.push(c.text)
  }
  const text = parts.join("")
  if (!text.trim()) throw new Error("模型未返回任何内容，请检查视觉模型配置")
  return text
}

export function makeVisionTool(deps: { vision: (env?: Record<string, string>) => LLMProvider | null }): Tool {
  return {
    name: "vision",
    description:
      "视觉分析：将图片文件交给多模态（视觉）模型分析。参数 target 为分析目标（要查看/识别/描述的内容，如「图中有几个人」「识别屏幕上的报错信息」）；image 为图片文件路径（相对会话工作目录，tmp/ 前缀可省略，支持 png/jpg/jpeg/gif/webp）。",
      parameters: {
        type: "object",
        properties: {
          target: { type: "string", description: "分析目标：要查看/识别/描述的内容（必填）" },
          image: { type: "string", description: "图片文件路径（相对会话工作目录——tmp/ 前缀可省略——或绝对路径，沙箱限定会话内；png/jpg/jpeg/gif/webp）" },
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
      if (!target.trim()) return { output: "vision: 缺少分析目标（target 参数）" }
      const image = String(args.image ?? "")
      if (!image) return { output: "vision: 缺少图片文件路径（image 参数）" }
      const path = ctx.resolvePath(image)
      const ext = path.split(".").pop()?.toLowerCase() ?? ""
      const mime = VISION_IMAGE_MIME[ext]
      if (!mime) return { output: `vision: 不支持的图片格式: ${image}（支持 png/jpg/jpeg/gif/webp）` }
      const buf = await ctx.readBinaryFile(path)
      if (buf.byteLength > VISION_MAX_IMAGE_BYTES) {
        return { output: `vision: 图片过大（${(buf.byteLength / 1024 / 1024).toFixed(1)}MB，上限 8MB），请压缩后再试` }
      }
      const base64 = Buffer.from(buf).toString("base64")
      const text = await collectChatText(provider.chat([{ role: "user", content: imageMessageBlocks(target, mime, base64) }]))
      const truncated = await truncate(text, "vision", ctx)
      return { ...truncated, blocks: [{ type: "image", path: image, mime }] }
    },
  }
}
