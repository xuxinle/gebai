/**
 * @gebai/agents 入口：**只导子代理专属基建**（cv/browser/analyzer/widgets-store/shared 工具件），
 * **不列子代理清单**——子代理发现注册全自动（DESIGN「子代理自动发现」）：dev 由 server
 * SubAgentManager.discover() 扫描本包 src/ 目录（{name}/{name}.ts | {name}/index.ts | 纯 md
 * 双入口 + md-only 规则），dist/binary 由构建期 build-subagents.ts 以同一规则扫描生成 bundle
 * 注册表。新增子代理 = 在 src/ 放定义文件即可，本入口与 server 均零改动（无手工清单可漏改）。
 *
 * 边界规则（DESIGN「TS 子代理抽包解耦」）：本包零 import @gebai/server（编译期强制——
 * tsconfig 无 server 引用）；server 单向依赖本包（boot 接线/discover 动态扫描/
 * 构建脚本 bundle/路由薄引用）。
 * 每个子代理模块约定：export const name / description / def（SubAgentDef，含 tools）。
 */

// 子Agent 目录扫描排除清单（dev 发现 subagents.ts 与构建 build-subagents.ts 共享的唯一事实源）
export { NON_AGENT_DIRS, NON_AGENT_FILES } from "./shared/scan"

// 纯 md 子代理解析（md 定义单一来源：server 发现/构建脚本共用）
export { parseSubAgentMd, mdSubAgentDef, type ParsedSubAgentMd } from "./shared/sub-agent-md"

// code 域工具（git/system_info/env_detect/preview_server——引擎 compose 从本包注册全局工具，单向依赖）
export { gitTool, systemInfoTool, envDetectTool, makePreviewServerTool, type PreviewServerEntry } from "./code/tools"
export { fetchWithRedirectGuard, assertPublicHttpUrl, checkWebhookUrl } from "./shared/fetch-guard"
export { VISION_MIME_SET, VISION_MAX_IMAGE_BYTES, setVisionProviderGetter, getVisionProvider, makeVisionTool } from "./shared/vision"
export * from "./feishu_docs/oauth"
export { renderOfficeReadingView, OFFICE_PREVIEW_EXTS } from "./wps/preview"
export { analyzeTool, searchSymbolsTool } from "./analyzer/analyzer"
export { resizeForVision, resizeNote, imageSize } from "./shared/image-resize"
export { createLazyBridge, withSessionLock, type BridgeLike } from "./browser/bridge"
export { feishuFetch, feishuWsOptions, feishuTlsInsecure } from "./shared/tls"
export { saveMiniTool, deleteMiniTool, listMiniTools, getMiniTool } from "./widgets-store/mini-tools"
export { pageCaptureTool, PAGE_CAPTURE_HTML_LIMIT } from "./shared/page-capture"
export { readFeedbackTool } from "./shared/feedback"
