/**
 * 统一任务管理（DESIGN「统一任务管理」）：定时 / 普通 / 闲时三类任务共用一份用户级存储
 * （`users/{user}/tasks.json`）与一条调度队列（每用户一条，并发额度 `GEBAI_TASK_MAX_CONCURRENT`，缺省 5）。
 *
 * 三类任务的差别只在**何时入队**：定时任务到期自动入队（队首）、普通任务由用户/接口入队（队尾，可置顶）、
 * 闲时任务仅在队列空闲（无待定/运行的定时与普通任务）且该用户无运行中的会话时串行执行（占一个额度）。
 * 运行中的任务**不因额度不足被中断**：额度满或目标会话忙都只排队等待，下一轮再评估。
 *
 * 执行体两类：script（shell 在任务资源目录执行，结果消息写回来源会话）与 prompt（触发一次完整 Agent 会话，
 * 目标 ephemeral/sticky/session）。超时经 engine.windDown 快速结束后取消；连续失败达阈值自动停用。
 *
 * 存储范式（与用户级待办一致）：启动 walkDir 扫描加载 + Map 驻留 + **磁盘真值 RMW** 落盘
 * （跨进程写锁 + 原子写；多实例并存时后写者不会抹掉他人条目）。
 */
import { randomUUID } from "node:crypto"
import { existsSync, type Dirent } from "node:fs"
import { dirname, join, relative, resolve as pathResolve, sep } from "node:path"
import { mkdir, readFile as readFileFs, readdir, rename, rm, stat, writeFile as writeFileFs } from "node:fs/promises"
import type {
  AgentEvent,
  Task,
  TaskCreateInput,
  TaskFileEntry,
  TaskKind,
  TaskNotifyChannel,
  TaskNotifyInput,
  TaskNotifyMessage,
  TaskNotifyResult,
  TaskNotifyWhen,
  TaskQueueEntry,
  TaskQueueSource,
  TaskQueueView,
  TaskRunRecord,
  TaskRunStatus,
  TaskRunner,
  TaskTarget,
  TaskUpdateInput,
} from "@gebai/sdk"
import type { AgentEngine } from "../engine/engine"
import type { SessionStore } from "../session/store"
import type { EnvManager } from "../session/env"
import { isSensitive } from "../session/env"
import { ENC_PREFIX, isSealed, seal, unseal } from "../support/crypto"
import type { Sandbox } from "../security/sandbox"
import type { EventBus } from "../base/event-bus"
import type { NotifyDeps, TaskMessageNotification } from "./notify"
import { validateNotifyChannel, sendTaskNotification, sendTaskMessage, normalizeAtList, isFeishuChatId, NOTIFY_TEXT_MAX } from "./notify"
import { isOneShotSchedule, parseSchedule } from "./expr"
import { TASK_RUNS_KEEP, importTaskRuns, listTaskRuns, trimTaskRuns, writeTaskRun } from "./task-runs"
import { mutateJsonList, writeJsonListAtomic } from "../support/json-store"
import { sessionPath, walkDir } from "../base/paths"
import { agentNoteHead } from "../support/agent-note"
import { log } from "@gebai/sdk/node"

/** 环境变量掩码占位（回显用；回传占位即保留原值）。 */
export const MASKED_ENV_VALUE = "***"

/** 任务调度 tick 周期（DESIGN「常量参考」）：到期检查与队列推进。 */
export const TASK_TICK_INTERVAL_MS = 30_000
/** 脚本型任务单次执行超时缺省（可按任务 timeoutMs 覆盖）。 */
export const TASK_SCRIPT_TIMEOUT_MS = 5 * 60 * 1000
/** 提示词型任务单次执行超时缺省（到时取消会话任务）。 */
export const TASK_PROMPT_TIMEOUT_MS = 30 * 60 * 1000
/** 单次执行超时上下限。 */
export const TASK_TIMEOUT_MIN_MS = 1_000
export const TASK_TIMEOUT_MAX_MS = 24 * 60 * 60 * 1000
/** 脚本输出在任务记录中的保留长度（消息中的完整输出另行截断）。 */
export const TASK_OUTPUT_MAX = 4000
/** 脚本结果写入会话消息的内容上限。 */
export const TASK_MESSAGE_MAX = 8000
/** 任务清单/运行历史默认展示的条数（执行记录改为按文件落盘后的展示默认值；存储上限见 `TASK_RUNS_KEEP`）。 */
export const TASK_RUNS_HISTORY = 10
/** 任务名长度上限。 */
export const TASK_NAME_MAX = 100
/** 单用户任务条数上限（防无限增长；一次性任务不计入判定）。 */
export const TASK_MAX_ITEMS = 500
/** 每用户并发额度缺省值（GEBAI_TASK_MAX_CONCURRENT）。 */
export const TASK_MAX_CONCURRENT_DEFAULT = 5
/** 任务资源目录内的单文件写入上限。 */
export const TASK_FILE_MAX_BYTES = 4 * 1024 * 1024
/** 队列排序权重（小者先）：定时任务恒在普通任务之前，闲时任务最后。 */
export const TASK_PRIORITY_SCHEDULED = 0
export const TASK_PRIORITY_MANUAL_FRONT = 999
export const TASK_PRIORITY_MANUAL = 1000
export const TASK_PRIORITY_IDLE = 2000

/** 通知时机归一：仅接受 auto/model 两值，其余按缺省（auto=执行结束自动发）。 */
function normalizeNotifyWhen(v: unknown): TaskNotifyWhen | undefined {
  return v === "auto" || v === "model" ? v : undefined
}

/** 内存队列条目（持久化形态是任务自身的 state/queue 字段，启动按此重建）。 */
interface QueueItem {
  taskId: string
  user: string
  kind: TaskKind
  source: TaskQueueSource
  priority: number
  enqueuedAt: number
  front?: boolean
  /** 上一轮尝试启动的等待原因（如目标会话忙），供队列视图展示。 */
  waiting?: string
}

/** 调度器依赖：结构接口（与 AgentEngineOptions 同风格），便于测试替身注入。 */
export interface TaskManagerDeps {
  home: string
  store: SessionStore
  env: EnvManager
  sandbox: Sandbox
  events: EventBus
  /** 可注入时钟（测试用），默认 Date.now。 */
  now?: () => number
  tickIntervalMs?: number
  /** 安全模式（GEBAI_SAFE_MODE=true 启动时加载）：script 型任务跳过执行；通知投递（外发网络）同样跳过。 */
  safeMode?: boolean
  /** prompt 型任务执行引擎（构造时可缺省，经 attach 注入，避免与 engine 互相依赖构造）。 */
  engine?: AgentEngine
  /** 通知投递依赖（fetch/飞书应用消息可注入伪造）。 */
  notify?: NotifyDeps
  /** 子Agent 名校验器（agents 预载名单合法性；生产接线注入 subAgents.def 探测）。 */
  agentExists?: (name: string) => boolean
  /** webhookId 引用解析器（notify 通道引用 REST 注册的事件 Webhook：返回其 URL 与签名密钥；
   *  归属校验由接线方完成——具名注册仅本人可引用，全局注册（admin，userId 未记录）人人可引用；不存在返回 null）。 */
  resolveWebhook?: (id: string, user: string) => { url: string; secret?: string } | null
  /** 全局默认通知通道（GEBAI_TASK_NOTIFY_WEBHOOK / GEBAI_TASK_NOTIFY_FEISHU 构建注入）：
   *  任务未配置自己的 notify 时投递使用——不写入任务数据（环境变量改动即时生效），任务自配通道则不叠加（防重复推送）。 */
  defaultNotify?: TaskNotifyChannel[]
  /** 每用户并发额度（GEBAI_TASK_MAX_CONCURRENT，缺省 5）。 */
  maxConcurrent?: number
  /** 任务运行结束回调（待办联动：闲时/一次性任务的结果回写待办条目）。 */
  onTaskFinished?: (task: Task, run: TaskRunRecord) => void | Promise<void>
  /** 闲时任务排队顺序（用户级待办按其清单序）：返回该用户绑定的闲时任务 id 序列（缺省按创建时间）。 */
  idleOrder?: (user: string) => string[] | undefined
  /** 本实例是否在跑调度（调度器主实例锁门控；缺省视为是）。**闲时任务自动进场**只由跑调度的实例发起——
   *  从实例没有自己的会话，`busyUser` 恒假，不加此门控则每次手动执行/新建任务都会顺手把闲时任务拉起来跑。 */
  schedulerActive?: () => boolean
}

/** 启动尝试结果：started=已启动；deferred=条件不满足（留队列下一轮再试）；dropped=已出队（不存在/已运行/前置失败）。 */
type StartAction = "started" | "deferred" | "dropped"

export class TaskManager {
  private entries = new Map<string, Task>()
  /** 内存队列（按优先级 + 入队时间排序；持久化形态见 QueueItem 注释）。 */
  private queue: QueueItem[] = []
  private timer: ReturnType<typeof setInterval> | null = null
  /** drain 单飞标记：一次只跑一个推进过程（启动是异步的，防并发重复启动同一批条目）。 */
  private draining = false
  /** 推进期间再收到入队/结束信号（本轮结束后立即再跑一轮）。 */
  private drainWanted = false
  /** 正在运行的任务（taskId → 执行会话与来源；终止与队列视图用）。 */
  private running = new Map<string, { sessionId?: string; startedAt: number; manual: boolean; source: TaskQueueSource }>()
  private engine: AgentEngine | undefined
  private now: () => number
  private maxConcurrent: number
  /** 本实例是否在跑调度（闸门读 deps，看门狗接管/退让即时生效）。 */
  private schedulerActive: () => boolean

  constructor(private deps: TaskManagerDeps) {
    this.engine = deps.engine
    this.now = deps.now ?? (() => Date.now())
    this.maxConcurrent = Math.max(1, Math.floor(deps.maxConcurrent ?? TASK_MAX_CONCURRENT_DEFAULT))
    this.schedulerActive = deps.schedulerActive ?? (() => true)
  }

  /** 注入执行引擎（prompt 型任务执行器；构造期缺省时调用）。双向绑定：同时回填引擎侧
   *  opts.tasks（task_* 工具的 ToolContext 绑定源），单向注入会使工具恒报「能力未启用」。 */
  attach(engine: AgentEngine): void {
    this.engine = engine
    engine.setTasks(this)
  }

  /** 注入任务运行结束回调（待办联动：晚于构造接线时使用；同一时刻只保留一个回调）。 */
  onFinished(fn: (task: Task, run: TaskRunRecord) => void | Promise<void>): void {
    this.deps.onTaskFinished = fn
  }

