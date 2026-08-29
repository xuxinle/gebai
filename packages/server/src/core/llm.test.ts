import { describe, expect, test } from "bun:test"
import type { MessageLike } from "@gebai/sdk"
import { applyModelEnvOverrides, createProvider, endpointUrl, modelEnvOverrides, parseSSE, parseExtraParams, imageMessageBlocks, resolveVisionProvider, salvageWriteArgs, type ProviderConfig, type LLMChunk } from "./llm"

const BASE_CFG: ProviderConfig = { apiKind: "openai", apiBase: "https://api.test", apiKey: "k", model: "m", maxContextTokens: 10000, multimodal: false }

function provider() {
  return createProvider(BASE_CFG)
}

const OK_STREAM = 'data: {"choices":[{"delta":{"content":"hi"},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n'

function withFetch(mock: (url?: string | URL, init?: RequestInit) => Promise<Response>, fn: () => Promise<void>) {
  const original = globalThis.fetch
  ;(globalThis as unknown as { fetch: typeof fetch }).fetch = mock as unknown as typeof fetch
  return fn().finally(() => {
    ;(globalThis as unknown as { fetch: typeof fetch }).fetch = original
  })
}

describe("provider robustness", () => {
  test("5xx 失败后重试成功（指数退避，仅重试一次）", async () => {
    let calls = 0
    await withFetch(async () => {
      calls++
      return calls === 1 ? new Response("boom", { status: 500 }) : new Response(OK_STREAM, { status: 200 })
    }, async () => {
      const chunks: string[] = []
      for await (const c of provider().chat([{ role: "user", content: "x" }])) if (c.type === "text") chunks.push(c.text!)
      expect(calls).toBe(2)
      expect(chunks).toEqual(["hi"])
    })
  })

  test("429 失败后重试成功", async () => {
    let calls = 0
    await withFetch(async () => {
      calls++
      return calls === 1 ? new Response("rate limited", { status: 429 }) : new Response(OK_STREAM, { status: 200 })
    }, async () => {
      const chunks: string[] = []
      for await (const c of provider().chat([{ role: "user", content: "x" }])) if (c.type === "text") chunks.push(c.text!)
      expect(calls).toBe(2)
      expect(chunks).toEqual(["hi"])
    })
  })

  test("4xx 不重试，直接抛出中文错误", async () => {
    let calls = 0
    await withFetch(async () => {
      calls++
      return new Response("bad key", { status: 401 })
    }, async () => {
      const run = (async () => {
        for await (const _ of provider().chat([{ role: "user", content: "x" }])) void _
      })()
      await expect(run).rejects.toThrow(/模型接口错误（HTTP 401）/)
      expect(calls).toBe(1)
    })
  })

  test("重试耗尽（连续 5xx）后抛出错误", async () => {
    let calls = 0
    await withFetch(async () => {
      calls++
      return new Response("boom", { status: 503 })
    }, async () => {
      const run = (async () => {
        for await (const _ of provider().chat([{ role: "user", content: "x" }])) void _
      })()
      await expect(run).rejects.toThrow(/模型接口错误（HTTP 503）/)
      expect(calls).toBe(3) // 1 次初始 + 2 次重试
    })
  })

  test("SSE 解析：data 行合并、多事件、尾部无空行", async () => {
    const stream = new ReadableStream<Uint8Array>({
      start(c) {
        const enc = new TextEncoder()
        c.enqueue(enc.encode("data: a\n\ndata: b\n\n"))
        c.enqueue(enc.encode("data: c"))
        c.close()
      },
    })
    const out: string[] = []
    for await (const d of parseSSE(stream)) out.push(d)
    expect(out).toEqual(["a", "b", "c"])
  })

  test("SSE 解析：忽略非 data 行与空 payload", async () => {
    const stream = new ReadableStream<Uint8Array>({
      start(c) {
        c.enqueue(new TextEncoder().encode(": comment\nevent: x\ndata: {\"a\":1}\n\n"))
        c.close()
      },
    })
    const out: string[] = []
    for await (const d of parseSSE(stream)) out.push(d)
    expect(out).toEqual(['{"a":1}'])
  })

  test("OpenAI 兼容流解析：正常文本逐 chunk 即时输出（不缓冲）", async () => {
    const chunks: string[] = []
    await withFetch(
      async () =>
        new Response(
          'data: {"choices":[{"delta":{"content":"第一"}}]}\n\ndata: {"choices":[{"delta":{"content":"第二"}}]}\n\ndata: {"choices":[{"delta":{"content":"第三"}}]}\n\ndata: [DONE]\n\n',
          { status: 200 },
        ),
      async () => {
        for await (const c of provider().chat([{ role: "user", content: "x" }])) if (c.type === "text") chunks.push(c.text!)
      },
    )
    expect(chunks).toEqual(["第一", "第二", "第三"])
  })

  test("OpenAI 兼容流解析：正常 reasoning_content 输出", async () => {
    const chunks: string[] = []
    await withFetch(
      async () => new Response('data: {"choices":[{"delta":{"reasoning_content":"正常思考内容"}}]}\n\ndata: [DONE]\n\n', { status: 200 }),
      async () => {
        for await (const c of provider().chat([{ role: "user", content: "x" }])) if (c.type === "reasoning") chunks.push(c.text!)
      },
    )
    expect(chunks).toEqual(["正常思考内容"])
  })

  test("OpenAI 兼容流：include_usage 末 chunk 的 usage 挂到 done", async () => {
    let body: Record<string, unknown> = {}
    const chunks: LLMChunk[] = []
    await withFetch(
      async (_u, init) => {
        body = JSON.parse(String((init as RequestInit).body))
        return new Response(
          'data: {"choices":[{"delta":{"content":"hi"},"finish_reason":"stop"}]}\n\ndata: {"choices":[],"usage":{"prompt_tokens":120,"completion_tokens":3,"total_tokens":123}}\n\ndata: [DONE]\n\n',
          { status: 200 },
        )
      },
      async () => {
        for await (const c of provider().chat([{ role: "user", content: "x" }])) chunks.push(c)
      },
    )
    expect(body?.stream_options).toEqual({ include_usage: true })
    expect(chunks.find((c) => c.type === "done")?.usage).toEqual({ inputTokens: 120, outputTokens: 3, totalTokens: 123 })
  })

  test("OpenAI 兼容流：usage 携带 prompt_tokens_details.cached_tokens 时提取缓存命中（含在 input 内）", async () => {
    const chunks: LLMChunk[] = []
    await withFetch(
      async () =>
        new Response(
          'data: {"choices":[{"delta":{"content":"hi"},"finish_reason":"stop"}]}\n\ndata: {"choices":[],"usage":{"prompt_tokens":120,"completion_tokens":3,"total_tokens":123,"prompt_tokens_details":{"cached_tokens":100}}}\n\ndata: [DONE]\n\n',
          { status: 200 },
        ),
      async () => {
        for await (const c of provider().chat([{ role: "user", content: "x" }])) chunks.push(c)
      },
    )
    expect(chunks.find((c) => c.type === "done")?.usage).toEqual({ inputTokens: 120, outputTokens: 3, totalTokens: 123, cachedTokens: 100 })
  })

  test("OpenAI 兼容流：服务端不返回 usage 时 done 不带 usage（估算兜底）", async () => {
    const chunks: LLMChunk[] = []
    await withFetch(
      async () => new Response(OK_STREAM, { status: 200 }),
      async () => {
        for await (const c of provider().chat([{ role: "user", content: "x" }])) chunks.push(c)
      },
    )
    const done = chunks.find((c) => c.type === "done")
    expect(done).toBeDefined()
    expect(done!.usage).toBeUndefined()
  })
})

