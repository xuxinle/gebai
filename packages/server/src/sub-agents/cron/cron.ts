import type { ToolSchema } from "@gebai/sdk"
import type { SubAgentDef, Tool } from "../../core/base/types"
import type { CronNotifyChannel } from "../../core/schedule/notify"

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

export const name = "cron"
export const description =
  "定时任务管理（到点自动执行的无人值守任务）：创建（脚本运行 / 提示词运行 agent）/ 查看 / 修改 / 手动触发 / 删除，任务为用户级资源（不随会话删除消失），支持飞书群等通知通道与时区。需要周期性脚本或无人值守的 Agent 任务时装载本子Agent。输入：任务需求或管理指令；输出：任务 ID、下次执行时间与执行情况。"
export const systemPrompt =
  "你是定时任务管理助手。定时任务 = 用户级无人值守任务（持久化于用户目录 cron.json，与会话生命周期解耦——会话删除后任务仍按期执行），到点自动执行。工具经本子Agent 命名空间暴露：cron_add 创建、cron_list 查看、cron_update 修改、cron_trigger 手动触发、cron_remove 删除。工作要点：\n" +
  "1) 创建（cron_add，需审批）：type 二选一——script（脚本运行：shell 命令在任务专属工作目录以用户环境执行，结果写入任务历史并可选通知）或 prompt（提示词运行 agent：以指定提示词触发一次完整 Agent 会话）。schedule 支持 5 段 cron（分 时 日 月 周，如 0 9 * * * 每天 9:00）、@every 30m、@daily/@hourly/@weekly/@monthly、@at 2026-09-01T09:00（一次性，触发后自动停用）；可配 timezone（IANA 名如 Asia/Shanghai，缺省服务器本地时区）；非法表达式创建即拒绝；\n" +
  "2) prompt 型执行目标 target：ephemeral（缺省，每次触发新建独立会话，上下文不累积——例行检查/报告首选）、sticky（专用会话跨次复用，上下文延续——需要延续记忆的任务用）、session（绑定既有会话执行，缺省为创建任务时的当前会话）；ephemeral/sticky 可配 agents 预载子Agent 名单；\n" +
  "3) 通知 notify（无人值守任务建议配置）：通道数组，每条 {type,target,webhook_id,secret,at}——type=webhook（任意 http(s) 回调 POST JSON，可直配 target URL 或以 webhook_id 引用 REST /api/v1/webhooks 已注册的事件 Webhook——投递自动带注册密钥的 X-Gebai-Signature HMAC 签名，注册侧改动即时生效）、feishu（飞书通知：target 填群机器人 webhook 地址，或直接填群 chat_id（oc_ 前缀）以应用身份推送指定群——后者需服务端配置飞书应用凭证，chat_id 可装载 feishu_group 子Agent 用 chats_list 查询；secret 为 webhook 加签密钥可选）、feishu_chat（同 feishu 的 chat_id 形态）；飞书通知默认以 **markdown 卡片**发送（状态着色头部 + 粗体字段 + 输出摘要），可配 at 名单 @特定人（open_id——不知道 open_id 时装载 feishu_group 子Agent 用 members_list 按姓名查；\"all\"=@所有人——注意：at 含 \"all\" 时自动降级为文本消息发送，飞书卡片不支持 @所有人）；webhook 通道的 at 名单随 JSON 载荷 at 字段携带（接收方系统据此渲染 @ 人），配 secret 时载荷带 HMAC 签名；notify_on=always（缺省每次通知）/error（仅失败）；运行结果/超时/自动停用会推送到全部通道，通知失败不影响任务执行。**全局默认通道**：服务端可配 GEBAI_CRON_NOTIFY_WEBHOOK（全局 webhook）与 GEBAI_CRON_NOTIFY_FEISHU（全局飞书群 chat_id 或群机器人 webhook）——任务未配 notify 时自动经全局通道推送（任务自配 notify 则用任务自己的、不叠加）；用户未要求特定通道且未显式拒绝通知时可不传 notify（走全局默认）；\n" +
  "4) 可靠性参数：misfire=skip（缺省，停机错过即跳过）/run（启动后立即补跑一次）；timeoutMs 单次执行超时（缺省脚本 5 分钟、提示词 30 分钟，到时终止）；maxConsecutiveErrors 连续失败 N 次自动停用（防错误任务无限重试刷屏，建议通知类任务配置如 5）；\n" +
  "5) 查看（cron_list）：列出当前用户全部任务（ID/名称/类型/周期/目标/启用状态/上次与下次执行/次数/最近错误/最近运行历史）；\n" +
  "6) 修改（cron_update，需审批）：按 id 改启用状态/周期/类型/内容/目标/通知等，改后自动重算下次执行时间；\n" +
  "7) 手动触发（cron_trigger，需审批）：按 id 立即执行一次用于验证（不改动既定调度节奏）；\n" +
  "8) 删除（cron_remove，需审批）：按 id 删除，不可恢复，删前向用户确认。\n" +
  "任务为用户级资源：任何会话创建后全局可见可管（跨会话不再隔离）；创建/修改/删除/手动触发均需用户审批（定时任务 = 无人值守的任意命令/会话执行）。"

