import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"
import type { LLMProvider, LLMChunk, ChatOptions } from "./core/llm"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"

class SseFake implements LLMProvider {
  readonly id = "fake"
  calls = 0
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 1000 }
  }
  async *chat(_msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.calls++
    if (this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-1", name: "ls", arguments: {} } }
      yield { type: "done" }
      return
    }
    yield { type: "text", text: "已获取时间" }
    yield { type: "done" }
  }
}

const home = mkdtempSync(join(tmpdir(), "gebai-sse-"))
let handle: ServerHandle

beforeAll(async () => {
  handle = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
  ;(handle.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = new SseFake()
})

afterAll(() => {
  handle.gc?.stop()
  handle.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

function base() {
  return `http://127.0.0.1:${handle.server.port}`
}

describe("prompt REST contract（非流式 JSON）", () => {
  test("runs the task and returns the final assistant message", async () => {
    const s = await (await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({}) })).json() as { id: string }
    const res = await fetch(`${base()}/api/v1/sessions/${s.id}/prompt`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ prompt: "what time is it" }),
    })
    expect(res.status).toBe(200)
    const body = (await res.json()) as { message: { id: string; content: string; createdAt: number } | null; error?: string }
    expect(body.error).toBeUndefined()
    expect(body.message?.content).toContain("已获取时间")
    // 消息已持久化（与历史一致）
    const detail = (await (await fetch(`${base()}/api/v1/sessions/${s.id}`)).json()) as { messages: Array<{ role: string; content: string }> }
    const last = detail.messages[detail.messages.length - 1]
    expect(last.role).toBe("assistant")
    expect(last.content).toBe(body.message!.content)
  })

  test("task failure returns error JSON instead of streaming error chunk", async () => {
    // provider 抛错 → engine.run reject → { error }（非 SSE error chunk）
    const failing = { ...((handle.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider), chat: async function* () { throw new Error("LLM 接口失败") } } as unknown as LLMProvider
    const prev = (handle.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider
    ;(handle.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = failing
    try {
      const s = await (await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({}) })).json() as { id: string }
      const res = await fetch(`${base()}/api/v1/sessions/${s.id}/prompt`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ prompt: "hi" }),
      })
      expect(res.status).toBe(200)
      const body = (await res.json()) as { message?: unknown; error?: string }
      // 任务错误（LLM 接口失败）：无 assistant 消息，返回 error 字段（非 SSE error chunk）
      expect(body.message).toBeNull()
      expect(body.error).toBeTruthy()
    } finally {
      ;(handle.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = prev
    }
  })

  test("engine-level errors (missing session) return 200 with error field", async () => {
    // 会话不存在（合法 id 格式）：engine.run 抛错 → catch 分支（与任务错误同一返回形态）
    const res = await fetch(`${base()}/api/v1/sessions/00000000000000000000000000000000/prompt`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ prompt: "hi" }),
    })
    expect(res.status).toBe(200)
    const body = (await res.json()) as { message?: unknown; error?: string }
    expect(body.error).toBeTruthy()
    // 非法 id（穿越形态）：中间件白名单直接 400
    const bad = await fetch(`${base()}/api/v1/sessions/no-such-session/prompt`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ prompt: "hi" }),
    })
    expect(bad.status).toBe(400)
  })

  test("stream=true 时发布流式 delta 事件（输出方式请求层配置）", async () => {
    const s = await (await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({}) })).json() as { id: string }
    const deltas: string[] = []
    const unsub = handle.events.subscribe((ev) => {
      if (ev.sessionId === s.id && ev.type === "event.message.delta") deltas.push(String((ev.payload as { text?: unknown }).text))
    })
    try {
      const res = await fetch(`${base()}/api/v1/sessions/${s.id}/prompt`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ prompt: "what time is it", stream: true }),
      })
      expect(res.status).toBe(200)
      // 流式输出：文本增量事件已发布（REST 同步响应之外，接入方可经事件订阅消费流）
      expect(deltas.join("")).toContain("已获取时间")
    } finally {
      unsub()
    }
  })

  test("默认（仅最终响应）不发布文本增量；interactionMode 非法值返回 400", async () => {
    const s = await (await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({}) })).json() as { id: string }
    const deltas: string[] = []
    const unsub = handle.events.subscribe((ev) => {
      if (ev.sessionId === s.id && ev.type === "event.message.delta") deltas.push(String((ev.payload as { text?: unknown }).text))
    })
    try {
      const res = await fetch(`${base()}/api/v1/sessions/${s.id}/prompt`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ prompt: "what time is it" }),
      })
      expect(res.status).toBe(200)
      // 仅最终响应：无文本增量事件（结构化事件仍推送）
      expect(deltas).toEqual([])
    } finally {
      unsub()
    }
    // 非法 interactionMode → 400
    const bad = await fetch(`${base()}/api/v1/sessions/${s.id}/prompt`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ prompt: "hi", interactionMode: "bogus" }),
    })
    expect(bad.status).toBe(400)
    expect(((await bad.json()) as { error?: string }).error).toContain("interactionMode 非法")
  })
})
