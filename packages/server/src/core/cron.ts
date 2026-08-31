import { randomUUID } from "node:crypto"
import { existsSync } from "node:fs"
import { join, dirname, relative, sep } from "node:path"
import { mkdir, readFile, writeFile, rename } from "node:fs/promises"
import type { AgentEvent } from "@gebai/sdk"
import type { AgentEngine } from "./engine"
import type { SessionStore } from "./store"
import type { EnvManager } from "./env"
import type { Sandbox } from "./sandbox"
import type { EventBus } from "./event-bus"
import type { CronNotifyChannel, CronResultNotification, FeishuAtTarget, NotifyDeps } from "./notify"
import { validateNotifyChannel, sendCronNotification, normalizeAtList } from "./notify"
import { sessionPath, walkDir } from "./paths"

/** 定时任务调度器 tick 周期（DESIGN「常量参考」）。 */
export const CRON_TICK_INTERVAL_MS = 30_000
/** 脚本型定时任务单次执行超时缺省（可按任务 timeoutMs 覆盖）。 */
export const CRON_SCRIPT_TIMEOUT_MS = 5 * 60 * 1000
/** 提示词型定时任务单次执行超时缺省（到时取消会话任务）。 */
export const CRON_PROMPT_TIMEOUT_MS = 30 * 60 * 1000
/** 单任务执行超时上下限。 */
export const CRON_TIMEOUT_MIN_MS = 1_000
export const CRON_TIMEOUT_MAX_MS = 24 * 60 * 60 * 1000
/** 脚本输出在任务记录中的保留长度（消息中的完整输出另行截断）。 */
export const CRON_OUTPUT_MAX = 4000
/** 脚本结果写入会话消息的内容上限。 */
export const CRON_MESSAGE_MAX = 8000
/** 每任务保留的运行历史条数（环形截断）。 */
export const CRON_RUNS_HISTORY = 10
/** 任务名长度上限。 */
export const CRON_NAME_MAX = 100

export type CronTaskType = "script" | "prompt"
/** prompt 型执行目标：ephemeral=每次触发新建会话（缺省）；sticky=专用会话跨次复用；session=绑定既有会话。 */
export type CronTarget = "ephemeral" | "sticky" | "session"
/** 停机错过触发的补跑策略：skip=跳过从当前重算（缺省）；run=启动后立即补跑一次。 */
export type CronMisfire = "skip" | "run"

/** 单次运行历史记录。 */
export interface CronRunRecord {
  id: string
  /** 触发时间。 */
  at: number
  /** 结束时间。 */
  endedAt: number
  status: "success" | "error" | "skipped" | "timeout"
  durationMs: number
  output?: string
  error?: string
  /** prompt 型本次运行的会话（执行轨迹）。 */
  sessionId?: string
  /** 手动触发（cron_trigger / REST run）。 */
  manual?: boolean
}

/** 定时任务（用户级资源，持久化于 users/{user}/cron.json，与会话生命周期解耦）。 */
export interface CronTask {
  id: string
  user: string
  name?: string
  /** script=脚本运行；prompt=提示词运行 agent。 */
  type: CronTaskType
  /** 定时表达式：5 段 cron（分 时 日 月 周）或 @every 30m / @daily / @at <时间> 等。 */
  schedule: string
  /** type=script：shell 命令（在任务专属工作目录以用户环境执行）。 */
  script?: string
  /** type=prompt：触发 agent 运行的提示词。 */
  prompt?: string
  /** type=prompt 执行目标（缺省 ephemeral）。 */
  target?: CronTarget
  /** target=session 绑定的会话（缺省=创建来源 originSessionId）。 */
  sessionId?: string
  /** target=ephemeral/sticky 的预载子Agent 名单。 */
  agents?: string[]
  /** IANA 时区（如 Asia/Shanghai；缺省服务器本地时区）。 */
  timezone?: string
  /** 停机错过补跑策略（缺省 skip）。 */
  misfire?: CronMisfire
  /** 单次执行超时（缺省 script 5 分钟 / prompt 30 分钟）。 */
  timeoutMs?: number
  /** 通知通道（可配多条）。 */
  notify?: CronNotifyChannel[]
  /** 通知时机：always=每次（缺省）/ error=仅失败。 */
  notifyOn?: "always" | "error"
  /** 连续失败自动停用阈值（缺省 0=不停用）。 */
  maxConsecutiveErrors?: number
  /** 创建来源会话（仅记录：脚本结果消息写回 + 兼容旧版会话绑定任务）。 */
  originSessionId?: string
  /** target=sticky 的专用会话 id（跨次复用）。 */
  stickySessionId?: string
  enabled: boolean
  createdAt: number
  updatedAt: number
  lastRunAt?: number
  nextRunAt: number
  runCount: number
  lastStatus?: "success" | "error" | "skipped" | "timeout"
  lastOutput?: string
  lastError?: string
  /** 连续失败计数（成功清零，达阈值自动停用）。 */
  consecutiveErrors?: number
  /** 最近运行历史（新在前，环形保留 CRON_RUNS_HISTORY 条）。 */
  runs?: CronRunRecord[]
  /** 最近一次通知投递错误（通知失败不影响执行结果）。 */
  lastNotifyError?: string
}

/** 通知通道输入形态（at 允许字符串 open_id/"all" 或 {id,name}，创建/修改时归一为 {id,name?}）。 */
export type CronNotifyInput = Omit<CronNotifyChannel, "at"> & { at?: Array<string | FeishuAtTarget> }

