import type { ToolSchema } from "@gebai/sdk"
import type { SubAgentDef, Tool } from "@gebai/sdk"
import type { TaskNotifyChannel } from "@gebai/sdk"

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

export const name = "task"
export const description =
  "统一任务管理（定时/普通/闲时三类任务的无人值守执行）：创建（脚本运行 / 提示词运行 agent）/查看/修改/手动执行/取消/删除/资源文件管理/主动通知，任务为用户级资源（不随会话删除消失）。支持任务级环境变量（服务端持久化，任务触发不依赖浏览器本地注入）、无人值守执行免审批（含服务模式）。三类任务共用一条队列：定时任务到期自动插队首，普通任务入队按序执行，闲时任务仅在队列空闲时串行执行；并发额度默认 5（GEBAI_TASK_MAX_CONCURRENT）。" +
  "需要周期性脚本、批量任务、无人值守 Agent 任务时装载。输入：任务需求或管理指令。"
export const systemPrompt =
  "你是任务管理助手。任务 = 用户级无人值守执行单元（持久化于用户目录 tasks.json，与会话生命周期解耦——会话删除后任务仍在），三类由 kind 区分：\n" +
  "1) kind=scheduled（定时任务）：按 schedule 表达式到期自动执行；\n" +
  "2) kind=manual（普通任务）：创建即入队（run_now，缺省 true）按顺序执行，也可随时 task_run 再次入队；\n" +
  "3) kind=idle（闲时任务）：仅在队列空闲（无排队/运行的定时与普通任务）且该用户没有运行中的会话时执行，同时只跑一个。\n" +
  "工具经本子Agent 命名空间暴露：task_add 创建、task_list 查看、task_update 修改、task_run 手动执行、task_cancel 取消/终止、task_remove 删除、task_files 任务资源文件、task_notify 主动推送通知。\n" +
  "执行体 runner 二选一（三类任务通用）：\n" +
  "- script（脚本运行）：shell 命令在**任务资源目录**（users/{用户}/tasks/{任务id}/）以用户环境执行，产物写在该目录跨次保留，结果写入任务历史并可选通知；\n" +
  "- prompt（提示词运行 agent）：以给定提示词触发一次完整 Agent 会话。\n" +
  "prompt 型执行目标 target：ephemeral（缺省，每次执行新建独立会话，上下文不累积——例行检查/报告首选）、sticky（专用会话跨次复用，上下文延续——需要延续记忆的任务用）、session（绑定既有会话执行，缺省为创建任务时的当前会话）；ephemeral/sticky 可配 agents 预载子Agent 名单。执行会话的交互能力随目标不同：ephemeral/sticky 为**无人值守**（无交互通道）——需审批工具**自动通过**（含服务模式），ask 询问与前端渲染/页面捕获不可用；target=session 绑定用户会话（可能有人在场）保持实时交互与实时审批。\n" +
  "定时表达式 schedule（kind=scheduled 必填）：5 段 cron（分 时 日 月 周，如 0 9 * * * 每天 9:00）、@every 30m、@daily/@hourly/@weekly/@monthly、@at 2026-09-01T09:00（一次性，入队后自动停用）；可配 timezone（IANA 名如 Asia/Shanghai，缺省服务器本地时区）；非法表达式创建即拒绝。\n" +
  "队列与度：所有手动执行（task_run）默认排普通任务队尾（front=true 置顶）；定时任务到期自动插队首；额度满或目标会话忙时任务排队等待（运行中的任务不会被中断让出额度）；队列视图用 task_list 的队列信息或 REST /api/v1/tasks/queue 查看。\n" +
  "通知 notify（无人值守任务建议配置）：通道数组，每条 {type,target,webhook_id,secret,at}——type=webhook（任意 http(s) 回调 POST JSON，可直配 target URL 或以 webhook_id 引用 REST /api/v1/webhooks 已注册的事件 Webhook——投递自动带注册密钥的 X-Gebai-Signature HMAC 签名）、feishu（群机器人 webhook 地址，或直接填群 chat_id（oc_ 前缀）以应用身份推送指定群——后者需服务端配置飞书应用凭证，chat_id 可装载 feishu_group 子Agent 用 chats_list 查询；secret 为加签密钥可选）、feishu_chat（同 feishu 的 chat_id 形态）；飞书默认以 markdown 卡片发送，可配 at 名单 @特定人（open_id，或 \"all\"=@所有人——at 含 all 时自动降级文本消息）；通知时机 notify_on（两种）：auto（缺省，执行结束自动把最后回复/输出作为通知发出）/ model（调度器不自动发，由执行会话的模型用 task_notify 决定）。服务端可配全局默认通道（GEBAI_TASK_NOTIFY_WEBHOOK / GEBAI_TASK_NOTIFY_FEISHU），任务未配 notify 时自动走全局通道（自配则不叠加）；用户未要求特定通道且未拒绝通知时可不传 notify。\n" +
  "通知由谁决定（notify_on，两种）：auto（缺省，执行结束自动把最后回复/输出作为通知发出）/ model（调度器不自动发——通知完全由执行会话的模型用 task_notify 决定与撑写，例行正常保持静默、异常/需用户知晓时主动推送）。任何模式下模型都可用 task_notify 主动补充通知；无可用通道时通知不可用（任务配 notify 或服务端配全局默认通道）。prompt 型任务有可用通道时，执行会话自动预载本子Agent 并在触发消息里注入任务 ID——执行任务期间不要用 task 的其它工具管理任务。\n" +
  "可靠性参数：misfire=skip（缺省，停机错过即跳过）/run（启动后立即补跑一次）；timeoutMs 单次执行超时（缺省脚本 5 分钟、提示词 30 分钟，到时终止）；maxConsecutiveErrors 连续失败 N 次自动停用（防错误任务无限重试刷屏，建议通知类任务配置如 5）。\n" +
  "任务级环境变量 env（可选，两类执行体通用）：服务端持久化于任务定义，每次执行注入——脚本型注进子进程环境、提示词型注入执行会话的任务 env（模型 Provider 与子Agent 环境读取一并生效），优先级高于进程全局与会话环境。这是无人值守任务获得配置的**正路**：浏览器本地 env 只随交互会话的 prompt 到达服务端，任务触发时无人发包，不注入即拿不到。敏感键名（含 KEY/TOKEN/SECRET/PASSWORD 等）的**值加密后落盘**（AES-256-GCM，密钥内置），回显时为掩码 `***`——掩码原样回传即保留原值（不改动就不要填新值）。加密只防「随手看到」（文件被浏览/被 grep/随备份外带/被模型读进上下文），不防能读取整个数据目录的攻击者；强度要求更高时把配置写进任务资源目录由脚本自行加载。\n" +
  "资源文件：每个任务有独立资源目录，脚本型任务的工作目录即它（相对路径直接读写），文档/配置放这里跨次保留。用 task_files 列目录/读/写/删；也可用通用文件工具直接操作该目录（task_list 输出含目录路径）。\n" +
  "执行记录：每次执行（含定时到期未启动的 skipped）各存一个文件：users/{用户}/task-runs/{任务ID}/{时间}.json（UTC ISO 为名，内容为完整记录：状态/耗时/输出/错误/执行会话）；任务定义文件不含记录，按时间倒序最多保留 200 条（超出删最旧）。需要回看历史时读该目录（或 REST GET /api/v1/tasks/:id/runs?limit=）。\n" +
  "工作要点：\n" +
  "1) 创建（task_add，需审批）：先确认 kind/runner，prompt 型说明执行目标与是否预载子Agent；脚本型把脚本写入资源目录后在 script 里引用（如 `bash run.sh`）；\n" +
  "2) 查看（task_list）：列出任务（ID/名称/类别/执行体/启用状态/运行态/上次与下次执行/次数/最近错误）；\n" +
  "3) 修改（task_update，需审批）：按 id 改名称/启用状态/表达式/内容/目标/通知等；\n" +
  "4) 手动执行（task_run，需审批）：立即入队执行一次用于验证或临时需要（不改动既定调度节奏），front=true 插队；\n" +
  "5) 取消（task_cancel，需审批）：排队中的执行可出队；正在运行的可终止（脚本型任务需等其自身结束或超时）；\n" +
  "6) 删除（task_remove，需审批）：按 id 删除，不可恢复，删前向用户确认；\n" +
  "7) 主动通知（task_notify，无需审批）：把自撰 markdown 正文推到任务的通知通道（id 缺省=当前正在执行的任务）；仅当用户需要知晓时才推送（结论先行、简明），例行正常保持静默；\n" +
  "8) 环境变量（env 参数，创建/修改时传）：任务需要脱离当前会话独立运行（定时触发/无人值守）时，把依赖的配置随任务一起创建或更新——不要指望浏览器本地 env。\n" +
  "任务为用户级资源：任何会话创建后全局可见可管（跨会话不再隔离）；创建/修改/删除/手动执行/取消均需用户审批（任务 = 无人值守的任意命令/会话执行）。无人值守执行的需审批工具自动通过（含服务模式），管理动作仍需逐次审批。用户级待办（todo）是独立的清单资源，其闲时自动执行会绑定一个闲时任务，但待办本身不经本子Agent 管理。"

