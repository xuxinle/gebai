import type { SubAgentDef } from "../../core/types"
import { projectAware } from "../../core/projects"
import { wordAppendTool, wordCreateTool, wordReadTool } from "./word"
import { excelEditTool, excelReadTool, excelWriteTool } from "./excel"
import { pptCreateTool, pptReadTool } from "./ppt"
// 系统提示词拆为独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./wps.md"

export const name = "wps"
export const description =
  "涉及 Word/Excel/PPT 文档时装载本子Agent：.docx/.xlsx/.pptx 的创建/读取/追加/编辑与富排版（标题层级、表格、图表、图片嵌入、单元格样式、母版布局、页眉页脚），csv/tsv 读取；旧版 .doc/.xls/.ppt 不支持。输入：文档需求或现有文档路径；输出：产物文件路径与内容摘要。"
export const systemPrompt = systemPromptBase
export const tools = {
  word_create: projectAware(wordCreateTool),
  word_read: projectAware(wordReadTool),
  word_append: projectAware(wordAppendTool),
  excel_read: projectAware(excelReadTool),
  excel_write: projectAware(excelWriteTool),
  excel_edit: projectAware(excelEditTool),
  ppt_create: projectAware(pptCreateTool),
  ppt_read: projectAware(pptReadTool),
}
export const preload = false

export const def: SubAgentDef = { name, description, systemPrompt, tools, preload }