export interface CronCreateInput {
  name?: string
  type: CronTaskType
  schedule: string
  script?: string
  prompt?: string
  target?: CronTarget
  sessionId?: string
  agents?: string[]
  timezone?: string
  misfire?: CronMisfire
  timeoutMs?: number
  notify?: CronNotifyInput[]
  notifyOn?: "always" | "error"
  maxConsecutiveErrors?: number
  enabled?: boolean
}

export interface CronUpdateInput {
  name?: string
  type?: CronTaskType
  schedule?: string
  script?: string
  prompt?: string
  target?: CronTarget
  sessionId?: string
  agents?: string[]
  timezone?: string
  misfire?: CronMisfire
  timeoutMs?: number
  notify?: CronNotifyInput[]
  notifyOn?: "always" | "error"
  maxConsecutiveErrors?: number
  enabled?: boolean
}

/** cron 字段解析结果。 */
interface CronField {
  values: Set<number>
  /** 是否 * 全匹配（占满合法区间）。 */
  all: boolean
}

/** 进程环境快照（剔除 undefined 值，收敛为 Record<string,string>）。 */
function processEnvSnapshot(): Record<string, string> {
  const out: Record<string, string> = {}
  for (const [k, v] of Object.entries(process.env)) if (v !== undefined) out[k] = v
  return out
}

function parseField(raw: string, min: number, max: number, label: string): CronField {
  const values = new Set<number>()
  const parts = raw.split(",")
  for (const part of parts) {
    const m = part.match(/^(\*|\d+)(?:-(\d+))?(?:\/(\d+))?$/)
    if (!m) throw new Error(`无效的 cron 表达式（${label} 段）: ${raw}`)
    const loRaw = m[1]
    const hiRaw = m[2]
    const stepRaw = m[3]
    const step = stepRaw ? Number(stepRaw) : 1
    if (step < 1) throw new Error(`无效的 cron 步长（${label} 段）: ${raw}`)
    if (loRaw === "*") {
      if (hiRaw) throw new Error(`无效的 cron 范围（${label} 段）: ${raw}`)
      for (let v = min; v <= max; v += step) values.add(v)
    } else {
      const lo = Number(loRaw)
      const hi = hiRaw ? Number(hiRaw) : lo
      if (lo < min || hi > max || lo > hi) throw new Error(`cron 字段越界（${label} 段）: ${raw}`)
      for (let v = lo; v <= hi; v += step) values.add(v)
    }
  }
  if (!values.size) throw new Error(`cron 字段无有效值（${label} 段）: ${raw}`)
  return { values, all: values.size === max - min + 1 }
}

export interface CronSchedule {
  /** 从 fromMs 之后（严格大于）的下一次执行时间。 */
  next(fromMs: number): number
}

const CRON_ALIASES: Record<string, string> = {
  "@hourly": "0 * * * *",
  "@daily": "0 0 * * *",
  "@midnight": "0 0 * * *",
  "@weekly": "0 0 * * 0",
  "@monthly": "0 0 1 * *",
  "@yearly": "0 0 1 1 *",
  "@annually": "0 0 1 1 *",
}

/** 是否一次性任务表达式（@at）。 */
export function isOneShotSchedule(raw: string): boolean {
  return /^@at\s/i.test(String(raw).trim())
}

/** 校验 IANA 时区名（非法抛友好错误）。 */
function validateTimezone(tz: string): void {
  try {
    new Intl.DateTimeFormat("en-US", { timeZone: tz })
  } catch {
    throw new Error(`无效的时区: ${tz}（须为 IANA 名称，如 Asia/Shanghai）`)
  }
}

/** 时区墙上时钟分量。 */
interface TzWall {
  y: number
  mo: number
  d: number
  h: number
  mi: number
  dow: number
}

const DOW_NAMES: Record<string, number> = { Sun: 0, Mon: 1, Tue: 2, Wed: 3, Thu: 4, Fri: 5, Sat: 6 }

function tzWall(tz: string, epochMs: number): TzWall {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: tz,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    weekday: "short",
    hour12: false,
  }).formatToParts(new Date(epochMs))
  const get = (t: string) => parts.find((p) => p.type === t)?.value ?? ""
  const h = Number(get("hour"))
  return {
    y: Number(get("year")),
    mo: Number(get("month")),
    d: Number(get("day")),
    h: h === 24 ? 0 : h,
    mi: Number(get("minute")),
    dow: DOW_NAMES[get("weekday")] ?? 0,
  }
}

/** 时区在某时刻的 UTC 偏移（毫秒）。 */
function tzOffsetMs(tz: string, epochMs: number): number {
  const w = tzWall(tz, epochMs)
  return Date.UTC(w.y, w.mo - 1, w.d, w.h, w.mi) - epochMs
}

/** 时区墙上时间 → epoch（两次偏移校正，兼容 DST 切换）。 */
function wallToEpochMs(w: { y: number; mo: number; d: number; h: number; mi: number }, tz: string): number {
  const guess = Date.UTC(w.y, w.mo - 1, w.d, w.h, w.mi)
  let t = guess - tzOffsetMs(tz, guess)
  const off2 = tzOffsetMs(tz, t)
  if (off2 !== guess - t) t = guess - off2
  return t
}

