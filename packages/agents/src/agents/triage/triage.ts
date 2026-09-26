/**
 * triage 子代理（TS 侧）：两级研判的会话内入口。
 *
 * 复用 `core/triage` 的共用管线（与 REST 接口同一份逻辑），差异只在 **L2 执行器**的注入方式：
 * 会话内用 `ctx.subSessions.start` 派生隔离子会话（真正的「由歌白 Agent 引擎执行兜底」），
 * REST 侧则用 Agent 引擎的会话入口。
 */
import { existsSync, readFileSync } from "node:fs"
import { isAbsolute, resolve } from "node:path"
import type { SubAgentDef, Tool, ToolContext, ToolSchema } from "@gebai/sdk"
import { defaultJobDir, runTriage } from "../../core/triage/pipeline"
import type { TriageItem, TriageL2Runner, TriageResult, TriageSummary } from "../../core/triage/types"
import { resolveTarget, targetHeaders } from "../local_infer/providers"
import systemPromptBase from "./triage.md"

export const name = "triage"
export const description =
  "通用的大小模型协同（两级研判）：把大量条目交给便宜的小模型批量粗筛（结构化输出 + 置信度），" +
  "低置信度、0 置信度、证据为空或未按约定输出结果的条目，自动交给大模型精审兜底——由歌白 Agent 引擎执行，" +
  "可装载领域子Agent 主动取证并给出证据链。适用于训练任务失败分析、日志异常分类、工单分诊、舆情研判、代码缺陷定位等" +
  "「先快速分流、再重点深挖」的场景；接入新场景只需写一个把领域数据转成统一特征描述的适配器。" +
  "输入：条目集合（或条目文件）+ 结果 schema + 阈值 + L1/L2 配置；输出：统一形态的逐条结果（含置信度、证据、L2 取证链与修正标记）。"
export const systemPrompt = systemPromptBase

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

const num = (v: unknown): number | undefined => (typeof v === "number" && Number.isFinite(v) ? v : undefined)

/**
 * 子会话服务的最小可用面（服务端引擎在可运行的上下文里注入 `ctx.subSessions`）。
 * 该字段属**服务端增强**而非 SDK 契约，故这里声明最小接口并做一次显式收窄：
 * 只用 start / wait / result 三个方法，其余面（get/list/cancel/finish）不依赖。
 */
interface SubSessionsLike {
  start(specs: Array<Record<string, unknown>>): Promise<Array<{ runId: string; status?: string }>>
  wait(runId: string, timeoutMs: number): Promise<{ status?: string } | undefined>
  result(runId: string): { output: string } | undefined
}

const subSessionsOf = (ctx: ToolContext): SubSessionsLike | undefined =>
  (ctx as unknown as { subSessions?: SubSessionsLike }).subSessions

/**
 * 子会话超时后的「快速结束」宽限：超出 timeoutMs 后先注入收敛指令让子会话输出结论，
 * 逾期才强制终止。取值与会话层的 SUBSESSION_FINISH_GRACE_MS 同口径（此处独立声明，
 * 避免 agents 包反向依赖 server 包——两侧的边界由测试固定）。
 */
export const L2_FINISH_GRACE_MS = 120_000
const str = (v: unknown): string | undefined => (typeof v === "string" && v.trim() ? v.trim() : undefined)
const strList = (v: unknown): string[] | undefined =>
  Array.isArray(v) ? v.filter((x): x is string => typeof x === "string" && x.trim() !== "") : undefined

/** 读条目文件（JSONL 每行一条，或 JSON 数组）；路径相对会话工作目录或绝对路径。 */
function readItems(file: string, ctx: ToolContext): { items: TriageItem[] } | { error: string } {
  const abs = isAbsolute(file) ? file : resolve(ctx.sessionWorkdir ?? ctx.workdir, file)
  if (!existsSync(abs)) return { error: `条目文件不存在：${abs}` }
  let text: string
  try {
    text = readFileSync(abs, "utf-8")
  } catch (e) {
    return { error: `条目文件读取失败：${(e as Error).message}` }
  }
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
    if (bad.length) return { error: `条目文件第 ${bad.slice(0, 5).join("/")}${bad.length > 5 ? " 等" : ""} 行不是合法 JSON（JSONL 要求每行一条）` }
  }
  const items: TriageItem[] = []
  for (const [i, r] of raw.entries()) {
    if (!r || typeof r !== "object") return { error: `第 ${i + 1} 条不是对象` }
    const o = r as Record<string, unknown>
    const id = str(o.id) ?? String(i)
    const features = typeof o.features === "string" ? o.features : undefined
    if (features == null) return { error: `第 ${i + 1} 条缺少 features（统一特征描述）` }
    items.push({ id, title: str(o.title), features, meta: (o.meta as Record<string, unknown>) ?? undefined })
  }
  return { items }
}

