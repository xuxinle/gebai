import { afterAll, beforeAll, describe, expect, test } from "bun:test"
import { mkdtempSync, readFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"
import type { ChatOptions, LLMChunk, LLMProvider } from "./core/llm/llm"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"

/** L2 的 fake 大模型：直接回精审 JSON（模拟引擎会话的最终正文）。 */
class L2Fake implements LLMProvider {
  readonly id = "fake-l2"
  calls = 0
  /** MessageLike.content 可能是字符串或内容块数组，两种都摊平。 */
  private static flat(c: unknown): string {
    if (typeof c === "string") return c
    if (Array.isArray(c)) {
      return c
        .map((b) => (b && typeof b === "object" && "text" in (b as object) ? String((b as { text?: unknown }).text ?? "") : ""))
        .join("")
    }
    return ""
  }
  async *chat(msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.calls++
    const text = msgs.map((m) => L2Fake.flat(m.content)).join("\n")
    if (text.includes("待复核的条目")) {
      yield {
        type: "text",
        text: JSON.stringify({
          results: [
            {
              id: "weak",
              result: { label: "网络" },
              confidence: 0.9,
              reason: "查到连接超时",
              evidence_chain: [{ tool: "grep", finding: "conn reset by peer" }],
              revised: true,
              action: "修正",
            },
          ],
        }),
      }
    } else {
      yield { type: "text", text: "收到" }
    }
    yield { type: "done" }
  }
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 100000 }
  }
}

/** L1 stub 端点：按 prompt 里的条目 id 回工具调用（模拟小模型的工具式结构化输出）。 */
function startL1Stub(): { port: number; stop: () => void } {
  const server = Bun.serve({
    port: 0,
    async fetch(req) {
      if (!req.url.endsWith("/v1/chat/completions")) return new Response("not found", { status: 404 })
      const body = (await req.json()) as { messages?: Array<{ content?: string }> }
      const text = (body.messages ?? []).map((m) => m.content ?? "").join("\n")
      const pick = (args: unknown) => ({
        choices: [
          {
            message: {
              role: "assistant",
              content: "",
              tool_calls: [{ id: "c1", type: "function", function: { name: "submit_result", arguments: JSON.stringify(args) } }],
            },
            finish_reason: "tool_calls",
          },
        ],
        usage: { prompt_tokens: 30, completion_tokens: 10 },
      })
      if (text.includes("【条目】good")) {
        return Response.json(pick({ result: { label: "代码退出" }, confidence: 0.95, reason: "日志含 exit 1", evidence_index: ["exit 1"] }))
      }
      if (text.includes("【条目】weak")) {
        return Response.json(pick({ result: { label: "未知" }, confidence: 0.2, reason: "信息不足", evidence_index: [] }))
      }
      return Response.json({
        choices: [{ message: { role: "assistant", content: "不知道" }, finish_reason: "stop" }],
      })
    },
  })
  return { port: server.port ?? 0, stop: () => server.stop(true) }
}

const home = mkdtempSync(join(tmpdir(), "gebai-triage-rest-"))
let handle: ServerHandle
let l1: { port: number; stop: () => void }

