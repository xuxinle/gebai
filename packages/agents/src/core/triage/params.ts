/**
 * 两级研判的**参数契约单一来源**：会话内工具（`triage_run`）与 REST 接口（`POST /api/v1/triage/analyze`）
 * 共用这一份字典做参数声明、别名归一与校验——两条通道的参数名、缺省值、能力边界与报错文案因此不可能漂移。
 *
 * **规范形态**：扁平 snake_case —— `items` / `items_file` / `target` / `schema` / `threshold` / `escalate`
 * 加 `l1_*` / `l2_*` 细项。同时接受嵌套写法（`l1.target` / `l2.enabled`）与既有别名作为兼容输入，
 * 归一为同一份 `TriageParams`。
 *
 * **通道差异只有一处**：`mode`（同步 / 后台）是 REST 的外部调用能力，工具调用天然同步——
 * 故它由 REST 路由单独声明，不进本字典。其余参数两条通道完全一致。
 */
import { existsSync, readFileSync } from "node:fs"
import { isAbsolute, resolve } from "node:path"
import { resolveTarget, targetHeaders, type InferTarget } from "../../agents/local_infer/providers"
import { defaultJobDir } from "./pipeline"
import type { TriageItem, TriageL2Runner, TriageOptions } from "./types"

// ── 缺省值（两条通道同源） ────────────────────────────────────────────────

/** 低置信度是否上升启用 agent 会话兜底（缺省开）。 */
export const TRIAGE_DEFAULT_ESCALATE = true
/** 结果数组返回条数（0 = 全量；结论数组默认完整返回，不做静默截断）。 */
export const TRIAGE_DEFAULT_RESULT_LIMIT = 0
/** 工具的**文本**呈现上限（结论数组本身不受此限，另见 `data.results` 与 `triage_view`）。 */
export const TRIAGE_TEXT_RENDER_MAX = 100

// ── 参数表（工具 schema 与接口校验的唯一来源） ─────────────────────────────

export interface TriageParamSpec {
  name: string
  type: "string" | "number" | "boolean" | "object" | "array"
  items?: Record<string, unknown>
  description: string
  /** 兼容输入：既有扁平别名与嵌套写法（先命中者生效）。 */
  aliases?: string[]
}