describe("extraParams 额外模型接口参数", () => {
  test("parseExtraParams：合法 JSON 对象解析成功", () => {
    expect(parseExtraParams('{"reasoning_effort":"high","temperature":0.5}')).toEqual({ reasoning_effort: "high", temperature: 0.5 })
    expect(parseExtraParams(undefined)).toEqual({})
    expect(parseExtraParams("")).toEqual({})
  })

  test("parseExtraParams：非法 JSON 抛错", () => {
    expect(() => parseExtraParams("{oops")).toThrow(/不是合法 JSON/)
  })

  test("parseExtraParams：非对象（数组/标量）抛错", () => {
    expect(() => parseExtraParams("[1,2]")).toThrow(/必须是 JSON 对象/)
    expect(() => parseExtraParams('"str"')).toThrow(/必须是 JSON 对象/)
    expect(() => parseExtraParams("42")).toThrow(/必须是 JSON 对象/)
  })

  test("OpenAI：Provider 级 extraParams 合并进请求体顶层", async () => {
    const p = createProvider({ apiKind: "openai", apiBase: "https://api.test", apiKey: "k", model: "m", maxContextTokens: 10000, multimodal: false, extraParams: { reasoning_effort: "high" } })
    let body: Record<string, unknown> = {}
    await withFetch(
      async (_url, init) => {
        body = JSON.parse(String((init as RequestInit).body))
        return new Response(OK_STREAM, { status: 200 })
      },
      async () => {
        for await (const _ of p.chat([{ role: "user", content: "x" }])) void _
      },
    )
    expect(body?.reasoning_effort).toBe("high")
  })

  test("OpenAI：调用级 extraParams 覆盖 Provider 级", async () => {
    const p = createProvider({ apiKind: "openai", apiBase: "https://api.test", apiKey: "k", model: "m", maxContextTokens: 10000, multimodal: false, extraParams: { reasoning_effort: "low" } })
    let body: Record<string, unknown> = {}
    await withFetch(
      async (_url, init) => {
        body = JSON.parse(String((init as RequestInit).body))
        return new Response(OK_STREAM, { status: 200 })
      },
      async () => {
        for await (const _ of p.chat([{ role: "user", content: "x" }], { extraParams: { reasoning_effort: "high" } })) void _
      },
    )
    expect(body?.reasoning_effort).toBe("high")
    expect(body?.model).toBe("m") // 基础字段保留
  })

  test("Anthropic：extraParams 合并进请求体顶层", async () => {
    const p = createProvider({ apiKind: "anthropic", apiBase: "https://api.test", apiKey: "k", model: "m", maxContextTokens: 10000, multimodal: false })
    let body: Record<string, unknown> = {}
    await withFetch(
      async (_url, init) => {
        body = JSON.parse(String((init as RequestInit).body))
        return new Response('data: {"type":"message_delta","delta":{"stop_reason":"end_turn"}}\n\n', { status: 200 })
      },
      async () => {
        for await (const _ of p.chat([{ role: "user", content: "x" }], { extraParams: { thinking: { type: "enabled", budget_tokens: 2000 } } })) void _
      },
    )
    expect(body?.thinking).toEqual({ type: "enabled", budget_tokens: 2000 })
    expect(body?.max_tokens).toBe(8192) // 默认值保留（大文件生成截断防护缺省）
  })

  test("OpenAI：无 extraParams 时请求体不受影响", async () => {
    let body: Record<string, unknown> = {}
    await withFetch(
      async (_url, init) => {
        body = JSON.parse(String((init as RequestInit).body))
        return new Response(OK_STREAM, { status: 200 })
      },
      async () => {
        for await (const _ of provider().chat([{ role: "user", content: "x" }])) void _
      },
    )
    expect(body).not.toHaveProperty("reasoning_effort")
    expect(body?.messages).toHaveLength(1)
  })

  test("Anthropic：message_start input_tokens + message_delta output_tokens 挂到 done", async () => {
    const p = createProvider({ apiKind: "anthropic", apiBase: "https://api.test", apiKey: "k", model: "m", maxContextTokens: 10000, multimodal: false })
    const stream = [
      'data: {"type":"message_start","message":{"usage":{"input_tokens":200,"output_tokens":1}}}',
      'data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}',
      'data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"hi"}}',
      'data: {"type":"content_block_stop","index":0}',
      'data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}}',
    ].join("\n\n")
    const chunks: LLMChunk[] = []
    await withFetch(
      async () => new Response(stream, { status: 200 }),
      async () => {
        for await (const c of p.chat([{ role: "user", content: "x" }])) chunks.push(c)
      },
    )
    const done = chunks.filter((c) => c.type === "done")
    expect(done[0]?.usage).toEqual({ inputTokens: 200, outputTokens: 5 })
  })

  test("Anthropic：cache_read_input_tokens 折算并入 inputTokens（cached ⊆ input 统一口径）", async () => {
    const p = createProvider({ apiKind: "anthropic", apiBase: "https://api.test", apiKey: "k", model: "m", maxContextTokens: 10000, multimodal: false })
    const stream = [
      'data: {"type":"message_start","message":{"usage":{"input_tokens":200,"cache_read_input_tokens":800,"cache_creation_input_tokens":50}}}',
      'data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}}',
    ].join("\n\n")
    const chunks: LLMChunk[] = []
    await withFetch(
      async () => new Response(stream, { status: 200 }),
      async () => {
        for await (const c of p.chat([{ role: "user", content: "x" }])) chunks.push(c)
      },
    )
    const done = chunks.filter((c) => c.type === "done")
    expect(done[0]?.usage).toEqual({ inputTokens: 1000, outputTokens: 5, cachedTokens: 800 })
  })
})

