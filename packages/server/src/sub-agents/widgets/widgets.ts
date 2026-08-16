import type { ToolSchema } from "@gebai/sdk"
import type { SubAgentDef, Tool } from "../../core/types"
import { saveMiniTool, deleteMiniTool, listMiniTools, getMiniTool } from "../../core/mini-tools"

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

export const name = "widgets"
export const description =
  "管理 HTML 小工具（Web 标题栏「小工具」面板中加载的可复用页面组件，与模型工具是两回事）：保存/列出/读取/删除。需要创建或维护小工具时装载本子Agent；制作流程通常先用全局 render_html 在聊天内调试页面，满意后保存。输入：小工具需求或维护指令；输出：保存/清单/源码/删除结果。"
export const systemPrompt =
  "你是 HTML 小工具管理助手。小工具 = 保存在服务端、经 Web 标题栏「小工具」面板随时加载的 HTML 页面组件（沙箱 iframe 渲染，脚本可执行但隔离于宿主页面），**与模型工具（tool call）是两回事**。工作流程：\n" +
  "1) 创建：先在聊天内用 render_html 调试页面（布局/交互/数据确认满意），再 save 保存——name 仅限字母/数字/下划线/中文（1-40 字符，不含 . / 等分隔符），scope=private 用户私有（默认）/ public 公用（所有用户可见），同名覆盖；\n" +
  "2) 查看：list 列出全部小工具（名称/范围/更新时间），get 按名读取完整源码（私有同名优先于公用）；\n" +
  "3) 修改：get 读取现有源码 → 修改 → save 同名覆盖 → 建议 render_html 复核效果后再收尾；\n" +
  "4) 删除：delete 按名删除（不可恢复）；公用删除影响所有用户，删前向用户确认。\n" +
  "收尾：汇报保存/删除结果与名称（用户按名在面板中加载）。"

const save: Tool = {
  name: "save",
  // 仅实时前端可用（供标题栏「小工具」面板加载，依赖前端 UI），多轮交互/无交互模式禁用
  interaction: "realtime",
  description: "保存 HTML 小工具（同名覆盖）。建议先用 render_html 调试满意后再保存。scope 参数选择可见范围（private 用户私有默认 / public 公用）。",
  card: { titleParams: ["name"] },
  parameters: schema(
    {
      name: { type: "string", description: "小工具名（1-40 字符，字母/数字/下划线/中文；面板列表与加载均按此名）" },
      html: { type: "string", description: "HTML 源码（完整文档或片段，加载时经沙箱 iframe 渲染）" },
      scope: { enum: ["private", "public"], description: "可见范围：private=用户私有（默认）/ public=公用（所有用户可用）" },
    },
    ["name", "html"],
  ),
  async execute(args, ctx) {
    const scope = args.scope === "public" ? "public" : "private"
    const info = await saveMiniTool(ctx.home, ctx.user, {
      name: String(args.name ?? ""),
      html: String(args.html ?? ""),
      scope,
    }, { mode: ctx.authMode ?? "local", role: ctx.userRole })
    return {
      output: `小工具已保存: ${info.name}（${scope === "public" ? "公用" : "用户私有"}，${info.html.length} 字符）\n可在标题栏「小工具」面板中加载使用${scope === "public" ? "，所有用户可见" : ""}。`,
    }
  },
}

const list: Tool = {
  name: "list",
  interaction: "realtime",
  description: "列出已保存的小工具（名称/范围/更新时间；私有同名覆盖公用，不重复展示）。",
  parameters: schema({}),
  async execute(_args, ctx) {
    const items = await listMiniTools(ctx.home, ctx.user)
    if (!items.length) return { output: "暂无小工具。可先在聊天内用 render_html 调试页面，满意后用 save 保存。", data: { items: [] } }
    const lines = items.map((t) => `${t.name}（${t.scope === "public" ? `公用${t.owner ? `，作者 ${t.owner}` : ""}` : "私有"}，更新 ${new Date(t.updatedAt).toISOString()}）`)
    return { output: `共 ${items.length} 个小工具：\n${lines.join("\n")}`, data: { items } }
  },
}

const get: Tool = {
  name: "get",
  interaction: "realtime",
  description: "按名读取小工具完整源码与元信息（解析顺序：用户私有 → 公用）。修改前先 get 拿当前源码。",
  card: { titleParams: ["name"] },
  parameters: schema({ name: { type: "string", description: "小工具名" } }, ["name"]),
  async execute(args, ctx) {
    const info = await getMiniTool(ctx.home, ctx.user, String(args.name ?? ""))
    if (!info) return { output: `小工具不存在或名称非法: ${args.name}（可用 list 查看清单）。` }
    return {
      output: `小工具 ${info.name}（${info.scope === "public" ? "公用" : "用户私有"}，${info.html.length} 字符，更新 ${new Date(info.updatedAt).toISOString()}）源码：\n${info.html}`,
      data: { name: info.name, scope: info.scope, html: info.html, updatedAt: info.updatedAt },
    }
  },
}

const remove: Tool = {
  name: "delete",
  // 仅实时前端可用（删除的是前端小工具库条目），多轮交互/无交互模式禁用
  interaction: "realtime",
  requiresApproval: true,
  description: "删除小工具（按名 + 范围，不可恢复）。私有仅本人；公用删除影响所有用户。",
  card: { titleParams: ["name"] },
  parameters: schema(
    {
      name: { type: "string", description: "要删除的小工具名" },
      scope: { enum: ["private", "public"], description: "删除的范围：private（默认）/ public" },
    },
    ["name"],
  ),
  async execute(args, ctx) {
    const scope = args.scope === "public" ? "public" : "private"
    const removed = await deleteMiniTool(ctx.home, ctx.user, String(args.name ?? ""), scope, { mode: ctx.authMode ?? "local", role: ctx.userRole })
    if (!removed) return { output: `小工具不存在或名称非法: ${args.name}（scope=${scope}，可用 list 查看清单）` }
    return { output: `小工具已删除: ${args.name}（${scope === "public" ? "公用" : "用户私有"}）` }
  },
}

export const tools: Record<string, Tool> = { save, list, get, delete: remove }
export const preload = false
export const def: SubAgentDef = { name, description, systemPrompt, tools, preload }