function notifyParam(): Record<string, unknown> {
  return {
    type: "array",
    description:
      "通知通道数组（可选）：每条 {type,target,webhook_id,secret,at}——webhook（http(s) 回调 URL，或 webhook_id 引用 REST /api/v1/webhooks 已注册的事件 Webhook——投递自动带其 HMAC 签名，二选一）/ feishu（飞书群自定义机器人 webhook 地址）/ feishu_chat（飞书 chat_id，需服务端飞书应用凭证）；secret 为密钥（feishu 加签 / webhook 直配时 HMAC 签名）；at 为 @ 人名单（飞书渲染进卡片；webhook 随 JSON 载荷 at 字段携带供接收方解析），通知以 markdown 卡片发送（webhook 为 JSON）",
    items: {
      type: "object",
      properties: {
        type: { enum: ["webhook", "feishu", "feishu_chat"], description: "通道类型" },
        target: { type: "string", description: "webhook=URL；feishu=群机器人 webhook URL 或群 chat_id（oc_ 前缀，应用身份推送指定群）；feishu_chat=群 chat_id；webhook 用 webhook_id 引用时可省略" },
        webhook_id: { type: "string", description: "引用 REST /api/v1/webhooks 注册的事件 Webhook id（仅 webhook 通道，与 target 二选一，投递带注册密钥的 X-Gebai-Signature 签名）" },
        secret: { type: "string", description: "密钥（可选；feishu 加签 / webhook 直配 URL 时 HMAC 签名；修改时传 *** 表示保持不变）" },
        at: {
          type: "array",
          description: "@ 人名单（可选）：open_id（ou_/un_/on_ 前缀）或 \"all\"（@所有人），条目可为字符串或 {id,name}；飞书通道渲染进卡片，webhook 通道随载荷携带",
          items: { type: ["string", "object"], properties: { id: { type: "string", description: "open_id 或 all" }, name: { type: "string", description: "展示名（可选，缺省解析真实姓名）" } }, required: ["id"] },
        },
      },
      required: ["type"],
    },
  }
}

function parseNotify(raw: unknown): CronNotifyChannel[] | undefined {
  if (raw === undefined || raw === null) return undefined
  if (!Array.isArray(raw)) return undefined
  const out = raw.map((ch) => {
    const c = (ch ?? {}) as Record<string, unknown>
    return {
      type: String(c.type),
      target: String(c.target ?? ""),
      // 入参 webhook_id（蛇形契约）；旧驼峰兜底读存量 cron.json 数据
      webhookId: c.webhook_id != null ? String(c.webhook_id) : c.webhookId != null ? String(c.webhookId) : undefined,
      secret: c.secret != null ? String(c.secret) : undefined,
      at: c.at,
    } as CronNotifyChannel
  })
  return out
}

function parseAgents(raw: unknown): string[] | undefined {
  if (raw === undefined || raw === null) return undefined
  if (!Array.isArray(raw)) return undefined
  const out = raw.map((a) => String(a ?? "").trim()).filter(Boolean)
  return out.length ? out : undefined
}

function describeTarget(t: { type: string; target?: string; agents?: string[] }): string {
  if (t.type !== "prompt") return "-"
  const target = t.target ?? "ephemeral"
  const label = target === "ephemeral" ? "独立新会话（每次）" : target === "sticky" ? "专用会话（复用）" : "绑定会话"
  return t.agents?.length ? `${label}，预载 ${t.agents.join(",")}` : label
}

