/**
 * 用户级待办（DESIGN「用户级待办」）：待办清单是**用户级资源**（`users/{user}/todos.json`，随用户目录
 * 生命周期，与会话删除/过期解耦），与引擎会话级待办（agent 自己维护的任务清单，随会话走）语义不同。
 *
 * 待办与统一任务（`core/schedule/tasks.ts`）**保持独立概念、按需关联**：
 * - 手动执行（`POST /api/v1/todos/:id/run`）—— 任何待办随时可执行：已绑定任务则入队该任务，否则建一条
 *   一次性普通任务（prompt=待办文本，执行完自动删除）入队，按统一队列的顺序与额度执行；
 * - 闲时自动执行（`todo.idle`）—— 开启时为待办**绑定一个闲时任务**（kind=idle，`todoId` 关联），由任务
 *   调度器在队列空闲且该用户无运行中会话时串行执行；关闭开关即删除绑定任务。
 *
 * 执行结果由任务调度器回调回写（`recordTaskResult`）：成功自动勾选完成并停用绑定任务；失败（含超时）
 * **关闭闲时自动执行**（`idle=false`、`idleState=failed`、记 `idleError`）并停用绑定任务——不自动重试，
 * 用户重新开启 ⚡ 即重置状态、下次队列空闲继续执行。
 *
 * 存储范式与任务一致：启动 walkDir 扫描加载 + Map 驻留 + **磁盘真值 RMW** 落盘（跨进程写锁 + 原子写）。
 */
import { randomUUID } from "node:crypto"
import { readFile } from "node:fs/promises"
import { join, relative, sep } from "node:path"
import type { Task, TaskRunRecord } from "@gebai/sdk"
import { walkDir } from "../base/paths"
import { mutateJsonList } from "../support/json-store"
import type { SessionStore } from "../session/store"
import type { TaskManager } from "./tasks"

/** 待办内容长度上限（也是执行时的提示词）。 */
export const TODO_TEXT_MAX = 2000
/** 执行结果摘要保留长度（写入待办记录，完整结果见执行会话）。 */
export const TODO_RESULT_MAX = 1000
/** 单用户待办条数上限（防无限增长；超出拒绝新增）。 */
export const TODO_MAX_ITEMS = 500
/** 待办闲时任务单次执行超时（闲时执行常为长任务，缺省给足 3 小时；可按任务 timeoutMs 覆盖）。 */
export const TODO_IDLE_TIMEOUT_MS = 3 * 60 * 60 * 1000
/** 执行会话标题里待办摘要的长度。 */
const TODO_HEADLINE_MAX = 40

/** 待办执行状态：pending 排队中 / running 执行中 / done 已成功执行 / failed 已失败停执行（需重新开启 ⚡）。 */
export type UserTodoIdleState = "pending" | "running" | "done" | "failed"

/** 用户级待办条目（持久化于 users/{user}/todos.json；数组顺序即清单顺序）。 */
export interface UserTodo {
  id: string
  /** 归属用户（多用户共库时定位与鉴权依据）。 */
  user: string
  /** 待办内容；执行时同时作为提示词。 */
  text: string
  /** 是否已完成（执行成功后自动置真）。 */
  done: boolean
  /** 是否闲时自动执行（开启后由服务端空闲时自动执行）。 */
  idle: boolean
  createdAt: number
  updatedAt: number
  /** 绑定的闲时任务 id（开启闲时自动执行时生成，关闭即删除）。 */
  idleTaskId?: string
  /** 执行状态（闲时自动执行；手动执行的入队不改变此状态）。 */
  idleState?: UserTodoIdleState
  /** 已尝试执行次数（成功或失败均计；重开闲时自动执行时清零）。 */
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
  /** 是否闲时自动执行（缺省 false）。 */
  idle?: boolean
}

/** 更新输入（字段缺省即不改动）。 */
export interface UserTodoUpdateInput {
  text?: string
  done?: boolean
  idle?: boolean
}

/** 手动执行结果（入队形态）。 */
export interface UserTodoRunResult {
  todo: UserTodo
  /** 是否已入队（任务已停用等情形为 false，reason 说明原因）。 */
  queued: boolean
  /** 队列内位置（1 起）。 */
  position?: number
  reason?: string
  /** 本次执行入队的任务（已绑定任务或一次性任务）。 */
  taskId: string
  /** 一次性任务（执行完自动删除）。 */
  ephemeral: boolean
}

/** 管理器依赖：结构接口（便于测试替身注入）。 */
export interface UserTodoManagerDeps {
  home: string
  store: SessionStore
  /** 统一任务调度器（手动执行入队与闲时任务绑定；能力未启用时缺省）。 */
  tasks?: TaskManager
  /** 可注入时钟（测试用），默认 Date.now。 */
  now?: () => number
}

