import type { SubAgentDef } from "../../core/base/types"
import { createPlaywrightTools } from "./playwright_tools"
import { createPlaywrightSessionTools } from "./playwright_session_tools"
import { createPlaywrightCvTools } from "./playwright_cv_tools"
// 系统提示词拆为独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./playwright.md"

export const name = "playwright"
export const description =
  "涉及网页浏览/信息抓取/表单填写/页面自动化验证时装载本子Agent：打开网页、读取内容、截图、点击/输入/悬停/拖拽/文件上传、多标签页、iframe 穿透、JS 执行、Cookie/localStorage/登录态管理、下载与对话框、环境仿真、PDF 导出；页面截图本地 OCR/文字定位/模板匹配（canvas 等无 DOM 内容的兜底，离线小模型）。输入：URL 或浏览/抓取/表单任务；输出：页面内容、截图与操作结果；导航/交互/凭证类操作需审批。"
export const systemPrompt = systemPromptBase
export const tools = { ...createPlaywrightTools(), ...createPlaywrightSessionTools(), ...createPlaywrightCvTools() }
export const requiresApproval = {
  open: true,
  click: true,
  fill: true,
  press: true,
  select: true,
  check: true,
  hover: true,
  dblclick: true,
  drag: true,
  upload: true,
  evaluate: true,
  new_page: true,
  serve_dir: true,
  emulate: true,
  cookies: true,
  local_storage: true,
  storage_state: true,
}
export const preload = false

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