function notifyParam(): Record<string, unknown> {
  return {
    type: "array",
    description:
      "通知通道数组（可选）：每条 {type,target,webhook_id,secret,at}——webhook（http(s) 回调 URL，或 webhook_id 引用 REST /api/v1/webhooks 已注册的事件 Webhook——投递自动带其 HMAC 签名，二选一）/ feishu（飞书群自定义机器人 webhook 地址，或群 chat_id）/ feishu_chat（飞书 chat_id，需服务端飞书应用凭证）；secret 为密钥（feishu 加签 / webhook 直配时 HMAC 签名；修改时传 *** 表示保持不变）；at 为 @ 人名单（飞书渲染进卡片；webhook 随 JSON 载荷 at 字段携带），通知以 markdown 卡片发送（webhook 为 JSON）",
    items: {
      type: "object",
      properties: {
        type: { enum: ["webhook", "feishu", "feishu_chat"], description: "通道类型" },
        target: { type: "string", description: "webhook=URL；feishu=群机器人 webhook URL 或群 chat_id（oc_ 前缀，应用身份推送指定群）；feishu_chat=群 chat_id；webhook 用 webhook_id 引用时可省略" },
        webhook_id: { type: "string", description: "引用 REST /api/v1/webhooks 注册的事件 Webhook id（仅 webhook 通道，与 target 二选一，投递带注册密钥的 X-Gebai-Signature 签名）" },
        secret: { type: "string", description: "密钥（可选；feishu 加签 / webhook 直配 URL 时 HMAC 签名；修改时传 *** 表示保持不变）" },
        at: {
          type: "array",
          description: "@ 人名单（open_id 如 ou_xxx，或 \"all\"=@所有人；可传字符串数组或 {id,name} 数组）",
          items: { type: "string" },
        },
      },
      required: ["type"],
    },
  }
}

