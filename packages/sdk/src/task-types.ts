/**
 * 统一任务数据类型（引擎调度器与 task 子代理的共享契约，浏览器安全集）。
 * 数据形状与引擎实现（server core/schedule/tasks.ts）结构兼容——服务类实现这些接口，
 * 子代理只经 ToolContext.tasks 契约接口消费，不依赖引擎内部模块。
 */

/** 通知通道类型：webhook=通用 HTTP 回调；feishu=飞书群自定义机器人 webhook；feishu_chat=飞书应用消息（chat_id）。 */
export type TaskNotifyType = "webhook" | "feishu" | "feishu_chat"

/** @ 人配置：id 为 open_id（ou_/un_/on_ 前缀）或 "all"（@所有人）；name 为展示名（缺省由客户端解析真实姓名）。 */
export interface FeishuAtTarget {
  id: string
  name?: string
}

/** 任务通知通道（任务内嵌配置，可配多条）。 */
export interface TaskNotifyChannel {
  type: TaskNotifyType
  /** webhook/feishu：webhook=http(s) URL；feishu=群机器人 webhook URL 或群 chat_id（oc_ 前缀，应用消息推送）；feishu_chat：群 chat_id。
   *  type=webhook 时可与 webhookId 二选一（引用已注册事件 Webhook，投递时解析其 URL 与签名密钥）。 */
  target?: string
  /** 引用 REST /api/v1/webhooks 注册的事件 Webhook（type=webhook；须本人注册或全局注册，创建时校验）。 */
  webhookId?: string
  /** feishu 加签密钥（可选，群机器人安全设置「签名校验」）；webhook 直配 URL 时为 HMAC 签名密钥（X-Gebai-Signature）。 */
  secret?: string
  /** @ 人名单（feishu/feishu_chat 渲染进卡片；webhook 随 JSON 载荷 at 字段携带，供接收方解析 @ 人）。 */
  at?: FeishuAtTarget[]
}

/** 通知通道输入形态（at 允许字符串 open_id/"all" 或 {id,name}，创建/修改时归一为 {id,name?}）。 */
export type TaskNotifyInput = Omit<TaskNotifyChannel, "at"> & { at?: Array<string | FeishuAtTarget> }

/** 通知时机（两种）：auto=执行结束自动把最后回复/输出作为通知发出（缺省）；model=不自动发，由执行会话的模型经 `task_notify` 主动决定。 */
export type TaskNotifyWhen = "auto" | "model"

/** 主动通知消息（task_notify / TaskService.notify）：正文由调用方自撰，投递到任务配置的通知通道。 */
export interface TaskNotifyMessage {
  /** 通知正文（markdown：飞书卡片正文 / webhook 载荷 text 字段），单条上限 2000 字符。 */
  text: string
  /** 标题（可选，缺省用任务名）。 */
  title?: string
  /** @ 人名单（可选，覆盖通道自带的 at；"all"=@所有人）。 */
  at?: Array<string | FeishuAtTarget>
}

/** 主动通知投递结果。 */
export interface TaskNotifyResult {
  /** 实际使用的任务 id（未指定时按执行会话推断）。 */
  taskId: string
  /** 投递成功的通道数。 */
  delivered: number
  /** 各通道失败原因（空数组=全部成功）。 */
  errors: string[]
}

/** 任务类别：scheduled=定时（按表达式触发）；manual=普通（入队即按顺序执行）；idle=闲时（队列空闲时执行一次，成功或失败/超时均停用，失败不重试）。 */
export type TaskKind = "scheduled" | "manual" | "idle"
/** 执行体：script=脚本（shell 在任务目录执行）；prompt=提示词（触发一次完整 Agent 会话）。 */
export type TaskRunner = "script" | "prompt"
/** prompt 型执行目标：ephemeral=新建会话（缺省）；sticky=专用会话跨次复用；session=绑定既有会话。 */
export type TaskTarget = "ephemeral" | "sticky" | "session"
/** 停机错过触发的补跑策略：skip=跳过从当前重算（缺省）；run=启动后立即补跑一次。 */
export type TaskMisfire = "skip" | "run"
/** 单次运行结果状态。 */
export type TaskRunStatus = "success" | "error" | "skipped" | "timeout"
/** 运行态：idle=不在队列也未运行；queued=排队等待；running=运行中（持久化，重启后按此恢复）。 */
export type TaskQueueState = "idle" | "queued" | "running"
/** 入队来源：schedule=定时到期；manual=用户/接口手动执行；idle=闲时调度；todo=待办手动执行。 */
export type TaskQueueSource = "schedule" | "manual" | "idle" | "todo"

