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

/**
 * 输出目标：人格发消息的「去处」清单（驱动层注入，模型据此选或直接不用感知）。
 *
 * ## 为什么需要它
 *
 * 靠模型从事件 refs 里抄 chat_id，只在「回来源那条消息」时成立；而人格大量行为是
 * 主动的（定时巡检报群里、告警推运维群、周报发部门群）——那些场景没有入站 refs，
 * 模型只能猜一个 id，猜错就被白名单拒绝。目标该由**部署配置**给定。
 *
 * ## 三个字段的语义
 *
 * - `channel:address` 是 act 的 `to`（如 `feishu:oc_xxx`、`local:outbox`），逐字可用
 * - `thread` 落进话题（飞书话题群 = 回复那条消息，自动继承 thread_id）
 * - `default` 表示「没有特别理由就用它」——模型不必每次自己选
 */
export interface OutputTarget {
  /** 渠道名（`feishu` / `local` / 二开渠道名）。 */
  channel: string
  /** 地址（群 chat_id / outbox / 工单项目…）。 */
  address: string
  /** 话题根消息 id（仅在话题群内需要）。 */
  thread?: string
  /** 给人看的标签（如「训练平台支持群」），进提示词。 */
  label?: string
  /** 默认目标（模型没理由时用它；同一声明里有多个 default 时取第一个）。 */
  default?: boolean
}

/** 输出目标的 `to` 串（可直接给 act）。 */
export function outputTargetTo(t: OutputTarget): string {
  return `${t.channel}:${t.address}`
}

/** 人格（角色）身份：**一个角色下可以有多个 driver**。 */
export interface PersonaIdentity {
  id: string
  /** 显示名（会话名前缀、审批卡抬头）。 */
  name: string
  /** 身份与角色（原样进系统提示词，本包不做任何改写）。 */
  identity: string
  role: string
  /**
   * 扮演角色的**实际用户身份**（可选；值支持 `${VAR}` 模板，由部署环境提供）。
   *
   * 为何单列而不是“写进 identity 文本里就行了”：
   * 1. 审批卡与出站审核要说“以谁的名义发”——那是结构化事实，不能靠正则从提示词里抓。
   * 2. 模型需要知道“我代表的是组织里哪个真人”，否则它会把自己当机器人。
   */
  actor?: PersonaActor
}

/**
 * 扮演角色的**实际用户身份**：一组**自由键值**（`fields`），不是几个固定字段。
 *
 * ## 为什么是自由键值而不是 name/staffId/account 三件套
 *
 * 那三个字段只是**示例**。真实部署里“这个人是谁”包含什么完全取决于组织：
 * 姓名/花名/工号/账号/open_id/部门/团队/邮箱/职位/工位/成本中心/入司时间…
 * 写死三个字段的后果是：第四件事只能塞进一个 `extra` 子对象，于是同一个概念
 * 有两种写法（`actor.name` 与 `actor.extra.姓名`），而模型与审批卡都得学两套。
 *
 * 所以这里只留**一个**容器：键由部署方自己定，值支持 `${VAR}` 模板。
 * 框架不解释键的含义（不做“name 优先于花名”这种猜测）——那是部署方的领域知识。
 *
 * ## 取值的环境变量风格
 *
 * 两种都支持，因为不同部署的心智不同：
 *
 * ```ts
 * // ① 显式模板（推荐：一眼看出读哪个变量，且能做拼接）
 * fields: { 姓名: "${ACTOR_NAME}", 工号: "${ACTOR_STAFF}", 邮箱: "${ACTOR_NAME}@corp.com" }
 *
 * // ② 整组注入（一个 JSON 变量搞定所有身份字段，适合按部署下发）
 * fields: actorFieldsFromEnv("GEBAI_ACTOR")
 * ```
 *
 * ② 的写法要求变量在 `envVars` 里声明为 `GEBAI_ACTOR`（值为 JSON 对象）。
 */
export interface PersonaActor {
  /**
   * 身份字段（键自由：姓名 / 工号 / 部门 / 邮箱 / 花名…）。
   *
   * 值支持 `${VAR}` 模板；变量必须在该人格 `envVars` 里声明（否则加载期报错）。
   */
  fields: Record<string, string>
}