/** 解析定时表达式：5 段 cron（分 时 日 月 周，按 timezone 指定时区、缺省本地）或
 *  @daily/@hourly/@weekly/@monthly/@every <n>s|m|h|d/@at <时间>（一次性，触发后自动停用）。 */
export function parseCronSchedule(raw: string, timezone?: string): CronSchedule {
  const s = String(raw).trim()
  if (!s) throw new Error("定时表达式不能为空")
  if (timezone) validateTimezone(timezone)
  if (s.startsWith("@")) {
    const alias = CRON_ALIASES[s.toLowerCase()]
    if (alias) return parseCronSchedule(alias, timezone)
    const every = s.match(/^@every\s+(\d+)\s*(s|m|h|d)$/i)
    if (every) {
      const n = Number(every[1])
      if (n < 1) throw new Error(`无效的定时间隔: ${raw}`)
      const unitMs: Record<string, number> = { s: 1000, m: 60_000, h: 3_600_000, d: 86_400_000 }
      const interval = n * unitMs[every[2].toLowerCase()]
      return { next: (fromMs) => fromMs - (fromMs % interval) + interval }
    }
    const at = s.match(/^@at\s+(.+)$/i)
    if (at) {
      const normalized = at[1].trim().replace(" ", "T")
      const t = Date.parse(normalized)
      if (!Number.isFinite(t)) throw new Error(`无效的 @at 时间: ${raw}（示例 @at 2026-09-01T09:00）`)
      return { next: () => t }
    }
    throw new Error(`无效的定时表达式: ${raw}（支持 5 段 cron 如 "0 9 * * *"、@daily/@hourly/@weekly/@monthly、@every 30m、@at 2026-09-01T09:00）`)
  }
  const parts = s.split(/\s+/)
  if (parts.length !== 5) throw new Error(`无效的 cron 表达式: ${raw}（需要 5 段: 分 时 日 月 周）`)
  const fields = {
    minute: parseField(parts[0], 0, 59, "分"),
    hour: parseField(parts[1], 0, 23, "时"),
    dom: parseField(parts[2], 1, 31, "日"),
    month: parseField(parts[3], 1, 12, "月"),
    dow: parseField(parts[4], 0, 7, "周"),
  }
  if (timezone) return { next: (fromMs) => nextCronTimeTz(fields, fromMs, raw, timezone) }
  return { next: (fromMs) => nextCronTime(fields, fromMs, raw) }
}

/** 5 段 cron 的下次执行时间：按天扫描（本地时区，跨 DST 由 Date 构造器处理），最多扫 5 年。 */
function nextCronTime(f: CronFieldRecord, fromMs: number, raw: string): number {
  const start = new Date(fromMs)
  const domRestricted = !f.dom.all
  const dowRestricted = !f.dow.all
  const hours = [...f.hour.values].sort((a, b) => a - b)
  const minutes = [...f.minute.values].sort((a, b) => a - b)
  const MAX_DAYS = 366 * 5
  for (let i = 0; i < MAX_DAYS; i++) {
    const d0 = new Date(start.getFullYear(), start.getMonth(), start.getDate() + i)
    if (!dayMatches(f, d0.getMonth() + 1, d0.getDate(), d0.getDay(), domRestricted, dowRestricted)) continue
    for (const hour of hours) {
      for (const minute of minutes) {
        const ts = new Date(d0.getFullYear(), d0.getMonth(), d0.getDate(), hour, minute).getTime()
        if (ts > fromMs) return ts
      }
    }
  }
  throw new Error(`无法计算下次执行时间（cron 表达式可能永不触发）: ${raw}`)
}

/** 指定时区的下次执行时间：墙上时钟按天扫描（UTC 域日历推进 + 双次偏移换算，跨 DST 正确）。 */
function nextCronTimeTz(f: CronFieldRecord, fromMs: number, raw: string, tz: string): number {
  const start = tzWall(tz, fromMs)
  const domRestricted = !f.dom.all
  const dowRestricted = !f.dow.all
  const hours = [...f.hour.values].sort((a, b) => a - b)
  const minutes = [...f.minute.values].sort((a, b) => a - b)
  const MAX_DAYS = 366 * 5
  for (let i = 0; i < MAX_DAYS; i++) {
    // 纯日历推进（UTC 域构造，仅取年月日星期分量——不涉及时区换算）
    const cur = new Date(Date.UTC(start.y, start.mo - 1, start.d + i))
    if (!dayMatches(f, cur.getUTCMonth() + 1, cur.getUTCDate(), cur.getUTCDay(), domRestricted, dowRestricted)) continue
    for (const hour of hours) {
      for (const minute of minutes) {
        const ts = wallToEpochMs({ y: cur.getUTCFullYear(), mo: cur.getUTCMonth() + 1, d: cur.getUTCDate(), h: hour, mi: minute }, tz)
        if (ts > fromMs) return ts
      }
    }
  }
  throw new Error(`无法计算下次执行时间（cron 表达式可能永不触发）: ${raw}`)
}

/** 经典 cron 语义：日与周均受限时任一命中即可（OR），否则须同时命中（AND）。 */
function dayMatches(f: CronFieldRecord, month: number, dom: number, dow: number, domRestricted: boolean, dowRestricted: boolean): boolean {
  if (!f.month.values.has(month)) return false
  const domMatch = f.dom.values.has(dom)
  const dowMatch = f.dow.values.has(dow) || f.dow.values.has(dow + 7)
  return domRestricted && dowRestricted ? domMatch || dowMatch : domMatch && dowMatch
}

