import { describe, expect, test } from "bun:test"
import { createServer } from "node:net"
import { GebaiClient, resolveWsUrl, wsEventToChunk } from "./client"

describe("resolveWsUrl", () => {
  test("explicit baseUrl produces absolute ws/wss URL", () => {
    expect(resolveWsUrl("http://127.0.0.1:3000")).toBe("ws://127.0.0.1:3000/ws")
    expect(resolveWsUrl("https://api.example.com")).toBe("wss://api.example.com/ws")
    expect(resolveWsUrl("https://api.example.com/gb")).toBe("wss://api.example.com/gb/ws")
  })

  test("empty baseUrl resolves from location to absolute URL (DOM)", () => {
    expect(resolveWsUrl("", { protocol: "https:", host: "gb.example.com" })).toBe("wss://gb.example.com/ws")
    expect(resolveWsUrl("", { protocol: "http:", host: "127.0.0.1:5173" })).toBe("ws://127.0.0.1:5173/ws")
  })

  test("empty baseUrl without DOM falls back to relative /ws", () => {
    expect(resolveWsUrl("", null)).toBe("/ws")
  })
})

describe("GebaiClient", () => {
  test("constructs with baseUrl", () => {
    const c = new GebaiClient({ baseUrl: "http://127.0.0.1:3000" })
    expect(c).toBeInstanceOf(GebaiClient)
  })

  test("token setter works", () => {
    const c = new GebaiClient({ baseUrl: "http://x" })
    c.setToken("t")
  })
})

describe("GebaiClient connect", () => {
  test("rejects quickly when server unreachable (no permanent hang)", async () => {
    const c = new GebaiClient({ baseUrl: "http://127.0.0.1:1", connectTimeoutMs: 300 })
    await expect(c.connect()).rejects.toThrow(/WS connect/)
  })

  test("times out when server accepts but never responds (proxy hang)", async () => {
    // 原始 TCP 服务器接受连接但不响应 HTTP 升级：WS 握手永久挂起，只能靠超时兜底
    const srv = createServer(() => {})
    await new Promise<void>((resolve) => srv.listen({ port: 0, host: "127.0.0.1" }, () => resolve()))
    try {
      const port = (srv.address() as { port: number }).port
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${port}`, connectTimeoutMs: 200 })
      const t0 = Date.now()
      await expect(c.connect()).rejects.toThrow("WS connect timeout")
      expect(Date.now() - t0).toBeGreaterThanOrEqual(150)
    } finally {
      await new Promise<void>((resolve) => srv.close(() => resolve()))
    }
  })

  test("concurrent connect() calls share one connection attempt", async () => {
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("ok")
      },
      websocket: {
        open() {},
        message() {},
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}`, connectTimeoutMs: 2000 })
      await Promise.all([c.connect(), c.connect(), c.connect()])
      // 连接成功后请求通道可用（同一 ws 实例）
      const ws = (c as unknown as { ws?: WebSocket }).ws
      expect(ws?.readyState).toBe(WebSocket.OPEN)
    } finally {
      srv.stop(true)
    }
  })

  test("second connect() after close reconnects", async () => {
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("ok")
      },
      websocket: {
        open() {},
        message() {},
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}`, connectTimeoutMs: 2000 })
      await c.connect()
      ;(c as unknown as { ws?: WebSocket }).ws?.close()
      await c.connect()
      const ws = (c as unknown as { ws?: WebSocket }).ws
      expect(ws?.readyState).toBe(WebSocket.OPEN)
    } finally {
      srv.stop(true)
    }
  })

  test("heartbeat sends periodic ping and stays connected when pong arrives", async () => {
    let pingCount = 0
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("ok")
      },
      websocket: {
        open() {},
        message(ws, raw) {
          const msg = JSON.parse(raw as string)
          if (msg.type === "ping") {
            pingCount++
            ws.send(JSON.stringify({ type: "ping", id: msg.id, ok: true, payload: { pong: true } }))
          }
        },
      },
    })
    try {
      const c = new GebaiClient({
        baseUrl: `http://127.0.0.1:${srv.port}`,
        connectTimeoutMs: 2000,
        heartbeatIntervalMs: 40,
        heartbeatTimeoutMs: 500,
      })
      await c.connect()
      await new Promise((r) => setTimeout(r, 150))
      expect(pingCount).toBeGreaterThanOrEqual(2)
      expect(c.isConnected()).toBe(true)
      ;(c as unknown as { closeWs: () => void }).closeWs()
    } finally {
      srv.stop(true)
    }
  })

  test("heartbeat detects dead connection (no pong) and reconnects", async () => {
    let opens = 0
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("ok")
      },
      websocket: {
        // 不回复 pong：模拟代理静默掐断（连接仍 OPEN，但数据不通）
        open() {
          opens++
        },
        message() {},
      },
    })
    try {
      const c = new GebaiClient({
        baseUrl: `http://127.0.0.1:${srv.port}`,
        connectTimeoutMs: 2000,
        heartbeatIntervalMs: 40,
        heartbeatTimeoutMs: 100,
      })
      const statuses: string[] = []
      c.onStatusChange((s) => statuses.push(s))
      await c.connect()
      // pong 超时 → 主动 close → 自动重连（退避后第二次 open）
      await new Promise((r) => setTimeout(r, 1400))
      expect(opens).toBeGreaterThanOrEqual(2)
      expect(statuses.filter((s) => s === "disconnected").length).toBeGreaterThanOrEqual(1)
      ;(c as unknown as { closeWs: () => void }).closeWs()
    } finally {
      srv.stop(true)
    }
  })

  test("status changes on open/close and malformed ws messages are skipped", async () => {
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("ok")
      },
      websocket: {
        open(ws) {
          // 推送畸形数据与合法事件：畸形应被静默跳过，不中断连接
          ws.send("not-json{{{")
          ws.send(JSON.stringify({ type: "event.task.done", payload: { sessionId: "s1" } }))
        },
        message() {},
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}`, connectTimeoutMs: 2000 })
      const statuses: string[] = []
      const events: string[] = []
      c.onStatusChange((s) => statuses.push(s))
      c.onEvent((ev) => events.push(ev.type))
      await c.connect()
      await new Promise((r) => setTimeout(r, 100))
      expect(statuses).toContain("connected")
      expect(events).toContain("event.task.done")
      // 主动关闭不触发自动重连（manualClose），状态回到 disconnected
      ;(c as unknown as { ws?: WebSocket }).ws?.close()
      await new Promise((r) => setTimeout(r, 100))
      expect(statuses).toContain("disconnected")
      // 手动 reconnect 复位标记：断线后应触发自动重连
      const srv2 = Bun.serve({
        port: 0,
        fetch(req, server) {
          if (server.upgrade(req)) return
          return new Response("ok")
        },
        websocket: { open() {}, message() {} },
      })
      try {
        ;(c as unknown as { baseUrl: string }).baseUrl = `http://127.0.0.1:${srv2.port}`
        await c.connect()
        expect((c as unknown as { ws?: WebSocket }).ws?.readyState).toBe(WebSocket.OPEN)
      } finally {
        srv2.stop(true)
      }
    } finally {
      srv.stop(true)
    }
  })
})

