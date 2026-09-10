/**
 * @gebai/agents 入口：13 个 TS 子代理（code/self_optimize/hsh/cron/desktop/explore/
 * feishu_docs/feishu_group/playwright/reverse_site/vision/widgets/wps）+ 子代理专属基建
 * （cv 全家 / browser 桥接 / analyzer 符号分析 / widgets 存储 / shared 公共件）。
 *
 * 边界规则（DESIGN「TS 子代理抽包解耦」）：本包零 import @gebai/server（编译期强制——
 * tsconfig 无 server 引用）；server 单向依赖本包（boot 接线/discover 动态扫描/
 * 构建脚本 bundle/路由薄引用）。
 * 每个子代理模块约定：export const name / description / def（SubAgentDef，含 tools）——
 * 与 server 侧 sub-agents 目录的模块形状一致（index 不做 star re-export：13 个模块
 * 同名导出会冲突，统一经下面 allAgents 注册表消费）。
 */
import type { SubAgentDef } from "@gebai/sdk"

import * as code from "./code/index"
import * as self_optimize from "./self_optimize/index"
import * as hsh from "./hsh/index"
import * as cron from "./cron/cron"
import * as desktop from "./desktop/desktop"
import * as explore from "./explore/explore"
import * as feishu_docs from "./feishu_docs/feishu_docs"
import * as feishu_group from "./feishu_group/feishu_group"
import * as playwright from "./playwright/playwright"
import * as reverse_site from "./reverse_site/reverse_site"
import * as vision from "./vision/vision"
import * as widgets from "./widgets/widgets"
import * as wps from "./wps/wps"

/** 全部 TS 子代理模块（name/description/def），目录序与 server 侧 sub-agents 一致。 */
export const allAgents: Array<{
  name: string
  description: string
  def: SubAgentDef
}> = [
  code, self_optimize, hsh, cron, desktop, explore,
  feishu_docs, feishu_group, playwright, reverse_site, vision, widgets, wps,
]

export { code, self_optimize, hsh, cron, desktop, explore, feishu_docs, feishu_group, playwright, reverse_site, vision, widgets, wps }

// 子Agent 目录扫描排除清单（dev 发现 subagents.ts 与构建 build-subagents.ts 共享的唯一事实源）
export { NON_AGENT_DIRS, NON_AGENT_FILES } from "./shared/scan"

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