/** 单次运行历史记录（按文件落盘：`users/{user}/task-runs/{taskId}/{时间}.json`，经 `TaskService.runs` 读取）。 */
export interface TaskRunRecord {
  id: string
  /** 触发（入队启动）时间。 */
  at: number
  /** 结束时间。 */
  endedAt: number
  status: TaskRunStatus
  durationMs: number
  output?: string
  error?: string
  /** prompt 型本次运行的会话（执行轨迹）。 */
  sessionId?: string
  /** 手动执行（task_run / 待办立即执行 / REST run）。 */
  manual?: boolean
  /** 未启动即跳过时的原因（同任务已在队列/运行中、目标会话忙等）。 */
  reason?: string
}

/** 队列中的条目（按用户队列排序后对外呈现）。 */
export interface TaskQueueEntry {
  taskId: string
  user: string
  kind: TaskKind
  runner: TaskRunner
  name?: string
  source: TaskQueueSource
  /** 排序权重（小者先）：定时 0、普通置顶 999、普通 1000、闲时 2000。 */
  priority: number
  enqueuedAt: number
  /** 显式置顶（普通任务插队）。 */
  front?: boolean
  /** 已被本次运行解析出的执行会话（入队前为任务配置目标）。 */
  sessionId?: string
  /** 本次排队的等待原因（如「目标会话正在运行，等待中」）。 */
  waiting?: string
  /** 队列内位置（1 起；仅 queued 条目有）。 */
  position: number
}

/** 队列视图（GET /api/v1/tasks/queue）。 */
export interface TaskQueueView {
  user: string
  /** 每用户并发额度（GEBAI_TASK_MAX_CONCURRENT）。 */
  limit: number
  running: Array<{ taskId: string; name?: string; kind: TaskKind; runner: TaskRunner; sessionId?: string; startedAt: number }>
  entries: TaskQueueEntry[]
  /** 是否有待执行的定时/普通任务（闲时任务据此让路）。 */
  busy: boolean
}

/** 任务（用户级资源，持久化于 users/{user}/tasks.json，与会话生命周期解耦）。 */
export interface Task {
  id: string
  user: string
  /** 任务类别（定时/普通/闲时）。 */
  kind: TaskKind
  /** 执行体（脚本/提示词）。 */
  runner: TaskRunner
  name?: string
  /** runner=script：shell 命令（在任务资源目录执行）。 */
  script?: string
  /** runner=prompt：触发 agent 运行的提示词。 */
  prompt?: string
  /** kind=scheduled：执行表达式（5 段 cron / @every 30m / @daily / @at <时间>）。 */
  schedule?: string
  /** kind=scheduled：IANA 时区（如 Asia/Shanghai；缺省服务器本地时区）。 */
  timezone?: string
  /** kind=scheduled：停机错过补跑策略（缺省 skip）。 */
  misfire?: TaskMisfire
  /** runner=prompt 执行目标（缺省 ephemeral）。 */
  target?: TaskTarget
  /** target=session 绑定的会话（缺省=创建来源 originSessionId）。 */
  sessionId?: string
  /** target=sticky 的专用会话 id（跨次复用）。 */
  stickySessionId?: string
  /** target=ephemeral/sticky 的预载子Agent 名单。 */
  agents?: string[]
  /** 单次执行超时（缺省 script 5 分钟 / prompt 30 分钟 / idle 3 小时）。 */
  timeoutMs?: number
  /** 任务级环境变量（持久化于任务定义）：脚本型注入子进程环境、提示词型注入执行会话的任务 env，均**高于**继承的会话/全局环境。 */
  env?: Record<string, string>
  /** 通知通道（可配多条）。 */
  notify?: TaskNotifyChannel[]
  /** 通知时机：auto=执行结束自动发（缺省）/ model=由模型经 task_notify 主动发。 */
  notifyOn?: TaskNotifyWhen
  /** 连续失败自动停用阈值（缺省 0=不停用）。 */
  maxConsecutiveErrors?: number
  /** 创建来源会话（脚本结果消息写回目标；target=session 未显式指定时的缺省绑定会话）。 */
  originSessionId?: string
  /** 绑定的用户级待办 id（由待办「闲时自动执行」生成的闲时任务携带；与待办双向联动）。 */
  todoId?: string
  /** 一次性任务（待办手动执行生成，执行结束自动删除；用户不可见）。 */
  ephemeral?: boolean
  enabled: boolean
  createdAt: number
  updatedAt: number
  /** 运行态（持久化：queued 重启后重新入队，running 重启后标记中断）。 */
  state: TaskQueueState
  /** 排队信息（state=queued 时有效）。 */
  queue?: { source: TaskQueueSource; enqueuedAt: number; front?: boolean }
  /** 本次运行开始时刻（state=running 时有效）。 */
  startedAt?: number
  runCount: number
  lastRunAt?: number
  /** kind=scheduled：下次执行时间（到期即入队，入队后即推进）。 */
  nextRunAt?: number
  lastStatus?: TaskRunStatus
  lastOutput?: string
  lastError?: string
  /** 连续失败计数（成功清零，达 maxConsecutiveErrors 自动停用）。 */
  consecutiveErrors?: number
  /** 最近一次通知投递错误（通知失败不影响执行结果）。 */
  lastNotifyError?: string
}

