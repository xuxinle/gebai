import { describe, expect, test } from "bun:test"
import {
  buildRequestBody,
  chatOnce,
  extractJson,
  normalizeMessages,
  probe,
  runBatch,
  structuredVia,
  validateAgainstSchema,
  type BatchItem,
} from "./api"

// ── 测试替身 ──────────────────────────────────────────────────────────────

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } })
}

/** 模拟 llama-server 的 chat 响应（含 reasoning_content 与 timings）。 */
function chatReply(content: string, reasoning = ""): Response {
  return jsonResponse({
    choices: [{ message: { role: "assistant", content, reasoning_content: reasoning }, finish_reason: "stop" }],
    usage: { prompt_tokens: 3, completion_tokens: 5, total_tokens: 8 },
    timings: { prompt_n: 3, prompt_ms: 10, predicted_n: 5, predicted_ms: 50 },
  })
}

interface Call {
  url: string
  body: Record<string, unknown>
}

/** 记录调用并按 handler 应答的 fetch 替身。 */
function fakeFetch(handler: (call: Call, index: number) => Response): { fetchImpl: typeof fetch; calls: Call[] } {
  const calls: Call[] = []
  const fetchImpl = (async (input: string | URL | Request, init?: RequestInit) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url
    const body = init?.body ? (JSON.parse(String(init.body)) as Record<string, unknown>) : {}
    const call: Call = { url, body }
    calls.push(call)
    return handler(call, calls.length - 1)
  }) as unknown as typeof fetch
  return { fetchImpl, calls }
}

// ── 请求体构造 ────────────────────────────────────────────────────────────

describe("请求体构造", () => {
  test("messages 优先于 system+prompt", () => {
    const msgs = normalizeMessages({
      messages: [{ role: "user", content: "hi" }],
      prompt: "ignored",
      system: "ignored",
    })
    expect(msgs).toEqual([{ role: "user", content: "hi" }])
  })

  test("system + prompt 组装为两条消息", () => {
    expect(normalizeMessages({ system: "s", prompt: "p" })).toEqual([
      { role: "system", content: "s" },
      { role: "user", content: "p" },
    ])
  })

  test("默认 temperature=0；max_tokens 同时给 OpenAI 兼容与新原生字段", () => {
    const body = buildRequestBody({ prompt: "p", max_tokens: 64 })
    expect(body.temperature).toBe(0)
    expect(body.max_tokens).toBe(64)
    expect(body.n_predict).toBe(64)
  })

  test("enable_thinking=false 走 chat_template_kwargs", () => {
    const body = buildRequestBody({ prompt: "p", enable_thinking: false })
    expect(body.chat_template_kwargs).toEqual({ enable_thinking: false })
  })

  test("JSON Schema → response_format.json_schema", () => {
    const schema = { type: "object", properties: { n: { type: "number" } }, required: ["n"] }
    const body = buildRequestBody({ prompt: "p", structured: { schema, schema_name: "out" } })
    expect(body.response_format).toEqual({ type: "json_schema", json_schema: { name: "out", schema, strict: true } })
    expect(structuredVia({ prompt: "p", structured: { schema } })).toBe("schema")
  })

  test("grammar 直传；json_object 退化为最弱约束", () => {
    const g = buildRequestBody({ prompt: "p", structured: { grammar: "root ::= \"a\"" } })
    expect(g.grammar).toBe('root ::= "a"')
    expect(g.response_format).toBeUndefined()
    const j = buildRequestBody({ prompt: "p", structured: { json_object: true } })
    expect(j.response_format).toEqual({ type: "json_object" })
    expect(structuredVia({ prompt: "p" })).toBe("none")
  })
})

// ── JSON 抽取 ─────────────────────────────────────────────────────────────

