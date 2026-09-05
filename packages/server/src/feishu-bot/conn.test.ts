import { describe, expect, test } from "bun:test"
import { encodeFrame, type PbFrame } from "./pb"
import { FeishuConn, type WsLike } from "./conn"

/** 可手动触发事件的 fake WebSocket。 */
class FakeWs implements WsLike {
  readyState = 0
  sent: Array<string | ArrayBuffer | Uint8Array> = []
  onopen?: () => void
  onmessage?: (ev: { data: unknown }) => void
  onclose?: (ev: { code?: number; reason?: string }) => void
  onerror?: (ev: unknown) => void
  send(d: string | ArrayBuffer | Uint8Array): void {
    this.sent.push(d)
  }
  close(code?: number, reason?: string): void {
    this.readyState = 3
    this.onclose?.({ code, reason })
  }
  open(): void {
    this.readyState = 1
    this.onopen?.()
  }
  receive(buf: Uint8Array): void {
    this.onmessage?.({ data: buf })
  }
}

function eventFrame(payload: object, method = 1, headers: Record<string, string> = {}, type = "event"): Uint8Array {
  const frame: PbFrame = {
    SeqID: 0,
    LogID: 0,
    service: 7,
    method,
    headers: [{ key: "type", value: method === 0 ? "pong" : type }, ...Object.entries(headers).map(([key, value]) => ({ key, value }))],
    payload: new TextEncoder().encode(JSON.stringify(payload)),
  }
  return encodeFrame(frame)
}

function makeConn(
  intervalsOverride?: { reconnectNonce?: number; reconnectInterval?: number; pingInterval?: number; handshakeTimeoutMs?: number; fastRetryMs?: number },
  endpointConfig: Record<string, number> | null = { PingInterval: 30, ReconnectInterval: 60 },
) {
  const wsList: FakeWs[] = []
  const events: Record<string, unknown>[] = []
  const endpointCalls: string[] = []
  const logs: string[] = []
  const fetchImpl = async (url: string, _init: RequestInit) => {
    endpointCalls.push(url)
    return {
      ok: true,
      status: 200,
      json: async () => ({ code: 0, msg: "ok", data: { URL: "wss://example.com/connect?device_id=dev1&service_id=7", ClientConfig: endpointConfig ?? {} } }),
    }
  }
  const wsFactory = {
    connect: () => {
      const w = new FakeWs()
      wsList.push(w)
      return w
    },
  }
  const conn = new FeishuConn({
    appId: "cli_a",
    appSecret: "sec",
    onEvent: (ev) => {
      events.push(ev)
    },
    fetchImpl,
    wsFactory,
    clock: () => 1000,
    intervals: { reconnectNonce: 0, reconnectInterval: 50, pingInterval: 60_000, handshakeTimeoutMs: 30, ...intervalsOverride },
    log: (m) => {
      logs.push(m)
    },
  })
  return { conn, wsList, events, endpointCalls, logs, get ws() { return wsList[wsList.length - 1] } }
}

/** 等待 conn.start() 创建 WS 实例（discoverEndpoint 为异步链）。 */
async function waitWs(wsList: FakeWs[]): Promise<FakeWs> {
  for (let i = 0; i < 100; i++) {
    if (wsList.length > 0) return wsList[0]
    await new Promise((r) => setTimeout(r, 5))
  }
  throw new Error("ws not created")
}