describe("接口地址拼接（apiBase 支持服务根地址与完整 endpoint 两种写法）", () => {
  test("endpointUrl：根地址追加 endpoint、尾部斜杠剥净、完整 endpoint 原样使用", () => {
    expect(endpointUrl("https://open.bigmodel.cn/api/paas/v4", "/chat/completions")).toBe("https://open.bigmodel.cn/api/paas/v4/chat/completions")
    expect(endpointUrl("https://open.bigmodel.cn/api/paas/v4/", "/chat/completions")).toBe("https://open.bigmodel.cn/api/paas/v4/chat/completions")
    expect(endpointUrl("https://open.bigmodel.cn/api/paas/v4//", "/chat/completions")).toBe("https://open.bigmodel.cn/api/paas/v4/chat/completions")
    // 用户直接粘贴完整接口地址（文档给到的形态）：不再双拼 /chat/completions
    expect(endpointUrl("https://open.bigmodel.cn/api/paas/v4/chat/completions", "/chat/completions")).toBe("https://open.bigmodel.cn/api/paas/v4/chat/completions")
    expect(endpointUrl("https://api.anthropic.com/v1/messages", "/v1/messages")).toBe("https://api.anthropic.com/v1/messages")
    expect(endpointUrl("https://api.anthropic.com", "/v1/messages")).toBe("https://api.anthropic.com/v1/messages")
  })

  test("OpenAI 兼容：apiBase 配完整 endpoint（智谱示例）时请求地址原样、不双拼", async () => {
    const full = "https://open.bigmodel.cn/api/paas/v4/chat/completions"
    const p = createProvider({ ...BASE_CFG, apiBase: full })
    let hitUrl = ""
    await withFetch(
      async (url) => {
        hitUrl = String(url)
        return new Response(OK_STREAM, { status: 200 })
      },
      async () => {
        for await (const _ of p.chat([{ role: "user", content: "x" }])) void _
      },
    )
    expect(hitUrl).toBe(full)
  })

  test("OpenAI 兼容：apiBase 配服务根地址时追加 /chat/completions（含尾部斜杠归一）", async () => {
    const p = createProvider({ ...BASE_CFG, apiBase: "https://open.bigmodel.cn/api/paas/v4/" })
    let hitUrl = ""
    await withFetch(
      async (url) => {
        hitUrl = String(url)
        return new Response(OK_STREAM, { status: 200 })
      },
      async () => {
        for await (const _ of p.chat([{ role: "user", content: "x" }])) void _
      },
    )
    expect(hitUrl).toBe("https://open.bigmodel.cn/api/paas/v4/chat/completions")
  })

  test("Anthropic：apiBase 配完整 /v1/messages 时原样使用", async () => {
    const full = "https://api.anthropic.com/v1/messages"
    const p = createProvider({ apiKind: "anthropic", apiBase: full, apiKey: "k", model: "m", maxContextTokens: 10000, multimodal: false })
    let hitUrl = ""
    await withFetch(
      async (url) => {
        hitUrl = String(url)
        return new Response('data: {"type":"message_delta","delta":{"stop_reason":"end_turn"}}\n\n', { status: 200 })
      },
      async () => {
        for await (const _ of p.chat([{ role: "user", content: "x" }])) void _
      },
    )
    expect(hitUrl).toBe(full)
  })
})

