/**
 * 两级研判管线：L1 小模型批量粗筛（结构化 + 置信度）→ 阈值分流 → L2 大模型精审兜底（证据链）。
 *
 * 设计取舍（实测经验，勿凭直觉推翻）：
 *   * **L1 必须关思维链**：推理型模型开着 thinking 会把 max_tokens 吃光导致正文为空。
 *   * **L1 走工具式输出**：schema 注册为 function tool、模型调用即产出结果——不依赖服务端对
 *     GBNF/response_format 的支持（异构端点通用），且"调用与否"本身可审计；未调用按提醒次数
 *     重新提醒，耗尽则如实失败并**强制转 L2**（采集失败的条目恰恰最可能是疑难条目，不能丢）。
 *   * **信息不足要给 0 置信度**：小模型只能用给定信息判断，说不出来龙去脉就该低置信——0 置信度
 *     与空证据一律进 L2，这是把"不知道"如实转化为待查项的关键。
 *   * **L2 是取证不是猜**：允许大模型输出「证据不足」，它必须给出工具取证链。
 *   * **成本结构**：整体 = 廉价的 O(N) + 昂贵的 O(k)，k ≪ N。
 */
import { existsSync, mkdirSync, readFileSync, appendFileSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { runBatch, extractJson, type BatchItem, type BatchResult } from "../../agents/local_infer/api"
import type {
  EvidenceLink,
  L1Record,
  TriageItem,
  TriageL1Options,
  TriageL2Options,
  TriageOptions,
  TriageResult,
  TriageSummary,
  TriageVerdict,
} from "./types"

// ── 常量 ──────────────────────────────────────────────────────────────────

export const TRIAGE_DEFAULT_THRESHOLD = 0.85
export const TRIAGE_DEFAULT_MAX_L2 = 20
export const TRIAGE_DEFAULT_L2_BATCH = 5
export const TRIAGE_DEFAULT_L1_MAX_TOKENS = 300
export const TRIAGE_DEFAULT_CONCURRENCY = 1

/** 信封字段：调用方 schema 未自带时由引擎统一附加。 */
export const TRIAGE_ENVELOPE_FIELDS = ["confidence", "reason", "evidence_index"] as const

export const DEFAULT_L1_SYSTEM =
  "你是批量研判的粗筛模型，对给定条目做快速分类。\n" +
  "判断尺度（两种情况要分开，不要一律给低置信度）：\n" +
  "  · 信息里已有能定位原因的线索（明确报错文本/异常类型/退出码/状态时间线的转折点）→ 据此给结论，置信度 0.8~0.95；\n" +
  "  · 信息确实无可用线索（只有“失败”二字、日志缺失或采集失败）→ 置信度填 0，并在理由里写明还缺什么。\n" +
  "不要因为“没有给出更多数据”就把有明确线索的条目也判成信息不足；也不要把“任务失败”这类无信息量的文本当成证据。\n" +
  "你必须调用输出工具提交结果，不要在正文里回答。"

export const DEFAULT_L1_PROMPT =
  "【条目】{id}{title}\n" +
  "【信息】\n{features}\n" +
  "{labels}" +
  "【判断步骤】\n" +
  "1) 先在信息里找能定位原因的线索：报错文本/异常类型/退出码/资源数值/状态时间线的转折点\n" +
  "2) 有线索 → 据此选标签，置信度给 0.8~0.95，并把支撑该判断的原文片段逐条填进证据索引\n" +
  "3) 确实无线索 → 标签取最接近的，置信度填 0，理由写明还缺什么信息\n" +
  "【硬性要求】\n" +
  "· 标签必须从【可选标签】里选一个，不要自造\n" +
  "· 证据索引必须是**能定位原因的原文片段**（如报错行、退出码、异常类型）；仅抄“任务失败”“failed”这类无信息量的文本不算证据，这种情况置信度必须填 0\n" +
  "· 绝不基于残缺信息编造高置信度结论；但也不要对已有明确线索的条目一律给 0\n" +
  "· 必须调用输出工具提交结果"

export const DEFAULT_L2_SYSTEM =
  "你是研判复核代理：对批量粗筛（L1）的初判结果做【证据链复核】。\n" +
  "1) 主动调用你可用的领域工具取证（查原始数据/日志/记录/上下文），不要只转述初判\n" +
  "2) 能确认的给出确定结论并列出证据链（工具名 + 关键发现原文）；不能确认的明确输出「证据不足」，并列出待查清单\n" +
  "3) 禁止无证据猜测；可以推翻或修正 L1 的判罚并说明原因\n" +
  "4) 只输出一个 JSON 对象，不要解释、不要代码块"

// ── L1 schema：信封包装 ────────────────────────────────────────────────────

/** 调用方 schema 顶层是否自带 confidence（自带即视为完整定义，不再包信封）。 */
export function hasOwnConfidence(schema?: Record<string, unknown>): boolean {
  const props = schema?.properties as Record<string, unknown> | undefined
  return Boolean(props && typeof props === "object" && "confidence" in props)
}

/** 把标签枚举注入业务 schema 的 `label.enum`（有该属性才注入；否则由提示词承载）。 */
function withLabelEnum(schema: Record<string, unknown>, labelEnum?: string[]): Record<string, unknown> {
  if (!labelEnum?.length) return schema
  const props = schema.properties as Record<string, Record<string, unknown>> | undefined
  if (!props?.label || typeof props.label !== "object") return schema
  return { ...schema, properties: { ...props, label: { ...props.label, enum: labelEnum } } }
}

/**
 * 组出 L1 的最终 schema：调用方未自带 confidence 时外包分析信封。
 * 返回 `wrapped=false` 表示直接用了调用方 schema（此时信封字段按可选处理）。
 */
export function buildL1Schema(
  schema: Record<string, unknown> | undefined,
  labelEnum?: string[],
): { schema: Record<string, unknown>; wrapped: boolean } {
  const business = schema
    ? withLabelEnum(schema, labelEnum)
    : {
        type: "object",
        properties: {
          label: labelEnum?.length
            ? { type: "string", enum: labelEnum, description: "研判结论标签" }
            : { type: "string", description: "研判结论标签" },
        },
        required: ["label"],
      }

  if (hasOwnConfidence(schema)) {
    return { schema: business, wrapped: false }
  }

  return {
    wrapped: true,
    schema: {
      type: "object",
      properties: {
        result: business,
        confidence: { type: "number", minimum: 0, maximum: 1, description: "0~1；给定信息不足以判断时必须为 0" },
        reason: { type: "string", description: "判断依据：引用输入里的哪一条事实" },
        evidence_index: {
          type: "array",
          items: { type: "string" },
          description: "引用的输入事实（原文片段或字段名）；说不出来源时留空",
        },
      },
      required: ["result", "confidence", "reason", "evidence_index"],
    },
  }
}

// ── L1 提示词 ─────────────────────────────────────────────────────────────

export function buildL1Prompt(item: TriageItem, opts: { template?: string; labelEnum?: string[] }): string {
  const labels = opts.labelEnum?.length ? `【可选标签】${opts.labelEnum.join(" | ")}\n` : ""
  return (opts.template ?? DEFAULT_L1_PROMPT)
    .replaceAll("{id}", item.id)
    .replaceAll("{title}", item.title ? ` / ${item.title}` : "")
    .replaceAll("{features}", item.features)
    .replaceAll("{labels}", labels)
}

// ── L1 结果解析与分流 ──────────────────────────────────────────────────────

/** 从业务结果里取标签（用于白名单判定）。 */
function labelOf(result: unknown): string | undefined {
  if (!result || typeof result !== "object") return undefined
  const v = (result as Record<string, unknown>).label ?? (result as Record<string, unknown>).verdict
  return typeof v === "string" ? v : undefined
}

function num(v: unknown): number | undefined {
  return typeof v === "number" && Number.isFinite(v) ? v : undefined
}

/** 工具调用参数（或自带 confidence 的结果）→ L1 记录。 */
export function toL1Record(batch: BatchResult, wrapped: boolean): L1Record {
  const base: L1Record = {
    ok: false,
    confidence: 0,
    tool_reminders_used: batch.tool_reminders_used,
    tool_call_missing: batch.tool_call_missing,
  }
  if (!batch.ok || batch.json === undefined) {
    return { ...base, error: batch.error ?? `结构化校验未通过：${(batch.json_errors ?? []).join("; ")}` }
  }
  const obj = batch.json as Record<string, unknown>
  if (wrapped) {
    const confidence = num(obj.confidence)
    if (confidence == null) return { ...base, error: "输出缺少 confidence 字段" }
    return {
      ...base,
      ok: true,
      result: obj.result,
      confidence: Math.max(0, Math.min(1, confidence)),
      reason: typeof obj.reason === "string" ? obj.reason : undefined,
      evidence_index: Array.isArray(obj.evidence_index) ? obj.evidence_index.filter((x) => typeof x === "string") : undefined,
    }
  }
  // 自带 confidence 的调用方 schema
  const confidence = num(obj.confidence)
  if (confidence == null) return { ...base, error: "输出缺少 confidence 字段" }
  return {
    ...base,
    ok: true,
    result: obj,
    confidence: Math.max(0, Math.min(1, confidence)),
    reason: typeof obj.reason === "string" ? obj.reason : undefined,
    evidence_index: Array.isArray(obj.evidence_index) ? obj.evidence_index.filter((x) => typeof x === "string") : undefined,
  }
}

/**
 * 证据质量下限：至少有一条证据达到该长度才算有效证据（缺省 6 字符，0 = 关闭该检查）。
 *
 * 为什么需要：弱模型（实测 1.5B）会把「任务失败」这类无信息量文本当作证据填入，
 * 从而以高置信度击穿「证据非空」这道防线。真实证据片段（报错行/异常类型/退出码）
 * 远长于该阈值，因此这道检查几乎不会误伤。
 */
export const TRIAGE_DEFAULT_MIN_EVIDENCE_CHARS = 6

/** 证据是否达到质量下限（至少一条够长）。 */
export function evidenceIsSubstantive(evidence: string[] | undefined, minChars: number): boolean {
  if (!evidence?.length) return false
  if (minChars <= 0) return true
  return evidence.some((e) => e.trim().length >= minChars)
}

/**
 * 分流：L1 结论能不能信？
 *   白名单命中         → 采纳（如"无异常"这类无需精审的结论）
 *   未取到结论         → 进 L2（未调用输出工具 / 解析失败 / 请求失败，强制）
 *   置信度 0 或 < 阈值  → 进 L2
 *   无有效证据         → 进 L2（证据为空，或全是无信息量的短文本）
 */
export function classifyL1(
  rec: L1Record,
  threshold: number,
  acceptLabels?: string[],
  minEvidenceChars: number = TRIAGE_DEFAULT_MIN_EVIDENCE_CHARS,
): TriageVerdict {
  const label = labelOf(rec.result)
  if (rec.ok && label && acceptLabels?.length && acceptLabels.includes(label)) return "adopted"
  if (!rec.ok) return "escalate"
  if (rec.confidence <= 0) return "escalate"
  if (rec.confidence < threshold) return "escalate"
  if (!evidenceIsSubstantive(rec.evidence_index, minEvidenceChars)) return "escalate"
  return "adopted"
}

// ── L2 提示词与解析 ────────────────────────────────────────────────────────

interface L2Entry {
  id: string
  title?: string
  features: string
  l1: L1Record
}

interface ParsedL2 {
  result?: unknown
  confidence: number
  reason?: string
  evidence_chain?: EvidenceLink[]
  revised?: boolean
  action?: string
}

/** 组 L2 精审提示词：注入 L1 初判与证据索引，要求取证后给出同 schema 的结论 + 取证链。 */
export function buildL2Prompt(
  entries: L2Entry[],
  opts: { labelEnum?: string[]; businessSchema?: Record<string, unknown>; agents?: string[] },
): string {
  const blocks = entries
    .map((e) =>
      [
        `【待复核条目 id=${e.id}】${e.title ? ` / ${e.title}` : ""}`,
        `【L1 初判】confidence=${e.l1.confidence}${e.l1.ok ? "" : "（L1 未取得可用结论）"}`,
        `  判据: ${e.l1.reason ?? "（无）"}`,
        `  证据索引: ${e.l1.evidence_index?.length ? e.l1.evidence_index.join(" | ") : "（空）"}`,
        e.l1.result !== undefined ? `  初判结果: ${JSON.stringify(e.l1.result)}` : "",
        e.l1.error ? `  失败原因: ${e.l1.error}` : "",
        "【原始信息】",
        e.features,
      ]
        .filter(Boolean)
        .join("\n"),
    )
    .join("\n\n")

  // 注意：条目块里只给 `id=xxx`（不再另加“条目 N”序号）——否则模型会把序号当 id 回填。
  return [
    "下面是待复核的条目。请对每一条主动取证后给出最终结论。",
    opts.agents?.length
      ? `【准备】先装载以下领域子Agent 以便取证：${opts.agents.join("、")}（用 agent_load；已装载则跳过）。`
      : "",
    "",
    blocks,
    "",
    "【输出要求】只输出一个 JSON 对象：",
    `{"results":[{"id":"<下方条目的 id 原文>","result":<业务结果，形状与 L1 一致>,"confidence":0~1,"reason":"依据",` +
      '"evidence_chain":[{"tool":"工具名","finding":"关键发现原文"}],"revised":false,"action":"采纳|修正|证据不足|误判"}]}',
    `results 里每条的 id 必须逐字照抄该条目的 id（即 ${entries.map((e) => e.id).join("、")}），不要编号、不要改写。`,
    opts.labelEnum?.length ? `业务结果里的标签必须取自：${opts.labelEnum.join(" | ")}` : "",
    opts.businessSchema ? `业务结果须满足：${JSON.stringify(opts.businessSchema)}` : "",
    "必须为上面每一个条目都给出结果；证据不足的条目也要输出，把 action 填「证据不足」并在 reason 里列出待查清单。",
  ]
    .filter(Boolean)
    .join("\n")
}

/** 解析 L2 输出（容错：单个对象 / {results:[...]} / 裸数组三种形态）。 */
export function parseL2Output(
  text: string,
  expectedIds?: string[],
): { results: Map<string, ParsedL2>; error?: string } {
  const results = new Map<string, ParsedL2>()
  const ex = extractJson(text)
  if (ex.error || ex.value === undefined) {
    const preview = text.trim().slice(0, 300)
    return { results, error: `L2 输出不是可解析的 JSON：${ex.error ?? "空输出"}｜原文片段：${preview || "（空）"}` }
  }
  const v = ex.value as unknown
  const arr = Array.isArray(v)
    ? v
    : Array.isArray((v as Record<string, unknown>).results)
      ? ((v as Record<string, unknown>).results as unknown[])
      : [v]

  const parsed: ParsedL2[] = []
  const rawIds: Array<string | undefined> = []
  for (const raw of arr) {
    if (!raw || typeof raw !== "object") continue
    const o = raw as Record<string, unknown>
    const conf = num(o.confidence)
    const chain = Array.isArray(o.evidence_chain) ? (o.evidence_chain as unknown[]) : undefined
    rawIds.push(typeof o.id === "string" ? o.id : undefined)
    parsed.push({
      result: o.result,
      confidence: conf == null ? 0 : Math.max(0, Math.min(1, conf)),
      reason: typeof o.reason === "string" ? o.reason : undefined,
      evidence_chain: chain
        ?.filter((x) => Boolean(x) && typeof x === "object")
        .map((x) => {
          const link = x as Record<string, unknown>
          return { tool: String(link.tool ?? ""), finding: String(link.finding ?? "") }
        }),
      revised: o.revised === true,
      action: typeof o.action === "string" ? o.action : undefined,
    })
  }
  if (!parsed.length) return { results, error: "L2 输出里没有带 id 的结果条目" }

  const expected = new Set(expectedIds ?? [])
  const matched = rawIds.filter((id): id is string => Boolean(id) && expected.has(id!)).length
  // 位置对齐兜底：模型改写了 id（如把“t-1001”写成“条目 1”）但条数对得上时，按顺序回填期望 id。
  // 仅在“没有任何 id 命中 + 条数一致”时启用——宁可少数情况下按位置对齐，也不因 id 改写丢掉整批结论。
  if (expected.size && matched === 0) {
    if (parsed.length === expected.size) {
      const ids = expectedIds!
      parsed.forEach((p, i) => results.set(ids[i], p))
      return { results }
    }
    // 条数也对不上：无法判定哪条对应哪条，如实报错而不猜
    return { results, error: `L2 输出的 id 与待复核条目均不匹配（期望 ${expected.size} 条，实得 ${parsed.length} 条）` }
  }

  for (let i = 0; i < parsed.length; i++) {
    const id = rawIds[i]
    if (id) results.set(id, parsed[i])
  }
  if (!results.size) return { results, error: "L2 输出里的 id 与待复核条目均不匹配" }
  return { results }
}

// ── 落盘 ──────────────────────────────────────────────────────────────────

function readJsonl<T>(file: string): T[] {
  if (!existsSync(file)) return []
  const out: T[] = []
  for (const line of readFileSync(file, "utf-8").split(/\r?\n/)) {
    const s = line.trim()
    if (!s) continue
    try {
      out.push(JSON.parse(s) as T)
    } catch {
      /* 并发追加时可能读到写了一半的尾部，忽略坏行 */
    }
  }
  return out
}

function writeJsonl(file: string, rows: unknown[]): void {
  writeFileSync(file, rows.map((r) => JSON.stringify(r)).join("\n") + (rows.length ? "\n" : ""), "utf-8")
}

/** 缺省任务目录：`{home}/users/{user}/triage/{job_id}`（用户级、会话外可访问，便于 REST 与审计）。 */
export function defaultJobDir(home: string, user: string, jobId: string): string {
  return join(home, "users", user, "triage", jobId)
}

// ── 主流程 ────────────────────────────────────────────────────────────────

/**
 * 跑一轮两级研判：L1 批量粗筛 → 阈值分流 → L2 精审兜底。
 *
 * 断点续跑：`l1.jsonl` 里已有 id 的条目不再重跑 L1（重调用同 job_id 即续跑）；
 * 结果与进度落在 jobDir（items/l1/l2/results.jsonl + job.json），可实时 tail、可审计。
 */
export async function runTriage(opts: TriageOptions): Promise<TriageSummary> {
  const t0 = Date.now()
  const threshold = opts.threshold ?? TRIAGE_DEFAULT_THRESHOLD
  const minEvidenceChars = opts.minEvidenceChars ?? TRIAGE_DEFAULT_MIN_EVIDENCE_CHARS
  const jobId = opts.jobId ?? `triage-${new Date().toISOString().replace(/[-:.TZ]/g, "").slice(0, 14)}`
  const home = opts.home ?? process.env.GEBAI_HOME ?? process.cwd()
  const jobDir = opts.jobDir ?? defaultJobDir(home, opts.user, jobId)
  mkdirSync(jobDir, { recursive: true })

  const items = opts.items
  const l1Path = join(jobDir, "l1.jsonl")
  const l2Path = join(jobDir, "l2.jsonl")
  const resultsPath = join(jobDir, "results.jsonl")
  const jobPath = join(jobDir, "job.json")
  const notes: string[] = []
  const started_at = new Date().toISOString()

  writeJsonl(join(jobDir, "items.jsonl"), items)

  const results = new Map<string, TriageResult>()
  const emit = async (r: TriageResult): Promise<void> => {
    results.set(r.id, r)
    appendFileSync(resultsPath, `${JSON.stringify(r)}\n`, "utf-8")
    await opts.onResult?.(r)
  }

  const l1Opts: TriageL1Options = opts.l1
  const { schema: l1Schema, wrapped } = buildL1Schema(opts.schema, opts.labelEnum)

  // ── ① L1 批量粗筛（工具式结构化输出 + 逐条落盘） ──────────────────────
  const l1ById = new Map<string, L1Record>()
  for (const row of readJsonl<{ id: string } & L1Record>(l1Path)) {
    const { id, ...rest } = row
    l1ById.set(id, rest as L1Record)
  }
  const resumed = l1ById.size
  if (resumed) notes.push(`断点续跑：${resumed} 条已有 L1 结果，本次不重跑。`)

  const pending = items.filter((it) => !l1ById.has(it.id))
  let l1Failed = 0
  if (pending.length) {
    const batchItems: BatchItem[] = pending.map((it) => ({
      id: it.id,
      prompt: buildL1Prompt(it, { template: l1Opts.promptTemplate, labelEnum: opts.labelEnum }),
      system: l1Opts.system ?? DEFAULT_L1_SYSTEM,
      meta: it.meta,
    }))
    let done = 0
    await runBatch(batchItems, {
      baseUrl: l1Opts.baseUrl,
      model: l1Opts.model,
      headers: l1Opts.headers,
      concurrency: l1Opts.concurrency ?? TRIAGE_DEFAULT_CONCURRENCY,
      timeoutMs: l1Opts.timeoutMs,
      fetchImpl: l1Opts.fetchImpl,
      signal: opts.signal,
      defaults: {
        max_tokens: l1Opts.maxTokens ?? TRIAGE_DEFAULT_L1_MAX_TOKENS,
        temperature: l1Opts.temperature ?? 0,
        enable_thinking: l1Opts.enableThinking ?? false,
      },
      structured: { schema: l1Schema, output_tool: { reminders: l1Opts.reminders ?? 3 } },
      onResult: (r: BatchResult) => {
        const rec = toL1Record(r, wrapped)
        l1ById.set(r.id, rec)
        appendFileSync(l1Path, `${JSON.stringify({ id: r.id, ...rec })}\n`, "utf-8")
        if (!rec.ok) l1Failed++
        done++
        return opts.onProgress?.({
          phase: "l1",
          done,
          total: pending.length,
          adopted: 0,
          escalated: 0,
          failed: l1Failed,
        })
      },
    })
  }
  const missingToolCalls = [...l1ById.values()].filter((r) => r.tool_call_missing).length
  if (missingToolCalls) {
    notes.push(`${missingToolCalls} 条在提醒上限内始终未调用输出工具 → 已强制转 L2 精审。`)
  }

  // ── ② 分流 ────────────────────────────────────────────────────────────
  const escalate: L2Entry[] = []
  let adopted = 0
  for (const it of items) {
    const rec = l1ById.get(it.id) ?? { ok: false, confidence: 0, error: "L1 未取到结果" }
    l1ById.set(it.id, rec)
    if (classifyL1(rec, threshold, opts.acceptLabels, minEvidenceChars) === "adopted") {
      adopted++
      await emit({
        id: it.id,
        title: it.title,
        layer: "L1",
        ok: true,
        result: rec.result,
        confidence: rec.confidence,
        reason: rec.reason,
        evidence_index: rec.evidence_index,
        l1: rec,
        model: l1Opts.model,
        meta: it.meta,
      })
    } else {
      escalate.push({ id: it.id, title: it.title, features: it.features, l1: rec })
    }
  }

  /** `escalate=false` = 只跑小模型：低置信度结论按原样输出，不上升也不标待精审。 */
  const escalateOn = opts.escalate !== false
  notes.push(
    `分流：${adopted} 条小模型采纳（置信度 ≥ ${threshold} 且证据非空）、${escalate.length} 条${
      escalateOn ? "上升兜底" : "低置信度（未上升兜底）"
    }。`,
  )

  // ── ③ L2 精审兜底（大模型 + 领域工具取证） ──────────────────────────────
  const l2: TriageL2Options | undefined = opts.l2
  const l2Enabled = escalateOn && Boolean(l2?.runner) && l2?.enabled !== false
  const maxL2 = Math.max(0, l2?.maxItems ?? TRIAGE_DEFAULT_MAX_L2)
  const queue = l2Enabled ? escalate.slice(0, maxL2) : []
  if (l2Enabled && escalate.length > queue.length) {
    notes.push(`超 maxItems=${maxL2} 的 ${escalate.length - queue.length} 条本次未精审，以「待精审」返回。`)
  }
  if (!escalateOn && escalate.length) {
    notes.push(
      `未上升兜底（escalate=false）：${escalate.length} 条低置信度结论按小模型原样输出，置信度如实偏低、未标失败。`,
    )
  }
  if (escalateOn && !l2Enabled && escalate.length) {
    notes.push("未注入精审执行器：低置信度条目仅标记待精审，未做兜底。")
  }

  const parsedL2 = new Map<string, ParsedL2>()
  const batchSize = Math.max(1, l2?.batchSize ?? TRIAGE_DEFAULT_L2_BATCH)
  if (queue.length) {
    for (let i = 0; i < queue.length; i += batchSize) {
      if (opts.signal?.aborted) {
        notes.push("L2 精审被取消，剩余条目以「待精审」返回。")
        break
      }
      const chunk = queue.slice(i, i + batchSize)
      try {
        // 精审铁律必须随提示词一起送达：子会话与引擎会话各自带自己的系统提示（不含本模式的约定），
        // 因此这里把系统段落前置进用户提示——否则「主动取证／允许说证据不足／可推翻初判」会丢失。
        const systemBlock = l2?.system ?? DEFAULT_L2_SYSTEM
        const output = await l2!.runner({
          prompt: `${systemBlock}\n\n${buildL2Prompt(chunk, { labelEnum: opts.labelEnum, businessSchema: opts.schema, agents: l2!.agents })}`,
          agents: l2!.agents,
          model: l2!.model,
          apiBase: l2!.apiBase,
          apiKey: l2!.apiKey,
          timeoutMs: l2!.timeoutMs,
        })
        appendFileSync(l2Path, `${JSON.stringify({ ids: chunk.map((c) => c.id), output })}\n`, "utf-8")
        const parsed = parseL2Output(
          output,
          chunk.map((c) => c.id),
        )
        if (parsed.error) notes.push(`L2 第 ${Math.floor(i / batchSize) + 1} 批：${parsed.error}`)
        for (const [id, v] of parsed.results) parsedL2.set(id, v)
      } catch (e) {
        notes.push(`L2 第 ${Math.floor(i / batchSize) + 1} 批执行失败：${(e as Error).message}`)
      }
    }
  }

  // ── ④ 合并与定案 ──────────────────────────────────────────────────────
  const queuedIds = new Set(queue.map((q) => q.id))
  let reviewed = 0
  let revised = 0
  let pendingReview = 0
  let failed = 0
  for (const e of escalate) {
    const p = parsedL2.get(e.id)
    if (p) {
      reviewed++
      if (p.revised) revised++
      await emit({
        id: e.id,
        title: e.title,
        layer: "L2",
        ok: true,
        result: p.result,
        confidence: p.confidence,
        reason: p.reason,
        evidence_chain: p.evidence_chain,
        revised: p.revised,
        action: p.action,
        l1: e.l1,
        model: l2?.model,
        meta: items.find((it) => it.id === e.id)?.meta,
      })
      continue
    }
    // 未上升兜底：小模型结论即最终结论（置信度如实偏低，不因未达标而标失败）
    if (!escalateOn) {
      if (e.l1.ok) adopted++
      await emit({
        id: e.id,
        title: e.title,
        layer: "L1",
        ok: e.l1.ok,
        result: e.l1.result,
        confidence: e.l1.confidence,
        reason: e.l1.reason,
        evidence_index: e.l1.evidence_index,
        l1: e.l1,
        error: e.l1.ok ? undefined : `小模型未取得可用结论且未上升兜底：${e.l1.error ?? "未调用输出工具"}`,
        meta: items.find((it) => it.id === e.id)?.meta,
      })
      continue
    }
    if (!queuedIds.has(e.id)) pendingReview++
    else failed++
    await emit({
      id: e.id,
      title: e.title,
      layer: "L1",
      ok: false,
      result: e.l1.result,
      confidence: e.l1.confidence,
      reason: e.l1.reason,
      evidence_index: e.l1.evidence_index,
      l1: e.l1,
      error: queuedIds.has(e.id)
        ? "精审未返回该条结论"
        : !l2Enabled
          ? "低置信度待精审（本次未注入精审执行器）"
          : "低置信度待精审（超出 maxItems，本次未执行）",
      meta: items.find((it) => it.id === e.id)?.meta,
    })
  }

  // ── ⑤ 落盘（运行期追加便于 tail，收尾重写为一行一条的干净快照） ─────────
  const finalResults = items.map((it) => results.get(it.id)).filter((r): r is TriageResult => Boolean(r))
  writeJsonl(resultsPath, finalResults)
  writeJsonl(l1Path, items.map((it) => ({ id: it.id, ...(l1ById.get(it.id) ?? {}) })))

  const summary: TriageSummary = {
    job_id: jobId,
    job_dir: jobDir,
    total: items.length,
    adopted,
    escalated: escalate.length,
    reviewed,
    pending_review: pendingReview,
    failed,
    revised,
    elapsed_ms: Date.now() - t0,
    results: finalResults,
    notes,
  }
  writeFileSync(
    jobPath,
    `${JSON.stringify(
      {
        job_id: jobId,
        state: opts.signal?.aborted ? "cancelled" : "done",
        started_at,
        finished_at: new Date().toISOString(),
        options: {
          threshold,
          min_evidence_chars: minEvidenceChars,
          accept_labels: opts.acceptLabels ?? [],
          l1: { base_url: l1Opts.baseUrl, model: l1Opts.model },
          l2: l2Enabled ? { enabled: true, agents: l2?.agents ?? [], model: l2?.model, max_items: maxL2, batch_size: batchSize } : { enabled: false },
        },
        summary: { ...summary, results: undefined },
        results_file: resultsPath,
        l1_file: l1Path,
        l2_file: l2Path,
      },
      null,
      2,
    )}\n`,
    "utf-8",
  )

  await opts.onProgress?.({
    phase: "done",
    done: items.length,
    total: items.length,
    adopted,
    escalated: escalate.length,
    failed,
  })

  return summary
}