describe("FeishuConn", () => {
  test("start：endpoint 发现并连接", async () => {
    const { conn, wsList, endpointCalls } = makeConn()
    const started = conn.start()
    const ws = await waitWs(wsList)
    expect(endpointCalls).toHaveLength(1)
    ws.open()
    await started
    expect(ws.readyState).toBe(1)
    conn.stop()
  })

  test("endpoint 发现请求体与失败抛错", async () => {
    let bodySeen: string | null = null
    const wsList: FakeWs[] = []
    const fetchImpl = async (_url: string, init: RequestInit) => {
      bodySeen = String(init.body)
      return { ok: true, status: 200, json: async () => ({ code: 0, data: { URL: "wss://x/connect?device_id=d&service_id=1" } }) }
    }
    const conn = new FeishuConn({ appId: "cli_a", appSecret: "sec", onEvent: () => {}, fetchImpl, wsFactory: { connect: () => { const w = new FakeWs(); wsList.push(w); return w } }, log: () => {} })
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    expect(JSON.parse(bodySeen!)).toEqual({ AppID: "cli_a", AppSecret: "sec" })
    conn.stop()

    const bad = new FeishuConn({
      appId: "a",
      appSecret: "s",
      onEvent: () => {},
      fetchImpl: async () => ({ ok: true, status: 200, json: async () => ({ code: 999, msg: "no" }) }),
      wsFactory: { connect: () => new FakeWs() },
      log: () => {},
      intervals: { reconnectInterval: 10 },
    })
    await bad.start() // 不抛错：进入重连流程
    await new Promise((r) => setTimeout(r, 30))
    bad.stop()
  })

  test("事件帧：回调收到事件并回发 ACK（含 biz_rt 与 code 200）", async () => {
    const { conn, wsList, events } = makeConn()
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    ws.receive(eventFrame({ schema: "2.0", header: { event_type: "im.message.receive_v1" }, event: {} }, 1, { message_id: "om_1", sum: "1", seq: "0", trace_id: "tr" }))
    expect(events).toHaveLength(1)
    expect(events[0].schema).toBe("2.0")
    const ack = ws.sent[ws.sent.length - 1] as Uint8Array
    const { decodeFrame } = await import("./pb")
    const dec = decodeFrame(ack)
    expect(dec.method).toBe(1)
    expect(dec.headers).toContainEqual({ key: "message_id", value: "om_1" })
    expect(dec.headers).toContainEqual({ key: "biz_rt", value: "0" })
    expect(JSON.parse(new TextDecoder().decode(dec.payload!))).toEqual({ code: 200 })
    conn.stop()
  })

  test("事件帧：回调返回响应体（卡片回调）→ ACK 封 {code:200,data:base64} 信封", async () => {
    const wsList: FakeWs[] = []
    const conn = new FeishuConn({
      appId: "cli_a",
      appSecret: "sec",
      onEvent: (ev) => {
        if (((ev.header ?? {}) as { event_type?: string }).event_type === "card.action.trigger") {
          return Promise.resolve({ card: { type: "raw", data: { elements: [] } } })
        }
      },
      fetchImpl: async () => ({ ok: true, status: 200, json: async () => ({ code: 0, data: { URL: "wss://x/connect?device_id=d&service_id=1" } }) }),
      wsFactory: { connect: () => { const w = new FakeWs(); wsList.push(w); return w } },
      log: () => {},
      intervals: { reconnectInterval: 10 },
    })
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    ws.receive(eventFrame({ schema: "2.0", header: { event_type: "card.action.trigger" }, event: {} }, 1, { message_id: "om_c1", sum: "1", seq: "0" }))
    await new Promise((r) => setTimeout(r, 10))
    const { decodeFrame } = await import("./pb")
    const ack = ws.sent[ws.sent.length - 1] as Uint8Array
    const dec = decodeFrame(ack)
    const body = JSON.parse(new TextDecoder().decode(dec.payload!)) as { code: number; data?: string }
    expect(body.code).toBe(200)
    // 响应体 base64 编码进 data（lark_oapi ws/client.py 同构）
    expect(JSON.parse(Buffer.from(body.data!, "base64").toString())).toEqual({ card: { type: "raw", data: { elements: [] } } })
    conn.stop()
  })

  test("分片事件合包后回调一次并 ACK", async () => {
    const { conn, wsList, events } = makeConn()
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    // 同一事件 JSON 拆两段字节分发（与真实合包语义一致）
    const payload = new TextEncoder().encode(JSON.stringify({ schema: "2.0", header: { event_type: "x" }, event: {} }))
    const half = Math.floor(payload.length / 2)
    const part = (seq: number, bytes: Uint8Array) =>
      encodeFrame({
        SeqID: 0,
        LogID: 0,
        service: 7,
        method: 1,
        headers: [
          { key: "type", value: "event" },
          { key: "message_id", value: "om_big" },
          { key: "sum", value: "2" },
          { key: "seq", value: String(seq) },
        ],
        payload: bytes,
      })
    ws.receive(part(1, payload.slice(half)))
    ws.receive(part(0, payload.slice(0, half)))
    expect(events).toHaveLength(1)
    expect(events[0].schema).toBe("2.0")
    conn.stop()
  })

  test("pong 帧更新客户端配置（PingInterval）", async () => {
    const { conn, wsList } = makeConn()
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    // 初始 pingInterval 60s；pong 下发 5s
    ws.receive(eventFrame({ ReconnectCount: -1, ReconnectInterval: 10, ReconnectNonce: 0, PingInterval: 5 }, 0))
    // 无法直接读私有字段，验证不抛错且连接存活
    expect(ws.readyState).toBe(1)
    conn.stop()
  })

  test("CARD 帧无处理回调时回空响应 ACK；未知类型帧不回调不 ACK", async () => {
    const { conn, wsList, events } = makeConn()
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    // type=card 的 DATA 帧（未注册 onCardAction：回空响应 ACK，事件回调不触发）
    ws.receive(encodeFrame({ SeqID: 0, LogID: 0, service: 7, method: 1, headers: [{ key: "type", value: "card" }, { key: "message_id", value: "om_c" }], payload: new TextEncoder().encode(JSON.stringify({ event: { action: { value: {} } } })) }))
    await new Promise((r) => setTimeout(r, 10))
    expect(events).toHaveLength(0)
    const ack = ws.sent[ws.sent.length - 1] as Uint8Array
    const { decodeFrame } = await import("./pb")
    expect(JSON.parse(new TextDecoder().decode(decodeFrame(ack).payload!))).toEqual({})
    // 无 type header 的 DATA 帧：不回调不 ACK
    const afterCard = ws.sent.length
    ws.receive(encodeFrame({ SeqID: 0, LogID: 0, service: 7, method: 1, headers: [{ key: "message_id", value: "om_d" }], payload: new TextEncoder().encode("{}") }))
    expect(events).toHaveLength(0)
    expect(ws.sent.length).toBe(afterCard)
    conn.stop()
  })

  test("CARD 帧路由到 onCardAction 并回填响应 JSON（toast/card 更新）", async () => {
    const events: Record<string, unknown>[] = []
    const cardActions: Record<string, unknown>[] = []
    const wsList: FakeWs[] = []
    const fetchImpl = async (_url: string, _init: RequestInit) => ({
      ok: true,
      status: 200,
      json: async () => ({ code: 0, msg: "ok", data: { URL: "wss://example.com/connect?device_id=dev1&service_id=7", ClientConfig: {} } }),
    })
    const conn = new FeishuConn({
      appId: "cli_a",
      appSecret: "sec",
      onEvent: (ev) => {
      events.push(ev)
    },
      onCardAction: async (payload) => {
        cardActions.push(payload)
        return { toast: { type: "success", content: "已选择" }, card: { config: { wide_screen_mode: true } } }
      },
      fetchImpl,
      wsFactory: { connect: () => { const w = new FakeWs(); wsList.push(w); return w } },
      clock: () => 1000,
      intervals: { reconnectNonce: 0, reconnectInterval: 50, pingInterval: 60_000 },
      log: () => {},
    })
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    const payload = {
      schema: "2.0",
      header: { event_type: "card.action.trigger" },
      event: {
        operator: { operator_id: { open_id: "ou_1" }, operator_type: "user" },
        action: { value: { choiceId: "c1", v: "方案A" }, tag: "button" },
        context: { open_message_id: "om_c1", open_chat_id: "oc_1" },
      },
    }
    ws.receive(eventFrame(payload, 1, { message_id: "om_c1" }, "card"))
    await new Promise((r) => setTimeout(r, 10))
    expect(cardActions).toHaveLength(1)
    expect(cardActions[0]).toEqual(payload)
    // 事件回调不触发（card 帧 ≠ 事件帧）
    expect(events).toHaveLength(0)
    const ack = ws.sent[ws.sent.length - 1] as Uint8Array
    const { decodeFrame } = await import("./pb")
    const dec = decodeFrame(ack)
    expect(dec.headers).toContainEqual({ key: "biz_rt", value: "0" })
    expect(JSON.parse(new TextDecoder().decode(dec.payload!))).toEqual({
      toast: { type: "success", content: "已选择" },
      card: { config: { wide_screen_mode: true } },
    })
    conn.stop()
  })

  test("连接关闭自动重连（endpoint 重新发现）", async () => {
    const { conn, wsList, endpointCalls } = makeConn()
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    expect(endpointCalls).toHaveLength(1)
    ws.close()
    await new Promise((r) => setTimeout(r, 120))
    expect(endpointCalls.length).toBeGreaterThanOrEqual(2)
    // 让重连完成（新连接 open）后停止，避免挂起的握手 promise
    wsList[1]?.open()
    await new Promise((r) => setTimeout(r, 10))
    conn.stop()
  })

  test("ReconnectCount 上限：达到上限后停止重连", async () => {
    const wsList: FakeWs[] = []
    let calls = 0
    const fetchImpl = async () => {
      calls++
      if (calls === 1) {
        // 首次成功并下发重连上限 2
        return { ok: true, status: 200, json: async () => ({ code: 0, data: { URL: "wss://x/connect?device_id=d&service_id=1", ClientConfig: { ReconnectCount: 2 } } }) }
      }
      return { ok: true, status: 200, json: async () => ({ code: 999, msg: "auth failed" }) }
    }
    const conn = new FeishuConn({
      appId: "a",
      appSecret: "s",
      onEvent: () => {},
      fetchImpl,
      wsFactory: { connect: () => { const w = new FakeWs(); wsList.push(w); return w } },
      log: () => {},
      intervals: { reconnectNonce: 0, reconnectInterval: 10 },
    })
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    ws.close() // 断线 → 触发重连（2 次失败尝试后达到上限停止）
    await new Promise((r) => setTimeout(r, 120))
    expect(calls).toBe(3) // 首连 1 + 重连尝试 2（上限），不再继续
    await new Promise((r) => setTimeout(r, 60))
    expect(calls).toBe(3)
    conn.stop()
  })

  test("stop 后不再重连", async () => {
    const { conn, wsList, endpointCalls } = makeConn()
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    conn.stop()
    const calls = endpointCalls.length
    await new Promise((r) => setTimeout(r, 120))
    expect(endpointCalls.length).toBe(calls)
  })

  test("服务端静默超时：3x ping 间隔无任何帧，强制断开重连（不依赖 onclose）", async () => {
    // endpoint 不下发配置：pingInterval 保持注入的 40ms（3x=120ms 判死）
    const { conn, wsList, endpointCalls } = makeConn({ reconnectNonce: 0, reconnectInterval: 50, pingInterval: 40 }, null)
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    await new Promise((r) => setTimeout(r, 300))
    expect(ws.readyState).toBe(3) // 旧连接被主动关闭
    expect(endpointCalls.length).toBeGreaterThanOrEqual(2) // 已重新 endpoint 发现
    conn.stop()
  })

  test("任意服务端帧均刷新判活时钟（事件帧持续到达不触发死链误判）", async () => {
    const { conn, wsList, endpointCalls } = makeConn({ reconnectNonce: 0, reconnectInterval: 50, pingInterval: 40 }, null)
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    // 每 30ms 发一个事件帧（非 pong），持续 300ms——若只认 pong，120ms 即判死断连
    const frame = eventFrame({ schema: "2.0", header: { event_type: "x" }, event: {} }, 1, { message_id: "om_alive", sum: "1", seq: "0" })
    for (let i = 0; i < 10; i++) {
      await new Promise((r) => setTimeout(r, 30))
      ws.receive(frame)
    }
    expect(endpointCalls).toHaveLength(1)
    expect(wsList).toHaveLength(1)
    conn.stop()
  })

  test("握手超时：黑洞连接（无 open/error/close）按超时失败进入重连", async () => {
    const { conn, wsList, endpointCalls } = makeConn({ reconnectNonce: 0, reconnectInterval: 50, handshakeTimeoutMs: 40 }, null)
    const p = conn.start()
    await waitWs(wsList) // 不 open：模拟防火墙黑洞
    await p // start 应正常返回（进入重连流程而非永久挂起）
    await new Promise((r) => setTimeout(r, 100))
    expect(endpointCalls.length).toBeGreaterThanOrEqual(2)
    conn.stop()
  })

  test("重连失败先快速重试（min(interval, fastRetry)），不干等服务端长间隔", async () => {
    // 服务端 interval 若生效为 60s，第二次失败重试要等 60s；快速重试应秒级连续尝试
    const { conn, wsList, endpointCalls } = makeConn({ reconnectNonce: 0, reconnectInterval: 60_000, handshakeTimeoutMs: 30, fastRetryMs: 40 }, null)
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    ws.close() // 断线 → 立即重连（ws#2 握手超时失败）→ fastRetry 40ms 后再试（ws#3 …）
    await new Promise((r) => setTimeout(r, 250))
    expect(endpointCalls.length).toBeGreaterThanOrEqual(3)
    conn.stop()
  })

  test("断连日志携带 close code/reason 与存活时长", async () => {
    const { conn, wsList, logs } = makeConn()
    const p = conn.start()
    const ws = await waitWs(wsList)
    ws.open()
    await p
    ws.close(1006, "abnormal closure")
    await new Promise((r) => setTimeout(r, 20))
    expect(logs.some((l) => l.includes("code=1006") && l.includes("abnormal closure") && l.includes("alive"))).toBe(true)
    conn.stop()
  })
})
