/** 用户级待办与闲时任务（DESIGN「用户级待办与闲时任务」）：待办清单是**用户级资源**
 *  （users/{user}/todos.json，随用户目录生命周期，与会话删除/过期解耦），与引擎会话级待办
 *  （agent 自己维护的任务清单，随会话走）语义不同、互不干扰。
 *
 *  闲时任务：待办可标记 idle——服务端**没有正在运行的会话**时（engine 全局空闲判定），调度器按
 *  列表顺序取第一条待执行的闲时待办，**新建一条会话**执行其内容（一次一条、串行推进），执行完
 *  自动勾选完成并回写结果摘要（可从该会话回看完整过程）；失败累计 idleAttempts，达上限自动
 *  放弃并记 idleError（防死循环重试）。手动执行（`run`，REST POST /api/v1/todos/:id/run）走同一条
 *  执行链路（同样是新建会话），仅不受「服务端空闲」限制（用户显式要求立即执行）。
 *
 *  存储范式与定时任务（cron.ts）一致：启动 walkDir 扫描加载 + Map 驻留 + 按用户串行写链。 */
import { randomUUID } from "node:crypto"
import { mkdir, readFile, writeFile } from "node:fs/promises"
import { dirname, join, relative, sep } from "node:path"
import { walkDir } from "../base/paths"
import type { AgentEngine } from "../engine/engine"
import type { SessionStore } from "../session/store"

/** 闲时调度器 tick 周期（与定时任务同量级；每 tick 至多启动一条闲时待办）。 */
export const IDLE_TODO_TICK_INTERVAL_MS = 30_000
/** 单条闲时待办执行超时缺省（到时取消该会话任务，按失败计次）。 */
export const IDLE_TODO_TIMEOUT_MS = 30 * 60 * 1000
/** 闲时待办连续失败上限（达上限自动放弃闲时执行，保留待办与错误原因待人工处理）。 */
export const IDLE_TODO_MAX_ATTEMPTS = 3
/** 待办内容长度上限（也是闲时任务的提示词）。 */
export const TODO_TEXT_MAX = 2000
/** 闲时执行结果摘要保留长度（写入待办记录，完整结果见执行会话）。 */
export const TODO_RESULT_MAX = 1000
/** 单用户待办条数上限（防无限增长；超出拒绝新增）。 */
export const TODO_MAX_ITEMS = 500
/** 闲时执行会话标题里待办摘要的长度。 */
const TODO_HEADLINE_MAX = 40

/** 闲时执行状态：pending 排队中 / running 执行中 / done 已成功执行 / failed 已放弃（达失败上限）。 */
export type UserTodoIdleState = "pending" | "running" | "done" | "failed"

/** 用户级待办条目（持久化于 users/{user}/todos.json；数组顺序即清单顺序）。 */
export interface UserTodo {
  id: string
  /** 归属用户（多用户共库时定位与鉴权依据）。 */
  user: string
  /** 待办内容；标记为闲时任务时同时作为执行提示词。 */
  text: string
  /** 是否已完成（闲时任务执行成功后自动置真）。 */
  done: boolean
  /** 是否闲时任务：服务端没有运行的会话时按顺序自动执行。 */
  idle: boolean
  createdAt: number
  updatedAt: number
  /** 闲时执行状态（未标记闲时时缺省）。 */
  idleState?: UserTodoIdleState
  /** 已尝试执行次数（成功或失败均计；重置闲时标记时清零）。 */
  idleAttempts?: number
  /** 最近一次执行失败原因（成功时清除）。 */
  idleError?: string
  /** 最近一次执行时间。 */
  idleRunAt?: number
  /** 最近一次执行所在的会话（完整过程与结果在此回看）。 */
  idleSessionId?: string
  /** 最近一次执行结果摘要（执行会话最后一条回复的截断）。 */
  idleResult?: string
}

/** 新建输入。 */
export interface UserTodoCreateInput {
  text: string
  /** 是否闲时任务（缺省 false）。 */
  idle?: boolean
}

