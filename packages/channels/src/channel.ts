/**
 * 渠道契约：把「对外动作」从「具体渠道」里剥出来。
 *
 * ## 为什么需要这层
 *
 * 按渠道列工具（`send_message` / `create_issue` / `write_memory`…）的分化方式泛化不下去：
 * 每加一个渠道就要加工具、改提示词、影响全部会话；而「发消息」「提工单」「写记忆」在
 * 性质上完全同构（有外部副作用、需审批、需留痕、0~N 次），只是目标与动词不同。
 *
 * 所以这里把一次对外动作拆成三个正交量：
 *
 * ```
 *                      ┌─ 目标 target：`channel:address`（谁）
 *   一次对外动作 = act ─┼─ 动词 verb：渠道声明的封闭集合（做什么）
 *                      └─ 载荷 args：由动词 schema 约束（什么内容）
 * ```
 *
 * 渠道只回答「**怎么把它做掉**」；「**能不能做**」由引擎侧的工具声明裁决
 * （审批、白名单、留痕）——渠道插件拿不到绕过治理的能力。
 *
 * ## 三条硬约束
 *
 * 1. **verb 是封闭集合且必须声明 schema**：模型据 `describe` 就知道怎么调，不需要猜；
 *    未声明的 verb 一律拒绝（不静默降级，那是「配了但没反应」的来源）。
 * 2. **target 白名单 fail-closed**：渠道给 `targets()`，越界**直接拒绝**而不是转审批。
 *    发错群是信息泄露，不是「让用户决定」的事。
 * 3. **幂等由调用方给**：渠道实现可能被重试（网络重试、上层重投），副作用必须可去重。
 *    `act` 的 `idempotencyKey` 原样透传，渠道按需用。
 */

/** 参数声明（精简 JSON Schema 子集：渠道只需描述意图，不需要表达力完备）。 */
export interface ArgSpec {
  type: "string" | "number" | "boolean" | "array" | "object"
  description: string
  items?: ArgSpec
  enum?: string[]
  required?: boolean
}

/** 一个动词/观测面的声明。 */
export interface CapabilitySpec {
  /** 一句话说明（会进系统提示词与 channels_list 输出）。 */
  summary: string
  /** 载荷参数（键为参数名）。 */
  args?: Record<string, ArgSpec>
  /**
   * 该动作是否产生外部副作用（true = 写）。
   *
   * 引擎据它决定审批姿态的缺省值：读面一律免审，写面按渠道策略。
   * 声明 false 却真的写入 = 绕过审批（实现方责任）。
   */
  writes?: boolean
}

/** 动作结果（渠道返回给调用方的凭证）。 */
export interface ActOutcome {
  /** 外部系统的稳定引用（message_id / issue_key / 文件路径…）：调用方据此后续引用。 */
  ref?: string
  /** 人类可读的结果摘要（进工具 output）。 */
  summary: string
  /** 渠道自定义结构化数据（进 ToolResult.data）。 */
  data?: Record<string, unknown>
}

/** 观测结果。 */
export interface ObserveOutcome {
  /** 人类可读的结果（进工具 output）。 */
  summary: string
  /** 结构化数据（进 ToolResult.data）。 */
  data?: Record<string, unknown>
}

/** 一次动作的调用参数（渠道侧）。 */
export interface ActInput {
  /** 目标地址（`to` 里冒号后的部分，已剥离渠道前缀；如 `oc_support`）。 */
  address: string
  /** 完整目标（含渠道前缀，如 `feishu:oc_support`）——渠道需要原值做日志/校验时用。 */
  target: string
  verb: string
  args: Record<string, unknown>
  /** 幂等键（调用方给；渠道重试时用它去重）。 */
  idempotencyKey?: string
}

/** 一次观测的调用参数。 */
export interface ObserveInput {
  address: string
  target: string
  what: string
  args: Record<string, unknown>
}

/**
 * 渠道运行时上下文（由注册表注入，渠道不得自取）。
 *
 * 刻意**没有** engine / store / registry：渠道不需要、也不该有跑任务或读写会话的能力。
 * 它要读写自身状态就落在 `home` 下。
 */
export interface ChannelContext {
  /** 数据根（`{GEBAI_HOME}`）：渠道自有游标/缓存放这里。 */
  home: string
  /** 渠道名（日志前缀）。 */
  name: string
  /** 结构化日志。 */
  log(level: "info" | "warn" | "error", msg: string): void
  /** 声明白名单内的环境变量（未声明读不到）。 */
  env(name: string): string | undefined
  /** 宿主注入的共享依赖（只读；如凭据存储）。 */
  deps: Readonly<Record<string, unknown>>
}