  /** 注入闲时任务排队顺序（待办联动：用户级待办按清单序执行）。 */
  setIdleOrder(fn: (user: string) => string[] | undefined): void {
    this.deps.idleOrder = fn
  }

  /** 用户级任务存储文件（users/{user}/tasks.json，随用户目录生命周期，与会话删除/过期解耦）。 */
  private userTaskFile(user: string): string {
    return join(this.deps.home, "users", user, "tasks.json")
  }

  /** 任务资源目录（脚本 cwd 与资料目录，跨次运行保留产物）。 */
  workspaceOf(entry: Pick<Task, "id" | "user">): string {
    return join(this.deps.home, "users", entry.user, "tasks", entry.id)
  }

  /** 扫描加载用户级任务（含旧 cron.json 一次性迁移），重建队列并启动 tick 循环。 */
  async start(): Promise<void> {
    const now = this.now()
    const base = join(this.deps.home, "users")
    const users = new Set<string>()
    await walkDir(base, 5, async (p) => {
      const rel = relative(base, p).split(sep)
      if (rel.length !== 2) return
      const user = rel[0]
      if (rel[1] === "cron.json" || rel[1] === "tasks.json") users.add(user)
    })
    for (const user of users) {
      await this.migrateLegacyCron(user, now)
      await this.loadUser(user, now)
    }
    this.rebuildQueue()
    if (this.timer) return
    this.timer = setInterval(() => void this.tick(), this.deps.tickIntervalMs ?? TASK_TICK_INTERVAL_MS)
    this.timer.unref?.()
  }

  /** 旧定时任务存储（cron.json）一次性迁移：转 kind=scheduled 写入 tasks.json，旧文件改名 .bak 保留。
   *  仅当该用户尚无 tasks.json 时执行（幂等：二次启动不再触碰）。 */
  private async migrateLegacyCron(user: string, now: number): Promise<void> {
    const legacyFile = join(this.deps.home, "users", user, "cron.json")
    if (!existsSync(legacyFile) || existsSync(this.userTaskFile(user))) return
    let raw: unknown
    try {
      raw = JSON.parse(await readFileFs(legacyFile, "utf8"))
    } catch {
      return
    }
    if (!Array.isArray(raw)) return
    const converted: Task[] = []
    for (const item of raw) {
      const entry = this.convertLegacyCron(item, user, now)
      if (entry) converted.push(entry)
    }
    await writeJsonListAtomic(this.userTaskFile(user), converted)
    // 脚本型任务工作目录（原 cron-workspace/{id}）并入新资源目录（同盘 rename；失败保留原目录不影响迁移结果）
    for (const entry of converted) {
      const from = join(this.deps.home, "users", user, "cron-workspace", entry.id)
      const to = this.workspaceOf(entry)
      if (!existsSync(from) || existsSync(to)) continue
      await mkdir(dirname(to), { recursive: true }).catch(() => {})
      await rename(from, to).catch(() => {})
    }
    await rename(legacyFile, `${legacyFile}.migrated.bak`).catch(() => {})
  }

  /** 旧定时任务条目 → 统一任务条目（字段同义搬移；无法识别返回 null）。 */
  private convertLegacyCron(raw: unknown, user: string, now: number): Task | null {
    if (!raw || typeof raw !== "object") return null
    const e = raw as Record<string, unknown>
    const id = typeof e.id === "string" ? e.id : ""
    if (!/^[0-9a-f]{32}$/.test(id) || typeof e.schedule !== "string") return null
    const runner: TaskRunner = e.type === "script" ? "script" : "prompt"
    const status = e.lastStatus
    const entry: Task = {
      id,
      user,
      kind: "scheduled",
      runner,
      name: typeof e.name === "string" && e.name ? e.name.slice(0, TASK_NAME_MAX) : undefined,
      script: runner === "script" ? String(e.script ?? "") : undefined,
      prompt: runner === "prompt" ? String(e.prompt ?? "") : undefined,
      schedule: e.schedule,
      timezone: typeof e.timezone === "string" ? e.timezone : undefined,
      misfire: e.misfire === "run" ? "run" : e.misfire === "skip" ? "skip" : undefined,
      target: e.target === "sticky" || e.target === "session" || e.target === "ephemeral" ? e.target : undefined,
      sessionId: typeof e.sessionId === "string" ? e.sessionId : undefined,
      stickySessionId: typeof e.stickySessionId === "string" ? e.stickySessionId : undefined,
      agents: Array.isArray(e.agents) ? e.agents.map(String) : undefined,
      timeoutMs: typeof e.timeoutMs === "number" ? e.timeoutMs : undefined,
      notify: Array.isArray(e.notify) ? (e.notify as TaskNotifyChannel[]) : undefined,
      notifyOn: normalizeNotifyWhen(e.notifyOn),
      maxConsecutiveErrors: typeof e.maxConsecutiveErrors === "number" ? e.maxConsecutiveErrors : undefined,
      originSessionId: typeof e.originSessionId === "string" ? e.originSessionId : undefined,
      enabled: e.enabled !== false,
      createdAt: typeof e.createdAt === "number" ? e.createdAt : now,
      updatedAt: typeof e.updatedAt === "number" ? e.updatedAt : now,
      state: "idle",
      runCount: typeof e.runCount === "number" ? e.runCount : 0,
      lastRunAt: typeof e.lastRunAt === "number" ? e.lastRunAt : undefined,
      nextRunAt: typeof e.nextRunAt === "number" ? e.nextRunAt : undefined,
      lastStatus: status === "success" || status === "error" || status === "skipped" || status === "timeout" ? status : undefined,
      lastOutput: typeof e.lastOutput === "string" ? e.lastOutput : undefined,
      lastError: typeof e.lastError === "string" ? e.lastError : undefined,
      consecutiveErrors: typeof e.consecutiveErrors === "number" ? e.consecutiveErrors : undefined,
      lastNotifyError: typeof e.lastNotifyError === "string" ? e.lastNotifyError : undefined,
    }
    return entry
  }

  /** 载入单个用户的任务清单（磁盘真值 + 归一化）。 */
  private async loadUser(user: string, now: number): Promise<void> {
    let raw: unknown
    try {
      raw = JSON.parse(await readFileFs(this.userTaskFile(user), "utf8"))
    } catch {
      return
    }
    if (!Array.isArray(raw)) return
    let migrated = 0
    for (const item of raw) {
      // 磁盘条目为密文态：先解封再归一化（内存态为明文）
      const entry = this.normalizeLoaded(this.unsealEnv(item as Task), now)
      if (!entry) continue
      // 旧内联执行记录（Task.runs）一次性迁移到执行记录目录（记录文件以时间为名）
      const legacyRuns = (item as { runs?: unknown } | null)?.runs
      if (Array.isArray(legacyRuns) && legacyRuns.length) {
        migrated += await importTaskRuns(this.deps.home, user, entry.id, legacyRuns as TaskRunRecord[]).catch(() => 0)
      }
      if (!this.entries.has(entry.id)) this.entries.set(entry.id, entry)
    }
    // 有迁移即重写定义文件（normalizeEntry 已剥离 runs 字段，需 forceWrite 沉降——恒等变更默认跳过写入）：
    // RMW 以磁盘真值为基准，不触及其他实例的并发改动
    if (migrated) {
      log.info(`[tasks] 用户 ${user} 迁移 ${migrated} 条旧内联执行记录到 task-runs/`)
      await this.persist(user, (disk) => disk, { forceWrite: true }).catch(() => {})
    }
  }

  /** 磁盘条目归一化（纯形状收敛：校验 + 补缺省，**不改写运行态与调度时间**）。
   *  RMW 读真值必须用它——载入期的重启恢复（运行中标记中断、过期触发点处置）若也在此执行，
   *  每次落盘都会把本进程/其他实例正在运行的任务状态与待触发时间抹掉。 */
  private normalizeEntry(raw: unknown): Task | null {
    if (!raw || typeof raw !== "object") return null
    const e = raw as Task
    if (typeof e.id !== "string" || !/^[0-9a-f]{32}$/.test(e.id)) return null
    if (typeof e.user !== "string" || !e.user) return null
    const kind: TaskKind = e.kind === "manual" || e.kind === "idle" || e.kind === "scheduled" ? e.kind : "scheduled"
    const runner: TaskRunner = e.runner === "script" ? "script" : "prompt"
    if (runner === "script" && typeof e.script !== "string") return null
    if (runner === "prompt" && typeof e.prompt !== "string") return null
    if (kind === "scheduled" && typeof e.schedule !== "string") return null
    const entry: Task = {
      ...e,
      kind,
      runner,
      enabled: e.enabled !== false,
      state: e.state === "queued" || e.state === "running" ? e.state : "idle",
      runCount: typeof e.runCount === "number" ? e.runCount : 0,
    }
    // 任务级环境变量（外部编辑可能损坏）：非法名/非字符串值整条丢弃（不阻断任务加载）
    if (e.env !== undefined) {
      try {
        entry.env = this.validateTaskEnv(e.env)
      } catch {
        entry.env = undefined
      }
    }
    // 执行记录不属任务定义（存于 `task-runs/{taskId}/{时间}.json`）：剥离旧数据的 `runs` 字段——
    // 它仅在启动迁移时由 loadUser 从磁盘原文读取，不会经归一化流入内存态与落盘
    if ("runs" in entry) delete (entry as { runs?: unknown }).runs
    return entry
  }

