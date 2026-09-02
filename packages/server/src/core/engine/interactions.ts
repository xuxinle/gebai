/** 交互等待中枢（自 engine.ts 拆分）：审批/选择/填值/画图/捕获五种阻塞交互的注册-等待-决策机制。
 *  等待状态存于每任务 TaskState（本文件定义，engine 构建与清理），函数为无状态自由函数——
 *  engine 的 decide 系列与 waitFor 系列方法一行委托至此。含「先到决策/回传排队」竞态处理（决策先于
 *  注册到达时入队，注册时立即消费）与条数上限防堆积。 */
import type { ChoiceOption, ChoicePlan, ChoiceResult, InteractionMode, OutputMode } from "../base/types"

/** 审批等待超时：超时按「拒绝」语义返回（timeout 与显式 rejected 区分，模型可见）。 */
export const APPROVAL_TIMEOUT = 5 * 60 * 1000
/** show 图表分支：等待前端渲染结果的最长时间（超时返回「画图能力受限」）。 */
export const DRAW_TIMEOUT = 5000
/** page_capture 工具：等待前端页面捕获结果的最长时间（超时返回「页面捕获失败」）。 */
export const CAPTURE_TIMEOUT = 30_000
/** 提前到达回传（pendingCaptures）的条数上限：超限丢最旧，防恶意高频回传堆积。 */
export const CAPTURE_PENDING_LIMIT = 64
/** 先到决策/选择/环境值排队 Map 的条数上限：随机 id 可无界堆积（任务期内存放大防护，同捕获队列）。 */
export const PENDING_QUEUE_LIMIT = 64

interface Approval {
  sessionId: string
  toolCallId: string
  /** 工具名（attach 快照重渲染审批卡用）。 */
  tool: string
  resolve: (verdict: ApprovalVerdict) => void
  timer: ReturnType<typeof setTimeout>
}

/** 审批等待结果：approved=通过；rejected=用户显式拒绝；timeout=等待超时（5 分钟未响应）。 */
export type ApprovalVerdict = "approved" | "rejected" | "timeout"

interface Choice {
  /** 展示载荷（attach 快照重渲染选择卡用）。 */
  prompt: string
  options: ChoiceOption[]
  multi: boolean
  /** 计划审批载荷（ask 计划分支）：选择卡内嵌计划全文，刷新/切回后凭 attach 快照恢复。 */
  plan?: ChoicePlan
  resolve: (result: ChoiceResult) => void
  timer: ReturnType<typeof setTimeout>
}

/** 前端渲染结果（show 图表分支回传）。 */
export interface DrawResult {
  ok: boolean
  error?: string
}

interface DrawWait {
  /** 展示载荷（attach 快照重渲染图表渲染请求用）。 */
  render: { code: string; name?: string; format?: import("@gebai/sdk").DiagramFormat }
  resolve: (result: DrawResult | null) => void
  timer: ReturnType<typeof setTimeout>
}

/** 前端页面捕获结果（page_capture 工具回传）。 */
export interface CaptureResult {
  html: string
  /** 截图 base64（data URL 或裸 base64，png）；前端未截图时缺省。 */
  imageBase64?: string
  error?: string
}

interface CaptureWait {
  /** 展示载荷（attach 快照重渲染捕获请求用）。 */
  opts: { fullPage: boolean; delayMs: number }
  resolve: (result: CaptureResult | null) => void
  timer: ReturnType<typeof setTimeout>
}

/** 前端捕获回传先于注册到达时排队（带时间戳，超时后迟到的回传惰性清理防堆积）。 */
interface PendingCapture {
  result: CaptureResult
  ts: number
}

/** ask 填值分支：等待中的环境变量请求（envId → 变量名 + 回调）。 */
interface EnvWait {
  name: string
  /** 展示载荷（attach 快照重渲染填值卡用）。 */
  description: string
  secret: boolean
  resolve: (ok: boolean) => void
  timer: ReturnType<typeof setTimeout>
}