export const TRIAGE_PARAM_SPECS: TriageParamSpec[] = [
  {
    name: "items",
    type: "array",
    items: { type: "object" },
    description:
      "全量信息数组，元素 { id, title?, features, meta? }。features 是用统一特征描述（摘要 + 关键字段 + 时间线/序列 + 文本片段，" +
      "建议 ≤500 token）——**必须含能区分阶段的证据**，否则小模型会基于残缺信息给出高置信度的错误结论。",
  },
  {
    name: "items_file",
    type: "string",
    description: "条目文件（JSONL 每行一条，或 JSON 数组；与 items 二选一）",
  },
  {
    name: "target",
    type: "string",
    aliases: ["l1_target", "l1.target", "l1.base_url"],
    description:
      '推理目标：缺省 local（本机受管推理服务，按服务状态文件定位端口）；也可为命名目标（LOCAL_INFER_TARGETS 里声明的 name）' +
      '或直连 URL（"http://192.168.1.20:8080"，任意 OpenAI 兼容端点）。',
  },
  {
    name: "target_api_key",
    type: "string",
    aliases: ["l1_api_key", "l1.api_key"],
    description: "推理目标鉴权密钥（远端需要时；不传则用命名目标的 api_key 字段或 LOCAL_INFER_REMOTE_API_KEY）",
  },
  {
    name: "schema",
    type: "object",
    description:
      "结构化输出 schema（描述「结论长什么样」）。引擎自动外包 confidence/reason/evidence_index 信封；" +
      "若该 schema 顶层已含 confidence，则原样使用、不再包装。",
  },
  {
    name: "label_enum",
    type: "array",
    items: { type: "string" },
    description: "业务标签枚举（注入结果 schema 的 label.enum；无 schema 时构成缺省业务结果）",
  },
  {
    name: "threshold",
    type: "number",
    description: "置信度阈值（0~1，缺省 0.85）：≥ 阈值且证据足够才由小模型直接采纳，否则按 escalate 处置",
  },
  {
    name: "min_evidence_chars",
    type: "number",
    description:
      "证据质量下限（缺省 6 字符，0 = 关闭）：至少一条证据达该长度才算有效——挡住弱模型把「任务失败」这类无信息量文本当证据、以高置信度击穿「证据非空」防线。",
  },
  {
    name: "accept_labels",
    type: "array",
    items: { type: "string" },
    description: "白名单标签：命中即直接采纳（不看置信度），如「无异常」",
  },
  {
    name: "escalate",
    type: "boolean",
    aliases: ["l2_enabled", "l2.enabled"],
    description:
      "低置信度是否上升启用 agent 会话兜底（缺省 true）：true = 交大模型精审（可装载领域子Agent 主动取证并给证据链）；" +
      "false = 仅小模型直接输出——低置信度结论按原样返回，置信度如实偏低，不标失败。",
  },
  {
    name: "l1_model",
    type: "string",
    aliases: ["l1.model"],
    description: "小模型名（端点需要 model 字段时；缺省用推理目标自带的模型名）",
  },
  {
    name: "l1_system",
    type: "string",
    aliases: ["l1.system"],
    description: "小模型系统提示（缺省内置：正反两面写清「有线索就给结论、确实无线索才给 0 置信度」）",
  },
  {
    name: "l1_prompt_template",
    type: "string",
    aliases: ["l1.prompt_template"],
    description: "单条提示模板（占位符 {id} / {title} / {features} / {labels}）",
  },
  {
    name: "l1_concurrency",
    type: "number",
    aliases: ["l1.concurrency"],
    description: "小模型并发度（缺省 1；仅在短 prompt + slot 数匹配时有收益，CPU 上并发无增益）",
  },
  {
    name: "l1_max_tokens",
    type: "number",
    aliases: ["l1.max_tokens"],
    description: "单条最大输出 token（缺省 300）",
  },
  {
    name: "l1_temperature",
    type: "number",
    aliases: ["l1.temperature"],
    description: "采样温度（缺省 0 = 确定性输出，批量研判应保持 0）",
  },
  {
    name: "l1_reminders",
    type: "number",
    aliases: ["l1.reminders"],
    description: "未调用输出工具时的提醒次数上限（缺省 3；提醒耗尽则该条失败并强制上升兜底）",
  },
  {
    name: "l1_timeout_ms",
    type: "number",
    aliases: ["l1.timeout_ms"],
    description: "单条请求超时毫秒数（缺省 600000）",
  },
  {
    name: "l1_enable_thinking",
    type: "boolean",
    aliases: ["enable_thinking", "l1.enable_thinking"],
    description: "是否启用思维链（缺省 false——思考链会吃光输出预算导致正文为空）",
  },
  {
    name: "l2_agents",
    type: "array",
    items: { type: "string" },
    aliases: ["l2.agents"],
    description: "兜底精审时装载的领域子Agent（如 code / wps / nsight，用于主动取证）",
  },
  {
    name: "l2_model",
    type: "string",
    aliases: ["l2.model"],
    description: "兜底模型/路由名（GEBAI_LLM_ROUTES 的命名路由或字面模型名；缺省沿用当前会话/服务的模型）",
  },
  {
    name: "l2_api_base",
    type: "string",
    aliases: ["l2.api_base"],
    description: "兜底模型的独立端点（缺省沿用当前会话/服务的主模型端点）",
  },
  {
    name: "l2_api_key",
    type: "string",
    aliases: ["l2.api_key"],
    description: "兜底模型端点的鉴权密钥（配合 l2_api_base）",
  },
  {
    name: "l2_system",
    type: "string",
    aliases: ["l2.system"],
    description: "兜底精审的系统提示（缺省内置精审骨架：主动取证、允许说「证据不足」、可推翻初判）",
  },
  {
    name: "l2_max_items",
    type: "number",
    aliases: ["l2.max_items"],
    description: "单次最多精审多少条（缺省 20；超出部分标记「待精审」，可用同 job_id 调大后续跑）",
  },
  {
    name: "l2_batch_size",
    type: "number",
    aliases: ["l2.batch_size"],
    description: "每轮精审多少条（缺省 5）",
  },
  {
    name: "l2_timeout_ms",
    type: "number",
    aliases: ["l2.timeout_ms"],
    description: "单项精审超时毫秒数（缺省 600000；超时走收尾而非硬杀）",
  },
  {
    name: "job_id",
    type: "string",
    description: "任务标识（缺省自动生成；同 job_id 续跑会复用已有小模型结果，不重复请求）",
  },
  {
    name: "job_dir",
    type: "string",
    description: "落盘目录（缺省 {GEBAI_HOME}/users/<user>/triage/<job_id>）",
  },
  {
    name: "result_limit",
    type: "number",
    description: "结果数组返回条数（缺省 0 = 全量；>0 时截断前 N 条，total 始终为实际条数）",
  },
  {
    name: "mode",
    type: "string",
    description:
      "执行模式：sync（缺省，本次调用等整批跑完）| async（后台执行，立即返回任务 id——用 bg_task 查进度/取结果/中止）。" +
      "异步与同步的参数与执行完全相同，只是不等结果；结论始终逐条落盘在 job_dir，因此即使后台任务记录不可用（如服务重启），仍可用 triage_view 读产物。",
  },
]

