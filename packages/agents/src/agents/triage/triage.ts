/**
 * triage 子代理（TS 侧）：大小模型协同的会话内入口。
 *
 * 参数契约、归一与校验、options 组装全部来自 `core/triage`（与 REST 接口同一份）——
 * 本文件只负责会话侧的**兜底执行器**（`ctx.subSessions` 派生隔离子会话）与结果呈现。
 */
import { existsSync, readFileSync } from "node:fs"
import { resolve } from "node:path"
import type { SubAgentDef, Tool, ToolContext, ToolSchema } from "@gebai/sdk"
import {
  defaultJobDir,
  normalizeTriageParams,
  runTriage,
  toTriageOptions,
  triageToolProperties,
  TRIAGE_TEXT_RENDER_MAX,
} from "../../core/triage"
import type { TriageL2Runner, TriageResult, TriageSummary } from "../../core/triage"
import systemPromptBase from "./triage.md"

export const name = "triage"
export const description =
  "通用的大小模型协同：把大量条目交给便宜的小模型批量粗筛（结构化输出 + 置信度），" +
  "低置信度、0 置信度、证据为空或未按约定输出结果的条目，按 escalate 决定是否交给大模型精审兜底——由歌白 Agent 引擎执行，" +
  "可装载领域子Agent 主动取证并给出证据链；escalate=false 时只跑小模型，低置信度结论按原样输出。" +
  "适用于训练任务失败分析、日志异常分类、工单分诊、舆情研判、代码缺陷定位等「先快速分流、再重点深挖」的场景；" +
  "接入新场景只需写一个把领域数据转成统一特征描述的适配器。" +
  "输入：全量信息数组（或条目文件）+ 推理目标 + 结构化输出 schema + 置信度阈值 + 兜底开关；" +
  "输出：结论数组（逐条含置信度与由 schema 定义的结论，L2 额外带取证链与修正标记）。"
export const systemPrompt = systemPromptBase

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

const num = (v: unknown): number | undefined => (typeof v === "number" && Number.isFinite(v) ? v : undefined)
const str = (v: unknown): string | undefined => (typeof v === "string" && v.trim() ? v.trim() : undefined)

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
 * 通用后台任务服务的最小可用面（服务端增强字段，同 subSessions 的处理方式：声明最小接口 + 显式收窄）。
 * 仅用 start（启动后立即返回记录），其余管理动作由 `bg_task` 承担——不在本工具重复实现。
 */
interface BgJobsLike {
  start(opts: {
    kind: string
    name: string
    ref?: Record<string, string>
    run: (task: {
      signal: AbortSignal
      onProgress?: (p: { phase: string; done?: number; total?: number; detail?: string }) => void
    }) => Promise<string | undefined>
  }): { id: string }
}

/** 缺省任务标识（与 REST 侧同一形态，便于日志/产物目录对齐阅读）。 */
function defaultJobId(): string {
  return `triage-${Date.now().toString(36)}`
}

/**
 * 子会话超时后的「快速结束」宽限：超出 timeoutMs 后先注入收敛指令让子会话输出结论，
 * 逾期才强制终止。取值与会话层的 SUBSESSION_FINISH_GRACE_MS 同口径（此处独立声明，
 * 避免 agents 包反向依赖 server 包——两侧的边界由测试固定）。
 */
export const L2_FINISH_GRACE_MS = 120_000

/** 结果摘要（按层与去向统计，便于一眼看清成本结构与待办）。 */
function summarize(s: TriageSummary): string[] {
  const lines = [
    `任务 ${s.job_id}：共 ${s.total} 条，用时 ${(s.elapsed_ms / 1000).toFixed(1)}s`,
    `  小模型即定案 ${s.adopted} 条 ｜ 低置信度 ${s.escalated} 条（O(k)，k≪N 即这套模式的价值）`,
    `  兜底定案 ${s.reviewed} 条（其中修正初判 ${s.revised} 条）｜ 待精审 ${s.pending_review} 条 ｜ 失败 ${s.failed} 条`,
    `  产物目录：${s.job_dir}`,
  ]
  for (const n of s.notes) lines.push(`  备注：${n}`)
  return lines
}