/** 运行中任务的流式快照（attach 用）：当前在途 assistant 回合的累积文本/推理（尚未持久化，
 *  页面刷新后从存储恢复不了的部分）。 */
export interface StreamSnapshot {
  messageId: string
  text: string
  reasoning: string
  session?: boolean
  sessionRunId?: string
}

/** 运行中任务状态（交互等待的载体；engine 的 tasks Map 值类型）。 */
export interface TaskState {
  controller: AbortController
  /** 任务开始时刻（Date.now()；attach 快照恢复前端单轮计时器起点用）。 */
  startedAt: number
  /** 用户显式停止标记：取消 vs 显式拒绝审批的区分依据（拒绝需落盘，取消短路不落盘）。 */
  cancelled?: boolean
  approvals: Map<string, Approval>
  pendingDecisions: Map<string, boolean>
  /** 每个工具调用的审批拒绝重试计数（approval.request 的 retries 字段）。 */
  retries: Map<string, number>
  /** 待用户选择（choose 工具）：choiceId → 等待中的选择回调。 */
  choices: Map<string, Choice>
  /** 选择决策先于注册到达时排队。 */
  pendingChoices: Map<string, ChoiceResult>
  /** show 图表分支：renderId → 等待中的渲染回调。 */
  draws: Map<string, DrawWait>
  /** 渲染结果先于注册到达时排队。 */
  pendingDraws: Map<string, DrawResult>
  /** page_capture 工具：captureId → 等待中的捕获回调。 */
  captures: Map<string, CaptureWait>
  /** 捕获结果先于注册到达时排队。 */
  pendingCaptures: Map<string, PendingCapture>
  /** 通道级禁用工具（如飞书桥接禁用依赖前端页面的工具）：精确名或 {agent}_ 前缀名匹配。 */
  disabledTools: string[]
  /** 极简模式工具白名单（DESIGN「极简模式」）：设置后仅名单内工具可用（schema 过滤 + 执行阻止）。 */
  enabledTools?: string[]
  /** 交互模式（none/multi_turn/realtime，DESIGN「交互模式」）：工具声明的最低可用模式高于此值时被禁用。 */
  interactionMode: InteractionMode
  /** 发起任务用户的角色（admin/user；公共资源权限判定用）。 */
  role?: string
  /** 输出方式（final_only/streaming）：final_only 不推送文本增量与推理流（仅最终响应）。 */
  outputMode: OutputMode
  /** 通道环境注记（引擎通道无关，由桥接层注入——如飞书：告知模型当前对话的宿主/渲染/能力边界）。 */
  channelNote?: string
  /** 任务级环境变量（run 时组装快照的同一引用）：ask 填值后原地更新，工具后续读取立即生效。 */
  env: Record<string, string>
  /** ask 填值分支：envId → 等待中的请求回调。 */
  envRequests: Map<string, EnvWait>
  /** 环境变量值先于注册到达时排队。 */
  pendingEnvRequests: Map<string, string>
  /** 在途 assistant 回合流式累积（delta/reasoning 发布点更新，message.done 清空——已持久化部分
   *  由存储恢复；页面刷新 attach 时据此重建未持久化的部分文本）。 */
  stream?: StreamSnapshot
}

/** 事件发布（engine.publish 同签名）。 */
type Publish = (sessionId: string, type: string, payload: Record<string, unknown>) => void

export function decideApproval(task: TaskState | undefined, toolCallId: string, approve: boolean): void {
  if (!task) return
  const approval = task.approvals.get(toolCallId)
  if (approval) {
    clearTimeout(approval.timer)
    task.approvals.delete(toolCallId)
    approval.resolve(approve ? "approved" : "rejected")
  } else {
    // decision arrived before the approval was registered; queue it
    if (task.pendingDecisions.size >= PENDING_QUEUE_LIMIT) task.pendingDecisions.delete(task.pendingDecisions.keys().next().value!)
    task.pendingDecisions.set(toolCallId, approve)
  }
  // 拒绝审批 = 停止当前会话生成：不再让模型调整方案继续执行（超时自动拒绝不停止，仅显式拒绝触发）。
  // 同步 abort 但不设 cancelled 标记：审批消费处区分「用户停止（短路不落盘）」与
  // 「显式拒绝（落盘拒绝消息后由下一轮 abort 检查结束）」，无延迟窗口竞态
  if (!approve) {
    task.controller.abort()
  }
}

