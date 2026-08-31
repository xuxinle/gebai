import { randomUUID } from "node:crypto"
import { existsSync } from "node:fs"
import { join, dirname } from "node:path"
import { mkdir, readFile, writeFile } from "node:fs/promises"
import type { AgentEvent } from "@gebai/sdk"
import type { AgentEngine } from "./engine"
import type { SessionStore } from "./store"
import type { EnvManager } from "./env"
import type { Sandbox } from "./sandbox"
import type { EventBus } from "./event-bus"
import { sessionPath, walkDir } from "./paths"

/** 定时任务调度器 tick 周期（DESIGN「常量参考」）。 */
export const CRON_TICK_INTERVAL_MS = 30_000
/** 脚本型定时任务单次执行超时（与 sh/py 工具同级）。 */
export const CRON_SCRIPT_TIMEOUT_MS = 5 * 60 * 1000
/** 脚本输出在任务记录中的保留长度（消息中的完整输出另行截断）。 */
export const CRON_OUTPUT_MAX = 4000
/** 脚本结果写入会话消息的内容上限。 */
export const CRON_MESSAGE_MAX = 8000

export type CronTaskType = "script" | "prompt"

/** 定时任务（持久化于会话目录 cron.json，随会话分片与清理）。 */
export interface CronTask {
  id: string
  sessionId: string
  user: string
  name?: string
  /** script=脚本运行；prompt=提示词运行 agent。 */
  type: CronTaskType
  /** 定时表达式：5 段 cron（分 时 日 月 周）或 @every 30m / @daily 等。 */
  schedule: string
  /** type=script：shell 命令（在会话 tmp/ 以会话环境执行）。 */
  script?: string
  /** type=prompt：触发 agent 运行的提示词。 */
  prompt?: string
  enabled: boolean
  createdAt: number
  updatedAt: number
  lastRunAt?: number
  nextRunAt: number
  runCount: number
  lastStatus?: "success" | "error" | "skipped"
  lastOutput?: string
  lastError?: string
}

export interface CronCreateInput {
  name?: string
  type: CronTaskType
  schedule: string
  script?: string
  prompt?: string
  enabled?: boolean
}

export interface CronUpdateInput {
  name?: string
  type?: CronTaskType
  schedule?: string
  script?: string
  prompt?: string
  enabled?: boolean
}

/** cron 字段解析结果。 */
interface CronField {
  values: Set<number>
  /** 是否 * 全匹配（占满合法区间）。 */
  all: boolean
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

/** 解析定时表达式：5 段 cron（分 时 日 月 周，本地时区）或 @daily/@hourly/@weekly/@monthly/@every <n>s|m|h|d。 */
export function parseCronSchedule(raw: string): CronSchedule {
  const s = String(raw).trim()
  if (!s) throw new Error("定时表达式不能为空")
  if (s.startsWith("@")) {
    const alias = CRON_ALIASES[s.toLowerCase()]
    if (alias) return parseCronSchedule(alias)
    const m = s.match(/^@every\s+(\d+)\s*(s|m|h|d)$/i)
    if (!m) {
      throw new Error(`无效的定时表达式: ${raw}（支持 5 段 cron 如 "0 9 * * *"，或 @daily/@hourly/@weekly/@monthly、@every 30m）`)
    }
    const n = Number(m[1])
    if (n < 1) throw new Error(`无效的定时间隔: ${raw}`)
    const unitMs: Record<string, number> = { s: 1000, m: 60_000, h: 3_600_000, d: 86_400_000 }
    const interval = n * unitMs[m[2].toLowerCase()]
    return { next: (fromMs) => fromMs - (fromMs % interval) + interval }
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
    const month = d0.getMonth() + 1
    if (!f.month.values.has(month)) continue
    // 经典 cron 语义：日与周均受限时任一命中即可（OR），否则须同时命中（AND）
    const domMatch = f.dom.values.has(d0.getDate())
    const dowMatch = f.dow.values.has(d0.getDay()) || f.dow.values.has(d0.getDay() + 7)
    if (!(domRestricted && dowRestricted ? domMatch || dowMatch : domMatch && dowMatch)) continue
    for (const hour of hours) {
      for (const minute of minutes) {
        const ts = new Date(d0.getFullYear(), d0.getMonth(), d0.getDate(), hour, minute).getTime()
        if (ts > fromMs) return ts
      }
    }
  }
  throw new Error(`无法计算下次执行时间（cron 表达式可能永不触发）: ${raw}`)
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
  /** 安全模式（GEBAI_SAFE_MODE=true 启动时加载）：script 型定时任务被阻止执行（跳过并落盘提示）。 */
  safeMode?: boolean
  /** prompt 型任务执行引擎（构造时可缺省，经 attach 注入，避免与 engine 互相依赖构造）。 */
  engine?: AgentEngine
}

export class CronManager {
  private entries = new Map<string, CronTask>()
  private timer: ReturnType<typeof setInterval> | null = null
  /** 执行中的任务 id（重入防护）：script 单次执行可远超 tick 间隔，nextRunAt 完成后才推进。 */
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