/** 归一化传入的条目数组（缺 id 用序号补，缺 features 视为错误）。 */
function normalizeItems(v: unknown): TriageItem[] {
  if (!Array.isArray(v)) return []
  return v.filter((x) => x && typeof x === "object").map((x, i) => {
    const o = x as Record<string, unknown>
    return {
      id: str(o.id) ?? String(i),
      title: str(o.title),
      features: typeof o.features === "string" ? o.features : "",
      meta: (o.meta as Record<string, unknown>) ?? undefined,
    }
  })
}

/** 结果摘要（按层与去向统计，便于一眼看清成本结构与待办）。 */
function summarize(s: TriageSummary): string[] {
  const lines = [
    `任务 ${s.job_id}：共 ${s.total} 条，用时 ${(s.elapsed_ms / 1000).toFixed(1)}s`,
    `  L1 采纳 ${s.adopted} 条 ｜ 转 L2 精审 ${s.escalated} 条（O(k)，k≪N 即这套模式的价值）`,
    `  L2 定案 ${s.reviewed} 条（其中修正 L1 初判 ${s.revised} 条）｜ 待精审 ${s.pending_review} 条 ｜ 失败 ${s.failed} 条`,
    `  产物目录：${s.job_dir}`,
  ]
  for (const n of s.notes) lines.push(`  备注：${n}`)
  return lines
}

/** 逐条结果的紧凑呈现：层 / 置信度 / 标签或结果片段 / 动作。 */
function renderResults(results: TriageResult[], limit: number): string[] {
  const out: string[] = []
  for (const r of results.slice(0, limit)) {
    const label =
      r.result && typeof r.result === "object"
        ? (() => {
            const o = r.result as Record<string, unknown>
            const lb = o.label ?? o.verdict
            return typeof lb === "string" ? lb : JSON.stringify(r.result).slice(0, 120)
          })()
        : String(r.result ?? "-")
    const bits = [
      `${r.ok ? "✓" : "✗"} ${r.id}`,
      `[${r.layer}]`,
      `conf=${r.confidence}`,
      label,
      r.revised ? "（修正 L1）" : "",
      r.action ? `action=${r.action}` : "",
      r.ok ? "" : `失败：${r.error ?? "未定案"}`,
    ]
    out.push(`  ${bits.filter(Boolean).join(" ")}`)
  }
  if (results.length > limit) out.push(`  …（共 ${results.length} 条，其余见产物目录的 results.jsonl）`)
  return out
}

// ── triage_run ────────────────────────────────────────────────────────────