describe("多模态图片内容块转换", () => {
  test("OpenAI：统一 image 块转为 image_url data URL", async () => {
    let body: Record<string, unknown> = {}
    await withFetch(
      async (_url, init) => {
        body = JSON.parse(String((init as RequestInit).body))
        return new Response(OK_STREAM, { status: 200 })
      },
      async () => {
        for await (const _ of provider().chat([{ role: "user", content: imageMessageBlocks("看看这张图", "image/png", "QUJD") }])) void _
      },
    )
    const content = (body?.messages as Array<Record<string, unknown>>)[0].content as Array<Record<string, unknown>>
    expect(content).toEqual([
      { type: "text", text: "看看这张图" },
      { type: "image_url", image_url: { url: "data:image/png;base64,QUJD" } },
    ])
  })

  test("Anthropic：统一 image 块转为 base64 image 块", async () => {
    const p = createProvider({ apiKind: "anthropic", apiBase: "https://api.test", apiKey: "k", model: "m", maxContextTokens: 10000, multimodal: false })
    let body: Record<string, unknown> = {}
    await withFetch(
      async (_url, init) => {
        body = JSON.parse(String((init as RequestInit).body))
        return new Response('data: {"type":"message_delta","delta":{"stop_reason":"end_turn"}}\n\n', { status: 200 })
      },
      async () => {
        for await (const _ of p.chat([{ role: "user", content: imageMessageBlocks("看看这张图", "image/webp", "QUJD") }])) void _
      },
    )
    const content = (body?.messages as Array<Record<string, unknown>>)[0].content as Array<Record<string, unknown>>
    expect(content).toEqual([
      { type: "text", text: "看看这张图" },
      { type: "image", source: { type: "base64", media_type: "image/webp", data: "QUJD" } },
    ])
  })

  test("OpenAI：纯文本数组与字符串消息行为不变", async () => {
    let body: Record<string, unknown> = {}
    await withFetch(
      async (_url, init) => {
        body = JSON.parse(String((init as RequestInit).body))
        return new Response(OK_STREAM, { status: 200 })
      },
      async () => {
        for await (const _ of provider().chat([{ role: "user", content: [{ type: "text", text: "你好" }] }])) void _
      },
    )
    expect((body?.messages as Array<Record<string, unknown>>)[0].content).toEqual([{ type: "text", text: "你好" }])
  })
})

function responsesProvider() {
  return createProvider({ apiKind: "responses", apiBase: "https://api.test/v1", apiKey: "k", model: "m", maxContextTokens: 10000, multimodal: false })
}

