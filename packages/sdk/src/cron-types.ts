/**
 * 定时任务数据类型（引擎调度器与 cron 子代理的共享契约）。
 * 数据形状与引擎实现（server core/schedule/cron.ts）结构兼容——服务类实现这些接口，
 * 子代理只经 ToolContext.cron 契约接口消费，不依赖引擎内部模块。
 */

/** 通知通道类型：webhook=通用 HTTP 回调；feishu=飞书群自定义机器人 webhook；feishu_chat=飞书应用消息（chat_id）。 */
export type CronNotifyType = "webhook" | "feishu" | "feishu_chat"

/** @ 人配置：id 为 open_id（ou_/un_/on_ 前缀）或 "all"（@所有人）；name 为展示名（缺省由客户端解析真实姓名）。 */
export interface FeishuAtTarget {
  id: string
  name?: string
}

/** 定时任务通知通道（任务内嵌配置，可配多条）。 */
export interface CronNotifyChannel {
  type: CronNotifyType
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
export type CronNotifyInput = Omit<CronNotifyChannel, "at"> & { at?: Array<string | FeishuAtTarget> }

export type CronTaskType = "script" | "prompt"
/** prompt 型执行目标：ephemeral=每次触发新建会话（缺省）；sticky=专用会话跨次复用；session=绑定既有会话。 */
export type CronTarget = "ephemeral" | "sticky" | "session"
/** 停机错过触发的补跑策略：skip=跳过从当前重算（缺省）；run=启动后立即补跑一次。 */
export type CronMisfire = "skip" | "run"

/** 单次运行历史记录。 */
export interface CronRunRecord {
  id: string
  /** 触发时间。 */
  at: number
  /** 结束时间。 */
  endedAt: number
  status: "success" | "error" | "skipped" | "timeout"
  durationMs: number
  output?: string
  error?: string
  /** prompt 型本次运行的会话（执行轨迹）。 */
  sessionId?: string
  /** 手动触发（cron_trigger / REST run）。 */
  manual?: boolean
}

/** 定时任务（用户级资源，持久化于 users/{user}/cron.json，与会话生命周期解耦）。 */
export interface CronTask {
  id: string
  user: string
  name?: string
  /** script=脚本运行；prompt=提示词运行 agent。 */
  type: CronTaskType
  /** 定时表达式：5 段 cron（分 时 日 月 周）或 @every 30m / @daily / @at <时间> 等。 */
  schedule: string
  /** type=script：shell 命令（在任务专属工作目录以用户环境执行）。 */
  script?: string
  /** type=prompt：触发 agent 运行的提示词。 */
  prompt?: string
  /** type=prompt 执行目标（缺省 ephemeral）。 */
  target?: CronTarget
  /** target=session 绑定的会话（缺省=创建来源 originSessionId）。 */
  sessionId?: string
  /** target=ephemeral/sticky 的预载子Agent 名单。 */
  agents?: string[]
  /** IANA 时区（如 Asia/Shanghai；缺省服务器本地时区）。 */
  timezone?: string
  /** 停机错过补跑策略（缺省 skip）。 */
  misfire?: CronMisfire
  /** 单次执行超时（缺省 script 5 分钟 / prompt 30 分钟）。 */
  timeoutMs?: number
  /** 通知通道（可配多条）。 */
  notify?: CronNotifyChannel[]
  /** 通知时机：always=每次（缺省）/ error=仅失败。 */
  notifyOn?: "always" | "error"
  /** 连续失败自动停用阈值（缺省 0=不停用）。 */
  maxConsecutiveErrors?: number
  /** 创建来源会话（脚本结果消息写回目标；target=session 未显式指定 sessionId 时的缺省绑定会话）。 */
  originSessionId?: string
  /** target=sticky 的专用会话 id（跨次复用）。 */
  stickySessionId?: string
  enabled: boolean
  createdAt: number
  updatedAt: number
  lastRunAt?: number
  nextRunAt: number
  runCount: number
  lastStatus?: "success" | "error" | "skipped" | "timeout"
  lastOutput?: string
  lastError?: string
  /** 连续失败计数（成功清零，达阈值自动停用）。 */
  consecutiveErrors?: number
  /** 最近运行历史（环形，新→旧）。 */
  runs?: CronRunRecord[]
  /** 最近一次通知投递错误（通知失败不影响执行结果）。 */
  lastNotifyError?: string
}

/** 定时任务创建入参（cron_add / REST 创建）。 */
export interface CronCreateInput {
  name?: string
  type: CronTaskType
  schedule: string
  script?: string
  prompt?: string
  target?: CronTarget
  sessionId?: string
  agents?: string[]
  timezone?: string
  misfire?: CronMisfire
  timeoutMs?: number
  notify?: CronNotifyChannel[]
  notifyOn?: "always" | "error"
  maxConsecutiveErrors?: number
  enabled?: boolean
}

/** 定时任务更新补丁（cron_update / REST 修改；字段可选，未提供不变）。 */
export interface CronUpdateInput {
  name?: string
  enabled?: boolean
  schedule?: string
  type?: CronTaskType
  script?: string
  prompt?: string
  target?: CronTarget
  sessionId?: string
  agents?: string[]
  timezone?: string
  misfire?: CronMisfire
  timeoutMs?: number
  notify?: CronNotifyChannel[]
  notifyOn?: "always" | "error"
  maxConsecutiveErrors?: number
}

/** 定时任务服务契约（ToolContext.cron 注入；引擎调度器实现，子代理消费）。 */
export interface CronService {
  add: (input: CronCreateInput, originSessionId?: string) => Promise<CronTask>
  list: () => Promise<CronTask[]>
  remove: (id: string) => Promise<boolean>
  update: (id: string, patch: CronUpdateInput) => Promise<CronTask | null>
  trigger: (id: string) => Promise<CronTask | null>
}