  /** 扫描全部用户会话加载 cron.json 并启动 tick 循环。停机期间错过的触发不补跑（下次从当前时间重算）。 */
  async start(): Promise<void> {
    const now = this.now()
    const base = join(this.deps.home, "users")
    await walkDir(base, 5, async (p) => {
      if (!p.endsWith("cron.json")) return
      try {
        const raw = await readFile(p, "utf8")
        const tasks = JSON.parse(raw)
        if (!Array.isArray(tasks)) return
        for (const t of tasks) {
          if (!t || typeof t.id !== "string" || !t.sessionId || !t.user || typeof t.schedule !== "string") continue
          const entry = t as CronTask
          // schedule 合法性校验（add/update 时已拒，此处防外部编辑损坏）：非法表达式直接禁用，
          // 否则 nextTime 解析失败回退 +30s 会形成每 30s 触发一次的热循环
          if (entry.enabled) {
            try {
              parseCronSchedule(entry.schedule)
            } catch {
              entry.enabled = false
              entry.lastError = "schedule 表达式非法：启动加载时已禁用（请修正后重新启用）"
            }
          }
          if (entry.enabled && (typeof entry.nextRunAt !== "number" || entry.nextRunAt <= now)) {
            entry.nextRunAt = this.nextTime(entry, now)
          }
          this.entries.set(entry.id, entry)
        }
      } catch {
        /* 跳过损坏文件 */
      }
    })
    if (this.timer) return
    this.timer = setInterval(() => void this.tick(), this.deps.tickIntervalMs ?? CRON_TICK_INTERVAL_MS)
    this.timer.unref?.()
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer)
    this.timer = null
  }

  async add(sessionId: string, user: string, input: CronCreateInput): Promise<CronTask> {
    if (input.type !== "script" && input.type !== "prompt") throw new Error(`无效的定时任务类型: ${String(input.type)}`)
    const schedule = String(input.schedule ?? "").trim()
    if (!schedule) throw new Error("缺少定时表达式（schedule）")
    const script = input.script != null ? String(input.script).trim() : ""
    const prompt = input.prompt != null ? String(input.prompt).trim() : ""
    if (input.type === "script" && !script) throw new Error("脚本型定时任务需要 script 参数（shell 命令）")
    if (input.type === "prompt" && !prompt) throw new Error("提示词型定时任务需要 prompt 参数")
    const now = this.now()
    const entry: CronTask = {
      id: randomUUID().replace(/-/g, ""),
      sessionId,
      user,
      name: input.name ? String(input.name).trim().slice(0, 100) || undefined : undefined,
      type: input.type,
      schedule,
      script: input.type === "script" ? script : undefined,
      prompt: input.type === "prompt" ? prompt : undefined,
      enabled: input.enabled !== false,
      createdAt: now,
      updatedAt: now,
      nextRunAt: 0,
      runCount: 0,
    }
    // 创建时严格校验表达式（非法即拒绝），并计算下次执行时间
    entry.nextRunAt = parseCronSchedule(schedule).next(now)
    this.entries.set(entry.id, entry)
    await this.saveSessionEntries(sessionId, user)
    return { ...entry }
  }

