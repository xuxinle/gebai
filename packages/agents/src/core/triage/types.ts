/**
 * 两级研判（大小模型协同）的契约层：小模型批量粗筛（L1）→ 置信度分流 → 大模型精审兜底（L2）。
 *
 * 场景无关：`TriageItem.features` 由调用方的**特征提取适配器**产出（领域数据 → 统一特征描述），
 * 本模块只认 items/schema/阈值/L2 执行器这几样，任何"先快速分流、再重点深挖"的场景共用同一套代码。
 *
 * 与执行环境解耦：L1 只依赖一个 OpenAI 兼容端点（本机小模型 / 局域网 / 云端），
 * L2 只依赖注入的 `TriageL2Runner`（会话内走子会话、REST 走 Agent 引擎），本模块不碰引擎与会话。
 */

/** 待研判条目（适配器产出；id 是断点续跑与结果关联的主键）。 */
export interface TriageItem {
  id: string
  /** 条目标题（可选；仅用于 L2 提示词与结果可读性）。 */
  title?: string
  /**
   * 统一特征描述：摘要 + 关键字段表 + 时间线/序列 + 文本片段（建议 ≤500 token）。
   * **必须包含能区分阶段的证据**（如时间线），否则模型会基于残缺信息自信地误判。
   */
  features: string
  /** 业务侧透传字段（原样回到结果，便于关联输入）。 */
  meta?: Record<string, unknown>
}

/** L1 原始产出（保留在最终结果里，供审计与 L1/L2 质量校准对比）。 */
export interface L1Record {
  ok: boolean
  confidence: number
  result?: unknown
  reason?: string
  evidence_index?: string[]
  error?: string
  /** 工具式输出：消耗的提醒次数。 */
  tool_reminders_used?: number
  /** 工具式输出：提醒耗尽仍未调用输出工具（强制进 L2 的信号之一）。 */
  tool_call_missing?: boolean
}

/** 取证链条目（L2 专属）：哪个工具、发现了什么关键事实。 */
export interface EvidenceLink {
  tool: string
  finding: string
}

/** 单条研判结果（L1 采纳与 L2 精审定案共用同一形态）。 */
export interface TriageResult {
  id: string
  title?: string
  /** 结论产出层：`L1` = 粗筛即采纳；`L2` = 精审定案（含 L1 结论被修正的情形）。 */
  layer: "L1" | "L2"
  ok: boolean
  /** 业务结果（调用方 schema 的形状）。 */
  result?: unknown
  /** 置信度 0~1（信息不足为 0）。 */
  confidence: number
  reason?: string
  evidence_index?: string[]
  /** L2 具备：取证链。 */
  evidence_chain?: EvidenceLink[]
  /** L2 具备：是否推翻/修正了 L1 结论（衡量 L1 质量的关键指标）。 */
  revised?: boolean
  /** L2 具备：采纳 | 修正 | 证据不足 | 误判。 */
  action?: string
  /** L1 初判（审计用）。 */
  l1?: L1Record
  /** 产出结论的模型/执行标识（追溯用）。 */
  model?: string
  error?: string
  latency_ms?: number
  meta?: Record<string, unknown>
}

/** 分流去向。 */
export type TriageVerdict =
  /** L1 置信度达标（或命中白名单）→ 直接采纳。 */
  | "adopted"
  /** 低置信度 / 0 置信度 / 证据为空 / 未调用输出工具 / 解析失败 → 交 L2 精审。 */
  | "escalate"

export interface TriageProgress {
  phase: "l1" | "l2" | "done"
  done: number
  total: number
  adopted: number
  escalated: number
  failed: number
}

export interface TriageSummary {
  job_id: string
  job_dir: string
  total: number
  /** 小模型即定案的条数（置信度达标采纳；`escalate=false` 时含低置信度直出）。 */
  adopted: number
  /** 转 L2 精审的条数（O(k)，k ≪ N 是这套模式的价值所在）。 */
  escalated: number
  /** L2 定案的条数。 */
  reviewed: number
  /** 转 L2 但本次未精审的条数（超 maxItems / 未启用 L2 / 被取消）——待下次带更大 maxItems 续跑。 */
  pending_review: number
  /** 最终失败（已交 L2 精审但没拿到该条结论）。 */
  failed: number
  /** L2 修正 L1 初判的条数（L1 质量校准依据）。 */
  revised: number
  elapsed_ms: number
  results: TriageResult[]
  notes: string[]
}