const add: Tool = {
  name: "add",
  description:
    "创建定时任务（用户级，不随会话删除消失），到点自动执行。类型二选一：script（脚本运行——shell 命令在任务专属工作目录执行）；prompt（提示词运行 agent——按 target 选执行会话：ephemeral 每次新建/sticky 专用复用/session 绑定会话）。schedule 支持 5 段 cron、@every 30m、@daily 等、@at 一次性；可配 timezone 时区、misfire 补跑、timeoutMs 超时、notify 通知通道（飞书群/webhook）、maxConsecutiveErrors 连续失败自动停用。",
  requiresApproval: true,
  parameters: schema(
    {
      name: { type: "string", description: "任务名称（可选，便于识别与管理）" },
      schedule: { type: "string", description: "定时表达式：5 段 cron（分 时 日 月 周）、@every 30m、@daily/@hourly/@weekly/@monthly、@at 2026-09-01T09:00（一次性）" },
      type: { enum: ["script", "prompt"], description: "任务类型：script=脚本运行（shell 命令）；prompt=提示词运行 agent" },
      script: { type: "string", description: "type=script 时必填：要执行的 shell 命令" },
      prompt: { type: "string", description: "type=prompt 时必填：触发 agent 运行的提示词" },
      target: { enum: ["ephemeral", "sticky", "session"], description: "type=prompt 执行目标（缺省 ephemeral）：ephemeral=每次触发新建独立会话；sticky=专用会话跨次复用（上下文延续）；session=绑定既有会话（缺省当前会话）" },
      session_id: { type: "string", description: "target=session 时可指定绑定会话 id（缺省为当前会话）" },
      agents: { type: "array", items: { type: "string" }, description: "target=ephemeral/sticky 预载子Agent 名单（可选，如 explore/code）" },
      timezone: { type: "string", description: "IANA 时区名（可选，如 Asia/Shanghai；缺省服务器本地时区）" },
      misfire: { enum: ["skip", "run"], description: "停机错过触发的策略（可选，缺省 skip 跳过；run 启动后立即补跑一次）" },
      timeout_ms: { type: "number", description: "单次执行超时毫秒（可选，缺省脚本 300000 / 提示词 1800000）" },
      max_consecutive_errors: { type: "number", description: "连续失败自动停用阈值（可选，缺省 0 不停用；建议无人值守任务配 3~10）" },
      notify_on: { enum: ["always", "error"], description: "通知时机（可选，缺省 always；error=仅失败时通知）" },
      notify: notifyParam(),
      enabled: { type: "boolean", description: "是否启用（默认 true）" },
    },
    ["schedule", "type"],
  ),
  async execute(args, ctx) {
    if (!ctx.cron) return { output: "定时任务能力未启用（服务端配置 GEBAI_CRON_ENABLED=false 关闭）。" }
    const task = await ctx.cron.add(
      {
        name: args.name != null ? String(args.name) : undefined,
        type: String(args.type) as "script" | "prompt",
        schedule: String(args.schedule ?? ""),
        script: args.script != null ? String(args.script) : undefined,
        prompt: args.prompt != null ? String(args.prompt) : undefined,
        target: args.target != null ? (String(args.target) as "ephemeral" | "sticky" | "session") : undefined,
        sessionId: args.session_id != null ? String(args.session_id) : undefined,
        agents: parseAgents(args.agents),
        timezone: args.timezone != null ? String(args.timezone) : undefined,
        misfire: args.misfire != null ? (String(args.misfire) as "skip" | "run") : undefined,
        timeoutMs: args.timeout_ms != null ? Number(args.timeout_ms) : undefined,
        maxConsecutiveErrors: args.max_consecutive_errors != null ? Number(args.max_consecutive_errors) : undefined,
        notifyOn: args.notify_on != null ? (String(args.notify_on) as "always" | "error") : undefined,
        notify: parseNotify(args.notify),
        enabled: args.enabled === undefined ? undefined : Boolean(args.enabled),
      },
      ctx.sessionId,
    )
    const notifyNote = task.notify?.length ? `\n通知: ${task.notify.length} 个通道（${task.notifyOn ?? "always"}）` : ""
    return {
      output: `定时任务已创建: ${task.id}${task.name ? `（${task.name}）` : ""}\n类型: ${task.type === "script" ? "脚本运行" : `提示词运行 agent（${describeTarget(task)}）`}\n周期: ${task.schedule}${task.timezone ? `（${task.timezone}）` : ""}\n下次执行: ${new Date(task.nextRunAt).toString()}${notifyNote}\n可用 cron_list 查看、cron_trigger 立即试跑、cron_update 修改、cron_remove 删除。`,
    }
  },
}

