import type { SubAgentDef } from "@gebai/sdk"
import { createFeishuTools } from "./feishu_api"
// 系统提示词拆为独立 md 维护（目录形式约定：{dir}/{dir}.md）。
// **显式 `with { type: "text" }`**：Bun 默认对 .md 走 html loader（会包 `<p>` 标签），必须拿原始 Markdown
// （与 styles/index.ts 同机制；否则提示词以 HTML 形态进入上下文，标签白占字符且割裂列表结构）。
import systemPromptBase from "./feishu_docs.md" with { type: "text" }

export const name = "feishu_docs"
export const description =
  "涉及飞书云文档时装载本子Agent：文档创建/读取/编辑、表格、多维表格、知识库、云空间与权限（需配置 FEISHU_DOCS_* 凭证）。写操作需审批。输入：操作需求（含文档链接/token）。"
export const systemPrompt = systemPromptBase
export const tools = createFeishuTools()
export const requiresApproval = {
  create_doc: true,
  fetch_doc: false,
  update_doc: true,
  add_blocks: true,
  update_block: true,
  replace_text: true,
  set_table_width: true,
  delete_blocks: true,
  import_markdown: true,
  import_xml: true,
  create_folder: true,
  upload_file: true,
  insert_image: true,
  download_file: true,
  delete_file: true,
  copy_file: true,
  move_file: true,
  add_comment: true,
  resolve_comment: true,
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
  { name: "FEISHU_DOCS_FOLDER_URL", description: "目标文件夹 URL 或 folder token：创建的资源落在该文件夹下，你自动拥有全部权限（未配置则创建在应用云空间，归应用所有）" },
]

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars }
