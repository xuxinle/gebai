/**
 * 事件源插件契约：**通用扩展规范**（与子 Agent 同款「丢文件即注册」）。
 *
 * 为什么要有插件：驱动源会不断增长（飞书、定时、webhook、邮件、Jira、监控告警、
 * 串口设备…）。若每加一个源都改 `runtime` / `compose` / 配置表，就是典型的能力增长
 * 点被写死在核心——与子 Agent 一样的病，用一样的药：**目录扫描 + 单文件定义 + 零注册**。
 *
 * 分层（对齐 `SubAgentDef`）：
 * - 插件只负责「**把外部世界的一件事变成 `PersonaEvent`**」，不解释人格、不跑引擎、不推送。
 * - 是否接管、进哪个会话、什么额度——全由 `PersonaRuntime`（核心）裁决。
 *   插件因此永远拿不到「绕过治理」的能力（DESIGN「能力外置、权力内守」）。
 *
 * 与子 Agent 的差异（为何不共用同一套定义）：
 * - 子 Agent 产出**工具**（进入模型工具面），事件源产出**事件**（进入调度器）——语义与
 *   生命周期完全不同，混在一个定义里会让两侧都变复杂。
 * - 但**扫描/热加载/失败隔离/二开覆盖**四套机制逐条对齐，见 `discovery.ts`。
 */

import type { PersonaAttachment, PersonaEventKind, PersonaEventRefs } from "./event"

/**
 * 一条「原始入站事件」：插件产出它，由核心统一转成 `PersonaEvent`。
 *
 * 为何不直接产出 `PersonaEvent`：
 * - `personaId` 与 `threadKey` 的推导属于**路由**（核心职责，含冲突检测）；
 * - `id`（幂等键）的推导规则各源不同，集中在 `events.ts` 才不会分叉。
 * 插件只回答「发生了什么」，核心回答「这归谁、进哪个会话」。
 */
export interface IngressEvent {
  /** 事件类别（决定默认路由：schedule/idle → anchor）。 */
  kind: PersonaEventKind
  /** 渠道标识（`feishu` / `mail` / `jira`…，由插件声明，用于路由匹配与展示）。 */
  channel: string
  /** 人类可读来源，如「飞书群「支持群」」。 */
  source: string
  /** 一句话摘要：发生了什么。 */
  summary: string
  /** 主体内容（模型的输入）。 */
  body: string
  /** 结构化引用（谁说的、回到哪）。 */
  refs?: PersonaEventRefs
  /** 显式幂等键（插件最清楚该源的稳定 id；缺省由核心按 kind 推导）。 */
  id?: string
  attachments?: PersonaAttachment[]
  /** 事件产生时刻（缺省 now）。 */
  at?: number
  /**
   * 显式指定归属人格（缺省 = **广播给所有关注者**）。
   *
   * 插件**几乎总该留空**：归属是部署配置（人格订阅）的事，不是源的事。
   * 仅当「这件事只该由某个人格处理」（如人格私有任务）时才填。
   */
  personaId?: string
}

/**
 * 插件生命周期上下文（由核心注入，插件不得自取）。
 *
 * 注意这里**没有** engine / store / registry：事件源插件不需要、也不该有运行任务或
 * 读写会话的能力。它唯一的出口是 `emit()`。
 */