  /** 启动载入归一化（在 normalizeEntry 之上做重启恢复与防护）：非法字段丢弃；
   *  运行中标记为中断；非法表达式禁用；调度时间按补跑策略处置。 */
  private normalizeLoaded(raw: unknown, now: number): Task | null {
    const entry = this.normalizeEntry(raw)
    if (!entry) return null
    // 进程重启后续跑：运行中无法续接，标记中断待人工处理（一次性任务直接清理）
    if (entry.state === "running") {
      entry.state = "idle"
      entry.startedAt = undefined
      entry.lastStatus = "error"
      entry.lastError = "服务重启，本次执行已中断（未自动重跑）"
    }
    if (entry.state === "queued" && !entry.queue) entry.queue = { source: "manual", enqueuedAt: now }
    if (entry.state !== "queued") entry.queue = undefined
    // 表达式/时区合法性（add/update 时已拒，此处防外部编辑损坏）：非法直接禁用，
    // 否则时间解析失败回退 +30s 会形成每 30s 触发一次的热循环
    if (entry.kind === "scheduled" && entry.enabled) {
      try {
        parseSchedule(entry.schedule!, entry.timezone)
      } catch {
        entry.enabled = false
        entry.lastError = `${entry.lastError ? `${entry.lastError}；` : ""}schedule/timezone 非法：启动加载时已禁用（请修正后重新启用）`
      }
    }
    if (entry.kind === "scheduled" && entry.enabled && (typeof entry.nextRunAt !== "number" || entry.nextRunAt <= now)) {
      if (entry.misfire === "run" && typeof entry.nextRunAt === "number" && entry.nextRunAt > 0) {
        // 补跑策略：保留过期触发点，首个 tick 立即入队一次（入队后按当前时间重算，至多一次）
      } else if (isOneShotSchedule(entry.schedule ?? "")) {
        // 一次性任务：@at 为绝对时刻，过期无未来时间可算——直接停用（否则重算回过去值形成热循环）
        entry.enabled = false
        entry.nextRunAt = undefined
      } else {
        entry.nextRunAt = this.nextTime(entry, now)
      }
    }
    return entry
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer)
    this.timer = null
  }
  // ---- 查询与增删改 ----

  /** 用户任务清单（按创建时间升序；密钥字段脱敏）。 */
  async list(user: string): Promise<Task[]> {
    return [...this.entries.values()]
      .filter((e) => e.user === user)
      .sort((a, b) => a.createdAt - b.createdAt)
      .map((t) => this.publicView(t))
  }

  async get(user: string, id: string): Promise<Task | null> {
    const entry = this.entryOf(user, id)
    return entry ? this.publicView(entry) : null
  }

  async add(user: string, input: TaskCreateInput, originSessionId?: string): Promise<Task> {
    if (input.runner !== "script" && input.runner !== "prompt") throw new Error(`无效的执行体: ${String(input.runner)}（script/prompt）`)
    const kind: TaskKind = input.kind === "scheduled" || input.kind === "idle" || input.kind === "manual" ? input.kind : input.schedule ? "scheduled" : "manual"
    const script = input.script != null ? String(input.script).trim() : ""
    const prompt = input.prompt != null ? String(input.prompt).trim() : ""
    if (input.runner === "script" && !script) throw new Error("脚本型任务需要 script 参数（shell 命令）")
    if (input.runner === "prompt" && !prompt) throw new Error("提示词型任务需要 prompt 参数")
    const now = this.now()
    const entry: Task = {
      id: randomUUID().replace(/-/g, ""),
      user,
      kind,
      runner: input.runner,
      name: this.normalizeName(input.name),
      script: input.runner === "script" ? script : undefined,
      prompt: input.runner === "prompt" ? prompt : undefined,
      timeoutMs: this.validateTimeout(input.timeoutMs),
      env: this.validateTaskEnv(input.env),
      notify: this.validateNotify(input.notify, undefined, user),
      notifyOn: normalizeNotifyWhen(input.notifyOn),
      maxConsecutiveErrors: this.validateMaxConsecutiveErrors(input.maxConsecutiveErrors),
      originSessionId: originSessionId || undefined,
      todoId: input.todoId ? String(input.todoId).trim() : undefined,
      ephemeral: input.ephemeral === true ? true : undefined,
      enabled: input.enabled !== false,
      createdAt: now,
      updatedAt: now,
      state: "idle",
      runCount: 0,
    }
    if (kind === "scheduled") {
      const schedule = String(input.schedule ?? "").trim()
      if (!schedule) throw new Error("定时任务缺少执行表达式（schedule）")
      const timezone = input.timezone ? String(input.timezone).trim() : undefined
      entry.schedule = schedule
      entry.timezone = timezone
      entry.misfire = input.misfire === "run" ? "run" : input.misfire === "skip" ? "skip" : undefined
      // 创建时严格校验表达式与时区（非法即拒绝），并计算下次执行时间
      const parsed = parseSchedule(schedule, timezone)
      if (isOneShotSchedule(schedule) && parsed.next(now) <= now) throw new Error("@at 时间已过去（一次性任务请指定未来时间）")
      entry.nextRunAt = parsed.next(now)
    }
    if (input.runner === "prompt") {
      const target = this.validateTarget(input.target)
      entry.target = target
      entry.sessionId = target === "session" && input.sessionId ? String(input.sessionId).trim() : undefined
      entry.agents = this.validateAgents(input.agents)
    }
    await this.persistEntry(user, entry)
    // 普通任务：创建即入队（可置顶）；定时任务等到期；闲时任务等队列空闲（drain 时进场）
    if (kind === "manual" && input.runNow !== false && entry.enabled) await this.enqueue(user, entry.id, { source: "manual", front: input.front === true })
    else if (kind === "idle" && entry.enabled) await this.drain()
    return this.publicView(entry)
  }

  async update(user: string, id: string, patch: TaskUpdateInput): Promise<Task | null> {
    const live = this.entryOf(user, id)
    if (!live) return null
    // 补丁在**副本**上应用与校验：任一校验失败不污染内存镜像（失败后仍按原值调度/持久化）
    const entry: Task = { ...live }
    if (patch.name !== undefined) entry.name = this.normalizeName(patch.name)
    if (patch.runner !== undefined) {
      if (patch.runner !== "script" && patch.runner !== "prompt") throw new Error(`无效的执行体: ${String(patch.runner)}（script/prompt）`)
      entry.runner = patch.runner
    }
    if (patch.script !== undefined) entry.script = String(patch.script).trim() || undefined
    if (patch.prompt !== undefined) entry.prompt = String(patch.prompt).trim() || undefined
    if (entry.runner === "script" && !entry.script) throw new Error("脚本型任务需要 script 参数（shell 命令）")
    if (entry.runner === "prompt" && !entry.prompt) throw new Error("提示词型任务需要 prompt 参数")
    if (patch.schedule !== undefined) {
      if (entry.kind !== "scheduled") throw new Error("普通/闲时任务没有执行表达式（schedule）")
      const s = String(patch.schedule).trim()
      if (!s) throw new Error("执行表达式不能为空")
      entry.schedule = s
    }
    if (patch.timezone !== undefined) entry.timezone = patch.timezone ? String(patch.timezone).trim() : undefined
    if (patch.misfire !== undefined) entry.misfire = patch.misfire === "run" ? "run" : "skip"
    if (patch.target !== undefined || patch.sessionId !== undefined || patch.runner !== undefined) {
      if (entry.runner === "prompt") {
        entry.target = this.validateTarget(patch.target !== undefined ? patch.target : entry.target)
        if (entry.target === "session") {
          if (patch.sessionId !== undefined) entry.sessionId = String(patch.sessionId).trim() || undefined
        } else if (patch.target !== undefined) {
          entry.sessionId = undefined
        }
      } else {
        entry.target = undefined
        entry.sessionId = undefined
      }
    }
    if (patch.agents !== undefined) entry.agents = entry.runner === "prompt" ? this.validateAgents(patch.agents) : undefined
    if (patch.timeoutMs !== undefined) entry.timeoutMs = this.validateTimeout(patch.timeoutMs)
    if (patch.env !== undefined) entry.env = this.validateTaskEnv(patch.env, live.env)
    if (patch.notify !== undefined) entry.notify = this.validateNotify(patch.notify, entry.notify, user)
    if (patch.notifyOn !== undefined) entry.notifyOn = normalizeNotifyWhen(patch.notifyOn) ?? "auto"
    if (patch.maxConsecutiveErrors !== undefined) entry.maxConsecutiveErrors = this.validateMaxConsecutiveErrors(patch.maxConsecutiveErrors)
    if (patch.enabled !== undefined) {
      entry.enabled = patch.enabled
      // 重新启用视为重置失败计数（用户明确干预，连续失败停用语义不应延续）
      if (entry.enabled) entry.consecutiveErrors = 0
    }
    entry.updatedAt = this.now()
    if (entry.kind === "scheduled" && entry.enabled) {
      // 修改周期/内容后严格校验表达式并重算下次执行时间
      const parsed = parseSchedule(entry.schedule!, entry.timezone)
      if (isOneShotSchedule(entry.schedule!) && parsed.next(this.now()) <= this.now()) throw new Error("@at 时间已过去（一次性任务请指定未来时间）")
      entry.nextRunAt = parsed.next(this.now())
    }
    // 停用即退出队列（排队中的执行一并撤销）；仍在队列中的条目按当前 kind/置顶状态重排序
    if (!entry.enabled) {
      this.dropQueueItem(entry.id)
      if (entry.state === "queued") {
        entry.state = "idle"
        entry.queue = undefined
      }
    } else {
      this.reprioritize(entry)
    }
    // 校验全部通过：补丁写回**镜像条目**（持錁状态的调用方与队列视图读到最新值），再落盘
    Object.assign(live, entry)
    await this.persistEntry(user, live)
    // 闲时任务重新启用：回到空闲执行候选（drain 时按队列空闲情况进场）
    if (live.kind === "idle" && live.enabled) await this.drain()
    return this.publicView(live)
  }

  async remove(user: string, id: string): Promise<boolean> {
    if (!this.entryOf(user, id)) return false
    this.dropQueueItem(id)
    return await this.deleteEntry(user, id)
  }

  private entryOf(user: string, id: string): Task | undefined {
    const entry = this.entries.get(id)
    return entry && entry.user === user ? entry : undefined
  }

  private normalizeName(name: unknown): string | undefined {
    return name != null && String(name).trim() ? String(name).trim().slice(0, TASK_NAME_MAX) : undefined
  }

  /** 校验并归一化 prompt 型执行目标参数。 */
  private validateTarget(target: unknown): TaskTarget {
    if (target === undefined || target === null || target === "") return "ephemeral"
    if (target !== "ephemeral" && target !== "sticky" && target !== "session") throw new Error(`无效的执行目标: ${String(target)}（ephemeral/sticky/session）`)
    return target
  }

  private validateAgents(agents: unknown): string[] | undefined {
    if (agents === undefined || agents === null) return undefined
    if (!Array.isArray(agents)) throw new Error("agents 须为子Agent 名单数组")
    const out: string[] = []
    for (const a of agents) {
      const name = String(a ?? "").trim()
      if (!name) continue
      if (!/^[a-z0-9_]+$/.test(name)) throw new Error(`无效的子Agent 名: ${name}`)
      if (this.deps.agentExists && !this.deps.agentExists(name)) throw new Error(`子Agent 不存在: ${name}`)
      if (!out.includes(name)) out.push(name)
    }
    return out.length ? out : undefined
  }

  private validateNotify(notify: unknown, prev: TaskNotifyChannel[] | undefined, user: string): TaskNotifyChannel[] | undefined {
    if (notify === undefined || notify === null) return undefined
    if (!Array.isArray(notify)) throw new Error("notify 须为通知通道数组")
    if (!notify.length) return undefined
    const out: TaskNotifyChannel[] = []
    for (const raw of notify) {
      if (!raw || typeof raw !== "object") throw new Error("通知通道须为 {type,target,secret} 对象")
      const ch = raw as TaskNotifyInput & { at?: unknown }
      const webhookId = ch.webhookId != null ? String(ch.webhookId).trim() : undefined
      if (webhookId && String(ch.target ?? "").trim()) throw new Error("webhook 通道的 webhookId 与 target 二选一（引用已注册 Webhook 或直配 URL）")
      const entry: TaskNotifyChannel = {
        type: ch.type,
        target: String(ch.target ?? "").trim(),
        webhookId,
        secret: ch.secret && ch.secret !== "***" ? String(ch.secret) : undefined,
        // at 名单归一（字符串 id 或 {id,name} → {id,name?}，非法 id 拒绝）
        at: normalizeAtList(ch.at),
      }
      // 引用形态：创建/修改时即校验存在性与归属（接线方注入的解析器；未注入解析器时引用即拒）
      if (webhookId && !this.deps.resolveWebhook?.(webhookId, user)) throw new Error(`webhook 不存在或无权引用: ${webhookId}（REST /api/v1/webhooks 注册后以返回 id 引用）`)
      // 飞书应用消息形态（feishu_chat，或 feishu 通道 target 为群 chat_id）：需服务端飞书应用凭证，创建即拒
      if (entry.type === "feishu_chat" || (entry.type === "feishu" && isFeishuChatId(entry.target ?? ""))) {
        if (!this.deps.notify?.feishuSend) throw new Error("指定飞书群 chat_id 推送需服务端配置飞书应用凭证（GEBAI_FEISHU_APP_ID/GEBAI_FEISHU_APP_SECRET），或改用群机器人 webhook 地址")
      }
      // 掩码 secret（列表回显）视为「保持原值」：按位置回填旧通道密钥
      if (!entry.secret && ch.secret === "***" && prev) {
        const idx = out.length
        if (prev[idx]?.secret) entry.secret = prev[idx].secret
      }
      validateNotifyChannel(entry)
      out.push(entry)
    }
    return out
  }

  private validateTimeout(timeoutMs: unknown): number | undefined {
    if (timeoutMs === undefined || timeoutMs === null) return undefined
    const n = Number(timeoutMs)
    if (!Number.isFinite(n) || n < TASK_TIMEOUT_MIN_MS || n > TASK_TIMEOUT_MAX_MS) {
      throw new Error(`无效的执行超时 timeoutMs（${TASK_TIMEOUT_MIN_MS}~${TASK_TIMEOUT_MAX_MS} 毫秒）`)
    }
    return Math.round(n)
  }

  private validateMaxConsecutiveErrors(v: unknown): number | undefined {
    if (v === undefined || v === null) return undefined
    const n = Number(v)
    if (!Number.isInteger(n) || n < 0 || n > 1000) throw new Error("maxConsecutiveErrors 须为 0~1000 的整数（0=不停用）")
    return n
  }

  /** 任务级环境变量归一：名须为标识符（拒绝 __proto__）、值须为字符串，重复名后写覆盖；空对象视为未配置。
   *  掩码占位：敏感键值在回显时替换为 `***`（与通知 secret 同惯例；前端表单是 env 全集视图，
   *  不改动即原样回传）——据此保留原值，避免掩码被当作新值写回而覆盖真值。 */
  private validateTaskEnv(env: unknown, prev?: Record<string, string>): Record<string, string> | undefined {
    if (env === undefined || env === null) return undefined
    if (typeof env !== "object" || Array.isArray(env)) return undefined
    const out: Record<string, string> = {}
    for (const [k, v] of Object.entries(env as Record<string, unknown>)) {
      if (k === "__proto__" || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(k)) throw new Error(`无效的环境变量名: ${k}`)
      if (typeof v !== "string") throw new Error(`环境变量 ${k} 的值必须是字符串`)
      // 掩码占位回传 + 有原值：保留原值（占位在往返中视为「保持不变」）
      if (MASKED_ENV_VALUE && v === MASKED_ENV_VALUE && prev?.[k] !== undefined) {
        out[k] = prev[k]
        continue
      }
      out[k] = v
    }
    return Object.keys(out).length ? out : undefined
  }

  /** 输出视图（通知密钥脱敏 + 环境变量敏感键掩码——REST/工具回显不泄露 secret）。
   *  掩码用固定占位 `***`（非真值片段）：前端表单回传占位即保留原值，与会话 env 的展示器脱敏不同——
   *  后者仅作展示（不可编辑回存），此处必须可往返。 */
  private publicView(entry: Task): Task {
    const copy = { ...entry }
    if (copy.notify?.length) copy.notify = copy.notify.map((ch) => ({ ...ch, secret: ch.secret ? "***" : undefined }))
    if (copy.env) {
      const masked: Record<string, string> = {}
      for (const [k, v] of Object.entries(copy.env)) masked[k] = isSensitive(k) ? MASKED_ENV_VALUE : v
      copy.env = masked
    }
    return copy
  }

  /** 落盘前封存敏感值（磁盘密文）；非敏感值保持明文，便于直接查看任务配置。 */
  private sealEnv(entry: Task): Task {
    if (!entry.env) return entry
    const out: Record<string, string> = {}
    for (const [k, v] of Object.entries(entry.env)) out[k] = isSensitive(k) ? seal(v) : v
    return { ...entry, env: out }
  }

  /** 读盘后解封敏感值（内存明文；密钥不符/数据损坏的密文原样保留，不阻断加载）。 */
  private unsealEnv(entry: Task): Task {
    if (!entry.env) return entry
    const out: Record<string, string> = {}
    for (const [k, v] of Object.entries(entry.env)) {
      const plain = unseal(v)
      out[k] = plain ?? v
    }
    return { ...entry, env: out }
  }
  // ---- 队列与推进 ----

  /** 入队（幂等：同一任务已在队列中则返回现有位置；运行中/已停用返回原因不入队）。 */
  async enqueue(
    user: string,
    id: string,
    opts: { source: TaskQueueSource; front?: boolean; enqueuedAt?: number },
  ): Promise<{ task: Task; queued: boolean; position?: number; reason?: string } | null> {
    const entry = this.entryOf(user, id)
    if (!entry) return null
    if (!entry.enabled) return { task: this.publicView(entry), queued: false, reason: "任务已停用（启用后可入队）" }
    if (entry.state === "running") return { task: this.publicView(entry), queued: false, reason: "任务正在运行中" }
    if (entry.state === "queued") {
      // 已在队列：置顶请求就地生效
      if (opts.front) {
        const item = this.queue.find((q) => q.taskId === id)
        if (item) {
          item.front = true
          item.priority = this.priorityOf(entry, true)
          this.sortQueue()
          entry.queue = { ...(entry.queue ?? { source: opts.source, enqueuedAt: this.now() }), front: true }
          await this.persistEntry(user, entry)
        }
      }
      return { task: this.publicView(entry), queued: true, position: this.queuePosition(user, id) }
    }
    const enqueuedAt = opts.enqueuedAt ?? this.now()
    await this.pushQueueItem(entry, opts.source, enqueuedAt, opts.front)
    const position = this.queuePosition(user, id)
    await this.drain()
    return { task: this.publicView(entry), queued: true, position }
  }

  /** 把条目放入队列并落盘排队状态（不推进队列：调用方负责 drain；已排队/运行中/停用的条目不动）。 */
  private async pushQueueItem(entry: Task, source: TaskQueueSource, enqueuedAt: number, front?: boolean): Promise<void> {
    entry.state = "queued"
    entry.queue = { source, enqueuedAt, ...(front ? { front: true } : {}) }
    entry.updatedAt = this.now()
    this.queue.push({
      taskId: entry.id,
      user: entry.user,
      kind: entry.kind,
      source,
      priority: this.priorityOf(entry, front),
      enqueuedAt,
      ...(front ? { front: true } : {}),
    })
    this.sortQueue()
    await this.persistEntry(entry.user, entry)
    this.publish(entry, "event.task.queued", {
      id: entry.id,
      kind: entry.kind,
      runner: entry.runner,
      name: entry.name ?? "",
      source,
      position: this.queuePosition(entry.user, entry.id),
    })
    this.publishQueue(entry.user)
  }

  /** 闲时任务进场（drain 调用）：仅当该用户无定时/普通任务排队或运行时，把可执行的闲时任务按清单顺序入队。
   *  顺序优先取外部提供的清单顺序（`idleOrder`，用户级待办按其清单序），缺省按创建时间；
   *  失败重试按 nextRunAt 节流；入队后由 drain 逐个拉起（同时只跑一个）。 */
  private async enqueueIdle(now: number): Promise<void> {
    for (const user of new Set([...this.entries.values()].map((e) => e.user))) {
      if (!this.idleEligible(user)) continue
      const idles = [...this.entries.values()].filter(
        (e) => e.user === user && e.kind === "idle" && e.enabled && e.state === "idle" && (typeof e.nextRunAt !== "number" || e.nextRunAt <= now),
      )
      const order = this.deps.idleOrder?.(user)
      const rank = (e: Task) => {
        const i = order ? order.indexOf(e.id) : -1
        return i < 0 ? Number.MAX_SAFE_INTEGER : i
      }
      idles.sort((a, b) => rank(a) - rank(b) || a.createdAt - b.createdAt || a.id.localeCompare(b.id))
      // 已入队的闲时条目保持既有相对序（enqueuedAt 依次递增，排序稳定）
      let offset = 0
      for (const e of idles) await this.pushQueueItem(e, "idle", now + offset++)
    }
  }

  /** 出队（排队中取消；运行中的任务请用 stop）。 */
  async dequeue(user: string, id: string): Promise<boolean> {
    const entry = this.entryOf(user, id)
    if (!entry || entry.state !== "queued") return false
    this.dropQueueItem(id)
    entry.state = "idle"
    entry.queue = undefined
    entry.updatedAt = this.now()
    await this.persistEntry(user, entry)
    this.publishQueue(user)
    return true
  }

  /** 手动执行：入队（可置顶）。 */
  async run(user: string, id: string, opts: { front?: boolean; source?: TaskQueueSource } = {}): Promise<{ task: Task; queued: boolean; position?: number; reason?: string } | null> {
    return await this.enqueue(user, id, { source: opts.source ?? "manual", front: opts.front === true })
  }

  private priorityOf(entry: Task, front?: boolean): number {
    if (entry.kind === "scheduled") return TASK_PRIORITY_SCHEDULED
    if (entry.kind === "idle") return TASK_PRIORITY_IDLE
    return front ? TASK_PRIORITY_MANUAL_FRONT : TASK_PRIORITY_MANUAL
  }

  private sortQueue(): void {
    // 同级按入队时间 FIFO；**不拿 taskId 兜底**——同毫秒入队时 taskId 比较会打乱入队顺序
    // （Array.sort 稳定，相等项保持 push 顺序，即真实入队次序）
    this.queue.sort((a, b) => a.priority - b.priority || a.enqueuedAt - b.enqueuedAt)
  }

  /** 仅移除内存队列条目（不动任务状态；调用方负责落盘状态变更）。 */
  private dropQueueItem(taskId: string): void {
    const i = this.queue.findIndex((q) => q.taskId === taskId)
    if (i >= 0) this.queue.splice(i, 1)
  }

  /** 就地重排队（kind/置顶变化后同步权重）。 */
  private reprioritize(entry: Task): void {
    const item = this.queue.find((q) => q.taskId === entry.id)
    if (!item) return
    item.kind = entry.kind
    item.priority = this.priorityOf(entry, item.front)
    this.sortQueue()
  }

  /** 启动时按持久化状态重建队列（queued 条目保持相对顺序；停用条目不入队；running 已在归一化时标记中断）。 */
  private rebuildQueue(): void {
    this.queue = []
    for (const entry of this.entries.values()) {
      if (entry.state !== "queued" || !entry.queue || !entry.enabled) continue
      this.queue.push({
        taskId: entry.id,
        user: entry.user,
        kind: entry.kind,
        source: entry.queue.source,
        priority: this.priorityOf(entry, entry.queue.front),
        enqueuedAt: entry.queue.enqueuedAt,
        ...(entry.queue.front ? { front: true } : {}),
      })
    }
    this.sortQueue()
  }

  private queueOf(user: string): QueueItem[] {
    return this.queue.filter((q) => q.user === user)
  }

  private queuePosition(user: string, taskId: string): number | undefined {
    const i = this.queueOf(user).findIndex((q) => q.taskId === taskId)
    return i < 0 ? undefined : i + 1
  }

  private runningCount(user: string): number {
    let n = 0
    for (const e of this.entries.values()) if (e.user === user && e.state === "running") n++
    return n
  }

  /** 闲时任务可执行判定：队列中无定时/普通条目、其任务无运行中、当前无闲时任务在跑、该用户无运行中的会话。 */
  private idleEligible(user: string): boolean {
    for (const q of this.queue) if (q.user === user && q.kind !== "idle") return false
    for (const e of this.entries.values()) {
      if (e.user !== user || e.state !== "running") continue
      return false // 有任意任务在运行（含闲时）：串行执行，本轮不再启动
    }
    if (this.engine?.busyUser(user)) return false
    return true
  }

  /** 队列视图（额度、运行中、排队顺序）。 */
  queueView(user: string): TaskQueueView {
    const items = this.queueOf(user)
    const entries: TaskQueueEntry[] = items.map((it, i) => {
      const e = this.entries.get(it.taskId)
      return {
        taskId: it.taskId,
        user,
        kind: it.kind,
        runner: e?.runner ?? "prompt",
        name: e?.name,
        source: it.source,
        priority: it.priority,
        enqueuedAt: it.enqueuedAt,
        front: it.front,
        sessionId: e ? this.targetSessionOf(e) : undefined,
        waiting: it.waiting,
        position: i + 1,
      }
    })
    const running: TaskQueueView["running"] = []
    for (const e of this.entries.values()) {
      if (e.user !== user || e.state !== "running") continue
      running.push({
        taskId: e.id,
        name: e.name,
        kind: e.kind,
        runner: e.runner,
        sessionId: this.running.get(e.id)?.sessionId,
        startedAt: e.startedAt ?? 0,
      })
    }
    return { user, limit: this.maxConcurrent, running, entries, busy: items.some((it) => it.kind !== "idle") || running.some((r) => r.kind !== "idle") }
  }

  /** 任务的目标会话（队列视图展示用）。 */
  private targetSessionOf(entry: Task): string | undefined {
    if (entry.runner !== "prompt") return undefined
    const target = entry.target ?? "ephemeral"
    if (target === "sticky") return entry.stickySessionId
    if (target === "session") return entry.sessionId ?? entry.originSessionId
    return undefined
  }

  /** 队列推进（tick 与入队后调用）：闲时任务进场 → 额度内启动可执行的队首条目；
   *  条件不满足（目标会话忙/闲时让路）留队等待。 */
  async drain(): Promise<void> {
    if (this.draining) {
      // 推进期间新到的入队/结束信号：标记脏位，本轮结束后立即再跑一轮（不等到下个 tick）
      this.drainWanted = true
      return
    }
    this.draining = true
    try {
      do {
        this.drainWanted = false
        // 闲时任务自动进场仅限跑调度的实例（从实例只服务请求，不主动领闲时活）
        if (this.schedulerActive()) await this.enqueueIdle(this.now())
        for (const user of new Set(this.queue.map((q) => q.user))) {
          const skip = new Set<string>()
          for (;;) {
            if (this.runningCount(user) >= this.maxConcurrent) break
            const item = this.pickNext(user, skip)
            if (!item) break
            const action = await this.startItem(item)
            if (action === "deferred") skip.add(item.taskId)
          }
        }
      } while (this.drainWanted)
    } finally {
      this.draining = false
    }
  }

  /** 取该用户队列中下一个可启动条目（跳过本轮已判定等待的；闲时条目在无资格时终止本轮）。 */
  private pickNext(user: string, skip: Set<string>): QueueItem | undefined {
    for (const item of this.queueOf(user)) {
      if (skip.has(item.taskId)) continue
      const entry = this.entries.get(item.taskId)
      if (!entry || entry.user !== user || entry.state !== "queued") {
        this.dropQueueItem(item.taskId)
        continue
      }
      if (entry.kind === "idle" && !this.idleEligible(user)) return undefined
      return item
    }
    return undefined
  }

  /** 启动一个队列条目：解析执行会话 → 标记运行 → 异步执行（不阻塞队列推进）。 */
  private async startItem(item: QueueItem): Promise<StartAction> {
    const entry = this.entries.get(item.taskId)
    if (!entry || entry.user !== item.user || entry.state !== "queued") {
      this.dropQueueItem(item.taskId)
      return "dropped"
    }
    if (entry.kind === "idle" && !this.idleEligible(entry.user)) return "deferred"
    const manual = item.source === "manual" || item.source === "todo"
    const startedAt = this.now()
    let sessionId: string | undefined
    if (entry.runner === "prompt") {
      const engine = this.engine
      if (!engine) {
        this.dropQueueItem(item.taskId)
        await this.finishRun(entry, { startedAt, status: "error", error: "任务执行引擎未就绪", manual, source: item.source })
        return "dropped"
      }
      try {
        sessionId = await this.resolveSession(entry)
      } catch (err) {
        this.dropQueueItem(item.taskId)
        await this.finishRun(entry, { startedAt, status: "error", error: String((err as Error).message || err).slice(0, 500), manual, source: item.source })
        return "dropped"
      }
      if (sessionId && engine.isRunning(sessionId)) {
        // 目标会话忙（用户正在该会话对话 / 该会话已有任务）：留队等待，不占额度、不打断
        this.dropQueueItem(item.taskId)
        this.queue.push({ ...item, waiting: "目标会话正在运行，等待中" })
        this.sortQueue()
        this.publishQueue(entry.user)
        return "deferred"
      }
    }
    this.dropQueueItem(item.taskId)
    entry.state = "running"
    entry.startedAt = startedAt
    entry.queue = undefined
    entry.updatedAt = startedAt
    this.running.set(entry.id, { sessionId, startedAt, manual, source: item.source })
    // 启动落盘失败（目录被移除/磁盘满）不阻断执行：同 finishRun，调度不因落盘降级而停摆
    try {
      await this.persistEntry(entry.user, entry)
    } catch (err) {
      log.warn(`[tasks] 任务 ${entry.id} 启动落盘失败（内存态已更新）：${String((err as Error)?.message ?? err).slice(0, 300)}`)
    }
    this.publish(entry, "event.task.start", {
      id: entry.id,
      kind: entry.kind,
      runner: entry.runner,
      name: entry.name ?? "",
      source: item.source,
      sessionId,
    })
    this.publishQueue(entry.user)
    // 执行链兜底：收尾抛出的异常不得成为进程级未捕获 rejection，也不能让任务态停在「运行中」
    void this.execute(entry, sessionId, startedAt, item.source).catch((err) => this.abortRun(entry, err))
    return "started"
  }

  /** 执行链异常兜底：清运行标记、发结果事件、推进队列（已收尾时只记日志）。 */
  private abortRun(entry: Task, err: unknown): void {
    const msg = String((err as Error)?.message ?? err).slice(0, 300)
    log.warn(`[tasks] 任务 ${entry.id}「${entry.name ?? ""}」执行链异常：${msg}`)
    if (entry.state !== "running") return
    entry.state = "idle"
    entry.startedAt = undefined
    entry.lastStatus = "error"
    entry.lastError = `执行链异常：${msg}`
    this.running.delete(entry.id)
    this.publish(entry, "event.task.result", {
      id: entry.id,
      kind: entry.kind,
      runner: entry.runner,
      name: entry.name ?? "",
      ok: false,
      status: "error",
      error: entry.lastError,
    })
    this.publishQueue(entry.user)
    void this.drain()
  }

  /** tick：到期定时任务入队 + 队列推进（循环与测试共用入口）。 */
  async tick(): Promise<void> {
    const now = this.now()
    for (const entry of [...this.entries.values()]) {
      if (entry.kind !== "scheduled" || !entry.enabled) continue
      if (typeof entry.nextRunAt !== "number" || entry.nextRunAt > now) continue
      try {
        await this.enqueueDue(entry, now)
      } catch (err) {
        // 入队失败（存储异常等）：必须重算下次执行时间，否则 nextRunAt 停留在过去 →
        // 每个 tick 都会重试失败任务，形成无限重试热循环；一次性任务（@at）无法重算未来时间——直接停用
        entry.lastError = String((err as Error).message || err).slice(0, 500)
        entry.lastStatus = "error"
        entry.updatedAt = now
        entry.state = "idle"
        entry.queue = undefined
        this.dropQueueItem(entry.id)
        if (isOneShotSchedule(entry.schedule ?? "")) {
          entry.enabled = false
          entry.nextRunAt = undefined
        } else {
          entry.nextRunAt = this.nextTime(entry, now)
        }
        await this.persistEntry(entry.user, entry).catch(() => {})
      }
    }
    await this.drain()
  }

  /** 定时任务到期：推进下次执行时间（执行排队不阻塞后续调度）后入队队首。 */
  private async enqueueDue(entry: Task, now: number): Promise<void> {
    // 一次性任务（@at）无未来触发点：入队后清 nextRunAt（tick 不再命中），执行结束再停用
    if (isOneShotSchedule(entry.schedule ?? "")) {
      entry.nextRunAt = undefined
    } else {
      entry.nextRunAt = this.nextTime(entry, now)
    }
    if (entry.state === "running" || entry.state === "queued") {
      // 上次尚未结束（或仍在排队）：本轮跳过并留痕，不并发叠加
      const reason = entry.state === "running" ? "上次执行尚未结束，本轮跳过" : "已在队列中排队，本轮跳过"
      const rec: TaskRunRecord = { id: randomUUID(), at: now, endedAt: now, status: "skipped", durationMs: 0, reason }
      await this.saveRun(entry, rec)
      entry.lastStatus = "skipped"
      entry.lastError = reason
      entry.updatedAt = now
      await this.persistEntry(entry.user, entry)
      this.publish(entry, "event.task.result", { id: entry.id, kind: entry.kind, name: entry.name ?? "", ok: false, status: "skipped", reason })
      return
    }
    entry.updatedAt = now
    await this.persistEntry(entry.user, entry)
    await this.enqueue(entry.user, entry.id, { source: "schedule", enqueuedAt: now })
  }

  private nextTime(entry: Task, fromMs: number): number {
    // 解析失败按「下个 tick 再试」回退（非法表达式在加载/更新时已禁用，此处仅兜底）
    try {
      return parseSchedule(entry.schedule ?? "", entry.timezone).next(fromMs)
    } catch {
      return fromMs + TASK_TICK_INTERVAL_MS
    }
  }
  // ---- 执行 ----

  /** 解析 prompt 型执行会话：ephemeral 每次新建（可选预载子Agent）；sticky 专用会话惰性创建并复用；
   *  session 用绑定会话（已删除则自愈降级为新建会话）。 */
  private async resolveSession(entry: Task): Promise<string | undefined> {
    const store = this.deps.store
    const target = entry.target ?? "ephemeral"
    if (target === "session") {
      const sid = entry.sessionId ?? entry.originSessionId
      if (sid && existsSync(join(sessionPath(this.deps.home, entry.user, sid), "chat.json"))) return sid
      // 绑定会话已删除：自愈降级为独立会话（任务保留，一次性记因）
      entry.target = "ephemeral"
      entry.sessionId = undefined
      entry.lastError = "绑定会话已删除，本次起改为独立会话执行"
    } else if (target === "sticky" && entry.stickySessionId && existsSync(join(sessionPath(this.deps.home, entry.user, entry.stickySessionId), "chat.json"))) {
      return entry.stickySessionId
    }
    const session = await store.createSession(entry.user, this.sessionTitle(entry))
    // 预载子Agent：写入会话装载名单，engine.run 的装载保障按此注册工具与提示词；
    // 通知通道可用时追加 task 子Agent——执行中的模型据此主动决定/自撰通知（task_notify），不改任务自身配置
    const agents = [...(entry.agents ?? [])]
    if (this.taskNotifyAvailable(entry) && !agents.includes("task")) agents.push("task")
    if (agents.length) {
      session.loadedSubAgents = agents
      await store.save(session)
    }
    if ((entry.target ?? "ephemeral") === "sticky") entry.stickySessionId = session.id
    return session.id
  }

  /** 执行会话/结果消息标题（任务类别 + 任务名或短 id）。 */
  private sessionTitle(entry: Task): string {
    const label = entry.kind === "scheduled" ? "定时任务" : entry.kind === "idle" ? "闲时任务" : "普通任务"
    return `${label}${entry.name ? `「${entry.name}」` : `(${entry.id.slice(0, 8)})`}`
  }

  /** 执行一次（script 直跑 / prompt 触发会话），完成后统一收尾。 */
  private async execute(entry: Task, sessionId: string | undefined, startedAt: number, source: TaskQueueSource): Promise<void> {
    const manual = source === "manual" || source === "todo"
    let status: TaskRunStatus = "success"
    let output = ""
    let error: string | undefined
    let runSessionId = sessionId
    if (entry.runner === "script") {
      // 安全模式：script 型任务直接执行 shell（不经工具拦截），已建任务同样阻止——跳过并留痕
      if (this.deps.safeMode) {
        status = "skipped"
        output = "安全模式：脚本执行已限制"
        error = "safe-mode"
        await this.appendOriginMessage(entry, `${agentNoteHead(`${this.sessionTitle(entry)}已跳过`)} 安全模式：脚本执行已限制（安全模式下仅允许只读操作）。`, startedAt)
      } else {
        // 工作目录保证存在（任务资源目录，跨次运行保留产物）
        const cwd = this.workspaceOf(entry)
        await mkdir(cwd, { recursive: true }).catch(() => {})
        // 会话环境尽力解析（sticky/来源会话已删除则退回进程环境）：任务不依赖会话存活
        const envRef = entry.stickySessionId ?? entry.originSessionId
        let env: Record<string, string>
        try {
          env = envRef ? await this.deps.env.resolve(envRef, entry.user) : processEnvSnapshot()
        } catch {
          env = processEnvSnapshot()
        }
        // 任务级环境变量最后合并（优先级最高，且不依赖浏览器本地注入与会话存活）
        if (entry.env) env = { ...env, ...entry.env }
        try {
          const { stdout, stderr, code } = await this.deps.sandbox.exec(entry.script ?? "", {
            cwd,
            env,
            timeoutMs: entry.timeoutMs ?? TASK_SCRIPT_TIMEOUT_MS,
            user: entry.user,
          })
          const out = code === 0 ? stdout : `${stdout}${stdout && stderr ? "\n" : ""}${stderr}\n[exit ${code}]`
          const ok = code === 0
          status = ok ? "success" : "error"
          output = out.slice(0, TASK_OUTPUT_MAX)
          error = ok ? undefined : `exit ${code}`
          await this.appendOriginMessage(entry, `${agentNoteHead(`${this.sessionTitle(entry)}执行结果（${ok ? "成功" : "失败"}）`)}\n${out}`.slice(0, TASK_MESSAGE_MAX), startedAt)
        } catch (err) {
          status = "error"
          error = String((err as Error).message || err).slice(0, 500)
          await this.appendOriginMessage(entry, `${agentNoteHead(`${this.sessionTitle(entry)}执行失败`)}\n${error}`.slice(0, TASK_MESSAGE_MAX), startedAt)
        }
      }
    } else {
      const engine = this.engine
      if (!engine || !runSessionId) {
        status = "error"
        error = "任务执行引擎未就绪"
      } else {
        const sid = runSessionId
        // 无人值守执行（ephemeral/sticky：无人盯着执行会话）按**无交互通道**运行：**需审批工具自动
        // 通过**（含服务模式）——任务经用户审批创建，执行即其授权的无人值守落地（与 REST autoApprove:true
        // 同一授权面）；不空等 5 分钟超时后跳过。ask/show/page_capture 等依赖前端的
        // 能力按 none 语义降级。target=session 绑定用户会话（可能有人在场审批），保持 realtime。
        const unattended = (entry.target ?? "ephemeral") !== "session"
        const hints: string[] = []
        if (unattended) {
          hints.push(
            "本次为无人值守执行（无交互通道）：不要依赖询问用户与前端渲染（ask、show 的页面预览、页面捕获不可用）；" +
              "需审批工具自动通过（任务创建时已经过审批判定）——不需要为工具审批等待或重试。",
          )
        }
        // 通知通道可用时（resolveSession 已预载 task）补一行执行上下文：任务 ID + task_notify 用法，
        // 让执行会话的模型主动决定是否通知用户（notifyOn=model 时通知完全由模型决定）
        if (this.taskNotifyAvailable(entry)) {
          hints.push(`本次执行的任务 ID: ${entry.id}；需要用户知晓结果时用 task_notify 推送自撰通知（不传 id 即本任务），例行正常可保持静默；执行任务期间不要用 task 的其他工具管理任务。`)
        }
        const hintText = hints.length ? `\n\n（${hints.join("")}）` : ""
        const promptText = `${agentNoteHead(`${this.sessionTitle(entry)}触发`)}\n${entry.prompt ?? ""}${hintText}`
        const timeoutMs = entry.timeoutMs ?? TASK_PROMPT_TIMEOUT_MS
        let timedOut = false
        // 注意不可 unref：await 挂起的 Promise 不保活事件循环，unref 定时器在「仅剩本定时器」场景
        // （测试/空闲进程）永不触发；finally 必 clear，无泄漏
        const timer = setTimeout(() => {
          timedOut = true
          // 超时先「快速结束」运行中的子会话（注入收敛指令让模型按已有信息给出结论，宽限逾期才强制终止），
          // 再取消本会话任务——直接硬杀会把子会话已跑出的结论一并丢掉
          void engine.windDown(sid, { reason: `任务执行超时（${Math.round(timeoutMs / 1000)}s）` })
        }, timeoutMs)
        let runError: string | undefined
        try {
          await engine.run(sid, entry.user, promptText, {
            interactionMode: unattended ? "none" : "realtime",
            // 任务级环境变量注入执行会话（模型 Provider / 脚本子进程 / 子Agent 环境读取一并生效）
            ...(entry.env ? { envOverride: entry.env } : {}),
            // 无人值守任务免审批（含服务模式）：任务经用户审批创建，执行即其授权的无人值守落地；
            // target=session 可能有人在场，保持实时审批
            ...(unattended ? { autoApprove: true } : {}),
          })
        } catch (err) {
          // 超时主动取消的拒绝不算异常（按 timeout 记录）；其余运行失败记为本次运行 error
          if (!timedOut) runError = String((err as Error).message || err).slice(0, 500)
        } finally {
          clearTimeout(timer)
        }
        if (timedOut) {
          status = "timeout"
          error = `执行超时（${Math.round(timeoutMs / 1000)}s），已终止`
        } else if (runError) {
          status = "error"
          error = runError
        } else {
          status = "success"
        }
        output = (await this.lastAssistantText(sid, entry.user)) ?? (status === "success" ? "已触发，执行过程与结果见会话消息" : "")
      }
    }
    await this.finishRun(entry, { startedAt, status, output, error, sessionId: runSessionId, manual, source })
  }

  /** 运行收尾：状态/历史/失败计数与自动停用/通知/事件/待办联动/一次性任务清理，随后继续推进队列。 */
  private async finishRun(
    entry: Task,
    r: { startedAt: number; status: TaskRunStatus; output?: string; error?: string; sessionId?: string; manual: boolean; source: TaskQueueSource },
  ): Promise<void> {
    const endedAt = this.now()
    const ok = r.status === "success"
    let disabled = false
    entry.state = "idle"
    entry.startedAt = undefined
    entry.queue = undefined
    entry.lastStatus = r.status
    entry.lastOutput = (r.output ?? "").slice(0, TASK_OUTPUT_MAX) || undefined
    entry.lastError = r.error
    entry.lastRunAt = r.startedAt
    entry.runCount += 1
    if (r.status === "error" || r.status === "timeout") {
      entry.consecutiveErrors = (entry.consecutiveErrors ?? 0) + 1
      const max = entry.maxConsecutiveErrors ?? 0
      if (max > 0 && entry.consecutiveErrors >= max) {
        entry.enabled = false
        disabled = true
        entry.lastError = `${r.error ?? "连续失败"}；连续失败 ${entry.consecutiveErrors} 次，已自动停用（修正后可重新启用）`
      }
    } else if (ok) {
      entry.consecutiveErrors = 0
    }
    // 闲时任务：执行成功即完成使命（自动停用，等待显式重新启用——待办重开开关或手动执行会自动恢复）；
    // 失败/超时按 tick 周期节流后再试，避免每轮 drain 立即重试
    if (entry.kind === "idle") {
      if (ok) {
        entry.enabled = false
        disabled = true
      } else {
        entry.nextRunAt = endedAt + TASK_TICK_INTERVAL_MS
      }
    }
    // 一次性定时任务（@at）：触发并入队后即完成调度使命，执行结束停用（不再重算时间）
    if (entry.kind === "scheduled" && isOneShotSchedule(entry.schedule ?? "")) {
      entry.enabled = false
      entry.nextRunAt = undefined
      disabled = true
    }
    const rec: TaskRunRecord = {
      id: randomUUID(),
      at: r.startedAt,
      endedAt,
      status: r.status,
      durationMs: Math.max(0, endedAt - r.startedAt),
      output: entry.lastOutput,
      error: entry.lastError,
      sessionId: r.sessionId,
      manual: r.manual || undefined,
    }
    await this.saveRun(entry, rec)
    entry.updatedAt = endedAt
    this.running.delete(entry.id)
    // 一次性任务（待办立即执行生成）：执行完毕即清理，不占用任务清单
    const ephemeral = entry.ephemeral === true
    // 落盘失败（目录被移除/磁盘满/权限）只降级为告警：收尾链必须走完（结果事件、队列推进、通知），
    // 否则异常逃逸为进程级未捕获 rejection；内存态已更新，落盘仅影响重启后的恢复
    try {
      if (ephemeral) await this.deleteEntry(entry.user, entry.id)
      else await this.persistEntry(entry.user, entry)
    } catch (err) {
      log.warn(`[tasks] 任务 ${entry.id} 收尾落盘失败（内存态已更新，重启后状态可能回退）：${String((err as Error)?.message ?? err).slice(0, 300)}`)
    }
    this.publish(entry, "event.task.result", {
      id: entry.id,
      kind: entry.kind,
      runner: entry.runner,
      name: entry.name ?? "",
      ok,
      status: r.status,
      output: entry.lastOutput,
      error: entry.lastError,
      sessionId: r.sessionId,
      manual: r.manual || undefined,
      disabled: disabled || undefined,
    })
    this.publishQueue(entry.user)
    if (this.deps.onTaskFinished) {
      try {
        await this.deps.onTaskFinished(this.publicView(entry), rec)
      } catch {
        /* 待办联动失败不影响任务执行结果 */
      }
    }
    if (!ephemeral) await this.dispatchNotify(entry, rec, disabled)
    void this.drain()
  }

  /** 出队（排队中取消）。 */
  async cancel(user: string, id: string): Promise<boolean> {
    return await this.dequeue(user, id)
  }

  /** 终止运行中的任务：取消其执行会话（session 型执行经 engine.cancel 走统一中断收口）。
   *  脚本型任务不经会话，只能等其自身结束或超时（sandbox 不支持中断）。 */
  async stopRun(user: string, id: string): Promise<boolean> {
    const entry = this.entryOf(user, id)
    if (!entry) return false
    if (entry.state === "queued") return await this.dequeue(user, id)
    if (entry.state !== "running") return false
    const run = this.running.get(id)
    if (run?.sessionId && this.engine) {
      try {
        await this.engine.cancel(run.sessionId)
      } catch {
        /* 取消失败（会话已结束等）不影响「已发出终止请求」的语义 */
      }
    }
    return true
  }

  /** 主动推送通知（task_notify / TaskService.notify）：投递到任务的 notify 通道（未配置回落全局默认通道）。
   *  id 缺省时按执行会话（opts.sessionId）反查正在运行的任务——模型执行任务时无需回显任务 ID。
   *  投递目标限定为用户已配置的通道（不接受调用方传入任意 URL），安全模式下拒绝；尽力而为，失败不影响任务本身。 */
  async notify(user: string, id: string | undefined, input: TaskNotifyMessage, opts: { sessionId?: string } = {}): Promise<TaskNotifyResult> {
    const entry = id ? this.entryOf(user, id) : this.runningEntryOfSession(user, opts.sessionId)
    if (!entry) throw new Error(id ? `任务不存在: ${id}` : "未指定任务 ID，且当前会话没有正在运行的任务（用 task_list 查看任务后传 id）")
    const channels = entry.notify?.length ? entry.notify : this.deps.defaultNotify
    if (!channels?.length) {
      throw new Error(`任务「${entry.name ?? entry.id}」未配置通知通道，且无全局默认通道（先用 task_update 配置 notify，或让服务端配置 GEBAI_TASK_NOTIFY_WEBHOOK / GEBAI_TASK_NOTIFY_FEISHU）`)
    }
    if (this.deps.safeMode) throw new Error("安全模式：通知投递已限制")
    const text = String(input.text ?? "").trim()
    if (!text) throw new Error("通知正文不能为空")
    // at 名单：调用方指定则覆盖通道自带 @ 配置（模型按需 @ 人）；非法 id 由归一化拒绝
    const at = input.at !== undefined ? normalizeAtList(input.at) : undefined
    const message: TaskMessageNotification = {
      event: "task.message",
      task: { id: entry.id, name: entry.name ?? "", kind: entry.kind, runner: entry.runner, user: entry.user },
      title: input.title != null && String(input.title).trim() ? String(input.title).trim() : undefined,
      text: text.slice(0, NOTIFY_TEXT_MAX),
      at: this.now(),
    }
    const errors: string[] = []
    let delivered = 0
    for (const ch of channels) {
      try {
        // webhookId 引用形态：投递时解析注册 Webhook 的 URL 与签名密钥（同 dispatchNotify：引用消失记错误跳过）
        let effective = ch
        if (ch.webhookId) {
          const resolved = this.deps.resolveWebhook?.(ch.webhookId, entry.user)
          if (!resolved) throw new Error(`webhook 引用不可用: ${ch.webhookId}`)
          effective = { ...ch, target: resolved.url, secret: resolved.secret ?? ch.secret }
        }
        await sendTaskMessage(at !== undefined ? { ...effective, at } : effective, message, this.deps.notify)
        delivered += 1
      } catch (err) {
        errors.push(`${ch.type}: ${String((err as Error).message || err).slice(0, 200)}`)
      }
    }
    entry.lastNotifyError = errors.length ? errors.join("；").slice(0, 500) : undefined
    if (!entry.ephemeral) await this.persistEntry(entry.user, entry).catch(() => {})
    return { taskId: entry.id, delivered, errors }
  }

  /** 按执行会话反查正在运行的任务（主动通知未指定任务 ID 时的推断路径）。 */
  private runningEntryOfSession(user: string, sessionId?: string): Task | undefined {
    if (!sessionId) return undefined
    for (const [id, run] of this.running) {
      if (run.sessionId !== sessionId) continue
      const entry = this.entryOf(user, id)
      if (entry) return entry
    }
    return undefined
  }

  /** 该任务是否存在可用通知通道（自配 notify 或全局默认通道）。 */
  private hasNotifyChannel(entry: Task): boolean {
    return (entry.notify?.length ?? 0) > 0 || (this.deps.defaultNotify?.length ?? 0) > 0
  }

  /** 执行会话是否具备主动通知能力（有通道且 task 子Agent 可用）——据此预载 task 并注入使用提示。 */
  private taskNotifyAvailable(entry: Task): boolean {
    return this.hasNotifyChannel(entry) && this.deps.agentExists?.("task") === true
  }

  /** 通知投递（auto=执行结束自动把结果摘要/最后回复发出；尽力而为：失败记 lastNotifyError，
   *  不影响执行结果与调度）。任务未配自己的 notify 时回落全局默认通道（环境变量配置，不写入任务数据）。
   *  notifyOn=model 时不自动投递：通知由执行会话的模型经 task_notify 自主决定与撰写。 */
  private async dispatchNotify(entry: Task, rec: TaskRunRecord, disabled: boolean): Promise<void> {
    if (entry.notifyOn === "model") return
    const channels = entry.notify?.length ? entry.notify : this.deps.defaultNotify
    if (!channels?.length) return
    const ok = rec.status === "success"
    if (this.deps.safeMode) {
      entry.lastNotifyError = "安全模式：通知投递已限制"
      return
    }
    const errors: string[] = []
    for (const ch of channels) {
      try {
        // webhookId 引用形态：投递时解析注册 Webhook 的 URL 与签名密钥（注册侧改 URL/密钥即时生效；
        // 引用消失（被删除/失去归属）记通知错误跳过，不影响其余通道与任务）
        let effective = ch
        if (ch.webhookId) {
          const resolved = this.deps.resolveWebhook?.(ch.webhookId, entry.user)
          if (!resolved) throw new Error(`webhook 引用不可用: ${ch.webhookId}`)
          effective = { ...ch, target: resolved.url, secret: resolved.secret ?? ch.secret }
        }
        await sendTaskNotification(
          effective,
          {
            event: "task.result",
            task: { id: entry.id, name: entry.name ?? "", kind: entry.kind, runner: entry.runner, schedule: entry.schedule, user: entry.user },
            ok,
            status: rec.status,
            at: rec.at,
            durationMs: rec.durationMs,
            output: entry.lastOutput,
            error: entry.lastError,
            sessionId: rec.sessionId,
            disabled: disabled || undefined,
            manual: rec.manual,
          },
          this.deps.notify,
        )
      } catch (err) {
        errors.push(`${ch.type}: ${String((err as Error).message || err).slice(0, 200)}`)
      }
    }
    entry.lastNotifyError = errors.length ? errors.join("；").slice(0, 500) : undefined
    if (!entry.ephemeral) await this.persistEntry(entry.user, entry).catch(() => {})
  }

  /** prompt 运行结果摘要：执行会话最后一条 assistant 消息。 */
  private async lastAssistantText(sessionId: string, user: string): Promise<string | undefined> {
    try {
      const session = await this.deps.store.load(sessionId, user)
      const msg = session ? [...session.messages].reverse().find((m) => m.role === "assistant" && typeof m.content === "string") : undefined
      return msg?.content ? msg.content.slice(0, TASK_OUTPUT_MAX) : undefined
    } catch {
      return undefined
    }
  }

  /** 结果消息写回来源会话（会话仍存在时；历史可见、模型可感知，会话删除则静默跳过）。
   *  角色为 **user + engineNote: "task"**：与引擎提醒同规则——思考类模型不接受以 assistant 结尾的请求
   *  （写回后该消息若成为尾消息，会话下次带工具面的请求会被 400 拒绝；实测），且标记供 UI 渲染为
   *  「任务」通知条（与用户自己发的消息区分）；内容头带 `【智体·…】` 身份标记（见「引擎注入消息的角色约定」）。 */
  private async appendOriginMessage(entry: Task, content: string, now: number): Promise<void> {
    const sid = entry.originSessionId
    if (!sid) return
    if (!existsSync(join(sessionPath(this.deps.home, entry.user, sid), "chat.json"))) return
    await this.deps.store
      .appendMessage(sid, { id: randomUUID(), role: "user", content, engineNote: "task", createdAt: now }, entry.user)
      .catch(() => {})
  }

  private publish(entry: Task, type: string, payload: Record<string, unknown>): void {
    // 事件按任务绑定会话路由（来源会话已删除时广播到全局 sessionId 占位）
    const sessionId = entry.originSessionId ?? entry.stickySessionId ?? "task"
    this.deps.events.publish({ type, sessionId, payload, timestamp: this.now() } as AgentEvent)
  }

  /** 队列变化事件（前端任务视图与队列面板据此刷新）。 */
  private publishQueue(user: string): void {
    const view = this.queueView(user)
    this.deps.events.publish({
      type: "event.task.queue",
      sessionId: "task",
      payload: { user, limit: view.limit, queued: view.entries.length, running: view.running.length },
      timestamp: this.now(),
    } as AgentEvent)
  }
  // ---- 任务资源文件（脚本/文档） ----

  /** 任务资源目录文件清单（递归，含目录项）。 */
  async files(user: string, id: string): Promise<TaskFileEntry[]> {
    const entry = this.requireEntry(user, id)
    const root = this.workspaceOf(entry)
    const out: TaskFileEntry[] = []
    await collectFiles(root, "", out, 0)
    return out.sort((a, b) => a.path.localeCompare(b.path))
  }

  /** 读取任务资源文件（UTF-8 文本，上限 TASK_FILE_MAX_BYTES）。 */
  async readFile(user: string, id: string, path: string): Promise<string> {
    const entry = this.requireEntry(user, id)
    const p = this.resolveInWorkspace(entry, path)
    const info = await stat(p).catch(() => null)
    if (!info?.isFile()) throw new Error(`文件不存在: ${path}`)
    if (info.size > TASK_FILE_MAX_BYTES) throw new Error(`文件过大（上限 ${TASK_FILE_MAX_BYTES} 字节）: ${path}`)
    return await readFileFs(p, "utf8")
  }

  /** 写入（覆盖）任务资源文件。 */
  async writeFile(user: string, id: string, path: string, content: string): Promise<TaskFileEntry> {
    const entry = this.requireEntry(user, id)
    const p = this.resolveInWorkspace(entry, path)
    if (p === this.workspaceOf(entry)) throw new Error("缺少文件路径")
    const text = String(content ?? "")
    if (Buffer.byteLength(text, "utf8") > TASK_FILE_MAX_BYTES) throw new Error(`文件过大（上限 ${TASK_FILE_MAX_BYTES} 字节）: ${path}`)
    await mkdir(dirname(p), { recursive: true })
    await writeFileFs(p, text, "utf8")
    const info = await stat(p)
    return { path: toPosix(relative(this.workspaceOf(entry), p)), size: info.size, mtimeMs: info.mtimeMs, dir: false }
  }

  /** 删除任务资源文件或目录（递归）。 */
  async deleteFile(user: string, id: string, path: string): Promise<boolean> {
    const entry = this.requireEntry(user, id)
    const root = this.workspaceOf(entry)
    const p = this.resolveInWorkspace(entry, path)
    if (p === root) throw new Error("不能删除任务资源目录本身")
    const info = await stat(p).catch(() => null)
    if (!info) return false
    await rm(p, { recursive: true, force: true })
    return true
  }

  private requireEntry(user: string, id: string): Task {
    const entry = this.entryOf(user, id)
    if (!entry) throw new Error(`任务不存在: ${id}`)
    return entry
  }

  /** 任务资源目录内的路径白名单：解析后必须落在该任务目录内（越界/穿越一律拒绝）。 */
  private resolveInWorkspace(entry: Task, rel: string): string {
    const root = this.workspaceOf(entry)
    const clean = String(rel ?? "").replace(/\\/g, "/").replace(/^\/+/, "")
    const p = pathResolve(root, clean)
    const prefix = root.endsWith(sep) ? root : `${root}${sep}`
    if (p !== root && !p.startsWith(prefix)) throw new Error(`路径越界（仅限任务资源目录内）: ${rel}`)
    return p
  }

  // ---- 执行记录（按文件落盘，见 task-runs.ts） ----

  /** 落盘一条执行记录（含超上限清理）。失败只告警不上抛：执行结果与调度不得因记录写入失败而降级。 */
  private async saveRun(entry: Pick<Task, "id" | "user">, rec: TaskRunRecord): Promise<void> {
    try {
      await writeTaskRun(this.deps.home, entry.user, entry.id, rec)
      await trimTaskRuns(this.deps.home, entry.user, entry.id, TASK_RUNS_KEEP)
    } catch (err) {
      log.warn(`[tasks] 任务 ${entry.id} 执行记录落盘失败：${String((err as Error)?.message ?? err).slice(0, 300)}`)
    }
  }

  /** 读取任务执行记录（新→旧，最多 limit 条，缺省 TASK_RUNS_KEEP）。 */
  async runs(user: string, id: string, limit?: number): Promise<TaskRunRecord[]> {
    if (!this.entryOf(user, id)) throw new Error(`任务不存在: ${id}`)
    return await listTaskRuns(this.deps.home, user, id, limit ?? TASK_RUNS_KEEP)
  }

  // ---- 持久化（磁盘真值 RMW） ----

  /** 以某用户**磁盘真值**为基准落盘（跨进程写锁 + 原子写 + 滚动备份）。forceWrite 用于结构沉降
   *  （如把旧版内联的 `runs` 字段从定义文件里清掉——归一化已剥离，恒等变更默认会被跳过）。 */
  private async persist(user: string, mutate: (disk: Task[]) => Task[], opts: { forceWrite?: boolean } = {}): Promise<Task[]> {
    // 磁盘真值为**密文态**（内存态为明文）：读取时解封供变更函数使用，写入前封存回密文
    const next = await mutateJsonList(this.userTaskFile(user), (disk) => mutate(disk.map((e) => this.unsealEnv(e))).map((e) => this.sealEnv(e)), {
      normalize: (raw) => {
        const entry = this.normalizeEntry(raw)
        return entry ? this.unsealEnv(entry) : null
      },
      forceWrite: opts.forceWrite,
    })
    return next.map((e) => this.unsealEnv(e))
  }

  /** 单条 upsert：以磁盘真值为基准合并本条改动（其余条目原样保留，磁盘上本进程未知的条目也不会丢）。
   *  落盘后把真值回填本地镜像与 `entry` 自身（执行结束后的 publish/notify 读到的是真值）。 */
  private async persistEntry(user: string, entry: Task): Promise<void> {
    const next = await this.persist(user, (disk) => {
      const i = disk.findIndex((e) => e.id === entry.id)
      if (i < 0) return [...disk, { ...entry }]
      const merged = disk.slice()
      merged[i] = { ...entry }
      return merged
    })
    this.replaceUserEntries(user, next)
    const fresh = next.find((e) => e.id === entry.id)
    if (fresh) Object.assign(entry, fresh)
  }

  /** 单条删除（磁盘真值与本地镜像同步）。 */
  private async deleteEntry(user: string, id: string): Promise<boolean> {
    const next = await this.persist(user, (disk) => disk.filter((e) => e.id !== id))
    const removed = !next.some((e) => e.id === id)
    this.replaceUserEntries(user, next)
    return removed
  }

  /** 用落盘真值同步本地镜像（该用户条目换成磁盘真值；其他用户条目保持原相对序）。
   *  磁盘上新增的条目（其他实例写入）一并纳入，本进程可见。
   *  **保持已有条目的对象标识**（只就地改写字段）：调用方与执行流程持有的是条目引用。
   *  磁盘条目**不比内存新**（updatedAt 不更大）时保留内存不变：本进程刚写入/正在收尾的任务
   *  不能被并发写路径读到的旧磁盘值回退（否则 in-flight 的 state/runCount 会被改回旧值）。 */
  private replaceUserEntries(user: string, list: Task[]): void {
    const byId = new Map(list.map((e) => [e.id, e]))
    for (const [key, value] of this.entries) {
      if (value.user !== user) continue
      const fresh = byId.get(key)
      if (!fresh) {
        this.entries.delete(key)
        continue
      }
      if (typeof fresh.updatedAt === "number" && typeof value.updatedAt === "number" && fresh.updatedAt <= value.updatedAt) {
        byId.delete(key)
        continue
      }
      Object.assign(value, fresh)
      byId.delete(key)
    }
    for (const e of byId.values()) if (!this.entries.has(e.id)) this.entries.set(e.id, e)
  }
}

