/** 编排支持类全局工具（tool_schemas）——自 core/tools.ts 按域拆分（原与 flow 同文件，flow 工具已移除）。 */
import type { Tool } from "../base/types"
import { schema, type GlobalToolEntry } from "./shared"

/** 批量获取工具的输入参数与结构化输出 schema（DESIGN「工具双输出」）：js 编排前理解输出结构，避免逐个试调。 */
export const toolSchemasTool: Tool = {
  name: "tool_schemas",
  description:
    "批量获取工具的输入参数 schema 与结构化输出（data）schema。编写 js 脚本编排调用工具前先用本工具了解相关工具的输出结构（js 内工具函数返回值的 data 字段引用前提）。tools 传工具名列表（可含子Agent 命名空间工具，如 code_read）；省略时返回全部已启用工具的输出结构概要（不含输入参数，紧凑一行一个）。无 outputSchema 的工具仅有文本 output（无结构化 data 可引用）。",
  parameters: schema({
    tools: { type: "array", items: { type: "string" }, description: "工具名列表（省略 = 全部已启用工具的输出概要）" },
  }),
  async execute(args, ctx) {
    const all = ctx.registry.schemas()
    const names = Array.isArray(args.tools) ? args.tools.map(String).filter(Boolean) : []
    if (!names.length) {
      const lines = all.map((s) => {
        const os = ctx.registry.resolve(s.name)?.tool.outputSchema
        return `- ${s.name}: ${os ? JSON.stringify(os) : "（仅文本 output，无结构化 data）"}`
      })
      return { output: `已启用工具（${all.length} 个）的输出结构：\n${lines.join("\n")}`, data: { tools: all.map((s) => ({ name: s.name, outputSchema: ctx.registry.resolve(s.name)?.tool.outputSchema ?? null })) } }
    }
    const entries = names.map((name) => {
      const s = all.find((x) => x.name === name)
      if (!s) return { name, error: "未知或未启用的工具" }
      return { name, description: s.description, parameters: s.parameters, outputSchema: ctx.registry.resolve(name)?.tool.outputSchema ?? null }
    })
    return { output: JSON.stringify(entries, null, 2), data: { tools: entries } }
  },
}

export const globalTools: GlobalToolEntry[] = [{ name: "tool_schemas", tool: toolSchemasTool }]