/** 任务类别语义说明（工具描述复用）。 */
const KIND_NOTE = "任务类别：scheduled=定时（到期自动执行，需 schedule）；manual=普通（创建即入队，可再次手动执行）；idle=闲时（队列空闲且无运行中会话时串行执行）"

function parseAgents(raw: unknown): string[] | undefined {
  if (raw === undefined || raw === null) return undefined
  const list = Array.isArray(raw) ? raw : [raw]
  const out = list.map((a) => String(a ?? "").trim()).filter(Boolean)
  return out.length ? out : undefined
}

/** 任务级环境变量参数归一：值为字符串化（数字/布尔也接受，模型常把端口写成数字）；非对象报错。 */
function parseEnv(raw: unknown): Record<string, string> | undefined {
  if (raw === undefined || raw === null) return undefined
  if (typeof raw !== "object" || Array.isArray(raw)) throw new Error("env 须为 { 变量名: 值 } 对象")
  const out: Record<string, string> = {}
  for (const [k, v] of Object.entries(raw as Record<string, unknown>)) {
    if (v === null || v === undefined) continue
    out[k] = typeof v === "string" ? v : String(v)
  }
  return out
}

/** env 参数 schema（add/update 共用）。 */
function envParam(): Record<string, unknown> {
  return {
    type: "object",
    additionalProperties: { type: "string" },
    description:
      "任务级环境变量（可选，服务端持久化于任务定义，每次执行注入）：脚本型注进子进程环境、提示词型注入执行会话的任务 env（模型与子Agent 一并生效），优先级高于进程全局与会话环境；无人值守运行不依赖浏览器本地注入。敏感键名的值加密落盘（AES-256-GCM）、回显为掩码 `***`——**掩码原样回传即保留原值**（只改其它项时不要为敏感项填新值）；传空对象 {} 清除。",
  }
}

