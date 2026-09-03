import type { AttachmentInput, AttachmentRef, DiagramFormat, Message, MessageLike, SessionRunArchive, SessionRunEntry } from "@gebai/sdk"
import { LLMConfigError, parseExtraParams, salvageWriteArgs, type LLMChunk, type LLMProvider, type LLMUsage } from "../llm/llm"
import { VISION_MAX_IMAGE_BYTES, VISION_MIME_SET } from "../tools/vision"
import type { ToolRegistry } from "../base/registry"
import type { SessionStore } from "../session/store"
import { estimateCtxTokens, estimateCharsTokens } from "../session/store"
import type { EnvManager } from "../session/env"
import type { Sandbox } from "../security/sandbox"
import type { EventBus } from "../base/event-bus"
import type { ServerConfig } from "../base/config"
import type { SubAgentManager } from "../agents/subagents"
import type { ToolContext, ToolResult, Tool, PresetProject, ChoiceResult, ChoiceOption, ChoicePlan, InteractionMode, OutputMode, SessionData, DynamicToolDef, SubAgentDef, ToolResultImage } from "../base/types"
import { ToolRegistry as BaseToolRegistry } from "../base/registry"
import { normalizeToolArgs, tolerantToolName } from "../base/tool-args"
import { agentListTool, agentLoadTool, agentRunTool, bgTaskTool, branchSyncTool, createGlobalTools, isGlobalToolExcluded, toolSchemasTool, PAGE_CAPTURE_HTML_LIMIT, truncate, TRUNCATE_THRESHOLD, spillLongUserInput, walkDirFiles } from "../tools"
import { makeVisionTool, getVisionProvider } from "../tools/vision"
import { jsTool, makeDynamicTool } from "../exec/js-tool"
import { ShTaskRunner } from "../exec/sh-tasks"
import { SessionRunRegistry, type SessionRunHandle } from "../session/session-runs"
import { BranchRunRegistry, type BranchRunHandle, type BranchSpec, BRANCH_MERGE_MAX_CHARS, BRANCH_MERGE_SUMMARY_SKIP_CHARS, branchNoticeHead } from "../session/branch-runs"
import { RESERVED_PROJECT_TMP } from "../tools/projects"
import { basenameName, resolveInSandbox, sessionPath } from "../base/paths"
import { dirname, isAbsolute, join, resolve } from "node:path"
import { isToolBlockedInSafeMode, safeModeRestrictionMsg, stripApprovalFlags } from "../security/safety"
import { createHash } from "node:crypto"
import { ContextCompressor } from "./compressor"
import {
  APPROVAL_TIMEOUT,
  CAPTURE_TIMEOUT,
  type ApprovalVerdict,
  decideApproval,
  decideCaptureResult,
  decideChoice,
  decideDrawResult,
  decideEnvResult,
  waitForCapture,
  waitForChoice,
  waitForDraw,
  waitForEnv,
  type CaptureResult,
  type DrawResult,
  type StreamSnapshot,
  type TaskState,
} from "./interactions"
import {
  agentDescription as agentDescriptionFn,
  allPresetProjects as allPresetProjectsFn,
  buildAgentSection as buildAgentSectionFn,
  buildPresetNote as buildPresetNoteFn,
  buildSystemPrompt as buildSystemPromptFn,
  type PromptDeps,
} from "./prompt"

/** 文件内容指纹（fileGuard 防陈旧覆盖）：BOM 无关（去 \uFEFF 前缀后哈希——read 登记去 BOM 正文、
 *  write 比对含 BOM 原文，边界归一后一致），sha256 十六进制前 16 位（非加密用途，防撞足够）。 */
function fingerprint(content: string): string {
  const body = content.startsWith("\uFEFF") ? content.slice(1) : content
  return createHash("sha256").update(body).digest("hex").slice(0, 16)
}

const MAX_TOOL_ROUNDS = 200
/** 待办续做：主循环完成后仍有未完成待办（pending/in_progress）时，追加提醒消息继续完成的轮次上限。 */
const MAX_TODO_CONTINUE = 3
/** 收尾验证提醒轮次上限：改了代码文件但全程未跑测试/检查的任务，结束时最多注入一次提醒（防反复打扰）。 */
const MAX_VERIFY_NUDGE = 1
/** 收尾验证提醒——代码文件判定（write/edit/patch 命中这些扩展名的 path 才计入；md/txt 等文档不触发）。 */
const VERIFY_CODE_FILE_RE = /\.(ts|tsx|js|jsx|mjs|cjs|py|go|rs|java|kt|kts|c|h|cpp|hpp|cc|cs|rb|php|swift|scala|vue|svelte|dart|lua|sh|bash|sql)$/i
/** 收尾验证提醒——测试/检查类命令判定（sh/py 的 command 文本匹配；宽匹配宁漏勿紧：误判已验证只少一次提醒）。 */
const VERIFY_CMD_RE = /\b(bun test|bun run test|npm test|npm run test|yarn test|pnpm test|pytest|vitest|jest|go test|cargo test|deno test|gradle test|gradlew\s+\S*test|mvn test|tsc|typecheck|type-check|eslint|biome check|ruff|mypy|flake8|clang-tidy|lint)\b/i
/** 收尾验证提醒——写类工具的拒绝形态（守卫/安全模式拦截未落盘，不计入修改文件）。 */
const MOD_REJECTED_RE = /^(write|edit|patch) 拒绝|安全模式|受限模式/
/** 重复检测滚动窗口：记录最近 N 次工具调用签名（工具名+参数），窗口内相同签名出现 ≥MAX_REPEAT_HITS 次判定为无效重复。 */
const MAX_REPEAT_WINDOW = 8
/** 重复检测命中阈值：相同签名（工具+参数）在窗口内出现第 MAX_REPEAT_HITS 次时中断该次执行并注入引导提示。 */
const MAX_REPEAT_HITS = 3
/** 重复中断上限：中断次数超过该值即终止工具循环（模型持续重复时防止无效空转）。 */
const MAX_REPEAT_STALLS = 2
/** 同批工具并行执行上限（DESIGN「同批工具并行执行」）：单次模型响应返回的多个工具调用并行执行，
 *  此为并发护栏（进程/文件句柄等资源保护），超出按调用顺序排队；需严格串行的操作由模型用 js 脚本编排。 */
const MAX_PARALLEL_TOOLS = 8
/** 工具执行超时兜底（毫秒）：脚本类工具由 sandbox 自身 timeoutMs（默认 5 分钟）先杀进程并返回超时结果；
 * 此兜底覆盖不响应超时的工具（如网络请求挂起）。超时不结束任务——结果作为「执行超时」返回给模型继续。 */
const TOOL_TIMEOUT_MS = 9 * 60 * 1000
/** 长工具执行心跳间隔（毫秒）：执行期间定期发布 event.tool.alive，供前端空闲看门狗（60s 无数据取消任务）刷新活跃——
 * 阻塞类工具（sh/py 跑构建/测试等）执行期间无其他事件，不心跳会被前端误判挂起取消（工具自身超时未及生效）。 */
const TOOL_HEARTBEAT_MS = 25_000
const SUBAGENT_DEPTH = 3
/** 模型接口异常/空响应的重试次数与退避基数（指数退避，DESIGN「常量参考」）。 */
const LLM_RETRY_COUNT = 2
const LLM_RETRY_BACKOFF_MS = 800
/** 上下文压缩阈值：窗口的 80%（DESIGN「常量参考」）。 */
const COMPACT_RATIO = 0.8
/** 附件图片内联上限（与 vision 工具一致）：超出不内联，降级为文本说明。 */
const ATTACHMENT_INLINE_LIMIT = VISION_MAX_IMAGE_BYTES
/** 历史图片内联窗口：仅最近 N 条含图片的用户消息内联进上下文，更早的降级为文本说明
 *  （图片永久占据上下文且不受压缩保护，长会话会被历史图片占死窗口）。 */
const INLINE_IMAGE_RECENT = 3
/** 单次 agent_run（新会话执行）可预加载的子Agent 数量上限（防异常/恶意调用拼装超大提示词）。 */
const MAX_AGENTS_PER_RUN = 5
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