describe("extractJson", () => {
  test("裸 JSON 与围栏 JSON", () => {
    expect(extractJson('{"a":1}').value).toEqual({ a: 1 })
    expect(extractJson('```json\n{"a":2}\n```').value).toEqual({ a: 2 })
    expect(extractJson("```\n[1,2]\n```").value).toEqual([1, 2])
  })

  test("前后噪声中抽取第一个配平值", () => {
    const r = extractJson('好的，结果如下：{"a":{"b":[1,2]}} 希望有帮助')
    expect(r.value).toEqual({ a: { b: [1, 2] } })
    expect(r.raw).toBe('{"a":{"b":[1,2]}}')
  })

  test("字符串内的括号与转义不误判", () => {
    const r = extractJson('{"s":"} not the end \\" still"}')
    expect(r.value).toEqual({ s: '} not the end " still' })
  })

  test("无法解析时给出错误而不抛异常", () => {
    const r = extractJson("这里没有 JSON")
    expect(r.value).toBeUndefined()
    expect(r.error).toBeTruthy()
    expect(extractJson("").error).toBe("输出为空")
  })
})

// ── Schema 校验 ───────────────────────────────────────────────────────────

describe("validateAgainstSchema", () => {
  const schema = {
    type: "object",
    required: ["name", "score", "tags"],
    properties: {
      name: { type: "string", minLength: 1 },
      score: { type: "number", minimum: 0, maximum: 10 },
      tags: { type: "array", items: { type: "string" }, minItems: 1 },
      grade: { enum: ["A", "B", "C"] },
      nested: { type: "object", required: ["ok"], properties: { ok: { type: "boolean" } } },
    },
  }

  test("合法对象无错误", () => {
    const errs = validateAgainstSchema({ name: "x", score: 8, tags: ["a"], grade: "A", nested: { ok: true } }, schema)
    expect(errs).toEqual([])
  })

  test("缺字段 / 类型不符 / 越界 / 嵌套错误都被报出", () => {
    const errs = validateAgainstSchema({ score: "8", tags: [], grade: "Z", nested: { ok: "yes" } }, schema)
    expect(errs.some((e) => e.includes("$.name: 缺少必需字段"))).toBe(true)
    expect(errs.some((e) => e.includes("$.score: 类型应为 number"))).toBe(true)
    expect(errs.some((e) => e.includes("$.tags: 元素数少于 minItems 1"))).toBe(true)
    expect(errs.some((e) => e.includes("$.grade: 不在枚举"))).toBe(true)
    expect(errs.some((e) => e.includes("$.nested.ok: 类型应为 boolean"))).toBe(true)
  })

  test("integer 不接受小数；anyOf 分支匹配", () => {
    expect(validateAgainstSchema(1.5, { type: "integer" })).toHaveLength(1)
    expect(validateAgainstSchema(2, { type: "integer" })).toHaveLength(0)
    const anyOf = { anyOf: [{ type: "string" }, { type: "number" }] }
    expect(validateAgainstSchema(true, anyOf).length).toBe(1)
    expect(validateAgainstSchema("s", anyOf).length).toBe(0)
  })

  test("无 schema 时不报错（结构化关闭）", () => {
    expect(validateAgainstSchema({ a: 1 }, undefined)).toEqual([])
  })
})

// ── 单条推理 ──────────────────────────────────────────────────────────────