describe("wsEventToChunk", () => {
  test("maps WS events to ChatChunk contract (delta/reasoning/tool/approval/done/error)", () => {
    const ev = (type: string, payload: Record<string, unknown> = {}) => ({ type, sessionId: "s1", payload, timestamp: Date.now() })
    expect(wsEventToChunk(ev("event.message.delta", { text: "你", messageId: "m1" }))).toEqual({ kind: "text", text: "你", messageId: "m1" })
    // 新会话执行过程 delta 透传 session 标记（前端据此分段显示）
    expect(wsEventToChunk(ev("event.message.delta", { text: "子代理过程", messageId: "m2", session: true }))).toEqual({ kind: "text", text: "子代理过程", messageId: "m2", session: true })
    expect(wsEventToChunk(ev("event.message.reasoning", { text: "思考" }))).toEqual({ kind: "reasoning", text: "思考" })
    // 新会话执行过程：reasoning/工具/审批事件透传 session 与 sessionRunId 标记（前端据此渲染到新会话容器）
    expect(wsEventToChunk(ev("event.message.reasoning", { text: "子代理思考", session: true, sessionRunId: "r1" }))).toEqual({
      kind: "reasoning",
      text: "子代理思考",
      session: true,
      sessionRunId: "r1",
    })
    expect(wsEventToChunk(ev("event.tool.call", { toolCallId: "t2", name: "code_todo", arguments: {}, session: true, sessionRunId: "r1" }))).toEqual({
      kind: "tool_call",
      toolCall: { id: "t2", name: "code_todo", arguments: {} },
      session: true,
      sessionRunId: "r1",
    })
    // 新会话 run 起止事件：start 携带 agents/input，done 携带 agents/output（前端折叠容器标题用）
    expect(wsEventToChunk(ev("event.session.start", { runId: "r1", agents: ["code", "playwright"], input: "改个文件", depth: 1 }))).toEqual({
      kind: "session_start",
      session: true,
      sessionRunId: "r1",
      sessionMeta: { agents: ["code", "playwright"], input: "改个文件" },
    })
    expect(wsEventToChunk(ev("event.session.done", { runId: "r1", agents: ["code"], output: "已完成" }))).toEqual({
      kind: "session_done",
      session: true,
      sessionRunId: "r1",
      sessionMeta: { agents: ["code"], output: "已完成" },
    })
    // 异常路径：output 为空（引擎 catch 显式传 ""）且携带 error → 折叠摘要显示中断原因
    expect(wsEventToChunk(ev("event.session.done", { runId: "r1", agents: ["code"], output: "", error: "cancelled" }))).toEqual({
      kind: "session_done",
      session: true,
      sessionRunId: "r1",
      sessionMeta: { agents: ["code"], output: "（已中断: cancelled）" },
    })
    // 已移除的 reset 事件不再映射（DSML 泄漏检测已去除）
    expect(wsEventToChunk(ev("event.message.reset", { messageId: "m1" }))).toBeNull()
    expect(wsEventToChunk(ev("event.tool.call", { toolCallId: "t1", name: "read", arguments: { path: "a" } }))).toEqual({
      kind: "tool_call",
      toolCall: { id: "t1", name: "read", arguments: { path: "a" } },
    })
    expect(wsEventToChunk(ev("event.tool.result", { toolCallId: "t1", name: "read", output: "ok" }))).toMatchObject({
      kind: "tool_result",
      toolCall: { id: "t1", name: "read", arguments: {} },
      output: "ok",
    })
    expect(wsEventToChunk(ev("event.approval.request", { toolCallId: "t1", retries: 1, tool: "write" }))).toEqual({
      kind: "approval",
      approval: { toolCallId: "t1", retries: 1, tool: "write" },
    })
    expect(wsEventToChunk(ev("event.task.done"))).toEqual({ kind: "done" })
    expect(wsEventToChunk(ev("event.task.error", { error: "boom" }))).toEqual({ kind: "error", error: "boom" })
    // 非会话事件类型 → null（未知事件忽略）
    expect(wsEventToChunk(ev("event.cron.run"))).toBeNull()
  })
})

