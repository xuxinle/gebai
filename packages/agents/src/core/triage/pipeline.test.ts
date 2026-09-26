import { afterAll, describe, expect, test } from "bun:test"
import { mkdtempSync, readFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import {
  buildL1Schema,
  buildL1Prompt,
  buildL2Prompt,
  classifyL1,
  defaultJobDir,
  hasOwnConfidence,
  parseL2Output,
  runTriage,
  toL1Record,
} from "./pipeline"
import type { L1Record, TriageOptions } from "./types"

// ── 测试替身 ──────────────────────────────────────────────────────────────

const dirs: string[] = []
function tempJobDir(label: string): string {
  const d = mkdtempSync(join(tmpdir(), `triage-${label}-`))
  dirs.push(d)
  return d
}
afterAll(() => {
  for (const d of dirs) rmSync(d, { recursive: true, force: true })
})

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } })
}

/** 模拟小模型调用输出工具的成功响应。 */
function toolReply(args: unknown): Response {
  return jsonResponse({
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
    usage: { prompt_tokens: 20, completion_tokens: 8 },
    timings: { predicted_n: 8, predicted_ms: 40 },
  })
}

/** 模拟小模型不调用工具、只回正文。 */
function textReply(text: string): Response {
  return jsonResponse({
    choices: [{ message: { role: "assistant", content: text }, finish_reason: "stop" }],
    usage: { prompt_tokens: 20, completion_tokens: 5 },
  })
}

const LABELS = ["代码退出", "调度", "网络", "未知"]

/** L1 端点的 fetch 替身：按条目 id 决定产出（从 prompt 里认出 id）。 */
function l1Fetch(byId: Record<string, () => Response>): { fetchImpl: typeof fetch; calls: number } {
  const state = { calls: 0 }
  const fetchImpl = (async (_input: unknown, init?: RequestInit) => {
    state.calls++
    const body = JSON.parse(String(init?.body ?? "{}")) as { messages?: Array<{ content: string }> }
    const text = body.messages?.map((m) => m.content).join("\n") ?? ""
    for (const [id, make] of Object.entries(byId)) {
      if (text.includes(`【条目】${id}`)) return make()
    }
    return textReply("我不知道")
  }) as unknown as typeof fetch
  return {
    fetchImpl,
    get calls() {
      return state.calls
    },
  } as unknown as { fetchImpl: typeof fetch; calls: number }
}

// ── schema 信封 ───────────────────────────────────────────────────────────

describe("L1 schema 与信封", () => {
  test("调用方 schema 未含 confidence → 外包分析信封，标签枚举注入 label.enum", () => {
    const business = { type: "object", properties: { label: { type: "string" } }, required: ["label"] }
    const { schema, wrapped } = buildL1Schema(business, LABELS)
    expect(wrapped).toBe(true)
    expect((schema.required as string[]).sort()).toEqual(["confidence", "evidence_index", "reason", "result"])
    const props = schema.properties as Record<string, Record<string, unknown>>
    expect((props.result.properties as Record<string, Record<string, unknown>>).label.enum).toEqual(LABELS)
    const confidence = props.confidence as Record<string, unknown>
    expect(confidence.minimum).toBe(0)
    expect(confidence.maximum).toBe(1)
  })

  test("无 schema 时用标签枚举组缺省业务 schema", () => {
    const { schema, wrapped } = buildL1Schema(undefined, LABELS)
    expect(wrapped).toBe(true)
    const props = schema.properties as Record<string, Record<string, unknown>>
    expect((props.result.properties as Record<string, Record<string, unknown>>).label.enum).toEqual(LABELS)
  })

  test("调用方 schema 自带 confidence → 不再包信封（尊重完整定义）", () => {
    const own = {
      type: "object",
      properties: { label: { type: "string" }, confidence: { type: "number" } },
      required: ["label", "confidence"],
    }
    expect(hasOwnConfidence(own)).toBe(true)
    const { schema, wrapped } = buildL1Schema(own, LABELS)
    expect(wrapped).toBe(false)
    expect(schema.required).toEqual(["label", "confidence"])
    // 仍按调用方给的标签枚举收敛取值空间
    expect((schema.properties as Record<string, Record<string, unknown>>).label.enum).toEqual(LABELS)
    // 未给标签枚举时原样透传（不做任何改写）
    expect(buildL1Schema(own).schema).toBe(own)
  })
})

// ── 提示词 ────────────────────────────────────────────────────────────────

