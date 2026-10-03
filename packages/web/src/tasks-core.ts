/** 任务管理视图的纯逻辑（零 DOM 依赖，独立单测）：类别/状态/来源文案、筛选、队列态判定、
 *  表单值与请求体的双向映射与校验。与渲染/DOM 解耦的理由同 todo-core.ts：可在无真实 DOM 的
 *  环境下直接断言，且渲染层只负责把这里的结果贴到元素上。 */
import type { Task, TaskCreateInput, TaskFileEntry, TaskKind, TaskMisfire, TaskNotifyInput, TaskNotifyWhen, TaskQueueSource, TaskQueueState,
  TaskQueueView, TaskRunRecord, TaskRunner, TaskRunStatus, TaskTarget, TaskUpdateInput } from "@gebai/sdk"

/** 列表筛选值（all=不筛选类别）。 */
export type KindFilter = TaskKind | "all"

export const KIND_LABELS: Record<TaskKind, string> = { scheduled: "定时", manual: "普通", idle: "闲时" }
export const RUNNER_LABELS: Record<TaskRunner, string> = { script: "脚本", prompt: "提示词" }
export const STATE_LABELS: Record<TaskQueueState, string> = { idle: "空闲", queued: "排队中", running: "运行中" }
export const STATUS_LABELS: Record<TaskRunStatus, string> = { success: "成功", error: "失败", skipped: "跳过", timeout: "超时" }
export const SOURCE_LABELS: Record<TaskQueueSource, string> = { schedule: "定时到期", manual: "手动执行", idle: "闲时调度", todo: "待办执行" }
export const TARGET_LABELS: Record<TaskTarget, string> = { ephemeral: "每次新建会话", sticky: "专用会话复用", session: "绑定既有会话" }
export const MISFIRE_LABELS: Record<TaskMisfire, string> = { skip: "错过即跳过", run: "立即补跑一次" }

/** 任务 id 短码（列表展示，避免 32 位全串挤爆一行）。 */
export function shortId(id: string): string {
  return id.slice(0, 8)
}

/** 任务标题：名称优先，缺省用短 id。 */
export function taskTitle(t: Pick<Task, "id" | "name">): string {
  return t.name?.trim() ? t.name.trim() : `任务 ${shortId(t.id)}`
}

/** 按类别筛选（all 原样返回）。 */
export function filterTasks(list: Task[], kind: KindFilter): Task[] {
  return kind === "all" ? list : list.filter((t) => t.kind === kind)
}

/** 运行态：运行中 / 排队中 / 空闲。 */
export function stateOf(t: Task): TaskQueueState {
  return t.state === "running" || t.state === "queued" ? t.state : "idle"
}

/** 状态文案（排队中附队列位置；位置未知时不带括号）。 */
export function stateLabel(t: Task, queue?: TaskQueueView | null): string {
  const state = stateOf(t)
  if (state !== "queued") return STATE_LABELS[state]
  const pos = queue?.entries.find((e) => e.taskId === t.id)?.position
  return pos ? `排队中（第 ${pos} 位）` : STATE_LABELS.queued
}

/** 是否可手动执行（运行中的任务再次入队会被后端拒，按钮直接置灰）。 */
export function canRun(t: Task): boolean {
  return t.enabled && stateOf(t) !== "running"
}

/** 是否可置顶（仅排队中的普通/闲时任务有意义；定时任务恒在队首）。 */
export function canFront(t: Task): boolean {
  return stateOf(t) === "queued" && t.kind !== "scheduled"
}

/** 是否可出队（排队中）。 */
export function canDequeue(t: Task): boolean {
  return stateOf(t) === "queued"
}

/** 是否可终止（运行中）。 */
export function canStop(t: Task): boolean {
  return stateOf(t) === "running"
}

/** 时间展示（缺省 "-"）。 */
export function formatTime(ms?: number): string {
  if (typeof ms !== "number" || ms <= 0) return "-"
  return new Date(ms).toLocaleString("zh-CN")
}