  async update(sessionId: string, user: string, id: string, patch: CronUpdateInput): Promise<CronTask | null> {
    const entry = this.entryOf(sessionId, user, id)
    if (!entry) return null
    if (patch.name !== undefined) entry.name = String(patch.name).trim().slice(0, 100) || undefined
    if (patch.schedule !== undefined) {
      const s = String(patch.schedule).trim()
      if (!s) throw new Error("定时表达式不能为空")
      entry.schedule = s
    }
    if (patch.type !== undefined) {
      if (patch.type !== "script" && patch.type !== "prompt") throw new Error(`无效的定时任务类型: ${String(patch.type)}`)
      entry.type = patch.type
    }
    if (patch.script !== undefined) entry.script = String(patch.script).trim() || undefined
    if (patch.prompt !== undefined) entry.prompt = String(patch.prompt).trim() || undefined
    if (entry.type === "script" && !entry.script) throw new Error("脚本型定时任务需要 script 参数（shell 命令）")
    if (entry.type === "prompt" && !entry.prompt) throw new Error("提示词型定时任务需要 prompt 参数")
    if (patch.enabled !== undefined) entry.enabled = patch.enabled
    entry.updatedAt = this.now()
    if (entry.enabled) {
      // 修改周期/类型/内容后严格校验表达式并重算下次执行时间
      entry.nextRunAt = parseCronSchedule(entry.schedule).next(this.now())
    }
    await this.saveSessionEntries(sessionId, user)
    return { ...entry }
  }

  async remove(sessionId: string, user: string, id: string): Promise<boolean> {
    const entry = this.entryOf(sessionId, user, id)
    if (!entry) return false
    this.entries.delete(id)
    await this.saveSessionEntries(sessionId, user)
    return true
  }

  async list(sessionId: string, user: string): Promise<CronTask[]> {
    return [...this.entries.values()]
      .filter((e) => e.sessionId === sessionId && e.user === user)
      .sort((a, b) => a.createdAt - b.createdAt)
      .map((t) => ({ ...t }))
  }

  /** 定时触发检查（tick 循环与测试共用的核心入口）。 */
  async tick(): Promise<void> {
    const now = this.now()
    for (const entry of [...this.entries.values()]) {
      if (!entry.enabled || entry.nextRunAt > now) continue
      try {
        await this.fire(entry, now)
      } catch (err) {
        // fire 抛错（会话已删除/引擎不可用/存储失败等）：必须重算下次执行时间，
        // 否则 nextRunAt 停留在过去 → 每个 tick（30 秒）都会重试失败任务，形成无限重试热循环
        entry.lastError = String((err as Error).message || err).slice(0, 500)
        entry.lastStatus = "error"
        entry.updatedAt = now
        entry.nextRunAt = this.nextTime(entry, now)
        this.publish(entry.sessionId, "event.cron.result", {
          id: entry.id,
          type: entry.type,
          name: entry.name ?? "",
          ok: false,
          error: entry.lastError,
        })
        await this.saveSessionEntries(entry.sessionId, entry.user)
      }
    }
  }

  private async fire(entry: CronTask, now: number): Promise<void> {
    // 重入防护：script 型单次执行可长达 CRON_SCRIPT_TIMEOUT_MS（远超 30s tick 间隔），而 nextRunAt
    // 要等执行结束才推进——无防护时每 tick 都会再次 fire 同一任务，长脚本被并发叠加执行（副作用重复）。
    // 执行中跳过本轮触发。
    if (this.firing.has(entry.id)) return
    this.firing.add(entry.id)
    try {
      await this.fireInner(entry, now)
    } finally {
      this.firing.delete(entry.id)
    }
  }