/** 进程环境快照（剔除 undefined 值，收敛为 Record<string,string>）。 */
function processEnvSnapshot(): Record<string, string> {
  const out: Record<string, string> = {}
  for (const [k, v] of Object.entries(process.env)) if (v !== undefined) out[k] = v
  return out
}

/** 路径统一为 POSIX 分隔符（对外路径形态与平台无关）。 */
function toPosix(p: string): string {
  return p.split(sep).join("/")
}

/** 递归收集任务资源目录下的文件（深度上限 6；跳过符号链接）。 */
async function collectFiles(root: string, prefix: string, out: TaskFileEntry[], depth: number): Promise<void> {
  if (depth > 6) return
  let items: Dirent[]
  try {
    items = await readdir(join(root, prefix), { withFileTypes: true })
  } catch {
    return
  }
  for (const it of items) {
    const rel = prefix ? `${prefix}/${it.name}` : it.name
    if (it.isSymbolicLink()) continue
    if (it.isDirectory()) {
      out.push({ path: rel, size: 0, mtimeMs: 0, dir: true })
      await collectFiles(root, rel, out, depth + 1)
      continue
    }
    if (!it.isFile()) continue
    const info = await stat(join(root, rel)).catch(() => null)
    out.push({ path: rel, size: info?.size ?? 0, mtimeMs: info?.mtimeMs ?? 0, dir: false })
  }
}