interface CronFieldRecord {
  minute: CronField
  hour: CronField
  dom: CronField
  month: CronField
  dow: CronField
}

/** 调度器依赖：结构接口（与 AgentEngineOptions 同风格），便于测试替身注入。 */
export interface CronManagerDeps {
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
}

export class CronManager {
  private entries = new Map<string, CronTask>()
  private timer: ReturnType<typeof setInterval> | null = null
  /** 执行中的任务 id（重入防护）：单次执行可远超 tick 间隔，nextRunAt 完成后才推进。 */
  private firing = new Set<string>()
  private writes = new Map<string, Promise<void>>()
  private engine: AgentEngine | undefined
  private now: () => number

  constructor(private deps: CronManagerDeps) {
    this.engine = deps.engine
    this.now = deps.now ?? (() => Date.now())
  }

  /** 注入 AgentEngine（prompt 型任务执行器；构造期缺省时调用）。双向绑定：同时回填引擎侧
   *  opts.cron（cron_* 工具的 ToolContext 绑定源），单向注入会使工具恒报「能力未启用」。 */
  attach(engine: AgentEngine): void {
    this.engine = engine
    engine.setCron(this)
  }

  /** 用户级任务存储文件（users/{user}/cron.json，随用户目录生命周期，与会话删除/过期解耦）。 */
  private userCronFile(user: string): string {
    return join(this.deps.home, "users", user, "cron.json")
  }

  /** 任务专属工作目录（脚本 cwd，跨次运行保留产物）。 */
  workspaceOf(entry: Pick<CronTask, "id" | "user">): string {
    return join(this.deps.home, "users", entry.user, "cron-workspace", entry.id)
  }

  /** 扫描加载用户级任务 + 迁移旧版会话级 cron.json，并启动 tick 循环。 */
  async start(): Promise<void> {
    const now = this.now()
    const base = join(this.deps.home, "users")
    const touchedUsers = new Set<string>()
    await walkDir(base, 5, async (p) => {
      if (!p.endsWith("cron.json")) return
      const rel = relative(base, p).split(sep)
      try {
        if (rel.length === 2 && rel[1] === "cron.json") {
          // 用户级存储：users/{user}/cron.json（条目自身携带 user 归属）
          const tasks = JSON.parse(await readFile(p, "utf8"))
          if (!Array.isArray(tasks)) return
          for (const t of tasks) {
            const entry = this.normalizeLoaded(t, now)
            if (entry) this.entries.set(entry.id, entry)
          }
        } else if (rel.length === 6 && rel[1] === "sessions" && rel[5] === "cron.json") {
          // 旧版会话级存储：users/{user}/sessions/{s0}/{s1}/{sessionId}/cron.json —— 迁移进用户级后改名
          const user = rel[0]
          const sessionId = rel[4]
          const tasks = JSON.parse(await readFile(p, "utf8"))
          if (!Array.isArray(tasks)) return
          let migrated = 0
          for (const t of tasks) {
            const entry = this.normalizeLoaded(t, now)
            if (!entry || this.entries.has(entry.id)) continue
            // 旧版任务与会话强绑定：prompt 保持原会话执行（target=session），脚本改用户级工作目录
            entry.originSessionId = entry.sessionId ?? sessionId
            entry.sessionId = undefined
            entry.target = "session"
            this.entries.set(entry.id, entry)
            migrated++
          }
          if (migrated > 0) touchedUsers.add(user)
          await rename(p, `${p}.migrated`).catch(() => {})
        }
      } catch {
        /* 跳过损坏文件 */
      }
    })
    for (const user of touchedUsers) await this.saveUserEntries(user)
    if (this.timer) return
    this.timer = setInterval(() => void this.tick(), this.deps.tickIntervalMs ?? CRON_TICK_INTERVAL_MS)
    this.timer.unref?.()
  }

