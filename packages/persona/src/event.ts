/**
 * 拟人运行模式的事件模型（本包的地基）。
 *
 * 设计前提（见 DESIGN「拟人运行模式」）：
 * - **会话是工作单元，不是对话**。渠道不参与会话身份——入站只提供上下文，出站靠显式工具调用。
 * - **事件是唯一驱动力**。定时任务、渠道消息、webhook、桌面操作一律收敛成 `PersonaEvent`，
 *   由 `PersonaRuntime` 按「收集 → 分析 → 输出」放手给引擎处理。
 * - **事件广播给关注者**（不是定向投递）：一条事件会被**所有订阅了该类型的人格**各自处理一遍，
 *   各自会话、各自队列。归属不再是"找唯一人格"。
 * - **幂等键必填**。同一事件对**同一人格**投两次不得产生重复副作用（不同人格各算一次）。
 */

/** 事件来源类别：决定默认路由与出站纪律提示文案。 */
export type PersonaEventKind = "schedule" | "message" | "webhook" | "desktop" | "idle"

/**
 * 事件引用（入站上下文的一部分，**不是路由键**）。
 *
 * 为何单列：模型需要知道「这是谁说的、回到哪」才可能正确出站；而路由（进哪个会话）
 * 由 `threadKey` 决定。两者混在一起正是旧实现把 chat_id 当会话身份的病根。
 */
export interface PersonaEventRefs {
  /** 渠道群/会话 id（如飞书 chat_id）。 */
  chatId?: string
  /** 原消息 id（出站时可用于引用回复）。 */
  messageId?: string
  /** 发送者（渠道侧 id，如飞书 open_id）。 */
  senderId?: string
  /** 发送者显示名。 */
  senderName?: string
  /** 关联的调度任务 id（kind=schedule 时）。 */
  taskId?: string
  /** 其它渠道自定义引用（原样透传给模型，不参与逻辑）。 */
  extra?: Record<string, string>
}

/** 入站上下文：注入系统提示词的「这是什么、谁说的、怎么回」。 */
export interface PersonaEventContext {
  /** 人类可读来源，如「飞书群「训练平台支持群」」。 */
  source: string
  /** 结构化引用。 */
  refs?: PersonaEventRefs
  /** 一句话摘要：发生了什么。 */
  summary: string
}

/** 附件（图片等）：与引擎 `AttachmentInput` 同形，本包不依赖引擎类型。 */
export interface PersonaAttachment {
  name: string
  mime?: string
  path?: string
  data?: Uint8Array
}

/**
 * 统一事件：所有驱动源的唯一入口形态。
 *
 * 字段纪律：
 * - `id` 是幂等键，**必填且稳定**（飞书消息 id / `task:{taskId}@{触发时刻}` / webhook 投递 id）。
 *   幂等判定按「id × personaId」记账——同一事件对不同人格是不同的一次处理。
 * - `personaId` **留空 = 广播给所有关注者**（常规形态，插件应该留空）；
 *   非空 = 定向投给该人格（插件明确知道归属时，且仍需该人格已订阅）。
 * - `body` 是主体内容（消息正文 / 任务指令）；`context` 是元信息，不进 `body`。
 */
export interface PersonaEvent {
  /** 幂等键（必填）。同 id 对同一人格的第二次投递必须被丢弃。 */
  id: string
  kind: PersonaEventKind
  /**
   * 定向人格（**空串 = 广播**）。
   *
   * 为何保留此字段而不是删除：个别场景插件确实知道"只该它处理"（如人格私有任务）；
   * 但默认应为空——归属是部署配置（订阅）的事，不是源的事。
   */
  personaId: string
  /**
   * 定向 driver（空 = 该角色下**所有关注该事件的 driver** 都处理）。
   *
   * 用途：worker 回报必须回到**派它的那个 driver**，而不是同角色的其它分身。
   */
  driverId?: string
  /** 渠道标识（`feishu` / `schedule` / `webhook` / `desktop` / `idle`）。 */
  channel: string
  /**
   * 线程键：保留作展示与分组（不再决定会话——会话由 driver 决定）。
   */
  threadKey?: string
  context: PersonaEventContext
  /** 主体内容（模型的输入）。 */
  body: string
  attachments?: PersonaAttachment[]
  /** 事件产生时刻（ms）。用于排队可观测与幂等窗口。 */
  receivedAt: number
}

/** 事件投递结果：调用方据此决定回什么话给用户。 */
export type PersonaDispatchStatus =
  /** 已接管（至少一个订阅者接管；逐人格结果见 `deliveries`）。 */
  | "accepted"
  /** 同 id 已被**全部订阅者**处理过（幂等丢弃）。 */
  | "duplicate"
  /** 全部订阅者都溢出（队列满）。 */
  | "overflow"
  /** **无人关注**本类事件 → 调用方走自己的旧路径。 */
  | "unrouted"
  /** 人格已停止/未启动，或全部订阅者不可用。 */
  | "unavailable"

export interface PersonaDispatchResult {
  status: PersonaDispatchStatus
  /** 首个成功接管的角色（兼容字段；多 driver 详情看 `deliveries`）。 */
  personaId?: string
  /** 首个成功接管的 driver。 */
  driverId?: string
  sessionId?: string
  /** 入队后前面还有多少件（仅首个接管者有值）。 */
  queued?: number
  /** 丢弃理由（可直接回给用户）。 */
  reason?: string
  /**
   * 逐 driver 的投递结果（**多 driver 模型的核心字段**）。
   *
   * 为何带 driverId：同一角色下的多个 driver 可能都关注该类事件，
   * 它们各自处理——这是正常形态，不是重复。
   */
  deliveries?: Array<{ personaId: string; driverId: string; status: string; sessionId?: string; queued?: number; reason?: string }>
}

/**
 * 线程键推导：把「从哪来」收敛成「属于哪段连续工作」。
 *
 * 默认规则：`{channel}:{chatId}`（同群连续对话）或 `{channel}`（无 chatId 的源）。
 * **刻意不含 messageId**——同一段对话里的每条消息必须落同一线程，否则上下文断裂。
 */
export function deriveThreadKey(e: Pick<PersonaEvent, "channel" | "kind" | "context" | "threadKey">): string {
  if (e.threadKey && e.threadKey.trim()) return e.threadKey.trim()
  // 定时任务与自省默认进 anchor（长期记忆会话），不按时间分线程
  if (e.kind === "schedule" || e.kind === "idle") return "anchor"
  const chatId = e.context.refs?.chatId
  return chatId ? `${e.channel}:${chatId}` : e.channel
}