/** 规范参数名清单（一致性测试与文档用）。 */
export const TRIAGE_PARAM_NAMES: string[] = TRIAGE_PARAM_SPECS.map((s) => s.name)

/** 工具 schema 的 properties（从参数表生成——两条通道的参数说明同源）。 */
export function triageToolProperties(): Record<string, unknown> {
  const out: Record<string, unknown> = {}
  for (const s of TRIAGE_PARAM_SPECS) {
    out[s.name] = {
      type: s.type,
      ...(s.items ? { items: s.items } : {}),
      description: s.description,
    }
  }
  return out
}

/** 按键取值：规范名优先，其次别名（支持 `a.b` 嵌套路径）。 */
function pick(raw: Record<string, unknown>, spec: TriageParamSpec): unknown {
  for (const key of [spec.name, ...(spec.aliases ?? [])]) {
    let cur: unknown = raw
    for (const part of key.split(".")) {
      cur = cur && typeof cur === "object" ? (cur as Record<string, unknown>)[part] : undefined
    }
    if (cur !== undefined && cur !== null && cur !== "") return cur
  }
  return undefined
}

const asNum = (v: unknown): number | undefined => (typeof v === "number" && Number.isFinite(v) ? v : undefined)
const asBool = (v: unknown): boolean | undefined => (typeof v === "boolean" ? v : undefined)
const asStr = (v: unknown): string | undefined => (typeof v === "string" && v.trim() ? v.trim() : undefined)
const asObj = (v: unknown): Record<string, unknown> | undefined =>
  v && typeof v === "object" && !Array.isArray(v) ? (v as Record<string, unknown>) : undefined
const asStrList = (v: unknown): string[] | undefined =>
  Array.isArray(v) ? v.filter((x): x is string => typeof x === "string" && x.trim() !== "") : undefined

// ── 归一结果 ──────────────────────────────────────────────────────────────

export interface TriageL1Params {
  model?: string
  system?: string
  promptTemplate?: string
  concurrency?: number
  maxTokens?: number
  temperature?: number
  reminders?: number
  timeoutMs?: number
  enableThinking?: boolean
}

export interface TriageL2Params {
  agents?: string[]
  model?: string
  apiBase?: string
  apiKey?: string
  system?: string
  maxItems?: number
  batchSize?: number
  timeoutMs?: number
}

export interface TriageParams {
  items: TriageItem[]
  schema?: Record<string, unknown>
  labelEnum?: string[]
  threshold?: number
  minEvidenceChars?: number
  acceptLabels?: string[]
  /** 低置信度是否上升启用 agent 会话兜底。 */
  escalate: boolean
  target?: string
  targetApiKey?: string
  l1: TriageL1Params
  l2: TriageL2Params
  jobId?: string
  jobDir?: string
  resultLimit: number
  /** 执行模式（缺省 `sync`）：`sync` = 本次调用等整批跑完；`async` = 后台执行并立即返回任务 id。 */
  mode: "sync" | "async"
}

// ── 条目归一与校验（两条通道同一份报错） ──────────────────────────────────

/** 条目数组归一：缺 id 用序号补；features 缺失留空串由 validateItems 统一报错。 */
export function normalizeItems(v: unknown): { items: TriageItem[] } | { error: string } {
  if (!Array.isArray(v)) return { error: "items 必须是数组（元素 { id, title?, features, meta? }）" }
  const items: TriageItem[] = []
  for (const [i, raw] of v.entries()) {
    if (!raw || typeof raw !== "object" || Array.isArray(raw)) return { error: `items 第 ${i + 1} 条不是对象` }
    const o = raw as Record<string, unknown>
    items.push({
      id: asStr(o.id) ?? String(i),
      title: asStr(o.title),
      features: typeof o.features === "string" ? o.features : "",
      meta: asObj(o.meta),
    })
  }
  return { items }
}

