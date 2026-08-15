import type { AttachmentInput, AttachmentRef, DiagramFormat, Message, MessageLike, SessionRunArchive, SessionRunEntry } from "@gebai/sdk"
import { LLMConfigError, parseExtraParams, type LLMChunk, type LLMProvider, type LLMUsage } from "./llm"
import { VISION_MAX_IMAGE_BYTES } from "./vision"
import type { ToolRegistry } from "./registry"
import type { SessionStore } from "./store"
import { estimateCtxTokens, estimateCharsTokens, isProtectedMessage } from "./store"
import type { EnvManager } from "./env"
import type { Sandbox } from "./sandbox"
import type { EventBus } from "./event-bus"
import type { ServerConfig } from "./config"
import type { SubAgentManager } from "./subagents"
import type { ToolContext, ToolResult, Tool, PresetProject, ChoiceResult, ChoiceOption, InteractionMode, OutputMode, SessionData } from "./types"
import { ToolRegistry as BaseToolRegistry } from "./registry"
import { agentListTool, agentLoadTool, agentRunTool, makeFlowTool, toolSchemasTool, PAGE_CAPTURE_HTML_LIMIT, truncate, TRUNCATE_THRESHOLD, spillLongUserInput } from "./tools"
import { basenameName, resolveInSandbox, sessionPath } from "./paths"
import { dirname, isAbsolute, join, resolve } from "node:path"
import { isRiskyToolName, safeModeRestrictionMsg } from "./safety"

const MAX_TOOL_ROUNDS = 200
/** 待办续做：主循环完成后仍有未完成待办（pending/in_progress）时，追加提醒消息继续完成的轮次上限。 */
const MAX_TODO_CONTINUE = 3
/** 重复检测滚动窗口：记录最近 N 次工具调用签名（工具名+参数），窗口内相同签名出现 ≥MAX_REPEAT_HITS 次判定为无效重复。 */
const MAX_REPEAT_WINDOW = 8
/** 重复检测命中阈值：相同签名（工具+参数）在窗口内出现第 MAX_REPEAT_HITS 次时中断该次执行并注入引导提示。 */
const MAX_REPEAT_HITS = 3
/** 重复中断上限：中断次数超过该值即终止工具循环（模型持续重复时防止无效空转）。 */
const MAX_REPEAT_STALLS = 2
const APPROVAL_TIMEOUT = 5 * 60 * 1000
/** draw 工具：等待前端渲染结果的最长时间（超时返回「画图能力受限」）。 */
const DRAW_TIMEOUT = 5000
/** page_capture 工具：等待前端页面捕获结果的最长时间（超时返回「页面捕获失败」）。 */
const CAPTURE_TIMEOUT = 30_000
/** 提前到达回传（pendingCaptures）的条数上限：超限丢最旧，防恶意高频回传堆积。 */
const CAPTURE_PENDING_LIMIT = 64
/** 工具执行超时兜底（毫秒）：脚本类工具由 sandbox 自身 timeoutMs（默认 5 分钟）先杀进程并返回超时结果；
 * 此兜底覆盖不响应超时的工具（如网络请求挂起）。超时不结束任务——结果作为「执行超时」返回给模型继续。 */
const TOOL_TIMEOUT_MS = 9 * 60 * 1000
const SUBAGENT_DEPTH = 3
/** 模型接口异常/空响应的重试次数与退避基数（指数退避，DESIGN「常量参考」）。 */
const LLM_RETRY_COUNT = 2
const LLM_RETRY_BACKOFF_MS = 800
/** 上下文压缩阈值：窗口的 80%（DESIGN「常量参考」）。 */
const COMPACT_RATIO = 0.8
/** 主动/自动压缩默认保留最近一半可压缩消息。 */
const COMPACT_KEEP_RATIO = 0.5
/** 摘要请求的输入裁剪长度与输出上限。 */
const SUMMARY_INPUT_LIMIT = 20000
const SUMMARY_OUTPUT_LIMIT = 2000
/** 附件图片内联上限（与 vision 工具一致）：超出不内联，降级为文本说明。 */
const ATTACHMENT_INLINE_LIMIT = VISION_MAX_IMAGE_BYTES
/** 图片附件 MIME 白名单（OpenAI 系与 Anthropic 均接受）。 */
const VISION_MIME_SET = new Set(["image/png", "image/jpeg", "image/gif", "image/webp"])
/** 历史图片内联窗口：仅最近 N 条含图片的用户消息内联进上下文，更早的降级为文本说明
 *  （图片永久占据上下文且不受压缩保护，长会话会被历史图片占死窗口）。 */
const INLINE_IMAGE_RECENT = 3
/** 单次 agent_run（新会话执行）可预加载的子Agent 数量上限（防异常/恶意调用拼装超大提示词）。 */
const MAX_AGENTS_PER_RUN = 5
/** 任务中途上下文回收阈值：最近一次真实 input tokens 超过窗口该比例时回收最早的旧工具结果
 *  （替换为归档占位——超长结果本就已落盘 tmp/truncated/，原文不丢）。 */
const MID_RUN_RECLAIM_RATIO = 0.9
/** 中途回收保留的最近工具结果条数（模型近期操作上下文不受影响）。 */
const RECLAIM_KEEP_RECENT = 8
/** LLM 流式调用读空闲超时（毫秒）：SSE 建立后超过该时长无任何 chunk 判定接口假死，中止本次调用
 *  （无产出走重试，有产出上抛为任务错误，不再无限挂起）。 */
const LLM_IDLE_TIMEOUT_MS = 120_000