const list: Tool = {
  name: "list",
  description: "查看当前用户的定时任务列表（用户级：含其他会话创建的任务；ID/名称/类型/周期/目标/启用状态/上次与下次执行时间/执行次数/最近错误/最近运行历史）。",
  parameters: schema({}),
  async execute(_args, ctx) {
    if (!ctx.cron) return { output: "定时任务能力未启用（服务端配置 GEBAI_CRON_ENABLED=false 关闭）。" }
    const tasks = await ctx.cron.list()
    if (!tasks.length) return { output: "当前用户暂无定时任务。" }
    return {
      output: tasks
        .map((t) => {
          const body = t.type === "script" ? `命令: ${t.script}` : `提示词: ${t.prompt}`
          const last = t.lastRunAt ? `${new Date(t.lastRunAt).toString()}（${t.lastStatus ?? "-"}）` : "未执行"
          const next = t.enabled ? new Date(t.nextRunAt).toString() : "-（已停用）"
          const extras: string[] = []
          if (t.timezone) extras.push(`时区: ${t.timezone}`)
          if (t.misfire === "run") extras.push("错过补跑")
          if (t.timeoutMs) extras.push(`超时: ${Math.round(t.timeoutMs / 1000)}s`)
          if (t.maxConsecutiveErrors) extras.push(`连续失败停用: ${t.maxConsecutiveErrors} 次（当前 ${t.consecutiveErrors ?? 0}）`)
          if (t.notify?.length) extras.push(`通知: ${t.notify.map((n) => n.type).join("+")}（${t.notifyOn ?? "always"}）${t.lastNotifyError ? ` 最近通知失败: ${t.lastNotifyError}` : ""}`)
          const runs = (t.runs ?? []).slice(0, 3).map((r) => `${new Date(r.at).toLocaleString("zh-CN")} ${r.status}${r.manual ? "（手动）" : ""}${r.error ? `: ${r.error.slice(0, 120)}` : ""}`)
          return `- ${t.id}${t.name ? `（${t.name}）` : ""} [${t.enabled ? "启用" : "停用"}] 类型: ${t.type === "script" ? "脚本运行" : `提示词运行 agent（${describeTarget(t)}）`} 周期: ${t.schedule}\n  ${body}\n  上次: ${last} 下次: ${next} 次数: ${t.runCount}${extras.length ? `\n  ${extras.join("；")}` : ""}${t.lastError ? `\n  最近错误: ${t.lastError}` : ""}${runs.length ? `\n  最近运行: ${runs.join("；")}` : ""}`
        })
        .join("\n"),
    }
  },
}