describe("chatOnce", () => {
  test("成功响应：正文/思维链/usage/timings 与实测速度", async () => {
    const { fetchImpl, calls } = fakeFetch(() => chatReply('{"n":3}', "先想一想"))
    const r = await chatOnce(
      { prompt: "q", structured: { schema: { type: "object", required: ["n"], properties: { n: { type: "integer" } } } } },
      { baseUrl: "http://127.0.0.1:8080", fetchImpl },
    )
    expect(r.ok).toBe(true)
    expect(r.content).toBe('{"n":3}')
    expect(r.reasoning).toBe("先想一想")
    expect(r.json).toEqual({ n: 3 })
    expect(r.json_errors).toBeUndefined()
    expect(r.structured_via).toBe("schema")
    expect(r.usage?.completion_tokens).toBe(5)
    expect(r.decode_tps).toBeCloseTo(100, 5) // predicted_n 5 / (50ms) → 100 t/s
    expect(r.attempts).toBe(1)
    expect(calls[0].url).toBe("http://127.0.0.1:8080/v1/chat/completions")
    expect((calls[0].body.response_format as Record<string, unknown>).type).toBe("json_schema")
  })

  test("结构化校验不过时回灌错误重试（第二次请求带提示，失败后仍如实报告）", async () => {
    const { fetchImpl, calls } = fakeFetch(() => chatReply('{"n":"三"}'))
    const r = await chatOnce(
      { prompt: "q", structured: { schema: { type: "object", required: ["n"], properties: { n: { type: "integer" } } } } },
      { baseUrl: "http://127.0.0.1:8080", fetchImpl },
    )
    expect(r.ok).toBe(true)
    expect(r.attempts).toBe(2)
    expect(r.json_errors?.length).toBeGreaterThan(0)
    const second = (calls[1].body.messages as Array<{ role: string; content: string }>)
    expect(second.length).toBe(3)
    expect(second[2].content).toContain("JSON")
  })

  test("服务端拒绝结构化约束时降级 json_object 重试并给出预警", async () => {
    const { fetchImpl, calls } = fakeFetch((_call, i) =>
      i === 0
        ? jsonResponse({ error: { message: "unsupported response_format: json_schema" } }, 400)
        : chatReply('{"n":1}'),
    )
    const r = await chatOnce({ prompt: "q", structured: { schema: { type: "object" } } }, { baseUrl: "http://127.0.0.1:8080", fetchImpl })
    expect(r.ok).toBe(true)
    expect(r.structured_via).toBe("json_object")
    expect(r.warning).toContain("降级")
    expect((calls[1].body.response_format as Record<string, unknown>).type).toBe("json_object")
  })

  test("HTTP 错误如实返回，不抛异常", async () => {
    const { fetchImpl } = fakeFetch(() => jsonResponse({ error: { message: "exceed_context_size" } }, 400))
    const r = await chatOnce({ prompt: "q" }, { baseUrl: "http://127.0.0.1:8080", fetchImpl })
    expect(r.ok).toBe(false)
    expect(r.error).toContain("exceed_context_size")
    expect(r.http_status).toBe(400)
  })

  test("超时中止：返回可读错误而非挂起", async () => {
    const fetchImpl = ((_input: unknown, init?: RequestInit) =>
      new Promise<Response>((_resolve, reject) => {
        init?.signal?.addEventListener("abort", () => reject(new Error("The operation was aborted")))
      })) as unknown as typeof fetch
    const r = await chatOnce({ prompt: "q" }, { baseUrl: "http://127.0.0.1:8080", fetchImpl, timeoutMs: 20 })
    expect(r.ok).toBe(false)
    expect(r.error).toContain("超时")
  })

  test("结构化关闭时不解析 JSON（纯文本输出）", async () => {
    const { fetchImpl } = fakeFetch(() => chatReply("这是普通文本 {不是 JSON"))
    const r = await chatOnce({ prompt: "q" }, { baseUrl: "http://127.0.0.1:8080", fetchImpl })
    expect(r.ok).toBe(true)
    expect(r.structured_via).toBe("none")
    expect(r.json).toBeUndefined()
    expect(r.json_errors).toBeUndefined()
  })
})

// ── 探活 ──────────────────────────────────────────────────────────────────

describe("probe", () => {
  test("读取 n_ctx 与 slot 数（并发上限的档位依据）", async () => {
    const { fetchImpl } = fakeFetch((call) => {
      if (call.url.endsWith("/health")) return jsonResponse({ status: "ok" })
      return jsonResponse({ default_generation_settings: { n_ctx: 32768 }, total_slots: 4, model_path: "C:/m/x.gguf" })
    })
    const p = await probe("http://127.0.0.1:8080", { fetchImpl })
    expect(p.ok).toBe(true)
    expect(p.n_ctx).toBe(32768)
    expect(p.total_slots).toBe(4)
    expect(p.model_path).toBe("C:/m/x.gguf")
  })

  test("服务不可达时给出错误", async () => {
    const fetchImpl = (() => Promise.reject(new Error("connect ECONNREFUSED"))) as unknown as typeof fetch
    const p = await probe("http://127.0.0.1:8080", { fetchImpl })
    expect(p.ok).toBe(false)
    expect(p.error).toContain("ECONNREFUSED")
  })
})