describe("OpenAI Responses Provider", () => {
  test("请求体与消息转换：function_call / function_call_output item、tools 扁平格式、max_output_tokens", async () => {
    let body: Record<string, unknown> = {}
    let url = ""
    await withFetch(
      async (u, init) => {
        url = String(u)
        body = JSON.parse(String((init as RequestInit).body))
        return new Response('data: {"type":"response.completed","response":{"status":"completed","output":[]}}\n\n', { status: 200 })
      },
      async () => {
        const msgs: MessageLike[] = [
          { role: "system", content: "sys" },
          { role: "user", content: "你好" },
          { role: "assistant", content: "", toolCalls: [{ id: "call_1", name: "get_weather", arguments: '{"city":"北京"}' }] },
          { role: "tool", content: "晴", toolCallId: "call_1", name: "get_weather" },
        ]
        for await (const _ of responsesProvider().chat(msgs, {
          tools: [{ name: "get_weather", description: "查天气", parameters: { type: "object" } }],
          maxTokens: 100,
        })) void _
      },
    )
    expect(url).toBe("https://api.test/v1/responses")
    expect(body?.model).toBe("m")
    expect(body?.stream).toBe(true)
    expect(body?.max_output_tokens).toBe(100)
    expect(body?.tools).toEqual([{ type: "function", name: "get_weather", description: "查天气", parameters: { type: "object" } }])
    expect(body?.input).toEqual([
      { role: "system", content: "sys" },
      { role: "user", content: "你好" },
      { role: "assistant", content: "" },
      { type: "function_call", call_id: "call_1", name: "get_weather", arguments: '{"city":"北京"}' },
      { type: "function_call_output", call_id: "call_1", output: "晴" },
    ])
  })

  test("SSE 流式解析：文本增量、函数调用跨 chunk 聚合、stop reason 取 last message finish_reason", async () => {
    const stream = [
      'data: {"type":"response.output_item.added","output_index":0,"item":{"id":"fc_1","call_id":"call_1","type":"function_call","name":"get_weather","status":"in_progress"}}',
      'data: {"type":"response.function_call_arguments.delta","item_id":"fc_1","delta":"{\\"city\\":\\"北"}',
      'data: {"type":"response.function_call_arguments.delta","item_id":"fc_1","delta":"京\\"}"}',
      'data: {"type":"response.function_call_arguments.done","item_id":"fc_1","arguments":"{\\"city\\":\\"北京\\"}"}',
      'data: {"type":"response.output_text.delta","item_id":"msg_1","delta":"正在查询"}',
      'data: {"type":"response.output_text.delta","item_id":"msg_1","delta":"天气"}',
      'data: {"type":"response.completed","response":{"id":"r1","status":"completed","output":[{"type":"message","finish_reason":"tool_calls"}]}}',
      "data: [DONE]",
    ].join("\n\n")
    const chunks: unknown[] = []
    await withFetch(
      async () => new Response(stream, { status: 200 }),
      async () => {
        for await (const c of responsesProvider().chat([{ role: "user", content: "x" }])) chunks.push(c)
      },
    )
    expect(chunks).toEqual([
      { type: "tool_call", toolCall: { id: "call_1", name: "get_weather", arguments: { city: "北京" } } },
      { type: "text", text: "正在查询" },
      { type: "text", text: "天气" },
      { type: "done", stopReason: "tool_calls" },
    ])
  })

  test("reasoning_summary_text delta 输出为推理内容", async () => {
    const chunks: string[] = []
    await withFetch(
      async () =>
        new Response(
          'data: {"type":"response.reasoning_summary_text.delta","item_id":"rs_1","delta":"推理过程"}\n\ndata: {"type":"response.completed","response":{"status":"completed","output":[]}}\n\n',
          { status: 200 },
        ),
      async () => {
        for await (const c of responsesProvider().chat([{ role: "user", content: "x" }])) if (c.type === "reasoning") chunks.push(c.text!)
      },
    )
    expect(chunks).toEqual(["推理过程"])
  })

  test("usage 真值：completed 的 response.usage 挂到 done chunk", async () => {
    const stream = 'data: {"type":"response.output_text.delta","item_id":"m1","delta":"hi"}\n\ndata: {"type":"response.completed","response":{"status":"completed","output":[],"usage":{"input_tokens":80,"output_tokens":4,"total_tokens":84}}}\n\n'
    const chunks: LLMChunk[] = []
    await withFetch(
      async () => new Response(stream, { status: 200 }),
      async () => {
        for await (const c of responsesProvider().chat([{ role: "user", content: "x" }])) chunks.push(c)
      },
    )
    const done = chunks.find((c) => c.type === "done")
    expect(done?.usage).toEqual({ inputTokens: 80, outputTokens: 4, totalTokens: 84 })
  })

  test("usage 真值：input_tokens_details.cached_tokens 提取缓存命中（含在 input 内）", async () => {
    const stream = 'data: {"type":"response.completed","response":{"status":"completed","output":[],"usage":{"input_tokens":80,"output_tokens":4,"total_tokens":84,"input_tokens_details":{"cached_tokens":64}}}}\n\n'
    const chunks: LLMChunk[] = []
    await withFetch(
      async () => new Response(stream, { status: 200 }),
      async () => {
        for await (const c of responsesProvider().chat([{ role: "user", content: "x" }])) chunks.push(c)
      },
    )
    const done = chunks.find((c) => c.type === "done")
    expect(done?.usage).toEqual({ inputTokens: 80, outputTokens: 4, totalTokens: 84, cachedTokens: 64 })
  })

  test("多模态：统一 image 块转为 image_url data URL", async () => {
    let body: Record<string, unknown> = {}
    await withFetch(
      async (_u, init) => {
        body = JSON.parse(String((init as RequestInit).body))
        return new Response('data: {"type":"response.completed","response":{"status":"completed","output":[]}}\n\n', { status: 200 })
      },
      async () => {
        for await (const _ of responsesProvider().chat([{ role: "user", content: imageMessageBlocks("看图", "image/png", "QUJD") }])) void _
      },
    )
    const input = body?.input as Array<Record<string, unknown>>
    expect(input[0].content).toEqual([
      { type: "text", text: "看图" },
      { type: "image_url", image_url: { url: "data:image/png;base64,QUJD" } },
    ])
  })

  test("extraParams 顶层合并；completed 缺失时兜底 done；response.failed 抛错", async () => {
    let body: Record<string, unknown> = {}
    const chunks: string[] = []
    await withFetch(
      async (_u, init) => {
        body = JSON.parse(String((init as RequestInit).body))
        return new Response('data: {"type":"response.output_text.delta","item_id":"m1","delta":"ok"}\n\n', { status: 200 })
      },
      async () => {
        const p = createProvider({ apiKind: "responses", apiBase: "https://api.test/v1", apiKey: "k", model: "m", maxContextTokens: 10000, multimodal: false, extraParams: { reasoning_effort: "high" } })
        for await (const c of p.chat([{ role: "user", content: "x" }])) if (c.type === "text") chunks.push(c.text!)
      },
    )
    expect(body?.reasoning_effort).toBe("high")
    expect(body?.model).toBe("m")

    let error: unknown = null
    await withFetch(
      async () => new Response('data: {"type":"response.failed","error":{"message":"overloaded"}}\n\n', { status: 200 }),
      async () => {
        try {
          for await (const _ of responsesProvider().chat([{ role: "user", content: "x" }])) void _
        } catch (e) {
          error = e
        }
      },
    )
    expect(String((error as Error)?.message)).toContain("overloaded")
  })

  test("HTTP 错误：4xx 不重试，抛出中文错误", async () => {
    let calls = 0
    await withFetch(
      async () => {
        calls++
        return new Response("bad key", { status: 401 })
      },
      async () => {
        const run = (async () => {
          for await (const _ of responsesProvider().chat([{ role: "user", content: "x" }])) void _
        })()
        await expect(run).rejects.toThrow(/模型接口错误（HTTP 401）/)
      },
    )
    expect(calls).toBe(1)
  })
})

