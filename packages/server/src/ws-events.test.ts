import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"
import { SessionStore } from "./core/session/store"
import type { LLMProvider, LLMChunk, ChatOptions } from "./core/llm/llm"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"

class WsFake implements LLMProvider {
  readonly id = "fake"
  calls = 0
  toolName = "ls"
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 1000 }
  }
  async *chat(_msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.calls++
    if (this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-1", name: this.toolName, arguments: {} } }
      yield { type: "done" }
      return
    }
    yield { type: "text", text: "已获取时间" }
    yield { type: "done" }
  }
}

const home = mkdtempSync(join(tmpdir(), "gebai-ws-"))
let handle: ServerHandle

beforeAll(async () => {
  handle = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
  ;(handle.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = new WsFake()
})

afterAll(() => {
  handle.gc?.stop()
  handle.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

function base() {
  return `http://127.0.0.1:${handle.server.port}`
}

async function waitFor(cond: () => boolean, timeout = 3000): Promise<void> {
  const start = Date.now()
  while (!cond()) {
    if (Date.now() - start > timeout) throw new Error("timeout waiting for WS events")
    await new Promise((r) => setTimeout(r, 20))
  }
}

describe("WS event push", () => {
  test("pushes engine events for owned sessions", async () => {
    const s = (await (
      await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({}) })
    ).json()) as { id: string }

    const ws = new WebSocket(`ws://127.0.0.1:${handle.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws connect failed"))
    })
    const events: Array<{ type: string; payload: Record<string, unknown> }> = []
    ws.onmessage = (m) => {
      const data = JSON.parse(String(m.data)) as { type: string; payload: Record<string, unknown> }
      if (data.type.startsWith("event.")) events.push(data)
    }

    const res = await fetch(`${base()}/api/v1/sessions/${s.id}/prompt`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ prompt: "what time is it", stream: true }),
    })
    expect(res.status).toBe(200)
    await res.text()

    await waitFor(() => events.some((e) => e.type === "event.task.done"))
    const types = events.map((e) => e.type)
    expect(types).toContain("event.tool.call")
    expect(types).toContain("event.tool.result")
    expect(types).toContain("event.message.delta")
    expect(types).toContain("event.task.done")
    // 事件 payload 携带目标会话 id（SDK onEvent 依赖 payload.sessionId）
    for (const e of events) expect(e.payload.sessionId).toBe(s.id)
    ws.close()
  })

  test("filters out events for sessions the connection cannot access", async () => {
    const ws = new WebSocket(`ws://127.0.0.1:${handle.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws connect failed"))
    })
    const events: string[] = []
    ws.onmessage = (m) => {
      const data = JSON.parse(String(m.data)) as { type: string }
      if (data.type.startsWith("event.")) events.push(data.type)
    }
    // 发布一个指向不存在会话的伪造事件：store.load 校验失败，不得推送
    handle.events.publish({
      type: "event.tool.call",
      sessionId: "no-such-session",
      payload: { name: "x", sessionId: "no-such-session" },
      timestamp: Date.now(),
    })
    await new Promise((r) => setTimeout(r, 150))
    expect(events).toHaveLength(0)
    ws.close()
  })

  test("page_capture: event pushed to frontend, capture.result round-trips and persists", async () => {
    const s = (await (
      await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({}) })
    ).json()) as { id: string }

    const ws = new WebSocket(`ws://127.0.0.1:${handle.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws connect failed"))
    })
    const events: Array<{ type: string; payload: Record<string, unknown> }> = []
    ws.onmessage = (m) => {
      const data = JSON.parse(String(m.data)) as { type: string; payload: Record<string, unknown> }
      if (data.type.startsWith("event.")) events.push(data)
    }
    // 加载 self_optimize 子 Agent（其 page_capture 工具注册为 self_optimize_page_capture）
    await new Promise<void>((resolve, reject) => {
      const onMsg = (m: MessageEvent) => {
        const data = JSON.parse(String(m.data)) as { type: string; ok?: boolean }
        if (data.type !== "sub_agent.load") return
        ws.removeEventListener("message", onMsg)
        if (data.ok) resolve()
        else reject(new Error("sub_agent.load failed"))
      }
      ws.addEventListener("message", onMsg)
      ws.send(JSON.stringify({ type: "sub_agent.load", payload: { name: "self_optimize" }, id: "r-load" }))
    })
    // 模型调用 self_optimize_page_capture（共享 WsFake 计数需重置，确保本轮发起工具调用）
    const fake = (handle.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider as WsFake
    fake.toolName = "self_optimize_page_capture"
    fake.calls = 0
    // 前端角色：收到 event.capture.request 后回传捕获结果（html + 截图）
    let captureId = ""
    const captureHtml = "<html><body><h1>端到端捕获</h1></body></html>"
    const captureImg = "data:image/png;base64," + Buffer.from("e2e-png").toString("base64")
    const onEvent = (m: MessageEvent) => {
      const data = JSON.parse(String(m.data)) as { type: string; payload: Record<string, unknown> }
      if (data.type === "event.capture.request") {
        captureId = String(data.payload.captureId)
        ws.send(JSON.stringify({ type: "capture.result", payload: { id: s.id, captureId, html: captureHtml, imageBase64: captureImg }, id: "r-cap" }))
      }
    }
    ws.addEventListener("message", onEvent)
    ws.send(JSON.stringify({ type: "session.prompt", payload: { id: s.id, prompt: "capture the page" }, id: "r-prompt" }))
    await waitFor(() => events.some((e) => e.type === "event.task.done"))
    expect(captureId).toBeTruthy()
    const detail = (await (await fetch(`${base()}/api/v1/sessions/${s.id}`)).json()) as {
      messages: Array<{ role: string; name?: string; content: string; blocks?: Array<{ type: string; path?: string }> }>
    }
    const toolMsg = detail.messages.find((m) => m.role === "tool" && m.name === "self_optimize_page_capture")
    expect(toolMsg?.content).toContain("已捕获当前页面")
    expect(toolMsg?.content).toContain("tmp/capture/page-")
    expect(toolMsg?.blocks?.some((b) => b.type === "image")).toBe(true)
    ws.close()
  })

  test("session.attach：运行中会话快照（在途流 + 日志基线 seq），任务结束后 running=false", async () => {
    // 阻塞 provider：第一轮输出部分文本后挂起（测试控制放行）——attach 快照的 stream 断言用
    class BlockingFake implements LLMProvider {
      readonly id = "blocking"
      release?: () => void
      capabilities(): LLMCapabilities {
        return { streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 1000 }
      }
      async *chat(_msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
        yield { type: "text", text: "partial " }
        yield { type: "text", text: "text" }
        await new Promise<void>((resolve) => {
          this.release = resolve
        })
        yield { type: "done" }
      }
    }
    const engine = handle.engine as unknown as { opts: { provider: LLMProvider } }
    const orig = engine.opts.provider
    const blocking = new BlockingFake()
    engine.opts.provider = blocking
    const s = (await (
      await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({}) })
    ).json()) as { id: string }

    const ws = new WebSocket(`ws://127.0.0.1:${handle.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws connect failed"))
    })
    const replyOf = (reqId: string) =>
      new Promise<Record<string, unknown>>((resolve) => {
        const onMsg = (m: MessageEvent) => {
          const data = JSON.parse(String(m.data)) as { id?: string; ok?: boolean; payload?: Record<string, unknown> }
          if (data.id === reqId) {
            ws.removeEventListener("message", onMsg)
            resolve({ ok: data.ok, ...(data.payload ?? {}) })
          }
        }
        ws.addEventListener("message", onMsg)
      })
    // 未运行：running=false（刷新进入空闲会话的附加直接放弃）
    const idleP = replyOf("r-attach-idle")
    ws.send(JSON.stringify({ type: "session.attach", payload: { id: s.id }, id: "r-attach-idle" }))
    expect((await idleP).running).toBe(false)
    // 经 WS 发起任务（reply 即时确认，不等待任务完成；阻塞在部分文本后）
    const promptP = replyOf("r-attach-prompt")
    ws.send(JSON.stringify({ type: "session.prompt", payload: { id: s.id, prompt: "hello", stream: true }, id: "r-attach-prompt" }))
    expect((await promptP).ok).toBe(true)
    await waitFor(() => !!blocking.release)
    const attachP = replyOf("r-attach-run")
    ws.send(JSON.stringify({ type: "session.attach", payload: { id: s.id }, id: "r-attach-run" }))
    const snap = (await attachP) as { running: boolean; stream?: { text: string }; lastSeq?: number; pending: unknown[] }
    expect(snap.running).toBe(true)
    expect(snap.stream?.text).toBe("partial text")
    expect(typeof snap.lastSeq).toBe("number")
    expect(Array.isArray(snap.pending)).toBe(true)
    // 归属校验：不存在的会话拒绝
    const missP = replyOf("r-attach-miss")
    ws.send(JSON.stringify({ type: "session.attach", payload: { id: "0123456789abcdef0123456789abcdef" }, id: "r-attach-miss" }))
    expect((await missP).ok).toBe(false)
    // 放行收尾并还原 provider
    blocking.release!()
    await new Promise((r) => setTimeout(r, 150))
    engine.opts.provider = orig
    ws.close()
  })
})