describe("GebaiClient sendPrompt (WS)", () => {
  test("streams ChatChunks from WS events until task.done", async () => {
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("upgrade failed", { status: 500 })
      },
      websocket: {
        open() {},
        message(ws, raw) {
          const msg = JSON.parse(String(raw)) as { type: string; id?: string; payload?: Record<string, unknown> }
          if (msg.type !== "session.prompt") return
          const sessionId = String(msg.payload?.id ?? "")
          // 先确认 reply，再推事件（模拟服务端）
          ws.send(JSON.stringify({ type: "session.prompt", id: msg.id, ok: true }))
          const push = (type: string, payload: Record<string, unknown>) =>
            ws.send(JSON.stringify({ type, payload: { ...payload, sessionId }, timestamp: Date.now() }))
          push("event.message.delta", { text: "你", messageId: "m1" })
          push("event.message.delta", { text: "好", messageId: "m1" })
          push("event.tool.call", { toolCallId: "t1", name: "ls", arguments: {} })
          push("event.tool.result", { toolCallId: "t1", name: "ls", output: "now" })
          push("event.task.done", {})
        },
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const chunks: Array<{ kind: string; text?: string }> = []
      for await (const chunk of c.sendPrompt("s1", "hi")) {
        chunks.push({ kind: chunk.kind, text: chunk.text })
      }
      expect(chunks.map((c) => c.kind)).toEqual(["text", "text", "tool_call", "tool_result", "done"])
      expect(chunks.filter((c) => c.kind === "text").map((c) => c.text).join("")).toBe("你好")
    } finally {
      srv.stop(true)
    }
  })

  test("abort signal ends the iteration with an error", async () => {
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("upgrade failed", { status: 500 })
      },
      websocket: {
        open() {},
        message(ws, raw) {
          const msg = JSON.parse(String(raw)) as { type: string; id?: string; payload?: Record<string, unknown> }
          if (msg.type === "session.prompt") ws.send(JSON.stringify({ type: "session.prompt", id: msg.id, ok: true }))
        },
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const ac = new AbortController()
      const collect = (async () => {
        const out: unknown[] = []
        for await (const chunk of c.sendPrompt("s1", "hi", { signal: ac.signal })) out.push(chunk)
        return out
      })()
      ac.abort()
      await expect(collect).rejects.toThrow(/aborted/)
    } finally {
      srv.stop(true)
    }
  })
})