describe("任务级模型配置覆盖（前端/会话 env → 实际使用模型）", () => {
  test("modelEnvOverrides：无覆盖键返回 null（沿用启动 Provider）", () => {
    expect(modelEnvOverrides(undefined)).toBeNull()
    expect(modelEnvOverrides({})).toBeNull()
    expect(modelEnvOverrides({ CODE_PROJECTS: "[x]" })).toBeNull()
  })

  test("modelEnvOverrides：提取 GEBAI_LLM_* 覆盖键，非法值忽略", () => {
    const o = modelEnvOverrides({ GEBAI_LLM_MODEL: "gpt-x", GEBAI_LLM_API_BASE: "https://x", GEBAI_LLM_API_KEY: "sk-x", GEBAI_LLM_API_KIND: "anthropic", GEBAI_LLM_MAX_CONTEXT: "64000", GEBAI_LLM_MULTIMODAL: "true" })
    expect(o).toEqual({ model: "gpt-x", apiBase: "https://x", apiKey: "sk-x", apiKind: "anthropic", maxContextTokens: 64000, multimodal: true })
    // 非法类型/非法数字/非法多模态值忽略（不覆盖）
    expect(modelEnvOverrides({ GEBAI_LLM_API_KIND: "garbage" })).toBeNull()
    expect(modelEnvOverrides({ GEBAI_LLM_MAX_CONTEXT: "abc" })).toBeNull()
    expect(modelEnvOverrides({ GEBAI_LLM_MAX_CONTEXT: "-5" })).toBeNull()
    expect(modelEnvOverrides({ GEBAI_LLM_MAX_OUTPUT_TOKENS: "16384" })).toEqual({ maxOutputTokens: 16384 })
    expect(modelEnvOverrides({ GEBAI_LLM_MAX_OUTPUT_TOKENS: "abc" })).toBeNull()
    // MULTIMODAL 显式 false 也生效（关闭多模态内联）
    expect(modelEnvOverrides({ GEBAI_LLM_MULTIMODAL: "false" })).toEqual({ multimodal: false })
  })

  test("applyModelEnvOverrides：无覆盖返回原引用；有覆盖合并出新配置", () => {
    expect(applyModelEnvOverrides(BASE_CFG, undefined)).toBe(BASE_CFG)
    const merged = applyModelEnvOverrides(BASE_CFG, { GEBAI_LLM_MODEL: "gpt-x" })
    expect(merged).not.toBe(BASE_CFG)
    expect(merged.model).toBe("gpt-x")
    expect(merged.apiBase).toBe(BASE_CFG.apiBase) // 未覆盖项继承启动配置
    expect(merged.multimodal).toBe(false)
  })

  test("resolveVisionProvider：无视觉配置且主模型无多模态 → null", () => {
    expect(resolveVisionProvider(BASE_CFG, null, undefined)).toBeNull()
  })

  test("resolveVisionProvider：任务级 GEBAI_LLM_MULTIMODAL=true 回落到主模型", () => {
    const p = resolveVisionProvider(BASE_CFG, null, { GEBAI_LLM_MULTIMODAL: "true" })
    expect(p).not.toBeNull()
    expect(p!.capabilities().multimodal).toBe(true)
  })

  test("resolveVisionProvider：任务级 GEBAI_VISION_MODEL 重建视觉 Provider（继承主模型缺省）", async () => {
    let captured: string | undefined
    await withFetch(
      async (url) => {
        captured = String(url)
        return new Response('data: {"choices":[{"delta":{"content":"hi"},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n', { status: 200 })
      },
      async () => {
        const p = resolveVisionProvider(BASE_CFG, null, { GEBAI_VISION_MODEL: "gpt-vision" })
        expect(p).not.toBeNull()
        for await (const _ of p!.chat([{ role: "user", content: "x" }])) void _
      },
    )
    expect(captured).toContain("https://api.test") // 接口地址缺省继承主模型
  })

  test("resolveVisionProvider：任务级 GEBAI_VISION_* 独立配置覆盖，非法 EXTRA_PARAMS 忽略", () => {
    const p = resolveVisionProvider(BASE_CFG, null, {
      GEBAI_VISION_MODEL: "v2",
      GEBAI_VISION_API_BASE: "https://vision.test",
      GEBAI_VISION_API_KEY: "vk",
      GEBAI_VISION_API_KIND: "anthropic",
      GEBAI_VISION_MAX_CONTEXT: "8000",
      GEBAI_VISION_EXTRA_PARAMS: "not-json",
    })
    expect(p).not.toBeNull()
    expect(p!.id).toBe("anthropic")
    const c = p!.capabilities()
    expect(c.maxContextTokens).toBe(8000)
    expect(c.multimodal).toBe(true)
  })

  test("resolveVisionProvider：启动已配视觉模型时任务级覆盖生效", async () => {
    const visionCfg: ProviderConfig = { apiKind: "openai", apiBase: "https://base-vision", apiKey: "k2", model: "v0", maxContextTokens: 20000, multimodal: true }
    let url = ""
    let body = ""
    await withFetch(
      async (u, init) => {
        url = String(u)
        body = String(init?.body)
        return new Response('data: {"choices":[{"delta":{"content":"hi"},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n', { status: 200 })
      },
      async () => {
        const p = resolveVisionProvider(BASE_CFG, visionCfg, { GEBAI_VISION_MODEL: "v1" })
        for await (const _ of p!.chat([{ role: "user", content: "x" }])) void _
      },
    )
    expect(url).toContain("https://base-vision") // 未覆盖的地址继承启动视觉配置
    expect(JSON.parse(body).model).toBe("v1") // 覆盖的模型生效
  })
})