// ── 批量调度 ──────────────────────────────────────────────────────────────

describe("runBatch", () => {
  const baseOptions = (fetchImpl: typeof fetch) => ({ baseUrl: "http://127.0.0.1:8080", fetchImpl, timeoutMs: 5000 })

  test("逐条推理：结果按输入顺序归位，逐条回调与进度回调各就各位", async () => {
    const { fetchImpl } = fakeFetch((call) => {
      const msgs = call.body.messages as Array<{ content: string }>
      return chatReply(`echo:${msgs[msgs.length - 1].content}`)
    })
    const items: BatchItem[] = [1, 2, 3, 4, 5].map((n) => ({ id: `i${n}`, prompt: `p${n}` }))
    const seen: string[] = []
    const progress: number[] = []
    const summary = await runBatch(items, {
      ...baseOptions(fetchImpl),
      concurrency: 2,
      onResult: (r) => {
        seen.push(r.id)
      },
      onProgress: (p) => {
        progress.push(p.done)
      },
    })
    expect(summary.total).toBe(5)
    expect(summary.ok).toBe(5)
    expect(summary.failed).toBe(0)
    expect(seen.sort()).toEqual(["i1", "i2", "i3", "i4", "i5"])
    expect(progress[progress.length - 1]).toBe(5)
    expect(summary.results[2].content).toBe("echo:p3")
    expect(summary.tokens_in).toBe(15)
    expect(summary.tokens_out).toBe(25)
  })

  test("失败按 retries 重试，最终失败的条目带错误信息", async () => {
    const { fetchImpl, calls } = fakeFetch((call) => {
      const msgs = call.body.messages as Array<{ content: string }>
      return msgs[msgs.length - 1].content.includes("bad")
        ? jsonResponse({ error: { message: "server busy" } }, 500)
        : chatReply("ok")
    })
    const summary = await runBatch(
      [
        { id: "good", prompt: "fine" },
        { id: "bad", prompt: "bad one" },
      ],
      { ...baseOptions(fetchImpl), retries: 1 },
    )
    expect(summary.ok).toBe(1)
    expect(summary.failed).toBe(1)
    expect(summary.results[1].error).toContain("server busy")
    expect(summary.results[1].attempts).toBe(2)
    expect(calls.length).toBe(3)
  })

  test("结构化校验不过算失败（含 schema 时），成功条目返回 json 字段", async () => {
    const { fetchImpl } = fakeFetch((call) => {
      const msgs = call.body.messages as Array<{ content: string }>
      return chatReply(msgs[msgs.length - 1].content.includes("bad") ? '{"n":"x"}' : '{"n":7}')
    })
    const summary = await runBatch(
      [
        { id: "a", prompt: "ok" },
        { id: "b", prompt: "bad" },
      ],
      {
        ...baseOptions(fetchImpl),
        structured: { schema: { type: "object", required: ["n"], properties: { n: { type: "integer" } } }, retry_on_invalid: false },
      },
    )
    expect(summary.results[0].json).toEqual({ n: 7 })
    expect(summary.results[1].ok).toBe(false)
    expect(summary.results[1].json_errors?.length).toBeGreaterThan(0)
  })

  test("取消信号：后续条目直接标记取消，不发起请求", async () => {
    const ac = new AbortController()
    const { fetchImpl, calls } = fakeFetch((call) => {
      const msgs = call.body.messages as Array<{ content: string }>
      if (msgs[msgs.length - 1].content === "p1") ac.abort()
      return chatReply("ok")
    })
    const summary = await runBatch(
      [1, 2, 3].map((n) => ({ id: `p${n}`, prompt: `p${n}` })),
      { ...baseOptions(fetchImpl), signal: ac.signal },
    )
    expect(calls.length).toBe(1)
    expect(summary.results[1].error).toBe("任务已取消")
    expect(summary.failed).toBe(2)
  })

  test("空清单不发起请求且汇总为零", async () => {
    const { fetchImpl, calls } = fakeFetch(() => chatReply("x"))
    const summary = await runBatch([], baseOptions(fetchImpl))
    expect(calls.length).toBe(0)
    expect(summary.total).toBe(0)
    expect(summary.decode_tps).toBe(0)
  })

  test("汇总里的有效吞吐按总耗时计算", async () => {
    const { fetchImpl } = fakeFetch(() => chatReply("x"))
    const summary = await runBatch([{ id: "a", prompt: "p" }], baseOptions(fetchImpl))
    const r = summary.results[0]
    expect(r.ok).toBe(true)
    expect(summary.decode_tps).toBeGreaterThan(0)
  })
})