function parseNotify(raw: unknown): TaskNotifyChannel[] | undefined {
  if (raw === undefined || raw === null) return undefined
  const list = Array.isArray(raw) ? raw : [raw]
  const out: TaskNotifyChannel[] = []
  for (const item of list) {
    if (!item || typeof item !== "object") continue
    const ch = item as Record<string, unknown>
    out.push({
      type: String(ch.type ?? "webhook") as TaskNotifyChannel["type"],
      ...(ch.target != null ? { target: String(ch.target) } : {}),
      ...(ch.webhook_id != null ? { webhookId: String(ch.webhook_id) } : {}),
      ...(ch.secret != null ? { secret: String(ch.secret) } : {}),
      ...(ch.at !== undefined ? { at: (Array.isArray(ch.at) ? ch.at : [ch.at]) as never } : {}),
    })
  }
  return out.length ? out : undefined
}

/** 任务一行摘要（列表与执行结果回显共用）。 */
function taskLine(t: { id: string; name?: string; kind: string; runner: string; enabled: boolean; state: string; schedule?: string; nextRunAt?: number; lastStatus?: string; runCount: number; lastError?: string }): string {
  const kind = t.kind === "scheduled" ? "定时" : t.kind === "idle" ? "闲时" : "普通"
  const state = t.state === "running" ? "运行中" : t.state === "queued" ? "排队中" : "空闲"
  const parts = [
    `${t.id}${t.name ? `（${t.name}）` : ""}`,
    `${kind}/${t.runner}`,
    t.enabled ? "启用" : "停用",
    state,
  ]
  if (t.kind === "scheduled" && t.schedule) parts.push(`周期: ${t.schedule}`)
  if (t.enabled && t.nextRunAt) parts.push(`下次: ${new Date(t.nextRunAt).toLocaleString("zh-CN")}`)
  parts.push(`已运行 ${t.runCount} 次`)
  if (t.lastStatus) parts.push(`上次: ${t.lastStatus}`)
  if (t.lastError) parts.push(`最近错误: ${t.lastError.slice(0, 200)}`)
  return parts.join(" | ")
}