describe("大文件生成截断防护（输出上限截断与参数抢救）", () => {
  test("salvageWriteArgs：截断的 write 参数前缀提取 path 与已生成 content", () => {
    // content 字符串中途截断（未闭合），JSON 转义解码（源串含字面 \n 转义，期望值为真实换行）
    expect(salvageWriteArgs('{"path":"a.txt","content":"line1\\nline2')).toEqual({ path: "a.txt", content: "line1\nline2" })
    // 尾部不完整转义序列丢弃（孤立反斜杠 / 不完整 \uXXXX）
    expect(salvageWriteArgs('{"path":"a.txt","content":"abc\\')).toEqual({ path: "a.txt", content: "abc" })
    expect(salvageWriteArgs('{"path":"a.txt","content":"ab\\u12')).toEqual({ path: "a.txt", content: "ab" })
    // 完整 \uXXXX 解码
    expect(salvageWriteArgs('{"path":"a.txt","content":"\\u4f60\\u597d')).toEqual({ path: "a.txt", content: "你好" })
    // 其他标量键（append:true）可跳过；path/content 顺序无关
    expect(salvageWriteArgs('{"append":true,"path":"b.md","content":"# t')).toEqual({ path: "b.md", content: "# t" })
    // content 后完整闭合再截断（值本身已完整）
    expect(salvageWriteArgs('{"path":"c.txt","content":"done"')).toEqual({ path: "c.txt", content: "done" })
  })

  test("salvageWriteArgs：path 截断/缺 content/空 content/非法转义/非对象返回 null", () => {
    expect(salvageWriteArgs('{"path":"a.t')).toBeNull() // path 值截断：目标不明确，不抢救
    expect(salvageWriteArgs('{"path":"a.txt","con')).toBeNull() // content 键截断
    expect(salvageWriteArgs('{"path":"a.txt","content":"')).toBeNull() // content 空（截断在内容起点，无可抢救内容）
    expect(salvageWriteArgs('{"path":"a.txt","content":"a\\q')).toBeNull() // 非法转义：整体放弃防脏数据落盘
    expect(salvageWriteArgs("content=abc")).toBeNull() // 非 JSON 对象前缀
    expect(salvageWriteArgs('{"path":"only.txt"}')).toBeNull() // 缺 content
  })

  test("OpenAI：finish_reason=length 如实传出（不再吞为 stop）；截断参数携带原始全文 raw", async () => {
    const stream = [
      'data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"write","arguments":"{\\"path\\":\\"a.txt\\",\\"content\\":\\"abc"}}]}}]}',
      'data: {"choices":[{"delta":{},"finish_reason":"length"}]}',
      "data: [DONE]",
    ].join("\n\n")
    const chunks: LLMChunk[] = []
    await withFetch(
      async () => new Response(stream, { status: 200 }),
      async () => {
        for await (const c of provider().chat([{ role: "user", content: "x" }])) chunks.push(c)
      },
    )
    expect(chunks.find((c) => c.type === "done")?.stopReason).toBe("length")
    const tc = chunks.find((c) => c.type === "tool_call")
    expect(tc?.toolArgsError).toBeTruthy()
    expect(tc?.toolCall?.raw).toBe('{"path":"a.txt","content":"abc')
    expect(tc?.toolCall?.arguments).toEqual({})
  })

  test("Responses：response.incomplete（max_output_tokens）→ done stopReason=length", async () => {
    const stream = 'data: {"type":"response.incomplete","response":{"status":"incomplete","incomplete_details":{"reason":"max_output_tokens"}}}\n\n'
    const chunks: LLMChunk[] = []
    await withFetch(
      async () => new Response(stream, { status: 200 }),
      async () => {
        for await (const c of responsesProvider().chat([{ role: "user", content: "x" }])) chunks.push(c)
      },
    )
    const done = chunks.filter((c) => c.type === "done")
    expect(done[0]?.stopReason).toBe("length")
    expect(done).toHaveLength(1)
  })

  test("Anthropic：max_tokens 缺省 8192，配置 maxOutputTokens 优先，调用级覆盖最高", async () => {
    let body: Record<string, unknown> = {}
    const anthropic = (cfg?: Partial<ProviderConfig>) =>
      createProvider({ apiKind: "anthropic", apiBase: "https://api.test", apiKey: "k", model: "m", maxContextTokens: 10000, multimodal: false, ...cfg })
    const run = (cfg?: Partial<ProviderConfig>, opts?: { maxTokens?: number }) =>
      withFetch(async (_u, init) => {
        body = JSON.parse(String((init as RequestInit).body))
        return new Response('data: {"type":"message_delta","delta":{"stop_reason":"end_turn"}}\n\n', { status: 200 })
      }, async () => {
        for await (const _ of anthropic(cfg).chat([{ role: "user", content: "x" }], opts)) void _
      })
    await run()
    expect(body.max_tokens).toBe(8192)
    await run({ maxOutputTokens: 32000 })
    expect(body.max_tokens).toBe(32000)
    await run({ maxOutputTokens: 32000 }, { maxTokens: 2048 })
    expect(body.max_tokens).toBe(2048)
  })

  test("OpenAI/Responses：maxOutputTokens 配置作为 max_tokens/max_output_tokens 请求值", async () => {
    let body: Record<string, unknown> = {}
    await withFetch(async (_u, init) => {
      body = JSON.parse(String((init as RequestInit).body))
      return new Response(OK_STREAM, { status: 200 })
    }, async () => {
      for await (const _ of createProvider({ ...BASE_CFG, maxOutputTokens: 9000 }).chat([{ role: "user", content: "x" }])) void _
    })
    expect(body.max_tokens).toBe(9000)
    await withFetch(async (_u, init) => {
      body = JSON.parse(String((init as RequestInit).body))
      return new Response('data: {"type":"response.completed","response":{"status":"completed","output":[]}}\n\n', { status: 200 })
    }, async () => {
      for await (const _ of createProvider({ ...BASE_CFG, apiKind: "responses", maxOutputTokens: 9000 }).chat([{ role: "user", content: "x" }])) void _
    })
    expect(body.max_output_tokens).toBe(9000)
  })
})