/** 提交用户选择（ask 选项询问分支等待的选择）；null 表示拒绝，string 为单选（选项/自定义文本），string[] 为多选。 */
export function decideChoice(task: TaskState | undefined, choiceId: string, selection: string | string[] | null): void {
  if (!task) return
  const result: ChoiceResult = Array.isArray(selection)
    ? { kind: "multi", values: selection }
    : selection == null
      ? { kind: "refuse" }
      : { kind: "option", value: selection }
  const choice = task.choices.get(choiceId)
  if (choice) {
    clearTimeout(choice.timer)
    task.choices.delete(choiceId)
    choice.resolve(result)
  } else {
    // 决策先于注册到达（并发竞态）：排队，waitForChoice 注册时立即消费
    if (task.pendingChoices.size >= PENDING_QUEUE_LIMIT) task.pendingChoices.delete(task.pendingChoices.keys().next().value!)
    task.pendingChoices.set(choiceId, result)
  }
}

/** 提交前端渲染结果（show 图表分支等待的渲染回传）。 */
export function decideDrawResult(task: TaskState | undefined, renderId: string, result: DrawResult): void {
  if (!task) return
  const draw = task.draws.get(renderId)
  if (draw) {
    clearTimeout(draw.timer)
    task.draws.delete(renderId)
    draw.resolve(result)
  } else {
    // 回传先于注册到达（并发竞态）：排队，waitForDraw 注册时立即消费
    task.pendingDraws.set(renderId, result)
  }
}

/**
 * 前端渲染图表并阻塞等待结果（show 图表分支）。
 * 发布 event.draw.render（含 renderId/源码/图表语言供前端渲染），成功/失败经 decideDrawResult 回传；
 * 5 秒超时返回 null（画图能力受限）。
 */
export async function waitForDraw(
  sessionId: string,
  task: TaskState,
  publish: Publish,
  render: { code: string; name?: string; format?: import("@gebai/sdk").DiagramFormat },
  signal?: AbortSignal,
): Promise<DrawResult | null> {
  const renderId = crypto.randomUUID().replace(/-/g, "")
  publish(sessionId, "event.draw.render", { renderId, code: render.code, name: render.name ?? "diagram", format: render.format ?? "plantuml", sessionId })
  // 先消费提前到达的结果
  const pre = task.pendingDraws.get(renderId)
  if (pre !== undefined) {
    task.pendingDraws.delete(renderId)
    return pre
  }
  if (signal?.aborted) return null
  return new Promise<DrawResult | null>((resolve) => {
    let timer: ReturnType<typeof setTimeout>
    let onAbort: () => void
    const done = (result: DrawResult | null) => {
      clearTimeout(timer)
      signal?.removeEventListener("abort", onAbort)
      task.draws.delete(renderId)
      resolve(result)
    }
    onAbort = () => done(null)
    timer = setTimeout(() => done(null), DRAW_TIMEOUT)
    signal?.addEventListener("abort", onAbort, { once: true })
    task.draws.set(renderId, { render, resolve: done, timer })
  })
}

