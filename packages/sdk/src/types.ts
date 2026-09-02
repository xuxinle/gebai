export interface AttachmentRef {
  path: string
  mime: string
  name: string
  size: number
}

export interface AttachmentInput {
  name: string
  mime?: string
  path?: string
  data?: Uint8Array
}

export interface AttachmentInfo {
  id: string
  name: string
  mime: string
  size: number
  path: string
}

export interface ToolCall {
  id: string
  name: string
  arguments: Record<string, unknown>
}

/** 行级 diff 结果（`diff` 内容块）：按顺序排列，每行标注差异类型。 */
export interface DiffLine {
  kind: "equal" | "add" | "del"
  text: string
}

/** 图表语言（show 图表分支四格式）：plantuml=UML 严谨建模 / mermaid=通用流程图时序图 / d2=美观架构图 / echarts=数据图表（JSON option）。 */
export type DiagramFormat = "plantuml" | "mermaid" | "d2" | "echarts"

/**
 * Rich content blocks embedded in messages, rendered by the UI.
 * `image`/`file`/`code`（`path` 可选，文件直显时携带）`path` values are logical paths relative to the session tmp/ dir,
 * or absolute paths (project files, e.g. read via code sub-agent) resolved by `files/preview`.
 */
export type ContentBlock =
  | { type: "text"; text: string }
  | { type: "code"; text: string; language?: string; path?: string; name?: string }
  | { type: "image"; path: string; name?: string; mime?: string }
  | { type: "file"; path: string; name: string; mime?: string }
  | { type: "diagram"; format: DiagramFormat; code: string; name?: string; version?: number }
  | { type: "diff"; oldText: string; newText: string; language?: string; name?: string; oldName?: string; newName?: string; lines: DiffLine[] }
  | { type: "html"; html: string; name?: string; width?: number; height?: number }

export interface Message {
  id: string
  role: "user" | "assistant" | "tool" | "system"
  content: string
  /** 推理内容（reasoning_content/thinking）独立字段：assistant 消息持久化时写入，content 保持纯正文；
   *  回放给 LLM 时（loadHistory）不携带——推理绝不进模型上下文；UI 历史渲染为折叠推理卡。 */
  reasoning?: string
  blocks?: ContentBlock[]
  attachments?: AttachmentRef[]
  toolCalls?: ToolCall[]
  toolCallId?: string
  name?: string
  /** 工具消息：多模态图片引用（read 等工具读取的图片文件，`ToolResult.images` 落盘形态）：`path` 为
   *  解析后绝对路径、`display` 为模型可见的原始参数路径。loadHistory 在主模型声明多模态且处于最近
   *  内联窗口时按 `path` 重读 base64 内联进上下文（统一 image 块）；非多模态/超出窗口降级为文本说明。 */
  images?: Array<{ path: string; display?: string; mime: string }>
  /** 工具消息：调用参数（历史会话重载时用于渲染卡片，与实时一致） */
  arguments?: Record<string, unknown>
  /** 子Agent 装载提示词消息标记（role=system）：装载（agent_load/启动预载/WS sub_agent.load）时写入会话记录，
   *  内容为子Agent 完整系统提示词；loadHistory 时按 system 角色透传进模型上下文，UI 渲染为简短装载提示。 */
  loadedAgent?: string
  /** 上下文压缩产生的摘要消息标记（role=system），UI 渲染为压缩通知 */
  compacted?: boolean
  /** 压缩摘要消息：被压缩的原始区间描述（条数/时间范围） */
  summary?: string
  /** 新会话执行（agent_run）过程消息标记：完整存档但【不进入主 LLM 上下文】（loadHistory 跳过），前端按 runId 分组折叠渲染。 */
  session?: boolean
  /** 新会话执行 run 标识：同一次 agent_run 执行过程的消息共享（前端回放按此分组）。 */
  sessionRunId?: string
  /** 新会话执行 run 元信息（仅该 run 首条消息携带）：折叠容器标题用（预加载子Agent 名与输入）。 */
  sessionMeta?: { agents: string[]; input: string }
  /**
   * 新会话执行完整存档（agent_run 工具调用记录的扩展字段）：该次执行的全部内容
   * （输入/每轮回复/推理/工具调用与结果），历史会话回放据此渲染折叠容器。
   * 分支运行（branch_run）同样以本形态存档（branch 字段标识）。
   */
  sessionRun?: SessionRunArchive
  /** 分支运行合并消息标记（role=assistant，DESIGN「会话分支运行与合并」）：branch_run 分支最终报告
   *  合入主上下文的消息携带；随消息落盘，loadHistory 按普通 assistant 消息进上下文（内容自带分支头行），
   *  UI 据此渲染分支合并样式，sessionRun 字段携带分支过程存档供回放。 */
  branchMeta?: { branchId: string; name: string; model?: string }
  /** 旧版（agent_call 时代）字段：兼容历史会话回放，新数据不再写入。 */
  subAgent?: boolean
  /** 旧版（agent_call 时代）字段：兼容历史会话回放，新数据不再写入。 */
  subAgentRunId?: string
  /** 旧版（agent_call 时代）字段：兼容历史会话回放，新数据不再写入。 */
  subAgentMeta?: { agent: string; input: string }
  /** 旧版（agent_call 时代）字段：兼容历史会话回放，新数据不再写入。 */
  subAgentRun?: LegacySubAgentRunArchive
  createdAt: number
}

