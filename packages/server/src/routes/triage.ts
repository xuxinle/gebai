/**
 * 两级研判 REST 接口（DESIGN「大小模型协同」）：外部程序 POST 批量数据 → 拿回批量结构化结果。
 *
 * 与 `triage` 子Agent 共用 `@gebai/agents` 的 core/triage 管线（同一份编排、同一份落盘约定），
 * **差异只在 L2 精审执行器**：会话内是隔离子会话，这里是 Agent 引擎的会话入口
 * （`engine.run` 驱动一轮完整会话）——"大模型兜底由歌白 Agent 引擎执行"在两条通道上一致。
 */
import { existsSync, readFileSync } from "node:fs"
import { isAbsolute, join, resolve } from "node:path"
import { defaultJobDir, runTriage, type TriageItem, type TriageResult, type TriageSummary } from "@gebai/agents"
import { TokenBucket } from "../core/security/ratelimit"
import type { RouteCtx } from "./context"

/** 每用户速率限制（研判会批量占用推理算力与 LLM 配额，比普通 prompt 更该限流：10 突发、2/秒补充）。 */
const analyzeRateLimit = new TokenBucket(10, 2)

/** 后台任务登记（仅用于上报"是否仍在跑"与意外错误；权威进度在产物目录的 job.json / l1.jsonl）。 */
const running = new Map<string, { startedAt: number; error?: string }>()

const num = (v: unknown): number | undefined => (typeof v === "number" && Number.isFinite(v) ? v : undefined)
const str = (v: unknown): string | undefined => (typeof v === "string" && v.trim() ? v.trim() : undefined)
const strList = (v: unknown): string[] | undefined =>
  Array.isArray(v) ? v.filter((x): x is string => typeof x === "string" && x.trim() !== "") : undefined

function normalizeItems(v: unknown): TriageItem[] {
  if (!Array.isArray(v)) return []
  return v
    .filter((x) => x && typeof x === "object")
    .map((x, i) => {
      const o = x as Record<string, unknown>
      return {
        id: str(o.id) ?? String(i),
        title: str(o.title),
        features: typeof o.features === "string" ? o.features : "",
        meta: (o.meta as Record<string, unknown>) ?? undefined,
      }
    })
}

/** 读条目文件（JSONL 或 JSON 数组）；相对路径按服务进程 cwd 解析。 */
function readItemsFile(file: string): { items: TriageItem[] } | { error: string } {
  const abs = isAbsolute(file) ? file : resolve(process.cwd(), file)
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
    for (const line of trimmed.split(/\r?\n/)) {
      const s = line.trim()
      if (!s || s.startsWith("#") || s.startsWith("//")) continue
      try {
        raw.push(JSON.parse(s) as unknown)
      } catch {
        return { error: "条目文件不是合法 JSONL（要求每行一条 JSON）" }
      }
    }
  }
  return { items: normalizeItems(raw) }
}

/** 读任务的产物快照（job.json 优先；未完成时用 l1.jsonl 行数报进度）。 */
function readJobState(jobDir: string): Record<string, unknown> | undefined {
  const jobPath = join(jobDir, "job.json")
  if (existsSync(jobPath)) {
    try {
      const job = JSON.parse(readFileSync(jobPath, "utf-8")) as Record<string, unknown>
      const resultsPath = join(jobDir, "results.jsonl")
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
      return { ...job, results }
    } catch (e) {
      return { state: "error", error: `job.json 解析失败：${(e as Error).message}` }
    }
  }
  const l1Path = join(jobDir, "l1.jsonl")
  if (existsSync(l1Path)) {
    const done = readFileSync(l1Path, "utf-8").split(/\r?\n/).filter((l) => l.trim()).length
    return { state: "running", l1_done: done, job_dir: jobDir }
  }
  return undefined
}