/** 耗时展示（<1s 显示毫秒）。 */
export function formatDuration(ms?: number): string {
  if (typeof ms !== "number" || ms < 0) return "-"
  if (ms < 1000) return `${Math.round(ms)}ms`
  if (ms < 60_000) return `${Math.round(ms / 100) / 10}s`
  const m = Math.floor(ms / 60_000)
  return `${m}m${Math.round((ms - m * 60_000) / 1000)}s`
}

/** 周期展示（表达式 + 时区）。 */
export function scheduleSummary(t: Task): string {
  if (t.kind !== "scheduled" || !t.schedule) return ""
  return t.timezone ? `${t.schedule}（${t.timezone}）` : t.schedule
}

/** 通知摘要（通道类型与条数；含 @ 人时标注）。 */
export function notifySummary(t: Task): string {
  if (!t.notify?.length) return ""
  const parts = t.notify.map((ch) => (ch.type === "webhook" ? "Webhook" : ch.type === "feishu_chat" ? "飞书群" : "飞书"))
  const at = t.notify.some((ch) => ch.at?.length) ? " · 含 @ 提醒" : ""
  return `${parts.join(" / ")}${at}`
}

/** 执行体/类别摘要行（列表行副标题用）。 */
export function metaLine(t: Task): string {
  const parts = [`${KIND_LABELS[t.kind]} · ${RUNNER_LABELS[t.runner]}`]
  const schedule = scheduleSummary(t)
  if (schedule) parts.push(`周期 ${schedule}`)
  if (t.kind === "scheduled" && t.enabled && t.nextRunAt) parts.push(`下次 ${formatTime(t.nextRunAt)}`)
  const envCount = t.env ? Object.keys(t.env).length : 0
  if (envCount) parts.push(`环境变量 ${envCount} 项`)
  parts.push(`已运行 ${t.runCount} 次`)
  if (t.lastRunAt) parts.push(`上次 ${formatTime(t.lastRunAt)}`)
  return parts.filter(Boolean).join(" · ")
}

/** 最近一次执行的结果摘要（状态 + 输出/错误截断）。 */
export function lastResultLine(t: Task): string {
  if (!t.lastStatus) return "尚未执行"
  const status = STATUS_LABELS[t.lastStatus]
  const detail = t.lastError ? `：${t.lastError}` : t.lastOutput ? `：${t.lastOutput}` : ""
  return `${status}${detail}`
}

/** 运行历史一行文案（时间/状态/耗时/说明）。 */
export function runLine(r: TaskRunRecord): string {
  const parts = [formatTime(r.at), STATUS_LABELS[r.status], formatDuration(r.durationMs)]
  if (r.manual) parts.push("手动")
  const detail = r.error ?? r.reason ?? r.output
  return `${parts.join(" · ")}${detail ? ` · ${detail}` : ""}`
}

/** 资源文件一行文案（目录/大小）。 */
export function fileLine(f: TaskFileEntry): string {
  return f.dir ? `${f.path}/` : `${f.path} · ${f.size} 字节`
}

/* ---------- 表单 ←→ 请求体 ---------- */

/** 表单值（全部为字符串/布尔，渲染层直接绑定原生控件；空串表示未填）。 */
export interface TaskFormValues {
  kind: TaskKind
  runner: TaskRunner
  name: string
  schedule: string
  timezone: string
  misfire: "" | TaskMisfire
  script: string
  prompt: string
  target: TaskTarget
  sessionId: string
  agents: string
  timeoutMs: string
  maxConsecutiveErrors: string
  notifyOn: TaskNotifyWhen
  /** 通知通道（每行一条：`type target [secret]`；webhook 通道第二段为 32 位 hex 时视为已注册 Webhook 引用）。 */
  notifyText: string
  /** 任务级环境变量（每行一条：`NAME=值`）。 */
  envText: string
  enabled: boolean
  runNow: boolean
  front: boolean
}

export function emptyForm(): TaskFormValues {
  return {
    kind: "scheduled",
    runner: "prompt",
    name: "",
    schedule: "",
    timezone: "",
    misfire: "",
    script: "",
    prompt: "",
    target: "ephemeral",
    sessionId: "",
    agents: "",
    timeoutMs: "",
    maxConsecutiveErrors: "",
    notifyOn: "auto",
    notifyText: "",
    envText: "",
    enabled: true,
    runNow: true,
    front: false,
  }
}