/** 取待办摘要（任务名/会话标题用）：首行 + 截断。 */
function headline(text: string): string {
  const first = text.trim().split("\n")[0] ?? ""
  const s = first.length > TODO_HEADLINE_MAX ? `${first.slice(0, TODO_HEADLINE_MAX)}…` : first
  return s || "未命名"
}

export class UserTodoManager {
  private entries = new Map<string, UserTodo>()
  private tasks: TaskManager | undefined
  private now: () => number

  constructor(private deps: UserTodoManagerDeps) {
    this.tasks = deps.tasks
    this.now = deps.now ?? (() => Date.now())
  }

  /** 后挂任务调度器（构造期缺省时调用）。 */
  attach(tasks: TaskManager): void {
    this.tasks = tasks
  }

  /** 用户级待办存储文件（users/{user}/todos.json，随用户目录生命周期）。 */
  private userTodoFile(user: string): string {
    return join(this.deps.home, "users", user, "todos.json")
  }

  /** 扫描加载用户级待办，并为开启闲时自动执行的待办补齐绑定任务。 */
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
    // 上一进程中断遗留的执行中状态复位（任务侧重启也按中断处理，两侧一致）：
    // 必须**落盘**——后续 RMW 以磁盘真值为基准，未落盘的复位会在下一次写入时被磁盘旧值覆盖
    for (const user of new Set([...this.entries.values()].map((e) => e.user))) {
      await this.persist(user, (disk) => disk.map((e) => (e.idleState === "running" ? { ...e, idleState: "pending" as const } : e))).catch(() => {})
    }
    // 闲时自动执行的待办：补齐/校正绑定任务（旧数据升级路径：仅有 idle 标记而无绑定任务）
    for (const entry of this.entries.values()) {
      if (!entry.idle || entry.done) continue
      await this.ensureIdleTask(entry).catch(() => {})
    }
    // 闲时任务排队顺序 = 待办清单顺序（保留「按清单顺序自动执行」的用户可见语义）
    this.tasks?.setIdleOrder((user) => this.boundIdleTaskIds(user))
  }

  /** 该用户已绑定闲时任务的 id 序列（按待办清单顺序；任务调度器据此排序闲时执行）。 */
  boundIdleTaskIds(user: string): string[] {
    const out: string[] = []
    for (const e of this.entries.values()) {
      if (e.user === user && e.idle && !e.done && e.idleTaskId) out.push(e.idleTaskId)
    }
    return out
  }

  stop(): void {
    /* 调度已统一到任务队列：待办侧无自有定时器 */
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
      ...(typeof e.idleTaskId === "string" ? { idleTaskId: e.idleTaskId } : {}),
      ...(e.idleState === "pending" || e.idleState === "running" || e.idleState === "done" || e.idleState === "failed" ? { idleState: e.idleState } : {}),
      ...(typeof e.idleAttempts === "number" ? { idleAttempts: e.idleAttempts } : {}),
      ...(typeof e.idleError === "string" ? { idleError: e.idleError } : {}),
      ...(typeof e.idleRunAt === "number" ? { idleRunAt: e.idleRunAt } : {}),
      ...(typeof e.idleSessionId === "string" ? { idleSessionId: e.idleSessionId } : {}),
      ...(typeof e.idleResult === "string" ? { idleResult: e.idleResult } : {}),
    }
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
    // 条数上限以**磁盘真值**判定（多实例并存时本进程镜像可能偏少）
    await this.persist(user, (disk) => {
      if (disk.length >= TODO_MAX_ITEMS) throw new Error(`待办条数已达上限（${TODO_MAX_ITEMS}），请先清理已完成项`)
      return disk.some((e) => e.id === entry.id) ? disk : [...disk, entry]
    })
    if (entry.idle) await this.ensureIdleTask(this.entryOf(user, entry.id) ?? entry).catch(() => {})
    return { ...(this.entryOf(user, entry.id) ?? entry) }
  }

  async update(user: string, id: string, patch: UserTodoUpdateInput): Promise<UserTodo | null> {
    const before = this.entryOf(user, id)
    if (!before) return null
    const updatedAt = this.now()
    // 补丁以**磁盘条目**为基准应用（本进程镜像可能陈旧；磁盘条目携带其他实例写入的字段）
    const applyPatch = (e: UserTodo): UserTodo => {
      const next = { ...e }
      if (typeof patch?.text === "string") next.text = this.normalizeText(patch.text)
      if (typeof patch?.done === "boolean") {
        next.done = patch.done
        // 取消勾选视为重新排队（若仍是闲时自动执行，下一轮空闲会再执行一次）
        if (!patch.done && next.idle) next.idleState = "pending"
      }
      if (typeof patch?.idle === "boolean" && patch.idle !== next.idle) {
        next.idle = patch.idle
        if (patch.idle) {
          // 开启（或重新开启）闲时自动执行：重置状态与计数，下一轮空闲重新执行
          next.idleState = "pending"
          next.idleAttempts = 0
          next.idleError = undefined
        } else {
          next.idleState = undefined
          next.idleError = undefined
        }
      }
      next.updatedAt = updatedAt
      return next
    }
    const patched = new Map<string, UserTodo>()
    await this.persist(user, (disk) =>
      disk.map((e) => {
        if (e.id !== id) return e
        const next = applyPatch(e)
        patched.set(e.id, next)
        return next
      }),
    )
    const result = this.entryOf(user, id) ?? patched.get(id)
    if (!result) return null
    await this.syncBinding(result, before).catch(() => {})
    return { ...(this.entryOf(user, id) ?? result) }
  }

  async remove(user: string, id: string): Promise<boolean> {
    const entry = this.entryOf(user, id)
    if (!entry) return false
    let removed = false
    await this.persist(user, (disk) => {
      const next = disk.filter((e) => e.id !== id)
      removed = next.length !== disk.length
      return next
    })
    // 绑定任务随待办删除（闲时任务失去待办即无存在意义）
    if (entry.idleTaskId) await this.tasks?.remove(user, entry.idleTaskId).catch(() => {})
    return removed
  }

  /** 拖动排序：按给定 id 顺序重排该用户清单（未列出的条目按原序追加在后，避免并发新增丢失）。 */
  async reorder(user: string, ids: string[]): Promise<UserTodo[]> {
    if (!Array.isArray(ids)) throw new Error("缺少顺序数组（ids）")
    const want = ids.map(String)
    await this.persist(user, (disk) => {
      const byId = new Map(disk.map((e) => [e.id, e]))
      const queue: UserTodo[] = []
      for (const id of want) {
        const e = byId.get(id)
        if (!e) continue
        byId.delete(e.id)
        queue.push(e)
      }
      // 未列出的条目（并发新增/其他实例写入）按原序追加在后，不丢
      for (const e of disk) if (byId.has(e.id)) queue.push(e)
      return queue
    })
    return this.list(user)
  }

  /** 手动执行待办：入队执行（统一队列、占额度、可置顶）；已绑定任务入队该任务，否则建一次性任务。 */
  async run(user: string, id: string, opts: { front?: boolean } = {}): Promise<UserTodoRunResult | null> {
    const entry = this.entryOf(user, id)
    if (!entry) return null
    const tasks = this.tasks
    if (!tasks) throw new Error("任务能力未启用（GEBAI_TASKS_ENABLED=false）")
    let taskId = entry.idleTaskId
    let ephemeral = false
    let bound: Task | null = null
    if (taskId) {
      bound = await tasks.get(user, taskId)
      if (!bound) taskId = undefined
      else if (!bound.enabled || bound.prompt !== entry.text || bound.timeoutMs !== TODO_IDLE_TIMEOUT_MS) {
        // 停用后的重新执行：恢复启用并同步文本/闲时超时缺省
        await tasks.update(user, taskId, { enabled: true, prompt: entry.text, timeoutMs: TODO_IDLE_TIMEOUT_MS })
      }
    }
    if (!taskId) {
      const created = await tasks.add(user, {
        kind: "manual",
        runner: "prompt",
        name: `待办：${headline(entry.text)}`,
        prompt: entry.text,
        target: "ephemeral",
        todoId: entry.id,
        ephemeral: true,
        runNow: false,
      })
      taskId = created.id
      ephemeral = true
    }
    const res = await tasks.run(user, taskId, { front: opts.front === true, source: "todo" })
    return {
      todo: { ...(this.entryOf(user, id) ?? entry) },
      queued: res?.queued === true,
      position: res?.position,
      reason: res?.reason,
      taskId,
      ephemeral,
    }
  }

  /** 任务运行结束回调（任务调度器 onFinished 注入）：回写待办状态、计次与结果摘要。 */
  async recordTaskResult(task: Task, run: TaskRunRecord): Promise<void> {
    const todoId = task.todoId
    if (!todoId) return
    const entry = this.entries.get(todoId)
    if (!entry || entry.user !== task.user) return
    const ok = run.status === "success"
    await this.persist(entry.user, (disk) =>
      disk.map((e) => {
        if (e.id !== todoId) return e
        const attempts = (e.idleAttempts ?? 0) + 1
        if (ok) {
          return {
            ...e,
            done: true,
            // 闲时执行状态仅闲时自动执行的待办携带（普通待办手动执行的完成态由 done/结果字段表达）
            ...(e.idle ? { idleState: "done" as const } : {}),
            idleError: undefined,
            idleResult: run.output?.slice(0, TODO_RESULT_MAX),
            idleAttempts: attempts,
            idleRunAt: run.endedAt,
            idleSessionId: run.sessionId,
            updatedAt: run.endedAt,
          }
        }
        const base = run.error ?? run.status
        // 闲时自动执行的待办：一次失败即关闭闲时运行（idle=false、idleState=failed、记因）——
        // 不自动重试；用户重新开启 ⚡ 即重置状态继续（普通待办手动执行只记因，不改开关）
        return {
          ...e,
          idleAttempts: attempts,
          idleRunAt: run.endedAt,
          updatedAt: run.endedAt,
          ...(e.idle
            ? { idle: false, idleState: "failed" as const, idleError: `${base}（重新开启 ⚡ 可继续，下次队列空闲再执行）` }
            : { idleError: base }),
        }
      }),
    )
    // 成功或失败：停用绑定任务（一次执行即终，不自动重跑；重新开启闲时或手动执行会自动恢复启用）。
    // 判定用**落盘后的最新状态**——`entry` 是 persist 之前的对象，计数与 idle 标记可能已被本次写入改动
    const fresh = this.entries.get(todoId)
    if (fresh?.idleTaskId && (ok || !fresh.idle)) {
      await this.tasks?.update(fresh.user, fresh.idleTaskId, { enabled: false }).catch(() => {})
    }
  }

  /** 待办变更后的绑定任务同步：开启闲时→确保任务存在；文本变更→同步提示词；勾选完成→停用。 */
  private async syncBinding(todo: UserTodo, before: UserTodo): Promise<void> {
    const tasks = this.tasks
    if (!tasks) return
    if (!todo.idle) {
      const orphan = todo.idleTaskId ?? before.idleTaskId
      if (orphan) {
        await tasks.remove(todo.user, orphan).catch(() => {})
        await this.persist(todo.user, (disk) => disk.map((e) => (e.id === todo.id ? { ...e, idleTaskId: undefined } : e)))
      }
      return
    }
    if (todo.done) {
      if (todo.idleTaskId) await tasks.update(todo.user, todo.idleTaskId, { enabled: false }).catch(() => {})
      return
    }
    await this.ensureIdleTask(todo)
  }

  /** 确保待办有可用的绑定闲时任务（惰性创建；文本/启用状态与待办同步），返回任务 id。 */
  private async ensureIdleTask(todo: UserTodo): Promise<string | undefined> {
    const tasks = this.tasks
    if (!tasks) return undefined
    if (!todo.idle || todo.done) return todo.idleTaskId
    if (todo.idleTaskId) {
      const existing = await tasks.get(todo.user, todo.idleTaskId)
      if (existing) {
        const patch: { prompt?: string; enabled?: boolean; timeoutMs?: number } = {}
        if (existing.prompt !== todo.text) patch.prompt = todo.text
        if (!existing.enabled) patch.enabled = true
        // 超时统一为待办闲时缺省（3 小时）：旧任务在重新开启时校正
        if (existing.timeoutMs !== TODO_IDLE_TIMEOUT_MS) patch.timeoutMs = TODO_IDLE_TIMEOUT_MS
        if (Object.keys(patch).length) await tasks.update(todo.user, existing.id, patch).catch(() => {})
        return existing.id
      }
    }
    const created = await tasks.add(todo.user, {
      kind: "idle",
      runner: "prompt",
      name: `待办：${headline(todo.text)}`,
      prompt: todo.text,
      target: "ephemeral",
      todoId: todo.id,
      timeoutMs: TODO_IDLE_TIMEOUT_MS,
      enabled: true,
    })
    await this.persist(todo.user, (disk) => disk.map((e) => (e.id === todo.id ? { ...e, idleTaskId: created.id } : e)))
    return created.id
  }

  /** 以**磁盘真值**为基准执行变更并落盘（跨进程写锁 + 原子写 + 滚动备份），随后用结果同步本地镜像。 */
  private async persist(user: string, mutate: (disk: UserTodo[]) => UserTodo[]): Promise<void> {
    const next = await mutateJsonList(this.userTodoFile(user), mutate, { normalize: (raw) => this.normalizeLoaded(raw) })
    this.syncUser(user, next)
  }

  /** 用落盘真值同步本地镜像：本用户条目按原有位置槽填充新顺序，其余用户条目相对序不变。 */
  private syncUser(user: string, list: UserTodo[]): void {
    const queue = [...list]
    const next = new Map<string, UserTodo>()
    for (const [key, value] of this.entries) {
      if (value.user !== user) {
        next.set(key, value)
        continue
      }
      const e = queue.shift()
      if (e) next.set(e.id, e)
    }
    for (const e of queue) next.set(e.id, e)
    this.entries = next
  }
}