const add: Tool = {
  name: "add",
  description:
    "创建任务（需审批）。" +
    KIND_NOTE +
    "。runner 二选一：script（shell 在任务资源目录执行）/ prompt（提示词触发一次 Agent 会话）。kind=scheduled 必须给 schedule（5 段 cron / @every 30m / @daily / @at 2026-09-01T09:00）；kind=manual 缺省创建即入队（run_now=false 则只创建）；kind=idle 在队列空闲时执行。可配 timezone/misfire/timeout_ms/max_consecutive_errors/env/notify 与 prompt 型 target（ephemeral/sticky/session）+ agents。",
  requiresApproval: true,
  parameters: schema(
    {
      kind: { enum: ["scheduled", "manual", "idle"], description: "任务类别（缺省：给了 schedule 视为 scheduled，否则 manual）" },
      runner: { enum: ["script", "prompt"], description: "执行体（必填）" },
      name: { type: "string", description: "任务名（列表展示用）" },
      schedule: { type: "string", description: "kind=scheduled 执行表达式（5 段 cron / @every 30m / @daily / @at <时间>）" },
      timezone: { type: "string", description: "IANA 时区（如 Asia/Shanghai；缺省服务器本地时区）" },
      misfire: { enum: ["skip", "run"], description: "停机错过触发的补跑策略（缺省 skip）" },
      script: { type: "string", description: "runner=script：shell 命令（在任务资源目录执行）" },
      prompt: { type: "string", description: "runner=prompt：触发 agent 运行的提示词" },
      target: { enum: ["ephemeral", "sticky", "session"], description: "runner=prompt 执行目标（缺省 ephemeral=每次新建会话；sticky=专用会话复用；session=绑定既有会话）" },
      session_id: { type: "string", description: "target=session 绑定的会话 id（缺省=当前会话）" },
      agents: { type: "array", description: "target=ephemeral/sticky 的预载子Agent 名单", items: { type: "string" } },
      timeout_ms: { type: "number", description: "单次执行超时毫秒（缺省脚本 5 分钟 / 提示词 30 分钟）" },
      env: envParam(),
      max_consecutive_errors: { type: "number", description: "连续失败 N 次自动停用（0=不停用）" },
      notify_on: { enum: ["auto", "model"], description: "通知时机（缺省 auto=执行结束自动发；model=由执行会话的模型用 task_notify 决定）" },
      notify: notifyParam(),
      enabled: { type: "boolean", description: "是否启用（缺省 true）" },
      run_now: { type: "boolean", description: "kind=manual：创建即入队执行一次（缺省 true）" },
      front: { type: "boolean", description: "入队时置顶（排到普通任务之前）" },
    },
    ["runner"],
  ),
  async execute(args, ctx) {
    if (!ctx.tasks) return { output: "任务能力未启用（服务端配置 GEBAI_TASKS_ENABLED=false 关闭）。" }
    const task = await ctx.tasks.add({
      kind: args.kind != null ? (String(args.kind) as "scheduled" | "manual" | "idle") : undefined,
      runner: String(args.runner) as "script" | "prompt",
      name: args.name != null ? String(args.name) : undefined,
      schedule: args.schedule != null ? String(args.schedule) : undefined,
      timezone: args.timezone != null ? String(args.timezone) : undefined,
      misfire: args.misfire != null ? (String(args.misfire) as "skip" | "run") : undefined,
      script: args.script != null ? String(args.script) : undefined,
      prompt: args.prompt != null ? String(args.prompt) : undefined,
      target: args.target != null ? (String(args.target) as "ephemeral" | "sticky" | "session") : undefined,
      sessionId: args.session_id != null ? String(args.session_id) : undefined,
      agents: parseAgents(args.agents),
      timeoutMs: args.timeout_ms != null ? Number(args.timeout_ms) : undefined,
      env: parseEnv(args.env),
      maxConsecutiveErrors: args.max_consecutive_errors != null ? Number(args.max_consecutive_errors) : undefined,
      notifyOn: args.notify_on != null ? (String(args.notify_on) as "auto" | "model") : undefined,
      notify: parseNotify(args.notify),
      enabled: args.enabled === undefined ? undefined : Boolean(args.enabled),
      runNow: args.run_now === undefined ? undefined : Boolean(args.run_now),
      front: args.front === true,
    })
    return { output: `任务已创建:\n${taskLine(task)}\n资源目录可用 task_files（id: ${task.id}）管理脚本与文档。` }
  },
}