/** 由任务（编辑态）反填表单。 */
export function formFromTask(t: Task): TaskFormValues {
  return {
    kind: t.kind,
    runner: t.runner,
    name: t.name ?? "",
    schedule: t.schedule ?? "",
    timezone: t.timezone ?? "",
    misfire: t.misfire ?? "",
    script: t.script ?? "",
    prompt: t.prompt ?? "",
    target: t.target ?? "ephemeral",
    sessionId: t.sessionId ?? "",
    agents: (t.agents ?? []).join(", "),
    timeoutMs: t.timeoutMs !== undefined ? String(t.timeoutMs) : "",
    maxConsecutiveErrors: t.maxConsecutiveErrors !== undefined ? String(t.maxConsecutiveErrors) : "",
    notifyOn: t.notifyOn ?? "auto",
    notifyText: notifyLines(t),
    envText: envLines(t),
    enabled: t.enabled,
    runNow: false,
    front: false,
  }
}

/** 任务环境变量 → 行文本（每行 `NAME=值`；空值为 `NAME=`）。 */
export function envLines(t: Pick<Task, "env">): string {
  if (!t.env) return ""
  return Object.entries(t.env)
    .map(([k, v]) => `${k}=${v}`)
    .join("\n")
}

/** 行文本 → 任务环境变量（空文本返回 undefined；非法行拒绝并给出定位）。 */
export function parseEnvLines(raw: string): Record<string, string> | undefined {
  const lines = raw.split("\n").map((s) => s.trim()).filter(Boolean)
  if (!lines.length) return undefined
  const out: Record<string, string> = {}
  lines.forEach((line, i) => {
    const eq = line.indexOf("=")
    const name = (eq < 0 ? line : line.slice(0, eq)).trim()
    if (eq < 0) throw new Error(`环境变量第 ${i + 1} 行缺少 \"=\"（格式 NAME=值）`)
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) throw new Error(`环境变量第 ${i + 1} 行变量名非法: ${name || "（空）"}`)
    out[name] = line.slice(eq + 1).trim()
  })
  return Object.keys(out).length ? out : undefined
}

function numOrUndefined(raw: string, label: string): number | undefined {
  const s = raw.trim()
  if (!s) return undefined
  const n = Number(s)
  if (!Number.isFinite(n)) throw new Error(`${label}须为数字`)
  return n
}

/** 通知通道 → 行文本（每行 `type target [secret]`；webhookId 引用形态写作 `webhook <webhookId>`）。 */
export function notifyLines(t: Pick<Task, "notify">): string {
  if (!t.notify?.length) return ""
  return t.notify
    .map((ch) => {
      const head = ch.webhookId ? `webhook ${ch.webhookId}` : `${ch.type} ${ch.target ?? ""}`
      return ch.secret ? `${head} ${ch.secret}` : head
    })
    .join("\n")
}

/** 行文本 → 通知通道（空文本返回 undefined=不配置/不改动）；非法行拒绝并给出定位。 */
export function parseNotifyLines(raw: string): TaskNotifyInput[] | undefined {
  const lines = raw.split("\n").map((s) => s.trim()).filter(Boolean)
  if (!lines.length) return undefined
  const out: TaskNotifyInput[] = []
  lines.forEach((line, i) => {
    const [type, target, secret] = line.split(/\s+/)
    if (type !== "webhook" && type !== "feishu" && type !== "feishu_chat") {
      throw new Error(`通知通道第 ${i + 1} 行类型无效: ${type ?? ""}（webhook / feishu / feishu_chat）`)
    }
    if (!target) throw new Error(`通知通道第 ${i + 1} 行缺少目标（webhook URL / 飞书 webhook 或群 chat_id）`)
    if (type === "webhook" && /^[a-f0-9]{32}$/.test(target)) out.push({ type, webhookId: target, ...(secret ? { secret } : {}) })
    else out.push({ type, target, ...(secret ? { secret } : {}) })
  })
  return out
}

function parseAgents(raw: string): string[] | undefined {
  const list = raw.split(/[,，\s]+/).map((s) => s.trim()).filter(Boolean)
  return list.length ? list : undefined
}