  /** 加载条目归一化与防护（非法表达式禁用 / 过期触发点按补跑策略处置）。 */
  private normalizeLoaded(t: unknown, now: number): CronTask | null {
    if (!t || typeof t !== "object") return null
    const entry = t as CronTask
    if (typeof entry.id !== "string" || !entry.user || typeof entry.schedule !== "string") return null
    if (this.entries.has(entry.id)) return null
    // schedule/时区合法性校验（add/update 时已拒，此处防外部编辑损坏）：非法直接禁用，
    // 否则 nextTime 解析失败回退 +30s 会形成每 30s 触发一次的热循环
    if (entry.enabled) {
      try {
        parseCronSchedule(entry.schedule, entry.timezone)
      } catch {
        entry.enabled = false
        entry.lastError = (entry.lastError ? `${entry.lastError}；` : "") + "schedule/timezone 非法：启动加载时已禁用（请修正后重新启用）"
      }
    }
    if (entry.enabled && (typeof entry.nextRunAt !== "number" || entry.nextRunAt <= now)) {
      if (entry.misfire === "run" && typeof entry.nextRunAt === "number" && entry.nextRunAt > 0) {
        // 补跑策略：保留过期触发点，首个 tick 立即执行一次（fire 后按当前时间重算，至多一次）
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

  async add(user: string, input: CronCreateInput, originSessionId?: string): Promise<CronTask> {
    if (input.type !== "script" && input.type !== "prompt") throw new Error(`无效的定时任务类型: ${String(input.type)}`)
    const schedule = String(input.schedule ?? "").trim()
    if (!schedule) throw new Error("缺少定时表达式（schedule）")
    const script = input.script != null ? String(input.script).trim() : ""
    const prompt = input.prompt != null ? String(input.prompt).trim() : ""
    if (input.type === "script" && !script) throw new Error("脚本型定时任务需要 script 参数（shell 命令）")
    if (input.type === "prompt" && !prompt) throw new Error("提示词型定时任务需要 prompt 参数")
    const now = this.now()
    const timezone = input.timezone ? String(input.timezone).trim() : undefined
    const target = this.validateTarget(input.type, input.target)
    const sessionId = target === "session" && input.sessionId ? String(input.sessionId).trim() : undefined
    const agents = this.validateAgents(input.agents)
    const notify = this.validateNotify(input.notify)
    const timeoutMs = this.validateTimeout(input.timeoutMs)
    const misfire = input.misfire === "run" ? "run" : input.misfire === "skip" ? "skip" : undefined
    const entry: CronTask = {
      id: randomUUID().replace(/-/g, ""),
      user,
      name: input.name ? String(input.name).trim().slice(0, CRON_NAME_MAX) || undefined : undefined,
      type: input.type,
      schedule,
      script: input.type === "script" ? script : undefined,
      prompt: input.type === "prompt" ? prompt : undefined,
      target: input.type === "prompt" ? target : undefined,
      sessionId,
      agents: input.type === "prompt" ? agents : undefined,
      timezone,
      misfire,
      timeoutMs,
      notify,
      notifyOn: input.notifyOn === "error" ? "error" : notify?.length ? "always" : undefined,
      maxConsecutiveErrors: this.validateMaxConsecutiveErrors(input.maxConsecutiveErrors),
      originSessionId: originSessionId || undefined,
      enabled: input.enabled !== false,
      createdAt: now,
      updatedAt: now,
      nextRunAt: 0,
      runCount: 0,
    }
    // 创建时严格校验表达式与时区（非法即拒绝），并计算下次执行时间
    const parsed = parseCronSchedule(schedule, timezone)
    if (isOneShotSchedule(schedule) && parsed.next(now) <= now) throw new Error("@at 时间已过去（一次性任务请指定未来时间）")
    entry.nextRunAt = parsed.next(now)
    this.entries.set(entry.id, entry)
    await this.saveUserEntries(user)
    return this.publicView(entry)
  }

  async update(user: string, id: string, patch: CronUpdateInput): Promise<CronTask | null> {
    const entry = this.entryOf(user, id)
    if (!entry) return null
    if (patch.name !== undefined) entry.name = String(patch.name).trim().slice(0, CRON_NAME_MAX) || undefined
    if (patch.schedule !== undefined) {
      const s = String(patch.schedule).trim()
      if (!s) throw new Error("定时表达式不能为空")
      entry.schedule = s
    }
    if (patch.timezone !== undefined) entry.timezone = patch.timezone ? String(patch.timezone).trim() : undefined
    if (patch.type !== undefined) {
      if (patch.type !== "script" && patch.type !== "prompt") throw new Error(`无效的定时任务类型: ${String(patch.type)}`)
      entry.type = patch.type
    }
    if (patch.script !== undefined) entry.script = String(patch.script).trim() || undefined
    if (patch.prompt !== undefined) entry.prompt = String(patch.prompt).trim() || undefined
    if (entry.type === "script" && !entry.script) throw new Error("脚本型定时任务需要 script 参数（shell 命令）")
    if (entry.type === "prompt" && !entry.prompt) throw new Error("提示词型定时任务需要 prompt 参数")
    if (patch.target !== undefined || patch.sessionId !== undefined || patch.type !== undefined) {
      entry.target = this.validateTarget(entry.type, patch.target !== undefined ? patch.target : entry.target)
      if (entry.target === "session") {
        if (patch.sessionId !== undefined) entry.sessionId = String(patch.sessionId).trim() || undefined
      } else {
        entry.sessionId = undefined
      }
    }
    if (patch.agents !== undefined) entry.agents = entry.type === "prompt" ? this.validateAgents(patch.agents) : undefined
    if (patch.misfire !== undefined) entry.misfire = patch.misfire === "run" ? "run" : "skip"
    if (patch.timeoutMs !== undefined) entry.timeoutMs = this.validateTimeout(patch.timeoutMs)
    if (patch.notify !== undefined) entry.notify = this.validateNotify(patch.notify, entry.notify)
    if (patch.notifyOn !== undefined) entry.notifyOn = patch.notifyOn === "error" ? "error" : "always"
    if (patch.maxConsecutiveErrors !== undefined) entry.maxConsecutiveErrors = this.validateMaxConsecutiveErrors(patch.maxConsecutiveErrors)
    if (patch.enabled !== undefined) {
      entry.enabled = patch.enabled
      // 重新启用视为重置失败计数（用户明确干预，连续失败停用语义不应延续）
      if (entry.enabled) entry.consecutiveErrors = 0
    }
    entry.updatedAt = this.now()
    if (entry.enabled) {
      // 修改周期/类型/内容后严格校验表达式并重算下次执行时间
      const parsed = parseCronSchedule(entry.schedule, entry.timezone)
      if (isOneShotSchedule(entry.schedule) && parsed.next(this.now()) <= this.now()) throw new Error("@at 时间已过去（一次性任务请指定未来时间）")
      entry.nextRunAt = parsed.next(this.now())
    }
    await this.saveUserEntries(user)
    return this.publicView(entry)
  }

  async remove(user: string, id: string): Promise<boolean> {
    const entry = this.entryOf(user, id)
    if (!entry) return false
    this.entries.delete(id)
    await this.saveUserEntries(user)
    return true
  }

  async list(user: string): Promise<CronTask[]> {
    return [...this.entries.values()]
      .filter((e) => e.user === user)
      .sort((a, b) => a.createdAt - b.createdAt)
      .map((t) => this.publicView(t))
  }

  /** 手动立即触发一次（cron_trigger / REST run）：立即执行，不推进 nextRunAt（一次性任务仍自动停用）。 */
  async trigger(user: string, id: string): Promise<CronTask | null> {
    const entry = this.entryOf(user, id)
    if (!entry) return null
    await this.fire(entry, this.now(), true)
    return this.publicView(entry)
  }

  /** 定时触发检查（tick 循环与测试共用的核心入口）。 */
  async tick(): Promise<void> {
    const now = this.now()
    for (const entry of [...this.entries.values()]) {
      if (!entry.enabled || entry.nextRunAt > now) continue
      try {
        await this.fire(entry, now)
      } catch (err) {
        // fire 抛错（引擎不可用/存储失败等）：必须重算下次执行时间，
        // 否则 nextRunAt 停留在过去 → 每个 tick（30 秒）都会重试失败任务，形成无限重试热循环；
        // 一次性任务（@at）无法重算未来时间——直接停用防热循环
        entry.lastError = String((err as Error).message || err).slice(0, 500)
        entry.lastStatus = "error"
        entry.updatedAt = now
        if (isOneShotSchedule(entry.schedule)) entry.enabled = false
        else entry.nextRunAt = this.nextTime(entry, now)
        this.publish(entry, "event.cron.result", {
          id: entry.id,
          type: entry.type,
          name: entry.name ?? "",
          ok: false,
          status: "error",
          error: entry.lastError,
        })
        await this.saveUserEntries(entry.user)
      }
    }
  }

  /** 校验并归一化 prompt 型执行目标参数。 */
  private validateTarget(type: CronTaskType, target: unknown): CronTarget | undefined {
    if (type !== "prompt") return undefined
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

  private validateNotify(notify: unknown, prev?: CronNotifyChannel[]): CronNotifyChannel[] | undefined {
    if (notify === undefined || notify === null) return undefined
    if (!Array.isArray(notify)) throw new Error("notify 须为通知通道数组")
    if (!notify.length) return undefined
    const out: CronNotifyChannel[] = []
    for (const raw of notify) {
      if (!raw || typeof raw !== "object") throw new Error("通知通道须为 {type,target,secret} 对象")
      const ch = raw as CronNotifyChannel
      const entry: CronNotifyChannel = {
        type: ch.type,
        target: String(ch.target ?? "").trim(),
        secret: ch.secret && ch.secret !== "***" ? String(ch.secret) : undefined,
        // at 名单归一（字符串 id 或 {id,name} → {id,name?}，非法 id 拒绝）
        at: normalizeAtList((ch as { at?: unknown }).at),
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
    if (!Number.isFinite(n) || n < CRON_TIMEOUT_MIN_MS || n > CRON_TIMEOUT_MAX_MS) {
      throw new Error(`无效的执行超时 timeoutMs（${CRON_TIMEOUT_MIN_MS}~${CRON_TIMEOUT_MAX_MS} 毫秒）`)
    }
    return Math.round(n)
  }

  private validateMaxConsecutiveErrors(v: unknown): number | undefined {
    if (v === undefined || v === null) return undefined
    const n = Number(v)
    if (!Number.isInteger(n) || n < 0 || n > 1000) throw new Error("maxConsecutiveErrors 须为 0~1000 的整数（0=不停用）")
    return n
  }

  /** 输出视图（通知密钥脱敏——REST/工具回显不泄露 secret）。 */
  private publicView(entry: CronTask): CronTask {
    const copy = { ...entry }
    if (copy.notify?.length) copy.notify = copy.notify.map((ch) => ({ ...ch, secret: ch.secret ? "***" : undefined }))
    return copy
  }

  private async fire(entry: CronTask, now: number, manual = false): Promise<void> {
    // 重入防护：单次执行可长达任务超时（远超 30s tick 间隔），而 nextRunAt 要等执行结束才推进——
    // 无防护时每 tick 都会再次 fire 同一任务，长脚本被并发叠加执行（副作用重复）。执行中跳过本轮触发。
    if (this.firing.has(entry.id)) return
    this.firing.add(entry.id)
    try {
      await this.fireInner(entry, now, manual)
    } finally {
      this.firing.delete(entry.id)
    }
  }

  private async fireInner(entry: CronTask, now: number, manual: boolean): Promise<void> {
    const { user } = entry
    this.publish(entry, "event.cron.run", { id: entry.id, type: entry.type, name: entry.name ?? "", schedule: entry.schedule, manual })
    const startedAt = this.now()
    let status: CronRunRecord["status"]
    let output = ""
    let error: string | undefined
    let runSessionId: string | undefined
    let ran = false
    if (entry.type === "script") {
      // 安全模式：script 型任务直接执行 shell（不经工具拦截），已建任务同样阻止——跳过并落盘提示；
      // 不提前 return：走统一收尾（推进 nextRunAt 防高频任务每 tick 重复刷提示，publish event.cron.result）
      if (this.deps.safeMode) {
        await this.appendOriginMessage(entry, `[定时任务${entry.name ? `「${entry.name}」` : ""}已跳过] 安全模式：脚本执行已限制（安全模式下仅允许只读操作）。`, now)
        status = "skipped"
        output = "安全模式：脚本执行已限制"
        error = "safe-mode"
        ran = true
      } else {
        // 工作目录保证存在（任务专属，跨次运行保留产物）
        const cwd = this.workspaceOf(entry)
        await mkdir(cwd, { recursive: true }).catch(() => {})
        // 会话环境尽力解析（sticky/来源会话已删除则退回进程环境）：定时任务不依赖会话存活
        const envRef = entry.stickySessionId ?? entry.originSessionId
        let env: Record<string, string>
        try {
          env = envRef ? await this.deps.env.resolve(envRef, user) : processEnvSnapshot()
        } catch {
          env = processEnvSnapshot()
        }
        const { stdout, stderr, code } = await this.deps.sandbox.exec(entry.script ?? "", {
          cwd,
          env,
          timeoutMs: entry.timeoutMs ?? CRON_SCRIPT_TIMEOUT_MS,
          user,
        })
        const out = code === 0 ? stdout : `${stdout}${stdout && stderr ? "\n" : ""}${stderr}\n[exit ${code}]`
        const ok = code === 0
        await this.appendOriginMessage(entry, `[定时任务${entry.name ? `「${entry.name}」` : ""}执行结果（${ok ? "成功" : "失败"}）]\n${out}`.slice(0, CRON_MESSAGE_MAX), now)
        status = ok ? "success" : "error"
        output = out.slice(0, CRON_OUTPUT_MAX)
        error = ok ? undefined : `exit ${code}`
        ran = true
      }
    } else {
      if (!this.engine) throw new Error("定时任务引擎未就绪")
      // 执行目标解耦（DESIGN「定时任务」）：ephemeral 每次新建会话 / sticky 专用会话复用 / session 绑定会话
      const target = entry.target ?? "ephemeral"
      let sid = await this.resolvePromptSession(entry, target)
      if (target === "session" && !sid) {
        // 绑定会话已删除：自愈降级为 ephemeral（任务保留，一次性记因）
        entry.target = "ephemeral"
        entry.sessionId = undefined
        error = `绑定会话已删除，本次起改为独立新会话执行`
        sid = await this.resolvePromptSession(entry, "ephemeral")
      }
      if (!sid) throw new Error("无法获得执行会话")
      runSessionId = sid
      // 会话正有任务运行时跳过本轮（避免并发写会话）
      if (this.engine.isRunning(sid)) {
        status = "skipped"
        output = "会话正在运行其他任务，跳过本次触发"
      } else {
        const promptText = `[定时任务${entry.name ? `「${entry.name}」` : ""}触发]\n${entry.prompt ?? ""}`
        const timeoutMs = entry.timeoutMs ?? CRON_PROMPT_TIMEOUT_MS
        let timedOut = false
        // 注意不可 unref：await 挂起的 Promise 不保活事件循环，unref 定时器在「仅剩本定时器」场景
        // （测试/空闲进程）永不触发；finally 必 clear，无泄漏
        const timer = setTimeout(() => {
          timedOut = true
          this.engine!.cancel(sid!)
        }, timeoutMs)
        let runError: string | undefined
        try {
          await this.engine.run(sid, user, promptText)
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
        output = (await this.lastAssistantText(sid, user)) ?? (status === "success" ? "已触发，执行过程与结果见会话消息" : "")
        ran = true
      }
    }
    const endedAt = this.now()
    const ok = status === "success"
    let disabled = false
    // 状态记录（含 skipped：跳过原因对 cron_list / 通知可见；计数与历史仅在实际运行时更新）
    entry.lastStatus = status
    entry.lastOutput = output.slice(0, CRON_OUTPUT_MAX) || undefined
    entry.lastError = error
    if (ran) {
      entry.lastRunAt = now
      entry.runCount += 1
      // 连续失败计数与自动停用（成功清零；skipped 不计）
      if (status === "error" || status === "timeout") {
        entry.consecutiveErrors = (entry.consecutiveErrors ?? 0) + 1
        const max = entry.maxConsecutiveErrors ?? 0
        if (max > 0 && entry.consecutiveErrors >= max) {
          entry.enabled = false
          disabled = true
          entry.lastError = `${error ?? "连续失败"}；连续失败 ${entry.consecutiveErrors} 次，已自动停用（修正后可重新启用）`
        }
      } else if (ok) {
        entry.consecutiveErrors = 0
      }
      // 一次性任务（@at）触发即完成：自动停用
      if (isOneShotSchedule(entry.schedule)) {
        entry.enabled = false
        disabled = true
      }
      // 运行历史（新在前，环形截断）
      const rec: CronRunRecord = {
        id: randomUUID(),
        at: now,
        endedAt,
        status,
        durationMs: endedAt - startedAt,
        output: output.slice(0, CRON_OUTPUT_MAX) || undefined,
        error,
        sessionId: runSessionId,
        manual: manual || undefined,
      }
      entry.runs = [rec, ...(entry.runs ?? [])].slice(0, CRON_RUNS_HISTORY)
    }
    // 手动触发不推进 nextRunAt（不影响既定节奏；一次性任务已停用无须推进）
    if (!manual) entry.nextRunAt = this.nextTime(entry, endedAt)
    entry.updatedAt = endedAt
    await this.saveUserEntries(user)
    this.publish(entry, "event.cron.result", {
      id: entry.id,
      type: entry.type,
      name: entry.name ?? "",
      ok,
      status,
      output: entry.lastOutput,
      error: entry.lastError,
      sessionId: runSessionId,
      manual: manual || undefined,
      disabled: disabled || undefined,
    })
    await this.dispatchNotify(entry, {
      event: "cron.result",
      task: { id: entry.id, name: entry.name ?? "", type: entry.type, schedule: entry.schedule, user },
      ok,
      status,
      at: now,
      durationMs: endedAt - startedAt,
      output: entry.lastOutput,
      error: entry.lastError,
      sessionId: runSessionId,
      disabled: disabled || undefined,
      manual: manual || undefined,
    })
  }

  /** 解析 prompt 型执行会话：ephemeral 每次新建（可选预载子Agent）；sticky 专用会话惰性创建并复用；session 用绑定会话。 */
  private async resolvePromptSession(entry: CronTask, target: CronTarget): Promise<string | undefined> {
    if (target === "session") {
      const sid = entry.sessionId ?? entry.originSessionId
      if (!sid) return undefined
      return existsSync(join(sessionPath(this.deps.home, entry.user, sid), "chat.json")) ? sid : undefined
    }
    if (target === "sticky" && entry.stickySessionId && existsSync(join(sessionPath(this.deps.home, entry.user, entry.stickySessionId), "chat.json"))) {
      return entry.stickySessionId
    }
    const session = await this.deps.store.createSession(entry.user, `定时任务${entry.name ? `「${entry.name}」` : `(${entry.id.slice(0, 8)})`}`)
    // 预载子Agent：写入会话装载名单，engine.run 的装载保障按此注册工具与提示词
    if (entry.agents?.length) {
      session.loadedSubAgents = [...entry.agents]
      await this.deps.store.save(session)
    }
    if (target === "sticky") entry.stickySessionId = session.id
    return session.id
  }

  /** prompt 运行结果摘要：执行会话最后一条 assistant 消息。 */
  private async lastAssistantText(sessionId: string, user: string): Promise<string | undefined> {
    try {
      const session = await this.deps.store.load(sessionId, user)
      const msg = session ? [...session.messages].reverse().find((m) => m.role === "assistant" && typeof m.content === "string") : undefined
      return msg?.content ? msg.content.slice(0, CRON_OUTPUT_MAX) : undefined
    } catch {
      return undefined
    }
  }

  /** 结果消息写回来源会话（会话仍存在时；脚本型历史可见、模型可感知，会话删除则静默跳过）。 */
  private async appendOriginMessage(entry: CronTask, content: string, now: number): Promise<void> {
    const sid = entry.originSessionId
    if (!sid) return
    if (!existsSync(join(sessionPath(this.deps.home, entry.user, sid), "chat.json"))) return
    await this.deps.store.appendMessage(
      sid,
      {
        id: randomUUID(),
        role: "assistant",
        content,
        createdAt: now,
      },
      entry.user,
    )
  }

  /** 通知投递（尽力而为：按 notifyOn 过滤；失败记 lastNotifyError，不影响执行结果与调度）。 */
  private async dispatchNotify(entry: CronTask, n: CronResultNotification): Promise<void> {
    if (!entry.notify?.length) return
    const ok = n.ok
    if (entry.notifyOn === "error" && ok && !n.disabled) {
      entry.lastNotifyError = undefined
      return
    }
    if (this.deps.safeMode) {
      entry.lastNotifyError = "安全模式：通知投递已限制"
      return
    }
    const errors: string[] = []
    for (const ch of entry.notify) {
      try {
        await sendCronNotification(ch, n, this.deps.notify)
      } catch (err) {
        errors.push(`${ch.type}: ${String((err as Error).message || err).slice(0, 200)}`)
      }
    }
    entry.lastNotifyError = errors.length ? errors.join("；").slice(0, 500) : undefined
  }

  private nextTime(entry: CronTask, fromMs: number): number {
    // 解析即校验：非法表达式在 add/update 时即拒绝，此处解析失败按永不触发处理（保留旧值）
    try {
      return parseCronSchedule(entry.schedule, entry.timezone).next(fromMs)
    } catch {
      return fromMs + CRON_TICK_INTERVAL_MS
    }
  }

  private entryOf(user: string, id: string): CronTask | undefined {
    const entry = this.entries.get(id)
    return entry && entry.user === user ? entry : undefined
  }

  private saveUserEntries(user: string): Promise<void> {
    const tasks = [...this.entries.values()].filter((e) => e.user === user)
    const prev = this.writes.get(user) ?? Promise.resolve()
    const next = prev
      .then(async () => {
        const file = this.userCronFile(user)
        await mkdir(dirname(file), { recursive: true })
        await writeFile(file, JSON.stringify(tasks, null, 2))
      })
      .catch(() => {})
    this.writes.set(user, next)
    return next
  }

  private publish(entry: CronTask, type: string, payload: Record<string, unknown>): void {
    // 事件按任务绑定会话路由（来源会话已删除时广播到全局 sessionId 占位）
    const sessionId = entry.originSessionId ?? entry.stickySessionId ?? "cron"
    this.deps.events.publish({ type, sessionId, payload, timestamp: this.now() } as AgentEvent)
  }
}