/** 新会话执行存档条目：执行过程消息（user/assistant/tool 全量内容）。 */
export interface SessionRunEntry {
  role: "user" | "assistant" | "tool"
  name?: string
  content: string
  /** 推理内容独立字段（同 Message.reasoning 语义）：assistant 条目持久化时写入，回放展示不回流模型。 */
  reasoning?: string
  toolCalls?: ToolCall[]
  toolCallId?: string
  arguments?: Record<string, unknown>
  blocks?: ContentBlock[]
  /** 嵌套 agent_run（新会话内再执行新会话）的存档递归携带。 */
  sessionRun?: SessionRunArchive
}

/** 新会话执行完整存档（agent_run 工具记录扩展字段，见 Message.sessionRun）。 */
export interface SessionRunArchive {
  runId: string
  /** 预加载进新会话的子Agent 列表（完整系统提示词与工具进入新会话上下文）。 */
  agents: string[]
  input: string
  /** 最终返回文本（容器折叠后摘要展示；异常/取消为空串）。 */
  output: string
  messages: SessionRunEntry[]
  /** 分支运行（branch_run）标识：分支存档携带（agents 为空数组），前端容器标题按分支名渲染。 */
  branch?: { name: string; model?: string }
}

/** 旧版（agent_call 时代）子Agent run 存档：结构同 SessionRunArchive 但 agent 为单值，仅历史会话兼容回放。 */
export interface LegacySubAgentRunEntry {
  role: "user" | "assistant" | "tool"
  name?: string
  content: string
  toolCalls?: ToolCall[]
  toolCallId?: string
  arguments?: Record<string, unknown>
  blocks?: ContentBlock[]
  subAgentRun?: LegacySubAgentRunArchive
}

/** 旧版（agent_call 时代）子Agent run 完整存档（仅历史会话兼容回放，见 Message.subAgentRun）。 */
export interface LegacySubAgentRunArchive {
  runId: string
  agent: string
  input: string
  output: string
  messages: LegacySubAgentRunEntry[]
}

export interface ChatChunk {
  kind: "text" | "reasoning" | "tool_call" | "tool_result" | "approval" | "done" | "error" | "reset" | "resume" | "session_start" | "session_done" | "model_error"
  messageId?: string
  /** 事件来自新会话执行过程（agent_run 派生会话；主回复不带此标记）。 */
  session?: boolean
  /** 新会话执行 run 标识：同一次 agent_run 的执行过程事件共享（前端按此分组渲染）。 */
  sessionRunId?: string
  /** 新会话执行 run 元信息：session_start 携带 agents/input，session_done 携带 agents/output（前端折叠容器标题用）；
   *  分支运行（branch_run）的 start/done 携带 branch（分支名）与 model（模型路由名，未指定缺省）。 */
  sessionMeta?: { agents: string[]; input?: string; output?: string; branch?: string; model?: string }
  text?: string
  toolCall?: ToolCall
  approval?: { toolCallId: string; retries: number; tool: string }
  output?: string
  blocks?: ContentBlock[]
  error?: string
  /** model_error：即将进行的重试序号（第几次重试）与总次数（引擎自动重试中的瞬时异常提示）。 */
  retry?: number
  maxRetry?: number
}

export interface AgentEvent {
  type: string
  sessionId: string
  payload: Record<string, unknown>
  timestamp: number
  /** 服务端每用户事件日志序号（断线重连后按 seq 重放补偿；旧服务端不携带）。 */
  seq?: number
}

/** WS 状态快照（state.snapshot payload）：重连/登录后客户端模型收敛基线。 */
export interface WsSnapshot {
  currentSessionId: string | null
  sessions: SessionInfo[]
  running: string[]
  lastSeq: number
  /** 模型上下文窗口（token）：0=未知/未配置，标题栏上下文占比显示用。 */
  maxContextTokens?: number
}

/** 计划审批选择请求携带的计划载荷（ask 计划分支，`event.choice.request` / attach 快照）：
 *  前端选择卡内嵌计划全文——审批时直接可见，不依赖消息流位置与滚动状态。 */
export interface ChoicePlanPayload {
  title: string
  content: string
  /** 计划文档逻辑路径（tmp/plans/ 下）。 */
  path: string
}

/** 运行中会话的待决交互（session.attach 快照项）：事件已推送过、新页面收不到，前端凭此重渲染卡片继续作答。 */
export type PendingInteraction =
  | { type: "approval"; toolCallId: string; tool: string; retries: number }
  | { type: "choice"; choiceId: string; prompt: string; options: Array<string | Record<string, unknown>>; multi: boolean; plan?: ChoicePlanPayload }
  | { type: "env"; envId: string; name: string; description: string; secret: boolean }
  | { type: "draw"; renderId: string; code: string; name?: string; format?: string }
  | { type: "capture"; captureId: string; fullPage: boolean; delay: number }