describe("提示词组装", () => {
  test("L1 模板占位符替换（含标签清单）", () => {
    const p = buildL1Prompt({ id: "t1", title: "任务失败", features: "exit 1" }, { labelEnum: LABELS })
    expect(p).toContain("【条目】t1 / 任务失败")
    expect(p).toContain("exit 1")
    expect(p).toContain("代码退出 | 调度")
    expect(p).toContain("置信度填 0")
  })

  test("L2 模板带 L1 初判、证据索引与输出契约，且逐条列全", () => {
    const p = buildL2Prompt(
      [
        { id: "a", title: "A", features: "fa", l1: { ok: true, confidence: 0.3, result: { label: "调度" }, reason: "r1", evidence_index: ["e1"] } },
        { id: "b", features: "fb", l1: { ok: false, confidence: 0, error: "未调用输出工具" } },
      ],
      { labelEnum: LABELS },
    )
    expect(p).toContain("id=a")
    expect(p).toContain("confidence=0.3")
    expect(p).toContain("e1")
    expect(p).toContain("L1 未取得可用结论")
    expect(p).toContain("未调用输出工具")
    expect(p).toContain("证据不足")
    expect(p).toContain('"revised"')
    // id 不得被序号混淄：只给 id=，且明确要求逐字照抄
    expect(p).not.toContain("【条目 1】")
    expect(p).toContain("逐字照抄")
  })
})

// ── 分流规则 ──────────────────────────────────────────────────────────────

describe("分流规则", () => {
  // 证据用真实形态（报错行/退出码）：证据质量下限（缺省 6 字符）会挡住无信息量的短文本
  const ok = (over: Partial<L1Record>): L1Record => ({ ok: true, confidence: 0.9, evidence_index: ["exit 137 at step 4"], ...over })

  test("高置信度 + 有证据 → 采纳", () => {
    expect(classifyL1(ok({ result: { label: "调度" } }), 0.85)).toBe("adopted")
  })

  test("低于阈值 → 进 L2", () => {
    expect(classifyL1(ok({ confidence: 0.5 }), 0.85)).toBe("escalate")
  })

  test("0 置信度（信息不足）→ 必进 L2", () => {
    expect(classifyL1(ok({ confidence: 0 }), 0.85)).toBe("escalate")
  })

  test("证据索引为空 → 进 L2（证据不足=不可信）", () => {
    expect(classifyL1(ok({ evidence_index: [] }), 0.85)).toBe("escalate")
    expect(classifyL1(ok({ evidence_index: undefined }), 0.85)).toBe("escalate")
  })

  test("弱证据（无信息量的短文本）→ 进 L2，挡住高置信度的错误结论", () => {
    // 实测场景：1.5B 把「任务失败」当证据，conf=1；这类证据无定位价值
    expect(classifyL1(ok({ confidence: 1, evidence_index: ["任务失败"], result: { label: "调度" } }), 0.85)).toBe("escalate")
    // 真实证据（报错行）即使很短也远超阈值
    expect(classifyL1(ok({ confidence: 0.9, evidence_index: ["exit 137"] }), 0.85)).toBe("adopted")
    // 多条里只要有一条够长即算有效
    expect(classifyL1(ok({ confidence: 0.9, evidence_index: ["failed", "torch.cuda.OutOfMemoryError"] }), 0.85)).toBe("adopted")
  })

  test("min_evidence_chars=0 可关闭该检查（兼容旧行为）", () => {
    expect(classifyL1(ok({ confidence: 1, evidence_index: ["任务失败"] }), 0.85, undefined, 0)).toBe("adopted")
  })

  test("未取到结论（未调用工具/解析失败）→ 强制进 L2", () => {
    expect(classifyL1({ ok: false, confidence: 0, tool_call_missing: true }, 0.85)).toBe("escalate")
  })

  test("白名单标签直接采纳，不看置信度", () => {
    expect(classifyL1(ok({ confidence: 0.1, evidence_index: [], result: { label: "未知" } }), 0.85, ["未知"])).toBe("adopted")
  })
})

