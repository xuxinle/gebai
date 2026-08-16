import type { ToolSchema } from "@gebai/sdk"
import type { SubAgentDef, Tool } from "../../core/types"

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

export const name = "cron"
export const description =
  "定时任务管理（到点自动执行的无人值守任务）：创建（脚本运行 / 提示词运行 agent）/ 查看 / 修改 / 删除。需要周期性脚本或无人值守的 Agent 任务时装载本子Agent。输入：任务需求或管理指令；输出：任务 ID、下次执行时间与执行情况。仅服务端开启定时任务能力（GEBAI_CRON_ENABLED=true）时可用。"
export const systemPrompt =
  "你是定时任务管理助手。定时任务 = 会话内创建、到点自动执行的无值守任务（持久化于会话 cron.json，随会话清理）。工具经本子Agent 命名空间暴露：cron_add 创建、cron_list 查看、cron_update 修改、cron_remove 删除。工作要点：\n" +
  "1) 创建（cron_add，需审批）：type 二选一——script（脚本运行：shell 命令在会话 tmp/ 目录以会话环境执行，结果写入会话消息）或 prompt（提示词运行 agent：以指定提示词触发一次完整 Agent 会话）；schedule 支持 5 段 cron（分 时 日 月 周，本地时区，如 0 9 * * * 每天 9:00）或 @every 30m / @daily / @hourly / @weekly / @monthly；非法表达式创建即拒绝；\n" +
  "2) 查看（cron_list）：列出本会话全部任务（ID/名称/类型/周期/启用状态/上次与下次执行/次数/最近错误）；\n" +
  "3) 修改（cron_update，需审批）：按 id 改启用状态/周期/类型/内容，改后自动重算下次执行时间；\n" +
  "4) 删除（cron_remove，需审批）：按 id 删除，不可恢复，删前向用户确认。\n" +
  "任务绑定创建它的会话，跨会话不可见不可操作；创建/修改/删除均需用户审批（定时任务 = 无人值守的任意命令/会话执行）。"

const add: Tool = {
  name: "add",
  description:
    "创建定时任务，到点自动执行。类型二选一：script（脚本运行——shell 命令在会话 tmp/ 目录以会话环境执行，执行结果写入会话消息）；prompt（提示词运行 agent——以指定提示词触发一次完整 Agent 会话，过程与结果出现在会话消息流）。schedule 支持 5 段 cron（分 时 日 月 周，本地时区，如 0 9 * * * 每天 9:00）或 @every 30m（每 30 分钟）/ @daily / @hourly / @weekly / @monthly。仅服务端开启定时任务能力（GEBAI_CRON_ENABLED=true）时可用。",
  requiresApproval: true,
  parameters: schema(
    {
      name: { type: "string", description: "任务名称（可选，便于识别与管理）" },
      schedule: { type: "string", description: "定时表达式：5 段 cron（分 时 日 月 周）或 @every 30m / @daily / @hourly / @weekly / @monthly" },
      type: { enum: ["script", "prompt"], description: "任务类型：script=脚本运行（shell 命令）；prompt=提示词运行 agent" },
      script: { type: "string", description: "type=script 时必填：要执行的 shell 命令" },
      prompt: { type: "string", description: "type=prompt 时必填：触发 agent 运行的提示词" },
      enabled: { type: "boolean", description: "是否启用（默认 true）" },
    },
    ["schedule", "type"],
  ),
  async execute(args, ctx) {
    if (!ctx.cron) return { output: "定时任务能力未启用（服务端未配置 GEBAI_CRON_ENABLED=true）。" }
    const task = await ctx.cron.add({
      name: args.name != null ? String(args.name) : undefined,
      type: String(args.type) as "script" | "prompt",
      schedule: String(args.schedule ?? ""),
      script: args.script != null ? String(args.script) : undefined,
      prompt: args.prompt != null ? String(args.prompt) : undefined,
      enabled: args.enabled === undefined ? undefined : Boolean(args.enabled),
    })
    return {
      output: `定时任务已创建: ${task.id}${task.name ? `（${task.name}）` : ""}\n类型: ${task.type === "script" ? "脚本运行" : "提示词运行 agent"}\n周期: ${task.schedule}\n下次执行: ${new Date(task.nextRunAt).toString()}\n可用 cron_list 查看、cron_update 修改、cron_remove 删除。`,
    }
  },
}