describe("SessionStore ownership", () => {
  test("load rejects cross-user cache hits", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-store-"))
    const store = new SessionStore({ home })
    const alice = await store.createSession("alice")
    expect(await store.load(alice.id, "alice")).not.toBeNull()
    // 缓存命中（key=id）但归属不符：必须拒绝，否则跨用户读取 + WS 事件泄漏
    expect(await store.load(alice.id, "bob")).toBeNull()
    rmSync(home, { recursive: true, force: true })
  })

  test("truncateMessages removes message and everything after it", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-store-"))
    const store = new SessionStore({ home })
    const s = await store.createSession("alice")
    const m1 = { id: "m1", role: "user" as const, content: "a", createdAt: 1 }
    const m2 = { id: "m2", role: "assistant" as const, content: "b", createdAt: 2 }
    const m3 = { id: "m3", role: "user" as const, content: "c", createdAt: 3 }
    await store.appendMessage(s.id, m1)
    await store.appendMessage(s.id, m2)
    await store.appendMessage(s.id, m3)
    await store.truncateMessages(s.id, "alice", "m2")
    const loaded = await store.load(s.id, "alice")
    expect(loaded!.messages.map((m) => m.id)).toEqual(["m1"])
    // 越权用户不能截断
    await expect(store.truncateMessages(s.id, "bob", "m1")).rejects.toThrow()
    rmSync(home, { recursive: true, force: true })
  })

  test("truncateMessages from assistant(toolCalls) removes its tool results too (助手消息撤回语义)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-store-"))
    const store = new SessionStore({ home })
    const s = await store.createSession("alice")
    await store.appendMessage(s.id, { id: "u1", role: "user", content: "hi", createdAt: 1 })
    await store.appendMessage(s.id, { id: "a1", role: "assistant", content: "", toolCalls: [{ id: "t1", name: "ls", arguments: {} }], createdAt: 2 })
    await store.appendMessage(s.id, { id: "r1", role: "tool", content: "out", toolCallId: "t1", name: "ls", createdAt: 3 })
    await store.appendMessage(s.id, { id: "a2", role: "assistant", content: "final", createdAt: 4 })
    // 撤回中途 assistant(toolCalls)：其 tool 结果与最终回复一并删除，前缀配对不受影响
    await store.truncateMessages(s.id, "alice", "a1")
    expect((await store.load(s.id, "alice"))!.messages.map((m) => m.id)).toEqual(["u1"])
    // 撤回最终 assistant：保留 assistant(toolCalls)+tool 配对（历史尾部为 tool 结果合法，下次 user 输入可续）
    await store.appendMessage(s.id, { id: "a1b", role: "assistant", content: "", toolCalls: [{ id: "t1b", name: "ls", arguments: {} }], createdAt: 5 })
    await store.appendMessage(s.id, { id: "r1b", role: "tool", content: "out2", toolCallId: "t1b", name: "ls", createdAt: 6 })
    await store.appendMessage(s.id, { id: "a2b", role: "assistant", content: "final2", createdAt: 7 })
    await store.truncateMessages(s.id, "alice", "a2b")
    expect((await store.load(s.id, "alice"))!.messages.map((m) => m.id)).toEqual(["u1", "a1b", "r1b"])
    rmSync(home, { recursive: true, force: true })
  })

  test("truncateMessages clears usage baseline anchors (索引随删除错位，防压缩误判)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-store-"))
    const store = new SessionStore({ home })
    const s = await store.createSession("alice")
    await store.appendMessage(s.id, { id: "m1", role: "user", content: "a", createdAt: 1 })
    await store.appendMessage(s.id, { id: "m2", role: "assistant", content: "b", createdAt: 2 })
    const seeded = await store.load(s.id, "alice")
    seeded!.ctxInputTokens = 500
    seeded!.ctxAtMessage = 2
    seeded!.ctxTokens = 600
    await store.save(seeded!)
    await store.truncateMessages(s.id, "alice", "m2")
    const loaded = await store.load(s.id, "alice")
    expect(loaded!.ctxInputTokens).toBeUndefined()
    expect(loaded!.ctxAtMessage).toBeUndefined()
    expect(loaded!.ctxTokens).toBeGreaterThan(0) // 列表展示口径回退估算
    rmSync(home, { recursive: true, force: true })
  })
})