const list: Tool = {
  name: "list",
  description: "列出当前用户的任务（ID/名称/类别/执行体/启用状态/运行态/周期/下次执行/次数/最近错误），并返回队列概览（并发额度、排队顺序、运行中）。",
  safeMode: true,
  parameters: schema({
    kind: { enum: ["scheduled", "manual", "idle"], description: "只看某一类别（缺省全部）" },
    state: { enum: ["idle", "queued", "running"], description: "只看某一运行态（缺省全部）" },
  }),
  async execute(args, ctx) {
    if (!ctx.tasks) return { output: "任务能力未启用（服务端配置 GEBAI_TASKS_ENABLED=false 关闭）。" }
    const all = await ctx.tasks.list()
    const kind = args.kind != null ? String(args.kind) : undefined
    const state = args.state != null ? String(args.state) : undefined
    const tasks = all.filter((t) => (!kind || t.kind === kind) && (!state || t.state === state))
    const q = await ctx.tasks.queue()
    const lines: string[] = []
    lines.push(`并发额度: ${q.running.length}/${q.limit}（运行中/上限）`)
    if (q.running.length) lines.push(`运行中:\n${q.running.map((r) => `  - ${r.taskId}${r.name ? `（${r.name}）` : ""}${r.sessionId ? ` 会话 ${r.sessionId}` : ""}`).join("\n")}`)
    if (q.entries.length) lines.push(`排队中（按执行顺序）:\n${q.entries.map((e) => `  ${e.position}. ${e.taskId}${e.name ? `（${e.name}）` : ""} [${e.kind}] 来源 ${e.source}${e.front ? " 置顶" : ""}${e.waiting ? `（${e.waiting}）` : ""}`).join("\n")}`)
    lines.push(tasks.length ? `任务清单（${tasks.length}）:\n${tasks.map((t) => `- ${taskLine(t)}`).join("\n")}` : "任务清单为空。")
    return { output: lines.join("\n") }
  },
}

const update: Tool = {
  name: "update",
  description: "修改任务配置（需审批）：名称/启用状态/执行体与内容/定时表达式与时区/补跑策略/执行目标与预载子Agent/超时/环境变量/通知/连续失败阈值。仅传需要修改的字段；env 传空对象 {} 清除。",
  requiresApproval: true,
  parameters: schema(
    {
      id: { type: "string", description: "任务 ID（task_list 查看）" },
      name: { type: "string", description: "任务名" },
      enabled: { type: "boolean", description: "启用/停用（停用即退出调度与队列）" },
      runner: { enum: ["script", "prompt"], description: "改执行体" },
      script: { type: "string", description: "脚本内容（runner=script）" },
      prompt: { type: "string", description: "提示词（runner=prompt）" },
      schedule: { type: "string", description: "新执行表达式（仅 kind=scheduled）" },
      timezone: { type: "string", description: "新时区（空串清除=用服务器本地时区）" },
      misfire: { enum: ["skip", "run"], description: "错过补跑策略" },
      target: { enum: ["ephemeral", "sticky", "session"], description: "执行目标" },
      session_id: { type: "string", description: "target=session 绑定的会话 id" },
      agents: { type: "array", description: "预载子Agent 名单（空数组清除）", items: { type: "string" } },
      timeout_ms: { type: "number", description: "单次执行超时毫秒" },
      env: envParam(),
      max_consecutive_errors: { type: "number", description: "连续失败自动停用阈值（0=不停用）" },
      notify_on: { enum: ["auto", "model"], description: "通知时机（auto=执行结束自动发 / model=由执行会话的模型用 task_notify 决定）" },
      notify: notifyParam(),
    },
    ["id"],
  ),
  async execute(args, ctx) {
    if (!ctx.tasks) return { output: "任务能力未启用（服务端配置 GEBAI_TASKS_ENABLED=false 关闭）。" }
    const task = await ctx.tasks.update(String(args.id), {
      name: args.name != null ? String(args.name) : undefined,
      enabled: args.enabled === undefined ? undefined : Boolean(args.enabled),
      runner: args.runner != null ? (String(args.runner) as "script" | "prompt") : undefined,
      script: args.script != null ? String(args.script) : undefined,
      prompt: args.prompt != null ? String(args.prompt) : undefined,
      schedule: args.schedule != null ? String(args.schedule) : undefined,
      timezone: args.timezone != null ? String(args.timezone) : undefined,
      misfire: args.misfire != null ? (String(args.misfire) as "skip" | "run") : undefined,
      target: args.target != null ? (String(args.target) as "ephemeral" | "sticky" | "session") : undefined,
      sessionId: args.session_id != null ? String(args.session_id) : undefined,
      agents: args.agents !== undefined ? parseAgents(args.agents) : undefined,
      timeoutMs: args.timeout_ms != null ? Number(args.timeout_ms) : undefined,
      env: args.env !== undefined ? parseEnv(args.env) : undefined,
      maxConsecutiveErrors: args.max_consecutive_errors != null ? Number(args.max_consecutive_errors) : undefined,
      notifyOn: args.notify_on != null ? (String(args.notify_on) as "auto" | "model") : undefined,
      notify: parseNotify(args.notify),
    })
    if (!task) return { output: `任务不存在: ${args.id}` }
    return { output: `任务已更新:\n${taskLine(task)}` }
  },
}
const run: Tool = {
  name: "run",
  description:
    "手动执行任务（需审批）：立即入队跑一次（不改动既定调度节奏；定时任务的 nextRunAt 不变）。默认排普通任务队尾，front=true 置顶（定时任务到期仍自动插队首）。额度满或目标会话忙时排队等待，不会中断正在运行的任务。",
  requiresApproval: true,
  parameters: schema(
    {
      id: { type: "string", description: "任务 ID（task_list 查看）" },
      front: { type: "boolean", description: "置顶（排到普通任务之前）" },
    },
    ["id"],
  ),
  async execute(args, ctx) {
    if (!ctx.tasks) return { output: "任务能力未启用（服务端配置 GEBAI_TASKS_ENABLED=false 关闭）。" }
    const res = await ctx.tasks.run(String(args.id), { front: args.front === true })
    if (!res) return { output: `任务不存在: ${args.id}` }
    if (!res.queued) return { output: `任务未入队: ${args.id}（${res.reason ?? "未知原因"}）` }
    const pos = res.position ? `队列位置: ${res.position}` : ""
    return {
      output: `任务已入队执行: ${res.task.id}${res.task.name ? `（${res.task.name}）` : ""}\n类别: ${res.task.kind} | 执行体: ${res.task.runner} | ${pos}\n执行会新建/复用会话（prompt 型）或直接在任务资源目录执行脚本；结束后可用 task_list 查看结果与执行会话。`,
    }
  },
}