  private async fireInner(entry: CronTask, now: number): Promise<void> {
    const { sessionId, user } = entry
    // 会话已删除（内存缓存可能残留）：校验磁盘目录真实存在，不存在则清理内存条目
    const chatFile = join(sessionPath(this.deps.home, user, sessionId), "chat.json")
    if (!existsSync(chatFile)) {
      this.entries.delete(entry.id)
      return
    }
    const session = await this.deps.store.load(sessionId, user)
    if (!session) {
      this.entries.delete(entry.id)
      return
    }
    this.publish(sessionId, "event.cron.run", { id: entry.id, type: entry.type, name: entry.name ?? "", schedule: entry.schedule })
    let ran = false
    if (entry.type === "script") {
      // 安全模式：script 型任务直接执行 shell（不经工具拦截），已建任务同样阻止——跳过并落盘提示；
      // 不提前 return：走统一收尾（推进 nextRunAt 防高频任务每 tick 重复刷提示，publish event.cron.result）
      if (this.deps.safeMode) {
        const note = `[定时任务${entry.name ? `「${entry.name}」` : ""}已跳过] 安全模式：脚本执行已限制（安全模式下仅允许只读操作）。`
        await this.deps.store.appendMessage(sessionId, {
          id: randomUUID(),
          role: "assistant",
          content: note,
          createdAt: now,
        })
        entry.lastStatus = "skipped"
        entry.lastOutput = "安全模式：脚本执行已限制"
        entry.lastError = "safe-mode"
        ran = true
      } else {
        const env = await this.deps.env.resolve(sessionId, user)
        const { stdout, stderr, code } = await this.deps.sandbox.exec(entry.script ?? "", {
          cwd: this.deps.store.getTmpDir(sessionId, user),
          env,
          timeoutMs: CRON_SCRIPT_TIMEOUT_MS,
          user,
        })
        const out = code === 0 ? stdout : `${stdout}${stdout && stderr ? "\n" : ""}${stderr}\n[exit ${code}]`
        const ok = code === 0
        await this.deps.store.appendMessage(sessionId, {
          id: randomUUID(),
          role: "assistant",
          content: `[定时任务${entry.name ? `「${entry.name}」` : ""}执行结果（${ok ? "成功" : "失败"}）]\n${out}`.slice(0, CRON_MESSAGE_MAX),
          createdAt: now,
        })
        entry.lastStatus = ok ? "success" : "error"
        entry.lastOutput = out.slice(0, CRON_OUTPUT_MAX)
        entry.lastError = ok ? undefined : `exit ${code}`
        ran = true
      }
    } else {
      if (!this.engine) throw new Error("定时任务引擎未就绪")
      // 提示词运行 agent：复用主循环（会话正有任务运行时跳过本轮，避免并发写会话）
      if (this.engine.isRunning(sessionId)) {
        entry.lastStatus = "skipped"
        entry.lastOutput = "会话正在运行其他任务，跳过本次触发"
      } else {
        const promptText = `[定时任务${entry.name ? `「${entry.name}」` : ""}触发]\n${entry.prompt ?? ""}`
        await this.engine.run(sessionId, user, promptText)
        entry.lastStatus = "success"
        entry.lastOutput = "已触发，执行过程与结果见会话消息"
        ran = true
      }
    }
    if (ran) {
      entry.lastRunAt = now
      entry.runCount += 1
    }
    entry.nextRunAt = this.nextTime(entry, now)
    entry.updatedAt = now
    await this.saveSessionEntries(sessionId, user)
    if (entry.type === "script") {
      this.publish(sessionId, "event.cron.result", {
        id: entry.id,
        type: entry.type,
        name: entry.name ?? "",
        ok: entry.lastStatus === "success",
        output: entry.lastOutput,
        error: entry.lastError,
      })
    }
  }

  private nextTime(entry: CronTask, fromMs: number): number {
    // 解析即校验：非法表达式在 add/update 时即拒绝，此处解析失败按永不触发处理（保留旧值）
    try {
      return parseCronSchedule(entry.schedule).next(fromMs)
    } catch {
      return fromMs + CRON_TICK_INTERVAL_MS
    }
  }

  private entryOf(sessionId: string, user: string, id: string): CronTask | undefined {
    const entry = this.entries.get(id)
    return entry && entry.sessionId === sessionId && entry.user === user ? entry : undefined
  }

  private cronFile(sessionId: string, user: string): string {
    return join(sessionPath(this.deps.home, user, sessionId), "cron.json")
  }

  private saveSessionEntries(sessionId: string, user: string): Promise<void> {
    const tasks = [...this.entries.values()].filter((e) => e.sessionId === sessionId && e.user === user)
    const prev = this.writes.get(sessionId) ?? Promise.resolve()
    const next = prev
      .then(async () => {
        const file = this.cronFile(sessionId, user)
        await mkdir(dirname(file), { recursive: true })
        await writeFile(file, JSON.stringify(tasks, null, 2))
      })
      .catch(() => {})
    this.writes.set(sessionId, next)
    return next
  }

  private publish(sessionId: string, type: string, payload: Record<string, unknown>): void {
    this.deps.events.publish({ type, sessionId, payload, timestamp: this.now() } as AgentEvent)
  }
}