/** 工具结果图片块降级文本（read 等读取的图片未内联时：接口拒绝图片/溢出护栏降级）。 */
function toolImageNote(b: { path?: unknown; name?: unknown; mime?: unknown }): string {
  return `[图片文件 ${String(b.name ?? b.path ?? "")}（${String(b.mime ?? "")}）未内联：模型接口不支持图片内容。可用 vision 工具查看（image 参数传 ${String(b.path ?? "")}）]`
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
 *  （js 等编排工具据此实现「内部调用覆盖审批」）；函数异常按需审批处理（fail-safe）。 */
async function toolRequiresApproval(tool: Tool, args: Record<string, unknown>, ctx: ToolContext): Promise<boolean> {
  const ra = tool.requiresApproval
  if (typeof ra !== "function") return !!ra
  try {
    return !!(await ra(args ?? {}, ctx))
  } catch {
    return true
  }
}

/** 必填参数缺失校验（模型漏传参数的防御，两循环共用）：schema `required` 声明的键在调用参数中
 *  缺失/null 时返回缺失清单——工具不执行、回传明确错误引导模型补参重试。缺失参数若直接进工具会被
 *  `String(undefined)` 成字面量 "undefined" 落进路径解析，报出与真实原因无关的 ENOENT（如
 *  edit 漏传 path → `tmp\undefined`），模型无法从报错定位到「少传了参数」。 */
function missingRequiredArgs(tool: Tool, args: Record<string, unknown>): string[] {
  const required = (tool.parameters as { required?: string[] }).required ?? []
  return required.filter((k) => args[k] === undefined || args[k] === null)
}

/** 必填参数缺失的错误文案（作为工具结果回传，模型下一轮自纠）。 */
function missingArgsMsg(name: string, missing: string[]): string {
  return `工具 ${name} 缺少必填参数: ${missing.join("、")}——本次调用未执行，请补齐参数后重试（各参数含义见工具描述）。`
}

/** 工具调用容错（模型误差自适应，两循环共用）：工具名与参数键归一到蛇形契约——
 *  tolerantToolName 把分隔符/驼峰偏差（agent.run/agentRun）归一蛇形；参数键按已解析工具的 schema
 *  做指纹归一（oldString→old_string）。在 assistant(toolCalls) 落盘/入上下文前执行，历史记录、事件
 *  推送与实际执行同用规范名；未知名（无 schema 可依）只归一名、参数原样，门控阶段照常报未知工具。 */
function normalizeToolCalls(registry: Pick<ToolRegistry, "resolve">, toolCalls: Array<{ name: string; arguments: Record<string, unknown> }>): void {
  for (const tc of toolCalls) {
    const fixed = tolerantToolName(tc.name)
    if (fixed !== tc.name) tc.name = fixed
    const rt = registry.resolve(tc.name)
    if (rt && tc.arguments && typeof tc.arguments === "object") tc.arguments = normalizeToolArgs(rt.tool, tc.arguments)
  }
}

/** 工具调用签名（重复检测滚动窗口的记录单元）：工具名 + 参数 JSON。 */
function toolCallSignature(name: string, toolArguments: Record<string, unknown>): string {
  return `${name}:${JSON.stringify(toolArguments ?? {})}`
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


export interface AgentEngineOptions {
  provider: LLMProvider
  /** 任务级主模型 Provider 解析：env 配置 GEBAI_LLM_* 时返回重建的 Provider（覆盖启动配置）；
   *  无覆盖返回 undefined（调用方沿用 opts.provider 实例）。 */
  resolveProvider?: (env: Record<string, string>) => LLMProvider | undefined
  /** 分支运行模型路由解析（DESIGN「会话分支运行与合并」多路接口）：按名（GEBAI_LLM_ROUTES 路由名或
   *  字面模型名）返回独立 Provider；未注入或名字为空返回 undefined（分支沿用任务级 Provider）。 */
  resolveModelProvider?: (env: Record<string, string>, name: string) => LLMProvider | undefined
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
  cron?: import("../schedule/cron").CronManager
  /** page_capture 等待前端捕获回传的超时（毫秒，默认 30 秒；测试可注入短超时）。 */
  captureTimeoutMs?: number
  /** 工具执行超时兜底（毫秒，默认 9 分钟；测试可注入短超时验证超时返回给模型）。 */
  toolTimeoutMs?: number
  /** 长工具执行心跳间隔（毫秒，默认 25s；测试可注入短间隔验证心跳事件发布）。 */
  heartbeatMs?: number
  /** LLM 流式调用读空闲超时（毫秒，默认 120s；测试可注入短超时验证假死中止）。 */
  llmIdleTimeoutMs?: number
  /** 认证模式（"server"=服务模式多用户隔离）。无交互通道（REST）的审批策略按此分级：本地模式保持「自动通过」，
   * 服务模式下需审批工具一律拒绝执行（防普通用户经 REST 免审批执行 sh/py 等敏感工具）。 */
  authMode?: "local" | "server"
}

export class AgentEngine {
  private tasks = new Map<string, TaskState>()

  /** 任务级「收尾验证提醒」数据（DESIGN「收尾验证提醒」）：sessionId → { 修改的代码文件, 是否已运行测试/检查类命令 }。
   *  run() 开始置位、runToolInterruptible 收集（write/edit/patch 成功改代码文件、sh/py 执行测试/检查命令）、
   *  任务结束清理——与待办续做同机制的兜底：模型改了代码却全程没跑验证时注入一次提醒。 */
  private taskMods = new Map<string, { files: Set<string>; verified: boolean }>()

  /** 会话级已读文件追踪（fileGuard 防误覆盖/防陈旧覆盖，DESIGN「write 防误覆盖守卫」）：sessionId → (已读绝对路径 →
   *  读取/写入时内容指纹)。read/edit/patch/write 成功后登记（含指纹），write/edit/patch 写前按「未读 → 防盲写、
   *  指纹漂移 → 防陈旧覆盖」两档拦截（并行分支/主线/脚本命令/外部编辑改动均会漂移指纹）；
   *  分支运行 fork 独立快照（runBranch 拷贝本表——分支已读基线 = fork 点主线可见内容，互不串扰）；
   *  会话删除经 forgetSession 释放（进程内无界增长防护）。 */
  private readFiles = new Map<string, Map<string, string | null>>()
  /** 单会话已读登记上限（防长会话无界增长；超出整表重置——守卫降级为「需重读」，保护语义不破坏）。 */
  private static readonly READ_TRACK_CAP = 2000

  /** 会话级运行时定义工具（js defineTool，DESIGN「js 脚本工具」运行时工具定义）：sessionId → (工具名 → {Tool, 定义})。
   *  定义随会话 chat.json 落盘（SessionData.dynamicTools）、run() 水合恢复，随会话删除释放；
   *  模型可见性经 sessionRegistry 覆盖层并入；执行用 Tool、持久化用 def（execute 源码序列化）。 */
  private dynamicTools = new Map<string, Map<string, { tool: Tool; def: DynamicToolDef }>>()
  /** 单会话动态工具上限（防注册风暴；重启水合同限）。 */
  private static readonly DYNAMIC_TOOLS_CAP = 50

  /** 会话级 sh 异步后台任务服务（会话 tmp/sh-tasks/ 落盘，跨调用/跨重启可见）：user:sessionId → runner。 */
  private shTaskServices = new Map<string, ShTaskRunner>()

  /** agent_run 异步后台运行句柄（进程内，引擎级共享：runId → handle；DESIGN「新会话执行的异步运行」）。
   *  运行存活与本进程绑定（重启即中断，不落盘恢复）；SessionRunRegistry 为按会话过滤的薄视图。 */
  private sessionRunStore = new Map<string, SessionRunHandle>()

  /** 会话分支运行句柄（进程内，引擎级共享：branchId → handle；DESIGN「会话分支运行与合并」）。
   *  同 session-runs 哲学：随进程存活、重启即中断；BranchRunRegistry 为按会话过滤的薄视图。 */
  private branchRunStore = new Map<string, BranchRunHandle>()

  /** 分支合并队列（DESIGN「会话分支运行与合并」）：sessionId → 待合入主上下文的分支报告消息。
   *  分支完成时入队；runLoop 在工具批处理边界排空（tool 结果之后追加，保持 tool_calls 配对完整，
   *  主线下轮模型调用即见）；任务结束（run finally）冲刷落盘（异步分支结果不因任务收尾丢失）。 */
  private branchMerges = new Map<string, Message[]>()

  /** 上下文压缩器（压缩/溢出恢复，自本类拆分；见 compressor.ts）。 */
  private compressor: ContextCompressor
  /** 系统提示词构建依赖包（项目解析等引擎方法注入；见 prompt.ts）。 */
  private promptDeps: PromptDeps
  /** 事件发布适配（interactions 自由函数用）。 */
  private publishFn = (sessionId: string, type: string, payload: Record<string, unknown>) => this.publish(sessionId, type, payload)

  constructor(private opts: AgentEngineOptions) {
    this.compressor = new ContextCompressor({
      store: opts.store,
      env: opts.env,
      getDefaultProvider: () => opts.provider,
      resolveProvider: opts.resolveProvider ? (env) => opts.resolveProvider?.(env) : undefined,
      idleTimeoutMs: opts.llmIdleTimeoutMs ?? LLM_IDLE_TIMEOUT_MS,
      isTaskRunning: (id) => this.tasks.has(id),
      taskSignal: (id) => this.tasks.get(id)?.controller.signal,
      publish: this.publishFn,
      loadHistory: (id, user, inlineMultimodal) => this.loadHistory(id, user, inlineMultimodal),
      callModel: (provider, messages, schemas, signal, onChunk, extraParams, sessionId) =>
        this.callModel(provider, messages, schemas, signal, onChunk, extraParams, sessionId),
    })
    this.promptDeps = {
      config: opts.config,
      sandbox: opts.sandbox,
      subAgents: opts.subAgents,
      channelNote: (id) => this.tasks.get(id)?.channelNote,
      resolveSubAgentProject: (user, env, name) => this.resolveSubAgentProject(user, env, name),
      presetProjectsFor: (user, env, name) => this.presetProjectsFor(user, env, name),
      loadProjectAgentsMd: (projectRoot) => this.loadProjectAgentsMd(projectRoot),
    }
  }

  /** 后挂定时任务调度器（生产接线：engine 先构造、CronManager 后建经 attach 回填——cron_* 工具的
   *  ToolContext 绑定读 opts.cron，不回填则 GEBAI_CRON_ENABLED=true 下工具仍恒报「能力未启用」）。 */
  setCron(cron: import("../schedule/cron").CronManager): void {
    this.opts.cron = cron
  }

  isRunning(sessionId: string): boolean {
    return this.tasks.has(sessionId)
  }

  /** 运行中会话附加快照（session.attach，DESIGN「运行中会话恢复」）：页面刷新/切换后前端据此恢复——
   *  在途流式累积（未持久化的部分文本/推理）+ 待决交互清单（审批/选择/填值/画图/捕获——事件已推送过、
   *  新页面收不到，凭此重渲染卡片继续作答）。未运行返回 null。 */
  attachSnapshot(sessionId: string): { running: true; startedAt: number; stream?: StreamSnapshot; pending: Array<Record<string, unknown>> } | null {
    const task = this.tasks.get(sessionId)
    if (!task) return null
    const pending: Array<Record<string, unknown>> = []
    for (const [toolCallId, a] of task.approvals) {
      pending.push({ type: "approval", toolCallId, tool: a.tool, retries: task.retries.get(toolCallId) ?? 0 })
    }
    for (const [choiceId, c] of task.choices) {
      pending.push({ type: "choice", choiceId, prompt: c.prompt, options: c.options, multi: c.multi, ...(c.plan ? { plan: c.plan } : {}) })
    }
    for (const [envId, e] of task.envRequests) {
      pending.push({ type: "env", envId, name: e.name, description: e.description, secret: e.secret })
    }
    for (const [renderId, d] of task.draws) {
      pending.push({ type: "draw", renderId, code: d.render.code, name: d.render.name, format: d.render.format })
    }
    for (const [captureId, c] of task.captures) {
      pending.push({ type: "capture", captureId, fullPage: c.opts.fullPage, delay: c.opts.delayMs })
    }
    return { running: true, startedAt: task.startedAt, stream: task.stream, pending }
  }

  // ---- 交互等待与决策（实现见 interactions.ts；等待状态在本类 tasks 的 TaskState 上）----
  async decideApproval(sessionId: string, toolCallId: string, approve: boolean): Promise<void> {
    decideApproval(this.tasks.get(sessionId), toolCallId, approve)
  }

  async decideChoice(sessionId: string, choiceId: string, selection: string | string[] | null): Promise<void> {
    decideChoice(this.tasks.get(sessionId), choiceId, selection)
  }

  async decideDrawResult(sessionId: string, renderId: string, result: DrawResult): Promise<void> {
    decideDrawResult(this.tasks.get(sessionId), renderId, result)
  }

  async decideCaptureResult(sessionId: string, captureId: string, result: CaptureResult): Promise<void> {
    decideCaptureResult(this.tasks.get(sessionId), captureId, result, PAGE_CAPTURE_HTML_LIMIT, VISION_MAX_IMAGE_BYTES)
  }

  async decideEnvResult(sessionId: string, envId: string, value: string | null): Promise<void> {
    decideEnvResult(this.tasks.get(sessionId), envId, value, (name) => this.isEnvNameAllowed(name))
  }

  private async waitForDraw(sessionId: string, render: { code: string; name?: string; format?: DiagramFormat }, signal?: AbortSignal): Promise<DrawResult | null> {
    return waitForDraw(sessionId, this.tasks.get(sessionId)!, this.publishFn, render, signal)
  }

  private async waitForCapture(sessionId: string, opts: { fullPage?: boolean; delayMs?: number } = {}, signal?: AbortSignal): Promise<CaptureResult | null> {
    return waitForCapture(sessionId, this.tasks.get(sessionId)!, this.publishFn, this.opts.captureTimeoutMs ?? CAPTURE_TIMEOUT, opts, signal)
  }

  private async waitForChoice(sessionId: string, prompt: string, options: ChoiceOption[], multi?: boolean, signal?: AbortSignal, plan?: ChoicePlan): Promise<ChoiceResult> {
    return waitForChoice(sessionId, this.tasks.get(sessionId)!, this.publishFn, prompt, options, multi, signal, plan)
  }

  private async waitForEnv(sessionId: string, name: string, description: string, secret: boolean, signal?: AbortSignal): Promise<boolean> {
    return waitForEnv(sessionId, this.tasks.get(sessionId)!, this.publishFn, (n) => this.isEnvNameAllowed(n), name, description, secret, signal)
  }

  // ---- 上下文压缩/溢出恢复（实现见 compressor.ts）----
  async compactSession(
    sessionId: string,
    user: string,
    scope?: "all" | { from: number; to: number },
    provider?: import("../llm/llm").LLMProvider,
    opts: { internal?: boolean } = {},
  ): Promise<{ compacted: number; summary: string }> {
    return this.compressor.compactSession(sessionId, user, scope, provider, opts)
  }

  private async summarizeBranchReport(content: string, env: Record<string, string>): Promise<string | undefined> {
    return this.compressor.summarizeBranchReport(content, env)
  }

  private async callModelWithOverflowRecovery(
    sessionId: string,
    user: string,
    provider: import("../llm/llm").LLMProvider,
    messages: MessageLike[],
    schemas: Array<{ name: string; description: string; parameters: Record<string, unknown> }>,
    systemPrompt: string,
    signal: AbortSignal,
    extraParams: Record<string, unknown> | undefined,
    ctxUsage: { ctxInputTokens?: number; ctxCachedTokens?: number; ctxCountedLen: number },
    onChunk?: (chunk: LLMChunk) => void,
  ): Promise<{ text: string; toolCalls: Array<{ id: string; name: string; arguments: Record<string, unknown>; argsError?: string; raw?: string }>; usage?: LLMUsage; stopReason?: string }> {
    return this.compressor.callModelWithOverflowRecovery(sessionId, user, provider, messages, schemas, systemPrompt, signal, extraParams, ctxUsage, onChunk)
  }

  private async makeContextRoom(
    sessionId: string,
    user: string,
    provider: import("../llm/llm").LLMProvider,
    messages: MessageLike[],
    systemPrompt: string,
    ctx: { ctxInputTokens?: number; ctxCachedTokens?: number; ctxCountedLen: number },
  ): Promise<boolean> {
    return this.compressor.makeContextRoom(sessionId, user, provider, messages, systemPrompt, ctx)
  }

  private degradeProtectedMessages(sessionId: string, user: string): Promise<boolean> {
    return this.compressor.degradeProtectedMessages(sessionId, user)
  }

  // ---- 系统提示词构建（实现见 prompt.ts）----
  private buildSystemPrompt(sessionId: string, user: string, env: Record<string, string>): string {
    return buildSystemPromptFn(this.promptDeps, sessionId, user, env)
  }

  private allPresetProjects(user: string, env: Record<string, string>): PresetProject[] {
    return allPresetProjectsFn(this.promptDeps, user, env)
  }

  private buildPresetNote(agentName: string, projectRoot: string | undefined, presetProjects: PresetProject[]): string {
    return buildPresetNoteFn(agentName, projectRoot, presetProjects)
  }

  private agentDescription(d: { name: string; description: string; tools?: string[] }, user: string, env: Record<string, string>): string {
    return agentDescriptionFn(this.promptDeps, d, user, env)
  }

  private async buildAgentSection(def: SubAgentDef, user: string, env: Record<string, string>, sessionId: string): Promise<string> {
    return buildAgentSectionFn(this.promptDeps, def, user, env, sessionId)
  }

  /** 会话删除时释放其运行态（已读文件追踪/动态工具/后台任务服务/异步运行句柄）；幂等，供 REST/WS 删除会话入口调用。 */
  forgetSession(sessionId: string): void {
    this.readFiles.delete(sessionId)
    this.dynamicTools.delete(sessionId)
    for (const key of this.shTaskServices.keys()) {
      if (key.endsWith(`:${sessionId}`)) this.shTaskServices.delete(key)
    }
    // 异步后台运行（agent_run async:true）：运行中的先终止（孤儿运行无消费者、句柄含全量存档，滞留即泄漏），
    // 该会话全部句柄移除（终态记录 prune 只在同会话新运行结束时触发，删除场景须显式清理）
    for (const [runId, h] of this.sessionRunStore) {
      if (h.sessionId !== sessionId) continue
      if (h.status === "running") h.controller.abort(new Error("会话已删除"))
      this.sessionRunStore.delete(runId)
    }
    // 分支运行（branch_run）同规则清理；合并队列一并丢弃（目标会话已不存在）
    for (const [branchId, h] of this.branchRunStore) {
      if (h.sessionId !== sessionId) continue
      if (h.status === "running") h.controller.abort(new Error("会话已删除"))
      this.branchRunStore.delete(branchId)
    }
    this.branchMerges.delete(sessionId)
  }

  /** 对本会话可见的子Agent 名集合（装载工具会话可见性，DESIGN「装载工具会话可见性」）：每次取用现算
   *  （任务中途装载下一轮 schema/解析即时生效）。本会话装载或全局装载（启动预载/admin 全局装载），
   *  其他会话的装载不扩散。 */
  private sessionVisibleAgents(sessionId: string): Set<string> {
    const visible = new Set<string>()
    for (const d of this.opts.subAgents.list()) {
      if (this.opts.subAgents.visibleTo(d.name, sessionId)) visible.add(d.name)
    }
    return visible
  }

  /** 会话注册表视图（全局注册表 + 本会话动态工具覆盖层 + 装载工具会话可见性过滤）：runLoop/buildContext 经此解析。
   *  {agent}_* 工具仅对本会话可见（其他会话装载不扩散，全局装载/启动预载对所有会话可见）；
   *  动态工具定义后同任务后续轮次 schema 立即可见、脚本内可直接按名调用。 */
  private sessionRegistry(sessionId: string): Pick<ToolRegistry, "schemas" | "resolve" | "getAgentNames"> {
    const base = this.opts.registry
    const self = this
    return {
      schemas: (enabledOnly = true) => {
        const visible = self.sessionVisibleAgents(sessionId)
        const dyn = [...(self.dynamicTools.get(sessionId)?.values() ?? [])].map(({ tool }) => ({
          name: tool.name,
          description: tool.description,
          parameters: tool.parameters as unknown as Record<string, unknown>,
        }))
        const out = [...dyn]
        for (const rt of base.list(enabledOnly)) {
          if (rt.agent && !visible.has(rt.agent)) continue // 其他会话装载的子Agent 工具：本会话不可见（未装载时路由自愈接管）
          out.push({ name: rt.name, description: rt.tool.description, parameters: rt.tool.parameters as unknown as Record<string, unknown> })
        }
        return out
      },
      resolve: (name: string) => {
        const dyn = self.dynamicTools.get(sessionId)?.get(name.replace(/[-.:]/g, "_"))
        if (dyn) return { name: dyn.tool.name, tool: dyn.tool, enabled: true }
        const rt = base.resolve(name)
        if (!rt) return undefined
        if (rt.agent && !self.sessionVisibleAgents(sessionId).has(rt.agent)) return undefined // 本会话未装载：交路由自愈按需装载
        return { name: rt.name, tool: rt.tool, agent: rt.agent, enabled: rt.enabled }
      },
      getAgentNames: () => [...self.sessionVisibleAgents(sessionId)],
    }
  }

  /** 注册会话级动态工具（js defineTool RPC → ToolContext.defineDynamicTool）：校验命名/重名/命名空间
   *  碰撞与安全模式后并入覆盖层，并随会话 chat.json 落盘（重启恢复）。 */
  private async registerDynamicTool(sessionId: string, user: string, def: DynamicToolDef): Promise<void> {
    // 安全模式：动态工具与 js 同规则降级（makeDynamicTool.execute 源码静态扫描 + 子进程只读 shim），允许注册
    const tool = makeDynamicTool(def)
    const view = this.sessionRegistry(sessionId)
    if (view.resolve(tool.name)) throw new Error(`工具名已存在: ${tool.name}（与现有工具/已定义工具冲突，请换名）`)
    for (const agent of view.getAgentNames()) {
      if (tool.name.startsWith(`${agent}_`)) throw new Error(`工具名 ${tool.name} 与子Agent 命名空间冲突（${agent}_ 前缀保留）`)
    }
    let m = this.dynamicTools.get(sessionId)
    if (!m) {
      m = new Map()
      this.dynamicTools.set(sessionId, m)
    }
    if (m.size >= AgentEngine.DYNAMIC_TOOLS_CAP) throw new Error(`本会话动态工具数量超上限（${AgentEngine.DYNAMIC_TOOLS_CAP}）`)
    // 持久化用定义（makeDynamicTool 校验/归一化后的形态）：parameters 取归一化 schema，源码去首尾空白；
    // requiresApproval 始终显式写布尔（默认 true 语义下省略键会被水合回 true，显式 false 定义必须保真）
    const persisted: DynamicToolDef = {
      name: tool.name,
      description: def.description.trim(),
      parameters: tool.parameters as unknown as Record<string, unknown>,
      source: def.source.trim(),
      requiresApproval: tool.requiresApproval === true,
    }
    m.set(tool.name, { tool, def: persisted })
    // 落盘 chat.json（store 缓存与 runLoop 消息追加同一引用，单任务/会话串行无并发写冲突）
    const session = await this.opts.store.load(sessionId, user)
    if (!session) throw new Error(`会话不存在: ${sessionId}`)
    session.dynamicTools = [...m.values()].map((x) => x.def)
    await this.opts.store.save(session)
  }

  /** run() 水合会话级动态工具（重启恢复）：按 chat.json 持久化定义重建 Tool 并入覆盖层。
   *  防御：损坏定义/与全局工具重名跳过（手工编辑等），水合数量受 DYNAMIC_TOOLS_CAP 限制；
   *  安全模式（GEBAI_SAFE_MODE）不水合——动态工具 execute 为任意代码，与只读承诺冲突（绕过通道）。 */
  private async hydrateDynamicTools(sessionId: string, session: SessionData): Promise<void> {
    const defs = session.dynamicTools
    if (!defs?.length) return
    const m = this.dynamicTools.get(sessionId) ?? new Map<string, { tool: Tool; def: DynamicToolDef }>()
    for (const d of defs.slice(0, AgentEngine.DYNAMIC_TOOLS_CAP)) {
      if (m.has(d.name) || this.opts.registry.resolve(d.name)) continue
      try {
        m.set(d.name, { tool: makeDynamicTool(d), def: d })
      } catch {
        /* 损坏定义（手工编辑等）：跳过，不阻塞会话 */
      }
    }
    if (m.size) this.dynamicTools.set(sessionId, m)
  }

  /** 取（或建）会话的 fileGuard：标记/查询本会话已读文件与内容指纹（BOM 无关——read 登记去 BOM 正文、
   *  write 比对含 BOM 原文，指纹在边界统一归一）。分支运行传入 fork 快照副本（与主线/兄弟分支隔离）。 */
  private fileGuardFor(tracked: Map<string, string | null>): NonNullable<ToolContext["fileGuard"]> {
    return {
      markRead(absPath: string, content?: string) {
        if (tracked.size >= AgentEngine.READ_TRACK_CAP) tracked.clear()
        tracked.set(absPath, content === undefined ? null : fingerprint(content))
      },
      hasRead(absPath: string) {
        return tracked.has(absPath)
      },
      staleSinceRead(absPath: string, currentContent: string) {
        const fp = tracked.get(absPath)
        return fp !== undefined && fp !== null && fp !== fingerprint(currentContent)
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
      .filter((g): g is NonNullable<import("../base/types").SubAgentDef["writeGuard"]> => !!g)
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


  /** 提交用户选择（ask 选项询问分支等待的选择）；null 表示拒绝，string 为单选（选项/自定义文本），string[] 为多选。 */

  /** 提交前端渲染结果（show 图表分支等待的渲染回传）。 */


  /** 提交前端页面捕获结果（page_capture 工具等待的捕获回传）。 */

  /**
   * 请求前端捕获当前页面并阻塞等待结果（page_capture 工具）。
   * 发布 event.capture.request（含 captureId），前端捕获渲染后 DOM html 与截图后经
   * decideCaptureResult 回传；30 秒超时返回 null（前端离线或捕获超时）。
   */
  /** 自动审批实时判定：任务 env 快照（含浏览器本地注入）或会话内存态 env 任一为 true 即跳过审批——会话运行中开启自动审批即时生效（关闭需下次任务；会话 env 不落盘，重启后由前端重新同步）。 */
  private async isApprovalSkipped(sessionId: string, user: string, env: Record<string, string>): Promise<boolean> {
    const task = this.tasks.get(sessionId)
    // 请求级审批策略优先（REST prompt/chat 的 autoApprove）：auto 显式自动通过（含服务模式——调用方
    // 即用户本人，等价其自设 GEBAI_APPROVAL_SKIP）；deny 显式不跳过（配合下方无交互硬门槛直接拒绝）
    if (task?.approvalPolicy === "auto") return true
    if (task?.approvalPolicy === "deny") return false
    // 无交互模式（REST 等单次请求通道）：单用户本地模式无人可询问，需审批工具自动通过；
    // 多用户隔离模式下不允许自动通过——普通用户不得经 REST 免审批执行敏感工具（见审批点拒绝逻辑）
    if (task?.interactionMode === "none" && this.opts.authMode !== "server") return true
    if (env.GEBAI_APPROVAL_SKIP === "true") return true
    const sessionEnv = await this.opts.store.getEnv(sessionId, user)
    return sessionEnv.GEBAI_APPROVAL_SKIP === "true"
  }

  /** 无交互通道硬门槛生效判定（需审批工具直接拒绝，不进入等待）：服务模式默认开启（普通用户免审批防线）；
   *  请求级 autoApprove=false 显式收紧（本地模式同样拒绝——单次调用通道无人可审批，不空等超时）。 */
  private noInteractionHardGate(sessionId: string): boolean {
    if (this.tasks.get(sessionId)?.approvalPolicy === "deny") return true
    return this.opts.authMode === "server"
  }

  /** 无交互通道下的需审批工具：不进入等待（无人可审批），直接返回拒绝文案。触发来源两态——
   *  服务模式默认防线（防普通用户经 REST 免审批执行敏感工具）与请求级 autoApprove=false 显式收紧。 */
  private noInteractionDenied(toolName: string): string {
    return `工具调用 ${toolName} 需要审批，但当前通道为无交互模式，无法向用户确认，已拒绝执行。请调整方案，改用无需审批的操作；如确需执行可在请求中传 autoApprove=true（由调用方担保审批），或经 Web UI（WS 通道）交互执行。`
  }


  /** ask 填值分支可设置的变量名校验：标识符格式 + 拒绝 __proto__（原型污染）+ 多用户模式拒绝审批跳过键
   * （GEBAI_APPROVAL_SKIP 是模型驱动的第四通道，不得自设——用户本人经前端开关/env 接口/飞书命令设置，
   * 防提示词注入诱导模型请求开启审批跳过；本地/单用户模式不受限）。 */
  private isEnvNameAllowed(name: string): boolean {
    if (name === "__proto__" || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) return false
    if (name === "GEBAI_APPROVAL_SKIP" && this.opts.authMode === "server") return false
    return true
  }

  /** 提交用户填写的环境变量值（ask 填值分支等待的请求回传）：非空值写入任务 env（ctx 同引用，工具后续读取立即生效）。 */


  private publish(sessionId: string, type: string, payload: Record<string, unknown>) {
    this.opts.events.publish({ type, sessionId, payload, timestamp: Date.now() })
  }

  /** 在途流式快照累积（attach 用）：delta/reasoning 发布点同步更新（messageId 变化开启新快照）；
   *  消息持久化点经 clearStream 清空（已持久化部分由存储恢复）。
   *  session 标记（新会话执行过程）在主任务快照流式期间不写入——异步后台运行与主任务真正并行
   *  （同步 agent_run 期间主任务在等工具、不流式，无此交错）：按 messageId 开新快照会互相整体替换，
   *  attach 恢复可能把后台运行文本渲染进主任务气泡；后台进度已有 event 推送（sessionRunId 路由），
   *  快照仅为 attach 兜底，主任务流式期间以主任务为准。 */
  private noteStream(sessionId: string, patch: { messageId?: string; text?: string; reasoning?: string; session?: boolean; sessionRunId?: string }): void {
    const task = this.tasks.get(sessionId)
    if (!task) return
    if (patch.session && task.stream && !task.stream.session) return
    if (patch.messageId !== undefined && patch.messageId !== task.stream?.messageId) {
      task.stream = { messageId: patch.messageId, text: "", reasoning: "", session: patch.session, sessionRunId: patch.sessionRunId }
    }
    if (!task.stream) return
    if (patch.text !== undefined) task.stream.text += patch.text
    if (patch.reasoning !== undefined) task.stream.reasoning = patch.reasoning
  }

  /** 在途流式快照清空（消息已持久化，刷新恢复改由存储承担）。sessionRunId 指定时只清属于该运行的
   *  session 快照（新会话循环轮末用——异步运行不误清正在流式的主任务快照）；缺省无条件清（主循环）。 */
  private clearStream(sessionId: string, sessionRunId?: string): void {
    const task = this.tasks.get(sessionId)
    if (!task?.stream) return
    if (sessionRunId !== undefined && (!task.stream.session || task.stream.sessionRunId !== sessionRunId)) return
    task.stream = undefined
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
      /** 通道环境注记（通道无关，注入系统提示词——飞书桥接等外部通道告知模型对话宿主/渲染/能力边界）。 */
      channelNote?: string
      /** 请求级审批策略（REST prompt/autoApprove 映射）：true 需审批工具自动通过（含服务模式——
       *  调用方即用户本人，等价其自设 GEBAI_APPROVAL_SKIP）；false 无交互通道下需审批工具直接拒绝
       *  （本地模式同样生效，不空等超时）；缺省 = 通道默认姿态。 */
      autoApprove?: boolean
    } = {},
  ): Promise<void> {
    if (this.tasks.has(sessionId)) throw new Error(`会话 ${sessionId} 已有任务在运行`)
    // 同步占位（TOCTOU 防护）：检查与注册之间存在多个 await，并发请求（WS 重复帧/REST 与 WS 双通道）
    // 会双双通过检查导致同会话双任务——消息交错持久化、tasks 注册互相覆盖、先结束任务的 finally
    // 删掉后者的注册（isRunning 归假而任务仍在跑）。先注册再异步校验，准备失败同步回滚。
    const controller = new AbortController()
    const task: TaskState = { controller, startedAt: Date.now(), approvals: new Map(), pendingDecisions: new Map(), retries: new Map(), choices: new Map(), pendingChoices: new Map(), draws: new Map(), pendingDraws: new Map(), captures: new Map(), pendingCaptures: new Map(), disabledTools: opts.disabledTools ?? [], interactionMode: opts.interactionMode ?? "realtime", outputMode: opts.outputMode ?? "streaming", role: opts.role, channelNote: opts.channelNote, env: {}, envRequests: new Map(), pendingEnvRequests: new Map(), ...(opts.autoApprove === undefined ? {} : { approvalPolicy: opts.autoApprove ? ("auto" as const) : ("deny" as const) }) }
    this.tasks.set(sessionId, task)
    // 收尾验证提醒数据（本任务范围）：修改的代码文件 + 是否运行过测试/检查类命令（runToolInterruptible 收集）
    this.taskMods.set(sessionId, { files: new Set(), verified: false })
    // 子Agent 热加载检查（目录签名变化即重扫）：每个新任务以最新定义构建系统提示词与路由
    await this.opts.subAgents.refreshIfChanged().catch(() => {})
    // 会话工作目录保证存在（纯命令会话无附件/写文件时 tmp/ 从未创建，sh/py/js/ls 相对路径全体 ENOENT）
    const { mkdir } = await import("node:fs/promises")
    await mkdir(this.opts.sandbox.workdir(user, sessionId), { recursive: true }).catch(() => {})
    try {
      const session = await this.opts.store.load(sessionId, user)
      if (!session) throw new Error(`会话不存在: ${sessionId}`)
      // 会话级子Agent 装载保障（DESIGN「装载 vs 新会话执行」）：新会话按启动预载名单初始化
      // （工具注册 + 提示词 system 消息写入会话记录），恢复历史会话时按会话记录重新注册工具并补齐提示词消息
      await this.ensureSessionAgents(session)
      // 会话级动态工具水合（重启恢复）：js defineTool 注册的定义随 chat.json 落盘，run() 时重建
      await this.hydrateDynamicTools(sessionId, session)
    } catch (err) {
      this.tasks.delete(sessionId)
      this.taskMods.delete(sessionId)
      throw err
    }

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
      }, user)
    } catch (err) {
      this.publish(sessionId, "event.task.error", { error: String((err as Error).message || err) })
      this.tasks.delete(sessionId)
      return
    }

    try {
      // 浏览器本地环境变量（前端 localStorage）经 prompt 请求临时注入，仅本次任务生效，不持久化
      const env = { ...(await this.opts.env.resolve(sessionId, user)), ...(opts.envOverride || {}) }
      // 任务级 env 引用：ask 填值后原地更新（ctx.env 同一引用，工具后续读取立即生效）
      this.tasks.get(sessionId)!.env = env
      // 极简模式（DESIGN「极简模式」）：任务启动按 env 快照裁剪工具白名单（仅 sh/edit + full_mode 切换入口），
      // 系统提示词同步极简化（buildSystemPrompt 极简分支），下次任务起生效
      if (env.GEBAI_MINIMAL_MODE === "true") this.tasks.get(sessionId)!.enabledTools = ["sh", "edit", "full_mode"]
      // 任务级主模型：env 配置 GEBAI_LLM_* 时重建 Provider（无覆盖时沿用启动实例）
      const taskProvider = this.opts.resolveProvider?.(env) ?? this.opts.provider
      const systemPrompt = this.buildSystemPrompt(sessionId, user, env)
      let history = await this.loadHistory(sessionId, user, taskProvider.capabilities().multimodal)
      // 自动压缩（DESIGN「上下文保护」）：上下文接近窗口阈值（80%）时先压缩最早历史，
      // 保证最新上下文完整、压缩过程对进行中的任务透明（阈值与摘要均用任务级模型）。
      // 占用口径：只认模型服务返回的真实 input tokens——上次任务最后一次调用持久化的 usage
      // 基线（session.ctxInputTokens，含 system 与工具 schema）。基线本身已超阈值即先压缩
      // （本次调用只会更大）；无基线（新会话/接口不返回 usage/压缩后锚点失效）不做估算预判，
      // 由本次调用返回的真实 usage（中途压缩）与接口上下文溢出恢复兜底，避免估算误判
      const cap = taskProvider.capabilities().maxContextTokens
      if (cap > 0) {
        // 迭代压缩：单次压缩可能不够（压缩后基线清除——摘要替换消息使锚点失效，循环随基线退出）；
        // 压缩无效（无可压缩内容，如历史几乎全是用户输入/系统提示词）时启用硬护栏——
        // 受保护消息让路（历史图片降级为文本说明、最旧用户消息裁剪为占位），
        // 保证长会话存在可收敛的溢出兜底，而非等模型接口报错后任务失败
        let baseline = (await this.opts.store.load(sessionId, user))?.ctxInputTokens
        for (let guard = 0; guard < 4 && baseline !== undefined && baseline > cap * COMPACT_RATIO; guard++) {
          const before = history.length
          await this.compactSession(sessionId, user, undefined, taskProvider, { internal: true })
          history = await this.loadHistory(sessionId, user, taskProvider.capabilities().multimodal)
          baseline = (await this.opts.store.load(sessionId, user))?.ctxInputTokens
          if (history.length >= before) {
            // 压缩无效（仅剩受保护消息）：硬护栏降级受保护消息（原文仍在会话存储中，不丢数据）
            const degraded = await this.degradeProtectedMessages(sessionId, user)
            if (!degraded) break
            history = await this.loadHistory(sessionId, user, taskProvider.capabilities().multimodal)
            baseline = (await this.opts.store.load(sessionId, user))?.ctxInputTokens
          }
        }
      }
      const messages: MessageLike[] = [{ role: "system", content: systemPrompt }, ...history]

      // 待办续做：每轮会话完成（模型给出最终回复）后检查待办，pending/in_progress 未完成则
      // 追加提醒消息继续会话，直至全部完成或达到续做轮次上限（DESIGN「待办续做」）
      let continueRound = 0
      let verifyRound = 0
      let finalText = ""
      let lastFinalText = ""
      let res: { text: string; reasoning: string; lastMessageId?: string; ctxInputTokens?: number; ctxCachedTokens?: number; ctxCountedLen: number } | undefined
      for (;;) {
        res = await this.runLoop({
          sessionId,
          user,
          messages,
          systemPrompt,
          registry: this.sessionRegistry(sessionId),
          signal: controller.signal,
          env,
          provider: taskProvider,
          // 任务级额外模型接口参数：浏览器本地注入 GEBAI_LLM_EXTRA_PARAMS 时覆盖 Provider 级配置（非法 JSON 忽略）
          extraParams: parseExtraParamsSafe(env.GEBAI_LLM_EXTRA_PARAMS),
          persist: (msg) => this.opts.store.appendMessage(sessionId, msg, user),
        })
        finalText = res.text

        if (finalText) {
          await this.opts.store.appendMessage(sessionId, {
            // 最终轮的流式 messageId（撤回/反馈定位对刚完成的回复立即生效）；无最终轮（重复终止/轮次上限）时生成
            id: res.lastMessageId ?? crypto.randomUUID(),
            role: "assistant",
            content: finalText,
            reasoning: res.reasoning.trim() ? res.reasoning.trim() : undefined,
            createdAt: Date.now(),
          }, user)
          this.clearStream(sessionId) // 最终回复已持久化，在途快照清空
          // 主线进展广播（分支互相感知，DESIGN「会话分支运行与合并」）：异步分支运行中主线每轮最终回复
          // 通知各分支（同步 fan-out 期间主线阻塞在 branch_run 工具内，无此交错）——分支据此感知主线决策
          if ([...this.branchRunStore.values()].some((h) => h.sessionId === sessionId && h.status === "running")) {
            this.relayToBranches(sessionId, undefined, `【主线进展】主线回复:\n${branchNoticeHead(finalText)}`)
          }
        }
        if (controller.signal.aborted) break

        const todos = await this.opts.store.getTodos(sessionId, user)
        const pending = todos.filter((t) => t.status === "pending" || t.status === "in_progress")
        if (!pending.length) {
          // 收尾验证提醒（DESIGN「收尾验证提醒」，与待办续做同机制）：任务修改了代码文件但全程未运行
          // 任何测试/检查类命令——注入一次提醒让模型先验证再收尾（或说明不适用原因），上限 MAX_VERIFY_NUDGE 轮
          const mods = this.taskMods.get(sessionId)
          if (mods && mods.files.size > 0 && !mods.verified && verifyRound < MAX_VERIFY_NUDGE) {
            const list = [...mods.files].slice(0, 5).map((f) => `- ${f}`).join("\n")
            const more = mods.files.size > 5 ? `\n…（共 ${mods.files.size} 个文件）` : ""
            const verifyMsg = `【验证提醒】本任务修改了 ${mods.files.size} 个代码文件，但尚未运行任何测试/类型检查/lint 类命令：\n${list}${more}\n请先运行与改动相关的测试或检查（如 bun test 指定相关测试文件、bun run typecheck / lint、pytest、go test 等）确认无回归后再给出最终回复；若改动确不影响代码行为（生成产物/临时脚本等），请在回复中简要说明。`
            messages.push({ role: "assistant", content: finalText })
            messages.push({ role: "user", content: verifyMsg })
            await this.opts.store.appendMessage(sessionId, {
              id: crypto.randomUUID(),
              role: "user",
              content: verifyMsg,
              createdAt: Date.now(),
            }, user)
            verifyRound++
            continue
          }
          break
        }
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
        }, user)
        lastFinalText = finalText
        continueRound++
        this.publish(sessionId, "event.todo.continue", { round: continueRound, remaining: pending.length, sessionId })
      }

      // 上下文大小与真实 usage 基线持久化（历史会话列表展示 + 下次 run 压缩判定基线）：
      // ctxInputTokens = 最近一次调用的真实 input tokens（含 system 与工具 schema）；ctxAtMessage = 那次调用
      // 已覆盖的历史消息条数（loadHistory 坐标，下次 run 以 history.slice 估算基线后的增量）；
      // ctxTokens（列表展示）= 真实基线 + 未发送增量估算；ctxCachedTokens（上下文悬浮命中率展示）= 同一
      // 次调用的提示词缓存命中（接口不返回缓存字段时 undefined）；无真值（接口不返回 usage）时估算兜底（与 toSessionInfo 同口径）
      if (finalText) messages.push({ role: "assistant", content: finalText }) // 最终回复也在基线之后，计入增量估算
      const saved = await this.opts.store.load(sessionId, user)
      if (saved && res) {
        if (res.ctxInputTokens !== undefined) {
          saved.ctxInputTokens = res.ctxInputTokens
          saved.ctxAtMessage = Math.max(0, res.ctxCountedLen - 1)
          saved.ctxTokens = res.ctxInputTokens + estimateTokens(messages.slice(res.ctxCountedLen))
          saved.ctxCachedTokens = res.ctxCachedTokens
        } else {
          saved.ctxInputTokens = undefined
          saved.ctxAtMessage = undefined
          saved.ctxTokens = estimateCtxTokens(saved.messages)
          saved.ctxCachedTokens = undefined
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
      this.taskMods.delete(sessionId)
      // 分支合并队列冲刷（仅落盘；上下文随任务结束，下次 run 经 loadHistory 进上下文）：置于 tasks.delete
      // 之后——其后完成的分支经 trunkMerge 判定无任务直接落盘，无入队/漏排空竞态
      await this.drainBranchMerges(sessionId, user).catch(() => {})
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
    // 历史图片内联窗口（从后往前数第几组图片）：超过窗口的降级为文本说明——
    // 图片永久占据上下文且不参与压缩，长会话会被历史图片占死窗口；
    // 用户附件图片与工具结果图片（read 读取的图片引用）同窗口计数
    let recentImageGroups = 0
    // 先确定哪些位置的图片允许内联（从最新往回数 INLINE_IMAGE_RECENT 组）
    const msgs = session?.messages ?? []
    const inlineAllowed = new Array<boolean>(msgs.length).fill(false)
    for (let i = msgs.length - 1; i >= 0; i--) {
      const m = msgs[i]
      const hasImages = (m.role === "user" && m.attachments?.some((a) => VISION_MIME_SET.has(a.mime))) || (m.role === "tool" && m.images?.length)
      if (!hasImages) continue
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
        // 工具结果图片（read 等读取的图片引用，Message.images）：多模态且在最近内联窗口时按引用重读内联
        const imgBlocks = inlineMultimodal && allowInline && m.images?.length ? await this.toolImageBlocks(m.images) : []
        out.push({
          role: "tool",
          toolCallId: m.toolCallId,
          content: imgBlocks.length ? [{ type: "text", text: m.content }, ...imgBlocks] : m.content,
          name: m.name,
        })
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

  /** 将消息中的内联图片块降级为文本说明（返回是否发生降级）：附件块（source 缺省）与工具结果图片块
   *  （source="tool"，read 等读取的图片）分别用对应说明文案。 */
  private degradeImageBlocks(messages: MessageLike[]): boolean {
    let changed = false
    for (const m of messages) {
      if (!Array.isArray(m.content)) continue
      let next: Array<Record<string, unknown>> | undefined
      for (const b of m.content) {
        if (b.type !== "image" || typeof b.path !== "string" || typeof b.name !== "string" || typeof b.mime !== "string") continue
        next ??= [...m.content]
        next[m.content.indexOf(b)] =
          b.source === "tool"
            ? { type: "text", text: toolImageNote(b) }
            : { type: "text", text: attachmentNote({ path: b.path, name: b.name, mime: b.mime, size: Number(b.size ?? 0) }, true) }
        changed = true
      }
      if (next) m.content = next
    }
    return changed
  }

  /** 工具结果图片引用 → 统一 image 块（多模态内联，DESIGN「多模态支持」read 图片内联）：
   *  data 缺省时按绝对路径重读（历史重建的落盘引用形态）；读取失败/超限（8MB）跳过该图——
   *  工具结果文本自带说明兜底，不阻塞结果入上下文。块携带 source="tool" 供接口拒绝时降级为工具图片说明。 */
  private async toolImageBlocks(images: ToolResultImage[]): Promise<Array<Record<string, unknown>>> {
    const blocks: Array<Record<string, unknown>> = []
    for (const img of images) {
      try {
        let data = img.data
        if (!data) {
          const buf = await Bun.file(img.path).arrayBuffer()
          if (buf.byteLength <= 0 || buf.byteLength > ATTACHMENT_INLINE_LIMIT) continue
          data = Buffer.from(buf).toString("base64")
        }
        const display = img.display ?? img.path
        blocks.push({ type: "image", mime: img.mime, data, path: display, name: basenameName(display) || display, source: "tool" })
      } catch {
        /* 文件缺失/不可读：跳过（文本说明兜底） */
      }
    }
    return blocks
  }


  /** 汇总所有已注册子Agent 的预置项目注册表（{AGENT_NAME_UPPER}_PROJECTS）：装载模式下总Agent 直接使用子Agent 工具时 project 参数路由用；同名去重（首个生效）。 */

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


  /**
   * 装载子Agent 到会话：逐个 subAgents.load（幂等注册工具，返回本次实际装载集合——依赖自动连带装载时依赖也计入），
   * 为每个新装载的子Agent 生成提示词 system 消息（### name（description）头 + 完整系统提示词 + 预置项目清单注记，
   * loadedAgent 标记）追加进会话 messages 并记录 loadedSubAgents（调用方负责 save）。已装载且提示词消息已存在的跳过（恢复场景幂等）。
   */
  private async loadAgentsForSession(session: SessionData, names: string[], env: Record<string, string>): Promise<string[]> {
    const added: string[] = []
    for (const name of names) {
      let loadedNow: string[]
      try {
        loadedNow = await this.opts.subAgents.load(name, session.id)
      } catch (err) {
        console.warn(`[engine] 装载子Agent ${name} 失败: ${(err as Error).message}`)
        continue
      }
      for (const n of loadedNow) {
        if (session.messages.some((m) => m.loadedAgent === n)) continue // 提示词消息已持久化（恢复场景）
        const def = this.opts.subAgents.def(n)
        if (!def) continue
        // 预置项目清单动态注入（装载模式闭环：模型按名使用 project 参数；与 runNewSession 的 presetNote 一致）；
        // 项目根注记与 agent_run 形态（buildAgentSection）对齐——装载模式下相对路径基准仍是会话目录，
        // 注记附限定语防误导（不宣称工作目录已切换，访问项目用 project 参数或绝对路径）
        const projectRoot = this.resolveSubAgentProject(session.userId, env, n)
        const workNote = projectRoot
          ? `\n项目根: ${projectRoot}（访问项目用 project 参数传该根或绝对路径；相对路径仍以会话工作目录为基准）`
          : `\n工作目录: ${sessionPath(this.opts.config.gebaiHome, session.userId, session.id)}/tmp`
        const presetNote = this.buildPresetNote(n, projectRoot, this.presetProjectsFor(session.userId, env, n))
        session.messages.push({
          id: crypto.randomUUID().replace(/-/g, ""),
          role: "system",
          loadedAgent: n,
          content: `### ${n}（${def.description}）\n${workNote}${presetNote}${def.systemPrompt}`,
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
    // 按会话解引用：其他会话/全局仍装载同名子Agent 时工具注册保留（全局注册表被砍会导致
    // 其他会话的 resolve 失败、工具调用全部「未知工具」）
    this.opts.subAgents.unload(name, sessionId)
  }

  private buildContext(
    sessionId: string,
    user: string,
    env: Record<string, string>,
    signal: AbortSignal,
    opts?: { workdir?: string; resolveBase?: string; projects?: PresetProject[]; role?: string; messages?: MessageLike[]; registry?: Pick<ToolRegistry, "schemas" | "resolve" | "getAgentNames">; writeGuard?: ToolContext["writeGuard"]; registerDynamic?: (def: DynamicToolDef) => Promise<void>; loadIntoRegistry?: ToolRegistry; branchSync?: (content?: string) => Promise<string>; fileGuardMap?: Map<string, string | null>; multimodal?: boolean },
    depth = 0,
  ): ToolContext {
    const store = this.opts.store
    const sandbox = this.opts.sandbox
    const self = this
    // 会话工作区（保留项目名 tmp 的解析目标）：恒定注入——workdir 可被项目绑定改写为项目根，tmp 不随之变化
    const sessionWorkdir = sandbox.workdir(user, sessionId)
    const workdir = opts?.workdir ?? sessionWorkdir
    const resolveRoot = opts?.resolveBase
    const projects = opts?.projects ?? []
    // 子进程取消信号：任务取消统一生效（子Agent 不设独立超时，无额外信号源）
    const execSignal = signal
    // 已读追踪表：缺省按会话（主循环/agent_run 新会话共用会话级表）；分支运行传 fork 快照副本（隔离，防跨流串扰）
    let sessionReads = this.readFiles.get(sessionId)
    if (!sessionReads) {
      sessionReads = new Map()
      this.readFiles.set(sessionId, sessionReads)
    }
    const guardMap = opts?.fileGuardMap ?? sessionReads
    return {
      user,
      userRole: opts?.role,
      authMode: this.opts.authMode,
      sessionId,
      workdir,
      sessionWorkdir,
      // 当前任务交互模式：show 等合并型工具按分支校验通道能力（HTML 预览仅 realtime 等）
      interactionMode: this.tasks.get(sessionId)?.interactionMode,
      boundProjectRoot: resolveRoot,
      home: this.opts.config.gebaiHome,
      env,
      sandboxed: sandbox.enforcedFor(user),
      // 任务级主模型多模态能力：read 等工具据此决定图片文件的处理形态（多模态=内联，非多模态=vision 指引）
      multimodal: opts?.multimodal,
      // 任务取消信号：js 脚本工具等监听中止并终止子进程（sh/py 经 runCommand 默认注入 execSignal）
      signal,
      // 会话上下文快照（js 脚本工具 ctx.messages 注入源）：live messages 数组按需取值，
      // 文本抽取（内容块取 text 段）、跳过 system/空消息；条数/单条长度截断由 js-tool 侧负责
      recentMessages: opts?.messages
        ? () =>
            (opts!.messages ?? [])
              .filter((m) => m.role !== "system")
              .map((m) => ({
                role: m.role,
                ...(m.name ? { name: m.name } : {}),
                content: typeof m.content === "string" ? m.content : (m.content ?? []).map((b) => (typeof b?.text === "string" ? (b.text as string) : "")).filter(Boolean).join("\n"),
              }))
              .filter((m) => m.content.trim().length > 0)
        : undefined,
      safeMode: this.opts.config.safeMode,
      fileGuard: this.fileGuardFor(guardMap),
      // 写范围守卫：显式传入（新会话模式：预加载子Agent 名单静态已知）或按会话装载名单动态收集（装载模式）
      writeGuard: opts?.writeGuard ?? ((absPaths: string[]) => this.sessionWriteGuard(sessionId, user, env, absPaths)),
      // 退出极简模式（full_mode 工具，DESIGN「极简模式」）：清会话极简标记（任务 env 快照 + 会话内存 env）并
      // 解锁当前任务工具白名单（下一轮 schema 即全量下发）；系统提示词原地升级为完整版（极简任务以极简提示词
      // 启动，首条 system 与极简参照全等才替换——agent_run 新会话 messages[0] 为子Agent 提示词，天然不触发）；
      // 发布 event.session.minimal 通知前端关闭本地开关（防下次任务前幂等同步把极简标记写回）
      exitMinimalMode: async () => {
        const m = opts?.messages
        const minimalRef = m?.length && m[0].role === "system" && typeof m[0].content === "string"
          ? this.buildSystemPrompt(sessionId, user, { ...env, GEBAI_MINIMAL_MODE: "true" })
          : undefined
        delete env.GEBAI_MINIMAL_MODE
        const task = this.tasks.get(sessionId)
        if (task) {
          delete task.env.GEBAI_MINIMAL_MODE
          task.enabledTools = undefined
        }
        await store.setEnv(sessionId, user, { GEBAI_MINIMAL_MODE: null })
        if (minimalRef !== undefined && m![0].content === minimalRef) m![0].content = this.buildSystemPrompt(sessionId, user, env)
        this.publish(sessionId, "event.session.minimal", { enabled: false, sessionId })
      },
      resolvePath: (p) => {
        // 子Agent 项目绑定：路径以项目根为基准（沙箱约束用户限定项目内，豁免/本地模式放开）
        if (resolveRoot) return sandbox.enforcedFor(user) ? resolveInSandbox(resolveRoot, p) : resolve(resolveRoot, p)
        return sandbox.resolvePath(user, sessionId, p)
      },
      // node utf8 读取保留 UTF-8 BOM（Bun.file().text() 会剥离）——read 展示/edit 匹配在工具层统一
      // stripBom 去除、edit/patch/write 写回时按原文件补回（BOM 文件往返编辑不丢头，见 DESIGN「BOM 感知」）
      readFile: async (p) => {
        const { readFile } = await import("node:fs/promises")
        return readFile(p, "utf8")
      },
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
      // 后端图表渲染（show 图表分支 render=backend，四语言）：惰性加载组合渲染器
      // （PlantUML TeaVM 引擎 / Mermaid + happy-dom / D2 WASM + @resvg/resvg-js），仅按需时加载，不拖慢启动/测试
      renderDiagram: async (code, opts) => {
        const { createDiagramRenderer } = await import("../support/diagram-render")
        return createDiagramRenderer().renderPng(code, { format: opts?.format, background: opts?.background, maxWidth: opts?.maxWidth, maxHeight: opts?.maxHeight })
      },
      // 文件清单基准与路径解析基准一致：绑定项目根（agent_run 新会话 {AGENT}_PROJECT）时列出项目文件树
      // （search_symbols 等以 listFiles 为扫描清单的工具随项目根生效）；默认列出会话 tmp 子树
      listFiles: () => (resolveRoot ? walkDirFiles(resolveRoot) : store.listSessionFiles(sessionId, user)),
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
      // sh 异步后台任务服务（DESIGN「sh 异步执行」）：会话 tmp/sh-tasks/ 落盘，进程经 Sandbox.spawnBackground
      // 启动（同 exec 的 env 脱敏/chcp/进程组语义，输出合并写日志文件）
      shTasks: this.shTaskServiceFor(user, sessionId),
      uploadAttachment: async (ref) => ref.path,
      publish: (type, payload) => self.publish(sessionId, type, payload),
      projects,
      resolveProjectPath: (name) => {
        const p = projects.find((x) => x.name === name)
        if (!p) throw new Error(`未知预置项目: ${name}`)
        return p.path
      },
      getTodos: () => store.getTodos(sessionId, user),
      setTodos: (todos) => store.setTodos(sessionId, todos, user),
      registry: {
        schemas: (enabledOnly = true) => (opts?.registry ?? self.opts.registry).schemas(enabledOnly),
        resolve: (name) => {
          const rt = (opts?.registry ?? self.opts.registry).resolve(name)
          return rt ? { name: rt.name, tool: rt.tool } : undefined
        },
        getAgentNames: () => (opts?.registry ?? self.opts.registry).getAgentNames(),
      },
      listSubAgentDefs: () =>
        self.opts.subAgents.list().map((d) => ({ name: d.name, description: self.agentDescription({ name: d.name, description: d.description, tools: d.tools }, user, env), preload: d.preload, loaded: d.loaded, tools: d.tools })),
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
          // 新会话执行形态（临时会话无持久化 SessionData）：仅全局注册工具（共享 run 标记防引用表随每次 agent_run 增长）
          await self.opts.subAgents.load(String(name), "agent_run").catch(() => {})
        }
        // 新会话执行形态的双注册（DESIGN「子Agent 路由」）：ctx 处于 per-run 注册表环境（loadIntoRegistry 注入）时，
        // 工具须同时注册进本次运行注册表——仅注册进全局注册表对新会话循环不可见（reg.resolve 查不上），
        // 模型装载后立即调用会「未知工具」
        if (opts?.loadIntoRegistry) self.registerIntoRegistry(opts.loadIntoRegistry, String(name))
        // 装载结果外露（agent_load 工具的失败通道）：装载后仍未注册 = 未知名或其文件加载失败——
        // loadAgentsForSession 对单个失败是 warn+跳过（会话恢复容错语义），agent_load 语义需要真实失败
        // 原因（含 loadErrors 附因）；已装载（幂等重装）视为成功不抛
        if (!self.opts.subAgents.isLoaded(String(name))) throw new Error(self.opts.subAgents.unknownAgentError(String(name)))
      },
      // 执行新会话（agent_run 工具）：派生临时新会话、预加载子Agent 列表后执行任务（DESIGN「装载 vs 新会话执行」）；
      // inheritGlobalTools（默认 true）= 全局工具一并注册进新会话；inheritGlobalPrompt（默认 true）= 总Agent
      // 全局提示词注入新会话；深度 +1 限制递归嵌套
      runNewSession: (agents, input, opts) => self.runNewSession(sessionId, user, env, agents, input, signal, depth + 1, opts),
      // agent_run 异步后台运行服务（agent_run async:true 启动、bg_task 查询/等待/终止；DESIGN「新会话执行的异步运行」）：
      // 句柄存引擎级共享表，本实例为按会话过滤视图；父任务取消信号传播进后台运行（用户停止连带终止）
      sessionRuns: new SessionRunRegistry({
        sessionId,
        store: self.sessionRunStore,
        parentSignal: signal,
        // 异步启动校验前置热加载重扫（与 runNewSession 同规则）：新写/新修的子Agent 文件即时可见
        validate: async (agents) => {
          await self.opts.subAgents.refreshIfChanged().catch(() => {})
          return self.normalizeRunAgents(agents, depth + 1)
        },
        runner: (agents, input, runSignal, runOpts) => self.runNewSession(sessionId, user, env, agents, input, runSignal, depth + 1, runOpts),
      }),
      // 会话分支运行服务（branch_run 工具，DESIGN「会话分支运行与合并」）：仅主循环（depth 0）注入——
      // 分支内/新会话执行内不可再分支（防递归扇出爆炸，分支是主上下文派生语义）。fork 源绑定本上下文
      // live messages（分支 fork 主线当前上下文快照，start 同步切片）；分支完成经 onDone 走引擎合并
      // 队列自动合入主上下文。
      branchRuns: depth === 0 && opts?.messages
        ? new BranchRunRegistry({
            sessionId,
            store: self.branchRunStore,
            runner: (spec, branchSignal, forkMessages) => self.runBranch(sessionId, user, env, spec, branchSignal, forkMessages),
            forkSource: opts.messages,
            parentSignal: signal,
            onDone: (handle) => self.trunkMerge(sessionId, user, env, handle, { final: true, content: handle.output ?? "" }),
          })
        : undefined,
      // 分支与主干双向同步（branch_sync 工具，DESIGN「会话分支运行与合并」互相感知）：仅分支运行上下文
      // 注入（runBranch 绑定分支身份回调——传 content 先合入再统一返回主干增量）；主会话/新会话执行未注入
      branchSync: opts?.branchSync,
      // 运行时工具定义（js defineTool）：主会话 → 会话覆盖层（随会话落盘）；新会话执行 → 本次运行注册表（随运行结束释放）。
      // 可选：未注入（无 registerDynamic 来源）时 js 侧 defineTool 返回不可用错误
      defineDynamicTool: opts?.registerDynamic,
      waitForChoice: (prompt, options, multi, plan) => self.waitForChoice(sessionId, prompt, options, multi, execSignal, plan),
      waitForEnv: (name, description, secret) => self.waitForEnv(sessionId, name, description ?? "", secret === true, execSignal),
      waitForDraw: (render) => self.waitForDraw(sessionId, render, execSignal),
      waitForCapture: (opts) => self.waitForCapture(sessionId, opts, execSignal),
      cron: self.opts.cron
        ? {
            add: (input, originSessionId) => self.opts.cron!.add(user, input, originSessionId ?? sessionId),
            list: () => self.opts.cron!.list(user),
            remove: (id) => self.opts.cron!.remove(user, id),
            update: (id, patch) => self.opts.cron!.update(user, id, patch),
            trigger: (id) => self.opts.cron!.trigger(user, id),
          }
        : undefined,
    }
  }

  /** 会话级 sh 异步后台任务服务（按 user:sessionId 复用；记录落盘会话 tmp/sh-tasks/，跨调用/跨重启可见）。 */
  private shTaskServiceFor(user: string, sessionId: string): ShTaskRunner {
    const key = `${user}:${sessionId}`
    let svc = this.shTaskServices.get(key)
    if (!svc) {
      const sandbox = this.opts.sandbox
      svc = new ShTaskRunner({
        dir: join(sandbox.workdir(user, sessionId), "sh-tasks"),
        spawner: (cmd, o) => sandbox.spawnBackground(cmd, { cwd: o.cwd, env: o.env, logPath: o.logPath, input: o.input, user }),
      })
      this.shTaskServices.set(key, svc)
    }
    return svc
  }

  /** 工具名 → 子Agent 归属（最长 `{agent}_` 前缀匹配，路由自愈用）：无匹配返回 undefined。 */
  private subAgentForToolName(name: string): string | undefined {
    const candidates = this.opts.subAgents.list().map((d) => d.name).filter((a) => name.startsWith(`${a}_`))
    if (!candidates.length) return undefined
    return candidates.sort((a, b) => b.length - a.length)[0]
  }

  /** 未知工具错误信息：命中某子Agent 命名空间时列出其可用工具全名（模型拼错工具名时的直接恢复路径）。 */
  private unknownToolMsg(name: string): string {
    const agent = this.subAgentForToolName(name)
    if (!agent) return `未知工具: ${name}`
    const tools = Object.keys(this.opts.subAgents.def(agent)?.tools ?? {}).map((t) => `${agent}_${t}`)
    return tools.length ? `未知工具: ${name}（${agent} 的可用工具: ${tools.join("、")}）` : `未知工具: ${name}`
  }

  /** 工具注册进指定注册表（幂等：任一工具已可解析则视为已注册；纯提示词子Agent 补注入编排工具；
   *  依赖级联同规则展开（cascade：依赖在前，reverse_site 连带 playwright、self_optimize 连带 code 等））。
   *  新会话循环内装载（路由自愈/显式 agent_load）用。 */
  private registerIntoRegistry(reg: ToolRegistry, name: string): void {
    for (const n of this.opts.subAgents.cascade(name)) {
      const def = this.opts.subAgents.def(n)
      if (!def) continue
      const keys = Object.keys(def.tools ?? {})
      if (!keys.length) {
        // 纯提示词子Agent：编排工具补注入（已注入则跳过——register 重名会抛错）
        if (!reg.resolve("agent_list")) {
          reg.register(agentListTool)
          reg.register(agentLoadTool)
          reg.register(agentRunTool)
        }
        continue
      }
      if (keys.some((k) => reg.resolve(`${n}_${k}`))) continue
      reg.registerSubAgentTools(n, def.tools ?? {}, def.requiresApproval)
    }
  }


  /** 新会话循环内的装载（路由自愈）：注册进本次运行注册表 + 提示词段落插入临时 messages 系统前置段 +
   *  预置项目并入 ctx.projects 引用数组——不写全局注册表、不落盘父会话记录（新会话执行隔离语义）。 */
  private async loadAgentIntoRun(
    reg: ToolRegistry,
    agent: string,
    messages: MessageLike[],
    user: string,
    env: Record<string, string>,
    sessionId: string,
    projects?: PresetProject[],
  ): Promise<void> {
    const def = this.opts.subAgents.def(agent)
    if (!def) return
    this.registerIntoRegistry(reg, agent)
    let i = 0
    while (i < messages.length && messages[i].role === "system") i++
    messages.splice(i, 0, { role: "system", content: await this.buildAgentSection(def, user, env, sessionId) })
    if (projects) {
      const seen = new Set(projects.map((p) => p.name))
      for (const p of this.presetProjectsFor(user, env, agent)) {
        if (!seen.has(p.name)) {
          seen.add(p.name)
          projects.push(p)
        }
      }
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
    sessionId?: string,
  ): Promise<{ text: string; toolCalls: Array<{ id: string; name: string; arguments: Record<string, unknown>; argsError?: string; raw?: string }>; usage?: LLMUsage; stopReason?: string }> {
    const toolCalls: Array<{ id: string; name: string; arguments: Record<string, unknown>; argsError?: string; raw?: string }> = []
    let text = ""
    let reasoningSeen = false
    let attempts = 0
    // 重试注入的提示（仅本次调用，不写入会话历史）：泄漏/空响应时引导模型重新输出
    let hint = ""
    // 多模态图片块已降级标记（DESIGN「能力探测」自动降级）：接口拒绝图片块后一次性降级为文本说明
    let degraded = false
    // 本次调用的 usage 真值（服务端不返回时保持 undefined → 调用方估算兜底）；每次尝试重制，仅最后一次成功尝试生效
    let usage: LLMUsage | undefined
    // 最后一次成功尝试的结束原因（length/max_tokens = 输出上限截断，argsError 处理区分引导）
    let stopReason: string | undefined
    for (;;) {
      const msgs = hint ? [...messages, { role: "user" as const, content: hint }] : messages
      try {
        usage = undefined
        stopReason = undefined
        for await (const chunk of this.chatWithIdleTimeout(provider, msgs, schemas, signal, extraParams)) {
          if (chunk.type === "text") text += chunk.text
          else if (chunk.type === "reasoning" && chunk.text?.trim()) reasoningSeen = true
          else if (chunk.type === "tool_call" && chunk.toolCall) toolCalls.push({ ...chunk.toolCall, argsError: chunk.toolArgsError })
          else if (chunk.type === "done" && chunk.stopReason) stopReason = chunk.stopReason
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
        // 模型服务异常且将重试（DESIGN「模型服务异常可见」）：推送前端实时提示——
        // 重试退避期间任务无任何输出，用户无从得知模型服务已出错
        if (sessionId) this.publish(sessionId, "event.model.error", { error: (err as Error).message, retry: attempts + 1, maxRetry: LLM_RETRY_COUNT, sessionId })
        hint = "" // 普通接口异常：与内容无关，不注入提示
        attempts++
        await sleep((this.opts.retryBackoffMs ?? LLM_RETRY_BACKOFF_MS) * 2 ** (attempts - 1), signal)
        continue
      }
      // 正常结束但空响应（无文本且无工具调用，含「只思考未输出」）：注入提示重试；耗尽后抛错
      if (!text && !toolCalls.length) {
        if (signal.aborted) throw new Error("cancelled")
        if (attempts < LLM_RETRY_COUNT) {
          if (sessionId) this.publish(sessionId, "event.model.error", { error: "模型未返回任何内容（空响应）", retry: attempts + 1, maxRetry: LLM_RETRY_COUNT, sessionId })
          hint = "你上一轮没有返回任何内容。请直接给出回答，或调用工具继续，不要复述要求。"
          attempts++
          await sleep(this.opts.retryBackoffMs ?? LLM_RETRY_BACKOFF_MS, signal)
          continue
        }
        throw new Error(`模型未返回任何内容（已重试 ${attempts} 次）`)
      }
      return { text, toolCalls, usage, stopReason }
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

  /** 输出上限截断的结束原因（OpenAI `length` / Anthropic `max_tokens` / Responses incomplete 对齐为 `length`）。 */
  private isTruncatedStop(stopReason: string | undefined): boolean {
    return stopReason === "length" || stopReason === "max_tokens"
  }

  /** write 类工具判定（全局 write 与子Agent 命名空间 `{agent}_write`，同 schema 可抢救）。 */
  private isWriteToolName(name: string): boolean {
    return name === "write" || name.endsWith("_write")
  }

  /**
   * 截断参数抢救落盘（大文件生成防护，DESIGN「大文件分段写入」）：write 类工具参数 JSON 未完整
   * 生成（输出被截断/非法）时，容错解析原始前缀提取 path 与已生成 content，先把部分内容落盘并
   * 登记已读——失败轮次转化为进度，模型下一轮 append 续写剩余部分即可（无需重新生成全量）。
   * 目标文件已存在且本会话未读（防盲覆盖守卫同规则适用）或前缀提取/落盘失败时返回 null，走普通错误引导。
   */
  private async salvageWriteCall(
    ctx: Pick<ToolContext, "resolvePath" | "readFile" | "writeFile" | "fileGuard">,
    raw: string,
  ): Promise<string | null> {
    const salvaged = salvageWriteArgs(raw)
    if (!salvaged) return null
    let path: string
    try {
      path = ctx.resolvePath(salvaged.path)
    } catch {
      return null
    }
    let exists = true
    try {
      await ctx.readFile(path)
    } catch {
      exists = false
    }
    if (exists && ctx.fileGuard && !ctx.fileGuard.hasRead(path)) return null
    try {
      await ctx.writeFile(path, salvaged.content)
    } catch {
      return null
    }
    ctx.fileGuard?.markRead(path, salvaged.content)
    return `工具参数 JSON 未完整生成（本次输出疑似被截断）：已将已生成的前 ${salvaged.content.length} 字符写入 ${salvaged.path}。请用 write 的 append:true 模式续写剩余内容（从已写入内容之后继续，不要重复已写入部分）。`
  }

  /** 工具参数解析失败回传文案：截断（输出上限）与普通解析失败区分引导——截断场景重新整体输出只会再次截断，
   *  引导拆小操作/分段写入。 */
  private toolArgsErrorMsg(tc: { name: string; argsError?: string }, truncated: boolean): string {
    if (truncated) {
      return `工具参数 JSON 解析失败：本次模型输出达到输出上限被截断，重新整体输出仍会被截断。请把内容拆分为更小的多次操作（大文件用 write 分段写入：首段普通 write，后续段以 append:true 续写，每段约 200~300 行）。`
    }
    return `工具参数 JSON 解析失败：模型输出的参数不是合法 JSON。原始片段: ${tc.argsError}。请重新调用 ${tc.name} 并输出合法的 JSON 参数。`
  }

  private async runLoop(params: {
    sessionId: string
    user: string
    messages: MessageLike[]
    systemPrompt: string
    registry: Pick<ToolRegistry, "schemas" | "resolve" | "getAgentNames">
    signal: AbortSignal
    env: Record<string, string>
    provider: LLMProvider
    extraParams?: Record<string, unknown>
    persist: (msg: Message) => Promise<void>
  }): Promise<{ text: string; reasoning: string; lastMessageId?: string; ctxInputTokens?: number; ctxCachedTokens?: number; ctxCountedLen: number }> {
    const { sessionId, user, messages, registry, signal, env, provider, extraParams, persist } = params
    let rounds = 0
    let lastText = ""
    let lastReasoning = ""
    // 上下文占用口径（真实 usage 为基准，估算只补未发送增量）：ctxInputTokens = 最近一次模型调用返回的
    // 真实 input tokens（含 system 与工具 schema）；ctxCountedLen = 那次调用时 messages 长度（已被真值覆盖的部分），
    // 其后的消息（增量）尚未发送，用 estimateTokens 估算补足——下一次真实调用会用真值接管；
    // 压缩重建消息后锚点失效（makeContextRoom 原地清除），真实值由下一次调用重新建立
    const ctxUsage: { ctxInputTokens?: number; ctxCachedTokens?: number; ctxCountedLen: number } = { ctxCountedLen: 0 }
    // 重复检测（DESIGN「重复检测」）：最近 MAX_REPEAT_WINDOW 次工具调用签名窗口，
    // 相同签名（工具+参数）出现 ≥MAX_REPEAT_HITS 次判定为无效重复 → 中断该次执行并注入引导提示；
    // 连续中断超过 MAX_REPEAT_STALLS 次终止循环，避免无效空转
    const recentCalls: string[] = []
    let repeatStalls = 0
    // 最终轮（无 toolCalls）的 assistantMsgId：本轮消息不在此持久化（由 run() 收口落盘），
    // 回传给 run() 用同一 id 落盘——流式增量已按该 id 推送前端，撤回/反馈对刚完成的回复立即生效
    let lastMessageId: string | undefined
    const ctx = this.buildContext(sessionId, user, env, signal, { projects: this.allPresetProjects(user, env), role: this.tasks.get(sessionId)?.role, messages, registry, registerDynamic: (def) => this.registerDynamicTool(sessionId, user, def), multimodal: provider.capabilities().multimodal }, 0)

    while (rounds < MAX_TOOL_ROUNDS) {
      if (signal.aborted) throw new Error("cancelled")
      const assistantMsgId = crypto.randomUUID()
      // 本轮推理全文累积（流式 publish 的同时落盘合并为 <think> 块，历史会话可见）
      let reasoningAcc = ""

      const schemas = registry.schemas().filter((s) => !this.isToolDisabled(sessionId, s.name, registry.resolve(s.name)?.tool))
      // 模型调用（含上下文溢出恢复：接口 4xx 上下文长度错误 = 真实大小信号，压缩后重试）
      const call = await this.callModelWithOverflowRecovery(sessionId, user, provider, messages, schemas, params.systemPrompt, signal, extraParams, ctxUsage, (chunk) => {
        // 输出方式：仅最终响应（final_only）不推送文本增量与推理流，流式输出（streaming）正常推送
        if (chunk.type === "text") {
          this.noteStream(sessionId, { messageId: assistantMsgId, text: chunk.text })
          if (this.tasks.get(sessionId)?.outputMode === "streaming") this.publish(sessionId, "event.message.delta", { text: chunk.text, messageId: assistantMsgId, sessionId })
        } else if (chunk.type === "reasoning" && chunk.text?.trim()) {
          reasoningAcc += chunk.text
          this.noteStream(sessionId, { reasoning: reasoningAcc })
          if (this.tasks.get(sessionId)?.outputMode === "streaming") this.publish(sessionId, "event.message.reasoning", { text: chunk.text, sessionId })
        }
      })
      const { text, toolCalls, usage, stopReason } = call
      lastText = text
      lastReasoning = reasoningAcc
      if (usage?.inputTokens !== undefined) {
        ctxUsage.ctxInputTokens = usage.inputTokens
        ctxUsage.ctxCachedTokens = usage.cachedTokens
        ctxUsage.ctxCountedLen = messages.length
      }
      // 上下文大小实时推送（前端会话列表展示，单位 k）：真实 usage 基准 + 未发送增量估算；
      // ctxCachedTokens = 同一次调用的提示词缓存命中（前端上下文圆环悬浮展示命中率，接口不返回时缺省）
      this.publish(sessionId, "event.session.ctx", {
        ctxTokens:
          ctxUsage.ctxInputTokens !== undefined ? ctxUsage.ctxInputTokens + estimateTokens(messages.slice(ctxUsage.ctxCountedLen)) : estimateTokens(messages),
        ...(ctxUsage.ctxInputTokens !== undefined ? { ctxCachedTokens: ctxUsage.ctxCachedTokens } : {}),
      })
      if (!toolCalls.length) {
        this.publish(sessionId, "event.message.done", { text, messageId: assistantMsgId, sessionId })
        lastMessageId = assistantMsgId
        break
      }

      normalizeToolCalls(registry, toolCalls)
      await persist({
        id: assistantMsgId,
        role: "assistant",
        content: text,
        reasoning: reasoningAcc.trim() ? reasoningAcc.trim() : undefined,
        toolCalls: toolCalls.map((tc) => ({ id: tc.id, name: tc.name, arguments: tc.arguments })),
        createdAt: Date.now(),
      })
      messages.push({ role: "assistant", content: text, toolCalls })
      this.clearStream(sessionId) // 本轮文本已随 assistant(toolCalls) 持久化，在途快照清空

      // 任务中途上下文腾挪（真实 usage 驱动）：本次调用返回的真实 input tokens 超过窗口阈值
      // （80%）时压缩最早历史，防止长任务继续膨胀——已持久化的 assistant 消息随 loadHistory
      // 回到重建后的消息列表（同序），后续工具结果照常追加，会话语义不受影响；
      // 压缩无效时硬护栏降级受保护消息（历史图片降级/最旧用户消息裁剪，原文不丢）
      const cap2 = provider.capabilities().maxContextTokens
      if (cap2 > 0 && ctxUsage.ctxInputTokens !== undefined && ctxUsage.ctxInputTokens > cap2 * COMPACT_RATIO) {
        await this.makeContextRoom(sessionId, user, provider, messages, params.systemPrompt, ctxUsage)
      }

      // 重复判定后终止循环：本轮剩余工具调用跳过（保持 tool 消息序列完整），随后退出
      let stopped = false
      // assistant(toolCalls) 先于执行落盘：任何中断路径（取消/审批等待取消/异常）都必须为本轮全部
      // toolCalls 补齐 tool 结果（含占位说明），否则历史留下未应答 toolCalls——严格校验的 LLM 接口
      // （OpenAI tool_calls/tool 配对）会让该会话后续每次请求都被 400 拒绝
      const toolCallDone = new Set<string>()
      // 同批并行执行的结果按完成先后落盘：appendMessage（load→push→save 共享缓存会话对象）不容忍并发
      // 调用（save 交错可能以旧覆新），promise 链串行化每次落盘；顺序不影响接口合法性（配对按 toolCallId）
      let persistChain = Promise.resolve()
      const persistTool = async (tc: { id: string; name: string }, content: string, extra: Partial<Message> = {}, imageBlocks?: Array<Record<string, unknown>>) => {
        const write = persistChain.then(() => persist({ id: crypto.randomUUID(), role: "tool", content, toolCallId: tc.id, name: tc.name, createdAt: Date.now(), ...extra }))
        persistChain = write.catch(() => {})
        await write
        toolCallDone.add(tc.id)
        // 多模态工具结果（read 图片内联）：图片块随文本块并入 tool 消息内容数组（provider 序列化为对应形态）
        messages.push({ role: "tool", content: imageBlocks?.length ? [{ type: "text", text: content }, ...imageBlocks] : content, toolCallId: tc.id, name: tc.name })
      }
      const fillMissingToolResults = async (note: string) => {
        for (const rest of toolCalls) {
          if (toolCallDone.has(rest.id)) continue
          try {
            await persistTool(rest, note)
            // 补写占位同样推送结果事件：实时卡片从「执行中」落为终态（无配对调用由前端兜底独立结果卡）
            this.publish(sessionId, "event.tool.result", { name: rest.name, toolCallId: rest.id, output: note, sessionId })
          } catch {
            /* 补写失败不掩盖原错误 */
          }
        }
      }
      // 门控说明性结果（不执行）同样推送 call+result 事件对：前端实时建卡，与落盘历史一致——
      // 否则该调用只在刷新后可见（历史有落盘、运行时无事件），并行批次下门控结果与执行结果混排时缺卡明显
      const persistGatedNote = async (tc: (typeof toolCalls)[number], note: string) => {
        this.publish(sessionId, "event.tool.call", { name: tc.name, arguments: tc.arguments, toolCallId: tc.id })
        await persistTool(tc, note)
        this.publish(sessionId, "event.tool.result", { name: tc.name, toolCallId: tc.id, output: note, sessionId })
      }
      try {
        if (signal.aborted) {
          // 取消路径：为尚未执行的调用补写终止记录后中止（保持 assistant/tool 配对完整）
          await fillMissingToolResults("任务已取消：该工具调用未执行。")
          throw new Error("cancelled")
        }
        // ── 门控阶段（按调用顺序串行）：重复检测/参数抢救/路由解析（含自动装载）/审批姿态等判定
        //    全部先行完成。说明性结果（缺参引导/未知工具/禁用/安全拦截/无交互拒绝）在此直接落盘；
        //    校验点在审批门之前（坏调用不打扰用户审批）。可执行项收集进入并行阶段
        const pending: Array<{ tc: (typeof toolCalls)[number]; rt: NonNullable<ReturnType<ToolRegistry["resolve"]>>; autoLoaded: string; approvalRequired: boolean }> = []
        // 同批重复签名只检测/记录一次（DESIGN「同批工具并行执行」）：同批并行发出相同调用是有意扇出
        // ——发出时尚未见任何结果，不存在「无视结果重试」；跨轮重复（已见过结果仍重发同签名）照常累积判定
        const batchSignatures = new Set<string>()
        for (const tc of toolCalls) {
          if (stopped) {
            await persistGatedNote(tc, "任务已中止：模型持续重复相同工具调用。")
            continue
          }
          const signature = toolCallSignature(tc.name, tc.arguments)
          if (!batchSignatures.has(signature)) {
            batchSignatures.add(signature)
            if (this.repeatedCall(recentCalls, tc.name, tc.arguments)) {
              // 相同工具+相同参数已执行 ≥MAX_REPEAT_HITS 次：中断执行（结果必然相同），注入提示引导模型换方向
              repeatStalls++
              const note = `已中断重复的工具调用 ${tc.name}：参数与之前完全相同，重复执行只会得到相同结果。请分析原因、改用其他方法，或直接给出最终回答，不要重复相同操作。`
              if (repeatStalls > MAX_REPEAT_STALLS) stopped = true
              await persistGatedNote(tc, note)
              continue
            }
          }
          // 工具参数不是合法 JSON（接口聚合失败/输出被截断）：不执行（以 {} 执行会做出错误行为）。
          // write 类工具先尝试抢救落盘已生成内容（失败轮次变进度，模型 append 续写）；未抢救走错误引导
          // （截断与普通解析失败区分文案）
          if (tc.argsError) {
            const salvaged = this.isWriteToolName(tc.name) ? await this.salvageWriteCall(ctx, typeof tc.raw === "string" ? tc.raw : "") : null
            await persistGatedNote(tc, salvaged ?? this.toolArgsErrorMsg(tc, this.isTruncatedStop(stopReason)))
            continue
          }
          let rt = registry.resolve(tc.name)
          let autoLoaded = ""
          if (!rt) {
            // 路由自愈（DESIGN「子Agent 路由」）：模型直接调用了未装载子Agent 的 {agent}_* 工具——
            // 按 agent_load 同路径自动装载（注册工具 + 提示词注入会话记录与当前上下文）后重解析执行，
            // 一次可恢复的调用不再以「未知工具」失败；仍不可解析（拼错工具名等）给出该子Agent 可用工具清单
            const agent = this.subAgentForToolName(tc.name)
            if (agent) {
              await ctx.loadSubAgent(agent).catch(() => {})
              rt = registry.resolve(tc.name)
              if (rt) autoLoaded = agent
            }
          }
          if (!rt) {
            await persistGatedNote(tc, this.unknownToolMsg(tc.name))
            continue
          }
          const missing = missingRequiredArgs(rt.tool, tc.arguments)
          if (missing.length) {
            // 必填参数缺失（模型漏传）：不执行，明确报缺什么参数（防缺失值进工具被解析成字面量 "undefined"）
            await persistGatedNote(tc, missingArgsMsg(rt.name, missing))
            continue
          }
          if (this.isToolDisabled(sessionId, rt.name, rt.tool)) {
            // 通道禁用工具（DESIGN「飞书机器人集成」）：模型不应调用，被调用时阻止执行并说明原因
            await persistGatedNote(tc, this.toolDisabledMsg(sessionId, rt.name))
            continue
          }
          if (this.isRiskyInSafeMode(rt.name)) {
            // 安全模式（DESIGN「安全模式」）：风险工具（命令执行/写删文件/定时任务调度）不执行、不弹审批，
            // 直接返回限制信息给模型（模型仍可见 schema，可据此改用只读方案）
            await persistGatedNote(tc, safeModeRestrictionMsg(rt.name))
            continue
          }
          const requiresByArgs = await toolRequiresApproval(rt.tool, tc.arguments, ctx)
          const approvalSkipped = await this.isApprovalSkipped(sessionId, user, env)
          // 无交互通道硬门槛（服务模式默认 / 请求级 autoApprove=false）：无人可审批，默认需审批的工具
          // 直接拒绝（防普通用户经 REST 免审批执行敏感工具）；approval:false 只放宽交互审批——硬门槛按
          // 剥离免审标记后的审批姿态解析（js 嵌套调用同规则），防模型自行声明免审绕过
          if (
            this.noInteractionHardGate(sessionId) && this.tasks.get(sessionId)?.interactionMode === "none" && !approvalSkipped &&
            (requiresByArgs || (await toolRequiresApproval(rt.tool, stripApprovalFlags(tc.arguments) as Record<string, unknown>, ctx)))
          ) {
            await persistGatedNote(tc, this.noInteractionDenied(rt.name))
            continue
          }
          pending.push({ tc, rt, autoLoaded, approvalRequired: requiresByArgs && !approvalSkipped })
        }
        // ── 执行阶段（同批工具并行执行，DESIGN「同批工具并行执行」）：每个调用独立走「审批等待 →
        //    执行 → 落盘 → 事件推送」，互不阻塞（免审批工具不等待同批审批项）；并发上限
        //    MAX_PARALLEL_TOOLS，超出按序排队。单个调用出错不中断其他调用（取消/异常由 firstError
        //    收口，池排空后统一补齐占位再上抛，与原串行语义一致）
        let firstError: unknown
        await runToolPool(pending, MAX_PARALLEL_TOOLS, async ({ tc, rt, autoLoaded, approvalRequired }) => {
          try {
            if (signal.aborted) return // 未及启动即取消：占位由收口补写
            if (approvalRequired) {
              const retries = this.tasks.get(sessionId)?.retries.get(tc.id) ?? 0
              this.publish(sessionId, "event.tool.call", { name: tc.name, arguments: tc.arguments, toolCallId: tc.id, requiresApproval: true })
              this.publish(sessionId, "event.approval.request", { toolCallId: tc.id, tool: tc.name, retries, arguments: tc.arguments })
              const verdict = await this.waitApproval(sessionId, tc.id, rt.name, signal)
              // 取消路径（用户停止，cancelled 标记）：等待被信号解开时立即中止，不写「用户拒绝」虚假记录；
              // 显式拒绝：落盘拒绝消息后由下一轮 abort/循环检查结束；超时：落盘超时提示，模型可继续调整
              if (signal.aborted && this.tasks.get(sessionId)?.cancelled) throw new Error("cancelled")
              if (verdict !== "approved") {
                this.tasks.get(sessionId)?.retries.set(tc.id, retries + 1)
                const denied =
                  verdict === "timeout"
                    ? `工具调用 ${tc.name} 审批等待超时（5 分钟未响应），已跳过该调用。请调整方案，或先向用户说明需要审批的操作。`
                    : `工具调用 ${tc.name} 已被用户拒绝。请调整方案后重试，或改用其他方法。`
                await persistTool(tc, denied)
                // 拒绝/超时同样推送结果事件：实时卡片落终态（tool.call 已随审批请求发出，卡片已存在）
                this.publish(sessionId, "event.tool.result", { name: tc.name, toolCallId: tc.id, output: denied, sessionId })
                return
              }
            } else {
              this.publish(sessionId, "event.tool.call", { name: tc.name, arguments: tc.arguments, toolCallId: tc.id })
            }

            this.publish(sessionId, "event.tool.result.start", { name: tc.name, toolCallId: tc.id })
            // 取消/超时统一收口：停止按钮中断执行（脚本进程同步被杀），超时作为结果返回模型不结束任务
            const result = await this.runToolInterruptible(rt.tool, tc.arguments, ctx, signal, rt.name, sessionId, tc.id)
            // 兜底截断（不依赖工具自觉）：工具未自行截断的超长输出统一截断落盘，防上下文爆炸；
            // 结构化 data 与存档扩展字段原样保留（截断只作用于模型可见文本）
            const safe = !result.truncated && result.output.length > TRUNCATE_THRESHOLD
              ? { ...(await truncate(result.output, rt.name, ctx)), blocks: result.blocks, data: result.data, sessionRun: result.sessionRun, images: result.images }
              : result
            // 多模态工具结果图片（read 等读取的图片）：主模型多模态时内联进 tool 消息；轻量引用
            // （path/display/mime，不含 base64）随消息落盘，loadHistory 历史重建按引用重读内联
            const imageBlocks = provider.capabilities().multimodal && safe.images?.length ? await this.toolImageBlocks(safe.images) : []
            const imageRefs = safe.images?.length ? { images: safe.images.map(({ path, display, mime }) => ({ path, ...(display ? { display } : {}), mime })) } : {}
            await persistTool(tc, autoLoaded ? `（引擎已自动装载子Agent ${autoLoaded}——其工具已并入当前工具集、提示词已注入上下文）\n${safe.output}` : safe.output, { blocks: safe.blocks, arguments: tc.arguments, sessionRun: safe.sessionRun, ...imageRefs }, imageBlocks)
            this.publish(sessionId, "event.tool.result", {
              name: tc.name,
              toolCallId: tc.id,
              truncated: !!safe.truncated,
              filePath: safe.filePath,
              output: safe.output,
              blocks: safe.blocks,
              sessionId,
            })
          } catch (err) {
            if (firstError === undefined) firstError = err
          }
        })
        if (firstError !== undefined) throw firstError
      } catch (err) {
        // 中断兜底：为本轮缺失结果的 toolCalls 补占位（幂等——已写过的由 toolCallDone 跳过）
        await fillMissingToolResults("任务中断：该工具调用未执行完成。")
        throw err
      }
      // 分支合并排空（DESIGN「会话分支运行与合并」）：本轮工具结果落盘后追加已完成分支的报告——
      // 位于 tool 消息之后（tool_calls 配对完整），主线下轮模型调用即见合并内容
      await this.drainBranchMerges(sessionId, user, persist, messages)
      rounds++
      if (stopped) break
    }
    return { text: lastText, reasoning: lastReasoning, lastMessageId, ctxInputTokens: ctxUsage.ctxInputTokens, ctxCachedTokens: ctxUsage.ctxCachedTokens, ctxCountedLen: ctxUsage.ctxCountedLen }
  }

  /**
   * 重复调用检测（DESIGN「重复检测」）：将本次工具调用签名（工具名+参数 JSON）记入滚动窗口，
   * 窗口内相同签名已出现 MAX_REPEAT_HITS-1 次（本次为第 MAX_REPEAT_HITS 次）时判定为重复。
   * 调用方负责同批去重（同批重复签名只检测/记录一次——同批并行发出相同调用是有意扇出，见「同批工具并行执行」）。
   */
  private repeatedCall(window: string[], name: string, toolArguments: Record<string, unknown>): boolean {
    const sig = toolCallSignature(name, toolArguments)
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
   * 安全模式硬阻断判定（GEBAI_SAFE_MODE=true 启动时加载，DESIGN「安全模式」）：
   * 仅定时任务调度类（cron_add/update/remove 及短名命中）无法降级被阻断；
   * sh/py/js/write 等风险工具在各自 execute 内降级（白名单/审计钩子/只读 shim/写范围），引擎不拦截。
   * 拦截语义与通道禁用不同：模型仍可见该工具 schema，调用时被阻止并返回限制信息（模型可据此调整方案）。
   */
  private isRiskyInSafeMode(name: string): boolean {
    if (!this.opts.config.safeMode) return false
    return isToolBlockedInSafeMode(name)
  }

  private isToolDisabled(sessionId: string, name: string, tool?: Tool): boolean {
    const task = this.tasks.get(sessionId)
    if (!task) return false
    if (task.disabledTools.some((d) => name === d || name.endsWith(`_${d}`))) return true
    if (task.enabledTools) {
      // 极简模式（DESIGN「极简模式」）：白名单外工具一律禁用（schema 过滤 + 执行阻止）
      if (!task.enabledTools.includes(name)) return true
    } else if (name === "full_mode") {
      // 完整模式（极简未启用/已切换）：full_mode 仅极简会话可见可用，其余会话从 schema 移除（防冗余工具干扰选择）
      return true
    }
    if (tool?.interaction) {
      const level: Record<InteractionMode, number> = { none: 1, multi_turn: 2, realtime: 3 }
      if (level[tool.interaction] > level[task.interactionMode]) return true
    }
    return false
  }

  /** 工具禁用原因说明（isToolDisabled 为真时取消息）：极简模式白名单 / 通道禁用（含交互模式不足）。 */
  private toolDisabledMsg(sessionId: string, name: string): string {
    const task = this.tasks.get(sessionId)
    if (task?.enabledTools && !task.enabledTools.includes(name)) {
      return `工具 ${name} 在当前会话不可用：会话处于极简模式，仅启用 sh 与 edit 两个工具。请改用 sh/edit 完成，或调用 full_mode 工具（需用户批准）切换到完整模式。`
    }
    if (name === "full_mode" && !task?.enabledTools) return "会话已是完整模式（全部工具可用），无需切换。"
    return `工具 ${name} 在当前通道不可用（该工具需要前端页面配合，而当前会话来自飞书聊天），请改用其他方式。`
  }

  /**
   * 工具执行包装（取消/超时统一收口）：
   * - 任务取消（停止按钮）：立即返回「已取消」；脚本类工具经 runCommand 传递的任务信号
   *   会同步杀进程（Sandbox.exec 的 signal 支持），因此真正执行的子进程会被打断
   * - 执行超时（TOOL_TIMEOUT_MS）：不结束任务，把「执行超时」作为工具结果返回给模型，
   *   由模型决定调整方案重试（脚本先由 sandbox 自身超时杀进程，此兜底覆盖挂起的非脚本工具）
   * - 工具异常：转为「工具执行失败」结果（与原有行为一致）
   */
  private runToolInterruptible(tool: Tool, args: Record<string, unknown>, ctx: ToolContext, signal: AbortSignal, name: string, sessionId: string, toolCallId: string): Promise<ToolResult> {
    if (signal.aborted) return Promise.resolve({ output: `工具 ${name} 已取消（任务已停止）` })
    return new Promise<ToolResult>((resolve) => {
      let done = false
      let timer: ReturnType<typeof setTimeout>
      let onAbort: () => void
      // 长工具心跳：执行期间按 heartbeatMs 周期发布（快工具在首个周期前结束，不产生事件）——
      // 前端空闲看门狗据此刷新活跃，阻塞类工具（sh/py 长命令）不被误判挂起取消
      const hb = setInterval(() => {
        this.publish(sessionId, "event.tool.alive", { name, toolCallId })
      }, this.opts.heartbeatMs ?? TOOL_HEARTBEAT_MS)
      const finish = (r: ToolResult) => {
        if (done) return
        done = true
        clearTimeout(timer)
        clearInterval(hb)
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
        (r) => {
          this.trackTaskMods(sessionId, name, args, r)
          finish(r)
        },
        (err) => finish({ output: `工具执行失败: ${(err as Error).message}` }),
      )
    })
  }

  /** 收尾验证提醒数据收集：write/edit/patch 成功修改代码文件（拒绝/安全模式拦截与 dryRun 不计）记入文件清单；
   *  sh/py 执行测试/检查类命令、run_tests 工具调用即标记已验证。名称取命名空间短名（code_write 等同权）。 */
  private trackTaskMods(sessionId: string, name: string, args: Record<string, unknown>, result: ToolResult): void {
    const mods = this.taskMods.get(sessionId)
    if (!mods) return
    const short = name.includes("_") ? name.slice(name.lastIndexOf("_") + 1) : name
    if (short === "write" || short === "edit" || short === "patch") {
      if (typeof args.path !== "string") return
      if (args.dry_run === true || args.dryRun === true || result.output.includes("预演")) return
      if (MOD_REJECTED_RE.test(result.output)) return
      if (VERIFY_CODE_FILE_RE.test(args.path)) mods.files.add(args.path)
    } else if (short === "sh" || short === "py") {
      if (VERIFY_CMD_RE.test(String(args.command ?? ""))) mods.verified = true
    } else if (short === "run_tests") {
      mods.verified = true
    }
  }

  /** agent_run 预加载清单校验与规范化：去重、依赖级联展开（cascade：依赖在前，与装载模式
   *  SubAgentManager.load 的依赖自动装载同规则——reverse_site 连带 playwright、self_optimize 连带 code 等）、
   *  数量与深度上限、未知名检查。同步（runNewSession）与异步（SessionRunRegistry.start）启动共用——
   *  异步启动在登记句柄前同步校验，未知名等错误立即抛给模型。 */
  private normalizeRunAgents(agents: string[], depth: number): string[] {
    const requested = [...new Set(agents)]
    // 未知名检查针对用户给定名单（依赖展开会跳过缺失依赖——用户名单错字仍须报错而非静默丢失）
    for (const name of requested) {
      // 未知子Agent 附加载失败原因（有文件但 import 抛错/缺 def 导出时引导精确修复——self_optimize 自修复闭环）
      if (!this.opts.subAgents.def(name)) throw new Error(`未知子Agent: ${name}${this.opts.subAgents.loadError(name) ? `（其文件加载失败: ${this.opts.subAgents.loadError(name)}——修复该文件后即可执行）` : ""}`)
    }
    const out: string[] = []
    for (const name of requested) {
      for (const n of this.opts.subAgents.cascade(name)) if (!out.includes(n)) out.push(n)
    }
    if (out.length > MAX_AGENTS_PER_RUN) throw new Error(`子Agent 数量超限（${out.length} > ${MAX_AGENTS_PER_RUN}）`)
    if (depth >= SUBAGENT_DEPTH) throw new Error(`子Agent 递归深度超限: ${out.join(",")}`)
    return out
  }

  /** 新会话执行（agent_run 工具）：派生临时新会话，预加载指定子Agent 列表（完整系统提示词拼接+工具并入，
   *  模块语义的「装载」在独立上下文生效）后执行任务，返回最终结果与完整存档（DESIGN「装载 vs 新会话执行」）。
   *  inheritGlobalTools（默认 true）：全局工具一并注册进新会话——与主会话同构的完整工具面（文件读写查询
   *  统一用全局工具，子Agent 只带独有能力）；关闭时仅预加载子Agent 工具 + 内建编排（tool_schemas/js）。
   *  inheritGlobalPrompt（默认 true）：总Agent 全局系统提示词（buildSystemPrompt——身份/行为约定/编排指引）
   *  作为新会话系统提示词前缀注入——与全局工具继承默认一致，新会话与主会话同构；关闭时仅子Agent 提示词
   *  （上下文最省）。提示词中的路径/工具可用性描述以新会话实际为准（附注说明）。
   *  onArchive：存档创建即回调（异步运行 registry 持活引用推导进度；同步路径不传）。 */
  private async runNewSession(
    sessionId: string,
    user: string,
    env: Record<string, string>,
    agents: string[],
    input: string,
    signal: AbortSignal,
    depth = 0,
    opts: { inheritGlobalTools?: boolean; inheritGlobalPrompt?: boolean; onArchive?: (archive: SessionRunArchive) => void } = {},
  ): Promise<{ output: string; archive: SessionRunArchive }> {
    // 前置热加载重扫（与 load() 同规则）：self_optimize 刚写完/修好子Agent 文件即 agent_run——
    // 校验（normalizeRunAgents）只读当前注册表，不重扫会以旧缓存误报「未知子Agent」（无加载失败附言）
    // 或沿用修复前的旧定义
    await this.opts.subAgents.refreshIfChanged().catch(() => {})
    agents = this.normalizeRunAgents(agents, depth)
    const defs = agents.map((name) => this.opts.subAgents.def(name)!)
    const inheritGlobals = opts.inheritGlobalTools !== false
    // 新会话工具注册：每个预加载子Agent 的独有工具以 {agent}_ 命名空间并入（装载语义）；
    // 安全模式与主注册表同规则（Tool.safeMode 自主声明过滤，引擎构造期常量）
    const reg = new BaseToolRegistry({ safeMode: this.opts.config.safeMode })
    let orchestrationInjected = false
    for (const def of defs) {
      reg.registerSubAgentTools(def.name, def.tools ?? {}, def.requiresApproval)
      // 简化定义（无工具，含纯 md 定义）：注入编排工具（原名暴露，无 {agent}_ 前缀，多 Agent 时仅注入一次）
      // ——支持组合式子 Agent 通过 agent_run/bg_task/agent_list/agent_load 编排其他子 Agent
      if (!def.tools || Object.keys(def.tools).length === 0) {
        if (!orchestrationInjected) {
          reg.register(agentListTool)
          reg.register(agentLoadTool)
          reg.register(agentRunTool)
          reg.register(bgTaskTool)
          orchestrationInjected = true
        }
      }
    }
    // 编排能力（与总Agent 主循环一致）：新会话内同样可用 tool_schemas 批量查询工具输出结构、
    // js 脚本动态编程（直接调用工具 + 会话上下文注入）——构建期排除清单（GEBAI_BUILD_EXCLUDE_TOOLS）同规则生效
    if (!isGlobalToolExcluded("tool_schemas")) reg.register(toolSchemasTool)
    if (!isGlobalToolExcluded("js")) reg.register(jsTool)
    // 全局工具继承（默认开启，DESIGN「新会话执行的上下文隔离」）：read/write/grep/sh 等全局工具（含
    // project 参数路由）一并注册——子Agent 不再重复定义文件工具，新会话与主会话工具面同构；
    // 已注册的名字跳过（子Agent 独有工具的 {agent}_ 前缀名与全局名不冲突，防御性去重）
    if (inheritGlobals) {
      for (const tool of Object.values(createGlobalTools())) {
        if (reg.resolve(tool.name)) continue
        reg.register(tool)
      }
      // vision 同为全局工具但不在 createGlobalTools 内（vision→tools 的 truncate 依赖会成环，注册在 index.ts）：
      // 新会话继承全局工具时一并注册，与主注册表同源同款（裁剪排除/已注册跳过同规则）
      if (!isGlobalToolExcluded("vision") && !reg.resolve("vision")) reg.register(makeVisionTool({ vision: getVisionProvider }))
    }

    // 系统提示词：各预加载子Agent 的完整系统提示词拼接 + 各自的项目注记（项目内置/预置项目/受限模式/AGENTS.md）；
    // 每个子Agent 前加职责分隔头（名称 + 能力描述），明确各段提示词对应的工具命名空间与职责域，多 Agent 预加载时不混淆
    const systemParts: string[] = []
    const mergedPresets: PresetProject[] = []
    const seen = new Set<string>()
    let baseProjectRoot: string | undefined
    for (const def of defs) {
      // 项目内置（特定项目绑定）：会话环境变量 {AGENT_NAME_UPPER}_PROJECT（如 CODE_PROJECT）指定子Agent 的项目根
      // —— 工作目录与路径解析以项目根为基准，系统提示词注入项目路径
      baseProjectRoot ??= this.resolveSubAgentProject(user, env, def.name)
      systemParts.push(await this.buildAgentSection(def, user, env, sessionId))
      // 预置项目全量合并（同名去重）：多 Agent 预加载时 project 参数路由均可用
      for (const p of this.presetProjectsFor(user, env, def.name)) {
        if (!seen.has(p.name)) {
          seen.add(p.name)
          mergedPresets.push(p)
        }
      }
    }
    const safeNote = this.opts.config.safeMode
      ? `\n安全模式已启用（风险能力降级而非禁用）：sh 仅允许只读命令白名单；py/js 为只读运行时（写文件/子进程/网络屏蔽，仅保留文件读取）；write/edit/patch/file 限定用户目录内；定时任务调度（cron_*）不可用；部分子Agent 风险工具未注册。`
      : ""
    const globalsNote = inheritGlobals
      ? `全局工具已继承进本会话（read/write/edit/patch/ls/grep/glob/file/diff/sh/py/fetch_url/todo/ask/agent_run 等，与主会话同名同参——文件工具可用 project 参数路由项目，未传时相对路径以${baseProjectRoot ? "项目根" : "会话工作目录"}为基准）；预加载子Agent 只提供独有工具（以 {agent}_ 前缀调用）。`
      : `本会话未继承全局工具（inherit_global_tools=false）：仅预加载子Agent 的工具（以 {agent}_ 前缀调用）与内建编排（tool_schemas/js）。`
    // 全局提示词注入（默认开启，与 inherit_global_tools 默认一致）：总Agent 主系统提示词作为前缀（单源复用
    // buildSystemPrompt，不复刻）；其中路径基准/工具清单等环境描述以本新会话实际为准，附注消歧
    const inheritGlobalPrompt = opts.inheritGlobalPrompt !== false
    const globalPromptPart = inheritGlobalPrompt
      ? `以下为总Agent 全局系统提示词（主会话行为约定与全局能力说明；路径基准与工具可用性以本会话上文为准）:\n${this.buildSystemPrompt(sessionId, user, env)}\n\n`
      : ""
    // 编排指引（js 优先）防重复注入：注入全局提示词时其编排段已含同款内容，开场白不再复述；
    // 仅 inherit_global_prompt=false（新会话无 buildSystemPrompt）时保留兜底版
    const orchestrationNote = inheritGlobalPrompt
      ? ""
      : "可预判的多步固定流程优先用 js 脚本动态编程一次执行（脚本内工具像内置函数一样直接 await read(params) 调用、ctx 注入会话上下文，可用 tool_schemas 批量查询工具输出结构，语法详见 js 工具描述）；纯系统操作也可编写脚本（sh/py）一次执行——避免大量单步工具调用浪费往返与词元。"
    const messages: MessageLike[] = [
      {
        role: "system",
        content: `你正在一个临时新会话中执行任务（与主会话隔离，执行过程不进入主上下文）。已预加载子Agent: ${agents.join(", ")}，其完整系统提示词如下。\n${globalsNote}${orchestrationNote ? `\n${orchestrationNote}` : ""}${safeNote}\n\n${globalPromptPart}${systemParts.join("\n\n")}`,
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
    // 存档创建即回调（异步运行 registry 持活引用，进度快照随执行实时推导；同步路径不传）
    opts.onArchive?.(archive)
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

  /**
   * 分支运行（branch_run 工具，DESIGN「会话分支运行与合并」）：从主会话**当前上下文** fork 派生分支——
   * 同一系统提示词（+分支附注）、同一消息历史（fork 快照）、同一工具面（会话注册表视图快照，含已装载
   * 子Agent 与动态工具），独立 LLM 循环并行执行（复用 runNewSessionLoop：审批/重复检测/存档/流式推送同构）。
   * spec.model 命中模型路由（resolveModelProvider，GEBAI_LLM_ROUTES 多路接口）时走独立 Provider——
   * 多分支多端点并行，摆脱单轮串行的模型服务速度限制。完成后报告经注册表 onDone（trunkMerge 最终合并）
   * 自动合入主上下文；本方法只负责执行与存档。
   */
  private async runBranch(
    sessionId: string,
    user: string,
    env: Record<string, string>,
    spec: BranchSpec & { branchId: string },
    signal: AbortSignal,
    forkMessages: MessageLike[],
  ): Promise<{ output: string; archive: SessionRunArchive }> {
    // 工具面快照：主会话当前注册表视图逐项注册进本次分支注册表（分支内装载子Agent 只进本分支——
    // loadAgentIntoRun 隔离语义，不写主会话记录/全局注册表；禁用态工具不进快照）；
    // 另注册 branch_sync（分支与主干双向同步的唯一工具，DESIGN「会话分支运行与合并」互相感知）——仅分支上下文可见
    const view = this.sessionRegistry(sessionId)
    const reg = new BaseToolRegistry({ safeMode: this.opts.config.safeMode })
    for (const s of view.schemas(false)) {
      const rt = view.resolve(s.name)
      if (rt && rt.enabled !== false) reg.register(rt.tool, (rt as { agent?: string }).agent)
    }
    reg.register(branchSyncTool)
    // 已读追踪 fork 快照（防误覆盖/防陈旧覆盖的分支隔离）：拷贝 fork 点会话级已读表——分支已读基线 =
    // fork 时主线可见内容；此后主线/兄弟分支的读写互不串扰（跨流盲写/陈旧覆盖由指纹漂移拦截，
    // fork 后他人读过的文件不视为本分支已读）。须在任何 await 前拷贝（fork 点同步语义）
    const branchReads = new Map(this.readFiles.get(sessionId) ?? [])
    // fork 点主干水位（branch_sync 增量基准）：以存储消息数为准——runLoop 边落盘边推进，
    // 此刻存储尾部即 fork 快照的持久化等价（在途 tool 结果尚未落盘，不属主干可见内容）。首个 await 后
    // 句柄必已登记（registry.start 在启动 runner 后同步 store.set）
    const forkSession = await this.opts.store.load(sessionId, user)
    const forkHandle = this.branchRunStore.get(spec.branchId)
    if (forkHandle) forkHandle.forkAt = forkSession?.messages.length ?? 0
    // fork 快照补齐：主线在 branch_run 工具调用内派生——快照尾部带尚未应答的 assistant(toolCalls)
    // （本轮工具批处理进行中）。严格校验的接口（tool_calls 后必须紧跟 tool 响应）会 400，
    // 为悬空 toolCall 合成占位结果（紧随已有 tool 结果之后，配对完整）
    const fork = [...forkMessages]
    const lastToolCalls = [...fork].reverse().find((m) => m.role === "assistant" && m.toolCalls?.length)
    if (lastToolCalls?.toolCalls?.length) {
      const answered = new Set(fork.filter((m) => m.role === "tool").map((m) => m.toolCallId))
      for (const tc of lastToolCalls.toolCalls) {
        if (!answered.has(tc.id)) fork.push({ role: "tool", content: "（主线已派生并行分支，该工具调用结果不在本分支上下文中）", toolCallId: tc.id, name: tc.name })
      }
    }
    // 分支系统提示词：主会话同一系统提示词（单源复用 buildSystemPrompt）+ 分支附注（并行职责/并发写提醒/
    // 双向同步与互相感知——branch_sync 交出阶段性成果/拉取主干增量，主干与其他分支的进展以通知注入本上下文）
    const modelNote = spec.model ? `，模型路由 ${spec.model}` : ""
    const branchAddendum = `\n\n【并行分支运行】你是主会话当前上下文派生的并行分支「${spec.name}」${modelNote}，与其他分支、主线同时执行。请专注完成本分支任务，可使用全部工具；其他分支可能并行修改同一文件，写入前先读取最新内容。分支内不要向用户提问（ask 类交互不适用分支），需审批的工具照常走审批。协作感知（唯一工具 branch_sync，双向）：传 content 即交出阶段性成果（立即合入主干并广播其他分支，本分支继续执行，可多次）；不传即拉取主干完整增量（主线输入/回复、其他分支合入全文）；两种用法都返回主干新进展——合入通知（【分支感知】/【主线进展】）只是摘要，需要完整内容时就调 branch_sync；收到进展后据此调整分工，避免重复工作或冲突。完成后直接输出最终报告（结论/产物/关键发现），报告将合并回主会话主线——不要输出与分支任务无关的内容。`
    const messages: MessageLike[] = [
      { role: "system", content: `${this.buildSystemPrompt(sessionId, user, env)}${branchAddendum}` },
      ...fork.slice(1),
      { role: "user", content: spec.prompt },
    ]
    // 模型路由（多路接口）：spec.model 命中路由走独立 Provider（resolveModelProvider 未注入/未命中回落任务级）
    const taskProvider = this.opts.resolveProvider?.(env) ?? this.opts.provider
    const provider = spec.model ? (this.opts.resolveModelProvider?.(env, spec.model) ?? taskProvider) : taskProvider
    // 分支存档（SessionRunArchive，branch 字段标识；runId = branchId——事件路由/存档关联/bg_task 一致）
    const archive: SessionRunArchive = {
      runId: spec.branchId,
      agents: [],
      input: spec.prompt,
      output: "",
      messages: [{ role: "user", content: spec.prompt }],
      branch: { name: spec.name, ...(spec.model ? { model: spec.model } : {}) },
    }
    this.publish(sessionId, "event.session.start", { runId: spec.branchId, agents: [], input: spec.prompt, depth: 1, branch: spec.name, ...(spec.model ? { model: spec.model } : {}), sessionId })
    try {
      const output = await this.runNewSessionLoop(sessionId, user, env, [], spec.prompt, messages, reg, signal, 1, archive, {
        provider,
        branch: archive.branch,
        fileGuardMap: branchReads,
        // 主干通知收件箱（句柄惰性查找——回调每轮调用时句柄必已在引擎级表登记）
        drainInbox: () => {
          const h = this.branchRunStore.get(spec.branchId)
          const out = h?.inbox ?? []
          if (h) h.inbox = []
          return out
        },
        // 分支与主干双向同步（branch_sync 工具 → ctx.branchSync，分支唯一同步工具）：传 content 先合入
        // 主干（trunkMerge 阶段性路径），随后统一返回主干增量快照（合入与拉取同径，天然合入即感知）
        branchSync: async (content) => {
          if (content !== undefined) {
            const h = this.branchRunStore.get(spec.branchId)
            if (h) await this.trunkMerge(sessionId, user, env, h, { final: false, content })
          }
          return await this.trunkSnapshot(sessionId, user, spec.branchId)
        },
      })
      archive.output = output
      return { output, archive }
    } catch (err) {
      this.publish(sessionId, "event.session.done", { runId: spec.branchId, agents: [], output: "", error: String((err as Error).message || err), branch: spec.name, sessionId })
      throw err
    }
  }

  /** 分支报告合入主上下文（DESIGN「会话分支运行与合并」）：最终合并（注册表 onDone）与阶段性合入
   *  （branch_sync 传 content）的统一实现——构造合并消息（assistant，内容带分支头行自描述 + branchMeta；
   *  最终合并携带完整过程存档 sessionRun，阶段性不带——分支仍在执行、存档为活引用）入主干、推送
   *  event.branch.merged（前端实时渲染合并气泡）、广播通知其他运行中分支（互相感知）。
   *  final=true 标记句柄已合入（bg_task 状态展示）；interimNote 供阶段性通知文案。
   *  merge=summary（合入粒度）：超阈值报告先经任务级模型摘要（summarizeBranchReport）再合入——失败全文兜底；
   *  异步方法（摘要含模型调用），同步 fan-out 经注册表 done promise 等待合入完成（结果前排空）。 */
  private async trunkMerge(sessionId: string, user: string, env: Record<string, string>, handle: BranchRunHandle, opts: { final: boolean; content: string }): Promise<void> {
    if (opts.final) handle.merged = true
    let raw = opts.content.trim() || "（分支已完成，无最终文本输出）"
    // 摘要合入（merge=summary）：短报告不值得一次模型调用，原文合入；全文保留在分支过程存档/bg_task
    let summarized = false
    if (handle.mergeMode === "summary" && raw.length > BRANCH_MERGE_SUMMARY_SKIP_CHARS) {
      const summary = await this.summarizeBranchReport(raw, env)
      if (summary && summary.length < raw.length) {
        raw = summary
        summarized = true
      }
    }
    // 长度保护（分支输出无落盘兜底，纯上下文保护）：超限保留头尾 + 省略说明
    const kind = opts.final ? "分支报告" : "阶段性成果"
    const content = raw.length > BRANCH_MERGE_MAX_CHARS
      ? `${raw.slice(0, Math.floor(BRANCH_MERGE_MAX_CHARS * 0.7))}\n…（${kind}过长，中间省略约 ${raw.length - BRANCH_MERGE_MAX_CHARS} 字符）\n${raw.slice(-Math.floor(BRANCH_MERGE_MAX_CHARS * 0.3))}`
      : raw
    const header = opts.final ? (summarized ? "已合并（摘要合入）" : "已合并") : summarized ? "阶段性合入（摘要）" : "阶段性合入"
    const msg: Message = {
      id: crypto.randomUUID(),
      role: "assistant",
      content: `【并行分支「${handle.name}」${header}】\n${content}${summarized ? "\n（报告全文见分支过程存档，bg_task wait 可取回）" : ""}`,
      createdAt: Date.now(),
      branchMeta: { branchId: handle.branchId, name: handle.name, ...(handle.model ? { model: handle.model } : {}) },
      ...(opts.final && handle.archive ? { sessionRun: handle.archive } : {}),
    }
    this.enqueueTrunkMerge(sessionId, user, msg)
    this.publish(sessionId, "event.branch.merged", { messageId: msg.id, branchId: handle.branchId, name: handle.name, ...(handle.model ? { model: handle.model } : {}), text: msg.content, sessionId })
    const noticeLead = opts.final ? "已完成并合入主干（最终报告）" : "阶段性合入主干"
    this.relayToBranches(sessionId, handle.branchId, `【分支感知】分支「${handle.name}」${noticeLead}:\n${branchNoticeHead(content)}`)
  }

  /** 合并消息入主干（最终/阶段性共用）：任务运行中入合并队列（runLoop 工具批处理边界排空），否则直接落盘。 */
  private enqueueTrunkMerge(sessionId: string, user: string, msg: Message): void {
    if (this.tasks.has(sessionId)) {
      const q = this.branchMerges.get(sessionId) ?? []
      q.push(msg)
      this.branchMerges.set(sessionId, q)
    } else {
      void this.opts.store.appendMessage(sessionId, msg, user).catch(() => {})
    }
  }

  /** 主干通知广播（互相感知）：把通知压入本会话其他运行中分支的收件箱（分支执行循环轮首排空注入）。 */
  private relayToBranches(sessionId: string, exceptBranchId: string | undefined, notice: string): void {
    for (const h of this.branchRunStore.values()) {
      if (h.sessionId !== sessionId || h.status !== "running" || h.branchId === exceptBranchId) continue
      ;(h.inbox ??= []).push(notice)
    }
  }

  /**
   * 主干增量快照（branch_sync 工具返回体，DESIGN「会话分支运行与合并」互相感知）：返回主干自
   * fork/上次同步以来的全部新消息——主线用户输入/回复、其他分支合入全文、主线工具结果摘要。
   * 水位推进至当前存储尾部；合并队列中尚未落盘的合入（同步 fan-out 期间）一并回显并登记已投递 id
   * （防其落盘后在下次增量中重复出现）；本分支自己的合入跳过（内容自产，无需回显）。
   */
  private async trunkSnapshot(sessionId: string, user: string, branchId: string): Promise<string> {
    const h = this.branchRunStore.get(branchId)
    if (!h || h.status !== "running") return "（分支已结束，无法同步主干）"
    const session = await this.opts.store.load(sessionId, user)
    const msgs = session?.messages ?? []
    const from = Math.min(h.syncedAt ?? h.forkAt ?? msgs.length, msgs.length)
    const delivered = (h.deliveredMergeIds ??= new Set<string>())
    const perMsg = 1200
    const head = (t: string, max: number) => {
      const flat = t.trim()
      return flat.length <= max ? flat : `${flat.slice(0, max)}\n…（该消息过长已截断）`
    }
    const lines: string[] = []
    for (const m of msgs.slice(from)) {
      if (delivered.has(m.id)) continue
      if (m.branchMeta?.branchId === branchId) continue // 本分支自己的合入：内容自产
      if (m.role === "user") lines.push(`【主线用户】${head(m.content, perMsg)}`)
      else if (m.role === "assistant" && m.branchMeta) lines.push(`【合并·${m.branchMeta.name}】${head(m.content, perMsg)}`)
      else if (m.role === "assistant") lines.push(`【主线回复】${head(m.content, perMsg)}`)
      else if (m.role === "tool") lines.push(`【主线工具·${m.name ?? "?"}】${head(m.content, 600)}`)
      // system（装载提示词/压缩摘要）：fork 后新增的装载提示词对分支无操作意义，跳过
    }
    // 合并队列中未落盘的合入（同步 fan-out 期间其他分支的合入在队列、落盘在工具批处理边界）
    for (const q of this.branchMerges.get(sessionId) ?? []) {
      if (delivered.has(q.id) || q.branchMeta?.branchId === branchId) continue
      delivered.add(q.id)
      lines.push(`【合并·${q.branchMeta?.name ?? "?"}】${head(q.content, perMsg)}`)
    }
    h.syncedAt = msgs.length
    return lines.length ? lines.join("\n\n") : "（主干自 fork/上次同步以来暂无新消息）"
  }

  /** 分支合并队列排空：把已完成分支的合并消息追加进存储与当前任务上下文（位于本轮 tool 结果之后——
   *  assistant(toolCalls)→tool 配对完整，主线下轮模型调用即见合并内容）。runLoop 工具批处理边界调用；
   *  任务收尾（run finally）仅落盘冲刷（上下文随任务结束，下次 run 经 loadHistory 进上下文）。 */
  private async drainBranchMerges(sessionId: string, user: string, persist?: (msg: Message) => Promise<void>, messages?: MessageLike[]): Promise<void> {
    const q = this.branchMerges.get(sessionId)
    if (!q?.length) return
    this.branchMerges.delete(sessionId)
    for (const msg of q) {
      try {
        if (persist) await persist(msg)
        else await this.opts.store.appendMessage(sessionId, msg, user)
        messages?.push({ role: "assistant", content: msg.content })
      } catch {
        /* 落盘失败不阻断任务收尾（与中断补写同策略） */
      }
    }
  }

  /** 解析子Agent 项目根（{AGENT_NAME_UPPER}_PROJECT 环境变量，未配置时回落 SubAgentDef.projectRoot
   *  默认项目根——如 self_optimize 脚本调试模式自动推导歌白仓库根）：沙箱模式限定用户数据目录内
   *  （默认根同样拒绝——仓库根在用户目录外），本地模式放开。 */
  private resolveSubAgentProject(user: string, env: Record<string, string>, agentName: string): string | undefined {
    const key = `${agentName.toUpperCase().replace(/-/g, "_")}_PROJECT`
    const raw = env[key]
    if (raw) {
      try {
        return this.resolveAgentProjectRoot(user, raw)
      } catch {
        return undefined // 沙箱拒绝越界/绝对路径绑定：回退工作目录
      }
    }
    const fallback = this.opts.subAgents.def(agentName)?.projectRoot?.(env)
    if (!fallback) return undefined
    try {
      return this.resolveAgentProjectRoot(user, fallback)
    } catch {
      return undefined // 沙箱拒绝默认根（仓库根在用户目录外）：回退工作目录
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
      // 保留名冲突防呆（DESIGN「项目机制」）：启动校验覆盖进程环境变量，此处兜底前端注入的任务级 env
      if (name === RESERVED_PROJECT_TMP) throw new Error(`预置项目名 "${RESERVED_PROJECT_TMP}" 为保留名（会话工作区），请改名（${key} 配置项）`)
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
    ctxOpts?: { workdir?: string; resolveBase?: string; projects?: PresetProject[]; provider?: LLMProvider; branch?: { name: string; model?: string }; drainInbox?: () => string[]; branchSync?: (content?: string) => Promise<string>; fileGuardMap?: Map<string, string | null> },
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
    // 会话上下文注入：messages 透传 buildContext（js 脚本工具 ctx.messages 快照源）；
    // 运行时工具定义进本次运行注册表（随运行结束释放，不落盘、不外泄主会话）；
    // 安全模式拒绝（与主会话 registerDynamicTool 同规则，js 工具已拦截，此为纵深防御）
    // 任务级主模型：与主循环一致，env 配置 GEBAI_LLM_* 时重建 Provider（无覆盖沿用启动实例）；
    // 分支运行（ctxOpts.provider）按模型路由解析的独立 Provider 优先——多路接口并行
    // （先于 ctx 解析：ctx.multimodal 按任务级 Provider 能力注入，read 等工具据此决定图片处理形态）
    const taskProvider = ctxOpts?.provider ?? this.opts.resolveProvider?.(env) ?? this.opts.provider
    const ctx = this.buildContext(sessionId, user, env, signal, { ...ctxOpts, registry: reg, loadIntoRegistry: reg, role: this.tasks.get(sessionId)?.role, writeGuard: this.defsWriteGuard(agents, env), messages, registerDynamic: async (def) => { reg.register(makeDynamicTool(def)) }, multimodal: taskProvider.capabilities().multimodal }, depth)
    // 任务级额外模型接口参数（浏览器本地注入 GEBAI_LLM_EXTRA_PARAMS）：非法 JSON 忽略
    const extraParams = parseExtraParamsSafe(env.GEBAI_LLM_EXTRA_PARAMS)
    // 存档收集（替代原逐条落盘）：执行过程消息追加进 archive.messages，最终由 agent_run 扩展字段落盘
    const pushArchive = (entry: SessionRunEntry) => {
      archive.messages.push(entry)
      return Promise.resolve()
    }

    while (rounds < MAX_TOOL_ROUNDS) {
      if (activeSignal.aborted) throw new Error(abortReason())
      // 主干通知注入（分支互相感知，DESIGN「会话分支运行与合并」）：其他分支合入/主线进展的通知
      // 在轮首排空注入——位于上一轮 tool 结果之后（tool_calls 配对完整），分支下轮模型调用即见
      for (const notice of ctxOpts?.drainInbox?.() ?? []) {
        await pushArchive({ role: "user", content: notice })
        messages.push({ role: "user", content: notice })
      }
      // 每轮重推 start 事件（同 runId 幂等，前端容器已存在时忽略）：前端容器随消息重载丢失
      // （切走会话/断线重连）后，新一轮 delta 前可据此重建折叠容器；分支运行携带 branch/model 标识
      this.publish(sessionId, "event.session.start", { runId: archive.runId, agents, input, depth, ...(ctxOpts?.branch ? { branch: ctxOpts.branch.name, ...(ctxOpts.branch.model ? { model: ctxOpts.branch.model } : {}) } : {}), sessionId })
      const assistantMsgId = crypto.randomUUID()
      // 执行过程：新会话的模型回复文本/推理实时推送到前端（与主循环同流显示，带 session 标记）
      let reasoningAcc = ""
      const { text, toolCalls, stopReason } = await this.callModel(taskProvider, messages, reg.schemas().filter((s) => !this.isToolDisabled(sessionId, s.name, reg.resolve(s.name)?.tool)), activeSignal, (chunk) => {
        if (chunk.type === "text") {
          // session 标记：区别于主循环推送，渠道层可据此识别「新会话执行过程」事件；
          // 仅最终响应（final_only）不推送新会话过程文本；异步后台运行在发起任务结束后仍照常推送
          // （任务已不在 tasks 表，缺省视为 streaming——final_only 会话仅在任务存续期内可判定）
          this.noteStream(sessionId, { messageId: assistantMsgId, text: chunk.text, session: true, sessionRunId: archive.runId })
          if ((this.tasks.get(sessionId)?.outputMode ?? "streaming") === "streaming") this.publish(sessionId, "event.message.delta", { text: chunk.text, messageId: assistantMsgId, session: true, sessionRunId: archive.runId, sessionId })
        } else if (chunk.type === "reasoning" && chunk.text?.trim()) {
          reasoningAcc += chunk.text
          this.noteStream(sessionId, { reasoning: reasoningAcc })
          if ((this.tasks.get(sessionId)?.outputMode ?? "streaming") === "streaming") this.publish(sessionId, "event.message.reasoning", { text: chunk.text, session: true, sessionRunId: archive.runId, messageId: assistantMsgId, sessionId })
        }
      }, extraParams, sessionId)
      lastText = text
      if (!toolCalls.length) {
        this.publish(sessionId, "event.message.done", { text, messageId: assistantMsgId, session: true, sessionRunId: archive.runId, sessionId })
        // 新会话 run 收尾：最终回复入存档（折叠容器回放展示）
        if (text) {
          await pushArchive({ role: "assistant", content: text, reasoning: reasoningAcc.trim() ? reasoningAcc.trim() : undefined })
        }
        this.clearStream(sessionId, archive.runId)
        this.publish(sessionId, "event.session.done", { runId: archive.runId, agents, output: text, ...(ctxOpts?.branch ? { branch: ctxOpts.branch.name } : {}), sessionId })
        return text
      }

      normalizeToolCalls(reg, toolCalls)
      await pushArchive({ role: "assistant", content: text, reasoning: reasoningAcc.trim() ? reasoningAcc.trim() : undefined, toolCalls: toolCalls.map((tc) => ({ id: tc.id, name: tc.name, arguments: tc.arguments })) })
      messages.push({ role: "assistant", content: text, toolCalls })
      this.clearStream(sessionId, archive.runId) // 本轮文本已入存档，在途快照清空（只清本 run 的，不误清并行主任务快照）
      let stopped = false
      // 门控阶段（按调用顺序串行，与主循环同构）：判定全部先行完成，说明性结果直接入存档；
      // 可执行项进入并行阶段（同批工具并行执行，DESIGN「同批工具并行执行」）
      // 同批重复签名只检测/记录一次（与主循环同因）：同批并行发出相同调用是有意扇出，跨轮重复才累积判定
      const batchSignatures = new Set<string>()
      const pending: Array<{ tc: (typeof toolCalls)[number]; rt: NonNullable<ReturnType<ToolRegistry["resolve"]>>; autoLoaded: string; approvalRequired: boolean }> = []
      // 门控说明性结果（不执行）同样推送 call+result 事件对（与主循环同因）：前端实时建卡，与存档回放一致
      const gatedNote = async (tc: (typeof toolCalls)[number], note: string) => {
        this.publish(sessionId, "event.tool.call", { name: tc.name, arguments: tc.arguments, toolCallId: tc.id, session: true, sessionRunId: archive.runId })
        await pushArchive({ role: "tool", content: note, toolCallId: tc.id, name: tc.name })
        messages.push({ role: "tool", content: note, toolCallId: tc.id, name: tc.name })
        this.publish(sessionId, "event.tool.result", { name: tc.name, toolCallId: tc.id, output: note, session: true, sessionRunId: archive.runId, sessionId })
      }
      for (const tc of toolCalls) {
        if (stopped) {
          await gatedNote(tc, "任务已中止：模型持续重复相同工具调用。")
          continue
        }
        const signature = toolCallSignature(tc.name, tc.arguments)
        if (!batchSignatures.has(signature)) {
          batchSignatures.add(signature)
          if (this.repeatedCall(recentCalls, tc.name, tc.arguments)) {
            repeatStalls++
            if (repeatStalls > MAX_REPEAT_STALLS) stopped = true
            await gatedNote(tc, `已中断重复的工具调用 ${tc.name}：参数与之前完全相同，重复执行只会得到相同结果。请分析原因、改用其他方法，或直接给出最终回答，不要重复相同操作。`)
            continue
          }
        }
        // 工具参数不是合法 JSON：与主循环一致（截断先抢救落盘、未抢救区分引导），回传错误让模型修正（不执行）
        if (tc.argsError) {
          const salvaged = this.isWriteToolName(tc.name) ? await this.salvageWriteCall(ctx, typeof tc.raw === "string" ? tc.raw : "") : null
          await gatedNote(tc, salvaged ?? this.toolArgsErrorMsg(tc, this.isTruncatedStop(stopReason)))
          continue
        }
        let rt = reg.resolve(tc.name)
        let autoLoaded = ""
        if (!rt) {
          // 路由自愈（与主循环一致，DESIGN「子Agent 路由」）：新会话内的装载走本运行注册表 + 临时 messages
          // 插段（loadAgentIntoRun），不写全局注册表/父会话记录（新会话执行隔离语义）
          const agent = this.subAgentForToolName(tc.name)
          if (agent) {
            await this.loadAgentIntoRun(reg, agent, messages, user, env, sessionId, ctxOpts?.projects).catch(() => {})
            rt = reg.resolve(tc.name)
            if (rt) autoLoaded = agent
          }
        }
        if (!rt) {
          await gatedNote(tc, this.unknownToolMsg(tc.name))
          continue
        }
        const missing = missingRequiredArgs(rt.tool, tc.arguments)
        if (missing.length) {
          // 必填参数缺失（模型漏传，与主循环同规则）：不执行，明确报缺什么参数
          await gatedNote(tc, missingArgsMsg(rt.name, missing))
          continue
        }
        if (this.isToolDisabled(sessionId, rt.name, rt.tool)) {
          await gatedNote(tc, this.toolDisabledMsg(sessionId, rt.name))
          continue
        }
        if (this.isRiskyInSafeMode(rt.name)) {
          // 安全模式：子Agent 内风险工具同样拦截（与主循环一致），返回限制信息供子Agent 调整方案
          await gatedNote(tc, safeModeRestrictionMsg(rt.name))
          continue
        }
        const requiresByArgs = await toolRequiresApproval(rt.tool, tc.arguments, ctx)
        const approvalSkipped = await this.isApprovalSkipped(sessionId, user, env)
        // 与主循环一致：无交互通道硬门槛（服务模式默认 / 请求级 autoApprove=false）按默认审批姿态拒绝
        //（approval:false 免审标记剥离后解析，防绕过）
        if (
          this.noInteractionHardGate(sessionId) && this.tasks.get(sessionId)?.interactionMode === "none" && !approvalSkipped &&
          (requiresByArgs || (await toolRequiresApproval(rt.tool, stripApprovalFlags(tc.arguments) as Record<string, unknown>, ctx)))
        ) {
          await gatedNote(tc, this.noInteractionDenied(rt.name))
          continue
        }
        pending.push({ tc, rt, autoLoaded, approvalRequired: requiresByArgs && !approvalSkipped })
      }
      // 执行阶段（并行，与主循环同构）：审批等待 → 执行 → 存档 → 事件；存档条目按完成先后追加
      // （仅前端回放顺序，不进 LLM 上下文）；单个调用出错不中断其他调用，池排空后上抛首个错误
      let firstError: unknown
      await runToolPool(pending, MAX_PARALLEL_TOOLS, async ({ tc, rt, autoLoaded, approvalRequired }) => {
        try {
          if (activeSignal.aborted) throw new Error(abortReason())
          if (approvalRequired) {
            const retries = this.tasks.get(sessionId)?.retries.get(tc.id) ?? 0
            this.publish(sessionId, "event.tool.call", { name: tc.name, arguments: tc.arguments, toolCallId: tc.id, requiresApproval: true, session: true, sessionRunId: archive.runId })
            this.publish(sessionId, "event.approval.request", { toolCallId: tc.id, tool: tc.name, retries, arguments: tc.arguments, session: true, sessionRunId: archive.runId, sessionId })
            const verdict = await this.waitApproval(sessionId, tc.id, rt.name, activeSignal)
            // 新会话内：取消/超时信号解开等待后立即中止（存档随 run 整体，取消由上层按已取消结果收尾）
            if (activeSignal.aborted) throw new Error(abortReason())
            if (verdict !== "approved") {
              this.tasks.get(sessionId)?.retries.set(tc.id, retries + 1)
              const denied = verdict === "timeout" ? `工具调用 ${rt.name} 审批等待超时，已跳过。` : `工具调用 ${rt.name} 已被用户拒绝。请调整方案。`
              await pushArchive({ role: "tool", content: denied, toolCallId: tc.id, name: tc.name })
              messages.push({ role: "tool", content: denied, toolCallId: tc.id, name: tc.name })
              // 拒绝/超时同样推送结果事件（与主循环同因）：实时卡片落终态
              this.publish(sessionId, "event.tool.result", { name: tc.name, toolCallId: tc.id, output: denied, session: true, sessionRunId: archive.runId, sessionId })
              return
            }
          } else {
            this.publish(sessionId, "event.tool.call", { name: tc.name, arguments: tc.arguments, toolCallId: tc.id, session: true, sessionRunId: archive.runId })
          }
          // 取消/超时统一收口：父任务停止均中断执行（脚本进程同步被杀），超时作为结果返回模型
          this.publish(sessionId, "event.tool.result.start", { name: tc.name, toolCallId: tc.id, session: true, sessionRunId: archive.runId, sessionId })
          const result = await this.runToolInterruptible(rt.tool, tc.arguments, ctx, activeSignal, rt.name, sessionId, tc.id)
          // 兜底截断（与主循环一致）：超长工具输出统一截断，防存档膨胀；结构化 data 与存档扩展字段原样保留
          const safe = !result.truncated && result.output.length > TRUNCATE_THRESHOLD
            ? { ...(await truncate(result.output, rt.name, ctx)), blocks: result.blocks, data: result.data, sessionRun: result.sessionRun, images: result.images }
            : result
          // 嵌套 agent_run：新会话的存档递归挂到工具消息上（历史回放嵌套容器）；不进主上下文，
          // provider 序列化只取已知字段，额外字段不会泄漏进 LLM 请求
          const nested = safe.sessionRun ? { sessionRun: safe.sessionRun } : {}
          const withNote = autoLoaded ? `（引擎已自动装载子Agent ${autoLoaded}——其工具已并入本次执行、提示词已注入上下文）\n${safe.output}` : safe.output
          // 多模态工具结果图片（read 等读取的图片）：主模型多模态时内联进本次运行的 tool 消息
          // （新会话执行为内存态，无落盘引用——随运行结束释放，不进存档/UI 走 blocks）
          const imageBlocks = taskProvider.capabilities().multimodal && safe.images?.length ? await this.toolImageBlocks(safe.images) : []
          await pushArchive({ role: "tool", content: withNote, blocks: safe.blocks, toolCallId: tc.id, name: tc.name, arguments: tc.arguments, ...nested })
          messages.push({ role: "tool", content: imageBlocks.length ? [{ type: "text", text: withNote }, ...imageBlocks] : withNote, toolCallId: tc.id, name: tc.name, ...nested })
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
        } catch (err) {
          if (firstError === undefined) firstError = err
        }
      })
      if (firstError !== undefined) throw firstError
      rounds++
      if (stopped) break
    }
    // 循环上限退出（重复调用风暴终止）：同样推送 done 事件折叠容器
    this.publish(sessionId, "event.session.done", { runId: archive.runId, agents, output: lastText, ...(ctxOpts?.branch ? { branch: ctxOpts.branch.name } : {}), sessionId })
    return lastText
  }

  private waitApproval(sessionId: string, toolCallId: string, tool: string, signal?: AbortSignal): Promise<ApprovalVerdict> {
    const task = this.tasks.get(sessionId)
    // 任务已结束（异步分支/后台运行晚于主线完成时触发审批）：无人可审批，按超时跳过——
    // 缺任务直接访问 task.approvals 会抛错使运行整体失败
    if (!task) return Promise.resolve("timeout")
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
      task.approvals.set(toolCallId, { sessionId, toolCallId, tool, resolve: done, timer })
    })
  }

  private async saveAttachments(sessionId: string, user: string, attachments: AttachmentInput[]): Promise<AttachmentRef[] | undefined> {
    if (!attachments.length) return undefined
    const { writeFile, mkdir } = await import("node:fs/promises")
    const tmp = this.opts.store.getTmpDir(sessionId, user)
    const refs: Array<{ path: string; mime: string; name: string; size: number }> = []
    const usedNames = new Set<string>()
    for (const a of attachments) {
      // 名称消毒：仅取 basename，拒绝路径分隔符与穿越（防止 ../ 逃逸会话目录）
      let name = basenameName(a.name)
      if (!name) throw new Error(`附件名无效: ${a.name}`)
      // 重名去重：同批两个 data.csv 会在同一路径静默覆盖（前一个内容丢失）；追加序号区分，
      // 也避免覆盖会话 tmp 下既有同名文件（上一轮任务产物）
      if (usedNames.has(name)) {
        const dot = name.lastIndexOf(".")
        const stem = dot > 0 ? name.slice(0, dot) : name
        const ext = dot > 0 ? name.slice(dot) : ""
        let i = 2
        while (usedNames.has(`${stem}-${i}${ext}`)) i++
        name = `${stem}-${i}${ext}`
      }
      usedNames.add(name)
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

/** 有界并行池（同批工具并行执行的并发护栏，DESIGN「同批工具并行执行」）：items 最多 limit 个同时执行，
 *  超出按序排队（worker 循环取件）。fn 必须自行捕获错误——单个失败不中断池（错误由调用方闭包收集）。 */
export async function runToolPool<T>(items: T[], limit: number, fn: (item: T) => Promise<void>): Promise<void> {
  let cursor = 0
  const workers: Promise<void>[] = []
  for (let w = 0; w < Math.min(Math.max(1, limit), items.length); w++) {
    workers.push(
      (async () => {
        while (cursor < items.length) {
          const item = items[cursor++]
          await fn(item)
        }
      })(),
    )
  }
  await Promise.all(workers)
}