describe("GebaiClient sendPrompt edge cases", () => {
  /** 模拟服务端：支持状态快照/sync.request/session.get 的重连场景。 */
  function makeResumeServer(opts: { first?: (ws: { send: (d: string) => void; close: () => void }, msg: { type: string; id?: string; payload?: Record<string, unknown> }) => void; journal: Array<{ seq: number; type: string; payload: Record<string, unknown> }>; running?: string[]; session?: { messages: Array<{ id: string; role: string; content: string; createdAt: number }> } }) {
    const journal = [...opts.journal]
    let conns = 0
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("upgrade failed", { status: 500 })
      },
      websocket: {
        open() {},
        message(ws, raw) {
          const msg = JSON.parse(String(raw)) as { type: string; id?: string; payload?: Record<string, unknown> }
          const reply = (payload: Record<string, unknown>) => ws.send(JSON.stringify({ type: msg.type, id: msg.id, ok: true, payload }))
          switch (msg.type) {
            case "session.prompt": {
              conns++
              if (conns === 1) {
                if (opts.first) opts.first(ws, msg)
                else reply({})
              } else {
                // 重连后的补发：接受并直接完成
                reply({})
                ws.send(JSON.stringify({ type: "event.message.delta", seq: 100, sessionId: "s1", payload: { text: "好", sessionId: "s1" }, timestamp: Date.now() }))
                ws.send(JSON.stringify({ type: "event.task.done", seq: 101, sessionId: "s1", payload: { sessionId: "s1" }, timestamp: Date.now() }))
              }
              break
            }
            case "state.snapshot":
              reply({ currentSessionId: null, sessions: [], running: opts.running ?? [], lastSeq: Math.max(0, ...journal.map((e) => e.seq)) })
              break
            case "sync.request": {
              const after = Number(msg.payload?.lastSeq ?? 0)
              const events = journal.filter((e) => e.seq > after).map((e) => ({ ...e, sessionId: "s1", payload: { ...e.payload, sessionId: "s1" } }))
              reply({ events, overrun: false, lastSeq: Math.max(after, ...events.map((e) => e.seq)) })
              break
            }
            case "session.get":
              reply({ session: { id: "s1", name: "t", userId: "default", createdAt: 0, updatedAt: 0, messages: opts.session?.messages ?? [] } })
              break
            default:
              reply({})
          }
        },
      },
    })
    return srv
  }

  test("WS disconnect mid-task: suspends and resumes via journal replay after reconnect", async () => {
    // 首连接：prompt 已确认，推送半截 delta 后断开；日志中错过 done
    const srv = makeResumeServer({
      journal: [{ seq: 2, type: "event.task.done", payload: {} }],
      running: ["s1"],
      first: (ws, msg) => {
        ws.send(JSON.stringify({ type: "session.prompt", id: msg.id, ok: true }))
        ws.send(JSON.stringify({ type: "event.message.delta", seq: 1, sessionId: "s1", payload: { text: "前", sessionId: "s1" }, timestamp: Date.now() }))
        setTimeout(() => ws.close(), 30)
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const got: Array<{ kind: string; text?: string }> = []
      for await (const chunk of c.sendPrompt("s1", "hi")) got.push({ kind: chunk.kind, text: chunk.text })
      // 重连后重放 done：流正常收尾而非报错
      expect(got.map((g) => g.kind)).toEqual(["text", "done"])
      expect(got[0].text).toBe("前")
    } finally {
      srv.stop(true)
    }
  })

  test("WS disconnect mid-task: missed events replayed in seq order", async () => {
    const srv = makeResumeServer({
      journal: [
        { seq: 1, type: "event.message.delta", payload: { text: "你" } },
        { seq: 2, type: "event.message.delta", payload: { text: "好" } },
        { seq: 3, type: "event.task.done", payload: {} },
      ],
      running: ["s1"],
      first: (ws, msg) => {
        ws.send(JSON.stringify({ type: "session.prompt", id: msg.id, ok: true }))
        setTimeout(() => ws.close(), 30) // 未推任何事件即断开
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const got: Array<{ kind: string; text?: string }> = []
      for await (const chunk of c.sendPrompt("s1", "hi")) got.push({ kind: chunk.kind, text: chunk.text })
      expect(got.map((g) => g.kind)).toEqual(["text", "text", "done"])
      expect(got.filter((g) => g.kind === "text").map((g) => g.text).join("")).toBe("你好")
    } finally {
      srv.stop(true)
    }
  })

  test("WS disconnect before prompt ack: task finished offline, synthesized from store", async () => {
    const srv = makeResumeServer({
      journal: [],
      running: [],
      session: {
        messages: [
          { id: "u1", role: "user", content: "hi", createdAt: 0 },
          { id: "a1", role: "assistant", content: "离线期间已跑完", createdAt: 1 },
        ],
      },
      first: (ws) => {
        // 不回复 prompt 即断开：请求确认丢失
        setTimeout(() => ws.close(), 30)
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const got: Array<{ kind: string; text?: string }> = []
      for await (const chunk of c.sendPrompt("s1", "hi", { messageId: "u1" })) got.push({ kind: chunk.kind, text: chunk.text })
      // resume 重置 + 存储合成收尾（不重复发起任务）
      expect(got.map((g) => g.kind)).toEqual(["resume", "text", "done"])
      expect(got[1].text).toBe("离线期间已跑完")
    } finally {
      srv.stop(true)
    }
  })

  test("WS disconnect before prompt ack: task never started, prompt re-sent once", async () => {
    const srv = makeResumeServer({
      journal: [],
      running: [],
      session: { messages: [{ id: "u1", role: "user", content: "hi", createdAt: 0 }] },
      first: (ws) => {
        setTimeout(() => ws.close(), 30) // 无回复断开：未开始
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const got: Array<{ kind: string; text?: string }> = []
      for await (const chunk of c.sendPrompt("s1", "hi", { messageId: "u1" })) got.push({ kind: chunk.kind, text: chunk.text })
      // 补发成功：文本 + done
      expect(got.map((g) => g.kind)).toEqual(["text", "done"])
      expect(got[0].text).toBe("好")
    } finally {
      srv.stop(true)
    }
  })

  test("events arriving before the prompt reply are still consumed (no race loss)", async () => {
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("upgrade failed", { status: 500 })
      },
      websocket: {
        open() {},
        message(ws, raw) {
          const msg = JSON.parse(String(raw)) as { type: string; id?: string; payload?: Record<string, unknown> }
          if (msg.type !== "session.prompt") return
          const sessionId = String(msg.payload?.id ?? "")
          // 先推事件、后 reply（与服务端极端时序一致：订阅先于请求注册，事件不应丢失）
          ws.send(JSON.stringify({ type: "event.message.delta", seq: 1, payload: { text: "先到", sessionId }, timestamp: Date.now() }))
          ws.send(JSON.stringify({ type: "event.task.done", seq: 2, payload: { sessionId }, timestamp: Date.now() }))
          ws.send(JSON.stringify({ type: "session.prompt", id: msg.id, ok: true }))
        },
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const kinds: string[] = []
      for await (const chunk of c.sendPrompt("s1", "hi")) kinds.push(chunk.kind)
      expect(kinds).toEqual(["text", "done"])
    } finally {
      srv.stop(true)
    }
  })

  test("concurrent sendPrompt streams are isolated per session", async () => {
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("upgrade failed", { status: 500 })
      },
      websocket: {
        open() {},
        message(ws, raw) {
          const msg = JSON.parse(String(raw)) as { type: string; id?: string; payload?: Record<string, unknown> }
          if (msg.type !== "session.prompt") return
          const sessionId = String(msg.payload?.id ?? "")
          ws.send(JSON.stringify({ type: "session.prompt", id: msg.id, ok: true }))
          for (let i = 0; i < 3; i++) {
            ws.send(JSON.stringify({ type: "event.message.delta", payload: { text: `${sessionId}-${i}`, sessionId }, timestamp: Date.now() }))
          }
          ws.send(JSON.stringify({ type: "event.task.done", payload: { sessionId }, timestamp: Date.now() }))
        },
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const [a, b] = await Promise.all([
        (async () => {
          const out: string[] = []
          for await (const chunk of c.sendPrompt("sA", "hi")) if (chunk.kind === "text") out.push(chunk.text ?? "")
          return out
        })(),
        (async () => {
          const out: string[] = []
          for await (const chunk of c.sendPrompt("sB", "hi")) if (chunk.kind === "text") out.push(chunk.text ?? "")
          return out
        })(),
      ])
      expect(a).toEqual(["sA-0", "sA-1", "sA-2"])
      expect(b).toEqual(["sB-0", "sB-1", "sB-2"])
    } finally {
      srv.stop(true)
    }
  })
})

describe("GebaiClient send (callback RPC)", () => {
  let counts: string[] = []
  function echoServer() {
    return Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("upgrade failed", { status: 500 })
      },
      websocket: {
        open() {},
        message(ws, raw) {
          const msg = JSON.parse(String(raw)) as { type: string; id?: string; payload?: Record<string, unknown> }
          if (msg.type === "echo") ws.send(JSON.stringify({ type: "echo", id: msg.id, ok: true, payload: { echoed: msg.payload?.v ?? null } }))
          else if (msg.type === "fail") ws.send(JSON.stringify({ type: "fail", id: msg.id, ok: false, error: "boom" }))
          else if (msg.type === "slow") setTimeout(() => ws.send(JSON.stringify({ type: "slow", id: msg.id, ok: true, payload: { late: true } })), 80)
          else if (msg.type === "count") {
            counts.push(String(msg.payload?.tag ?? "?"))
            ws.send(JSON.stringify({ type: "count", id: msg.id, ok: true, payload: { n: counts.length } }))
          }
        },
      },
    })
  }

  test("send registers callbacks; onOk receives payload on ok reply", async () => {
    const srv = echoServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const ok: unknown[] = []
      const errs: Error[] = []
      c.send("echo", { v: 42 }, { onOk: (p) => ok.push(p), onError: (e) => errs.push(e) })
      await new Promise((r) => setTimeout(r, 100))
      expect(errs).toEqual([])
      expect(ok).toEqual([{ echoed: 42 }])
    } finally {
      srv.stop(true)
    }
  })

  test("onError receives server error reply (ok=false)", async () => {
    const srv = echoServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const errs: Error[] = []
      await new Promise<void>((resolve) => {
        c.send("fail", {}, { onError: (e) => { errs.push(e); resolve() } })
      })
      expect(errs[0]?.message).toContain("boom")
    } finally {
      srv.stop(true)
    }
  })

  test("timeout fires onError when no reply arrives", async () => {
    const srv = echoServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const errs: Error[] = []
      await new Promise<void>((resolve) => {
        c.send("slow", {}, { timeoutMs: 40, onError: (e) => { errs.push(e); resolve() } })
      })
      expect(errs[0]?.message).toContain("超时")
    } finally {
      srv.stop(true)
    }
  })

  test("cancel prevents callbacks after cancellation", async () => {
    const srv = echoServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const ok: unknown[] = []
      await new Promise<void>((resolve) => {
        const cancel = c.send("slow", {}, { onOk: (p) => ok.push(p) })
        setTimeout(() => {
          cancel()
          resolve()
        }, 20) // 早于 80ms 应答
      })
      await new Promise((r) => setTimeout(r, 120))
      expect(ok).toEqual([]) // 应答到达但已取消：静默丢弃
    } finally {
      srv.stop(true)
    }
  })

  test("queueOffline: messages queue while offline and flush in order after connect", async () => {
    counts = []
    const srv = echoServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      // 未建连：三条消息排队（queueOffline），建连后按序冲刷
      const done = new Promise<void>((resolve) => {
        c.send("count", { tag: "a" }, { queueOffline: true, onOk: () => {} })
        c.send("count", { tag: "b" }, { queueOffline: true, onOk: () => {} })
        c.send("count", { tag: "c" }, { queueOffline: true, onOk: () => { resolve() } })
      })
      await done
      expect(counts).toEqual(["a", "b", "c"]) // 严格按入队顺序
    } finally {
      srv.stop(true)
    }
  })

  test("request auto-connects before sending (Promise RPC)", async () => {
    const srv = echoServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const r = await c.request<{ echoed: number }>("echo", { v: 1 })
      expect(r).toEqual({ echoed: 1 })
    } finally {
      srv.stop(true)
    }
  })
})

