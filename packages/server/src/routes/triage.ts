/**
 * 大小模型协同的 REST 接口：外部程序 POST 批量数据 → 拿回批量结论数组。
 *
 * 参数契约、归一与校验、选项组装全部来自 `@gebai/agents` 的 core/triage（与 `triage_run` 工具**同一份**），
 * 差异只有两处：① 兜底执行器走 Agent 引擎的会话入口（会话内工具走隔离子会话）；
 * ② `mode`（同步 / 后台）是外部调用能力，工具调用天然同步。
 */
import { existsSync, readFileSync } from "node:fs"
import { join } from "node:path"
import {
  defaultJobDir,
  loadTriagePreset,
  normalizeTriageParams,
  runTriage,
  toTriageOptions,
  type TriageL2Runner,
  type TriageResult,
  type TriageSummary,
} from "@gebai/agents"
import { TokenBucket } from "../core/security/ratelimit"
import type { RouteCtx } from "./context"

/** 每用户速率限制（研判会批量占用推理算力与 LLM 配额，比普通 prompt 更该限流：缺省 10 突发、2/秒补充）。
 *  可用 `GEBAI_TRIAGE_RATE_BURST` / `GEBAI_TRIAGE_RATE_PER_SEC` 调（任一为 0 即关闭限流，供内网批量放开）——
 *  惰性构造：环境变量在进程内可注入（测试与部署都是同一路径），不必为改限流重启服务改代码。 */
let analyzeRateLimit: TokenBucket | null | undefined
function rateLimiter(): TokenBucket | null {
  if (analyzeRateLimit === undefined) {
    const burst = positiveEnv("GEBAI_TRIAGE_RATE_BURST", 10)
    const perSec = positiveEnv("GEBAI_TRIAGE_RATE_PER_SEC", 2)
    analyzeRateLimit = burst > 0 && perSec > 0 ? new TokenBucket(burst, perSec) : null
  }
  return analyzeRateLimit
}

/** 同步请求的**整体**超时（缺省 120s；`GEBAI_TRIAGE_SYNC_TIMEOUT_MS` 或 body 的 `sync_timeout_ms` 可改，0 = 不限）。
 *  超过即降级为后台执行并返 202 + job_id（**不中断已跑的任务**）：调用方拿 job_id 轮询即可，
 *  不会像以前那样把连接挂到整批跑完（P3/P5）。 */
const SYNC_TIMEOUT_DEFAULT_MS = 120_000
/** setTimeout 上限（避免超时值被夹成 1ms）。 */
const TIMER_MAX_MS = 2 ** 31 - 1

function positiveEnv(name: string, fallback: number): number {
  const v = process.env[name]
  if (typeof v !== "string" || !v.trim()) return fallback
  const n = Number(v)
  return Number.isFinite(n) && n >= 0 ? Math.floor(n) : fallback
}

/** 后台任务的进程内运行记录由引擎的通用注册表提供（与工具通道同一份；作用域 api:<user>）。 */

const str = (v: unknown): string | undefined => (typeof v === "string" && v.trim() ? v.trim() : undefined)

/** 读逐条结论（results.jsonl 边写边读，可拿到运行中的部分结果）。 */
function readResultsFile(jobDir: string, limit?: number): { results: TriageResult[]; total: number } {
  const resultsPath = join(jobDir, "results.jsonl")
  const all: TriageResult[] = []
  if (existsSync(resultsPath)) {
    for (const line of readFileSync(resultsPath, "utf-8").split(/\r?\n/)) {
      const s = line.trim()
      if (!s) continue
      try {
        all.push(JSON.parse(s) as TriageResult)
      } catch {
        /* 读到写了一半的尾部，忽略 */
      }
    }
  }
  return { total: all.length, results: limit !== undefined && limit > 0 ? all.slice(0, limit) : all }
}