beforeAll(async () => {
  l1 = startL1Stub()
  handle = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
  ;(handle.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = new L2Fake()
})

afterAll(() => {
  l1?.stop()
  handle?.gc?.stop()
  handle?.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

const base = (): string => `http://127.0.0.1:${handle.server.port}`

const ITEMS = [
  { id: "good", features: "命令 exit 1，日志尾部含 traceback" },
  { id: "weak", features: "状态卡在 pending，无更多信息" },
]

describe("两级研判 REST 契约", () => {
  test("同步：L1 粗筛 + L2 引擎兜底 → 批量结果", async () => {
    const res = await fetch(`${base()}/api/v1/triage/analyze`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        items: ITEMS,
        schema: { type: "object", properties: { label: { type: "string" } }, required: ["label"] },
        label_enum: ["代码退出", "网络", "未知"],
        threshold: 0.85,
        job_id: "rest-sync",
        l1: { base_url: `http://127.0.0.1:${l1.port}`, concurrency: 1 },
        l2: { enabled: true, batch_size: 5 },
      }),
    })
    expect(res.status).toBe(200)
    const body = (await res.json()) as Record<string, unknown>
    expect(body.state).toBe("done")
    expect(body.total).toBe(2)
    const summary = body.summary as Record<string, unknown>
    expect(summary.adopted).toBe(1)
    expect(summary.escalated).toBe(1)
    expect(summary.reviewed).toBe(1)
    expect(summary.revised).toBe(1)

    const results = body.results as Array<Record<string, unknown>>
    const good = results.find((r) => r.id === "good")!
    expect(good).toMatchObject({ layer: "L1", ok: true, confidence: 0.95 })
    expect(good.evidence_index).toEqual(["exit 1"])
    const weak = results.find((r) => r.id === "weak")!
    expect(weak).toMatchObject({ layer: "L2", ok: true, revised: true, action: "修正" })
    expect(weak.evidence_chain).toEqual([{ tool: "grep", finding: "conn reset by peer" }])
    // L1 初判被保留（校准对比用）
    expect((weak.l1 as Record<string, unknown>).confidence).toBe(0.2)

    // 落盘：job.json + results.jsonl 一行一条
    const jobDir = String(body.job_dir)
    const job = JSON.parse(readFileSync(join(jobDir, "job.json"), "utf-8"))
    expect(job.state).toBe("done")
    expect(job.options.threshold).toBe(0.85)
    expect(readFileSync(join(jobDir, "results.jsonl"), "utf-8").trim().split("\n").length).toBe(2)
    expect(readFileSync(join(jobDir, "l1.jsonl"), "utf-8").trim().split("\n").length).toBe(2)
  })

  test("查进度与逐条结果（支持按层与成败过滤）", async () => {
    const job = (await (await fetch(`${base()}/api/v1/triage/jobs/rest-sync`)).json()) as Record<string, unknown>
    expect(job.state).toBe("done")
    expect((job.summary as Record<string, unknown>).reviewed).toBe(1)

    const all = (await (await fetch(`${base()}/api/v1/triage/jobs/rest-sync/results`)).json()) as Record<string, unknown>
    expect(all.count).toBe(2)

    const l2 = (await (await fetch(`${base()}/api/v1/triage/jobs/rest-sync/results?layer=L2`)).json()) as Record<string, unknown>
    expect(l2.count).toBe(1)
    expect((l2.results as Array<Record<string, unknown>>)[0].id).toBe("weak")

    const none = (await (await fetch(`${base()}/api/v1/triage/jobs/rest-sync/results?only=failed`)).json()) as Record<string, unknown>
    expect(none.count).toBe(0)
  })

  test("未知任务 404", async () => {
    const res = await fetch(`${base()}/api/v1/triage/jobs/does-not-exist`)
    expect(res.status).toBe(404)
  })

  test("缺 items / 缺 features → 400 并指出修复方向", async () => {
    const noItems = await fetch(`${base()}/api/v1/triage/analyze`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({}),
    })
    expect(noItems.status).toBe(400)
    expect(((await noItems.json()) as Record<string, unknown>).error).toContain("items")

    const noFeatures = await fetch(`${base()}/api/v1/triage/analyze`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ items: [{ id: "x" }] }),
    })
    expect(noFeatures.status).toBe(400)
    expect(((await noFeatures.json()) as Record<string, unknown>).error).toContain("features")
  })

  test("异步：立即返回 job_id，可轮询到完成", async () => {
    const res = await fetch(`${base()}/api/v1/triage/analyze`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        items: [{ id: "good", features: "命令 exit 1" }],
        label_enum: ["代码退出", "网络", "未知"],
        job_id: "rest-async",
        mode: "async",
        l1: { base_url: `http://127.0.0.1:${l1.port}` },
        l2: { enabled: false },
      }),
    })
    expect(res.status).toBe(202)
    const body = (await res.json()) as Record<string, unknown>
    expect(body.state).toBe("running")
    expect(body.job_id).toBe("rest-async")
    expect(String(body.job ?? "")).toMatch(/^j[0-9a-f]{8}$/) // 后台任务 id（与工具通道同一份注册表）

    // 202 之后的**第一次轮询不得 404**：此刻产物可能尚未落盘（job.json/l1.jsonl 都还没有），
    // 但本进程的运行记录已知它在跑——把「还没写产物」当成「任务不存在」会让调用方误判失败
    const first = await fetch(`${base()}/api/v1/triage/jobs/rest-async`)
    expect(first.status).toBe(200)
    const firstBody = (await first.json()) as Record<string, unknown>
    expect(["running", "done"]).toContain(String(firstBody.state))

    // 轮询直到落盘（异步批次在同一进程内后台跑）
    let state = "running"
    for (let i = 0; i < 60 && state === "running"; i++) {
      await new Promise((r) => setTimeout(r, 50))
      const j = (await (await fetch(`${base()}/api/v1/triage/jobs/rest-async`)).json()) as Record<string, unknown>
      state = String(j.state ?? "running")
    }
    expect(state).toBe("done")
    const results = (await (await fetch(`${base()}/api/v1/triage/jobs/rest-async/results`)).json()) as Record<string, unknown>
    expect(results.count).toBe(1)
  })
})