describe("GebaiClient state snapshot (MVC model)", () => {
  test("state.snapshot push updates model and notifies onSnapshot; events carry seq", async () => {
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("upgrade failed", { status: 500 })
      },
      websocket: {
        open(ws) {
          ws.send(JSON.stringify({ type: "state.snapshot", payload: { currentSessionId: "s1", sessions: [{ id: "s1", name: "t" }], running: ["s1"], lastSeq: 5 } }))
        },
        message() {},
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const snaps: Array<{ currentSessionId: string | null; running: string[]; lastSeq: number }> = []
      c.onSnapshot((s) => snaps.push({ currentSessionId: s.currentSessionId, running: s.running, lastSeq: s.lastSeq }))
      await c.connect()
      await new Promise((r) => setTimeout(r, 100))
      expect(snaps.length).toBeGreaterThanOrEqual(1)
      const snap = c.getSnapshot()
      expect(snap.currentSessionId).toBe("s1")
      expect(snap.running).toEqual(["s1"])
      expect(snap.sessions.some((s) => s.id === "s1")).toBe(true)
      expect(snap.lastSeq).toBe(5)
    } finally {
      srv.stop(true)
    }
  })

  test("state.snapshot push keeps maxContextTokens (titlebar ctx ratio display)", async () => {
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("upgrade failed", { status: 500 })
      },
      websocket: {
        open(ws) {
          ws.send(JSON.stringify({ type: "state.snapshot", payload: { currentSessionId: "s1", sessions: [], running: [], lastSeq: 1, maxContextTokens: 128000 } }))
        },
        message() {},
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const seen: number[] = []
      c.onSnapshot((s) => seen.push(s.maxContextTokens ?? 0))
      await c.connect()
      await new Promise((r) => setTimeout(r, 100))
      expect(seen.at(-1)).toBe(128000)
      expect(c.getSnapshot().maxContextTokens).toBe(128000)
      // 新快照未携带该字段时不应残留旧值（0=未知）
      ;(c as unknown as { handleMessage: (m: MessageEvent) => void }).handleMessage(
        { data: JSON.stringify({ type: "state.snapshot", payload: { currentSessionId: null, sessions: [], running: [], lastSeq: 2 } }) } as MessageEvent,
      )
      expect(c.getSnapshot().maxContextTokens).toBe(0)
    } finally {
      srv.stop(true)
    }
  })

  test("running set updates on task.done; seq tracked on events", async () => {
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("upgrade failed", { status: 500 })
      },
      websocket: {
        open(ws) {
          ws.send(JSON.stringify({ type: "state.snapshot", payload: { currentSessionId: null, sessions: [], running: ["s1"], lastSeq: 3 } }))
          ws.send(JSON.stringify({ type: "event.task.done", seq: 4, sessionId: "s1", payload: { sessionId: "s1" }, timestamp: Date.now() }))
        },
        message() {},
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      await c.connect()
      await new Promise((r) => setTimeout(r, 100))
      expect(c.getSnapshot().running).toEqual([]) // done 事件移除运行态
      expect(c.getSnapshot().lastSeq).toBe(4)
      // 内部 lastSeq 推进（sendPrompt 恢复流程依赖）
      expect((c as unknown as { lastSeq: number }).lastSeq).toBe(4)
      ;(c as unknown as { closeWs: () => void }).closeWs()
    } finally {
      srv.stop(true)
    }
  })
})