const update: Tool = {
  name: "update",
  description: "修改定时任务（按 id：改启用状态/周期/类型/内容/执行目标/通知通道/可靠性参数），修改后下次执行时间自动重算；notify 的 secret 传 *** 保持原值。",
  requiresApproval: true,
  parameters: schema(
    {
      id: { type: "string", description: "任务 ID（cron_list 查看）" },
      enabled: { type: "boolean", description: "启用/停用（重新启用会重置连续失败计数）" },
      name: { type: "string", description: "新的任务名称" },
      schedule: { type: "string", description: "新的定时表达式" },
      type: { enum: ["script", "prompt"], description: "新的任务类型" },
      script: { type: "string", description: "type=script：新的 shell 命令" },
      prompt: { type: "string", description: "type=prompt：新的提示词" },
      target: { enum: ["ephemeral", "sticky", "session"], description: "新的执行目标（type=prompt）" },
      session_id: { type: "string", description: "target=session 绑定的会话 id" },
      agents: { type: "array", items: { type: "string" }, description: "预载子Agent 名单（ephemeral/sticky）" },
      timezone: { type: "string", description: "新的时区（空串清除=用服务器本地时区）" },
      misfire: { enum: ["skip", "run"], description: "错过补跑策略" },
      timeout_ms: { type: "number", description: "单次执行超时毫秒" },
      max_consecutive_errors: { type: "number", description: "连续失败自动停用阈值（0=不停用）" },
      notify_on: { enum: ["always", "error"], description: "通知时机" },
      notify: notifyParam(),
    },
    ["id"],
  ),
  async execute(args, ctx) {
    if (!ctx.cron) return { output: "定时任务能力未启用（服务端配置 GEBAI_CRON_ENABLED=false 关闭）。" }
    const task = await ctx.cron.update(String(args.id), {
      enabled: args.enabled === undefined ? undefined : Boolean(args.enabled),
      name: args.name != null ? String(args.name) : undefined,
      schedule: args.schedule != null ? String(args.schedule) : undefined,
      type: args.type != null ? (String(args.type) as "script" | "prompt") : undefined,
      script: args.script != null ? String(args.script) : undefined,
      prompt: args.prompt != null ? String(args.prompt) : undefined,
      target: args.target != null ? (String(args.target) as "ephemeral" | "sticky" | "session") : undefined,
      sessionId: args.session_id != null ? String(args.session_id) : undefined,
      agents: parseAgents(args.agents),
      timezone: args.timezone != null ? String(args.timezone) : undefined,
      misfire: args.misfire != null ? (String(args.misfire) as "skip" | "run") : undefined,
      timeoutMs: args.timeout_ms != null ? Number(args.timeout_ms) : undefined,
      maxConsecutiveErrors: args.max_consecutive_errors != null ? Number(args.max_consecutive_errors) : undefined,
      notifyOn: args.notify_on != null ? (String(args.notify_on) as "always" | "error") : undefined,
      notify: parseNotify(args.notify),
    })
    if (!task) return { output: `定时任务不存在: ${args.id}` }
    return { output: `定时任务已更新: ${task.id}${task.name ? `（${task.name}）` : ""} [${task.enabled ? "启用" : "停用"}] 周期: ${task.schedule}${task.timezone ? `（${task.timezone}）` : ""}\n下次执行: ${task.enabled ? new Date(task.nextRunAt).toString() : "-（已停用）"}` }
  },
}

const trigger: Tool = {
  name: "trigger",
  description: "手动立即触发一次定时任务（验证任务配置/立即执行；不改动既定调度节奏——nextRunAt 不变，一次性 @at 任务触发后仍自动停用）。",
  requiresApproval: true,
  parameters: schema({ id: { type: "string", description: "任务 ID（cron_list 查看）" } }, ["id"]),
  async execute(args, ctx) {
    if (!ctx.cron) return { output: "定时任务能力未启用（服务端配置 GEBAI_CRON_ENABLED=false 关闭）。" }
    const task = await ctx.cron.trigger(String(args.id))
    if (!task) return { output: `定时任务不存在: ${args.id}` }
    const run = task.runs?.[0]
    return {
      output: `定时任务已手动执行: ${task.id}${task.name ? `（${task.name}）` : ""}\n结果: ${run ? `${run.status}${run.error ? `（${run.error.slice(0, 300)}）` : ""}${run.sessionId ? `，执行会话: ${run.sessionId}` : ""}${run.output ? `\n输出摘要: ${run.output.slice(0, 800)}` : ""}` : task.lastStatus ?? "-"}\n既定下次执行: ${task.enabled ? new Date(task.nextRunAt).toString() : "-（已停用）"}`,
    }
  },
}

const remove: Tool = {
  name: "remove",
  description: "删除定时任务（按 id，不可恢复；任务工作目录文件保留）。",
  requiresApproval: true,
  parameters: schema({ id: { type: "string" } }, ["id"]),
  async execute(args, ctx) {
    if (!ctx.cron) return { output: "定时任务能力未启用（服务端配置 GEBAI_CRON_ENABLED=false 关闭）。" }
    const removed = await ctx.cron.remove(String(args.id))
    return removed ? { output: `定时任务已删除: ${args.id}` } : { output: `定时任务不存在: ${args.id}` }
  },
}

export const tools: Record<string, Tool> = { add, list, update, trigger, remove }
export const requiresApproval = { add: true, update: true, remove: true, trigger: true }
export const preload = false
export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
