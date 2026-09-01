import type { SubAgentDef } from "../../core/types"
import { createCaptureTools, createHttpRequestTool } from "./reverse_site_tools"
// 系统提示词拆为独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./reverse_site.md"

export const name = "reverse_site"
export const description =
  "涉及网站/接口逆向时装载本子Agent：分析站点结构、捕获网络请求还原接口、探测验证，输出站点地图与 API 文档（可转交 self_optimize 生成子Agent）；依赖 playwright（装载时自动连带装载，浏览器自动化工具以 playwright_ 前缀复用，与接口录制共享同一浏览器会话）；仅限授权站点，浏览器交互与接口探测需审批。输入：目标 URL 与逆向目标；输出：分析文档与接口清单。"
export const systemPrompt = systemPromptBase

// 工具集只含接口逆向专属工具（capture_* 经 createLazyBridge 共享 playwright 的同一桥接进程与
// 浏览器会话——页面操作与网络录制天然一致）；浏览器自动化全套由依赖 playwright 提供（dependencies
// 自动装载，playwright_ 命名空间注册），文件读写与编排（read/write/fetch_url/agent_*）为全局工具
// 直接用全局名——本 def 不重复声明任何依赖方或全局已有工具。
export const tools = {
  ...createCaptureTools(),
  http_request: createHttpRequestTool(),
}
export const requiresApproval = {
  http_request: true,
}
export const preload = false

export const def: SubAgentDef = {
  name,
  description,
  systemPrompt,
  tools,
  requiresApproval,
  preload,
  // 依赖自动装载（DESIGN「子Agent 依赖与自动装载」）：装载/预加载 reverse_site 时系统连带装载
  // playwright——浏览器工具（open/click/content/evaluate…）以 playwright_ 前缀注册，审批映射由
  // playwright def 单源维护；playwright 被启停名单移除/构建裁剪时跳过（本 Agent 仅剩接口直连探测）
  dependencies: ["playwright"],
}