/** 读任务的产物快照（job.json 优先；未完成时用 l1.jsonl + results.jsonl 报进度与**部分结果**）。 */
function readJobState(jobDir: string, o: { partialLimit?: number } = {}): Record<string, unknown> | undefined {
  const jobPath = join(jobDir, "job.json")
  if (existsSync(jobPath)) {
    try {
      const job = JSON.parse(readFileSync(jobPath, "utf-8")) as Record<string, unknown>
      return { ...job, results: readResultsFile(jobDir).results }
    } catch (e) {
      return { state: "error", error: `job.json 解析失败：${(e as Error).message}` }
    }
  }
  const l1Path = join(jobDir, "l1.jsonl")
  const partial = readResultsFile(jobDir, o.partialLimit ?? 200)
  if (existsSync(l1Path)) {
    const done = readFileSync(l1Path, "utf-8").split(/\r?\n/).filter((l) => l.trim()).length
    if (done || partial.total) {
      return {
        state: "running",
        l1_done: done,
        job_dir: jobDir,
        // 增量消费：运行中也能拿到已定案的结论（长任务不必等到收尾才能用）
        partial: true,
        results: partial.results,
        results_ready: partial.total,
      }
    }
  }
  return undefined
}

export function registerTriageRoutes(rc: RouteCtx): void {
  const { app, d, userOf } = rc

  // 一次请求完成「小模型批量粗筛 → 置信度分流 → 按 escalate 决定是否引擎精审兜底 → 结论数组」
  app.post("/api/v1/triage/analyze", async (c) => {
    const user = await userOf(c)
    const limiter = rateLimiter()
    if (limiter && !limiter.allow(user.id)) return c.json({ error: "rate limited: too many requests" }, 429)

    const body = (await c.req.json().catch(() => ({}))) as Record<string, unknown>

    // ① 参数归一（与工具调用同一份契约、校验与缺省）
    // 任务配置（{GEBAI_HOME}/triage/preset.json）：调用方从此只需传 items——schema/标签/提示词/阈值/目标名
    // 全部可写进预设文件，优先级恒为「显式参数 > 预设 > 内置缺省」。每次调用重读（改完即生效，不必重启）。
    const preset = loadTriagePreset({ home: d.config.gebaiHome })
    if (preset.error) return c.json({ error: preset.error }, 400)
    const norm = normalizeTriageParams(body, { itemsBaseDir: process.cwd(), preset: preset.fields })
    if ("error" in norm) return c.json({ error: norm.error }, 400)
    const params = norm.params
    // REST 专属参数（不进共用参数表）：工具调用天然同步，没有「同步等待超时」这个概念
    const stRaw = body.sync_timeout_ms
    params.syncTimeoutMs = typeof stRaw === "number" && Number.isFinite(stRaw) && stRaw >= 0 ? Math.floor(stRaw) : undefined
    params.jobId = params.jobId ?? `triage-${Date.now().toString(36)}`
    params.jobDir = params.jobDir ?? defaultJobDir(d.config.gebaiHome, user.id, params.jobId)
    const { jobId, jobDir } = params

    // ② 兜底执行器：Agent 引擎的一轮会话（真正由歌白引擎执行的兜底）
    const l2TimeoutMs = params.l2.timeoutMs ?? 600_000
    const l2Runner: TriageL2Runner = async ({ prompt, model, apiBase, apiKey }) => {
      // 兜底模型可指向独立端点与模型名（会话级环境覆盖；工具调用通道无此能力，见 triage 子Agent）
      const envOverride: Record<string, string> = {}
      if (model) envOverride.GEBAI_LLM_MODEL = model
      if (apiBase) envOverride.GEBAI_LLM_API_BASE = apiBase
      if (apiKey) envOverride.GEBAI_LLM_API_KEY = apiKey
      const session = await d.store.createSession(user.id, `triage ${jobId}`)
      let taskError: string | undefined
      const unsub = d.events.subscribe((ev) => {
        if (ev.sessionId !== session.id || ev.type !== "event.task.error") return
        taskError = String(ev.payload.error ?? "unknown error")
      })
      try {
        // 限时：精审是 Agent 循环（会主动取证），不设上限会无界占用会话与配额；
        // 超时用 windDown 让引擎收尾（而不是硬杀），本批按失败计入 notes。
        const timer = setTimeout(() => {
          try {
            d.engine.windDown(session.id, { reason: `triage 精审超时（${l2TimeoutMs} ms）` })
          } catch {
            /* 会话已结束等情形忽略 */
          }
        }, l2TimeoutMs)
        try {
          await d.engine.run(session.id, user.id, prompt, {
            interactionMode: "none",
            outputMode: "final_only",
            autoApprove: false,
            ...(Object.keys(envOverride).length ? { envOverride } : {}),
          })
        } finally {
          clearTimeout(timer)
        }
      } finally {
        unsub()
      }
      if (taskError) throw new Error(`精审会话失败：${taskError}`)
      const loaded = await d.store.load(session.id, user.id)
      const last = loaded ? [...loaded.messages].reverse().find((m) => m.role === "assistant") : undefined
      const text = last && typeof last.content === "string" ? last.content : ""
      if (!text.trim()) throw new Error("精审会话没有产出结论")
      return text
    }

    // ③ 选项组装（与工具调用同源；推理目标按 REST 侧环境解析 local/命名目标/URL）
    const env: Record<string, string> = {}
    for (const [k, v] of Object.entries(process.env)) if (typeof v === "string") env[k] = v
    env.GEBAI_HOME = d.config.gebaiHome
    const built = toTriageOptions(params, { user: user.id, home: d.config.gebaiHome, env, l2Runner })
    if ("error" in built) return c.json({ error: built.error }, 400)

    // ④ 同步（缺省，等批次跑完）或后台（立即返回 job_id，用 GET 查进度/取结果）
    // 后台跟踪与工具通道用**同一份注册表**（kind=triage）；作用域为 api:<user>，与任何会话隔离。
    const jobs = d.engine.backgroundJobs(`api:${user.id}`)
    const startJob = (): { id: string } =>
      jobs.start({
        kind: "triage",
        name: `研判 ${jobId}（${params.items.length} 条）`,
        ref: { job_id: jobId, job_dir: jobDir },
        run: async (task) => {
          const summary = await runTriage({
            ...built.options,
            jobId,
            jobDir,
            signal: task.signal,
            onProgress: (p) =>
              task.onProgress?.({
                phase: `小模型 ${p.done}/${p.total}`,
                done: p.done,
                total: p.total,
                detail: `采纳 ${p.adopted}、待审 ${p.escalated}、失败 ${p.failed}`,
              }),
          })
          return `共 ${summary.total} 条：小模型定案 ${summary.adopted}、兜底定案 ${summary.reviewed}、待精审 ${summary.pending_review}、失败 ${summary.failed}`
        },
      })
    const accepted = (rec: { id: string }, o: { degraded?: string; partial?: boolean } = {}) =>
      c.json(
        {
          job_id: jobId,
          job_dir: jobDir,
          state: "running",
          job: rec.id,
          poll: `/api/v1/triage/jobs/${jobId}`,
          ...(o.degraded ? { degraded: o.degraded, note: "任务未中断，仍在后台跑；轮询 poll 取进度与（运行中的）部分结论。" } : {}),
          ...(o.partial ? { partial: true } : {}),
        },
        202,
      )

    if (params.mode === "async") return accepted(startJob())

    /** 跑完即按同步口径返回（结论数组默认全量，result_limit > 0 才截断）。 */
    const respondDone = (summary: TriageSummary) => {
      const results = params.resultLimit > 0 ? summary.results.slice(0, params.resultLimit) : summary.results
      return c.json({
        job_id: jobId,
        job_dir: jobDir,
        state: "done",
        escalate: params.escalate,
        target: { name: built.endpoint.name, kind: built.endpoint.kind, base_url: built.endpoint.baseUrl },
        summary: { ...summary, results: undefined },
        total: summary.results.length,
        returned: results.length,
        results,
      })
    }

    // 同步路径：**整体超时保护**。研判本身没有整体超时上限时，一个 sync 请求可以无限挂起
    // （网关排队 / 单条超时缺省 600s），外部调用方感知为「卡死」。这里到点**不杀任务**，
    // 而是把它转交后台并返 202——已跑的进度不浪费，调用方改拿 job_id 轮询（降级不是失败）。
    const syncTimeoutMs = params.syncTimeoutMs ?? positiveEnv("GEBAI_TRIAGE_SYNC_TIMEOUT_MS", SYNC_TIMEOUT_DEFAULT_MS)
    if (syncTimeoutMs > 0) {
      let timer: ReturnType<typeof setTimeout> | undefined
      const timeout = new Promise<"timeout">((res) => {
        timer = setTimeout(() => res("timeout"), Math.min(syncTimeoutMs, TIMER_MAX_MS))
      })
      const running = runTriage({ ...built.options, jobId, jobDir })
      running.catch(() => {
        /* 降级后由后台任务接管并记错，这里只防未处理的 rejection */
      })
      const winner = await Promise.race([running.then(() => "done" as const), timeout])
      if (timer) clearTimeout(timer)
      if (winner === "timeout") {
        const rec = startJob()
        return accepted(rec, { degraded: `同步等待超过 ${syncTimeoutMs} ms，已转为后台执行` })
      }
      try {
        return respondDone(await running)
      } catch (e) {
        return c.json({ error: `研判失败：${String((e as Error).message ?? e)}` }, 500)
      }
    }

    let summary: TriageSummary
    try {
      summary = await runTriage({ ...built.options, jobId, jobDir })
    } catch (e) {
      return c.json({ error: `研判失败：${String((e as Error).message ?? e)}` }, 500)
    }
    return respondDone(summary)
  })

  // 任务进度/汇总（未完成时按 l1.jsonl 行数报进度）
  app.get("/api/v1/triage/jobs/:id", async (c) => {
    const user = await userOf(c)
    const jobId = c.req.param("id")
    const jobDir = str(c.req.query("job_dir")) ?? defaultJobDir(d.config.gebaiHome, user.id, jobId)
    const rec = d.engine
      .backgroundJobs(`api:${user.id}`)
      .list()
      .find((j) => j.ref?.job_id === jobId)
    const state = readJobState(jobDir)
    // 刚起步的批次产物尚未落盘（job.json / l1.jsonl 都还没有）：本进程的运行记录就是权威回答，
    // 不能当成「任务不存在」——否则调用方在 202 之后第一次轮询会拿到 404
    if (!state) {
      if (rec) {
        return c.json({
          job_id: jobId,
          job_dir: jobDir,
          state: rec.status,
          job: rec.id,
          job_status: rec.status,
          ...(rec.progress ? { progress: rec.progress } : {}),
          ...(rec.error ? { running_error: rec.error } : {}),
        })
      }
      return c.json({ error: "job not found", job_id: jobId, job_dir: jobDir }, 404)
    }
    return c.json({
      job_id: jobId,
      ...state,
      ...(rec ? { job: rec.id, job_status: rec.status, progress: rec.progress } : {}),
      ...(rec?.error ? { running_error: rec.error } : {}),
    })
  })

  // 逐条结论（只读 results.jsonl；可按 ok/层过滤）——**运行中也可读**（增量消费：拿到已定案的部分结论）
  app.get("/api/v1/triage/jobs/:id/results", async (c) => {
    const user = await userOf(c)
    const jobId = c.req.param("id")
    const jobDir = str(c.req.query("job_dir")) ?? defaultJobDir(d.config.gebaiHome, user.id, jobId)
    const state = readJobState(jobDir, { partialLimit: 0 })
    if (!state) return c.json({ error: "job not found", job_id: jobId, job_dir: jobDir }, 404)
    const all = (state.results as TriageResult[] | undefined) ?? []
    const layer = str(c.req.query("layer"))
    const only = str(c.req.query("only"))
    let pool = all
    if (layer) pool = pool.filter((r) => r.layer === layer)
    if (only === "ok") pool = pool.filter((r) => r.ok)
    if (only === "failed") pool = pool.filter((r) => !r.ok)
    const limit = Number(c.req.query("limit") ?? 200)
    const results = pool.slice(0, Number.isFinite(limit) && limit > 0 ? limit : 200)
    // 全量始终在产物目录的 results.jsonl（本响应为分页视图）
    return c.json({
      job_id: jobId,
      job_dir: jobDir,
      // state=done 时本响应已是全量；running 时是**部分结果**（results_ready 为已定案数）
      state: state.state,
      ...(state.partial ? { partial: true, results_ready: state.results_ready } : {}),
      count: pool.length,
      returned: results.length,
      results,
    })
  })
}