/** 表单校验（返回错误文案，null=通过）；与后端语义一致：脚本型需 script、提示词型需 prompt、定时需表达式。 */
export function validateForm(v: TaskFormValues): string | null {
  if (v.runner === "script" && !v.script.trim()) return "脚本型任务需要填写脚本命令"
  if (v.runner === "prompt" && !v.prompt.trim()) return "提示词型任务需要填写提示词"
  if (v.kind === "scheduled" && !v.schedule.trim()) return "定时任务需要填写执行表达式（如 0 9 * * * 或 @every 30m）"
  try {
    numOrUndefined(v.timeoutMs, "执行超时")
    numOrUndefined(v.maxConsecutiveErrors, "连续失败阈值")
    parseNotifyLines(v.notifyText)
    parseEnvLines(v.envText)
  } catch (err) {
    return (err as Error).message
  }
  return null
}

/** 表单 → 创建请求体。 */
export function formToCreateInput(v: TaskFormValues): TaskCreateInput {
  const input: TaskCreateInput = {
    kind: v.kind,
    runner: v.runner,
    enabled: v.enabled,
  }
  const name = v.name.trim()
  if (name) input.name = name
  if (v.kind === "scheduled") {
    input.schedule = v.schedule.trim()
    const tz = v.timezone.trim()
    if (tz) input.timezone = tz
    if (v.misfire) input.misfire = v.misfire
  }
  if (v.runner === "script") input.script = v.script.trim()
  else {
    input.prompt = v.prompt.trim()
    input.target = v.target
    const sid = v.sessionId.trim()
    if (v.target === "session" && sid) input.sessionId = sid
    const agents = parseAgents(v.agents)
    if (agents) input.agents = agents
  }
  const timeout = numOrUndefined(v.timeoutMs, "执行超时")
  if (timeout !== undefined) input.timeoutMs = timeout
  const maxErr = numOrUndefined(v.maxConsecutiveErrors, "连续失败阈值")
  if (maxErr !== undefined) input.maxConsecutiveErrors = maxErr
  input.notifyOn = v.notifyOn
  const notify = parseNotifyLines(v.notifyText)
  if (notify) input.notify = notify
  const env = parseEnvLines(v.envText)
  if (env) input.env = env
  if (v.kind === "manual") {
    input.runNow = v.runNow
    if (v.front) input.front = true
  }
  return input
}

/** 表单 → 更新补丁（改名/启停/执行体与内容/表达式/目标/超时阈值；通知配置留给通知专用入口不改动）。 */
export function formToUpdateInput(v: TaskFormValues): TaskUpdateInput {
  const patch: TaskUpdateInput = {
    name: v.name.trim(),
    enabled: v.enabled,
    runner: v.runner,
  }
  if (v.kind === "scheduled") {
    patch.schedule = v.schedule.trim()
    patch.timezone = v.timezone.trim()
    if (v.misfire) patch.misfire = v.misfire
  }
  if (v.runner === "script") {
    patch.script = v.script.trim()
  } else {
    patch.prompt = v.prompt.trim()
    patch.target = v.target
    patch.sessionId = v.sessionId.trim()
    patch.agents = parseAgents(v.agents) ?? []
  }
  const timeout = numOrUndefined(v.timeoutMs, "执行超时")
  patch.timeoutMs = timeout
  const maxErr = numOrUndefined(v.maxConsecutiveErrors, "连续失败阈值")
  if (maxErr !== undefined) patch.maxConsecutiveErrors = maxErr
  patch.notifyOn = v.notifyOn
  patch.notify = parseNotifyLines(v.notifyText)
  // 编辑态表单是环境变量的全量视图：解析为空即清除（与后端「未提供不改动」区分）
  patch.env = parseEnvLines(v.envText) ?? {}
  return patch
}

/** 额度占用文案。 */
export function limitText(q: TaskQueueView): string {
  return `${q.running.length}/${q.limit}`
}

/** 闲时任务是否正在让路（队列里有定时/普通任务排队或运行）。 */
export function idleBlockedText(q: TaskQueueView): string {
  return q.busy ? "闲时任务让路中（有定时/普通任务待执行）" : "闲时任务可执行（队列空闲）"
}