/**
 * 入站事件汇（渠道唯一的「推」出口）。
 *
 * 与 `act`/`observe` 的方向相反：那两个是模型**主动拉**（有返回值、即调即止）；
 * 本接口是外部世界**主动推**（无同步返回、常驻）。混成一个方法会让两边都变复杂。
 *
 * 返回值语义（核心已裁决，渠道据此决定记不记日志）：
 * - `accepted`   至少一个订阅者已接管
 * - `unrouted`   **无人关注**——渠道不应自行处理，仅记日志（保证零配置不误吞）
 * - `duplicate`  同 id 已被全部订阅者处理（幂等丢弃，正常）
 * - `overflow`   全部订阅者队列已满
 * - `unavailable` 运行时未启动/已停止
 */
export interface EventSink {
  emit(event: IngressEvent): Promise<EmitResult>
}

/**
 * 一条入站事件（渠道产出，核心负责路由与幂等）。
 *
 * 为什么不让渠道直接产出完整的 `PersonaEvent`：`id`（幂等键）、`channel`（路由键）、
 * 归属人格的推导属于**核心职责**——放渠道里会让每个源重新实现一遍，必然分叉。
 * 渠道只回答「发生了什么」，核心回答「这归谁、进哪个会话」。
 */
export interface IngressEvent {
  /** 一句话摘要：发生了什么（进系统提示词的「事件摘要」）。 */
  summary: string
  /** 主体内容（模型的输入）。 */
  body: string
  /** 人类可读来源，如「飞书群「支持群」」。缺省用渠道名。 */
  source?: string
  /** 事件类别（决定默认路由与出站纪律文案）。缺省按渠道的 `defaultKind` 或 `message`。 */
  kind?: IngressKind
  /** 结构化引用（谁说的、回到哪）：模型需要它才能正确出站。 */
  refs?: IngressRefs
  /** 显式幂等键（渠道最清楚自己那个世界的稳定 id）。强烈建议给。 */
  id?: string
  /** 事件产生时刻（ms）。缺省 now。 */
  at?: number
  /** 附件（图片等）。 */
  attachments?: Array<{ name: string; mime?: string; path?: string; data?: Uint8Array }>
}

/** 事件类别（与 persona 的 `PersonaEventKind` 同集合）。 */
export type IngressKind = "schedule" | "message" | "webhook" | "desktop" | "idle"

/** 入站事件的引用（谁说的、回到哪）。 */
export interface IngressRefs {
  /** 渠道会话/群 id（如飞书 chat_id）。 */
  chatId?: string
  /** 原消息 id（出站时可用于引用回复）。 */
  messageId?: string
  /** 发送者（渠道侧 id，如飞书 open_id）。 */
  senderId?: string
  /** 发送者显示名。 */
  senderName?: string
  /** 关联的调度任务 id（kind=schedule 时）。 */
  taskId?: string
  /** 其它渠道自定义引用（原样透传，不参与逻辑）。 */
  extra?: Record<string, string>
}

/** 汇的裁决结果（渠道据此决定回不回话/记日志）。 */
export type EmitResult =
  | { status: "accepted"; personaId?: string; sessionId?: string; queued?: number }
  | { status: "unrouted" | "duplicate" | "overflow" | "unavailable"; reason?: string }

/**
 * 渠道定义（单文件 `export const channel: ChannelDef = {…}`）。
 *
 * 与子 Agent / 事件源同款「丢文件即注册」：放 `custom/channels/{name}.ts` 即生效，
 * 不改核心代码、不改提示词。
 */
export interface ChannelDef {
  /** 渠道名（`[a-z0-9_]+`，须与文件名一致）：也是 target 的前缀。 */
  name: string
  /** 一句话说明（面板与 channels_list 可见）。 */
  description: string
  /** 本渠道需要的环境变量（声明式白名单）。 */
  envVars?: Array<{ name: string; description: string; secret?: boolean }>