const cancel: Tool = {
  name: "cancel",
  description:
    "取消任务的执行（需审批）：mode=dequeue（缺省）把排队中的执行出队；mode=stop 终止正在运行的那次执行（提示词型会取消其执行会话；脚本型需等其自身结束或超时）。不影响任务本身与既定调度。",
  requiresApproval: true,
  parameters: schema(
    {
      id: { type: "string", description: "任务 ID（task_list 查看）" },
      mode: { enum: ["dequeue", "stop"], description: "dequeue=出队（排队中）；stop=终止运行中的执行（缺省 dequeue）" },
    },
    ["id"],
  ),
  async execute(args, ctx) {
    if (!ctx.tasks) return { output: "任务能力未启用（服务端配置 GEBAI_TASKS_ENABLED=false 关闭）。" }
    const id = String(args.id)
    if (String(args.mode ?? "dequeue") === "stop") {
      const ok = await ctx.tasks.stop(id)
      return { output: ok ? `已请求终止运行中的任务: ${id}（脚本型任务需等其自身结束或超时）` : `任务未在运行中: ${id}` }
    }
    const ok = await ctx.tasks.cancel(id)
    return { output: ok ? `已出队: ${id}` : `任务不在排队中: ${id}` }
  },
}

const remove: Tool = {
  name: "remove",
  description: "删除任务（需审批，按 id，不可恢复；任务资源目录文件保留）。",
  requiresApproval: true,
  parameters: schema({ id: { type: "string" } }, ["id"]),
  async execute(args, ctx) {
    if (!ctx.tasks) return { output: "任务能力未启用（服务端配置 GEBAI_TASKS_ENABLED=false 关闭）。" }
    const removed = await ctx.tasks.remove(String(args.id))
    return removed ? { output: `任务已删除: ${args.id}（资源目录文件未删除）` } : { output: `任务不存在: ${args.id}` }
  },
}