function attachmentSizeText(n: number): string {
  if (n >= 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)}MB`
  if (n >= 1024) return `${Math.ceil(n / 1024)}KB`
  return `${n}B`
}

/** 附件文本说明（模型可见：路径/MIME/大小；图片另附 vision 工具指引）。 */
function attachmentNote(ref: { name: string; path: string; mime: string; size: number }, isImage: boolean): string {
  const vision = isImage ? "。如需查看图片内容，请调用 vision 工具（image 参数传该路径）" : ""
  return `[用户附件${isImage ? "图片" : "文件"}: ${ref.name}（${ref.mime}，${attachmentSizeText(ref.size)}，会话路径 ${ref.path}）${vision}]`
}

/** 粗略估算消息 token 数（CJK 感知，见 store.estimateCharsTokens）。
 *  仅用于估算「真实 usage 基线之外尚未发送的增量」与无 usage 真值时的兜底（全量）。 */
function estimateTokens(msgs: MessageLike[]): number {
  let tokens = 0
  for (const m of msgs) {
    // 多模态内容块（图片 base64）按序列化长度计，避免低估触发压缩不及时
    tokens += estimateCharsTokens(Array.isArray(m.content) ? JSON.stringify(m.content) : String(m.content))
    if (m.toolCalls) {
      for (const tc of m.toolCalls) {
        tokens += estimateCharsTokens(tc.name + JSON.stringify(tc.arguments))
      }
    }
  }
  return tokens
}

/** 任务级 GEBAI_LLM_EXTRA_PARAMS 解析：非法 JSON 静默忽略（不阻塞任务），仅记录控制台提示。 */
function parseExtraParamsSafe(raw: string | undefined): Record<string, unknown> {
  try {
    return parseExtraParams(raw)
  } catch (err) {
    console.warn(`[gebai] 忽略无效的 GEBAI_LLM_EXTRA_PARAMS: ${(err as Error).message}`)
    return {}
  }
}

/** 解析工具审批要求（DESIGN「工具审批」）：布尔静态声明，或函数按调用参数动态判定
 *  （flow 等编排工具据此实现「内部任一工具需审批则整体审批」）；函数异常按需审批处理（fail-safe）。 */
async function toolRequiresApproval(tool: Tool, args: Record<string, unknown>, ctx: ToolContext): Promise<boolean> {
  const ra = tool.requiresApproval
  if (typeof ra !== "function") return !!ra
  try {
    return !!(await ra(args ?? {}, ctx))
  } catch {
    return true
  }
}

/** 递归剥离参数中的 approval 免审标记（仅删键，其余原样深拷贝）：服务模式无交互硬门槛用——
 *  sh/py 的 approval:false 只放宽交互审批（不弹卡），不得绕过「无人值守不执行敏感工具」拒绝；
 *  flow 嵌套步骤的 params 同规则递归剥离，防经编排免审执行内层脚本。 */
function stripApprovalFlags(v: unknown): unknown {
  if (Array.isArray(v)) return v.map(stripApprovalFlags)
  if (v && typeof v === "object") {
    const out: Record<string, unknown> = {}
    for (const [k, val] of Object.entries(v as Record<string, unknown>)) if (k !== "approval") out[k] = stripApprovalFlags(val)
    return out
  }
  return v
}

function sleep(ms: number, signal?: AbortSignal | null): Promise<void> {
  if (signal?.aborted) return Promise.reject(new DOMException("Aborted", "AbortError"))
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      signal?.removeEventListener("abort", onAbort)
      resolve()
    }, ms)
    const onAbort = () => {
      clearTimeout(timer)
      reject(new DOMException("Aborted", "AbortError"))
    }
    signal?.addEventListener("abort", onAbort, { once: true })
  })
}

interface Approval {
  sessionId: string
  toolCallId: string
  resolve: (verdict: ApprovalVerdict) => void
  timer: ReturnType<typeof setTimeout>
}

/** 审批等待结果：approved=通过；rejected=用户显式拒绝；timeout=等待超时（5 分钟未响应）。 */
type ApprovalVerdict = "approved" | "rejected" | "timeout"

interface Choice {
  resolve: (result: ChoiceResult) => void
  timer: ReturnType<typeof setTimeout>
}

/** 前端渲染结果（draw 工具回传）。 */
interface DrawResult {
  ok: boolean
  error?: string
}

interface DrawWait {
  resolve: (result: DrawResult | null) => void
  timer: ReturnType<typeof setTimeout>
}

/** 前端页面捕获结果（page_capture 工具回传）。 */
interface CaptureResult {
  html: string
  /** 截图 base64（data URL 或裸 base64，png）；前端未截图时缺省。 */
  imageBase64?: string
  error?: string
}

interface CaptureWait {
  resolve: (result: CaptureResult | null) => void
  timer: ReturnType<typeof setTimeout>
}

/** 前端捕获回传先于注册到达时排队（带时间戳，超时后迟到的回传惰性清理防堆积）。 */
interface PendingCapture {
  result: CaptureResult
  ts: number
}

/** ask_env 工具：等待中的环境变量请求（envId → 变量名 + 回调）。 */
interface EnvWait {
  name: string
  resolve: (ok: boolean) => void
  timer: ReturnType<typeof setTimeout>
}

interface TaskState {
  controller: AbortController
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
  /** draw 工具：renderId → 等待中的渲染回调。 */
  draws: Map<string, DrawWait>
  /** 渲染结果先于注册到达时排队。 */
  pendingDraws: Map<string, DrawResult>
  /** page_capture 工具：captureId → 等待中的捕获回调。 */
  captures: Map<string, CaptureWait>
  /** 捕获结果先于注册到达时排队。 */
  pendingCaptures: Map<string, PendingCapture>
  /** 通道级禁用工具（如飞书桥接禁用依赖前端页面的工具）：精确名或 {agent}_ 前缀名匹配。 */
  disabledTools: string[]
  /** 交互模式（none/multi_turn/realtime，DESIGN「交互模式」）：工具声明的最低可用模式高于此值时被禁用。 */
  interactionMode: InteractionMode
  /** 发起任务用户的角色（admin/user；公共资源权限判定用，如公共 mini-tool 仅管理员可写）。 */
  role?: string
  /** 输出方式（final_only/streaming）：final_only 不推送文本增量与推理流（仅最终响应）。 */
  outputMode: OutputMode
  /** 任务级环境变量（run 时组装快照的同一引用）：ask_env 用户填值后原地更新，工具后续读取立即生效。 */
  env: Record<string, string>
  /** ask_env 工具：envId → 等待中的请求回调。 */
  envRequests: Map<string, EnvWait>
  /** 环境变量值先于注册到达时排队。 */
  pendingEnvRequests: Map<string, string>
}

export interface AgentEngineOptions {
  provider: LLMProvider
  /** 任务级主模型 Provider 解析：env 配置 GEBAI_LLM_* 时返回重建的 Provider（覆盖启动配置）；
   *  无覆盖返回 undefined（调用方沿用 opts.provider 实例）。 */
  resolveProvider?: (env: Record<string, string>) => LLMProvider | undefined
  registry: ToolRegistry
  store: SessionStore
  env: EnvManager
  sandbox: Sandbox
  events: EventBus
  config: ServerConfig
  subAgents: SubAgentManager
  /** 模型接口异常/空响应的重试退避基数（毫秒，缺省 800；测试注入小值加速重试用例）。 */
  retryBackoffMs?: number
  /** 定时任务调度器（GEBAI_CRON_ENABLED=true 时注入，cron_* 工具经 ToolContext 绑定）。 */
  cron?: import("./cron").CronManager
  /** page_capture 等待前端捕获回传的超时（毫秒，默认 30 秒；测试可注入短超时）。 */
  captureTimeoutMs?: number
  /** 工具执行超时兜底（毫秒，默认 9 分钟；测试可注入短超时验证超时返回给模型）。 */
  toolTimeoutMs?: number
  /** LLM 流式调用读空闲超时（毫秒，默认 120s；测试可注入短超时验证假死中止）。 */
  llmIdleTimeoutMs?: number
  /** 认证模式（"server"=服务模式多用户隔离）。无交互通道（REST）的审批策略按此分级：本地模式保持「自动通过」，
   * 服务模式下需审批工具一律拒绝执行（防普通用户经 REST 免审批执行 sh/py 等敏感工具）。 */
  authMode?: "local" | "server"
}

export class AgentEngine {
  private tasks = new Map<string, TaskState>()

  /** 会话级已读文件追踪（fileGuard 防误覆盖，DESIGN「write 防误覆盖守卫」）：sessionId → 已读绝对路径集合。
   *  read/edit/apply_patch/write 成功后登记，write 整体覆盖「已存在但未读过」的文件前据此拦截；
   *  会话删除经 forgetSession 释放（进程内无界增长防护）。 */
  private readFiles = new Map<string, Set<string>>()
  /** 单会话已读登记上限（防长会话无界增长；超出整表重置——守卫降级为「需重读」，保护语义不破坏）。 */
  private static readonly READ_TRACK_CAP = 2000

  constructor(private opts: AgentEngineOptions) {}

  isRunning(sessionId: string): boolean {
    return this.tasks.has(sessionId)
  }

  /** 会话删除时释放其运行态（已读文件追踪等）；幂等，供 REST/WS 删除会话入口调用。 */
  forgetSession(sessionId: string): void {
    this.readFiles.delete(sessionId)
  }

  /** 取（或建）会话的 fileGuard：标记/查询本会话已读文件（绝对路径）。 */
  private fileGuardFor(sessionId: string): NonNullable<ToolContext["fileGuard"]> {
    let set = this.readFiles.get(sessionId)
    if (!set) {
      set = new Set<string>()
      this.readFiles.set(sessionId, set)
    }
    const tracked = set
    return {
      markRead(absPath: string) {
        if (tracked.size >= AgentEngine.READ_TRACK_CAP) tracked.clear()
        tracked.add(absPath)
      },
      hasRead(absPath: string) {
        return tracked.has(absPath)
      },
    }
  }

  /** 装载模式写范围守卫：按**调用时点**的会话装载名单（loadedSubAgents）收集各子Agent 声明的
   *  SubAgentDef.writeGuard 并依次校验——任务中途 agent_load 装载（如 self_optimize）后立即生效；
   *  任一守卫返回非空即拒绝。无声明守卫的子Agent 不产生开销（快速返回 null）。 */
  private async sessionWriteGuard(sessionId: string, user: string, env: Record<string, string>, absPaths: string[]): Promise<string | null> {
    const names = (await this.opts.store.load(sessionId, user))?.loadedSubAgents ?? []
    for (const n of names) {
      const g = this.opts.subAgents.def(n)?.writeGuard
      if (!g) continue
      const msg = g(env, absPaths)
      if (msg) return msg
    }
    return null
  }

  /** 新会话模式写范围守卫：预加载子Agent 名单静态已知，静态组合各 SubAgentDef.writeGuard。 */
  private defsWriteGuard(agentNames: string[], env: Record<string, string>): ToolContext["writeGuard"] {
    const guards = agentNames
      .map((n) => this.opts.subAgents.def(n)?.writeGuard)
      .filter((g): g is NonNullable<import("./types").SubAgentDef["writeGuard"]> => !!g)
    if (!guards.length) return () => null
    return (absPaths) => {
      for (const g of guards) {
        const msg = g(env, absPaths)
        if (msg) return msg
      }
      return null
    }
  }

  /** 模型上下文窗口（token）：0 表示未知/未配置；前端用于上下文占比显示（context 使用比例）。 */
  contextWindow(): number {
    return this.opts.provider.capabilities().maxContextTokens || 0
  }

  /** 用户可访问的进行中会话（WS 状态快照 running 列表用）。 */
  async runningIds(userId: string): Promise<string[]> {
    const out: string[] = []
    for (const id of this.tasks.keys()) {
      const s = await this.opts.store.load(id, userId)
      if (s) out.push(id)
    }
    return out
  }

  cancel(sessionId: string): void {
    const task = this.tasks.get(sessionId)
    if (task) {
      task.cancelled = true // 用户停止标记：审批消费处据此短路（不写「用户拒绝」虚假记录）
      task.controller.abort()
      // 统一解开所有挂起等待（审批/选择/画图/捕获）：仅 abort 信号不会中断 await 中的 promise，
      // 不 resolve 会导致 runLoop 永久挂起、任务收尾不完成（isRunning 残留），
      // 下一次 prompt 被 "task already running" 拒绝——表现为「中断后要发两次才能继续」
      for (const a of task.approvals.values()) {
        clearTimeout(a.timer)
        a.resolve("timeout") // 取消解开等待（消费处由 cancelled 标记短路，不落盘）
      }
      task.approvals.clear()
      for (const ch of task.choices.values()) {
        clearTimeout(ch.timer)
        ch.resolve(null)
      }
      task.choices.clear()
      for (const d of task.draws.values()) {
        clearTimeout(d.timer)
        d.resolve(null)
      }
      task.draws.clear()
      for (const c of task.captures.values()) {
        clearTimeout(c.timer)
        c.resolve({ html: "", error: "cancelled" })
      }
      task.captures.clear()
      task.pendingCaptures.clear()
      task.pendingDecisions.clear()
      task.pendingChoices.clear()
      task.pendingDraws.clear()
      task.envRequests.clear()
      task.pendingEnvRequests.clear()
    }
  }

  async decideApproval(sessionId: string, toolCallId: string, approve: boolean): Promise<void> {
    const task = this.tasks.get(sessionId)
    if (!task) return
    const approval = task.approvals.get(toolCallId)
    if (approval) {
      clearTimeout(approval.timer)
      task.approvals.delete(toolCallId)
      approval.resolve(approve ? "approved" : "rejected")
    } else {
      // decision arrived before the approval was registered; queue it
      task.pendingDecisions.set(toolCallId, approve)
    }
    // 拒绝审批 = 停止当前会话生成：不再让模型调整方案继续执行（超时自动拒绝不停止，仅显式拒绝触发）。
    // 同步 abort 但不设 cancelled 标记：审批消费处区分「用户停止（短路不落盘）」与
    // 「显式拒绝（落盘拒绝消息后由下一轮 abort 检查结束）」，无延迟窗口竞态
    if (!approve) {
      const task = this.tasks.get(sessionId)
      if (task) task.controller.abort()
    }
  }

  /** 提交用户选择（ask_user 工具等待的选择）；null 表示拒绝，string 为单选（选项/自定义文本），string[] 为多选。 */
  async decideChoice(sessionId: string, choiceId: string, selection: string | string[] | null): Promise<void> {
    const task = this.tasks.get(sessionId)
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
      task.pendingChoices.set(choiceId, result)
    }
  }

  /** 提交前端渲染结果（draw 工具等待的渲染回传）。 */
  async decideDrawResult(sessionId: string, renderId: string, result: DrawResult): Promise<void> {
    const task = this.tasks.get(sessionId)
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
   * 前端渲染图表并阻塞等待结果（draw 工具）。
   * 发布 event.draw.render（含 renderId/源码/图表语言供前端渲染），成功/失败经 decideDrawResult 回传；
   * 5 秒超时返回 null（画图能力受限）。
   */
  private async waitForDraw(sessionId: string, render: { code: string; name?: string; format?: DiagramFormat }, signal?: AbortSignal): Promise<DrawResult | null> {
    const task = this.tasks.get(sessionId)!
    const renderId = crypto.randomUUID().replace(/-/g, "")
    this.publish(sessionId, "event.draw.render", { renderId, code: render.code, name: render.name ?? "diagram", format: render.format ?? "plantuml", sessionId })
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
      task.draws.set(renderId, { resolve: done, timer })
    })
  }

  /** 提交前端页面捕获结果（page_capture 工具等待的捕获回传）。 */
  async decideCaptureResult(sessionId: string, captureId: string, result: CaptureResult): Promise<void> {
    const task = this.tasks.get(sessionId)
    if (!task) return
    // 输入防线（任意 WS 客户端可发 capture.result，前端截断不可信）：
    // html 截断到落盘上限；截图 base64 超限（约 8MB 解码体积）丢弃；error 截断防输出注入
    const html = result.html.slice(0, PAGE_CAPTURE_HTML_LIMIT)
    const imageBase64 = result.imageBase64 && result.imageBase64.length <= VISION_MAX_IMAGE_BYTES * 1.4 ? result.imageBase64 : undefined
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
   * decideCaptureResult 回传；30 秒超时返回 null（前端离线或捕获超时）。
   */
  /** 自动审批实时判定：任务 env 快照（含浏览器本地注入）或会话实时 env 任一为 true 即跳过审批——会话运行中开启自动审批即时生效（关闭需下次任务）。 */
  private async isApprovalSkipped(sessionId: string, user: string, env: Record<string, string>): Promise<boolean> {
    // 无交互模式（REST 等单次请求通道）：单用户本地模式无人可询问，需审批工具自动通过；
    // 多用户隔离模式下不允许自动通过——普通用户不得经 REST 免审批执行敏感工具（见审批点拒绝逻辑）
    if (this.tasks.get(sessionId)?.interactionMode === "none" && this.opts.authMode !== "server") return true
    if (env.GEBAI_APPROVAL_SKIP === "true") return true
    const sessionEnv = await this.opts.store.getEnv(sessionId, user)
    return sessionEnv.GEBAI_APPROVAL_SKIP === "true"
  }

  /** 多用户 + 无交互通道下的需审批工具：不进入等待（无人可审批），直接返回拒绝文案。 */
  private noInteractionDenied(toolName: string): string {
    return `工具调用 ${toolName} 需要审批，但当前通道为无交互模式（多用户服务端部署），无法向用户确认，已拒绝执行。请调整方案，改用无需审批的操作，或通过 Web UI（WS 通道）执行。`
  }

  private async waitForCapture(sessionId: string, opts: { fullPage?: boolean; delayMs?: number } = {}, signal?: AbortSignal): Promise<CaptureResult | null> {
    const task = this.tasks.get(sessionId)!
    const captureId = crypto.randomUUID().replace(/-/g, "")
    const timeoutMs = this.opts.captureTimeoutMs ?? CAPTURE_TIMEOUT
    this.publish(sessionId, "event.capture.request", { captureId, sessionId, fullPage: opts.fullPage === true, delay: opts.delayMs ?? 0 })
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
      task.captures.set(captureId, { resolve: done, timer })
    })
  }

  /**
   * 向用户提出选择并阻塞等待结果（ask_user 工具）。
   * 发布 event.choice.request（含 choiceId/multi 供 UI 提交）；返回 ChoiceResult：
   * 单选/自定义文本为 { kind: "option" }，多选为 { kind: "multi" }，用户拒绝为 { kind: "refuse" }，超时（审批超时同值）为 null。
   */
  private async waitForChoice(sessionId: string, prompt: string, options: ChoiceOption[], multi?: boolean, signal?: AbortSignal): Promise<ChoiceResult> {
    const task = this.tasks.get(sessionId)!
    const choiceId = crypto.randomUUID().replace(/-/g, "")
    this.publish(sessionId, "event.choice.request", { choiceId, prompt, options, multi: !!multi, sessionId })
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
      task.choices.set(choiceId, { resolve: done, timer })
    })
  }

  /** ask_env 可设置的变量名校验：标识符格式 + 拒绝 __proto__（原型污染）+ 多用户模式拒绝审批跳过键
   * （GEBAI_APPROVAL_SKIP 仅管理员经正式 env 通道设置，ask_env 是模型驱动的第四通道，不得绕过）。 */
  private isEnvNameAllowed(name: string): boolean {
    if (name === "__proto__" || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) return false
    if (name === "GEBAI_APPROVAL_SKIP" && this.opts.authMode === "server") return false
    return true
  }

  /** 提交用户填写的环境变量值（ask_env 工具等待的请求回传）：非空值写入任务 env（ctx 同引用，工具后续读取立即生效）。 */
  async decideEnvResult(sessionId: string, envId: string, value: string | null): Promise<void> {
    const task = this.tasks.get(sessionId)
    if (!task) return
    const req = task.envRequests.get(envId)
    if (req) {
      clearTimeout(req.timer)
      task.envRequests.delete(envId)
      if (value != null && value !== "" && this.isEnvNameAllowed(req.name)) {
        task.env[req.name] = value
        req.resolve(true)
      } else {
        req.resolve(false)
      }
    } else {
      // 值先于注册到达（并发竞态）：排队，waitForEnv 注册时立即消费
      task.pendingEnvRequests.set(envId, value ?? "")
    }
  }

  /**
   * 向用户请求设置环境变量并阻塞等待（ask_env 工具）。
   * 发布 event.env.request（含 envId/name/description/secret 供前端弹窗填值）；
   * 用户提交后值写入任务 env（本次任务后续工具立即生效）并返回 true；拒绝/超时（审批超时同值）返回 false。
   */
  private async waitForEnv(sessionId: string, name: string, description: string, secret: boolean, signal?: AbortSignal): Promise<boolean> {
    const task = this.tasks.get(sessionId)!
    // 变量名合法性 + 敏感键限制：非法名/多用户下审批跳过键直接拒绝（不发布卡片、不阻塞）
    if (!this.isEnvNameAllowed(name)) return false
    const envId = crypto.randomUUID().replace(/-/g, "")
    this.publish(sessionId, "event.env.request", { envId, name, description, secret, sessionId })
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
      task.envRequests.set(envId, { name, resolve: done, timer })
    })
  }

  private publish(sessionId: string, type: string, payload: Record<string, unknown>) {
    this.opts.events.publish({ type, sessionId, payload, timestamp: Date.now() })
  }

  /**
   * 上下文压缩（DESIGN「上下文保护」）：将 [from,to) 区间可压缩（assistant/tool）历史消息替换为一条 LLM 摘要消息。
   * 系统提示词与用户输入不压缩不改变（isProtectedMessage：不选进区间、不进摘要输入、区间夹带原位保留）。
   * - 主动压缩：用户/UI 通过 `session.compact` 或 REST compact 触发，scope 指定区间
   * - 自动压缩：run() 在上下文接近窗口阈值时调用（压缩最早一半可压缩消息）
   * 摘要生成失败时降级为滚动裁剪（仅丢弃，注入占位摘要提示），保证压缩不阻塞任务。
   */
  async compactSession(
    sessionId: string,
    user: string,
    scope?: "all" | { from: number; to: number },
    provider?: LLMProvider,
  ): Promise<{ compacted: number; summary: string }> {
    const session = await this.opts.store.load(sessionId, user)
    if (!session) throw new Error(`会话不存在: ${sessionId}`)
    // Provider 未显式指定（UI/REST 主动压缩入口）时按合并后 env 解析：用户/会话级模型配置同样生效
    const llm = provider ?? (this.opts.resolveProvider ? this.opts.resolveProvider(await this.opts.env.resolve(sessionId, user)) : this.opts.provider)
    const messages = session.messages
    // 可压缩消息：排除受保护消息（isProtectedMessage：系统提示词含装载提示词/压缩摘要、用户输入、
    // 新会话执行存档）——用户输入与系统提示词不压缩不改变（不选进区间、不进摘要输入，
    // 区间夹带时由 compactMessages 原位保留）；重复压缩只压缩新增长的 assistant/tool 历史
    const compactable: number[] = []
    for (let i = 0; i < messages.length; i++) {
      if (!isProtectedMessage(messages[i])) compactable.push(i)
    }
    if (compactable.length < 2) return { compacted: 0, summary: "" }

    let from: number
    let to: number
    if (scope && typeof scope === "object") {
      from = Math.max(0, Math.min(scope.from, messages.length))
      to = Math.min(messages.length, scope.to)
    } else if (scope === "all") {
      from = compactable[0]
      to = compactable[compactable.length - 1] + 1
    } else {
      // 默认：压缩最早部分，保留最近一半（保证最新上下文完整）
      const keep = Math.ceil(compactable.length * COMPACT_KEEP_RATIO)
      const target = compactable.slice(0, Math.max(0, compactable.length - keep))
      if (target.length < 2) return { compacted: 0, summary: "" }
      from = target[0]
      to = target[target.length - 1] + 1
    }
    if (from >= to) return { compacted: 0, summary: "" }

    const slice = messages.slice(from, to)
    const kept = slice.filter(isProtectedMessage)
    const removed = slice.length - kept.length
    if (removed === 0) return { compacted: 0, summary: "" } // 区间内仅剩受保护消息：无可压缩内容
    // 摘要输入只含将被移除的消息（受保护消息原样保留，无需进摘要）
    const summary = await this.summarize(slice.filter((m) => !isProtectedMessage(m)), llm)
    await this.opts.store.compactMessages(sessionId, user, { from, to, summary })
    this.publish(sessionId, "event.message.compact", { from, to, count: removed, summary, sessionId })
    return { compacted: removed, summary }
  }

  /** 上下文占用估算（真实 usage 基线 + 未发送增量；无基线全量估算，CJK 感知）。 */
  private async estimateContext(sessionId: string, user: string, systemPrompt: string, history: MessageLike[]): Promise<number> {
    const baseline = await this.opts.store.load(sessionId, user)
    if (baseline?.ctxInputTokens !== undefined) {
      return baseline.ctxInputTokens + estimateTokens(history.slice(Math.max(0, baseline.ctxAtMessage ?? 0)))
    }
    return estimateTokens([{ role: "system", content: systemPrompt }, ...history])
  }

  /**
   * 溢出硬护栏（上下文压缩无法收敛时的最后防线）：受保护消息让路——
   * 1) 最旧带图片附件的用户消息：附件图片降级为文本说明（图片永久占窗口且不参与压缩）；
   * 2) 仍无图片可降级：最旧用户消息内容替换为裁剪占位（原文仍在 chat.json，UI 可查、不丢数据）。
   * 最新一条用户消息（本次任务的输入）永不裁剪——裁掉当前任务输入则任务失去意义。
   * 返回是否发生降级。
   */
  private async degradeProtectedMessages(sessionId: string, user: string): Promise<boolean> {
    const session = await this.opts.store.load(sessionId, user)
    if (!session) return false
    let lastUserIdx = -1
    for (let i = session.messages.length - 1; i >= 0; i--) {
      if (session.messages[i].role === "user") {
        lastUserIdx = i
        break
      }
    }
    // 1) 图片附件降级（从最旧开始，一次降级一条消息的全部图片附件）
    for (let i = 0; i < session.messages.length; i++) {
      if (i === lastUserIdx) continue
      const m = session.messages[i]
      if (m.role !== "user" || !m.attachments?.length) continue
      const images = m.attachments.filter((a) => VISION_MIME_SET.has(a.mime))
      if (!images.length) continue
      m.attachments = m.attachments.filter((a) => !VISION_MIME_SET.has(a.mime))
      const note = images.map((a) => `[历史图片已降级为路径说明: ${a.path}（${a.name}），可用 vision/read 工具按需查看]`).join(" ")
      m.content = `${note}\n${m.content}`
      console.warn(`[engine] 会话 ${sessionId} 溢出护栏：最旧用户消息的 ${images.length} 张图片降级为文本说明`)
      await this.opts.store.save(session)
      return true
    }
    // 2) 最旧用户消息裁剪占位（最新一条用户消息即本次任务输入，跳过）
    for (let i = 0; i < session.messages.length; i++) {
      if (i === lastUserIdx) continue
      const m = session.messages[i]
      if (m.role !== "user" || typeof m.content !== "string" || m.content.length <= 500) continue
      if (m.content.startsWith("[历史消息已裁剪")) continue
      const size = m.content.length
      m.content = `[历史消息已裁剪（原 ${size} 字符，原文仍在会话记录中可查看）] ${m.content.slice(0, 200)}`
      console.warn(`[engine] 会话 ${sessionId} 溢出护栏：最旧用户消息（${size} 字符）裁剪为占位`)
      await this.opts.store.save(session)
      return true
    }
    return false
  }

  /**
   * 任务中途工具结果回收（长任务上下文护栏）：真实 usage 逼近窗口上限时，把最早的
   * 旧工具结果替换为归档占位（每轮一条，渐进收敛；保留最近 RECLAIM_KEEP_RECENT 条，
   * 模型近期操作上下文不受影响）。超长结果本就落盘 tmp/truncated/，原文可经文件面板读取。
   * 返回本次回收的条目（调用方同步替换内存消息副本）。
   */
  private async recycleOldToolOutputs(sessionId: string, user: string): Promise<Array<{ toolCallId: string; saved: number }>> {
    const session = await this.opts.store.load(sessionId, user)
    if (!session) return []
    const out: Array<{ toolCallId: string; saved: number }> = []
    const plainTools = session.messages.filter((m) => m.role === "tool" && !m.sessionRun && !m.session && typeof m.content === "string")
    const recent = plainTools.slice(-RECLAIM_KEEP_RECENT)
    for (const m of plainTools) {
      if (recent.includes(m)) continue
      const content = m.content as string
      if (content.length <= 800) continue
      const saved = content.length
      m.content = `[工具结果已归档回收（原 ${saved} 字符；完整内容见会话文件面板，可用 read 读取）]`
      console.warn(`[engine] 会话 ${sessionId} 中途回收：工具 ${m.name} 结果（${saved} 字符）归档为占位`)
      out.push({ toolCallId: String(m.toolCallId ?? ""), saved })
      break
    }
    if (out.length) await this.opts.store.save(session)
    return out
  }

  /** 用 LLM 生成最早历史消息的摘要；失败返回降级占位文本（滚动裁剪语义）。
   *  默认用启动 Provider；自动压缩（任务内触发）可传入任务级 Provider（与任务同模型）。 */
  private async summarize(slice: Array<import("@gebai/sdk").Message>, provider: LLMProvider = this.opts.provider): Promise<string> {
    try {
      const text = slice
        .map((m) => `[${m.role}] ${m.content}`)
        .join("\n")
        .slice(0, SUMMARY_INPUT_LIMIT)
      const msgs: MessageLike[] = [
        {
          role: "system",
          content: "你是对话压缩器。将以下对话历史压缩为一段简洁中文摘要：保留关键决定、结论、任务进度、文件路径与待办，舍弃寒暄与过程细节。不超过 500 字，直接输出摘要正文。",
        },
        { role: "user", content: text },
      ]
      let summary = ""
      for await (const chunk of provider.chat(msgs)) {
        if (chunk.type === "text") summary += chunk.text
      }
      const trimmed = summary.trim()
      if (!trimmed) throw new Error("摘要为空")
      return trimmed.slice(0, SUMMARY_OUTPUT_LIMIT)
    } catch {
      // 摘要失败降级为滚动裁剪：仅丢弃，占位文本向模型说明历史已被裁剪
      return "[上下文已裁剪：历史消息过多，已丢弃最早部分。如需详情可查看会话文件。]"
    }
  }

  async run(
    sessionId: string,
    user: string,
    prompt: string,
    opts: {
      attachments?: AttachmentInput[]
      envOverride?: Record<string, string>
      messageId?: string
      disabledTools?: string[]
      interactionMode?: InteractionMode
      outputMode?: OutputMode
      /** 发起任务用户的角色（admin/user；公共资源权限判定用，如公共 mini-tool 仅管理员可写）。 */
      role?: string
    } = {},
  ): Promise<void> {
    if (this.tasks.has(sessionId)) throw new Error(`会话 ${sessionId} 已有任务在运行`)
    const session = await this.opts.store.load(sessionId, user)
    if (!session) throw new Error(`会话不存在: ${sessionId}`)
    // 会话级子Agent 装载保障（DESIGN「装载 vs 新会话执行」）：新会话按启动预载名单初始化
    // （工具注册 + 提示词 system 消息写入会话记录），恢复历史会话时按会话记录重新注册工具并补齐提示词消息
    await this.ensureSessionAgents(session)
    const controller = new AbortController()
    const task: TaskState = { controller, approvals: new Map(), pendingDecisions: new Map(), retries: new Map(), choices: new Map(), pendingChoices: new Map(), draws: new Map(), pendingDraws: new Map(), captures: new Map(), pendingCaptures: new Map(), disabledTools: opts.disabledTools ?? [], interactionMode: opts.interactionMode ?? "realtime", outputMode: opts.outputMode ?? "streaming", role: opts.role, env: {}, envRequests: new Map(), pendingEnvRequests: new Map() }
    this.tasks.set(sessionId, task)

    try {
      const attachmentRefs = await this.saveAttachments(sessionId, user, opts.attachments || [])
      // 客户端携带的会话消息 id（撤回/反馈定位用）：合法才采用，否则服务端生成
      // （防非法 id 注入存储：仅限 8-64 位字母/数字/`-`/`_`，UUID 形式即满足）
      const messageId = opts.messageId && /^[A-Za-z0-9_-]{8,64}$/.test(opts.messageId) ? opts.messageId : crypto.randomUUID()
      // 超长用户输入落盘（DESIGN「上下文保护」预防策略）：全文写入会话 tmp/user_inputs/（原文不丢），
      // 消息正文保留头尾 + 文件引用，避免大段粘贴撑爆上下文；未超阈值原样不变
      const userContent = await spillLongUserInput(prompt, this.opts.store.getTmpDir(sessionId, user))
      await this.opts.store.appendMessage(sessionId, {
        id: messageId,
        role: "user",
        content: userContent.content,
        attachments: attachmentRefs,
        createdAt: Date.now(),
      })
    } catch (err) {
      this.publish(sessionId, "event.task.error", { error: String((err as Error).message || err) })
      this.tasks.delete(sessionId)
      return
    }

    try {
      // 浏览器本地环境变量（前端 localStorage）经 prompt 请求临时注入，仅本次任务生效，不持久化
      const env = { ...(await this.opts.env.resolve(sessionId, user)), ...(opts.envOverride || {}) }
      // 任务级 env 引用：ask_env 用户填值后原地更新（ctx.env 同一引用，工具后续读取立即生效）
      this.tasks.get(sessionId)!.env = env
      // 任务级主模型：env 配置 GEBAI_LLM_* 时重建 Provider（无覆盖时沿用启动实例）
      const taskProvider = this.opts.resolveProvider?.(env) ?? this.opts.provider
      const systemPrompt = this.buildSystemPrompt(sessionId, user, env)
      let history = await this.loadHistory(sessionId, user, taskProvider.capabilities().multimodal)
      // 自动压缩（DESIGN「上下文保护」）：上下文接近窗口阈值（80%）时先压缩最早历史，
      // 保证最新上下文完整、压缩过程对进行中的任务透明（阈值与摘要均用任务级模型）。
      // 占用口径：上次任务的真实 usage 基线（session.ctxInputTokens，含 system 与工具 schema）+
      // 基线之后的增量估算（history.slice(ctxAtMessage)）；无基线（新会话/接口不返回 usage/压缩后失效）全量估算兜底
      const cap = taskProvider.capabilities().maxContextTokens
      if (cap > 0) {
        // 迭代压缩：单次压缩可能不够（压缩后估算仍超阈值则继续压缩更近的区间）；
        // 压缩无效（无可压缩内容，如历史几乎全是用户输入/系统提示词）时启用硬护栏——
        // 受保护消息让路（历史图片降级为文本说明、最旧用户消息裁剪为占位），
        // 保证长会话存在可收敛的溢出兜底，而非等模型接口报错后任务失败
        let estimate = await this.estimateContext(sessionId, user, systemPrompt, history)
        for (let guard = 0; guard < 4 && estimate > cap * COMPACT_RATIO; guard++) {
          const before = history.length
          await this.compactSession(sessionId, user, undefined, taskProvider)
          history = await this.loadHistory(sessionId, user, taskProvider.capabilities().multimodal)
          estimate = await this.estimateContext(sessionId, user, systemPrompt, history)
          if (history.length >= before) {
            // 压缩无效（仅剩受保护消息）：硬护栏降级受保护消息（原文仍在会话存储中，不丢数据）
            const degraded = await this.degradeProtectedMessages(sessionId, user)
            if (!degraded) break
            history = await this.loadHistory(sessionId, user, taskProvider.capabilities().multimodal)
            estimate = await this.estimateContext(sessionId, user, systemPrompt, history)
          }
        }
      }
      const messages: MessageLike[] = [{ role: "system", content: systemPrompt }, ...history]

      // 待办续做：每轮会话完成（模型给出最终回复）后检查待办，pending/in_progress 未完成则
      // 追加提醒消息继续会话，直至全部完成或达到续做轮次上限（DESIGN「待办续做」）
      let continueRound = 0
      let finalText = ""
      let lastFinalText = ""
      let res: { text: string; reasoning: string; ctxInputTokens?: number; ctxCountedLen: number } | undefined
      for (;;) {
        res = await this.runLoop({
          sessionId,
          user,
          messages,
          systemPrompt,
          registry: this.opts.registry,
          signal: controller.signal,
          env,
          provider: taskProvider,
          // 任务级额外模型接口参数：浏览器本地注入 GEBAI_LLM_EXTRA_PARAMS 时覆盖 Provider 级配置（非法 JSON 忽略）
          extraParams: parseExtraParamsSafe(env.GEBAI_LLM_EXTRA_PARAMS),
          persist: (msg) => this.opts.store.appendMessage(sessionId, msg),
        })
        finalText = res.text

        if (finalText) {
          await this.opts.store.appendMessage(sessionId, {
            id: crypto.randomUUID(),
            role: "assistant",
            content: finalText,
            reasoning: res.reasoning.trim() ? res.reasoning.trim() : undefined,
            createdAt: Date.now(),
          })
        }
        if (controller.signal.aborted) break

        const todos = await this.opts.store.getTodos(sessionId)
        const pending = todos.filter((t) => t.status === "pending" || t.status === "in_progress")
        if (!pending.length) break
        if (continueRound >= MAX_TODO_CONTINUE) break

        const titleList = pending.map((t) => `- ${t.title}`).join("\n")
        // 文本重复检测：回复与上上轮完全相同 → 追加提醒，避免待办续做空转复述（DESIGN「重复检测」）
        const repeated = finalText !== "" && finalText === lastFinalText
        const contMsg = `【待办续做】当前会话仍有未完成的待办：\n${titleList}\n请继续执行，直至全部完成后再给出最终回复。${repeated ? "\n注意：你上一次的回复与上上一次完全相同，请勿复述，直接继续执行未完成的待办。" : ""}`
        messages.push({ role: "assistant", content: finalText })
        messages.push({ role: "user", content: contMsg })
        await this.opts.store.appendMessage(sessionId, {
          id: crypto.randomUUID(),
          role: "user",
          content: contMsg,
          createdAt: Date.now(),
        })
        lastFinalText = finalText
        continueRound++
        this.publish(sessionId, "event.todo.continue", { round: continueRound, remaining: pending.length, sessionId })
      }

      // 上下文大小与真实 usage 基线持久化（历史会话列表展示 + 下次 run 压缩判定基线）：
      // ctxInputTokens = 最近一次调用的真实 input tokens（含 system 与工具 schema）；ctxAtMessage = 那次调用
      // 已覆盖的历史消息条数（loadHistory 坐标，下次 run 以 history.slice 估算基线后的增量）；
      // ctxTokens（列表展示）= 真实基线 + 未发送增量估算；无真值（接口不返回 usage）时估算兜底（与 toSessionInfo 同口径）
      if (finalText) messages.push({ role: "assistant", content: finalText }) // 最终回复也在基线之后，计入增量估算
      const saved = await this.opts.store.load(sessionId, user)
      if (saved && res) {
        if (res.ctxInputTokens !== undefined) {
          saved.ctxInputTokens = res.ctxInputTokens
          saved.ctxAtMessage = Math.max(0, res.ctxCountedLen - 1)
          saved.ctxTokens = res.ctxInputTokens + estimateTokens(messages.slice(res.ctxCountedLen))
        } else {
          saved.ctxInputTokens = undefined
          saved.ctxAtMessage = undefined
          saved.ctxTokens = estimateCtxTokens(saved.messages)
        }
        await this.opts.store.save(saved)
      }
      this.publish(sessionId, "event.task.done", { sessionId })
    } catch (err) {
      const aborted = controller.signal.aborted
      this.publish(sessionId, "event.task.error", {
        error: aborted ? "cancelled" : String((err as Error).message || err),
      })
    } finally {
      for (const a of task.approvals.values()) clearTimeout(a.timer)
      for (const ch of task.choices.values()) clearTimeout(ch.timer)
      this.tasks.delete(sessionId)
    }
  }

  /** 任务级模型能力（多模态内联）作为参数传入：env 覆盖 GEBAI_LLM_MULTIMODAL 时按任务模型决定图片内联策略。 */
  private async loadHistory(sessionId: string, user: string, inlineMultimodal = this.opts.provider.capabilities().multimodal): Promise<MessageLike[]> {
    const session = await this.opts.store.load(sessionId, user)
    const out: MessageLike[] = []
    // 子Agent 装载提示词消息（loadedAgent 标记）：收集后统一置于历史最前（顺序保持）——
    // 装载发生在会话中途，若按原位透传会夹在 assistant(tool_calls) 与 tool 结果之间，
    // 接口校验失败（assistant tool_calls 后必须紧跟 tool 响应消息），装载后会话即无法继续
    const agentSystems: MessageLike[] = []
    // 历史图片内联窗口（从后往前数第几组图片附件）：超过窗口的降级为文本说明——
    // 图片永久占据上下文且不参与压缩，长会话会被历史图片占死窗口
    let recentImageGroups = 0
    // 先确定哪些位置的图片允许内联（从最新往回数 INLINE_IMAGE_RECENT 组）
    const msgs = session?.messages ?? []
    const inlineAllowed = new Array<boolean>(msgs.length).fill(false)
    for (let i = msgs.length - 1; i >= 0; i--) {
      const m = msgs[i]
      if (m.role !== "user" || !m.attachments?.some((a) => VISION_MIME_SET.has(a.mime))) continue
      if (recentImageGroups >= INLINE_IMAGE_RECENT) break
      inlineAllowed[i] = true
      recentImageGroups++
    }
    let idx = 0
    for (const m of msgs) {
      const allowInline = inlineAllowed[idx++]
      // 子Agent 执行过程消息：仅存档与前端回放，不进入主 LLM 上下文
      if (m.subAgent || m.session) continue
      if (m.role === "system") {
        if (m.loadedAgent && m.content) {
          agentSystems.push({ role: "system", content: m.content })
        } else if (m.compacted && m.content) {
          // 上下文压缩摘要消息：作为 assistant 角色注入（保持消息序合法，不混入 system 段）
          out.push({ role: "assistant", content: `[历史摘要] ${m.content}` })
        }
        continue
      }
      if (m.role === "user") {
        out.push({
          role: "user",
          content: m.attachments?.length ? await this.userAttachmentBlocks(sessionId, user, m.content, m.attachments, inlineMultimodal && allowInline) : m.content,
        })
      } else if (m.role === "assistant") {
        // 推理独立字段（Message.reasoning）绝不进模型上下文——此处仅映射 content；
        // stripThinkTags 兼容旧版数据（推理曾内嵌 content 的 <think> 块），回放时一并剥离
        const content = stripThinkTags(m.content)
        if (m.toolCalls?.length) {
          out.push({
            role: "assistant",
            content,
            toolCalls: m.toolCalls.map((tc) => ({ id: tc.id, name: tc.name, arguments: tc.arguments })),
          })
        } else if (content) {
          out.push({ role: "assistant", content })
        }
      } else if (m.role === "tool") {
        out.push({ role: "tool", toolCallId: m.toolCallId, content: m.content, name: m.name })
      }
    }
    return [...agentSystems, ...out]
  }

  /**
   * 用户消息附件 → LLM 内容块（DESIGN「多模态支持」）：
   * - 图片附件且主模型声明多模态能力且 ≤8MB：base64 内联为统一 `image` 块（携带 path/name/size
   *   元数据，供接口拒绝图片时自动降级还原为文本说明）
   * - 其余（非图片/超限/文件缺失/模型无多模态能力）：文本说明（路径 + MIME + 大小 + vision 工具指引），
   *   由模型决定用 vision（外挂视觉模型）/read 等工具处理
   */
  private async userAttachmentBlocks(sessionId: string, user: string, prompt: string, refs: AttachmentRef[], inlineImages = this.opts.provider.capabilities().multimodal): Promise<Array<Record<string, unknown>>> {
    const blocks: Array<Record<string, unknown>> = [{ type: "text", text: prompt }]
    const inline = inlineImages
    for (const ref of refs) {
      const isImage = VISION_MIME_SET.has(ref.mime)
      if (inline && isImage && ref.size <= ATTACHMENT_INLINE_LIMIT) {
        try {
          const abs = this.opts.sandbox.resolvePath(user, sessionId, ref.path)
          const buf = await Bun.file(abs).arrayBuffer()
          if (buf.byteLength > 0 && buf.byteLength <= ATTACHMENT_INLINE_LIMIT) {
            blocks.push({ type: "image", mime: ref.mime, data: Buffer.from(buf).toString("base64"), path: ref.path, name: ref.name, size: ref.size })
            continue
          }
        } catch {
          // 文件缺失/沙箱拒绝（如历史绝对路径）：降级为文本说明
        }
      }
      blocks.push({ type: "text", text: attachmentNote(ref, isImage) })
    }
    return blocks
  }

  /** 将消息中的内联图片块降级为文本说明（返回是否发生降级）。 */
  private degradeImageBlocks(messages: MessageLike[]): boolean {
    let changed = false
    for (const m of messages) {
      if (!Array.isArray(m.content)) continue
      let next: Array<Record<string, unknown>> | undefined
      for (const b of m.content) {
        if (b.type !== "image" || typeof b.path !== "string" || typeof b.name !== "string" || typeof b.mime !== "string") continue
        next ??= [...m.content]
        next[m.content.indexOf(b)] = { type: "text", text: attachmentNote({ path: b.path, name: b.name, mime: b.mime, size: Number(b.size ?? 0) }, true) }
        changed = true
      }
      if (next) m.content = next
    }
    return changed
  }

  private buildSystemPrompt(sessionId: string, user: string, env: Record<string, string>): string {
    const workdir = sessionPath(this.opts.config.gebaiHome, user, sessionId)
    const sandboxNote = this.opts.sandbox.enforcedFor(user)
      ? `（文件读写限定在此目录内，禁止越界）`
      : `（本地模式：不限制文件目录，可访问本机任意路径）`
    const parts = [
      `你是歌白智能体（GEBAI Agent）：极致动态扩展能力的智能体`,
      `当前会话临时工作目录: ${workdir}/tmp${sandboxNote}`,
      `复杂/多步操作优先数据流编排：可预判的多步固定流程用 flow 一次调用执行(支持 {{步骤id.data.字段}} 引用映射、when 分支、foreach/while 循环，编排前可用 tool_schemas 批量查询工具输出结构，语法详见 flow 工具描述)，或编写脚本（sh/py）一次执行，避免大量单步工具调用浪费往返与词元。`,
      `任务类型路由（子Agent 两种用法语义不同：默认 agent_load 装载——其工具并入当前工具集，装载后直接调用、全程在当前上下文完成，不创建独立执行；仅当需要干净上下文（结果隔离、不污染主上下文）或防止上下文膨胀（中间过程多、输出大）时，才用 agent_run 执行新会话——派生临时新会话，预加载一个或多个子Agent（完整系统提示词与工具）后阻塞执行，只返回最终结果；拿不准时先判断任务类型再选。按任务类型从下方「可选子Agent」清单选用——每个子Agent 的描述即其触发场景，匹配任务类型即装载或执行新会话；纯文本问答（无需工具）时直接回答，不装载子Agent。）`,
      // 项目绑定声明：装载模式下总Agent 直接使用子Agent 工具时按名操作绑定项目；
      // 未装载清单描述动态体现预置项目（方便总Agent 按项目名关联任务，完整清单注记仍只注入子Agent 提示词）
      this.subAgentProjectNote(user, env),
      this.opts.subAgents.systemPromptInjection((d) => this.agentDescription(d, user, env)),
    ]
    return parts.filter(Boolean).join("\n")
  }

  /** 项目绑定注入总Agent 系统提示词（{AGENT_NAME_UPPER}_PROJECT 环境变量，DESIGN「项目内置」）；预置项目说明与受限模式说明（{AGENT_NAME_UPPER}_PROJECTS / CODE_RESTRICT_PROJECTS）属 code 子Agent 行为约束，只注入子Agent 系统提示词（agent_run 执行新会话时），不注入总Agent 系统提示词。 */
  private subAgentProjectNote(user: string, env: Record<string, string>): string {
    const lines: string[] = []
    for (const d of this.opts.subAgents.list()) {
      const key = `${d.name.toUpperCase().replace(/-/g, "_")}_PROJECT`
      const raw = env[key]
      if (!raw) continue
      let root: string
      try {
        root = this.resolveAgentProjectRoot(user, raw)
      } catch {
        continue // 沙箱拒绝越界/绝对路径绑定：静默跳过（与非法 JSON 忽略一致）
      }
      // 仅声明绑定与根路径（agent_run 新会话执行该子Agent 时以其为项目根；装载模式下路径基准仍是会话目录，
      // 访问项目请用预置项目 project 参数或绝对路径，不宣称工作目录已切换）
      lines.push(`${d.name} 子Agent 项目绑定：${root}（agent_run 新会话执行该子Agent 时以其为项目根；装载模式下路径基准为会话目录，访问项目用 project 参数或绝对路径）`)
    }
    return lines.length ? `\n\n${lines.join("\n")}` : ""
  }

  /** 汇总所有已注册子Agent 的预置项目注册表（{AGENT_NAME_UPPER}_PROJECTS）：装载模式下总Agent 直接使用子Agent 工具时 project 参数路由用；同名去重（首个生效）。 */
  private allPresetProjects(user: string, env: Record<string, string>): PresetProject[] {
    const out: PresetProject[] = []
    const seen = new Set<string>()
    for (const d of this.opts.subAgents.list()) {
      for (const p of this.presetProjectsFor(user, env, d.name)) {
        if (seen.has(p.name)) continue
        seen.add(p.name)
        out.push(p)
      }
    }
    return out
  }

  /**
   * 会话级子Agent 装载保障：新会话（loadedSubAgents 未定义）按启动预载名单（GEBAI_PRELOAD_SUB_AGENTS，
   * 未配置 = 不预载任何）初始化；恢复历史会话按会话记录（loadedSubAgents）重新注册工具、补齐缺失的提示词消息。
   * 装载失败不中断任务（单个子Agent 失败跳过，仅告警）。
   */
  private async ensureSessionAgents(session: SessionData): Promise<void> {
    try {
      // env：装载提示词需动态拼接预置项目清单（{AGENT}_PROJECTS，与 runNewSession 的 presetNote 一致）
      const env = await this.opts.env.resolve(session.id, session.userId)
      const names = session.loadedSubAgents ?? this.opts.config.preloadSubAgents
      const added = await this.loadAgentsForSession(session, names, env)
      if (added.length) await this.opts.store.save(session)
    } catch (err) {
      console.warn(`[engine] 会话子Agent 装载保障失败: ${(err as Error).message}`)
    }
  }

  /** 预置项目清单注记（子Agent 提示词开头动态追加：名称/说明/路径，供模型按名使用 project 参数）。 */
  private buildPresetNote(agentName: string, projectRoot: string | undefined, presetProjects: PresetProject[]): string {
    if (!presetProjects.length) return ""
    return `\n预置项目（文件工具可用 project 参数指定，路径参数相对所选项目根；未指定时默认 ${projectRoot ? `${agentName.toUpperCase()}_PROJECT 项目根` : "工作目录"}）:\n${presetProjects
      .map((p) => `- ${p.name}${p.description ? `: ${p.description}` : ""}（${p.path}）`)
      .join("\n")}`
  }

  /** 子Agent 对外描述（动态）：静态 description + 预置项目摘要（{AGENT}_PROJECTS 名称: 说明（路径））——
   *  未装载清单（总Agent 提示词）与 agent_list 展示用，模型在装载前即可按项目名关联任务与代码位置。 */
  private agentDescription(d: { name: string; description: string }, user: string, env: Record<string, string>): string {
    const projects = this.presetProjectsFor(user, env, d.name)
    if (!projects.length) return d.description
    return `${d.description} 预置项目：${projects.map((p) => `${p.name}${p.description ? `: ${p.description}` : ""}（${p.path}）`).join("、")}`
  }

  /**
   * 装载子Agent 到会话：逐个 subAgents.load（幂等注册工具，返回本次实际装载集合含 self_optimize 连带 code），
   * 为每个新装载的子Agent 生成提示词 system 消息（### name（description）头 + 完整系统提示词 + 预置项目清单注记，
   * loadedAgent 标记）追加进会话 messages 并记录 loadedSubAgents（调用方负责 save）。已装载且提示词消息已存在的跳过（恢复场景幂等）。
   */
  private async loadAgentsForSession(session: SessionData, names: string[], env: Record<string, string>): Promise<string[]> {
    const added: string[] = []
    for (const name of names) {
      let loadedNow: string[]
      try {
        loadedNow = await this.opts.subAgents.load(name)
      } catch (err) {
        console.warn(`[engine] 装载子Agent ${name} 失败: ${(err as Error).message}`)
        continue
      }
      for (const n of loadedNow) {
        if (session.messages.some((m) => m.loadedAgent === n)) continue // 提示词消息已持久化（恢复场景）
        const def = this.opts.subAgents.def(n)
        if (!def) continue
        // 预置项目清单动态注入（装载模式闭环：模型按名使用 project 参数；与 runNewSession 的 presetNote 一致）
        const projectRoot = this.resolveSubAgentProject(session.userId, env, n)
        const presetNote = this.buildPresetNote(n, projectRoot, this.presetProjectsFor(session.userId, env, n))
        session.messages.push({
          id: crypto.randomUUID().replace(/-/g, ""),
          role: "system",
          loadedAgent: n,
          content: `### ${n}（${def.description}）\n${presetNote}${def.systemPrompt}`,
          createdAt: Date.now(),
        })
        added.push(n)
      }
    }
    if (added.length) session.loadedSubAgents = [...new Set([...(session.loadedSubAgents ?? []), ...added])]
    return added
  }

  /**
   * 装载子Agent 到指定会话（WS sub_agent.load 带 sessionId 用）：注册工具 + 提示词 system 消息写入会话记录并落盘。
   * 返回本次实际装载集合；会话不存在抛错。
   */
  async loadAgentToSession(sessionId: string, user: string, name: string): Promise<string[]> {
    const session = await this.opts.store.load(sessionId, user)
    if (!session) throw new Error(`会话不存在: ${sessionId}`)
    const env = await this.opts.env.resolve(sessionId, user)
    const added = await this.loadAgentsForSession(session, [name], env)
    if (added.length) await this.opts.store.save(session)
    return added
  }

  /**
   * 从会话卸载子Agent（与 loadAgentToSession 对称）：移除该子Agent 的装载提示词消息
   * （卸载后提示词不再占用上下文）与 loadedSubAgents 记录（会话恢复时不再按记录重新装载），
   * 并注销其工具注册。会话不存在抛错。
   */
  async unloadAgentFromSession(sessionId: string, user: string, name: string): Promise<void> {
    const session = await this.opts.store.load(sessionId, user)
    if (!session) throw new Error(`会话不存在: ${sessionId}`)
    session.messages = session.messages.filter((m) => !(m.role === "system" && m.loadedAgent === name))
    if (session.loadedSubAgents) {
      session.loadedSubAgents = session.loadedSubAgents.filter((n) => n !== name)
      if (!session.loadedSubAgents.length) session.loadedSubAgents = undefined
    }
    await this.opts.store.save(session)
    this.opts.subAgents.unload(name)
  }

  private buildContext(
    sessionId: string,
    user: string,
    env: Record<string, string>,
    signal: AbortSignal,
    opts?: { workdir?: string; resolveBase?: string; projects?: PresetProject[]; role?: string; messages?: MessageLike[]; registry?: ToolRegistry; writeGuard?: ToolContext["writeGuard"] },
    depth = 0,
  ): ToolContext {
    const store = this.opts.store
    const sandbox = this.opts.sandbox
    const self = this
    const workdir = opts?.workdir ?? sandbox.workdir(user, sessionId)
    const resolveRoot = opts?.resolveBase
    const projects = opts?.projects ?? []
    // 子进程取消信号：任务取消统一生效（子Agent 不设独立超时，无额外信号源）
    const execSignal = signal
    return {
      user,
      userRole: opts?.role,
      authMode: this.opts.authMode,
      sessionId,
      workdir,
      boundProjectRoot: resolveRoot,
      home: this.opts.config.gebaiHome,
      env,
      sandboxed: sandbox.enforcedFor(user),
      safeMode: this.opts.config.safeMode,
      fileGuard: this.fileGuardFor(sessionId),
      // 写范围守卫：显式传入（新会话模式：预加载子Agent 名单静态已知）或按会话装载名单动态收集（装载模式）
      writeGuard: opts?.writeGuard ?? ((absPaths: string[]) => this.sessionWriteGuard(sessionId, user, env, absPaths)),
      resolvePath: (p) => {
        // 子Agent 项目绑定：路径以项目根为基准（沙箱约束用户限定项目内，豁免/本地模式放开）
        if (resolveRoot) return sandbox.enforcedFor(user) ? resolveInSandbox(resolveRoot, p) : resolve(resolveRoot, p)
        return sandbox.resolvePath(user, sessionId, p)
      },
      readFile: async (p) => (await Bun.file(p).text()),
      readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
      writeFile: async (p, content) => {
        const { mkdir, writeFile } = await import("node:fs/promises")
        const { dirname } = await import("node:path")
        await mkdir(dirname(p), { recursive: true })
        await writeFile(p, content)
      },
      writeBinaryFile: async (p, data) => {
        const { mkdir, writeFile } = await import("node:fs/promises")
        const { dirname } = await import("node:path")
        await mkdir(dirname(p), { recursive: true })
        await writeFile(p, data)
      },
      // 后端图表渲染（draw 工具 render=backend，三语言）：惰性加载组合渲染器
      // （PlantUML TeaVM 引擎 / Mermaid + happy-dom / D2 WASM + @resvg/resvg-js），仅按需时加载，不拖慢启动/测试
      renderDiagram: async (code, opts) => {
        const { createDiagramRenderer } = await import("./diagram-render")
        return createDiagramRenderer().renderPng(code, { format: opts?.format, background: opts?.background, maxWidth: opts?.maxWidth, maxHeight: opts?.maxHeight })
      },
      listFiles: () => store.listSessionFiles(sessionId, user),
      listDir: async (p) => {
        const { readdir, stat } = await import("node:fs/promises")
        const entries = await readdir(p, { withFileTypes: true })
        const out: import("@gebai/sdk").FileEntry[] = []
        for (const e of entries) {
          const full = `${p}/${e.name}`
          if (e.isDirectory()) out.push({ path: e.name, size: 0, modifiedAt: 0, isDir: true })
          else {
            try {
              const st = await stat(full)
              out.push({ path: e.name, size: st.size, modifiedAt: st.mtimeMs, isDir: false })
            } catch {
              out.push({ path: e.name, size: 0, modifiedAt: 0, isDir: false })
            }
          }
        }
        return out
      },
      deleteFile: async (p) => {
        const { rm } = await import("node:fs/promises")
        await rm(p, { recursive: true, force: true })
      },
      moveFile: async (from, to) => {
        const { mkdir, rename } = await import("node:fs/promises")
        // 与 writeFile 一致：目标父目录不存在时自动创建（rename 对缺失父目录直接 ENOENT）
        await mkdir(dirname(to), { recursive: true })
        await rename(from, to)
      },
      runCommand: (cmd, o) => sandbox.exec(cmd, { cwd: o?.workdir ?? workdir, env: o?.env ?? env, timeoutMs: o?.timeoutMs, input: o?.input, signal: o?.signal ?? execSignal, user }),
      uploadAttachment: async (ref) => ref.path,
      publish: (type, payload) => self.publish(sessionId, type, payload),
      projects,
      resolveProjectPath: (name) => {
        const p = projects.find((x) => x.name === name)
        if (!p) throw new Error(`未知预置项目: ${name}`)
        return p.path
      },
      getTodos: () => store.getTodos(sessionId),
      setTodos: (todos) => store.setTodos(sessionId, todos),
      registry: {
        schemas: (enabledOnly = true) => (opts?.registry ?? self.opts.registry).schemas(enabledOnly),
        resolve: (name) => {
          const rt = (opts?.registry ?? self.opts.registry).resolve(name)
          return rt ? { name: rt.name, tool: rt.tool } : undefined
        },
        getAgentNames: () => (opts?.registry ?? self.opts.registry).getAgentNames(),
      },
      listSubAgentDefs: () =>
        self.opts.subAgents.list().map((d) => ({ name: d.name, description: self.agentDescription(d, user, env), preload: d.preload, loaded: d.loaded })),
      loadSubAgent: async (name) => {
        // 装载子Agent 到当前会话：注册工具 + 提示词 system 消息写入会话记录并落盘；
        // 若当前 run 的 messages 可达，提示词消息同时并入系统前置段（紧跟主 system 提示词之后，
        // 装载后后续轮次立即生效，不必等下次 run；直接 push 到末尾会夹在 assistant(tool_calls)
        // 与 tool 结果之间导致接口校验失败）
        const session = await self.opts.store.load(sessionId, user)
        if (session) {
          const added = await self.loadAgentsForSession(session, [String(name)], env)
          if (added.length) {
            await self.opts.store.save(session)
            if (opts?.messages) {
              const newMsgs: MessageLike[] = []
              for (const n of added) {
                const msg = session.messages.find((m) => m.loadedAgent === n)
                if (msg) newMsgs.push({ role: "system", content: msg.content })
              }
              if (newMsgs.length) {
                let i = 0
                while (i < opts.messages.length && opts.messages[i].role === "system") i++
                opts.messages.splice(i, 0, ...newMsgs)
              }
            }
          }
        } else {
          // 新会话执行形态（临时会话无持久化 SessionData）：仅全局注册工具
          await self.opts.subAgents.load(String(name)).catch(() => {})
        }
      },
      // 执行新会话（agent_run 工具）：派生临时新会话、预加载子Agent 列表后执行任务（DESIGN「装载 vs 新会话执行」）；深度 +1 限制递归嵌套
      runNewSession: (agents, input) => self.runNewSession(sessionId, user, env, agents, input, signal, depth + 1),
      waitForChoice: (prompt, options, multi) => self.waitForChoice(sessionId, prompt, options, multi, execSignal),
      waitForEnv: (name, description, secret) => self.waitForEnv(sessionId, name, description ?? "", secret === true, execSignal),
      waitForDraw: (render) => self.waitForDraw(sessionId, render, execSignal),
      waitForCapture: (opts) => self.waitForCapture(sessionId, opts, execSignal),
      cron: self.opts.cron
        ? {
            add: (input) => self.opts.cron!.add(sessionId, user, input),
            list: () => self.opts.cron!.list(sessionId, user),
            remove: (id) => self.opts.cron!.remove(sessionId, user, id),
            update: (id, patch) => self.opts.cron!.update(sessionId, user, id, patch),
          }
        : undefined,
    }
  }

  /**
   * 流式调用模型（健壮性包装）：
   * - 接口异常（网络/HTTP/流中断）且本轮尚无任何产出（无文本、无工具调用）→ 指数退避重试；
   *   已有产出后断流不重试（避免重复输出），直接上抛
   * - 正常结束但空响应（无文本且无工具调用，含「只思考未输出」）→ 注入提示消息重试（不持久化）
   * - 重试耗尽仍失败时抛出中文错误（含已重试次数）
   */
  private async callModel(
    provider: LLMProvider,
    messages: MessageLike[],
    schemas: Array<{ name: string; description: string; parameters: Record<string, unknown> }>,
    signal: AbortSignal,
    onChunk?: (chunk: LLMChunk) => void,
    extraParams?: Record<string, unknown>,
  ): Promise<{ text: string; toolCalls: Array<{ id: string; name: string; arguments: Record<string, unknown>; argsError?: string }>; usage?: LLMUsage }> {
    const toolCalls: Array<{ id: string; name: string; arguments: Record<string, unknown>; argsError?: string }> = []
    let text = ""
    let reasoningSeen = false
    let attempts = 0
    // 重试注入的提示（仅本次调用，不写入会话历史）：泄漏/空响应时引导模型重新输出
    let hint = ""
    // 多模态图片块已降级标记（DESIGN「能力探测」自动降级）：接口拒绝图片块后一次性降级为文本说明
    let degraded = false
    // 本次调用的 usage 真值（服务端不返回时保持 undefined → 调用方估算兜底）；每次尝试重制，仅最后一次成功尝试生效
    let usage: LLMUsage | undefined
    for (;;) {
      const msgs = hint ? [...messages, { role: "user" as const, content: hint }] : messages
      try {
        usage = undefined
        for await (const chunk of this.chatWithIdleTimeout(provider, msgs, schemas, signal, extraParams)) {
          if (chunk.type === "text") text += chunk.text
          else if (chunk.type === "reasoning" && chunk.text?.trim()) reasoningSeen = true
          else if (chunk.type === "tool_call" && chunk.toolCall) toolCalls.push({ ...chunk.toolCall, argsError: chunk.toolArgsError })
          if (chunk.usage) usage = chunk.usage
          onChunk?.(chunk)
        }
      } catch (err) {
        if (signal.aborted) throw err
        // 配置类错误（模型接口地址未配置）：重试无意义，直接失败并保留指引文案
        if (err instanceof LLMConfigError) throw err
        // 图片块被接口拒绝（HTTP 4xx，如模型实际不支持 image_url）：一次性降级为文本说明后重试，
        // 模型可改走 vision 工具（外挂视觉模型路径），实现「无多模态能力自动降级」
        if (
          !text && !toolCalls.length && !reasoningSeen && !degraded &&
          /^模型接口错误（HTTP 4\d\d）/.test((err as Error).message) &&
          this.degradeImageBlocks(messages)
        ) {
          degraded = true
          attempts++
          await sleep((this.opts.retryBackoffMs ?? LLM_RETRY_BACKOFF_MS) * 2 ** (attempts - 1), signal)
          continue
        }
        // 已有产出（文本/工具调用/推理，推理已推送到前端）：不重试，避免重复输出
        if (text || toolCalls.length || reasoningSeen) throw err
        if (attempts >= LLM_RETRY_COUNT) {
          throw new Error(`模型接口调用失败（已重试 ${attempts} 次）: ${(err as Error).message}`)
        }
        hint = "" // 普通接口异常：与内容无关，不注入提示
        attempts++
        await sleep((this.opts.retryBackoffMs ?? LLM_RETRY_BACKOFF_MS) * 2 ** (attempts - 1), signal)
        continue
      }
      // 正常结束但空响应（无文本且无工具调用，含「只思考未输出」）：注入提示重试；耗尽后抛错
      if (!text && !toolCalls.length) {
        if (signal.aborted) throw new Error("cancelled")
        if (attempts < LLM_RETRY_COUNT) {
          hint = "你上一轮没有返回任何内容。请直接给出回答，或调用工具继续，不要复述要求。"
          attempts++
          await sleep(this.opts.retryBackoffMs ?? LLM_RETRY_BACKOFF_MS, signal)
          continue
        }
        throw new Error(`模型未返回任何内容（已重试 ${attempts} 次）`)
      }
      return { text, toolCalls, usage }
    }
  }

  /**
   * 流式调用带读空闲超时（接口假死防护）：连续超过 opts.llmIdleTimeoutMs（默认 120s）
   * 未收到任何 chunk 即中止本次调用——SSE 建立后网关/上游挂起会无限挂起任务，
   * 此前无任何总超时机制。用 Promise.race 硬超时（不依赖 provider 响应 abort——
   * fetch 会响应 abort 释放连接，但实现异常的迭代器可能不响应，超时必须强制生效）。
   * 中止后按接口异常路径处理（无产出走重试，有产出上抛）。取消仍经 signal 传播。
   */
  private async *chatWithIdleTimeout(
    provider: LLMProvider,
    messages: MessageLike[],
    schemas: Array<{ name: string; description: string; parameters: Record<string, unknown> }>,
    signal: AbortSignal,
    extraParams?: Record<string, unknown>,
  ): AsyncGenerator<LLMChunk> {
    const idleMs = this.opts.llmIdleTimeoutMs ?? LLM_IDLE_TIMEOUT_MS
    const iter = provider.chat(messages, { tools: schemas, signal, extraParams })[Symbol.asyncIterator]()
    try {
      for (;;) {
        let timer: ReturnType<typeof setTimeout> | undefined
        const timedOut = new Promise<never>((_, reject) => {
          timer = setTimeout(() => reject(new Error(`模型接口读超时（${Math.round(idleMs / 1000)} 秒无数据），判定接口假死`)), idleMs)
        })
        const next = await Promise.race([iter.next(), timedOut])
        if (timer) clearTimeout(timer)
        if (next.done) return
        yield next.value
      }
    } finally {
      void iter.return?.().catch(() => {})
    }
  }

  private async runLoop(params: {
    sessionId: string
    user: string
    messages: MessageLike[]
    systemPrompt: string
    registry: ToolRegistry
    signal: AbortSignal
    env: Record<string, string>
    provider: LLMProvider
    extraParams?: Record<string, unknown>
    persist: (msg: Message) => Promise<void>
  }): Promise<{ text: string; reasoning: string; ctxInputTokens?: number; ctxCountedLen: number }> {
    const { sessionId, user, messages, registry, signal, env, provider, extraParams, persist } = params
    let rounds = 0
    let lastText = ""
    let lastReasoning = ""
    // 上下文占用口径（真实 usage 为基准，估算只补未发送增量）：ctxInputTokens = 最近一次模型调用返回的
    // 真实 input tokens（含 system 与工具 schema）；ctxCountedLen = 那次调用时 messages 长度（已被真值覆盖的部分），
    // 其后的消息（增量）尚未发送，用 estimateTokens 估算补足——下一次真实调用会用真值接管
    let ctxInputTokens: number | undefined
    let ctxCountedLen = 0
    // 重复检测（DESIGN「重复检测」）：最近 MAX_REPEAT_WINDOW 次工具调用签名窗口，
    // 相同签名（工具+参数）出现 ≥MAX_REPEAT_HITS 次判定为无效重复 → 中断该次执行并注入引导提示；
    // 连续中断超过 MAX_REPEAT_STALLS 次终止循环，避免无效空转
    const recentCalls: string[] = []
    let repeatStalls = 0
    const ctx = this.buildContext(sessionId, user, env, signal, { projects: this.allPresetProjects(user, env), role: this.tasks.get(sessionId)?.role, messages }, 0)

    while (rounds < MAX_TOOL_ROUNDS) {
      if (signal.aborted) throw new Error("cancelled")
      const assistantMsgId = crypto.randomUUID()
      // 本轮推理全文累积（流式 publish 的同时落盘合并为 <think> 块，历史会话可见）
      let reasoningAcc = ""

      const { text, toolCalls, usage } = await this.callModel(provider, messages, registry.schemas().filter((s) => !this.isToolDisabled(sessionId, s.name, registry.resolve(s.name)?.tool)), signal, (chunk) => {
        // 输出方式：仅最终响应（final_only）不推送文本增量与推理流，流式输出（streaming）正常推送
        if (chunk.type === "text") {
          if (this.tasks.get(sessionId)?.outputMode === "streaming") this.publish(sessionId, "event.message.delta", { text: chunk.text, messageId: assistantMsgId, sessionId })
        } else if (chunk.type === "reasoning" && chunk.text?.trim()) {
          reasoningAcc += chunk.text
          if (this.tasks.get(sessionId)?.outputMode === "streaming") this.publish(sessionId, "event.message.reasoning", { text: chunk.text, sessionId })
        }
      }, extraParams)
      lastText = text
      lastReasoning = reasoningAcc
      if (usage?.inputTokens !== undefined) {
        ctxInputTokens = usage.inputTokens
        ctxCountedLen = messages.length
      }
      // 任务中途上下文回收：真实 usage 接近窗口上限（长任务逐步累积工具结果）时，
      // 渐进回收最早的旧工具结果（原文已落盘，替换为归档占位），防中途打满窗口被接口拒绝
      const cap2 = provider.capabilities().maxContextTokens
      if (cap2 > 0 && usage?.inputTokens !== undefined && usage.inputTokens > cap2 * MID_RUN_RECLAIM_RATIO) {
        const recycled = await this.recycleOldToolOutputs(sessionId, user)
        for (const r of recycled) {
          const mi = messages.findIndex((x) => x.role === "tool" && (x as { toolCallId?: string }).toolCallId === r.toolCallId)
          if (mi >= 0) {
            messages[mi] = {
              role: "tool",
              toolCallId: r.toolCallId,
              content: `[工具结果已归档回收（原 ${r.saved} 字符；完整内容见会话文件面板，可用 read 读取）]`,
            }
          }
        }
      }
      // 上下文大小实时推送（前端会话列表展示，单位 k）：真实 usage 基准 + 未发送增量估算
      this.publish(sessionId, "event.session.ctx", {
        ctxTokens: ctxInputTokens !== undefined ? ctxInputTokens + estimateTokens(messages.slice(ctxCountedLen)) : estimateTokens(messages),
      })
      if (!toolCalls.length) {
        this.publish(sessionId, "event.message.done", { text, messageId: assistantMsgId, sessionId })
        break
      }

      await persist({
        id: assistantMsgId,
        role: "assistant",
        content: text,
        reasoning: reasoningAcc.trim() ? reasoningAcc.trim() : undefined,
        toolCalls: toolCalls.map((tc) => ({ id: tc.id, name: tc.name, arguments: tc.arguments })),
        createdAt: Date.now(),
      })
      messages.push({ role: "assistant", content: text, toolCalls })

      // 重复判定后终止循环：本轮剩余工具调用跳过（保持 tool 消息序列完整），随后退出
      let stopped = false
      for (const tc of toolCalls) {
        if (signal.aborted) throw new Error("cancelled")
        if (stopped) {
          const abortNote = "任务已中止：模型持续重复相同工具调用。"
          await persist({ id: crypto.randomUUID(), role: "tool", content: abortNote, toolCallId: tc.id, name: tc.name, createdAt: Date.now() })
          messages.push({ role: "tool", content: abortNote, toolCallId: tc.id, name: tc.name })
          continue
        }
        if (this.repeatedCall(recentCalls, tc.name, tc.arguments)) {
          // 相同工具+相同参数已执行 ≥MAX_REPEAT_HITS 次：中断执行（结果必然相同），注入提示引导模型换方向
          repeatStalls++
          const note = `已中断重复的工具调用 ${tc.name}：参数与之前完全相同，重复执行只会得到相同结果。请分析原因、改用其他方法，或直接给出最终回答，不要重复相同操作。`
          if (repeatStalls > MAX_REPEAT_STALLS) stopped = true
          await persist({ id: crypto.randomUUID(), role: "tool", content: note, toolCallId: tc.id, name: tc.name, createdAt: Date.now() })
          messages.push({ role: "tool", content: note, toolCallId: tc.id, name: tc.name })
          continue
        }
        // 工具参数不是合法 JSON（接口聚合失败）：不执行（以 {} 执行会做出错误行为），回传原始片段让模型修正
        if (tc.argsError) {
          const errMsg = `工具参数 JSON 解析失败：模型输出的参数不是合法 JSON。原始片段: ${tc.argsError}。请重新调用 ${tc.name} 并输出合法的 JSON 参数。`
          await persist({ id: crypto.randomUUID(), role: "tool", content: errMsg, toolCallId: tc.id, name: tc.name, createdAt: Date.now() })
          messages.push({ role: "tool", content: errMsg, toolCallId: tc.id, name: tc.name })
          continue
        }
        const rt = registry.resolve(tc.name)
        if (!rt) {
          const errMsg = `未知工具: ${tc.name}`
          await persist({ id: crypto.randomUUID(), role: "tool", content: errMsg, toolCallId: tc.id, name: tc.name, createdAt: Date.now() })
          messages.push({ role: "tool", content: errMsg, toolCallId: tc.id, name: tc.name })
          continue
        }
        if (this.isToolDisabled(sessionId, rt.name, rt.tool)) {
          // 通道禁用工具（DESIGN「飞书机器人集成」）：模型不应调用，被调用时阻止执行并说明原因
          const disabledMsg = `工具 ${rt.name} 在当前通道不可用（该工具需要前端页面配合，而当前会话来自飞书聊天），请改用其他方式。`
          await persist({ id: crypto.randomUUID(), role: "tool", content: disabledMsg, toolCallId: tc.id, name: tc.name, createdAt: Date.now() })
          messages.push({ role: "tool", content: disabledMsg, toolCallId: tc.id, name: tc.name })
          continue
        }
        if (this.isRiskyInSafeMode(rt.name)) {
          // 安全模式（DESIGN「安全模式」）：风险工具（命令执行/写删文件/定时任务调度）不执行、不弹审批，
          // 直接返回限制信息给模型（模型仍可见 schema，可据此改用只读方案）
          const safeMsg = safeModeRestrictionMsg(rt.name)
          await persist({ id: crypto.randomUUID(), role: "tool", content: safeMsg, toolCallId: tc.id, name: tc.name, createdAt: Date.now() })
          messages.push({ role: "tool", content: safeMsg, toolCallId: tc.id, name: tc.name })
          continue
        }
        const requiresByArgs = await toolRequiresApproval(rt.tool, tc.arguments, ctx)
        const approvalSkipped = await this.isApprovalSkipped(sessionId, user, env)
        // 多用户隔离 + 无交互通道（REST）：无人可审批，默认需审批的工具直接拒绝（防普通用户经 REST 免审批执行敏感工具）；
        // approval:false 只放宽交互审批——硬门槛按剥离免审标记后的审批姿态解析（flow 嵌套步骤同规则），防模型自行声明免审绕过
        if (
          this.opts.authMode === "server" && this.tasks.get(sessionId)?.interactionMode === "none" && !approvalSkipped &&
          (requiresByArgs || (await toolRequiresApproval(rt.tool, stripApprovalFlags(tc.arguments) as Record<string, unknown>, ctx)))
        ) {
          const denied = this.noInteractionDenied(rt.name)
          await persist({ id: crypto.randomUUID(), role: "tool", content: denied, toolCallId: tc.id, name: tc.name, createdAt: Date.now() })
          messages.push({ role: "tool", content: denied, toolCallId: tc.id, name: tc.name })
          continue
        }
        const approvalRequired = requiresByArgs && !approvalSkipped
        if (approvalRequired) {
          const retries = this.tasks.get(sessionId)?.retries.get(tc.id) ?? 0
          this.publish(sessionId, "event.tool.call", { name: tc.name, arguments: tc.arguments, toolCallId: tc.id, requiresApproval: true })
          this.publish(sessionId, "event.approval.request", { toolCallId: tc.id, tool: tc.name, retries })
          const verdict = await this.waitApproval(sessionId, tc.id, rt.name, signal)
          // 取消路径（用户停止，cancelled 标记）：等待被信号解开后立即中止，不写「用户拒绝」虚假记录；
          // 显式拒绝：落盘拒绝消息后由下一轮 abort/循环检查结束；超时：落盘超时提示，模型可继续调整
          if (signal.aborted && this.tasks.get(sessionId)?.cancelled) throw new Error("cancelled")
          if (verdict !== "approved") {
            this.tasks.get(sessionId)?.retries.set(tc.id, retries + 1)
            const denied =
              verdict === "timeout"
                ? `工具调用 ${tc.name} 审批等待超时（5 分钟未响应），已跳过该调用。请调整方案，或先向用户说明需要审批的操作。`
                : `工具调用 ${tc.name} 已被用户拒绝。请调整方案后重试，或改用其他方法。`
            await persist({ id: crypto.randomUUID(), role: "tool", content: denied, toolCallId: tc.id, name: tc.name, createdAt: Date.now() })
            messages.push({ role: "tool", content: denied, toolCallId: tc.id, name: tc.name })
            continue
          }
        } else {
          this.publish(sessionId, "event.tool.call", { name: tc.name, arguments: tc.arguments, toolCallId: tc.id })
        }

        this.publish(sessionId, "event.tool.result.start", { name: tc.name, toolCallId: tc.id })
        // 取消/超时统一收口：停止按钮中断执行（脚本进程同步被杀），超时作为结果返回模型不结束任务
        const result = await this.runToolInterruptible(rt.tool, tc.arguments, ctx, signal, rt.name)
        // 兜底截断（不依赖工具自觉）：工具未自行截断的超长输出统一截断落盘，防上下文爆炸；
        // 结构化 data 与存档扩展字段原样保留（截断只作用于模型可见文本）
        const safe = !result.truncated && result.output.length > TRUNCATE_THRESHOLD
          ? { ...(await truncate(result.output, rt.name, ctx)), blocks: result.blocks, data: result.data, sessionRun: result.sessionRun }
          : result
        await persist({ id: crypto.randomUUID(), role: "tool", content: safe.output, blocks: safe.blocks, toolCallId: tc.id, name: tc.name, arguments: tc.arguments, sessionRun: safe.sessionRun, createdAt: Date.now() })
        messages.push({ role: "tool", content: safe.output, toolCallId: tc.id, name: tc.name })
        this.publish(sessionId, "event.tool.result", {
          name: tc.name,
          toolCallId: tc.id,
          truncated: !!safe.truncated,
          filePath: safe.filePath,
          output: safe.output,
          blocks: safe.blocks,
          sessionId,
        })
      }
      rounds++
      if (stopped) break
    }
    return { text: lastText, reasoning: lastReasoning, ctxInputTokens, ctxCountedLen }
  }

  /**
   * 重复调用检测（DESIGN「重复检测」）：将本次工具调用签名（工具名+参数 JSON）记入滚动窗口，
   * 窗口内相同签名已出现 MAX_REPEAT_HITS-1 次（本次为第 MAX_REPEAT_HITS 次）时判定为重复。
   */
  private repeatedCall(window: string[], name: string, toolArguments: Record<string, unknown>): boolean {    const sig = `${name}:${JSON.stringify(toolArguments ?? {})}`
    const hits = window.filter((s) => s === sig).length
    window.push(sig)
    if (window.length > MAX_REPEAT_WINDOW) window.shift()
    return hits >= MAX_REPEAT_HITS - 1
  }

  /**
   * 通道级工具禁用判定（如飞书桥接禁用依赖前端页面的工具）：
   * 禁用名匹配精确工具名，或匹配任意子Agent 命名空间下的同名工具（`{agent}_{tool}`）。
   * 交互模式：工具声明的最低可用模式（Tool.interaction）高于当前模式时同样禁用（schema 过滤 + 执行阻止）。
   */
  /**
   * 安全模式风险工具判定（GEBAI_SAFE_MODE=true 启动时加载，DESIGN「安全模式」）：
   * 全局工具精确名命中，或子Agent 工具按 `{agent}_{tool}` 剥离前缀后短名命中（如 code_sh → sh）。
   * 拦截语义与通道禁用不同：模型仍可见该工具 schema，调用时被阻止并返回限制信息（模型可据此调整方案）。
   */
  private isRiskyInSafeMode(name: string): boolean {
    if (!this.opts.config.safeMode) return false
    return isRiskyToolName(name)
  }

  private isToolDisabled(sessionId: string, name: string, tool?: Tool): boolean {
    const task = this.tasks.get(sessionId)
    if (!task) return false
    if (task.disabledTools.some((d) => name === d || name.endsWith(`_${d}`))) return true
    if (tool?.interaction) {
      const level: Record<InteractionMode, number> = { none: 1, multi_turn: 2, realtime: 3 }
      if (level[tool.interaction] > level[task.interactionMode]) return true
    }
    return false
  }

  /**
   * 工具执行包装（取消/超时统一收口）：
   * - 任务取消（停止按钮）：立即返回「已取消」；脚本类工具经 runCommand 传递的任务信号
   *   会同步杀进程（Sandbox.exec 的 signal 支持），因此真正执行的子进程会被打断
   * - 执行超时（TOOL_TIMEOUT_MS）：不结束任务，把「执行超时」作为工具结果返回给模型，
   *   由模型决定调整方案重试（脚本先由 sandbox 自身超时杀进程，此兜底覆盖挂起的非脚本工具）
   * - 工具异常：转为「工具执行失败」结果（与原有行为一致）
   */
  private runToolInterruptible(tool: Tool, args: Record<string, unknown>, ctx: ToolContext, signal: AbortSignal, name: string): Promise<ToolResult> {
    if (signal.aborted) return Promise.resolve({ output: `工具 ${name} 已取消（任务已停止）` })
    return new Promise<ToolResult>((resolve) => {
      let done = false
      let timer: ReturnType<typeof setTimeout>
      let onAbort: () => void
      const finish = (r: ToolResult) => {
        if (done) return
        done = true
        clearTimeout(timer)
        signal.removeEventListener("abort", onAbort)
        resolve(r)
      }
      onAbort = () => finish({ output: `工具 ${name} 已取消（任务已停止）` })
      signal.addEventListener("abort", onAbort, { once: true })
      timer = setTimeout(
        () => finish({ output: `工具 ${name} 执行超时（超过 ${Math.round((this.opts.toolTimeoutMs ?? TOOL_TIMEOUT_MS) / 1000)} 秒）已终止。请分析原因（死循环/等待外部资源等）后调整方案，或拆分为更小步骤重试。` }),
        this.opts.toolTimeoutMs ?? TOOL_TIMEOUT_MS,
      )
      tool.execute(args, ctx).then(
        (r) => finish(r),
        (err) => finish({ output: `工具执行失败: ${(err as Error).message}` }),
      )
    })
  }

  /** 新会话执行（agent_run 工具）：派生临时新会话，预加载指定子Agent 列表（完整系统提示词拼接+工具并入，
   *  模块语义的「装载」在独立上下文生效）后执行任务，阻塞返回最终结果与完整存档（DESIGN「装载 vs 新会话执行」）。 */
  private async runNewSession(
    sessionId: string,
    user: string,
    env: Record<string, string>,
    agents: string[],
    input: string,
    signal: AbortSignal,
    depth = 0,
  ): Promise<{ output: string; archive: SessionRunArchive }> {
    // 加固：去重 + 数量上限（异常/恶意调用拼装超大提示词会撑爆上下文）
    agents = [...new Set(agents)]
    // self_optimize 复用 code 的通用能力（def 只声明独有工具，提示词不复刻 code 工作流）：
    // 预加载 self_optimize 时自动连带预加载 code（与装载模式 SubAgentManager.load 的连带装载同规则），
    // 文件/分析类工具与通用工作流提示词由 code 提供——不重复定义
    if (agents.includes("self_optimize") && this.opts.subAgents.def("code") && !agents.includes("code")) {
      agents = ["code", ...agents]
    }
    if (agents.length > MAX_AGENTS_PER_RUN) throw new Error(`子Agent 数量超限（${agents.length} > ${MAX_AGENTS_PER_RUN}）`)
    const defs = agents.map((name) => {
      const def = this.opts.subAgents.def(name)
      if (!def) throw new Error(`未知子Agent: ${name}`)
      return def
    })
    if (depth >= SUBAGENT_DEPTH) throw new Error(`子Agent 递归深度超限: ${agents.join(",")}`)
    // 新会话工具注册：每个预加载子Agent 的工具以 {agent}_ 命名空间并入（装载语义）
    const reg = new BaseToolRegistry()
    let orchestrationInjected = false
    for (const def of defs) {
      reg.registerSubAgentTools(def.name, def.tools ?? {}, def.requiresApproval)
      // 简化定义（无工具，含纯 md 定义）：注入编排工具（原名暴露，无 {agent}_ 前缀，多 Agent 时仅注入一次）
      // ——支持组合式子 Agent 通过 agent_run/agent_list/agent_load 编排其他子 Agent
      if (!def.tools || Object.keys(def.tools).length === 0) {
        if (!orchestrationInjected) {
          reg.register(agentListTool)
          reg.register(agentLoadTool)
          reg.register(agentRunTool)
          orchestrationInjected = true
        }
      }
    }
    // 数据流编排能力（与总Agent 主循环一致）：新会话内同样可用 flow 一次编排多步、tool_schemas 批量查询输出结构
    reg.register(makeFlowTool())
    reg.register(toolSchemasTool)

    // 系统提示词：各预加载子Agent 的完整系统提示词拼接 + 各自的项目注记（项目内置/预置项目/受限模式/AGENTS.md）；
    // 每个子Agent 前加职责分隔头（名称 + 能力描述），明确各段提示词对应的工具命名空间与职责域，多 Agent 预加载时不混淆
    const systemParts: string[] = []
    const mergedPresets: PresetProject[] = []
    const seen = new Set<string>()
    let baseProjectRoot: string | undefined
    for (const def of defs) {
      // 项目内置（特定项目绑定）：会话环境变量 {AGENT_NAME_UPPER}_PROJECT
      // （如 CODE_PROJECT / SELF_OPTIMIZE_PROJECT）指定子Agent 的项目根
      // —— 工作目录与路径解析以项目根为基准，系统提示词注入项目路径
      const projectRoot = this.resolveSubAgentProject(user, env, def.name)
      baseProjectRoot ??= projectRoot
      const presetProjects = this.presetProjectsFor(user, env, def.name)
      const workNote = projectRoot ? `\n项目根: ${projectRoot}` : `\n工作目录: ${sessionPath(this.opts.config.gebaiHome, user, sessionId)}/tmp`
      // 预置项目清单注入子Agent 系统提示词：文件工具可用 project 参数（项目名）切换目标项目
      const presetNote = this.buildPresetNote(def.name, projectRoot, presetProjects)
      // 受限模式（CODE_RESTRICT_PROJECTS=true）：仅允许操作预配置项目（CODE_PROJECTS 清单），禁止自由路径
      const restrictNote = env.CODE_RESTRICT_PROJECTS === "true"
        ? `\n受限模式（CODE_RESTRICT_PROJECTS=true）：仅允许操作预配置项目（${def.name} 的 ${def.name.toUpperCase()}_PROJECTS 清单，或 ${def.name.toUpperCase()}_PROJECT 绑定根），文件工具必须携带 project 参数，自由路径（path）不可用。`
        : ""
      // 动态环境注记（项目根/预置项目清单/受限模式）置于职责分隔头之后、静态提示词之前——
      // 配置信息前置，模型开工先读环境（目标项目与 project 参数取值），再读工作流
      systemParts.push(`### ${def.name}（${def.description}）\n${workNote}${presetNote}${restrictNote}${def.systemPrompt}${await this.loadProjectAgentsMd(projectRoot)}`)
      // 预置项目全量合并（同名去重）：多 Agent 预加载时 project 参数路由均可用
      for (const p of presetProjects) {
        if (!seen.has(p.name)) {
          seen.add(p.name)
          mergedPresets.push(p)
        }
      }
    }
    const messages: MessageLike[] = [
      {
        role: "system",
        content: `你正在一个临时新会话中执行任务（与主会话隔离，执行过程不进入主上下文）。已预加载子Agent: ${agents.join(", ")}，其完整系统提示词如下。\n可预判的多步固定流程优先用 flow 数据流编排一次执行（引用映射/分支/循环，编排前可用 tool_schemas 批量查询工具输出结构，语法详见 flow 工具描述），或编写脚本（sh/py）一次执行，避免大量单步工具调用浪费往返与词元。\n\n${systemParts.join("\n\n")}`,
      },
      { role: "user", content: input },
    ]

    // 不设超时：执行过程新会话回复实时推送到前端（进度可见），无进度空转问题已由可见性解决；
    // 中止仅依赖父任务取消信号（用户停止/任务取消传播），工具级超时兜底（TOOL_TIMEOUT_MS）仍在
    const runId = crypto.randomUUID()
    // 新会话 run 完整存档（DESIGN「新会话执行存档」）：执行过程全部内容收集进 archive，
    // 由 agent_run 工具作为调用记录的扩展字段落盘（不逐条写会话消息）——仅存档与前端回放，
    // loadHistory 不受影响（不进入主 LLM 上下文）
    const archive: SessionRunArchive = { runId, agents, input, output: "", messages: [{ role: "user", content: input }] }
    this.publish(sessionId, "event.session.start", { runId, agents, input, depth, sessionId })
    try {
      const output = await this.runNewSessionLoop(sessionId, user, env, agents, input, messages, reg, signal, depth, archive, { workdir: baseProjectRoot ?? undefined, resolveBase: baseProjectRoot, projects: mergedPresets })
      archive.output = output
      return { output, archive }
    } catch (err) {
      // 异常/取消收尾：推送 done 事件让前端折叠容器（存档不落盘，失败过程不回放）
      this.publish(sessionId, "event.session.done", { runId, agents, output: "", error: String((err as Error).message || err), sessionId })
      throw err
    }
  }

  /** 解析子Agent 项目根（{AGENT_NAME_UPPER}_PROJECT 环境变量）：沙箱模式限定用户数据目录内，本地模式放开。 */
  private resolveSubAgentProject(user: string, env: Record<string, string>, agentName: string): string | undefined {
    const key = `${agentName.toUpperCase().replace(/-/g, "_")}_PROJECT`
    const raw = env[key]
    if (!raw) return undefined
    try {
      return this.resolveAgentProjectRoot(user, raw)
    } catch {
      return undefined // 沙箱拒绝越界/绝对路径绑定：回退工作目录
    }
  }

  /** 项目约定注入：项目根存在 AGENTS.md（兼容 AGENT.md 命名）时读取并注入系统提示词；不存在/不可读静默跳过，超长截断防上下文膨胀。 */
  private async loadProjectAgentsMd(root: string | undefined): Promise<string> {
    if (!root) return ""
    const { readFile } = await import("node:fs/promises")
    for (const file of ["AGENTS.md", "AGENT.md"]) {
      try {
        const content = (await readFile(join(root, file), "utf8")).trim()
        if (!content) continue
        const capped = content.length > 8000 ? `${content.slice(0, 8000)}\n…（${file} 过长已截断）` : content
        return `\n\n项目约定（${file}，编码/维护必须遵守）:\n${capped}`
      } catch {
        /* 文件不存在或不可读：跳过 */
      }
    }
    return ""
  }

  /** 解析子Agent 项目根绝对路径：沙箱约束用户限定用户数据目录内（防越界），豁免/本地模式放开。 */
  private resolveAgentProjectRoot(user: string, raw: string): string {
    if (this.opts.sandbox.enforcedFor(user)) {
      return resolveInSandbox(join(this.opts.config.gebaiHome, "users", user), raw)
    }
    return isAbsolute(raw) ? raw : resolve(process.cwd(), raw)
  }

  /** 解析子Agent 预置项目注册表（{AGENT_NAME_UPPER}_PROJECTS 环境变量，JSON 数组）：非法 JSON 静默忽略；同名去重（首个生效）。 */
  private presetProjectsFor(user: string, env: Record<string, string>, agentName: string): PresetProject[] {
    const key = `${agentName.toUpperCase().replace(/-/g, "_")}_PROJECTS`
    const raw = env[key]
    if (!raw) return []
    let list: unknown
    try {
      list = JSON.parse(raw)
    } catch {
      return []
    }
    if (!Array.isArray(list)) return []
    const out: PresetProject[] = []
    const seen = new Set<string>()
    for (const item of list) {
      if (!item || typeof item !== "object") continue
      const p = item as Record<string, unknown>
      const name = typeof p.name === "string" ? p.name.trim() : ""
      const path = typeof p.path === "string" ? p.path.trim() : ""
      if (!name || !path || seen.has(name)) continue
      let root: string
      try {
        root = this.resolveAgentProjectRoot(user, path)
      } catch {
        continue // 沙箱拒绝越界/绝对路径项目：静默跳过该条目（与非法 JSON 忽略一致）
      }
      seen.add(name)
      out.push({ name, path: root, description: typeof p.description === "string" && p.description.trim() ? p.description.trim() : undefined })
    }
    return out
  }

  private async runNewSessionLoop(
    sessionId: string,
    user: string,
    env: Record<string, string>,
    agents: string[],
    input: string,
    messages: MessageLike[],
    reg: ToolRegistry,
    signal: AbortSignal,
    depth: number,
    archive: SessionRunArchive,
    ctxOpts?: { workdir?: string; resolveBase?: string; projects?: PresetProject[] },
  ): Promise<string> {
    let rounds = 0
    let lastText = ""
    void agents
    void input
    // 重复检测（与主循环一致，DESIGN「重复检测」）：相同工具+参数重复调用中断并注入引导提示，超限终止
    const recentCalls: string[] = []
    let repeatStalls = 0
    // 取消中止判定：新会话执行不设独立超时，中止仅由父任务取消传播（报「已取消」）
    const activeSignal = signal
    const abortReason = () => (activeSignal.reason instanceof Error ? activeSignal.reason.message : activeSignal.reason ? String(activeSignal.reason) : "cancelled")
    const ctx = this.buildContext(sessionId, user, env, signal, { ...ctxOpts, registry: reg, role: this.tasks.get(sessionId)?.role, writeGuard: this.defsWriteGuard(agents, env) }, depth)
    // 任务级主模型：与主循环一致，env 配置 GEBAI_LLM_* 时重建 Provider（无覆盖沿用启动实例）
    const taskProvider = this.opts.resolveProvider?.(env) ?? this.opts.provider
    // 任务级额外模型接口参数（浏览器本地注入 GEBAI_LLM_EXTRA_PARAMS）：非法 JSON 忽略
    const extraParams = parseExtraParamsSafe(env.GEBAI_LLM_EXTRA_PARAMS)
    // 存档收集（替代原逐条落盘）：执行过程消息追加进 archive.messages，最终由 agent_run 扩展字段落盘
    const pushArchive = (entry: SessionRunEntry) => {
      archive.messages.push(entry)
      return Promise.resolve()
    }

    while (rounds < MAX_TOOL_ROUNDS) {
      if (activeSignal.aborted) throw new Error(abortReason())
      // 每轮重推 start 事件（同 runId 幂等，前端容器已存在时忽略）：前端容器随消息重载丢失
      // （切走会话/断线重连）后，新一轮 delta 前可据此重建折叠容器
      this.publish(sessionId, "event.session.start", { runId: archive.runId, agents, input, depth, sessionId })
      const assistantMsgId = crypto.randomUUID()
      // 执行过程：新会话的模型回复文本/推理实时推送到前端（与主循环同流显示，带 session 标记）
      let reasoningAcc = ""
      const { text, toolCalls } = await this.callModel(taskProvider, messages, reg.schemas().filter((s) => !this.isToolDisabled(sessionId, s.name, reg.resolve(s.name)?.tool)), activeSignal, (chunk) => {
        if (chunk.type === "text") {
          // session 标记：区别于主循环推送，渠道层可据此识别「新会话执行过程」事件；
          // 仅最终响应（final_only）不推送新会话过程文本
          if (this.tasks.get(sessionId)?.outputMode === "streaming") this.publish(sessionId, "event.message.delta", { text: chunk.text, messageId: assistantMsgId, session: true, sessionRunId: archive.runId, sessionId })
        } else if (chunk.type === "reasoning" && chunk.text?.trim()) {
          reasoningAcc += chunk.text
          if (this.tasks.get(sessionId)?.outputMode === "streaming") this.publish(sessionId, "event.message.reasoning", { text: chunk.text, session: true, sessionRunId: archive.runId, messageId: assistantMsgId, sessionId })
        }
      }, extraParams)
      lastText = text
      if (!toolCalls.length) {
        this.publish(sessionId, "event.message.done", { text, messageId: assistantMsgId, session: true, sessionRunId: archive.runId, sessionId })
        // 新会话 run 收尾：最终回复入存档（折叠容器回放展示）
        if (text) {
          await pushArchive({ role: "assistant", content: text, reasoning: reasoningAcc.trim() ? reasoningAcc.trim() : undefined })
        }
        this.publish(sessionId, "event.session.done", { runId: archive.runId, agents, output: text, sessionId })
        return text
      }

      await pushArchive({ role: "assistant", content: text, reasoning: reasoningAcc.trim() ? reasoningAcc.trim() : undefined, toolCalls: toolCalls.map((tc) => ({ id: tc.id, name: tc.name, arguments: tc.arguments })) })
      messages.push({ role: "assistant", content: text, toolCalls })
      let stopped = false
      for (const tc of toolCalls) {
        if (activeSignal.aborted) throw new Error(abortReason())
        if (stopped) {
          const abortNote = "任务已中止：模型持续重复相同工具调用。"
          await pushArchive({ role: "tool", content: abortNote, toolCallId: tc.id, name: tc.name })
          messages.push({ role: "tool", content: abortNote, toolCallId: tc.id, name: tc.name })
          continue
        }
        if (this.repeatedCall(recentCalls, tc.name, tc.arguments)) {
          repeatStalls++
          if (repeatStalls > MAX_REPEAT_STALLS) stopped = true
          const note = `已中断重复的工具调用 ${tc.name}：参数与之前完全相同，重复执行只会得到相同结果。请分析原因、改用其他方法，或直接给出最终回答，不要重复相同操作。`
          await pushArchive({ role: "tool", content: note, toolCallId: tc.id, name: tc.name })
          messages.push({ role: "tool", content: note, toolCallId: tc.id, name: tc.name })
          continue
        }
        // 工具参数不是合法 JSON：与主循环一致，回传原始片段让模型修正（不执行）
        if (tc.argsError) {
          const errMsg = `工具参数 JSON 解析失败：模型输出的参数不是合法 JSON。原始片段: ${tc.argsError}。请重新调用 ${tc.name} 并输出合法的 JSON 参数。`
          await pushArchive({ role: "tool", content: errMsg, toolCallId: tc.id, name: tc.name })
          messages.push({ role: "tool", content: errMsg, toolCallId: tc.id, name: tc.name })
          continue
        }
        const rt = reg.resolve(tc.name)
        if (!rt) {
          const errMsg = `未知工具: ${tc.name}`
          await pushArchive({ role: "tool", content: errMsg, toolCallId: tc.id, name: tc.name })
          messages.push({ role: "tool", content: errMsg, toolCallId: tc.id, name: tc.name })
          continue
        }
        if (this.isToolDisabled(sessionId, rt.name, rt.tool)) {
          const disabledMsg = `工具 ${rt.name} 在当前通道不可用（该工具需要前端页面配合），请改用其他方式。`
          await pushArchive({ role: "tool", content: disabledMsg, toolCallId: tc.id, name: tc.name })
          messages.push({ role: "tool", content: disabledMsg, toolCallId: tc.id, name: tc.name })
          continue
        }
        if (this.isRiskyInSafeMode(rt.name)) {
          // 安全模式：子Agent 内风险工具同样拦截（与主循环一致），返回限制信息供子Agent 调整方案
          const safeMsg = safeModeRestrictionMsg(rt.name)
          await pushArchive({ role: "tool", content: safeMsg, toolCallId: tc.id, name: tc.name })
          messages.push({ role: "tool", content: safeMsg, toolCallId: tc.id, name: tc.name })
          continue
        }
        const requiresByArgs = await toolRequiresApproval(rt.tool, tc.arguments, ctx)
        const approvalSkipped = await this.isApprovalSkipped(sessionId, user, env)
        // 与主循环一致：服务模式 + 无交互通道按默认审批姿态拒绝（approval:false 免审标记剥离后解析，防绕过）
        if (
          this.opts.authMode === "server" && this.tasks.get(sessionId)?.interactionMode === "none" && !approvalSkipped &&
          (requiresByArgs || (await toolRequiresApproval(rt.tool, stripApprovalFlags(tc.arguments) as Record<string, unknown>, ctx)))
        ) {
          const denied = this.noInteractionDenied(rt.name)
          await pushArchive({ role: "tool", content: denied, toolCallId: tc.id, name: tc.name })
          messages.push({ role: "tool", content: denied, toolCallId: tc.id, name: tc.name })
          continue
        }
        if (requiresByArgs && !approvalSkipped) {
          const retries = this.tasks.get(sessionId)?.retries.get(tc.id) ?? 0
          this.publish(sessionId, "event.tool.call", { name: tc.name, arguments: tc.arguments, toolCallId: tc.id, requiresApproval: true, session: true, sessionRunId: archive.runId })
          this.publish(sessionId, "event.approval.request", { toolCallId: tc.id, tool: tc.name, retries, session: true, sessionRunId: archive.runId, sessionId })
          const verdict = await this.waitApproval(sessionId, tc.id, rt.name, activeSignal)
          // 新会话内：取消/超时信号解开等待后立即中止（存档随 run 整体，取消由上层按已取消结果收尾）
          if (activeSignal.aborted) throw new Error(abortReason())
          if (verdict !== "approved") {
            this.tasks.get(sessionId)?.retries.set(tc.id, retries + 1)
            const denied = verdict === "timeout" ? `工具调用 ${rt.name} 审批等待超时，已跳过。` : `工具调用 ${rt.name} 已被用户拒绝。请调整方案。`
            await pushArchive({ role: "tool", content: denied, toolCallId: tc.id, name: tc.name })
            messages.push({ role: "tool", content: denied, toolCallId: tc.id, name: tc.name })
            continue
          }
        } else {
          this.publish(sessionId, "event.tool.call", { name: tc.name, arguments: tc.arguments, toolCallId: tc.id, session: true, sessionRunId: archive.runId })
        }
        // 取消/超时统一收口：父任务停止均中断执行（脚本进程同步被杀），超时作为结果返回模型
        this.publish(sessionId, "event.tool.result.start", { name: tc.name, toolCallId: tc.id, session: true, sessionRunId: archive.runId })
        const result = await this.runToolInterruptible(rt.tool, tc.arguments, ctx, activeSignal, rt.name)
        // 兜底截断（与主循环一致）：超长工具输出统一截断，防存档膨胀；结构化 data 与存档扩展字段原样保留
        const safe = !result.truncated && result.output.length > TRUNCATE_THRESHOLD
          ? { ...(await truncate(result.output, rt.name, ctx)), blocks: result.blocks, data: result.data, sessionRun: result.sessionRun }
          : result
        // 嵌套 agent_run：新会话的存档递归挂到工具消息上（历史回放嵌套容器）；不进主上下文，
        // provider 序列化只取已知字段，额外字段不会泄漏进 LLM 请求
        const nested = safe.sessionRun ? { sessionRun: safe.sessionRun } : {}
        await pushArchive({ role: "tool", content: safe.output, blocks: safe.blocks, toolCallId: tc.id, name: tc.name, arguments: tc.arguments, ...nested })
        messages.push({ role: "tool", content: safe.output, toolCallId: tc.id, name: tc.name, ...nested })
        this.publish(sessionId, "event.tool.result", {
          name: tc.name,
          toolCallId: tc.id,
          truncated: !!safe.truncated,
          filePath: safe.filePath,
          output: safe.output,
          blocks: safe.blocks,
          session: true,
          sessionRunId: archive.runId,
          sessionId,
        })
      }
      rounds++
      if (stopped) break
    }
    // 循环上限退出（重复调用风暴终止）：同样推送 done 事件折叠容器
    this.publish(sessionId, "event.session.done", { runId: archive.runId, agents, output: lastText, sessionId })
    return lastText
  }

  private waitApproval(sessionId: string, toolCallId: string, _tool: string, signal?: AbortSignal): Promise<ApprovalVerdict> {
    const task = this.tasks.get(sessionId)!
    const pre = task.pendingDecisions.get(toolCallId)
    if (pre !== undefined) {
      task.pendingDecisions.delete(toolCallId)
      return Promise.resolve(pre ? "approved" : "rejected")
    }
    // 取消/超时信号：abort 立即以「timeout」解开等待（否则 await 永久挂起，任务收尾不完成；
    // 取消路径由消费处 cancelled 标记短路，不落盘）
    if (signal?.aborted) return Promise.resolve("timeout")
    return new Promise<ApprovalVerdict>((resolve) => {
      let timer: ReturnType<typeof setTimeout>
      let onAbort: () => void
      const done = (v: ApprovalVerdict) => {
        clearTimeout(timer)
        signal?.removeEventListener("abort", onAbort)
        task.approvals.delete(toolCallId)
        resolve(v)
      }
      onAbort = () => done("timeout")
      timer = setTimeout(() => done("timeout"), APPROVAL_TIMEOUT)
      signal?.addEventListener("abort", onAbort, { once: true })
      task.approvals.set(toolCallId, { sessionId, toolCallId, resolve: done, timer })
    })
  }

  private async saveAttachments(sessionId: string, user: string, attachments: AttachmentInput[]): Promise<AttachmentRef[] | undefined> {
    if (!attachments.length) return undefined
    const { writeFile, mkdir } = await import("node:fs/promises")
    const tmp = this.opts.store.getTmpDir(sessionId, user)
    const refs: Array<{ path: string; mime: string; name: string; size: number }> = []
    for (const a of attachments) {
      // 名称消毒：仅取 basename，拒绝路径分隔符与穿越（防止 ../ 逃逸会话目录）
      const name = basenameName(a.name)
      if (!name) throw new Error(`附件名无效: ${a.name}`)
      const path = `${tmp}/${name}`
      await mkdir(tmp, { recursive: true })
      if (a.data) {
        await writeFile(path, a.data)
      } else if (a.path) {
        // 来源路径统一按沙箱规则基于会话根解析：沙箱启用时限定会话目录内（防任意文件读取）；
        // 本地模式基于会话根解析（绝对路径放行）——修复相对进程 CWD 解析导致附件读取失败的缺陷
        const src = this.opts.sandbox.resolvePath(user, sessionId, a.path)
        const buf = await Bun.file(src).arrayBuffer()
        await writeFile(path, new Uint8Array(buf))
      }
      const size = (await Bun.file(path).size) ?? 0
      // 存储逻辑路径（相对会话根，如 tmp/foo.png，SDK 契约）：模型/工具/前端统一按此解析
      refs.push({ path: `tmp/${name}`, mime: a.mime || "application/octet-stream", name, size })
    }
    return refs
  }
}

/** 剥离 assistant 消息 content 中的 `<think>…</think>` 推理块：兼容旧版数据（推理曾内嵌 content，
 *  新版推理为独立字段 reasoning，content 已是纯正文）；历史回放给 LLM 时防推理泄漏进上下文。 */
export function stripThinkTags(content: string): string {
  return content.replace(/<think>[\s\S]*?<\/think>/g, "").trim()
}
