import type { SubAgentDef } from "../../core/types"
import { createFeishuTools } from "./feishu_api"
// 系统提示词拆为独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./feishu_docs.md"

export const name = "feishu_docs"
export const description =
  "涉及飞书云文档时装载本子Agent：文档创建/读取/编辑、表格、多维表格、知识库、云空间与权限（需配置 FEISHU_DOCS_* 凭证）。输入：操作需求（含文档链接/token）；输出：操作结果与产物链接；写操作需审批。"
export const systemPrompt = systemPromptBase
export const tools = createFeishuTools()
export const requiresApproval = {
  create_doc: true,
  add_blocks: true,
  update_block: true,
  delete_blocks: true,
  import_markdown: true,
  create_folder: true,
  upload_file: true,
  insert_image: true,
  download_file: true,
  delete_file: true,
  create_sheet: true,
  write_sheet: true,
  append_sheet: true,
  create_bitable: true,
  add_bitable_records: true,
  update_bitable_record: true,
  delete_bitable_records: true,
  create_wiki_node: true,
  add_permission: true,
  set_link_share: true,
  api_call: true,
}
export const preload = false
export const envVars = [
  { name: "FEISHU_DOCS_APP_ID", description: "飞书应用 App ID（feishu_docs 文档/表格/多维表格等操作凭证）" },
  { name: "FEISHU_DOCS_APP_SECRET", description: "飞书应用 App Secret（敏感，仅本次任务临时注入，不落盘）" },
]

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars }
