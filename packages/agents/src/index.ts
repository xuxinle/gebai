/**
 * @gebai/agents 入口：**只导子代理专属基建**（cv/browser/analyzer/shared 工具件），
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

// 纯 md 子代理解析（md 定义单一来源：server 发现/构建脚本共用）
export { parseSubAgentMd, mdSubAgentDef, type ParsedSubAgentMd } from "./core/shared/sub-agent-md"

// code 域工具（git/system_info/env_detect/preview_server——引擎 compose 从本包注册全局工具，单向依赖）
export { gitTool, systemInfoTool, envDetectTool, makePreviewServerTool, type PreviewServerEntry } from "./core/code-tools"
export { fetchWithRedirectGuard, assertPublicHttpUrl, checkWebhookUrl } from "./core/shared/fetch-guard"
export { VISION_MIME_SET, VISION_MAX_IMAGE_BYTES, setVisionProviderGetter, getVisionProvider, makeVisionTool } from "./core/shared/vision"
export * from "./agents/feishu_docs/oauth"
export { renderOfficeReadingView, OFFICE_PREVIEW_EXTS } from "./agents/wps/preview"
export { analyzeTool, searchSymbolsTool, grammarBytes } from "./core/analyzer/analyzer"
export { resizeForVision, resizeNote, imageSize } from "./core/shared/image-resize"
export { createLazyBridge, withSessionLock, type BridgeLike } from "./core/browser/bridge"
export { feishuFetch, feishuWsOptions, feishuTlsInsecure } from "./core/shared/tls"
export { pageCaptureTool, PAGE_CAPTURE_HTML_LIMIT } from "./core/shared/page-capture"
export { readFeedbackTool } from "./core/shared/feedback"

// 本机离线语音合成引擎（core/tts）：子Agent 工具与 REST 朗读接口共用同一份脚本与解析逻辑，
// 执行通道（runCommand/文件读写）经 TtsDeps 注入——server 侧不重写脚本
export {
  TTS_CHUNK_CHARS,
  TTS_ENGINES,
  TTS_MAX_TEXT,
  TTS_PITCH,
  TTS_RATE,
  TTS_REQUEST_MAX_TEXT,
  TTS_TIMEOUT_MS,
  TTS_VOLUME,
  UNSUPPORTED_PLATFORM_NOTE,
  clampPercent,
  concatWav,
  escapeXml,
  isSupportedPlatform,
  normalizeEngine,
  plainTextForSpeech,
  runTtsScript,
  scriptFailureNote,
  setTtsPlatform,
  splitText,
  validateText,
  type TtsDeps,
  type TtsEngine,
  type TtsRunInput,
  type TtsRunOutput,
  type TtsScriptResult,
  type TtsVoice,
} from "./core/tts/speech"

// 两级研判（core/triage）：子Agent 工具（会话内）与 REST 接口共用同一份逻辑与同一套参数契约，
// L2 精审执行器（隔离子会话 / 引擎会话）由调用方注入——两侧不重写编排、不重建参数面
export {
  buildL1Prompt,
  buildL1Schema,
  buildL2Prompt,
  classifyL1,
  defaultJobDir,
  hasOwnConfidence,
  normalizeItems,
  normalizeTriageParams,
  parseItemsText,
  parseL2Output,
  resolveL1Endpoint,
  runTriage,
  toL1Record,
  toTriageOptions,
  triageToolProperties,
  validateItems,
  DEFAULT_L1_PROMPT,
  DEFAULT_L1_SYSTEM,
  DEFAULT_L2_SYSTEM,
  TRIAGE_DEFAULT_CONCURRENCY,
  TRIAGE_DEFAULT_ESCALATE,
  TRIAGE_DEFAULT_L1_MAX_TOKENS,
  TRIAGE_DEFAULT_L2_BATCH,
  TRIAGE_DEFAULT_MAX_L2,
  TRIAGE_DEFAULT_RESULT_LIMIT,
  TRIAGE_DEFAULT_THRESHOLD,
  TRIAGE_ENVELOPE_FIELDS,
  TRIAGE_PARAM_NAMES,
  TRIAGE_PARAM_SPECS,
  TRIAGE_TEXT_RENDER_MAX,
  type EvidenceLink,
  type L1Record,
  type TriageItem,
  type TriageL1Options,
  type TriageL1Params,
  type TriageL2Options,
  type TriageL2Params,
  type TriageL2Runner,
  type TriageOptions,
  type TriageParamSpec,
  type TriageParams,
  type TriageProgress,
  type TriageResult,
  type TriageSummary,
  type TriageVerdict,
} from "./core/triage"
