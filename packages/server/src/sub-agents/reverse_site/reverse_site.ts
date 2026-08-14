import type { SubAgentDef } from "../../core/types"
import { readTool, writeTool, fetchUrlTool, agentListTool, agentLoadTool, agentRunTool } from "../../core/tools"
import { Bridge, createPlaywrightTools } from "../playwright/playwright_tools"
import { createCaptureTools, createHttpRequestTool } from "./reverse_site_tools"
// 系统提示词拆为独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./reverse_site.md"

export const name = "reverse_site"
export const description =
  "涉及网站/接口逆向时装载本子Agent：分析站点结构、捕获网络请求还原接口、探测验证，输出站点地图与 API 文档（可转交 self_optimize 生成子Agent）；仅限授权站点，浏览器交互与接口探测需审批。输入：目标 URL 与逆向目标；输出：分析文档与接口清单。"
export const systemPrompt = systemPromptBase

// 与 playwright 共享同一桥接进程与浏览器会话：页面操作与网络录制天然一致
const bridge = new Bridge()
export const tools = {
  ...createPlaywrightTools({ bridge }),
  ...createCaptureTools({ bridge }),
  http_request: createHttpRequestTool(),
  fetch_url: fetchUrlTool,
  read: readTool,
  write: writeTool,
  agent_list: agentListTool,
  agent_load: agentLoadTool,
  agent_run: agentRunTool,
}
export const requiresApproval = {
  open: true,
  click: true,
  fill: true,
  press: true,
  select: true,
  check: true,
  evaluate: true,
  new_page: true,
  http_request: true,
  write: true,
}
export const preload = false

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
