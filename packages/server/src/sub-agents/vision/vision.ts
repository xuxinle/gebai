/**
 * vision 视觉子代理 TS 侧贡献（跨语言同名合并，见 native-agents/README.md「跨语言同名合并」）：
 * 仅贡献多模态语义分析 analyze——provider 抽象属宿主 LLM 层（三协议流式适配、GEBAI_VISION_*
 * 与任务级 env 覆盖、多模态回落链），凭证不下传边车。
 * 本地识别四工具（ocr/locate/locate_image/detect）由 Python native 侧贡献
 * （native-agents/python/vision/，onnxruntime 原生推理）——「基础工具 TS 写、特殊工具其他
 * 语言写」的反向样例：重计算迁边车、LLM 耦合留宿主。description/PROMPT 留空不贡献，
 * 识别工具描述与决策序提示词由 native 侧 PROMPT.md 提供，合并层拼接两侧。
 */
import type { SubAgentDef } from "../../core/base/types"
import { makeVisionTool, getVisionProvider } from "../../core/tools/vision"
import systemPromptBase from "./vision.md"

export const name = "vision"
// 跨语言合并：描述留空 = 本侧不贡献，由 native 侧 manifest description 与合并层兜底
export const description = ""
export const systemPrompt = systemPromptBase

export const tools = { analyze: makeVisionTool({ vision: getVisionProvider, name: "analyze" }) }
export const requiresApproval = {}

export const preload = false

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