// 参数面与工具调用（triage_run）同一份契约：规范名、escalate 语义、目标解析都在接口上生效
describe("参数契约（与工具调用一致）", () => {
  const SCHEMA = { type: "object", properties: { label: { type: "string" } }, required: ["label"] }
  const LABELS = ["代码退出", "网络", "未知"]
  const post = (body: Record<string, unknown>) =>
    fetch(`${base()}/api/v1/triage/analyze`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    })

  test("扁平规范名等价可用：target / escalate / l1_* / l2_*（与嵌套写法同果）", async () => {
    const res = await post({
      items: ITEMS,
      schema: SCHEMA,
      label_enum: LABELS,
      threshold: 0.85,
      job_id: "rest-canonical",
      target: `http://127.0.0.1:${l1.port}`,
      l1_concurrency: 1,
      escalate: true,
      l2_batch_size: 5,
    })
    expect(res.status).toBe(200)
    const body = (await res.json()) as Record<string, unknown>
    const summary = body.summary as Record<string, unknown>
    // 与嵌套写法的用例（rest-sync）完全一致的分流结果
    expect(summary.adopted).toBe(1)
    expect(summary.escalated).toBe(1)
    expect(summary.reviewed).toBe(1)
    expect(summary.revised).toBe(1)
    // 目标解析走同一份 resolveTarget：直连 URL → remote，并回传本批结果出自哪个端点
    expect(body.escalate).toBe(true)
    expect((body.target as Record<string, unknown>).kind).toBe("remote")
    expect(String((body.target as Record<string, unknown>).base_url)).toContain(String(l1.port))
  })

  test("escalate=false：只跑小模型，低置信度结论按原样输出（不标失败）", async () => {
    const res = await post({
      items: ITEMS,
      schema: SCHEMA,
      label_enum: LABELS,
      threshold: 0.85,
      job_id: "rest-noesc",
      target: `http://127.0.0.1:${l1.port}`,
      escalate: false,
    })
    expect(res.status).toBe(200)
    const body = (await res.json()) as Record<string, unknown>
    expect(body.escalate).toBe(false)
    const summary = body.summary as Record<string, unknown>
    expect(summary.escalated).toBe(1) // 低置信度 1 条
    expect(summary.adopted).toBe(2) // 两条都有结论（含低置信度直出）
    expect(summary.pending_review).toBe(0)
    expect(summary.failed).toBe(0)
    const weak = (body.results as Array<Record<string, unknown>>).find((r) => r.id === "weak")!
    expect(weak).toMatchObject({ layer: "L1", ok: true, confidence: 0.2 })
  })

  test("target 命名目标未命中 → 400 且给可操作提示（同一份目标解析）", async () => {
    const res = await post({ items: ITEMS, target: "nope-not-a-target", escalate: false })
    expect(res.status).toBe(400)
    expect(((await res.json()) as Record<string, unknown>).error).toContain("未知推理目标")
  })
})