describe("L1 产出 → 记录", () => {
  const batch = (over: Record<string, unknown>) =>
    ({ id: "x", index: 0, ok: true, attempts: 1, elapsed_ms: 1, ...over }) as never

  test("信封形态：result/confidence/reason/evidence_index 归位", () => {
    const rec = toL1Record(
      batch({ json: { result: { label: "网络" }, confidence: 0.9, reason: "r", evidence_index: ["e"] } }),
      true,
    )
    expect(rec).toMatchObject({ ok: true, confidence: 0.9, result: { label: "网络" }, reason: "r" })
  })

  test("置信度越界被夹取；缺 confidence 记失败", () => {
    expect(toL1Record(batch({ json: { result: {}, confidence: 3, reason: "", evidence_index: [] } }), true).confidence).toBe(1)
    const bad = toL1Record(batch({ json: { result: {} } }), true)
    expect(bad.ok).toBe(false)
    expect(bad.error).toContain("confidence")
  })

  test("未调用输出工具 → 失败并带标记", () => {
    const rec = toL1Record(batch({ ok: false, error: "模型未调用输出工具 submit_result（已提醒 3 次）", tool_call_missing: true, tool_reminders_used: 3 }), true)
    expect(rec.ok).toBe(false)
    expect(rec.tool_call_missing).toBe(true)
    expect(rec.tool_reminders_used).toBe(3)
  })

  test("自带 confidence 的调用方 schema：整个对象作为业务结果", () => {
    const rec = toL1Record(batch({ json: { label: "调度", confidence: 0.7 } }), false)
    expect(rec.result).toEqual({ label: "调度", confidence: 0.7 })
  })
})

describe("L2 输出解析", () => {
  test("标准形态 {results:[...]}", () => {
    const { results } = parseL2Output(
      '{"results":[{"id":"a","result":{"label":"网络"},"confidence":0.9,"reason":"r","evidence_chain":[{"tool":"query_log","finding":"连接超时"}],"revised":true,"action":"修正"}]}',
    )
    const a = results.get("a")!
    expect(a.confidence).toBe(0.9)
    expect(a.revised).toBe(true)
    expect(a.action).toBe("修正")
    expect(a.evidence_chain).toEqual([{ tool: "query_log", finding: "连接超时" }])
  })

  test("容错：围栏代码块与裸数组", () => {
    const { results } = parseL2Output('```json\n[{"id":"a","result":{},"confidence":0.5,"action":"证据不足"}]\n```')
    expect(results.get("a")?.action).toBe("证据不足")
  })

  test("无 id 条目 / 非 JSON → 报错而不抛异常", () => {
    expect(parseL2Output('{"results":[{"confidence":1}]}').error).toContain("不匹配")
    expect(parseL2Output("我看不出来").error).toContain("不是可解析的 JSON")
  })

  test("位置对齐兜底：模型改写了 id 但条数一致时按顺序回填", () => {
    const { results, error } = parseL2Output(
      '{"results":[{"id":"条目 1","result":{"label":"OOM"},"confidence":0.9},{"id":"条目 2","result":{"label":"网络问题"},"confidence":0.8}]}',
      ["t-1001", "t-1002"],
    )
    expect(error).toBeUndefined()
    expect(results.get("t-1001")?.confidence).toBe(0.9)
    expect((results.get("t-1002")?.result as Record<string, unknown>).label).toBe("网络问题")
  })

  test("有条目 id 命中时不做位置对齐（避免误配）", () => {
    const { results } = parseL2Output('{"results":[{"id":"t-1002","result":{"label":"网络问题"},"confidence":0.8}]}', ["t-1001", "t-1002"])
    expect([...results.keys()]).toEqual(["t-1002"])
  })

  test("id 完全不匹配且条数不一致 → 报错（不猜）", () => {
    const { error } = parseL2Output('{"results":[{"id":"x","result":{},"confidence":1}]}', ["t-1001", "t-1002"])
    expect(error).toContain("不匹配")
  })
})

describe("缺省任务目录", () => {
  test("用户级路径可预测", () => {
    expect(defaultJobDir("/home/gebai", "admin", "t1").replaceAll("\\", "/")).toBe("/home/gebai/users/admin/triage/t1")
  })
})

// ── 端到端（mock L1 端点 + mock L2 执行器） ─────────────────────────────────