/** 条目校验：条目非空、features 非空——报错里带上「features 该写成什么」的修复方向。 */
export function validateItems(items: TriageItem[]): { error?: string } {
  if (!items.length) return { error: "需要 items（全量信息数组）或 items_file（JSONL/JSON 数组）之一，且至少一条。" }
  const bad = items.filter((it) => !it.features.trim())
  if (bad.length) {
    return {
      error:
        `${bad.length} 条缺少 features（统一特征描述），例如 ${bad.slice(0, 3).map((b) => b.id).join("、")}。` +
        "features 是适配器的产出：把领域数据拼成「摘要 + 关键字段 + 时间线/序列 + 文本片段」，必须含能区分阶段的证据。",
    }
  }
  return {}
}

/** 解析条目文件文本（JSONL 每行一条，或 JSON 数组）；不抛异常。 */
export function parseItemsText(text: string): { items: TriageItem[] } | { error: string } {
  const trimmed = text.trim()
  let raw: unknown[]
  if (trimmed.startsWith("[")) {
    try {
      const arr = JSON.parse(trimmed) as unknown
      if (!Array.isArray(arr)) return { error: "条目文件是 JSON 但不是数组" }
      raw = arr
    } catch (e) {
      return { error: `条目文件不是合法 JSON 数组：${(e as Error).message}` }
    }
  } else {
    raw = []
    const bad: number[] = []
    trimmed.split(/\r?\n/).forEach((line, i) => {
      const s = line.trim()
      if (!s || s.startsWith("#") || s.startsWith("//")) return
      try {
        raw.push(JSON.parse(s) as unknown)
      } catch {
        bad.push(i + 1)
      }
    })
    if (bad.length) {
      return { error: `条目文件第 ${bad.slice(0, 5).join("/")}${bad.length > 5 ? " 等" : ""} 行不是合法 JSON（JSONL 要求每行一条）` }
    }
  }
  return normalizeItems(raw)
}

// ── 归一化 ────────────────────────────────────────────────────────────────

export interface NormalizeOptions {
  /** items_file 相对路径的基准目录（工具 = 会话工作目录，接口 = 服务进程 cwd）。 */
  itemsBaseDir: string
  /** 读文本（工具传 ctx 侧读取器以走宿主守卫；缺省 node:fs）。 */
  readText?: (absPath: string) => string
}

/**
 * 归一调用参数：把扁平规范名、嵌套写法与既有别名收敛为一份 `TriageParams`，
 * 并做跨通道一致的校验（条目、阈值范围）。
 */
export function normalizeTriageParams(
  raw: Record<string, unknown>,
  o: NormalizeOptions,
): { params: TriageParams } | { error: string } {
  const of = (name: string): unknown => {
    const spec = TRIAGE_PARAM_SPECS.find((s) => s.name === name)
    return spec ? pick(raw, spec) : undefined
  }

  // ① 条目：内联数组优先，其次条目文件
  let items: TriageItem[] = []
  const inline = of("items")
  if (inline !== undefined) {
    const got = normalizeItems(inline)
    if ("error" in got) return { error: got.error }
    items = got.items
  }
  const itemsFile = asStr(of("items_file"))
  if (!items.length && itemsFile) {
    const abs = isAbsolute(itemsFile) ? itemsFile : resolve(o.itemsBaseDir, itemsFile)
    if (!existsSync(abs)) return { error: `条目文件不存在：${abs}` }
    let text: string
    try {
      text = (o.readText ?? ((p: string) => readFileSync(p, "utf-8")))(abs)
    } catch (e) {
      return { error: `条目文件读取失败：${(e as Error).message}` }
    }
    const got = parseItemsText(text)
    if ("error" in got) return { error: got.error }
    items = got.items
  }
  const itemCheck = validateItems(items)
  if (itemCheck.error) return { error: itemCheck.error }

  // ② 阈值与证据下限
  const threshold = asNum(of("threshold"))
  if (threshold !== undefined && (threshold < 0 || threshold > 1)) {
    return { error: `threshold 必须在 0~1（收到 ${threshold}）` }
  }
  const minEvidenceChars = asNum(of("min_evidence_chars"))
  if (minEvidenceChars !== undefined && minEvidenceChars < 0) {
    return { error: `min_evidence_chars 不能为负（收到 ${minEvidenceChars}）` }
  }

  const resultLimitRaw = asNum(of("result_limit"))
  return {
    params: {
      items,
      schema: asObj(of("schema")),
      labelEnum: asStrList(of("label_enum")),
      threshold,
      minEvidenceChars,
      acceptLabels: asStrList(of("accept_labels")),
      escalate: asBool(of("escalate")) ?? TRIAGE_DEFAULT_ESCALATE,
      target: asStr(of("target")),
      targetApiKey: asStr(of("target_api_key")),
      l1: {
        model: asStr(of("l1_model")),
        system: asStr(of("l1_system")),
        promptTemplate: asStr(of("l1_prompt_template")),
        concurrency: asNum(of("l1_concurrency")),
        maxTokens: asNum(of("l1_max_tokens")),
        temperature: asNum(of("l1_temperature")),
        reminders: asNum(of("l1_reminders")),
        timeoutMs: asNum(of("l1_timeout_ms")),
        enableThinking: asBool(of("l1_enable_thinking")),
      },
      l2: {
        agents: asStrList(of("l2_agents")),
        model: asStr(of("l2_model")),
        apiBase: asStr(of("l2_api_base")),
        apiKey: asStr(of("l2_api_key")),
        system: asStr(of("l2_system")),
        maxItems: asNum(of("l2_max_items")),
        batchSize: asNum(of("l2_batch_size")),
        timeoutMs: asNum(of("l2_timeout_ms")),
      },
      jobId: asStr(of("job_id")),
      jobDir: asStr(of("job_dir")),
      mode: asStr(of("mode")) === "async" ? "async" : "sync",
      resultLimit: resultLimitRaw !== undefined && resultLimitRaw > 0 ? Math.floor(resultLimitRaw) : TRIAGE_DEFAULT_RESULT_LIMIT,
    },
  }
}