/** 更新输入（字段缺省即不改动）。 */
export interface UserTodoUpdateInput {
  text?: string
  done?: boolean
  idle?: boolean
}

/** 调度器依赖：结构接口（与 CronManagerDeps 同风格），便于测试替身注入。 */
export interface UserTodoManagerDeps {
  home: string
  store: SessionStore
  /** 闲时执行引擎（构造期可缺省，经 attach 注入，避免与 engine 互相依赖构造）。 */
  engine?: AgentEngine
  /** 可注入时钟（测试用），默认 Date.now。 */
  now?: () => number
  tickIntervalMs?: number
  timeoutMs?: number
  maxAttempts?: number
}

/** 闲时执行会话标题前缀 / 手动执行会话标题前缀（新建会话的可见名，附待办摘要）。 */
const IDLE_TITLE = "闲时待办"
const MANUAL_TITLE = "待办执行"
/** 闲时执行 / 手动执行的提示词前缀（正文即待办全文，作为完整提示词交给模型）。 */
const IDLE_PROMPT_HEAD = "[闲时待办任务]"
const MANUAL_PROMPT_HEAD = "[待办执行]"

/** 待办正在执行中（同一待办并发触发）：路由据此返回 409。 */
export class TodoBusyError extends Error {}

/** 取待办摘要（会话标题用）：首行 + 截断。 */
function headline(text: string): string {
  const first = text.trim().split("\n")[0] ?? ""
  const s = first.length > TODO_HEADLINE_MAX ? `${first.slice(0, TODO_HEADLINE_MAX)}…` : first
  return s || "未命名"
}

export class UserTodoManager {
  private entries = new Map<string, UserTodo>()
  private timer: ReturnType<typeof setInterval> | null = null
  /** 单飞标记：一次只执行一条闲时待办（执行可能远超 tick 间隔，防并发叠加）。 */
  private firing = false
  private writes = new Map<string, Promise<void>>()
  private engine: AgentEngine | undefined
  private now: () => number
  private tickMs: number
  private timeoutMs: number
  private maxAttempts: number

  constructor(private deps: UserTodoManagerDeps) {
    this.engine = deps.engine
    this.now = deps.now ?? (() => Date.now())
    this.tickMs = deps.tickIntervalMs ?? IDLE_TODO_TICK_INTERVAL_MS
    this.timeoutMs = deps.timeoutMs ?? IDLE_TODO_TIMEOUT_MS
    this.maxAttempts = deps.maxAttempts ?? IDLE_TODO_MAX_ATTEMPTS
  }

  /** 注入执行引擎（闲时任务执行器；构造期缺省时调用）。 */
  attach(engine: AgentEngine): void {
    this.engine = engine
  }

  /** 用户级待办存储文件（users/{user}/todos.json，随用户目录生命周期）。 */
  private userTodoFile(user: string): string {
    return join(this.deps.home, "users", user, "todos.json")
  }

  /** 扫描加载用户级待办，并启动闲时调度 tick 循环。 */
  async start(): Promise<void> {
    const base = join(this.deps.home, "users")
    await walkDir(base, 5, async (p) => {
      if (!p.endsWith("todos.json")) return
      const rel = relative(base, p).split(sep)
      if (rel.length !== 2 || rel[1] !== "todos.json") return
      try {
        const raw = JSON.parse(await readFile(p, "utf8"))
        if (!Array.isArray(raw)) return
        for (const t of raw) {
          const entry = this.normalizeLoaded(t)
          if (entry && !this.entries.has(entry.id)) this.entries.set(entry.id, entry)
        }
      } catch {
        /* 跳过损坏文件 */
      }
    })
    if (this.timer) return
    this.timer = setInterval(() => void this.tick(), this.tickMs)
    this.timer.unref?.()
  }