  /** 写面声明：verb → 声明。空/缺省 = 该渠道无写能力。 */
  verbs?: Record<string, CapabilitySpec>
  /** 读面声明：what → 声明。空/缺省 = 不可观测。 */
  observes?: Record<string, CapabilitySpec>
  /**
   * **输入面**声明：listens → 声明。空/缺省 = 该渠道不接收事件。
   *
   * 与 `verbs`/`observes` 并列：同一个渠道文件因此能同时声明「能发什么、能收什么」。
   */
  listens?: Record<string, CapabilitySpec>
  /** 入站事件的缺省类别（渠道未在单条事件里指定时用）。 */
  defaultKind?: IngressKind

  /**
   * 订阅入站事件（常驻）。
   *
   * 三条约束（与事件源插件同口径）：
   * 1. **必须尽快返回**：订阅/建连后立即 resolve，长驻循环放后台（否则启动阻塞）。
   * 2. **状态放本次调用闭包**：同进程可能有多个注册表实例（热加载新旧并存、测试并行），
   *    模块级变量会互相清空，表现为「事件偶发丢失」。
   * 3. **不要自己建第二条连接**：宿主已有同一系统的连接时，经 `ctx.deps` 取回调注册面
   *    （如飞书长连接只能一条，两套各自连会互踢）。
   */
  startListen?(sink: EventSink, ctx: ChannelContext): Promise<void>

  /**
   * 停止订阅（释放连接/定时器）。
   *
   * 带 `ctx` 的原因：停止时需要定位「停哪个实例」——同一份渠道定义可能同时被多个
   * 注册表实例使用（并行测试、多租户、多用户），`ctx.home` 是区分它们的稳定键。
   *
   * 实现方约定：把清理函数按 `(ctx.home, this)` 登记，这里按同一键取出并调用。
   */
  stopListen?(ctx: ChannelContext): Promise<void>

  /**
   * 合法目标清单（白名单，fail-closed）。
   *
   * - 返回数组：**只**允许这些 address（越界直接拒绝，不进审批）。
   * - 返回 `null`：不做地址级限制（仍需 `enabled` 开关开启；适用于地址本身即权限边界的渠道，
   *   如 `local:memory`）。
   *
   * 为何必须存在：模型的 `to` 来自事件上下文或它自己的推断，一次推断错就是「把内部信息发到
   * 无关的群」。调用方不该有机会决定「是否可以问用户」——直接拒绝才是正确的失败姿态。
   */
  targets?(ctx: ChannelContext): Promise<string[] | null> | string[] | null

  /** 执行一次写动作（`verbs` 已声明且校验通过后才会被调）。 */
  act?(input: ActInput, ctx: ChannelContext): Promise<ActOutcome>
  /** 执行一次观测（`observes` 已声明且校验通过后才会被调）。 */
  observe?(input: ObserveInput, ctx: ChannelContext): Promise<ObserveOutcome>

  /** 生命周期（建立连接/加载凭据）；抛错 = 该渠道不可用（不阻断其它渠道）。 */
  start?(ctx: ChannelContext): Promise<void>
  /** 释放连接/定时器。 */
  stop?(): Promise<void>
}

/** 渠道加载失败记录（失败隔离：单渠道坏不影响其它，根因可见）。 */
export interface ChannelLoadError {
  name: string
  error: string
  /** 来源域（`builtin` / `custom`）：二开覆盖时便于定位是谁坏了。 */
  domain: "builtin" | "custom"
}

/** target 解析结果。 */
export interface ParsedTarget {
  channel: string
  address: string
  /** 原值（错误信息与日志用）。 */
  raw: string
}

/**
 * 解析 `channel:address`。
 *
 * 只切第一个冒号：address 自身可以含冒号（如 `local:memory#section`、`jira:PROJ:123`），
 * 用 `split(":")[0..1]` 会截断它们。
 */
export function parseTarget(raw: string): ParsedTarget | { error: string } {
  const s = String(raw ?? "").trim()
  if (!s) return { error: "target 不能为空（格式：channel:address，如 feishu:oc_xxx）" }
  const i = s.indexOf(":")
  if (i < 0) return { error: `target「${s}」缺少渠道前缀（格式：channel:address）` }
  const channel = s.slice(0, i).trim()
  const address = s.slice(i + 1).trim()
  if (!channel) return { error: `target「${s}」的渠道前缀为空` }
  if (!address) return { error: `target「${s}」的地址为空（形如 ${channel}:<address>）` }
  if (!/^[a-z0-9_]+$/.test(channel)) return { error: `target 渠道名非法「${channel}」（须 [a-z0-9_]+）` }
  return { channel, address, raw: s }
}