// ── 端点解析与 options 组装（两条通道同源） ────────────────────────────────

/** 解析推理目标：`local` 走服务状态文件，命名目标走 LOCAL_INFER_TARGETS，URL 直连。 */
export function resolveL1Endpoint(
  params: TriageParams,
  env: Record<string, string>,
): InferTarget | { error: string } {
  return resolveTarget(params.target, { env }, params.targetApiKey)
}

export interface ToOptionsInput {
  user: string
  home: string
  /** 目标解析用的环境（工具 = 会话 env；接口 = 进程 env + GEBAI_HOME）。 */
  env: Record<string, string>
  /** 兜底执行器：会话内 = 隔离子会话；接口 = Agent 引擎会话。 */
  l2Runner?: TriageL2Runner
  signal?: AbortSignal
}

/**
 * 组合 `runTriage` 的选项：两条通道唯一的差异是注入的 `l2Runner`，其余映射完全同源。
 * 同时回传解析后的推理目标（供调用方报告「结果出自哪个端点」）。
 *
 * `escalate=false` 时不注入 runner（只跑小模型，低置信度结论按原样输出）；
 * `escalate=true` 但当前通道没有可用执行器时**显式报错**，不静默降级成"小模型直出"。
 */
export function toTriageOptions(
  params: TriageParams,
  o: ToOptionsInput,
): { options: TriageOptions; endpoint: InferTarget } | { error: string } {
  const endpoint = resolveL1Endpoint(params, o.env)
  if ("error" in endpoint) return { error: endpoint.error }
  if (params.escalate && !o.l2Runner) {
    return { error: "已要求低置信度上升兜底（escalate=true），但当前通道没有可用的精审执行器。" }
  }

  // 端点覆盖：兜底模型可指向独立端点（接口通道经会话 env 落实；工具通道的能力边界见 triage.ts）
  const l2 = params.escalate
    ? {
        runner: o.l2Runner!,
        agents: params.l2.agents,
        model: params.l2.model,
        apiBase: params.l2.apiBase,
        apiKey: params.l2.apiKey,
        system: params.l2.system,
        maxItems: params.l2.maxItems,
        batchSize: params.l2.batchSize,
        timeoutMs: params.l2.timeoutMs,
      }
    : undefined

  return {
    endpoint,
    options: {
      user: o.user,
      home: o.home,
      items: params.items,
      schema: params.schema,
      labelEnum: params.labelEnum,
      threshold: params.threshold,
      minEvidenceChars: params.minEvidenceChars,
      acceptLabels: params.acceptLabels,
      escalate: params.escalate,
      l1: {
        baseUrl: endpoint.baseUrl,
        headers: targetHeaders(endpoint),
        model: params.l1.model ?? endpoint.model,
        system: params.l1.system,
        promptTemplate: params.l1.promptTemplate,
        concurrency: params.l1.concurrency,
        maxTokens: params.l1.maxTokens,
        temperature: params.l1.temperature,
        reminders: params.l1.reminders,
        timeoutMs: params.l1.timeoutMs,
        enableThinking: params.l1.enableThinking,
      },
      l2,
      jobDir: params.jobDir ?? (params.jobId ? defaultJobDir(o.home, o.user, params.jobId) : undefined),
      jobId: params.jobId,
      signal: o.signal,
    },
  }
}