  /** 加载条目归一化（外部编辑损坏的条目直接丢弃，不影响其余清单）。 */
  private normalizeLoaded(t: unknown): UserTodo | null {
    if (!t || typeof t !== "object") return null
    const e = t as UserTodo
    if (typeof e.id !== "string" || !/^[0-9a-f]{32}$/.test(e.id)) return null
    if (typeof e.user !== "string" || !e.user) return null
    if (typeof e.text !== "string" || !e.text.trim()) return null
    const now = this.now()
    return {
      id: e.id,
      user: e.user,
      text: e.text.slice(0, TODO_TEXT_MAX),
      done: e.done === true,
      idle: e.idle === true,
      createdAt: typeof e.createdAt === "number" ? e.createdAt : now,
      updatedAt: typeof e.updatedAt === "number" ? e.updatedAt : now,
      ...(e.idleState === "pending" || e.idleState === "running" || e.idleState === "done" || e.idleState === "failed" ? { idleState: e.idleState } : {}),
      ...(typeof e.idleAttempts === "number" ? { idleAttempts: e.idleAttempts } : {}),
      ...(typeof e.idleError === "string" ? { idleError: e.idleError } : {}),
      ...(typeof e.idleRunAt === "number" ? { idleRunAt: e.idleRunAt } : {}),
      ...(typeof e.idleSessionId === "string" ? { idleSessionId: e.idleSessionId } : {}),
      ...(typeof e.idleResult === "string" ? { idleResult: e.idleResult } : {}),
    }
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer)
    this.timer = null
  }

  /** 用户待办清单（数组顺序即清单顺序）。 */
  async list(user: string): Promise<UserTodo[]> {
    return [...this.entries.values()].filter((e) => e.user === user).map((e) => ({ ...e }))
  }

  private entryOf(user: string, id: string): UserTodo | undefined {
    const entry = this.entries.get(id)
    return entry && entry.user === user ? entry : undefined
  }

  /** 归一化待办文本（非空 + 长度上限）。 */
  private normalizeText(input: unknown): string {
    const text = String(input ?? "").trim()
    if (!text) throw new Error("待办内容不能为空")
    if (text.length > TODO_TEXT_MAX) throw new Error(`待办内容过长（上限 ${TODO_TEXT_MAX} 字符）`)
    return text
  }

  async add(user: string, input: UserTodoCreateInput): Promise<UserTodo> {
    const text = this.normalizeText(input?.text)
    const count = [...this.entries.values()].filter((e) => e.user === user).length
    if (count >= TODO_MAX_ITEMS) throw new Error(`待办条数已达上限（${TODO_MAX_ITEMS}），请先清理已完成项`)
    const now = this.now()
    const idle = input?.idle === true
    const entry: UserTodo = {
      id: randomUUID().replace(/-/g, ""),
      user,
      text,
      done: false,
      idle,
      createdAt: now,
      updatedAt: now,
      ...(idle ? { idleState: "pending" as const, idleAttempts: 0 } : {}),
    }
    this.entries.set(entry.id, entry) // Map 保持插入序 → 新待办追加在清单末尾
    await this.saveUserEntries(user)
    return { ...entry }
  }

  async update(user: string, id: string, patch: UserTodoUpdateInput): Promise<UserTodo | null> {
    const entry = this.entryOf(user, id)
    if (!entry) return null
    if (typeof patch?.text === "string") entry.text = this.normalizeText(patch.text)
    if (typeof patch?.done === "boolean") {
      entry.done = patch.done
      // 取消勾选视为重新排队（若仍是闲时任务，下一轮空闲会再执行一次）
      if (!patch.done && entry.idle) entry.idleState = "pending"
    }
    if (typeof patch?.idle === "boolean" && patch.idle !== entry.idle) {
      entry.idle = patch.idle
      if (patch.idle) {
        // 开启闲时执行：重置失败计数与状态，重新排队
        entry.idleState = "pending"
        entry.idleAttempts = 0
        entry.idleError = undefined
      } else {
        entry.idleState = undefined
        entry.idleError = undefined
      }
    }
    entry.updatedAt = this.now()
    await this.saveUserEntries(user)
    return { ...entry }
  }

  async remove(user: string, id: string): Promise<boolean> {
    const entry = this.entryOf(user, id)
    if (!entry) return false
    this.entries.delete(id)
    await this.saveUserEntries(user)
    return true
  }

  /** 拖动排序：按给定 id 顺序重排该用户清单（未列出的条目按原序追加在后，避免并发新增丢失）。 */
  async reorder(user: string, ids: string[]): Promise<UserTodo[]> {
    if (!Array.isArray(ids)) throw new Error("缺少顺序数组（ids）")
    const mine = [...this.entries.values()].filter((e) => e.user === user)
    const byId = new Map(mine.map((e) => [e.id, e]))
    const queue: UserTodo[] = []
    for (const raw of ids) {
      const e = byId.get(String(raw))
      if (!e) continue
      byId.delete(e.id)
      queue.push(e)
    }
    for (const e of mine) if (byId.has(e.id)) queue.push(e)
    // 重建 Map：本用户条目占据原有位置槽、按新顺序填充，其余用户条目保持原相对序
    const next = new Map<string, UserTodo>()
    for (const [key, value] of this.entries) {
      if (value.user !== user) {
        next.set(key, value)
        continue
      }
      const e = queue.shift()
      if (e) next.set(e.id, e)
    }
    this.entries = next
    await this.saveUserEntries(user)
    return this.list(user)
  }

  /** 下一条待执行的闲时待办：按清单顺序取第一条未完成、未放弃、未执行的。 */
  private nextIdle(): UserTodo | undefined {
    for (const e of this.entries.values()) {
      if (!e.idle || e.done) continue
      if (e.idleState === "running" || e.idleState === "done" || e.idleState === "failed") continue
      if ((e.idleAttempts ?? 0) >= this.maxAttempts) continue
      return e
    }
    return undefined
  }

  /** 闲时调度 tick（循环与测试共用入口）：空闲且有排队中的闲时待办时执行一条。 */
  async tick(): Promise<void> {
    if (this.firing) return
    const engine = this.engine
    if (!engine) return
    // 服务端没有正在运行的会话时才执行（用户会话优先；忙碌时不抢资源，下个 tick 再评估）
    if (engine.busy()) return
    const entry = this.nextIdle()
    if (!entry) return
    this.firing = true
    try {
      await this.runIdle(entry)
    } finally {
      this.firing = false
    }
  }

  /** 执行一条闲时待办（tick 路径）：新建会话执行 → 回写状态（成功自动勾选完成）。 */
  private async runIdle(entry: UserTodo): Promise<void> {
    let sid: string
    try {
      sid = await this.openRunSession(entry, IDLE_TITLE)
    } catch (err) {
      await this.recordFailure(entry, err)
      return
    }
    await this.runInSession(entry, sid, IDLE_PROMPT_HEAD)
  }

  /** 手动立即执行（REST POST /api/v1/todos/:id/run）：**新建一条会话**执行，不等待完成——建会话完成即
   *  返回其 id（前端可据此跳转/提示），结果后续回写待办；不受「服务端空闲」限制（用户显式要求）。
   *  返回 null 表示待办不存在；同一待办已在执行中抛 TodoBusyError（路由 409）。 */
  async run(user: string, id: string): Promise<{ todo: UserTodo; sessionId: string } | null> {
    const entry = this.entryOf(user, id)
    if (!entry) return null
    if (!this.engine) throw new Error("执行引擎未就绪")
    // 同步占位防重入（并发点击/与 tick 撞车）：占位在建会话的 await 之前完成
    if (entry.idleState === "running") throw new TodoBusyError("该待办正在执行中，请稍候")
    entry.idleState = "running"
    entry.idleError = undefined
    let sid: string
    try {
      sid = await this.openRunSession(entry, MANUAL_TITLE)
    } catch (err) {
      await this.recordFailure(entry, err)
      throw err
    }
    void this.runInSession(entry, sid, MANUAL_PROMPT_HEAD)
    return { todo: { ...entry }, sessionId: sid }
  }

  /** 建立执行会话并登记（手动与闲时共用）：新建一条会话 + 标记 running/执行会话并落盘，返回会话 id。 */
  private async openRunSession(entry: UserTodo, titlePrefix: string): Promise<string> {
    const session = await this.deps.store.createSession(entry.user, `${titlePrefix} · ${headline(entry.text)}`)
    const startAt = this.now()
    entry.idleState = "running"
    entry.idleRunAt = startAt
    entry.idleSessionId = session.id
    entry.idleError = undefined
    entry.updatedAt = startAt
    await this.saveUserEntries(entry.user)
    return session.id
  }

  /** 在已建会话内执行待办内容并回写状态（手动与闲时共用链路）：**完整 Agent 循环**跑该待办文本
   *  （作为详细提示词）。成功自动勾选完成并记结果摘要；失败/超时累计尝试次数，达上限停止自动执行。 */
  private async runInSession(entry: UserTodo, sid: string, promptHead: string): Promise<void> {
    const engine = this.engine
    if (!engine) return
    let status: "success" | "error" | "timeout" = "success"
    let error: string | undefined
    let timedOut = false
    // 注意不可 unref：await 挂起的 Promise 不保活事件循环，unref 定时器在「仅剩本定时器」场景永不触发
    const timer = setTimeout(() => {
      timedOut = true
      engine.cancel(sid)
    }, this.timeoutMs)
    try {
      await engine.run(sid, entry.user, `${promptHead}\n${entry.text}`)
    } catch (err) {
      // 超时主动取消的拒绝不算异常（按 timeout 记录）
      if (!timedOut) {
        status = "error"
        error = String((err as Error).message || err).slice(0, 500)
      }
    } finally {
      clearTimeout(timer)
    }
    if (timedOut) {
      status = "timeout"
      error = `执行超时（${Math.round(this.timeoutMs / 1000)}s），已终止`
    }

    const endedAt = this.now()
    entry.idleAttempts = (entry.idleAttempts ?? 0) + 1
    entry.idleRunAt = endedAt
    entry.updatedAt = endedAt
    if (status === "success") {
      entry.done = true
      entry.idleState = "done"
      entry.idleError = undefined
      entry.idleResult = await this.lastAssistantText(sid, entry.user)
    } else {
      entry.idleError = error ?? status
      entry.idleState = entry.idleAttempts >= this.maxAttempts ? "failed" : "pending"
      if (entry.idleState === "failed") {
        entry.idleError = `${entry.idleError}；已累计失败 ${entry.idleAttempts} 次，已停止闲时自动执行（可关闭再开启闲时任务以重试）`
      }
    }
    await this.saveUserEntries(entry.user)
  }

  /** 执行前置失败（建会话异常等）：如实计次并回写原因，防状态卡在 running。 */
  private async recordFailure(entry: UserTodo, err: unknown): Promise<void> {
    const endedAt = this.now()
    entry.idleAttempts = (entry.idleAttempts ?? 0) + 1
    entry.idleError = String((err as Error)?.message || err).slice(0, 500)
    entry.idleState = entry.idleAttempts >= this.maxAttempts ? "failed" : "pending"
    entry.updatedAt = endedAt
    entry.idleRunAt = endedAt
    await this.saveUserEntries(entry.user)
  }

  /** 执行结果摘要：执行会话最后一条 assistant 消息（截断）。 */
  private async lastAssistantText(sessionId: string, user: string): Promise<string | undefined> {
    try {
      const session = await this.deps.store.load(sessionId, user)
      const msg = session ? [...session.messages].reverse().find((m) => m.role === "assistant" && typeof m.content === "string") : undefined
      return msg?.content ? msg.content.slice(0, TODO_RESULT_MAX) : undefined
    } catch {
      return undefined
    }
  }

  /** 落盘：按用户串行化写链（并发写不互相覆盖）。 */
  private saveUserEntries(user: string): Promise<void> {
    const todos = [...this.entries.values()].filter((e) => e.user === user)
    const prev = this.writes.get(user) ?? Promise.resolve()
    const next = prev
      .then(async () => {
        const file = this.userTodoFile(user)
        await mkdir(dirname(file), { recursive: true })
        await writeFile(file, JSON.stringify(todos, null, 2))
      })
      .catch(() => {})
    this.writes.set(user, next)
    return next
  }
}
