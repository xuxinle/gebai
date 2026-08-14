import type { SubAgentDef } from "../../core/types"
import { createPlaywrightTools } from "./playwright_tools"
// 系统提示词拆为独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./playwright.md"

export const name = "playwright"
export const description =
  "涉及网页浏览/信息抓取/表单填写/页面自动化验证时装载本子Agent：打开网页、读取内容、截图、点击/输入/表单、多标签页、JS 执行。输入：URL 或浏览/抓取/表单任务；输出：页面内容、截图与操作结果；导航/交互类操作需审批。"
export const systemPrompt = systemPromptBase
export const tools = createPlaywrightTools()
export const requiresApproval = {
  open: true,
  click: true,
  fill: true,
  press: true,
  select: true,
  check: true,
  evaluate: true,
  new_page: true,
  serve_dir: true,
}
export const preload = false

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