describe("断线重放事件回流全局订阅者", () => {
  /** 与 makeResumeServer 相同的重连模拟：首连接推送后断开，重放 journal 中的审批事件。 */
  function makeReplayServer(journal: Array<{ seq: number; type: string; payload: Record<string, unknown> }>) {
    let conns = 0
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("upgrade failed", { status: 500 })
      },
      websocket: {
        open() {},
        message(ws, raw) {
          const msg = JSON.parse(String(raw)) as { type: string; id?: string; payload?: Record<string, unknown> }
          const reply = (payload: Record<string, unknown>) => ws.send(JSON.stringify({ type: msg.type, id: msg.id, ok: true, payload }))
          switch (msg.type) {
            case "session.prompt":
              conns++
              if (conns === 1) {
                ws.send(JSON.stringify({ type: "session.prompt", id: msg.id, ok: true }))
                setTimeout(() => ws.close(), 30) // 未推任何事件即断开
              } else {
                reply({})
              }
              break
            case "state.snapshot":
              reply({ currentSessionId: null, sessions: [], running: ["s1"], lastSeq: Math.max(0, ...journal.map((e) => e.seq)) })
              break
            case "sync.request": {
              const after = Number(msg.payload?.lastSeq ?? 0)
              const events = journal.filter((e) => e.seq > after).map((e) => ({ ...e, sessionId: "s1", payload: { ...e.payload, sessionId: "s1" } }))
              reply({ events, overrun: false, lastSeq: Math.max(after, ...events.map((e) => e.seq)) })
              break
            }
            case "session.get":
              reply({ session: { id: "s1", name: "t", userId: "default", createdAt: 0, updatedAt: 0, messages: [] } })
              break
            default:
              reply({})
          }
        },
      },
    })
    return srv
  }

  test("离线期间的审批/工具事件重放后回流全局 onEvent（前端审批卡恢复）", async () => {
    const srv = makeReplayServer([
      { seq: 1, type: "event.approval.request", payload: { toolCallId: "tc1", tool: "sh" } },
      { seq: 2, type: "event.tool.call", payload: { toolCallId: "tc1", name: "sh" } },
      { seq: 3, type: "event.task.done", payload: {} },
    ])
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const globalEvents: string[] = []
      const unsub = c.onEvent((ev) => globalEvents.push(ev.type))
      const got: Array<{ kind: string }> = []
      for await (const chunk of c.sendPrompt("s1", "hi")) got.push({ kind: chunk.kind })
      unsub()
      // 全局订阅者收到重放的审批请求（断线前的事件 + 重放事件）
      expect(globalEvents).toContain("event.approval.request")
      expect(globalEvents).toContain("event.tool.call")
      // sendPrompt chunk 流同样收到（approval/task.done 均转换）
      expect(got.map((g) => g.kind)).toEqual(["approval", "tool_call", "done"])
    } finally {
      srv.stop(true)
    }
  })

  test("session.prompt 补发幂等以协议错误码判定（already_running 不抛错）", async () => {
    let conns = 0
    const srv = Bun.serve({
      port: 0,
      fetch(req, server) {
        if (server.upgrade(req)) return
        return new Response("upgrade failed", { status: 500 })
      },
      websocket: {
        open() {},
        message(ws, raw) {
          const msg = JSON.parse(String(raw)) as { type: string; id?: string; payload?: Record<string, unknown> }
          switch (msg.type) {
            case "session.prompt":
              conns++
              if (conns === 1) {
                // 首连接：不回复确认即断开（请求确认丢失，客户端转入挂起恢复）
                setTimeout(() => ws.close(), 30)
              } else {
                // 重连补发：服务端返回错误码（已运行）——客户端按协议码接受并转入恢复
                ws.send(JSON.stringify({ type: "session.prompt", id: msg.id, ok: false, error: "task already running", payload: { code: "already_running" } }))
                ws.send(JSON.stringify({ type: "event.message.delta", seq: 10, sessionId: "s1", payload: { text: "好", sessionId: "s1" }, timestamp: Date.now() }))
                ws.send(JSON.stringify({ type: "event.task.done", seq: 11, sessionId: "s1", payload: { sessionId: "s1" }, timestamp: Date.now() }))
              }
              break
            case "state.snapshot":
              // 快照未显示运行中：客户端补发 prompt，随后服务端回 already_running 协议码
              ws.send(JSON.stringify({ type: "state.snapshot", id: msg.id, ok: true, payload: { currentSessionId: null, sessions: [], running: [], lastSeq: 9 } }))
              break
            case "sync.request":
              ws.send(JSON.stringify({ type: "sync.request", id: msg.id, ok: true, payload: { events: [], overrun: false, lastSeq: 9 } }))
              break
            case "session.get":
              ws.send(JSON.stringify({ type: "session.get", id: msg.id, ok: true, payload: { session: { id: "s1", name: "t", userId: "default", createdAt: 0, updatedAt: 0, messages: [] } } }))
              break
            default:
              ws.send(JSON.stringify({ type: msg.type, id: msg.id, ok: true, payload: {} }))
          }
        },
      },
    })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${srv.port}` })
      const got: Array<{ kind: string; text?: string }> = []
      for await (const chunk of c.sendPrompt("s1", "hi")) got.push({ kind: chunk.kind, text: chunk.text })
      // 错误码判定为「已接受」：转入恢复流程，正常收尾而非抛「task already running」
      expect(got.map((g) => g.kind)).toEqual(["text", "done"])
      expect(got[0].text).toBe("好")
    } finally {
      srv.stop(true)
    }
  })
})