const run: Tool = {
  name: "run",
  safeMode: false, // 安全模式下不提供：会向推理端点发起请求、派生会话并写产物
  description:
    "跑一轮两级研判：L1 小模型批量粗筛（结构化输出 + 置信度）→ 按阈值分流 → 低置信度/无结论条目交 L2 大模型精审兜底（" +
    "由歌白 Agent 引擎派生隔离子会话执行，可装载领域子Agent 主动取证并给出证据链）。逐条落盘（items/l1/l2/results.jsonl + job.json），" +
    "同 job_id 续跑会复用已有 L1 结果。免审批（不改变服务状态，但会占用算力）。",
  parameters: schema({
    items: {
      type: "array",
      description:
        "待研判条目数组，元素 { id, title?, features, meta? }。features 是适配器产出的统一特征描述" +
        "（摘要 + 关键字段 + 时间线/序列 + 文本片段，建议 ≤500 token）——**必须含能区分阶段的证据**，" +
        "否则小模型会基于残缺信息给出高置信度的错误结论。",
      items: { type: "object" },
    },
    items_file: { type: "string", description: "条目文件（JSONL 每行一条，或 JSON 数组；与 items 二选一）" },
    schema: {
      type: "object",
      description:
        "业务结果 JSON Schema（描述「结论长什么样」）。引擎自动外包 confidence/reason/evidence_index 信封；" +
        "若该 schema 顶层已含 confidence，则原样使用、不再包装。",
    },
    label_enum: { type: "array", items: { type: "string" }, description: "业务标签枚举（注入结果 schema 的 label.enum；无 schema 时构成缺省业务结果）" },
    threshold: { type: "number", description: "置信度阈值（缺省 0.85）：≥ 阈值且证据足够才由 L1 采纳，否则转 L2" },
    min_evidence_chars: {
      type: "number",
      description:
        "证据质量下限（缺省 6 字符，0 = 关闭）：至少一条证据达该长度才算有效——挡住弱模型把「任务失败」这类无信息量文本当证据、以高置信度击穿「证据非空」防线。",
    },
    accept_labels: { type: "array", items: { type: "string" }, description: "白名单标签：命中即直接采纳（不看置信度），如「无异常」" },
    l1_target: {
      type: "string",
      description: 'L1 小模型端点：缺省 local（本机受管推理服务）；也可是 LOCAL_INFER_TARGETS 的命名目标或直连 URL（"http://host:port"）',
    },
    l1_api_key: { type: "string", description: "L1 端点鉴权密钥（远端需要时）" },
    l1_model: { type: "string", description: "L1 模型名（OpenAI 兼容端点需要 model 字段时）" },
    l1_system: { type: "string", description: "L1 系统提示（缺省内置：只依据给定信息判断、信息不足给 0 置信度）" },
    l1_prompt_template: { type: "string", description: "L1 单条提示模板（占位符 {id} / {title} / {features} / {labels}）" },
    l1_concurrency: { type: "number", description: "L1 并发度（缺省 1；仅在短 prompt + slot 数匹配时有收益，CPU 上并发无增益）" },
    l1_max_tokens: { type: "number", description: "L1 单条最大输出 token（缺省 300）" },
    l1_reminders: { type: "number", description: "L1 未调用输出工具时的提醒次数上限（缺省 3；超过则该条失败并强制转 L2）" },
    l1_timeout_ms: { type: "number", description: "L1 单条请求超时毫秒数（缺省 600000）" },
    enable_thinking: { type: "boolean", description: "L1 是否启用思维链（缺省 false——思考链会吃光输出预算导致正文为空）" },
    l2_enabled: { type: "boolean", description: "是否启用 L2 精审兜底（缺省 true）" },
    l2_agents: { type: "array", items: { type: "string" }, description: "L2 精审子会话预装载的领域子Agent（如 code / wps / nsight，用于主动取证）" },
    l2_model: { type: "string", description: "L2 模型/路由名（GEBAI_LLM_ROUTES 的命名路由或字面模型名；缺省沿用当前会话模型）" },
    l2_max_items: { type: "number", description: "单次最多精审多少条（缺省 20；超出部分标记「待精审」，可用同 job_id 调大后续跑）" },
    l2_batch_size: { type: "number", description: "L2 每轮精审多少条（缺省 5）" },
    l2_timeout_ms: { type: "number", description: "L2 单项精审超时毫秒数（缺省 600000）" },
    job_id: { type: "string", description: "任务标识（缺省自动生成；同 job_id 续跑会复用已有 L1 结果，不重复请求）" },
    job_dir: { type: "string", description: "落盘目录（缺省 {GEBAI_HOME}/users/<user>/triage/<job_id>）" },
    result_limit: { type: "number", description: "返回里逐条展示多少条（缺省 30；全量始终在产物目录的 results.jsonl）" },
  }),
  async execute(args, ctx) {
    // ① 条目
    let items = normalizeItems(args.items)
    if (!items.length && str(args.items_file)) {
      const got = readItems(str(args.items_file)!, ctx)
      if ("error" in got) return { output: got.error }
      items = got.items
    }
    if (!items.length) return { output: "需要 items（条目数组）或 items_file（JSONL/JSON 数组）之一——条目至少含 id 与 features。" }
    const bad = items.filter((it) => !it.features.trim())
    if (bad.length) {
      return {
        output:
          `有 ${bad.length} 条缺少 features（统一特征描述），例如 ${bad.slice(0, 3).map((b) => b.id).join("、")}。` +
          "features 是适配器的产出：把领域数据拼成「摘要 + 关键字段 + 时间线/序列 + 文本片段」，必须含能区分阶段的证据。",
      }
    }

    // ② L1 端点
    const l1Target = resolveTarget(str(args.l1_target), ctx, str(args.l1_api_key))
    if ("error" in l1Target) {
      return {
        output:
          `L1 端点解析失败：${l1Target.error}\n` +
          '用法：l1_target 缺省 local（本机受管推理服务，需先用 local_infer_start 起服务）；' +
          '也可给命名目标（LOCAL_INFER_TARGETS）或直连 URL，如 l1_target="http://192.168.1.20:8080"。\n' +
          '本机小模型可用 cpu-small 档（不占显卡）：装载 local_infer 子Agent 后 local_infer_start(profile="cpu-small")。',
      }
    }

    // ③ L2 执行器：派生隔离子会话（歌白 Agent 引擎），同步等待其结论
    const l2Enabled = args.l2_enabled !== false
    let l2Runner: TriageL2Runner | undefined
    if (l2Enabled) {
      const subSessions = subSessionsOf(ctx)
      if (!subSessions) {
        return { output: "L2 精审需要子会话能力（ctx.subSessions 不可用）：当前调用不在可运行的会话上下文中。可改传 l2_enabled=false 只做 L1 粗筛。" }
      }
      const l2Timeout = num(args.l2_timeout_ms) ?? 600000
      l2Runner = async ({ prompt, agents, model, timeoutMs }) => {
        const budget = timeoutMs ?? l2Timeout
        const [rec] = await subSessions.start([
          {
            input: prompt,
            agents,
            model,
            inheritContext: false, // 隔离：精审过程不污染父会话
            async: false,
            merge: "summary",
            timeoutMs: budget,
          },
        ])
        // 等待预算须覆盖「超时后的快速结束宽限」（子会话到时先注入收敛指令让模型输出结论、宽限逾期才强制
        // 终止），否则会在子会话正要交付结论时被当作失败。wait 本身超时会返回当前快照，故循环等到终态。
        const deadline = Date.now() + budget + L2_FINISH_GRACE_MS + 30_000
        let snap = await subSessions.wait(rec.runId, Math.min(budget, 60_000))
        while (snap && snap.status === "running" && Date.now() < deadline) {
          snap = await subSessions.wait(rec.runId, Math.min(60_000, deadline - Date.now()))
        }
        const res = subSessions.result(rec.runId)
        // 已有结论就用（快速结束阶段也可能已产出）；只有在真的没结论时才报失败
        if (res?.output) return res.output
        if (snap && snap.status !== "done") {
          throw new Error(`精审子会话未正常结束（status=${snap.status}）且未产出结论`)
        }
        throw new Error("精审子会话没有产出结论")
      }
    }

    // ④ 跑管线
    const home = ctx.home
    const jobId = str(args.job_id)
    const summary = await runTriage({
      user: ctx.user,
      home,
      items,
      jobId,
      jobDir: str(args.job_dir) ?? (jobId ? defaultJobDir(home, ctx.user, jobId) : undefined),
      schema: args.schema && typeof args.schema === "object" ? (args.schema as Record<string, unknown>) : undefined,
      labelEnum: strList(args.label_enum),
      threshold: num(args.threshold),
      minEvidenceChars: num(args.min_evidence_chars),
      acceptLabels: strList(args.accept_labels),
      l1: {
        baseUrl: l1Target.baseUrl,
        headers: targetHeaders(l1Target),
        model: str(args.l1_model) ?? l1Target.model,
        system: str(args.l1_system),
        promptTemplate: str(args.l1_prompt_template),
        concurrency: num(args.l1_concurrency),
        maxTokens: num(args.l1_max_tokens),
        reminders: num(args.l1_reminders),
        timeoutMs: num(args.l1_timeout_ms),
        enableThinking: args.enable_thinking === true,
      },
      l2: l2Enabled
        ? {
            runner: l2Runner!,
            agents: strList(args.l2_agents),
            model: str(args.l2_model),
            maxItems: num(args.l2_max_items),
            batchSize: num(args.l2_batch_size),
            timeoutMs: num(args.l2_timeout_ms),
          }
        : undefined,
      signal: ctx.signal,
    })

    const limit = num(args.result_limit) ?? 30
    const lines = [...summarize(summary), "", `逐条结果（前 ${Math.min(limit, summary.results.length)} 条）：`, ...renderResults(summary.results, limit)]
    return {
      output: lines.join("\n"),
      data: {
        job_id: summary.job_id,
        job_dir: summary.job_dir,
        total: summary.total,
        adopted: summary.adopted,
        escalated: summary.escalated,
        reviewed: summary.reviewed,
        pending_review: summary.pending_review,
        failed: summary.failed,
        revised: summary.revised,
        elapsed_ms: summary.elapsed_ms,
        notes: summary.notes,
        l1_target: { name: l1Target.name, kind: l1Target.kind, base_url: l1Target.baseUrl },
        results: summary.results,
      },
    }
  },
}

