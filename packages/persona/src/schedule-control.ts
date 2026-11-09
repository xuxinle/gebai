/**
 * 人格定时任务的**启停控制面**：让使用者在界面上关掉某条定时，而不必改代码里的 cron。
 *
 * ## 为什么要有它
 *
 * `drivers[].schedules` 是**代码里的声明**（`cron` + `prompt`）。部署之后想「今天先别跑这条」
 * 只有两条路：改代码重启，或者忍着。而定时任务恰恰是最需要临时关停的东西——
 * 它无人值守地对外发消息，一旦发现它在错发，第一反应必须是「先停」。
 *
 * 所以这一层只做一件事：**声明照旧来自代码，是否生效由使用者的开关决定**。
 * 代码是「它应该做什么」，控制面是「现在允不允许它做」——两者分开，
 * 既不用为了关一条去改源码，也不会因为关了就把声明丢掉（重新打开时一切照旧）。
 *
 * ## 为什么落在读取器出口，而不是源或调度器里
 *
 * `schedules.ts` 的 `collectDue` 是纯函数：**声明交给它就会触发，不交给它就是停用**。
 * 而宿主注入的 `personaSchedules()` 是唯一的声明读取面（每 tick 现取）。
 * 在那里过滤，等于用最小改动拿到完整效果：
 *
 * - 不碰 cron 解析（`nextSlot` 的语义一条都不用改）
 * - 不碰源的 tick 循环与游标
 * - 不碰调度器和事件投递
 * - 天然热加载（读取器本来每 tick 现取）
 *
 * 反过来说，如果在源里加 `if (disabled) continue`，就必须把控制面塞进源的闭包状态，
 * 而源的状态是「实例私有、多实例不串扰」的——为了让界面能改它，反而要把它暴露出来，
 * 比在出口过滤复杂得多且更容易出现「界面显示已关，实际还在跑」。
 *
 * ## 三条语义（都遵循「不静默改变既有行为」）
 *
 * 1. **缺省启用**：文件里没有记录 = 启用。上线这个功能不该把任何正在跑的定时停掉。
 * 2. **开关持久**：写盘，重启后仍然生效（临时关停如果一重启就复活，等于没关）。
 * 3. **坏文件不影响运行**：解析失败按「全部缺省」处理并保留原文件（下次写入才覆盖），
 *    不能让一个手抖的 JSON 把整套定时任务全停掉。
 *
 * ## key 的形态
 *
 * `{personaId}/{driverId}/{scheduleId}`，与 `schedules.ts` 的 `scheduleKey()` **逐字一致**。
 * 复用同一个函数而不是重新拼一遍：两个地方各拼一次，迟早会出现「界面关的是这条、
 * 实际生效的是另一条」——这类错配排查起来极难（两边看起来都对）。
 */

import { scheduleKey, type ScheduleDeclaration } from "./schedules"

/** 一条开关记录。 */
export interface ScheduleControlEntry {
  /** 是否启用。缺省（无记录）视为启用。 */
  enabled: boolean
  /** 最后一次改动时刻（排查「谁在什么时候停的」）。 */
  updatedAt: number
  /** 最近一次手动「立即运行」的时刻（界面展示用；与 cron 触发的统计分开）。 */
  lastRunAt?: number
}

/** 落盘形态：key → 记录。 */
export type ScheduleControlState = Record<string, ScheduleControlEntry>

/**
 * 持久化接口（宿主注入）。
 *
 * 为什么不在这里直接读文件：`packages/persona` 是**纯逻辑层**（可以看到它其它模块
 * 连 `node:fs` 都不 import），落盘位置属于宿主知识（`{GEBAI_HOME}/data/persona/` 是
 * 服务端的约定）。把它抽成接口，测试可以用内存实现，宿主换存储也不影响语义。
 */
export interface ScheduleControlPersistence {
  load(): Promise<ScheduleControlState>
  save(state: ScheduleControlState): Promise<void>
}

/** 内存实现（测试 / 无持久化场景：开关只在本次进程内有效）。 */
export class MemoryScheduleControlPersistence implements ScheduleControlPersistence {
  private state: ScheduleControlState
  constructor(initial: ScheduleControlState = {}) {
    this.state = structuredClone(initial)
  }
  async load(): Promise<ScheduleControlState> {
    return structuredClone(this.state)
  }
  async save(state: ScheduleControlState): Promise<void> {
    this.state = structuredClone(state)
  }
}