/** 提交前端页面捕获结果（page_capture 工具等待的捕获回传）。 */
export function decideCaptureResult(
  task: TaskState | undefined,
  captureId: string,
  result: CaptureResult,
  htmlLimit: number,
  imageMaxBytes: number,
): void {
  if (!task) return
  // 输入防线（任意 WS 客户端可发 capture.result，前端截断不可信）：
  // html 截断到落盘上限；截图 base64 超限（约 8MB 解码体积）丢弃；error 截断防输出注入
  const html = result.html.slice(0, htmlLimit)
  const imageBase64 = result.imageBase64 && result.imageBase64.length <= imageMaxBytes * 1.4 ? result.imageBase64 : undefined
  const error = result.error ? result.error.slice(0, 500) : undefined
  const safe: CaptureResult = { html, imageBase64, error }
  const cap = task.captures.get(captureId)
  if (cap) {
    clearTimeout(cap.timer)
    task.captures.delete(captureId)
    cap.resolve(safe)
  } else {
    // 回传先于注册到达（并发竞态）：排队，waitForCapture 注册时立即消费；
    // 条数上限防恶意高频回传堆积（超限丢最旧）
    if (task.pendingCaptures.size >= CAPTURE_PENDING_LIMIT) {
      let oldest: string | null = null
      let oldestTs = Infinity
      for (const [id, p] of task.pendingCaptures) {
        if (p.ts < oldestTs) {
          oldestTs = p.ts
          oldest = id
        }
      }
      if (oldest) task.pendingCaptures.delete(oldest)
    }
    task.pendingCaptures.set(captureId, { result: safe, ts: Date.now() })
  }
}

/**
 * 请求前端捕获当前页面并阻塞等待结果（page_capture 工具）。
 * 发布 event.capture.request（含 captureId），前端捕获渲染后 DOM html 与截图后经
 * decideCaptureResult 回传；超时返回 null（前端离线或捕获超时）。
 */
export async function waitForCapture(
  sessionId: string,
  task: TaskState,
  publish: Publish,
  timeoutMs: number,
  opts: { fullPage?: boolean; delayMs?: number } = {},
  signal?: AbortSignal,
): Promise<CaptureResult | null> {
  const captureId = crypto.randomUUID().replace(/-/g, "")
  publish(sessionId, "event.capture.request", { captureId, sessionId, fullPage: opts.fullPage === true, delay: opts.delayMs ?? 0 })
  // 先消费提前到达的结果；顺带惰性清理超时后迟到的回传（防堆积）
  for (const [id, p] of task.pendingCaptures) {
    if (Date.now() - p.ts > timeoutMs) task.pendingCaptures.delete(id)
  }
  const pre = task.pendingCaptures.get(captureId)
  if (pre !== undefined) {
    task.pendingCaptures.delete(captureId)
    return pre.result
  }
  if (signal?.aborted) return null
  return new Promise<CaptureResult | null>((resolve) => {
    let timer: ReturnType<typeof setTimeout>
    let onAbort: () => void
    const done = (result: CaptureResult | null) => {
      clearTimeout(timer)
      signal?.removeEventListener("abort", onAbort)
      task.captures.delete(captureId)
      resolve(result)
    }
    onAbort = () => done(null)
    timer = setTimeout(() => done(null), timeoutMs)
    signal?.addEventListener("abort", onAbort, { once: true })
    task.captures.set(captureId, { opts: { fullPage: opts.fullPage === true, delayMs: opts.delayMs ?? 0 }, resolve: done, timer })
  })
}

/**
 * 向用户发起选项询问并阻塞等待（ask 选项询问分支）。
 * 发布 event.choice.request（含 choiceId/multi 供 UI 提交）；返回 ChoiceResult：
 * 单选/自定义文本为 { kind: "option" }，多选为 { kind: "multi" }，用户拒绝为 { kind: "refuse" }，超时（审批超时同值）为 null。
 */