// ── 工具式结构化输出 ──────────────────────────────────────────────────────

/** 模拟「模型调用了输出工具」的响应。 */
function toolReply(name: string, args: unknown, content = ""): Response {
  return jsonResponse({
    choices: [
      {
        message: {
          role: "assistant",
          content,
          tool_calls: [{ id: "call_1", type: "function", function: { name, arguments: JSON.stringify(args) } }],
        },
        finish_reason: "tool_calls",
      },
    ],
    usage: { prompt_tokens: 10, completion_tokens: 6, total_tokens: 16 },
    timings: { predicted_n: 6, predicted_ms: 60 },
  })
}

const ANALYSIS_SCHEMA = {
  type: "object",
  properties: { label: { type: "string" }, confidence: { type: "number" } },
  required: ["label", "confidence"],
}

describe("工具式结构化输出", () => {
  test("schema 注册为 function tool，默认强制调用", () => {
    const body = buildRequestBody({
      prompt: "p",
      structured: { schema: ANALYSIS_SCHEMA, output_tool: {} },
    })
    expect(body.tools).toEqual([
      {
        type: "function",
        function: { name: "submit_result", description: expect.any(String), parameters: ANALYSIS_SCHEMA },
      },
    ])
    expect(body.tool_choice).toBe("required")
    // 与 GBNF 约束解码互斥：同一请求不写 response_format
    expect(body.response_format).toBeUndefined()
    expect(body.grammar).toBeUndefined()
    expect(structuredVia({ prompt: "p", structured: { schema: ANALYSIS_SCHEMA, output_tool: {} } })).toBe("tool")
  })

  test("工具名/描述/提醒次数可配", () => {
    const body = buildRequestBody({
      prompt: "p",
      structured: {
        schema: ANALYSIS_SCHEMA,
        output_tool: { name: "report", description: "提交研判", tool_choice: "auto" },
      },
    })
    const fn = (body.tools as Array<{ function: { name: string; description: string } }>)[0].function
    expect(fn.name).toBe("report")
    expect(fn.description).toBe("提交研判")
    expect(body.tool_choice).toBe("auto")
  })

  test("调用成功：结果取自工具参数并按 schema 校验", async () => {
    const { fetchImpl } = fakeFetch(() => toolReply("submit_result", { label: "OOM", confidence: 0.9 }))
    const resp = await chatOnce(
      { prompt: "p", structured: { schema: ANALYSIS_SCHEMA, output_tool: {} } },
      { baseUrl: "http://x", fetchImpl },
    )
    expect(resp.ok).toBe(true)
    expect(resp.structured_via).toBe("tool")
    expect(resp.json).toEqual({ label: "OOM", confidence: 0.9 })
    expect(resp.tool_call?.name).toBe("submit_result")
    expect(resp.tool_reminders_used).toBe(0)
    expect(resp.tool_call_missing).toBeUndefined()
  })

  test("未调用工具 → 逐次提醒，达到上限返回失败信息", async () => {
    const { fetchImpl, calls } = fakeFetch(() => chatReply("我认为是 OOM"))
    const resp = await chatOnce(
      { prompt: "p", structured: { schema: ANALYSIS_SCHEMA, output_tool: { reminders: 3 } } },
      { baseUrl: "http://x", fetchImpl },
    )
    expect(calls.length).toBe(4) // 首轮 + 3 次提醒
    expect(resp.ok).toBe(false)
    expect(resp.tool_call_missing).toBe(true)
    expect(resp.tool_reminders_used).toBe(3)
    expect(resp.error).toContain("未调用输出工具")
    // 提醒以 user 消息下发，且点名工具
    const lastMsgs = calls[3].body.messages as Array<{ role: string; content: string }>
    expect(lastMsgs[lastMsgs.length - 1].role).toBe("user")
    expect(lastMsgs[lastMsgs.length - 1].content).toContain("submit_result")
  })

  test("提醒 0 次：首次未调用即失败（只发一次请求）", async () => {
    const { fetchImpl, calls } = fakeFetch(() => chatReply("没有工具"))
    const resp = await chatOnce(
      { prompt: "p", structured: { schema: ANALYSIS_SCHEMA, output_tool: { reminders: 0 } } },
      { baseUrl: "http://x", fetchImpl },
    )
    expect(calls.length).toBe(1)
    expect(resp.tool_call_missing).toBe(true)
  })

  test("补充调用后成功：提醒即可纠正", async () => {
    const { fetchImpl, calls } = fakeFetch((_call, i) =>
      i === 0 ? chatReply("我先想想……") : toolReply("submit_result", { label: "网络", confidence: 0.7 }),
    )
    const resp = await chatOnce(
      { prompt: "p", structured: { schema: ANALYSIS_SCHEMA, output_tool: {} } },
      { baseUrl: "http://x", fetchImpl },
    )
    expect(calls.length).toBe(2)
    expect(resp.ok).toBe(true)
    expect(resp.tool_reminders_used).toBe(1)
  })

  test("工具参数不合规 → 回灌错误并重试，耗尽仍失败（不算 tool_call_missing）", async () => {
    const { fetchImpl, calls } = fakeFetch(() => toolReply("submit_result", { label: "X" })) // 缺 confidence
    const resp = await chatOnce(
      { prompt: "p", structured: { schema: ANALYSIS_SCHEMA, output_tool: { reminders: 2 } } },
      { baseUrl: "http://x", fetchImpl },
    )
    expect(calls.length).toBe(3)
    expect(resp.ok).toBe(false)
    expect(resp.tool_call_missing).toBeUndefined()
    expect(resp.json_errors?.join(" ")).toContain("confidence")
    expect(resp.error).toContain("参数不合法")
  })

  test("服务端拒绝 tool_choice=required → 降级 auto 重发，不消耗提醒额度", async () => {
    let first = true
    const { fetchImpl, calls } = fakeFetch(() => {
      if (first) {
        first = false
        return jsonResponse({ error: { message: "unsupported tool_choice value" } }, 400)
      }
      return toolReply("submit_result", { label: "OOM", confidence: 1 })
    })
    const resp = await chatOnce(
      { prompt: "p", structured: { schema: ANALYSIS_SCHEMA, output_tool: {} } },
      { baseUrl: "http://x", fetchImpl },
    )
    expect(calls.length).toBe(2)
    expect(calls[1].body.tool_choice).toBe("auto")
    expect(resp.ok).toBe(true)
    expect(resp.warning).toContain("tool_choice")
  })

  test("批量：工具式输出逐条生效，未调用工具记为失败并可交上层兜底", async () => {
    const { fetchImpl } = fakeFetch((call) => {
      const msgs = call.body.messages as Array<{ content: string }>
      return msgs[0].content === "good"
        ? toolReply("submit_result", { label: "OK", confidence: 0.95 })
        : chatReply("我答不上来")
    })
    const summary = await runBatch([{ id: "g", prompt: "good" }, { id: "b", prompt: "bad" }], {
      baseUrl: "http://x",
      fetchImpl,
      structured: { schema: ANALYSIS_SCHEMA, output_tool: { reminders: 1 } },
    })
    expect(summary.results[0].ok).toBe(true)
    expect(summary.results[0].json).toEqual({ label: "OK", confidence: 0.95 })
    expect(summary.results[1].ok).toBe(false)
    expect(summary.results[1].tool_call_missing).toBe(true)
    expect(summary.failed).toBe(1)
  })
})