/** 操作结果（路由直接拿去回包，避免每个调用点各判一次）。 */
export interface ScheduleControlResult {
  ok: boolean
  key: string
  enabled?: boolean
  error?: string
}

/**
 * 定时任务启停控制面。
 *
 * 读侧（`isEnabled` / `filterEnabled`）是**同步**的：它跑在源每 20s 一次的 tick 里，
 * 每次都 await 一次磁盘 IO 没必要。构造时 `load()` 一次，之后写操作保持内存与磁盘同步。
 */
export class ScheduleControl {
  /** key → 记录（内存镜像，读侧直接查）。 */
  private state: ScheduleControlState = {}
  /** 写串行化：并发点两个开关时整文件覆写会丢记录（与 drivers.json 同一个坑）。 */
  private chain: Promise<unknown> = Promise.resolve()
  private loaded = false

  constructor(private readonly persistence: ScheduleControlPersistence) {}

  /** 载入落盘状态。宿主应在装配期调用一次；重复调用是幂等的。 */
  async load(): Promise<void> {
    if (this.loaded) return
    this.state = await this.persistence.load().catch(() => ({}))
    this.loaded = true
  }

  private serialize<T>(fn: () => Promise<T>): Promise<T> {
    const next = this.chain.then(fn, fn)
    this.chain = next.then(
      () => undefined,
      () => undefined,
    )
    return next
  }

  /** 某条声明当前是否启用（无记录 = 启用，见文件头语义 1）。 */
  isEnabled(key: string): boolean {
    const entry = this.state[key]
    return entry ? entry.enabled !== false : true
  }

  /**
   * 过滤掉被停用的声明（读取器出口调用）。
   *
   * 返回**新数组**而不是原地改：调用方拿到的是共享的声明视图，
   * 原地过滤会让上游看到「声明消失了」（热加载 diff 会误报 driver 少了定时）。
   */
  filterEnabled(declarations: ReadonlyArray<ScheduleDeclaration>): ScheduleDeclaration[] {
    return declarations.filter((d) => this.isEnabled(scheduleKey(d)))
  }

  /** 全部开关记录（面板展示用；未记录的声明不在其中，按启用处理）。 */
  snapshot(): ScheduleControlState {
    return structuredClone(this.state)
  }

  /** 改开关（持久）。 */
  async setEnabled(key: string, enabled: boolean): Promise<ScheduleControlResult> {
    if (!key.trim()) return { ok: false, key, error: "key 不能为空" }
    return this.serialize(async () => {
      const prev = this.state[key]
      this.state[key] = { ...(prev ?? {}), enabled, updatedAt: Date.now() }
      await this.persist()
      return { ok: true, key, enabled }
    })
  }

  /**
   * 记一次手动运行时刻。
   *
   * 只记时间戳，不写 enabled——**手动运行不改变启用状态**：
   * 「我临时跑一次看看」与「以后都让它跑」是两件事，混在一起会让人不敢点。
   * 同理，停用状态下也允许手动跑（见 `resolveAction` 的 run 分支）。
   */
  async markRun(key: string): Promise<void> {
    await this.serialize(async () => {
      const prev = this.state[key]
      this.state[key] = { ...(prev ?? { enabled: true }), lastRunAt: Date.now() }
      await this.persist()
    })
  }

  private async persist(): Promise<void> {
    // 坏文件不阻断：写失败只影响「下次重启记不住」，不该让接口报错、更不该让运行中的定时停摆
    await this.persistence.save(this.state).catch(() => undefined)
  }
}

/** 控制面上支持的动作。 */
export type ScheduleAction = "enable" | "disable" | "run"

/** 解析动作字符串（路由用；未知动作如实报错而不是静默当成某个默认值）。 */
export function parseScheduleAction(raw: string): ScheduleAction | null {
  return raw === "enable" || raw === "disable" || raw === "run" ? raw : null
}

/**
 * 手动运行要造的事件 id。
 *
 * 与 `schedules.ts` 的 `scheduleEventId` 同前缀同结构，但 slot 用**当前毫秒**：
 * - 复用那个函数的 slot 语义会与真实档期撞车（同一条声明的同一档期本来就该只跑一次，
 *   手动再跑一次理应是新事件；否则会被幂等去重吃掉，用户点了没反应）
 * - 毫秒粒度下连点两次仍可能同 id（同一毫秒），这是可接受的：那本来就是重复点击
 */
export function manualRunEventId(personaId: string, driverId: string, scheduleId: string, at: number): string {
  return `task:persona:${personaId}/${driverId}/${scheduleId}@manual:${at}`
}