/**
 * driver 的**定时工作面**：一条声明 = 一个到点就跑的任务。
 *
 * ## 为什么定时长在 driver 上
 *
 * driver 的粒度是**专注的任务类别**，它的入口本来就是 `subscriptions`。定时与订阅
 * 是同一件事的两种触发方式（“我被消息叫醒” vs “我到点自己开工”），所以放在同一层。
 * 放人格级会迫使一个角色的多个分身共用一张时刻表——而“早报在报告分身跑、巡检在运维
 * 分身跑”才是真实分工。
 *
 * ## 与统一任务管理（`task` 子Agent）的区别
 *
 * | | 统一任务（`tasks.json`） | 这里的定时声明 |
 * |---|---|---|
 * | 归属 | 用户级资源，跨会话可见可管 | 人格定义的一部分（部署配置，随代码走） |
 * | 触发后 | 起一次独立会话（或直跑脚本） | 唤醒**这个分身**的常驻会话（带人格身份与出站纪律） |
 * | 用途 | 用户排的活（跑脚本、批量任务） | 角色自己的工作节律（早报、巡检、周复盘） |
 *
 * 两者不互相替代：前者是“人给系统派的活”，后者是“这个人的作息”。
 */
export interface DriverSchedule {
  /** 声明 id（分身内唯一，`[a-z0-9_]+`）：日志/幂等键/面板都用它。 */
  id: string
  /** 执行表达式：5 段 cron，或 `@hourly`/`@daily`/`@weekly`/`@monthly`、`@every 30m`、`@at 2026-09-01T09:00`（一次性）。 */
  cron: string
  /** 到点后的指令（对模型说的话，原样进事件正文）。 */
  prompt: string
  /** 给人看的名字（进事件的“来源”与面板；缺省用 id）。 */
  description?: string
  /** IANA 时区（缺省服务器本地），如 `Asia/Shanghai`。 */
  timezone?: string
  /**
   * 首次见到这条声明时是否补跑一次（缺省 `skip`）。
   *
   * `skip`：游标从此刻起算（加一条 `0 9 * * *` 不该在下午就被立刻触发一次）。
   * `run`：启动即触发一次（进程刚部署、或调过表达式时想要一次样本）。
   */
  misfire?: "skip" | "run"
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
   * 该分身**发消息的去处**（驱动层注入；缺省不注入，模型只能回事件来源）。
   *
   * 为何放在 driver 上而不是人格上：同一个人的两个分身往往面向不同受众
   * （盯告警的发运维群、答支持的发支持群）。放人格级会迫使它们共用一套目标，
   * 结果是「告警发到支持群」这类事故。人格级声明只是分身的默认值。
   */
  outputs?: OutputTarget[]
  /**
   * 该 driver 的并发额度（同时最多跑几个会话）。缺省继承 `PersonaLookup.maxConcurrent()`。
   *
   * 实际作用面取决于会话路由：声明了 `sessionKeyOf`（按事务分割）的 driver 可能同时
   * 有**多个**上下文，此时额度就是真上限；不分上下文的 driver 只有一个会话，
   * 会话级串行先把它锁死，额度等于不起作用。
   */
  maxConcurrent?: number
  /**
   * 会话映射规则表（可选）：决定「这件事归哪个上下文」。
   *
   * ## 语义（按顺序匹配，第一条命中即用）
   *
   * 不声明 = 该分身一个常驻上下文（**缺省，也是推荐**）：所有事件（含不同群的消息）
   * 都注入同一会话。因为会话面向的是**上下文**，不是群。
   *
   * 声明了就按规则表分（规则语法与占位符见 `session-map.ts`）：
   *
   * ```ts
   * sessionMap: [
   *   { when: "feishu:message", key: "{chat}__t_{thread}" },  // 话题群：每话题一会话
   *   { when: "message",        key: "{chat}" },              // 其余群：每群一会话
   * ]
   * ```
   */
  sessionMap?: import("./session-map").SessionMapRule[]
  /**
   * 会话路由函数（可选，**优先级最高**，比 `sessionMap` 更直接）。
   *
   * 何时用函数而不是规则表：键要经过代码才能算出来（查表、拼哈希、调本地索引）。
   * 声明它就等于声明「按什么分割上下文」，应当基于**事务**而不是渠道。
   *
   * 返回空串 = 此事不属于任何特殊事务 → 回到该分身的常驻会话。
   */
  sessionKeyOf?: (event: import("./event").PersonaEvent) => string
  /**
   * 该分身的定时工作面（可选）：声明几条「到点就跑」的任务。
   *
   * 声明了它就必须让 `subscriptions` 能匹配 `schedule` 事件（加载期强制校验）——
   * 否则到点的事件找不到这个分身，表现是「配了定时但永不触发」且零报错。
   */
  schedules?: DriverSchedule[]
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
  /**
   * 这个人格声明的子代理工具包（`PersonaDef.agents` 透传）。
   *
   * 为何要传到 runner 而不只交给会话层：**提示词得跟着变**。
   * 声明了工具包的人格，提示词里要多一段「何时查、何时写」——
   * 光把工具放进表里，模型面对一排工具常常想不起来用。
   * 两半（工具面 + 提示词）必须由同一个来源驱动，否则会出现
   * 「没装工具却提示它有」与「装了工具但不告诉它」两种错配。
   */
  agents?: string[]
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
  /**
   * 本轮事件归属的会话键（已解析；空串 = 默认单会话）。
   *
   * 为何要给 runner：提示词需要告知模型「你现在在哪个群/哪个话题里」——
   * 否则同一个分身在不同群里的答复会互相串味，而模型无法从事件本身区分。
   */
  sessionKey?: string
  /** 本轮序号（1 起；>1 说明被插话续上了）。 */
  round: number
  sessionId: string
  /** 取消信号（角色停止 / 会话被终止时触发）。 */
  signal: AbortSignal
  /** 插话信箱：可在任意时刻 drain 提前取走新事件（不 drain 也不会丢）。 */
  mailbox: import("./mailbox").PersonaMailbox
  attachments?: PersonaAttachment[]
  /**
   * 同人格的其它 driver（不含自己）。
   *
   * 为何要传给 runner：不告诉它有哪些兄弟分身时，同一个问题会被几个分身重复答一遍——
   * 每个都在自己的会话里把它当「第一次看见」。这是“多分身”模型特有的失效模式。
   */
  siblings?: DriverDef[]
}