export function registerTriageRoutes(rc: RouteCtx): void {
  const { app, d, userOf } = rc

  // 两级研判：一次请求完成「L1 小模型批量粗筛 → 置信度分流 → L2 引擎精审兜底 → 批量结果」
  app.post("/api/v1/triage/analyze", async (c) => {
    const user = await userOf(c)
    if (!analyzeRateLimit.allow(user.id)) return c.json({ error: "rate limited: too many requests" }, 429)

    const body = (await c.req.json().catch(() => ({}))) as Record<string, unknown>

    // ① 条目（内联数组或文件）
    let items = normalizeItems(body.items)
    if (!items.length && str(body.items_file)) {
      const got = readItemsFile(str(body.items_file)!)
      if ("error" in got) return c.json({ error: got.error }, 400)
      items = got.items
    }
    if (!items.length) return c.json({ error: "items（条目数组）或 items_file 至少给一个；条目需含 id 与 features" }, 400)
    const noFeatures = items.filter((it) => !it.features.trim())
    if (noFeatures.length) {
      return c.json(
        {
          error:
            `${noFeatures.length} 条缺少 features（统一特征描述），例如 ${noFeatures.slice(0, 3).map((b) => b.id).join("、")}。` +
            "features 必须含能区分阶段的证据（时间线/状态序列），否则小模型会基于残缺信息给出高置信度的错误结论。",
        },
        400,
      )
    }

    // ② L1 端点（缺省用本机受管推理服务）
    const l1 = (body.l1 ?? {}) as Record<string, unknown>
    const baseUrl = str(l1.base_url) ?? `http://127.0.0.1:${Number(process.env.LOCAL_INFER_PORT ?? 8080)}`
    const headers = str(l1.api_key) ? { Authorization: `Bearer ${str(l1.api_key)!}` } : undefined

    // ③ L2 执行器：Agent 引擎的一轮会话（真正由歌白引擎执行的兜底）
    const l2 = (body.l2 ?? {}) as Record<string, unknown>
    const l2Enabled = l2.enabled !== false
    const l2Agents = strList(l2.agents)
    const l2Model = str(l2.model)
    const l2TimeoutMs = num(l2.timeout_ms) ?? 600000
    const jobId = str(body.job_id) ?? `triage-${Date.now().toString(36)}`
    const jobDir = str(body.job_dir) ?? defaultJobDir(d.config.gebaiHome, user.id, jobId)

    // L2 的"远端大模型"：可单独指定模型名与端点（缺省沿用本服务启动时配置的主模型）
    const l2EnvOverride: Record<string, string> = {}
    if (l2Model) l2EnvOverride.GEBAI_LLM_MODEL = l2Model
    if (str(l2.api_base)) l2EnvOverride.GEBAI_LLM_API_BASE = str(l2.api_base)!
    if (str(l2.api_key)) l2EnvOverride.GEBAI_LLM_API_KEY = str(l2.api_key)!

    const l2Runner = async ({ prompt }: { prompt: string }) => {
      // 领域工具的准备：会话通道用 agent_load 让模型自行装载（引擎不提供按次预载名单）
      const prepared = l2Agents?.length
        ? `【准备】先装载以下领域子Agent 以便取证：${l2Agents.join("、")}（用 agent_load；已装载则跳过）。\n\n${prompt}`
        : prompt
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
            d.engine.windDown(session.id, { reason: `triage L2 精审超时（${l2TimeoutMs} ms）` })
          } catch {
            /* 会话已结束等情形忽略 */
          }
        }, l2TimeoutMs)
        try {
          await d.engine.run(session.id, user.id, prepared, {
            interactionMode: "none",
            outputMode: "final_only",
            autoApprove: false,
            ...(Object.keys(l2EnvOverride).length ? { envOverride: l2EnvOverride } : {}),
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

    const options = {
      user: user.id,
      home: d.config.gebaiHome,
      items,
      jobId,
      jobDir,
      schema: body.schema && typeof body.schema === "object" ? (body.schema as Record<string, unknown>) : undefined,
      labelEnum: strList(body.label_enum),
      threshold: num(body.threshold),
      minEvidenceChars: num(body.min_evidence_chars),
      acceptLabels: strList(body.accept_labels),
      l1: {
        baseUrl,
        headers,
        model: str(l1.model),
        system: str(l1.system),
        promptTemplate: str(l1.prompt_template),
        concurrency: num(l1.concurrency),
        maxTokens: num(l1.max_tokens),
        reminders: num(l1.reminders),
        timeoutMs: num(l1.timeout_ms),
        enableThinking: l1.enable_thinking === true,
      },
      l2: l2Enabled
        ? { runner: l2Runner, agents: l2Agents, model: l2Model, maxItems: num(l2.max_items), batchSize: num(l2.batch_size), timeoutMs: l2TimeoutMs }
        : undefined,
    }

    const resultLimit = num(body.result_limit) ?? 100

    // ④ 同步（缺省，等批次跑完）或后台（立即返回 job_id，用 GET 查进度/取结果）
    if (body.mode === "async") {
      running.set(`${user.id}:${jobId}`, { startedAt: Date.now() })
      void runTriage(options)
        .catch((e) => {
          const rec = running.get(`${user.id}:${jobId}`)
          if (rec) rec.error = String((e as Error).message ?? e)
        })
        .finally(() => {
          const rec = running.get(`${user.id}:${jobId}`)
          if (rec && !rec.error) running.delete(`${user.id}:${jobId}`)
        })
      return c.json({ job_id: jobId, job_dir: jobDir, state: "running", poll: `/api/v1/triage/jobs/${jobId}` }, 202)
    }

    let summary: TriageSummary
    try {
      summary = await runTriage(options)
    } catch (e) {
      return c.json({ error: `研判失败：${String((e as Error).message ?? e)}` }, 500)
    }
    return c.json({
      job_id: summary.job_id,
      job_dir: summary.job_dir,
      state: "done",
      summary: { ...summary, results: undefined },
      total: summary.results.length,
      results: summary.results.slice(0, resultLimit),
    })
  })

  // 任务进度/汇总（未完成时按 l1.jsonl 行数报进度）
  app.get("/api/v1/triage/jobs/:id", async (c) => {
    const user = await userOf(c)
    const jobId = c.req.param("id")
    const jobDir = str(c.req.query("job_dir")) ?? defaultJobDir(d.config.gebaiHome, user.id, jobId)
    const rec = running.get(`${user.id}:${jobId}`)
    const state = readJobState(jobDir)
    if (!state) return c.json({ error: "job not found", job_id: jobId, job_dir: jobDir }, 404)
    return c.json({ job_id: jobId, ...state, ...(rec?.error ? { running_error: rec.error } : {}) })
  })

  // 逐条结果（只读 results.jsonl；可按 ok/层过滤）
  app.get("/api/v1/triage/jobs/:id/results", async (c) => {
    const user = await userOf(c)
    const jobId = c.req.param("id")
    const jobDir = str(c.req.query("job_dir")) ?? defaultJobDir(d.config.gebaiHome, user.id, jobId)
    const state = readJobState(jobDir)
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
    return c.json({ job_id: jobId, job_dir: jobDir, count: pool.length, returned: results.length, results })
  })
}