// ── triage_view ───────────────────────────────────────────────────────────

const view: Tool = {
  name: "view",
  safeMode: true,
  description:
    "查看两级研判任务的进度/汇总与逐条结果（读产物目录，只读）：status=汇总与备注；results=逐条结果；" +
    "failed=只看未定案的（待精审或 L2 未取到结论）。免审批。",
  parameters: schema({
    job_id: { type: "string", description: "任务标识（缺省取最近一次任务）" },
    action: { type: "string", description: "status（缺省）| results | failed" },
    job_dir: { type: "string", description: "任务目录（给了就直接用它，优先于 job_id）" },
    limit: { type: "number", description: "results 呈现条数上限（缺省 50）" },
  }),
  async execute(args, ctx) {
    const dir = str(args.job_dir) ?? (str(args.job_id) ? defaultJobDir(ctx.home, ctx.user, str(args.job_id)!) : undefined)
    if (!dir) return { output: "需要 job_id 或 job_dir 之一。" }
    const jobPath = resolve(dir, "job.json")
    if (!existsSync(jobPath)) return { output: `未找到任务：${jobPath}（triage_run 的返回里有 job_dir）` }
    let job: Record<string, unknown>
    try {
      job = JSON.parse(readFileSync(jobPath, "utf-8")) as Record<string, unknown>
    } catch (e) {
      return { output: `job.json 解析失败：${(e as Error).message}` }
    }
    const resultsPath = resolve(dir, "results.jsonl")
    const results: TriageResult[] = []
    if (existsSync(resultsPath)) {
      for (const line of readFileSync(resultsPath, "utf-8").split(/\r?\n/)) {
        const s = line.trim()
        if (!s) continue
        try {
          results.push(JSON.parse(s) as TriageResult)
        } catch {
          /* 读到写了一半的尾部，忽略 */
        }
      }
    }
    const s = (job.summary ?? {}) as Record<string, unknown>
    const notes = Array.isArray(s.notes) ? (s.notes as string[]) : []
    const action = str(args.action) ?? "status"
    const limit = num(args.limit) ?? 50
    const lines = [
      `任务 ${job.job_id ?? "?"}（state=${job.state ?? "?"}）${dir}`,
      `  总数 ${s.total ?? results.length} ｜ L1 采纳 ${s.adopted ?? 0} ｜ 转 L2 ${s.escalated ?? 0} ｜ L2 定案 ${s.reviewed ?? 0} ｜ 待精审 ${s.pending_review ?? 0} ｜ 失败 ${s.failed ?? 0} ｜ 修正 ${s.revised ?? 0}`,
    ]
    if (notes.length) lines.push(`  备注：${notes.join("；")}`)
    if (action === "results" || action === "failed") {
      const pool = action === "failed" ? results.filter((r) => !r.ok) : results
      lines.push("", `逐条结果（${pool.length} 条中展示前 ${Math.min(limit, pool.length)} 条）：`, ...renderResults(pool, limit))
    }
    return {
      output: lines.join("\n"),
      data: {
        job_id: job.job_id,
        state: job.state,
        job_dir: dir,
        summary: s,
        total: results.length,
        results: action === "status" ? undefined : results.filter((r) => (action === "failed" ? !r.ok : true)).slice(0, limit),
      },
    }
  },
}

export const tools: Record<string, Tool> = { run, view }
/** 两者都不改变服务状态（只发请求、派生会话、写产物目录），免审批。 */
export const requiresApproval: Record<string, boolean> = {}
export const preload = false

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