const list: Tool = {
  name: "list",
  description: "查看本会话的定时任务列表（ID/名称/类型/周期/启用状态/上次与下次执行时间/执行次数）。",
  parameters: schema({}),
  async execute(_args, ctx) {
    if (!ctx.cron) return { output: "定时任务能力未启用（服务端未配置 GEBAI_CRON_ENABLED=true）。" }
    const tasks = await ctx.cron.list()
    if (!tasks.length) return { output: "本会话暂无定时任务。" }
    return {
      output: tasks
        .map((t) => {
          const body = t.type === "script" ? `命令: ${t.script}` : `提示词: ${t.prompt}`
          const last = t.lastRunAt ? new Date(t.lastRunAt).toString() : "未执行"
          const next = t.enabled ? new Date(t.nextRunAt).toString() : "-（已停用）"
          return `- ${t.id}${t.name ? `（${t.name}）` : ""} [${t.enabled ? "启用" : "停用"}] 类型: ${t.type === "script" ? "脚本运行" : "提示词运行 agent"} 周期: ${t.schedule}\n  ${body}\n  上次: ${last}（${t.lastStatus ?? "-"}） 下次: ${next} 次数: ${t.runCount}${t.lastError ? `\n  最近错误: ${t.lastError}` : ""}`
        })
        .join("\n"),
    }
  },
}

const update: Tool = {
  name: "update",
  description: "修改定时任务（按 id：改启用状态/周期/类型/内容），修改后下次执行时间自动重算。",
  requiresApproval: true,
  parameters: schema(
    {
      id: { type: "string", description: "任务 ID（cron_list 查看）" },
      enabled: { type: "boolean", description: "启用/停用" },
      schedule: { type: "string", description: "新的定时表达式" },
      type: { enum: ["script", "prompt"], description: "新的任务类型" },
      script: { type: "string", description: "type=script：新的 shell 命令" },
      prompt: { type: "string", description: "type=prompt：新的提示词" },
    },
    ["id"],
  ),
  async execute(args, ctx) {
    if (!ctx.cron) return { output: "定时任务能力未启用（服务端未配置 GEBAI_CRON_ENABLED=true）。" }
    const task = await ctx.cron.update(String(args.id), {
      enabled: args.enabled === undefined ? undefined : Boolean(args.enabled),
      schedule: args.schedule != null ? String(args.schedule) : undefined,
      type: args.type != null ? String(args.type) as "script" | "prompt" : undefined,
      script: args.script != null ? String(args.script) : undefined,
      prompt: args.prompt != null ? String(args.prompt) : undefined,
    })
    if (!task) return { output: `定时任务不存在: ${args.id}` }
    return { output: `定时任务已更新: ${task.id}${task.name ? `（${task.name}）` : ""} [${task.enabled ? "启用" : "停用"}] 周期: ${task.schedule}\n下次执行: ${task.enabled ? new Date(task.nextRunAt).toString() : "-（已停用）"}` }
  },
}

const remove: Tool = {
  name: "remove",
  description: "删除定时任务（按 id，不可恢复）。",
  requiresApproval: true,
  parameters: schema({ id: { type: "string" } }, ["id"]),
  async execute(args, ctx) {
    if (!ctx.cron) return { output: "定时任务能力未启用（服务端未配置 GEBAI_CRON_ENABLED=true）。" }
    const removed = await ctx.cron.remove(String(args.id))
    return removed ? { output: `定时任务已删除: ${args.id}` } : { output: `定时任务不存在: ${args.id}` }
  },
}

export const tools: Record<string, Tool> = { add, list, update, remove }
export const requiresApproval = { add: true, update: true, remove: true }
export const preload = false
export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