export async function waitForChoice(
  sessionId: string,
  task: TaskState,
  publish: Publish,
  prompt: string,
  options: ChoiceOption[],
  multi?: boolean,
  signal?: AbortSignal,
  plan?: ChoicePlan,
): Promise<ChoiceResult> {
  const choiceId = crypto.randomUUID().replace(/-/g, "")
  publish(sessionId, "event.choice.request", { choiceId, prompt, options, multi: !!multi, ...(plan ? { plan } : {}), sessionId })
  // 先消费提前到达的决策
  const pre = task.pendingChoices.get(choiceId)
  if (pre !== undefined) {
    task.pendingChoices.delete(choiceId)
    return pre
  }
  // 取消/超时信号：abort 立即以 null（等同超时）解开等待
  if (signal?.aborted) return null
  return new Promise<ChoiceResult>((resolve) => {
    let timer: ReturnType<typeof setTimeout>
    let onAbort: () => void
    const done = (result: ChoiceResult) => {
      clearTimeout(timer)
      signal?.removeEventListener("abort", onAbort)
      task.choices.delete(choiceId)
      resolve(result)
    }
    onAbort = () => done(null)
    timer = setTimeout(() => done(null), APPROVAL_TIMEOUT)
    signal?.addEventListener("abort", onAbort, { once: true })
    task.choices.set(choiceId, { prompt, options, multi: multi === true, ...(plan ? { plan } : {}), resolve: done, timer })
  })
}

/** 提交用户填写的环境变量值（ask 填值分支等待的请求回传）：非空值写入任务 env（ctx 同引用，工具后续读取立即生效）。 */
export function decideEnvResult(
  task: TaskState | undefined,
  envId: string,
  value: string | null,
  isNameAllowed: (name: string) => boolean,
): void {
  if (!task) return
  const req = task.envRequests.get(envId)
  if (req) {
    clearTimeout(req.timer)
    task.envRequests.delete(envId)
    if (value != null && value !== "" && isNameAllowed(req.name)) {
      task.env[req.name] = value
      req.resolve(true)
    } else {
      req.resolve(false)
    }
  } else {
    // 值先于注册到达（并发竞态）：排队，waitForEnv 注册时立即消费
    if (task.pendingEnvRequests.size >= PENDING_QUEUE_LIMIT) task.pendingEnvRequests.delete(task.pendingEnvRequests.keys().next().value!)
    task.pendingEnvRequests.set(envId, value ?? "")
  }
}

/**
 * 向用户请求设置环境变量并阻塞等待（ask 填值分支）。
 * 发布 event.env.request（含 envId/name/description/secret 供前端弹窗填值）；
 * 用户提交后值写入任务 env（本次任务后续工具立即生效）并返回 true；拒绝/超时（审批超时同值）返回 false。
 */
export async function waitForEnv(
  sessionId: string,
  task: TaskState,
  publish: Publish,
  isNameAllowed: (name: string) => boolean,
  name: string,
  description: string,
  secret: boolean,
  signal?: AbortSignal,
): Promise<boolean> {
  // 变量名合法性 + 敏感键限制：非法名/多用户下审批跳过键直接拒绝（不发布卡片、不阻塞）
  if (!isNameAllowed(name)) return false
  const envId = crypto.randomUUID().replace(/-/g, "")
  publish(sessionId, "event.env.request", { envId, name, description, secret, sessionId })
  // 先消费提前到达的值
  const pre = task.pendingEnvRequests.get(envId)
  if (pre !== undefined) {
    task.pendingEnvRequests.delete(envId)
    if (pre !== "") {
      task.env[name] = pre
      return true
    }
    return false
  }
  // 取消/超时信号：abort 立即以 false（等同拒绝）解开等待
  if (signal?.aborted) return false
  return new Promise<boolean>((resolve) => {
    let timer: ReturnType<typeof setTimeout>
    let onAbort: () => void
    const done = (ok: boolean) => {
      clearTimeout(timer)
      signal?.removeEventListener("abort", onAbort)
      task.envRequests.delete(envId)
      resolve(ok)
    }
    onAbort = () => done(false)
    timer = setTimeout(() => done(false), APPROVAL_TIMEOUT)
    signal?.addEventListener("abort", onAbort, { once: true })
    task.envRequests.set(envId, { name, description, secret, resolve: done, timer })
  })
}