/** 运行中会话附加快照（session.attach，DESIGN「运行中会话恢复」）：页面刷新/切换后恢复用。 */
export interface AttachSnapshot {
  running: boolean
  /** 任务开始时刻（前端单轮计时器起点恢复用）。 */
  startedAt?: number
  /** 在途 assistant 回合的累积文本/推理（尚未持久化——刷新后从存储恢复不了的部分）。 */
  stream?: { messageId: string; text: string; reasoning: string; session?: boolean; sessionRunId?: string }
  /** 待决交互清单（审批/选择/填值/画图/捕获）。 */
  pending: PendingInteraction[]
  /** 快照反映到的事件日志 seq（attach 流据此过滤已含入快照的事件并重放缺口）。 */
  lastSeq?: number
}

export interface SessionInfo {
  id: string
  name: string
  userId: string
  createdAt: number
  updatedAt: number
  /** 上下文 token 估算（chars/4，会话列表展示用，单位 k）。 */
  ctxTokens?: number
  /** 同一次调用的提示词缓存命中 tokens（ctxTokens 真值基线的一部分，接口返回缓存字段才有值；
   *  上下文圆环悬浮命中率展示用，运行中随 event.session.ctx 实时更新）。 */
  ctxCachedTokens?: number
}

export interface SessionDetail extends SessionInfo {
  messages: Message[]
}

export interface TodoItem {
  id: string
  title: string
  status: "pending" | "in_progress" | "completed" | "failed" | "cancelled"
  priority?: "low" | "medium" | "high"
  progress?: number
  etaMin?: number
  dependencies?: string[]
  note?: string
}

export interface FileEntry {
  path: string
  size: number
  modifiedAt: number
  isDir: boolean
}

export interface FeedbackInput {
  messageId: string
  sessionId: string
  type: "thumbs_up" | "thumbs_down" | "text" | "suggestion"
  text?: string
  label?: string
  anonymous?: boolean
}

export interface FeedbackFilter {
  messageId?: string
  sessionId?: string
  type?: FeedbackInput["type"]
}

export interface FeedbackInfo extends FeedbackInput {
  id: string
  userId: string
  model?: string
  subAgent?: string
  createdAt: number
}

export interface WebhookInfo {
  id: string
  url: string
  /** 订阅的事件类型白名单；为空数组时订阅全部事件。 */
  events: string[]
  /** HMAC 签名密钥（服务端脱敏返回 ***）。 */
  secret?: string
  userId?: string
  createdAt: number
}

export interface UserInfo {
  id: string
  username: string
  role: "user" | "admin"
  disabled: boolean
  /** 注册待审批标记（GEBAI_SIGNUP_MODE=approval 时注册用户为 true，admin 批准后清除）。 */
  pending?: boolean
  createdAt: number
}

export interface UserPatch {
  password?: string
  role?: "user" | "admin"
  disabled?: boolean
  /** 待审批标记：admin 批准注册用户时置 false（同时 disabled=false 启用）。 */
  pending?: boolean
}

export interface ToolInfo {
  name: string
  description: string
  enabled: boolean
  group: "global" | string
  approvalRequired: boolean
  /** 卡片展示元数据（同服务端 Tool.card，前端渲染工具卡片用）。 */
  card?: {
    titleParams?: string[]
    args?: "json" | "kv" | "none" | "code" | "edits" | "block"
    codeField?: string
    codeLang?: string
    /** 路径参数名（文件卡声明）：「弹窗查看」模式下该工具产物 file 块收敛为文件链接。 */
    file?: string
  }
}

export interface SubAgentInfo {
  name: string
  description: string
  tools: string[]
  preload: boolean
  loaded: boolean
  bundled: boolean
}

/** HTML 小工具（Agent 经 save_tool 保存到服务端，公用或用户私有）。 */
export interface MiniToolMeta {
  name: string
  scope: "public" | "private"
  /** 创建者用户 id（公用工具记录归属）。 */
  owner?: string
  createdAt: number
  updatedAt: number
}

/** 完整小工具信息（含 HTML 源码，仅单条读取时返回）。 */
export interface MiniToolInfo extends MiniToolMeta {
  html: string
}

export interface LLMCapabilities {
  streaming: boolean
  toolCalling: boolean
  multimodal: boolean
  maxContextTokens: number
}

export interface ToolSchema {
  type: "object"
  properties: Record<string, unknown>
  required?: string[]
}

export interface EnvVarSource {
  name: string
  value: string
  source: "global" | "session"
  sensitive: boolean
}

export interface MessageLike {
  role: "system" | "user" | "assistant" | "tool"
  content: string | Array<Record<string, unknown>>
  name?: string
  toolCallId?: string
  toolCalls?: Array<{ id: string; name: string; arguments: Record<string, unknown> | string }>
  /** 嵌套 agent_run 存档（仅服务端内存态挂载，provider 序列化忽略；不进主 LLM 上下文）。 */
  sessionRun?: SessionRunArchive
}