/** L2 精审执行器（由调用方注入：会话内 = 子会话运行；REST = Agent 引擎会话）。返回该轮的最终正文。 */
export type TriageL2Runner = (req: {
  prompt: string
  agents?: string[]
  model?: string
  /** 独立端点（支持 env 覆盖的通道据此落实；不支持的通道须显式报错而非静默忽略）。 */
  apiBase?: string
  apiKey?: string
  timeoutMs?: number
}) => Promise<string>

/** L1（小模型端）配置。 */
export interface TriageL1Options {
  /** OpenAI 兼容端点（本机小模型 / 局域网 / 云端）。 */
  baseUrl: string
  /** 端点鉴权头（远端）。 */
  headers?: Record<string, string>
  /** 模型名（端点需要时）。 */
  model?: string
  /** 系统提示（缺省内置：强调"信息不足给 0 置信度"）。 */
  system?: string
  /** 单条用户提示模板（占位符 `{id}` / `{title}` / `{features}` / `{labels}`）。 */
  promptTemplate?: string
  /** 并发度（缺省 1；仅在短 prompt + slot 匹配时有收益）。 */
  concurrency?: number
  maxTokens?: number
  temperature?: number
  /** 工具式输出：未调用输出工具时的提醒次数上限（缺省 3）。 */
  reminders?: number
  timeoutMs?: number
  /** 思维链开关（缺省 false——思考链会吃光输出预算）。 */
  enableThinking?: boolean
  fetchImpl?: typeof fetch
}

/** L2（大模型端）配置。 */
export interface TriageL2Options {
  /** 执行器（必填；不注入则不启用 L2）。 */
  runner: TriageL2Runner
  /** 是否启用（缺省：给了 runner 即启用）。 */
  enabled?: boolean
  /** 精审时装载的领域子Agent（会话/子会话形态下生效）。 */
  agents?: string[]
  /** 模型/路由名（会话/子会话形态下生效）。 */
  model?: string
  /** 精审模型的独立端点（缺省沿用当前会话/服务的主模型端点；不支持的通道必须显式报错，不静默忽略）。 */
  apiBase?: string
  /** 精审模型端点的鉴权密钥（配合 apiBase）。 */
  apiKey?: string
  /** 单次研判最多精审多少条（缺省 20；其余按"证据不足"标记，避免无界成本）。 */
  maxItems?: number
  /** 每轮精审送入多少条（缺省 5；条目多时分轮）。 */
  batchSize?: number
  /** L2 系统提示（缺省内置精审骨架）。 */
  system?: string
  timeoutMs?: number
}

export interface TriageOptions {
  user: string
  /** 数据根（缺省 `GEBAI_HOME` 环境变量；仅用于推导缺省 jobDir）。 */
  home?: string
  items: TriageItem[]
  /**
   * 业务结果 schema。引擎会自动外包一层分析信封（confidence/reason/evidence_index）；
   * 若该 schema 顶层已含 `confidence`，则不再包装、直接使用（尊重调用方的完整定义）。
   */
  schema?: Record<string, unknown>
  /** 业务标签枚举（注入 schema 的 `label.enum`；无 schema 时构成缺省业务 schema）。 */
  labelEnum?: string[]
  l1: TriageL1Options
  /** 置信度阈值（缺省 0.85）。 */
  threshold?: number
  /**
   * 证据质量下限：至少一条证据达到该长度才算有效（缺省 6 字符，0 = 关闭）。
   * 用于挡住弱模型把「任务失败」这类无信息量文本当证据、从而以高置信度击穿「证据非空」防线的情形。
   */
  minEvidenceChars?: number
  /** 白名单标签：命中即直接采纳，不看置信度（如"无异常"）。 */
  acceptLabels?: string[]
  /**
   * 低置信度是否上升启用 agent 会话兜底（缺省 true）。
   * `false` = 只跑小模型：低置信度结论按原样输出（置信度如实偏低），不标待精审/失败。
   */
  escalate?: boolean
  l2?: TriageL2Options
  /** 落盘目录（缺省 `{GEBAI_HOME}/users/{user}/triage/{job_id}`）。 */
  jobDir?: string
  /** 任务标识（缺省按时间戳生成；同 job_id 重复调用按 id 跳过已完成的 L1）。 */
  jobId?: string
  onResult?: (r: TriageResult) => void | Promise<void>
  onProgress?: (p: TriageProgress) => void | Promise<void>
  signal?: AbortSignal
}