/** 任务创建入参（task_add / REST 创建）。 */
export interface TaskCreateInput {
  kind?: TaskKind
  runner: TaskRunner
  name?: string
  script?: string
  prompt?: string
  schedule?: string
  timezone?: string
  misfire?: TaskMisfire
  target?: TaskTarget
  sessionId?: string
  agents?: string[]
  timeoutMs?: number
  /** 任务级环境变量（无人值守执行时注入，不依赖浏览器会话）。 */
  env?: Record<string, string>
  notify?: TaskNotifyInput[]
  notifyOn?: TaskNotifyWhen
  maxConsecutiveErrors?: number
  enabled?: boolean
  /** 绑定的用户级待办 id（由待办侧创建闲时任务时携带）。 */
  todoId?: string
  /** 一次性任务（执行结束自动删除）：待办手动执行生成。 */
  ephemeral?: boolean
  /** kind=manual：创建即入队（缺省 true）。 */
  runNow?: boolean
  /** 入队时置顶（普通任务插队）。 */
  front?: boolean
}

/** 任务更新补丁（task_update / REST 修改；字段可选，未提供不变）。 */
export interface TaskUpdateInput {
  name?: string
  enabled?: boolean
  runner?: TaskRunner
  script?: string
  prompt?: string
  schedule?: string
  timezone?: string
  misfire?: TaskMisfire
  target?: TaskTarget
  sessionId?: string
  agents?: string[]
  timeoutMs?: number
  /** 任务级环境变量（传空对象 {} 清除，未提供不改动）。 */
  env?: Record<string, string>
  notify?: TaskNotifyInput[]
  notifyOn?: TaskNotifyWhen
  maxConsecutiveErrors?: number
}

/** 手动执行的入队结果。 */
export interface TaskRunHandle {
  task: Task
  queued: boolean
  /** 队列内位置（1 起）。 */
  position?: number
  /** 未入队原因（任务已在队列/运行中）。 */
  reason?: string
}

/** 任务资源目录条目（GET /api/v1/tasks/:id/files）。 */
export interface TaskFileEntry {
  /** 相对任务目录的路径（POSIX 分隔符）。 */
  path: string
  size: number
  mtimeMs: number
  dir: boolean
}

/** 任务服务契约（ToolContext.tasks 注入；引擎调度器实现，子代理消费）。 */
export interface TaskService {
  add: (input: TaskCreateInput, originSessionId?: string) => Promise<Task>
  list: () => Promise<Task[]>
  get: (id: string) => Promise<Task | null>
  remove: (id: string) => Promise<boolean>
  update: (id: string, patch: TaskUpdateInput) => Promise<Task | null>
  /** 手动执行：入队（可置顶），返回排队位置。 */
  run: (id: string, opts?: { front?: boolean }) => Promise<TaskRunHandle | null>
  /** 出队（排队中取消，不终止运行中的任务）。 */
  cancel: (id: string) => Promise<boolean>
  /** 终止运行中的任务（取消其执行会话/超时收尾）。 */
  stop: (id: string) => Promise<boolean>
  /** 队列视图（额度、排队顺序、运行中）。 */
  queue: () => Promise<TaskQueueView>
  /** 读取执行记录（新→旧，最多 limit 条，缺省上限；存于 `task-runs/{taskId}/{时间}.json`）。 */
  runs: (id: string, limit?: number) => Promise<TaskRunRecord[]>
  /** 主动推送通知（模型决定内容与时机）：投递到任务配置的通知通道（未配置回落全局默认通道）。
   *  id 缺省时按当前执行会话反查正在运行的任务（模型在执行任务时无需回显任务 ID）；
   *  投递目标限定为用户已配置的通道（不接受调用方传入任意 URL），安全模式拒绝。 */
  notify: (input: TaskNotifyMessage, id?: string) => Promise<TaskNotifyResult>
  /** 任务资源目录（脚本/文档）操作。 */
  files: (id: string) => Promise<TaskFileEntry[]>
  readFile: (id: string, path: string) => Promise<string>
  writeFile: (id: string, path: string, content: string) => Promise<TaskFileEntry>
  deleteFile: (id: string, path: string) => Promise<boolean>
}