const files: Tool = {
  name: "files",
  description:
    "任务资源文件管理（脚本/文档/配置，需审批）：op=list 列出任务资源目录（含脚本型任务的运行目录）、op=read 读文件、op=write 写文件（覆盖，父目录自动创建）、op=delete 删除文件或目录。路径为相对任务目录的相对路径，越界拒绝。",
  requiresApproval: true,
  parameters: schema(
    {
      id: { type: "string", description: "任务 ID（task_list 查看）" },
      op: { enum: ["list", "read", "write", "delete"], description: "操作类型" },
      path: { type: "string", description: "相对任务资源目录的路径（list 可省略）" },
      content: { type: "string", description: "op=write 的文件内容（覆盖写入）" },
    },
    ["id", "op"],
  ),
  async execute(args, ctx) {
    if (!ctx.tasks) return { output: "任务能力未启用（服务端配置 GEBAI_TASKS_ENABLED=false 关闭）。" }
    const id = String(args.id)
    const op = String(args.op)
    const path = args.path != null ? String(args.path) : ""
    if (op === "list") {
      const list = await ctx.tasks.files(id)
      if (!list.length) return { output: `任务资源目录为空（id: ${id}）。用 op=write 写入脚本或文档。` }
      const lines = list.map((f) => `${f.dir ? "[目录] " : "       "}${f.path}${f.dir ? "" : `  ${f.size} 字节`}`)
      return { output: `任务资源目录（相对路径）:\n${lines.join("\n")}` }
    }
    if (op === "read") {
      if (!path) return { output: "缺少 path（op=read 需要文件路径）" }
      const content = await ctx.tasks.readFile(id, path)
      return { output: `文件 ${path}:\n${content}` }
    }
    if (op === "write") {
      if (!path) return { output: "缺少 path（op=write 需要文件路径）" }
      const entry = await ctx.tasks.writeFile(id, path, String(args.content ?? ""))
      return { output: `已写入任务资源文件: ${entry.path}（${entry.size} 字节）` }
    }
    if (op === "delete") {
      if (!path) return { output: "缺少 path（op=delete 需要路径）" }
      const ok = await ctx.tasks.deleteFile(id, path)
      return { output: ok ? `已删除: ${path}` : `不存在: ${path}` }
    }
    return { output: `不支持的操作: ${op}（list/read/write/delete）` }
  },
}

const notify: Tool = {
  name: "notify",
  description:
    "主动推送一条通知（无需审批）：把自撰的 markdown 正文投递到任务配置的通知通道（任务未配则用全局默认通道），让用户知晓执行结果或异常。" +
    "id 缺省时按当前会话反查正在运行的任务（任务执行中调用无需传 id）；投递目标限定为用户已配置的通道（不接受任意 URL），无可用通道时返回配置指引。仅当用户需要知晓时才推送，例行正常保持静默。",
  safeMode: false,
  requiresApproval: false,
  parameters: schema(
    {
      text: { type: "string", description: "通知正文（markdown，必填）——结论先行、简明；单条上限 2000 字符" },
      title: { type: "string", description: "标题（可选，缺省用任务名）" },
      id: { type: "string", description: "任务 ID（可选，缺省=当前会话正在运行的任务）" },
      at: { type: "array", description: "@ 人名单（可选，覆盖任务通道自带的 @ 配置；open_id 或 \"all\"=@所有人）", items: { type: "string" } },
    },
    ["text"],
  ),
  async execute(args, ctx) {
    if (!ctx.tasks) return { output: "任务能力未启用（服务端配置 GEBAI_TASKS_ENABLED=false 关闭）。" }
    const text = String(args.text ?? "").trim()
    if (!text) return { output: "缺少 text（通知正文）" }
    const id = args.id != null && String(args.id).trim() ? String(args.id).trim() : undefined
    try {
      const res = await ctx.tasks.notify(
        {
          text,
          title: args.title != null ? String(args.title) : undefined,
          at: Array.isArray(args.at) ? (args.at as string[]) : undefined,
        },
        id,
      )
      const head = res.delivered ? `通知已推送（任务 ${res.taskId}）：${res.delivered} 个通道` : `通知未推送（任务 ${res.taskId}）`
      return { output: res.errors.length ? `${head}；问题：${res.errors.join("；")}` : head }
    } catch (err) {
      return { output: `通知失败：${String((err as Error).message || err)}` }
    }
  },
}

export const tools: Record<string, Tool> = { add, list, update, run, cancel, remove, files, notify }
export const requiresApproval = { add: true, update: true, remove: true, run: true, cancel: true, files: true, notify: false }
export const preload = false
export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