/** 逐条结果的紧凑呈现：层 / 置信度 / 结论片段 / 动作。 */
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
      r.revised ? "（修正初判）" : "",
      r.action ? `action=${r.action}` : "",
      r.ok ? "" : `失败：${r.error ?? "未定案"}`,
    ]
    out.push(`  ${bits.filter(Boolean).join(" ")}`)
  }
  if (results.length > limit) out.push(`  …（共 ${results.length} 条，其余见 data.results 与产物目录的 results.jsonl）`)
  return out
}

// ── triage_run ────────────────────────────────────────────────────────────

const run: Tool = {
  name: "run",
  safeMode: false, // 安全模式下不提供：会向推理端点发起请求、派生会话并写产物
  description:
    "跑一轮大小模型协同：小模型按全量信息数组批量粗筛（结构化输出 + 置信度）→ 按阈值分流 → " +
    "低置信度/无结论条目按 escalate 决定是否交大模型精审兜底（由歌白 Agent 引擎派生隔离子会话执行，可装载领域子Agent 主动取证并给出证据链）；" +
    "escalate=false 则只跑小模型，低置信度结论按原样输出。逐条落盘（items/l1/l2/results.jsonl + job.json），" +
    "同 job_id 续跑会复用已有小模型结果。免审批（不改变服务状态，但会占用算力）。",
  parameters: schema(triageToolProperties()),
  async execute(args, ctx) {
    // ① 参数归一（与 REST 接口同一份契约与校验）
    const norm = normalizeTriageParams(args, { itemsBaseDir: ctx.sessionWorkdir ?? ctx.workdir })
    if ("error" in norm) return { output: norm.error }
    const params = norm.params

    // ② 会话侧的能力边界：显式报错，不静默降级
    const subSessions = subSessionsOf(ctx)
    if (params.escalate && !subSessions) {
      return {
        output:
          "已要求上升兜底（escalate=true），但当前调用不在可派生会话的上下文中（ctx.subSessions 不可用）。\n" +
          "可传 escalate=false 只跑小模型并输出其结论，或走 REST 接口（由 Agent 引擎会话执行兜底）。",
      }
    }

    // ③ 兜底执行器：派生隔离子会话（歌白 Agent 引擎），同步等待其结论
    // 端点/模型覆盖经**子会话自定义环境变量**落实（env_mode=inherit + GEBAI_LLM_*）——子会话的
    // Provider 按自身 env 解析，因此工具通道与 REST 通道能力一致。
    const l2Env: Record<string, string> = {}
    if (params.l2.model) l2Env.GEBAI_LLM_MODEL = params.l2.model
    if (params.l2.apiBase) l2Env.GEBAI_LLM_API_BASE = params.l2.apiBase
    if (params.l2.apiKey) l2Env.GEBAI_LLM_API_KEY = params.l2.apiKey
    const l2Runner: TriageL2Runner | undefined = subSessions
      ? async ({ prompt, agents, model, timeoutMs }) => {
          const budget = timeoutMs ?? params.l2.timeoutMs ?? 600_000
          const [rec] = await subSessions.start([
            {
              input: prompt,
              agents,
              model,
              inheritContext: false, // 隔离：精审过程不污染父会话
              async: false,
              merge: "summary",
              timeoutMs: budget,
              env_mode: "inherit",
              env: l2Env,
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
      : undefined

    // ④ 跑管线（选项组装与 REST 接口同源）
    const built = toTriageOptions(params, {
      user: ctx.user,
      home: ctx.home,
      env: ctx.env,
      l2Runner,
      signal: ctx.signal,
    })
    if ("error" in built) {
      return {
        output:
          `${built.error}\n` +
          '用法：target 缺省 local（本机受管推理服务，需先用 local_infer_start 起服务）；' +
          '也可给命名目标（LOCAL_INFER_TARGETS）或直连 URL，如 target="http://192.168.1.20:8080"。\n' +
          '本机小模型可用 cpu-small 档（不占显卡）：装载 local_infer 子Agent 后 local_infer_start(profile="cpu-small")。',
      }
    }
    // ⑤ 执行：async 走通用后台任务注册表（与 bg_task 同一管理面），sync 当场等结果
    const bgJobs = (ctx as unknown as { bgJobs?: BgJobsLike }).bgJobs
    if (params.mode === "async") {
      if (!bgJobs) {
        return {
          output:
            "mode=async 需要后台任务服务（ctx.bgJobs 不可用——当前不在可启动后台任务的会话上下文中）。\n" +
            "可改用 mode=sync 当场等结果，或走 REST 接口（POST /api/v1/triage/analyze 的 mode=async）。",
        }
      }
      const jobId = params.jobId ?? defaultJobId()
      const jobDir = params.jobDir ?? defaultJobDir(ctx.home, ctx.user, jobId)
      // 同一份 options：异步与同步的参数与执行完全相同，只是不等结果
      const asyncOptions = { ...built.options, jobId, jobDir }
      const rec = bgJobs.start({
        kind: "triage",
        name: `研判 ${jobId}（${params.items.length} 条）`,
        ref: { job_id: jobId, job_dir: jobDir },
        run: async (task) => {
          const summary = await runTriage({
            ...asyncOptions,
            signal: task.signal,
            onProgress: (p) =>
              task.onProgress?.({
                phase: `小模型 ${p.done}/${p.total}`,
                done: p.done,
                total: p.total,
                detail: `采纳 ${p.adopted}、待审 ${p.escalated}、失败 ${p.failed}`,
              }),
          })
          return `共 ${summary.total} 条：小模型定案 ${summary.adopted}、兜底定案 ${summary.reviewed}、待精审 ${summary.pending_review}、失败 ${summary.failed}（产物 ${summary.job_dir}）`
        },
      })
      return {
        output:
          `[研判已后台启动] jobId ${rec.id} ｜ 任务 ${jobId} ｜ ${params.items.length} 条\n` +
          `产物目录：${jobDir}（结论逐条落盘在 results.jsonl，可实时 tail）\n` +
          `管理：bg_task action=status id=${rec.id} 查进度、action=wait 等完成取摘要、action=stop 协作中止（已落盘结果保留）、action=list 列全部；\n` +
          `另可用 triage_view（job_id="${jobId}"）读产物——它是**跨服务重启的权威口径**（后台任务记录随进程消失）。`,
        data: {
          bg_job_id: rec.id,
          job_id: jobId,
          job_dir: jobDir,
          total: params.items.length,
          escalate: params.escalate,
          target: { name: built.endpoint.name, kind: built.endpoint.kind, base_url: built.endpoint.baseUrl },
          state: "running",
        },
      }
    }
    const summary = await runTriage({ ...built.options, ...(params.jobId ? { jobId: params.jobId } : {}) })

    // ⑥ 呈现：结论数组完整进 data.results（受 result_limit 约束），文本只渲染前若干条
    const results = params.resultLimit > 0 ? summary.results.slice(0, params.resultLimit) : summary.results
    const renderCap = Math.min(params.resultLimit > 0 ? params.resultLimit : TRIAGE_TEXT_RENDER_MAX, TRIAGE_TEXT_RENDER_MAX)
    const lines = [
      ...summarize(summary),
      "",
      `结论数组（${results.length} 条${params.resultLimit > 0 ? `，已按 result_limit 截断` : ""}；文本展示前 ${Math.min(renderCap, results.length)} 条）：`,
      ...renderResults(results, renderCap),
    ]
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
        target: { name: built.endpoint.name, kind: built.endpoint.kind, base_url: built.endpoint.baseUrl },
        escalate: params.escalate,
        results,
      },
    }
  },
}

// ── triage_view ───────────────────────────────────────────────────────────

const view: Tool = {
  name: "view",
  safeMode: true,
  description:
    "查看大小模型协同任务的进度/汇总与逐条结论（读产物目录，只读）：status=汇总与备注；results=逐条结论；" +
    "failed=只看未定案的（待精审或兜底未取到结论）。免审批。",
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
      `  总数 ${s.total ?? results.length} ｜ 小模型定案 ${s.adopted ?? 0} ｜ 低置信度 ${s.escalated ?? 0} ｜ 兜底定案 ${s.reviewed ?? 0} ｜ 待精审 ${s.pending_review ?? 0} ｜ 失败 ${s.failed ?? 0} ｜ 修正 ${s.revised ?? 0}`,
    ]
    if (notes.length) lines.push(`  备注：${notes.join("；")}`)
    if (action === "results" || action === "failed") {
      const pool = action === "failed" ? results.filter((r) => !r.ok) : results
      lines.push("", `结论数组（${pool.length} 条中展示前 ${Math.min(limit, pool.length)} 条）：`, ...renderResults(pool, limit))
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
