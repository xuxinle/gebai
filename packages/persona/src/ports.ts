/**
 * Port（端口）接口：事件驱动核心与外部世界的唯一接缝。
 *
 * 分层理由（与仓库既有纪律一致，见 AGENTS「接口优先 / 依赖注入」）：
 * - 本包**不依赖 `@gebai/server`**，事件驱动的全部逻辑可脱离引擎单测；
 * - 服务端通过实现这些接口接入，且服务端不得反向让本包认识 `AgentEngine`
 *   （那会让「拟人是运行模式」退化成「引擎的一个分支」）。
 *
 * 权力归属（DESIGN「能力外置、权力内守」）：本包只做**调度与路由**——
 * 决定「这事件归哪个 driver、什么时候跑」。审批、沙箱、工具可见性一律留在引擎。
 */

import type { PersonaAttachment, PersonaEvent } from "./event"

/** 人格（角色）身份：**一个角色下可以有多个 driver**。 */
export interface PersonaIdentity {
  id: string
  /** 显示名（会话名前缀、审批卡抬头）。 */
  name: string
  /** 身份与角色（原样进系统提示词，本包不做任何改写）。 */
  identity: string
  role: string
}

/**
 * driver（驱动会话）定义：角色的一个**专注面**。
 *
 * 为什么一个角色要有多个 driver：
 * 一个人不会把所有事混在一个脑子里想。他会有"我盯告警"和"我做支持答疑"两个工作状态，
 * 各自带着不同的关注点与上下文。把它们塞进同一个会话，结果是：
 * - 上下文互相污染（告警的排查细节挤掉了支持问答的上下文）
 * - 一个长时间任务卡住，另一类事件完全无法响应
 *
 * 所以 driver 的粒度 = **专注的任务类别**，而不是"一个人"。
 * 同一个角色可以有：`alerts`（盯告警）、`support`（答支持群）、`report`（写周报）。
 */
export interface DriverDef {
  /**
   * driver id（角色内唯一，`[a-z0-9_]+`）。
   *
   * 用于日志、状态展示、幂等记账（`事件 × driver`）与会话命名。
   */
  id: string
  /** 这个 driver 负责什么（展示与提示词用，一句话）。 */
  focus: string
  /**
   * 关注的事件类型（订阅）。
   *
   * 同一事件**可以同时命中同一角色的多个 driver**（如"周报 driver"与"告警 driver"
   * 都关注定时任务）——各自独立处理，不是二选一。
   */
  subscriptions: string[]
  /**
   * 该 driver 的并发额度（同时最多跑几个轮次），缺省继承 `PersonaLookup.maxConcurrent()`。
   *
   * 实际作用面：一个 driver 只对应一个常驻会话，会话级串行先把它锁死——
   * 所以大于 1 在当前路由下不会生效。真正的并发来自「同时有事的 driver 个数」。
   */
  maxConcurrent?: number
}

/** 会话解析结果。 */
export interface ResolvedSession {
  /** 会话 id（32 位 hex）。 */
  sessionId: string
  /** 是否本次新建（新建时需给名字，避免前端自动命名覆盖）。 */
  created: boolean
  /** 会话名。 */
  name: string
}

/** 一次运行的目标：角色 + 它下面的某个 driver。 */
export interface DriverTarget {
  personaId: string
  /** 该 driver 的定义（含 focus / 额度）。 */
  driver: DriverDef
  /** 角色身份（提示词注入用）。 */
  identity: PersonaIdentity
  /** 会话归属用户。 */
  user: string
}

/**
 * 事件运行器：把一个**轮次**交给引擎跑完。
 *
 * 返回 Promise 在该轮结束后 resolve。调度器把同一运行的多轮串起来：
 * 轮末若信箱非空则再调一次 `run`（引擎视角是两个独立 run，不违反同会话串行约束）。
 */