/**
 * 会话解析器：按「角色 + driver」取会话。
 *
 * 会话与 driver 一一对应（一个 driver 一个常驻会话），不是"一个角色一个会话"。
 * 实现必须幂等：同一 (personaId, driverId) 多次调用返回同一 sessionId（重启后亦然）。
 */
export interface SessionResolver {
  resolveDriver(input: { personaId: string; driverId: string; user: string; event: PersonaEvent; sessionKey?: string }): Promise<ResolvedSession>
  /**
   * 只把一条消息**写进会话上下文**，不启动任何执行。
   *
   * ## 为何需要这个能力
   *
   * 「进上下文」与「触发执行」原本是同一件事：事件投给 runner 就必然跑一轮。
   * 但群消息里有一类——人格自己代发的回灌——必须记下（它得知道群里现在有自己
   * 这段话），却绝不能拿它再跑一轮（会把自己刚说的话当成新指令重答）。
   *
   * 所以写入需要一条**绕过调度器与引擎**的路：直接落会话记录。
   * 放在 SessionResolver 而不是 Runner，正是因为 Runner 的语义就是「跑一轮」，
   * 而且引擎有 `tasks.has(sessionId)` 互斥——会话正在跑时塞进去只会排队等下一轮，
   * 还是会被执行。
   *
   * 可选：未实现时调用方退化为「不记」（即旧行为）并记日志。
   */
  appendOnly?(input: {
    personaId: string
    driverId: string
    user: string
    event: PersonaEvent
    sessionKey?: string
    /** 写进消息正文的额外说明（身份注记；缺省用事件 body）。 */
    note?: string
    /** 发送者类别（落盘标记用：面板能区分“它看过没答”与“它没收到”）。 */
    kind?: "self" | "owner" | "other"
    reason?: string
  }): Promise<{ sessionId: string; created: boolean }>
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
   * 每个 driver 各自受限（多会话路由下一个 driver 可有多个并发会话）。
   * 要做角色总额限制得靠宿主侧或新增跨 driver 闸门。
   */
  maxConcurrent(personaId: string): number
  /**
   * 该人格声明的子代理工具包（`PersonaDef.agents`；无声明时返回空数组）。
   *
   * ## 为何单独一个方法，而不是塞进 `get()`
   *
   * `get()` 刻意只返回身份三件套（`id/name/identity/role`），它进的是**系统提示词**，
   * 调用方不该在那里看到 drivers / description 这类内部字段。把 agents 塞进 `get()`
   * 会让那条边界模糊。而 agents 确实需要流到两个地方（会话层装工具、提示词写纪律），
   * 所以给它一条**显式的窄接口**：要的人主动取，不想知道的人看不见。
   *
   * 可选：未实现的旧 `PersonaLookup` 实现可缺省它（返回 undefined = 无工具包）。
   */
  agents?(personaId: string): string[]
  /**
   * 该人格的**会话归属用户**（决定会话建在谁的目录下）。
   *
   * 为何要从 lookup 取而不是写死 `"admin"`：人格定义里可以用 `user` 声明
   * “这个角色由谁扮演”（工号/账号）——它是一个**部署事实**，不能全靠一个常量。
   * 缺省实现返回 `"admin"`（本地形态的默认用户）。
   */
  userOf?(personaId: string): string
}

/** 日志接口（避免本包依赖具体日志实现）。 */
export interface PersonaLogger {
  info(msg: string): void
  warn(msg: string): void
}