describe("两级研判端到端", () => {
  test("分流 + L2 兜底 + 修正统计 + 落盘", async () => {
    const jobDir = tempJobDir("e2e")
    const l1 = l1Fetch({
      good: () => toolReply({ result: { label: "代码退出" }, confidence: 0.92, reason: "日志含 exit 1", evidence_index: ["exit 1 at step 4"] }),
      low: () => toolReply({ result: { label: "调度" }, confidence: 0.4, reason: "疑似", evidence_index: ["pending"] }),
      zero: () => toolReply({ result: { label: "未知" }, confidence: 0, reason: "信息不足：缺时间线", evidence_index: [] }),
      bad: () => textReply("我觉得是网络问题"),
    })

    const l2Prompts: string[] = []
    const summary = await runTriage({
      user: "tester",
      jobId: "e2e",
      jobDir,
      items: [
        { id: "good", features: "命令 exit 1" },
        { id: "low", features: "状态卡在 pending" },
        { id: "zero", features: "只有一行报错" },
        { id: "bad", features: "连接失败" },
      ],
      labelEnum: LABELS,
      l1: { baseUrl: "http://l1", fetchImpl: l1.fetchImpl, reminders: 1 },
      threshold: 0.85,
      l2: {
        runner: async ({ prompt }) => {
          l2Prompts.push(prompt)
          return JSON.stringify({
            results: [
              { id: "low", result: { label: "调度" }, confidence: 0.95, reason: "查到队列阻塞", evidence_chain: [{ tool: "query_job", finding: "queue=blocked" }], revised: false, action: "采纳" },
              { id: "zero", result: { label: "网络" }, confidence: 0.8, reason: "查到连接超时", evidence_chain: [{ tool: "query_log", finding: "timeout" }], revised: true, action: "修正" },
              { id: "bad", result: { label: "网络" }, confidence: 0.9, reason: "日志确证", evidence_chain: [{ tool: "query_log", finding: "conn reset" }], revised: false, action: "采纳" },
            ],
          })
        },
        batchSize: 5,
        model: "big-model",
      },
    })

    expect(summary.total).toBe(4)
    expect(summary.adopted).toBe(1)
    expect(summary.escalated).toBe(3)
    expect(summary.reviewed).toBe(3)
    expect(summary.revised).toBe(1)
    expect(summary.failed).toBe(0)
    expect(summary.pending_review).toBe(0)

    const byId = new Map(summary.results.map((r) => [r.id, r]))
    expect(byId.get("good")).toMatchObject({ layer: "L1", ok: true, confidence: 0.92 })
    expect(byId.get("good")?.evidence_index).toEqual(["exit 1 at step 4"])
    expect(byId.get("low")).toMatchObject({ layer: "L2", ok: true, action: "采纳", model: "big-model" })
    expect(byId.get("zero")).toMatchObject({ layer: "L2", revised: true, action: "修正" })
    expect(byId.get("zero")?.evidence_chain?.[0]).toEqual({ tool: "query_log", finding: "timeout" })
    // L1 初判被完整保留，便于校准对比
    expect(byId.get("zero")?.l1?.confidence).toBe(0)
    expect(byId.get("bad")?.l1?.tool_call_missing).toBe(true)

    // L2 提示词确实带上了三条待精审条目
    expect(l2Prompts.length).toBe(1)
    expect(l2Prompts[0]).toContain("待复核条目 id=low")
    expect(l2Prompts[0]).toContain("信息不足：缺时间线")

    // 落盘：一行一条的干净快照
    const rows = readFileSync(join(jobDir, "results.jsonl"), "utf-8").trim().split("\n").map((l) => JSON.parse(l))
    expect(rows.length).toBe(4)
    expect(readFileSync(join(jobDir, "items.jsonl"), "utf-8").trim().split("\n").length).toBe(4)
    const job = JSON.parse(readFileSync(join(jobDir, "job.json"), "utf-8"))
    expect(job.state).toBe("done")
    expect(job.summary.reviewed).toBe(3)
    expect(job.options.l2.enabled).toBe(true)
  })

  test("上升但未注入执行器：低置信度条目如实标记待精审，不谎报成功", async () => {
    const jobDir = tempJobDir("nol2")
    const l1 = l1Fetch({ a: () => toolReply({ result: { label: "调度" }, confidence: 0.2, reason: "疑似", evidence_index: ["x"] }) })
    const summary = await runTriage({
      user: "tester",
      jobId: "nol2",
      jobDir,
      items: [{ id: "a", features: "f" }],
      labelEnum: LABELS,
      l1: { baseUrl: "http://l1", fetchImpl: l1.fetchImpl },
    })
    expect(summary.adopted).toBe(0)
    expect(summary.escalated).toBe(1)
    expect(summary.pending_review).toBe(1)
    const r = summary.results[0]
    expect(r.ok).toBe(false)
    expect(r.error).toContain("待精审")
    expect(summary.notes.join(" ")).toContain("未注入精审执行器")
  })

  test("escalate=false：低置信度结论按小模型原样输出（不上升、不标失败）", async () => {
    const jobDir = tempJobDir("noesc")
    const l1 = l1Fetch({ a: () => toolReply({ result: { label: "调度" }, confidence: 0.2, reason: "只有部分线索", evidence_index: ["queue=blocked"] }) })
    const summary = await runTriage({
      user: "tester",
      jobId: "noesc",
      jobDir,
      items: [{ id: "a", features: "f" }],
      labelEnum: LABELS,
      l1: { baseUrl: "http://l1", fetchImpl: l1.fetchImpl },
      escalate: false, // 不注入 l2：只跑小模型
    })
    // 结论按原样输出：置信度如实偏低，但已定案（不待精审、不计失败）
    expect(summary.adopted).toBe(1)
    expect(summary.pending_review).toBe(0)
    expect(summary.failed).toBe(0)
    const r = summary.results[0]
    expect(r.ok).toBe(true)
    expect(r.layer).toBe("L1")
    expect(r.confidence).toBe(0.2)
    expect(r.result).toEqual({ label: "调度" })
    expect(summary.notes.join(" ")).toContain("按小模型原样输出")
  })

  test("maxItems 限制精审条数，其余标记待精审", async () => {
    const jobDir = tempJobDir("max")
    const l1 = l1Fetch({
      a: () => toolReply({ result: { label: "调度" }, confidence: 0.1, reason: "x", evidence_index: ["e"] }),
      b: () => toolReply({ result: { label: "调度" }, confidence: 0.1, reason: "x", evidence_index: ["e"] }),
    })
    const summary = await runTriage({
      user: "tester",
      jobId: "max",
      jobDir,
      items: [
        { id: "a", features: "fa" },
        { id: "b", features: "fb" },
      ],
      l1: { baseUrl: "http://l1", fetchImpl: l1.fetchImpl },
      l2: {
        maxItems: 1,
        runner: async () =>
          JSON.stringify({ results: [{ id: "a", result: { label: "调度" }, confidence: 0.9, reason: "ok", evidence_chain: [], action: "采纳" }] }),
      },
    })
    expect(summary.escalated).toBe(2)
    expect(summary.reviewed).toBe(1)
    expect(summary.pending_review).toBe(1)
    expect(summary.notes.join(" ")).toContain("maxItems")
  })

  test("L2 执行失败：该条如实失败，且不影响已采纳条目", async () => {
    const jobDir = tempJobDir("l2fail")
    const l1 = l1Fetch({
      a: () => toolReply({ result: { label: "代码退出" }, confidence: 0.9, reason: "r", evidence_index: ["exit 1 at step 4"] }),
      b: () => toolReply({ result: { label: "调度" }, confidence: 0.3, reason: "r", evidence_index: ["queue=blocked"] }),
    })
    const summary = await runTriage({
      user: "tester",
      jobId: "l2fail",
      jobDir,
      items: [
        { id: "a", features: "fa" },
        { id: "b", features: "fb" },
      ],
      l1: { baseUrl: "http://l1", fetchImpl: l1.fetchImpl },
      l2: { runner: async () => "模型没按格式回" },
    })
    expect(summary.adopted).toBe(1)
    expect(summary.failed).toBe(1)
    const b = summary.results.find((r) => r.id === "b")!
    expect(b.ok).toBe(false)
    expect(b.error).toContain("精审未返回")
    expect(summary.notes.join(" ")).toContain("不是可解析的 JSON")
  })

  test("断点续跑：同 job_id 复用已有 L1 结果，不重复请求", async () => {
    const jobDir = tempJobDir("resume")
    const spec = { a: () => toolReply({ result: { label: "代码退出" }, confidence: 0.95, reason: "r", evidence_index: ["exit 1 at step 4"] }) }
    const first = l1Fetch(spec)
    const opts = (fetchImpl: typeof fetch): TriageOptions => ({
      user: "tester",
      jobId: "resume",
      jobDir,
      items: [{ id: "a", features: "fa" }],
      labelEnum: LABELS,
      l1: { baseUrl: "http://l1", fetchImpl },
    })
    const s1 = await runTriage(opts(first.fetchImpl))
    expect(s1.adopted).toBe(1)
    expect(first.calls).toBe(1)

    const second = l1Fetch(spec)
    const s2 = await runTriage(opts(second.fetchImpl))
    expect(second.calls).toBe(0) // 命中 l1.jsonl 缓存
    expect(s2.adopted).toBe(1)
    expect(s2.notes.join(" ")).toContain("断点续跑")
  })
})