export interface PersonaRunner {
  /** 该会话当前是否有任务在跑（引擎既有能力）。 */
  isRunning(sessionId: string): boolean
  /** 跑一轮（可能含多个事件：插话在轮次边界被合并）。 */
  run(input: PersonaRunInput): Promise<void>
}

export interface PersonaRunInput extends DriverTarget {
  /**
   * 本轮要处理的事件（按到达顺序）。
   *
   * 为何是数组：运行中插入的事件在轮次边界被合并进本轮。
   * 单事件时长度 = 1（与无插入机制时行为一致）。
   */
  events: PersonaEvent[]
  /** 首个事件（便利字段）。 */
  event: PersonaEvent
  /** 本轮序号（1 起；>1 说明被插话续上了）。 */
  round: number
  sessionId: string
  /** 取消信号（角色停止 / 会话被终止时触发）。 */
  signal: AbortSignal
  /** 插话信箱：可在任意时刻 drain 提前取走新事件（不 drain 也不会丢）。 */
  mailbox: import("./mailbox").PersonaMailbox
  attachments?: PersonaAttachment[]
}

/**
 * 会话解析器：按「角色 + driver」取会话。
 *
 * 会话与 driver 一一对应（一个 driver 一个常驻会话），不是"一个角色一个会话"。
 * 实现必须幂等：同一 (personaId, driverId) 多次调用返回同一 sessionId（重启后亦然）。
 */
export interface SessionResolver {
  resolveDriver(input: { personaId: string; driverId: string; user: string; event: PersonaEvent }): Promise<ResolvedSession>
}

/** 单条投递结果（广播后每个命中的 driver 各一份）。 */
export type PersonaDeliveryStatus =
  /** 已接管（可能已开跑或已排队）。 */
  | "accepted"
  /** 该 driver 已处理过同 id 事件（幂等丢弃）。 */
  | "duplicate"
  /** 该 driver 排队溢出（队列满）。 */
  | "overflow"
  /** 该 driver 不可用（运行时已停 / 会话解析失败 / driver 已下线）。 */
  | "unavailable"

export interface PersonaDelivery {
  personaId: string
  /** 命中的 driver（多 driver 模型的必备字段：同一角色可能有多条投递）。 */
  driverId: string
  status: PersonaDeliveryStatus
  sessionId?: string
  /** 入队后前面还有多少件。 */
  queued?: number
  reason?: string
}

/**
 * 角色注册表（只读视图）。
 *
 * **两级订阅模型**：
 * 1. 角色层：`subscribers()` 找出关注该事件的角色
 * 2. driver 层：每个角色内再按 `drivers[].subscriptions` 筛出关注的 driver
 *
 * 第二级是本轮新增的：一个角色收到事件后**不是全部分身都处理**，
 * 只有关注该类型的 driver 才动——否则"盯告警"的分身会被支持群的消息淹没。
 */
export interface PersonaLookup {
  /** 按 id 取角色（不存在返回 null）。 */
  get(personaId: string): PersonaIdentity | null
  /** 该角色下的全部 driver（无则空数组）。 */
  drivers(personaId: string): DriverDef[]
  /**
   * 找出**关注了该事件**的全部角色，以及各自命中的 driver。
   *
   * 返回空数组 = 无人关注 → `dispatch` 返回 `unrouted`（调用方走自己的旧路径）。
   */
  subscribers(
    event: Pick<PersonaEvent, "kind" | "channel" | "context">,
  ): Array<{ personaId: string; user: string; driverIds: string[] }>
  /**
   * 角色级并发额度的默认值（driver 未单独配置时继承）。
   *
   * 注意语义：这**不是**「该角色同时运行会话数」的上限——调度器是 per-driver 的，
   * 每个 driver 各自受限，真实并发度 = 同时有事的 driver 个数。要做角色总额限制
   * 得靠宿主侧或新增跨 driver 闸门。
   */
  maxConcurrent(personaId: string): number
}

/** 日志接口（避免本包依赖具体日志实现）。 */
export interface PersonaLogger {
  info(msg: string): void
  warn(msg: string): void
}