describe("WS event push (multi-user isolation)", () => {
  let mh: ServerHandle
  const mhome = mkdtempSync(join(tmpdir(), "gebai-ws-multi-"))

  beforeAll(async () => {
    mh = await startServer({ gebaiHome: mhome, auth: "server", sandbox: "on", binaryMode: false, preloadSubAgents: [], port: 0 })
    ;(mh.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = new WsFake()
    await mh.auth.createUser("alice", "pw")
    await mh.auth.createUser("bob", "pw")
  })

  afterAll(() => {
    mh.gc?.stop()
    mh.server.stop(true)
    rmSync(mhome, { recursive: true, force: true })
  })

  test("bob's unauthenticated connection receives no events from alice's session", async () => {
    const mbase = `http://127.0.0.1:${mh.server.port}`
    const login = (await (
      await fetch(`${mbase}/api/v1/auth/login`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: "alice", password: "pw" }),
      })
    ).json()) as { token: string }
    const auth = { Authorization: `Bearer ${login.token}` }

    const s = (await (
      await fetch(`${mbase}/api/v1/sessions`, { method: "POST", headers: { ...auth, "Content-Type": "application/json" }, body: JSON.stringify({}) })
    ).json()) as { id: string }

    // bob 连接（未登录，服务端回退 SERVICE_USER）
    const ws = new WebSocket(`ws://127.0.0.1:${mh.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws connect failed"))
    })
    const bobEvents: string[] = []
    ws.onmessage = (m) => {
      const data = JSON.parse(String(m.data)) as { type: string }
      if (data.type.startsWith("event.")) bobEvents.push(data.type)
    }

    // alice 触发对话（会话在 store 缓存中，key=id）
    const res = await fetch(`${mbase}/api/v1/sessions/${s.id}/prompt`, {
      method: "POST",
      headers: { ...auth, "Content-Type": "application/json" },
      body: JSON.stringify({ prompt: "what time is it" }),
    })
    expect(res.status).toBe(200)
    await res.text()
    // 给 WS 推送留出时间（若有泄漏，事件会在对话期间到达）
    await new Promise((r) => setTimeout(r, 200))
    expect(bobEvents).toHaveLength(0)
    ws.close()
  })
})

describe("truncate route (消息撤回)", () => {
  test("running task rejects truncate with 409; after completion truncate succeeds", async () => {
    // 阻塞模型调用：任务保持运行态，截断请求期间不结束
    const engine = handle.engine as unknown as { opts: { provider: LLMProvider }; isRunning(id: string): boolean }
    const orig = engine.opts.provider
    let release!: () => void
    const gate = new Promise<void>((r) => (release = r))
    engine.opts.provider = {
      id: "blocking",
      capabilities: () => ({ streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 1000 }),
      async *chat() {
        await gate
        yield { type: "text", text: "done" }
        yield { type: "done" }
      },
    }
    try {
      const s = (await (
        await fetch(`${base()}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({}) })
      ).json()) as { id: string }
      const promptPromise = fetch(`${base()}/api/v1/sessions/${s.id}/prompt`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ prompt: "hi" }),
      }).then((r) => r.text())
      await waitFor(() => engine.isRunning(s.id))
      // 运行中撤回被拒（任务持有自己的上下文快照，中途截断会产生交错历史）
      const denied = await fetch(`${base()}/api/v1/sessions/${s.id}/truncate`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ before: "whatever" }),
      })
      expect(denied.status).toBe(409)
      release()
      await promptPromise
      await waitFor(() => !engine.isRunning(s.id))
      // 任务结束后撤回正常（最终 assistant 消息按流式 messageId 定位）
      const loaded = await handle.store.load(s.id)
      const final = loaded!.messages.find((m) => m.role === "assistant" && m.content === "done")
      expect(final).toBeDefined()
      const ok = await fetch(`${base()}/api/v1/sessions/${s.id}/truncate`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ before: final!.id }),
      })
      expect(ok.status).toBe(200)
      const after = await handle.store.load(s.id)
      expect(after!.messages.some((m) => m.id === final!.id)).toBe(false)
    } finally {
      engine.opts.provider = orig
    }
  })
})