export interface EventSourceContext {
  /**
   * 投递一条入站事件（**广播给所有关注者**）。
   *
   * 返回值语义（核心已裁决，插件据此决定要不要回话/记日志）：
   * - `accepted`：至少一个订阅者已接管
   * - `unattended`：**无人关注**该类事件 → 插件不应自行处理，仅记日志
   *   （保证零配置时不误吞）
   * - `duplicate`：同 id 已被全部订阅者处理（幂等丢弃，正常）
   * - `overflow`：全部订阅者队列已满
   * - `unavailable`：人格运行时未启动/已停止
   */
  emit(event: IngressEvent): Promise<EmitResult>
  /** 结构化日志（带插件名前缀，受 `GEBAI_LOG_LEVEL` 过滤）。 */
  log(level: "info" | "warn" | "error", msg: string): void
  /** 数据根（`GEBAI_HOME`）：插件自有的游标/去重状态可放这里。 */
  home: string
  /** 插件名（诊断用）。 */
  name: string
  /**
   * 可选：环境变量读取（插件声明的 `envVars` 之外一律不可见）。
   *
   * 为什么收窄：事件源插件常在多用户/服务端形态下运行，直接 `process.env` 会把
   * 模型密钥等敏感配置暴露给一个只需要「读告警」的插件。核心按声明白名单过滤。
   */
  env(name: string): string | undefined
  /**
   * 宿主注入的能力（可选，由装配方经 `EventSourceManager` 的 `deps` 传入）。
   *
   * 为什么不用 import：部分源需要宿主侧能力（如飞书源要复用 `@gebai/agents` 的
   * 会话级 user_access_token 存储）。若让插件直接 import 宿主包，本内核就会被
   * 拖入其全部重依赖（PDF/Office/Playwright…），「零依赖、可独立单测」不复存在。
   * 因此这类能力只能**注入**，插件按需取用，取不到就如实降级（并记日志）。
   *
   * 约定：插件**不得**修改 deps（它是共享实例）；只读使用。
   */
  deps: Readonly<Record<string, unknown>>
}

export type EmitResult =
  | {
      status: "accepted"
      /** 首个接管的人格（兼容字段）。 */
      personaId?: string
      sessionId?: string
      queued?: number
      /** 逐订阅者结果（广播模型：一条事件可能被多个人格同时处理）。 */
      deliveries?: Array<{ personaId: string; status: string; sessionId?: string; queued?: number; reason?: string }>
    }
  | { status: "unattended" | "duplicate" | "overflow" | "unavailable"; reason?: string }

/**
 * 事件源插件定义（单文件 `export const source: EventSourceDef = {…}`）。
 *
 * 与 `SubAgentDef` 对齐的字段：`name` / `description` / （可选）`dependencies`。
 * 事件源特有的：`channel`（路由键）/ `start`（订阅）/ `stop`。
 */
export interface EventSourceDef {
  /**
   * 插件名（`[a-z0-9_]+`，须与文件名/目录名一致）。
   * 用途：日志前缀、`/api/v1/persona/sources` 展示、启停名单（`GEBAI_EVENT_SOURCES_ENABLE/DISABLE`）。
   */
  name: string
  /** 一句话说明（面板/诊断可见）。 */
  description: string
  /**
   * 该源产出事件的渠道标识（写入 `IngressEvent.channel`）。
   * 人格定义按它做路由匹配（`channels.{channel}.chats` 等），所以**必须稳定**。
   */
  channel: string
  /**
   * 插件需要的环境变量（声明式白名单，面板据此展示；核心按此过滤 `ctx.env`）。
   * 与子 Agent 的 `envVars` 同形同义。
   */
  envVars?: Array<{ name: string; description: string; secret?: boolean }>
  /**
   * 启动订阅。抛错 = 该源不可用（记 `loadErrors` + 告警，**不阻断其它源与服务本体**）。
   *
   * 约定：`start` 应尽快返回（订阅/建连后立即 resolve），不要在内部跑长循环——
   * 长循环会让启动阻塞；需要长驻的连接请自行起后台任务并在 `stop` 里清理。
   */
  start(ctx: EventSourceContext): Promise<void>
  /** 停止订阅（释放连接/定时器）。可省略；异常只记日志。 */
  stop?(): Promise<void>
}

/** 插件加载失败记录（失败隔离：单插件坏不影响其它，根因可见）。 */
export interface EventSourceLoadError {
  name: string
  /** 失败原因（import 抛错 / 未导出 source / 字段非法…）。 */
  error: string
  /** 来源域（`builtin` / `custom`）：二开覆盖时便于定位是谁坏了。 */
  domain: "builtin" | "custom"
}
