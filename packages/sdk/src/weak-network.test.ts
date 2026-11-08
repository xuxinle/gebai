/**
 * 弱网健壮性：`hurryReconnect()`、REST 超时、幂等读重试。
 *
 * 这三条对应移动网络/桥接路由器下最常见的三种「卡」：
 *
 * 1. **退避不被打断**——断网后重连呈指数退避（实测 1/2/4/8/16/30s），网络恢复与「下一拍退避到点」
 *    毫无关系，用户要干等最长 30s；`hurryReconnect()` 是这条路径的唯一出口。
 * 2. **REST 永不返回**——`fetch` 默认无超时，有 TCP 无响应时既无 resolve 也无 reject
 *    （实测 20s 仍在挂），启动链/加载链会整个挂死。
 * 3. **抖动即失败**——`onclose` 会把在途请求一律 reject；幂等读若不重试，弱网下每次抖动
 *    都换来一次可见失败（会话列表变空、打开会话报错）。
 */
import { describe, expect, test } from "bun:test"
import { createServer, type Socket } from "node:net"
import { GebaiClient } from "./client"

/** 回 pong / 快照 / 计数的 WS 服务；`failFirst` 控制前 N 个会话请求返回业务错。 */
function startWsServer(opts: { failFirst?: number } = {}) {
  const counts: string[] = []
  let opens = 0
  let failLeft = opts.failFirst ?? 0
  const srv = Bun.serve({
    port: 0,
    fetch(req, server) {
      if (server.upgrade(req)) return
      return new Response("ok")
    },
    websocket: {
      open(ws) {
        opens++
        ws.send(JSON.stringify({ type: "state.snapshot", payload: { currentSessionId: null, sessions: [], running: [], lastSeq: 0 } }))
      },
      message(ws, raw) {
        const m = JSON.parse(String(raw)) as { type: string; id?: string }
        if (m.type === "ping") {
          ws.send(JSON.stringify({ type: "ping", id: m.id, ok: true, payload: { pong: true } }))
          return
        }
        if (m.type === "state.snapshot") {
          ws.send(JSON.stringify({ type: "state.snapshot", id: m.id, ok: true, payload: { currentSessionId: null, sessions: [], running: [], lastSeq: 0 } }))
          return
        }
        if (m.type === "sync.request") {
          ws.send(JSON.stringify({ type: "sync.request", id: m.id, ok: true, payload: { events: [], overrun: false, lastSeq: 0 } }))
          return
        }
        counts.push(m.type)
        if (m.type === "session.list" && failLeft > 0) {
          failLeft -= 1
          ws.send(JSON.stringify({ type: m.type, id: m.id, ok: false, error: "WS closed" }))
          return
        }
        if (m.type === "session.list") {
          ws.send(JSON.stringify({ type: m.type, id: m.id, ok: true, payload: { sessions: [{ id: "s1", name: "x" }] } }))
          return
        }
        ws.send(JSON.stringify({ type: m.type, id: m.id, ok: true, payload: {} }))
      },
    },
  })
  return { srv, port: srv.port, counts, opens: () => opens }
}

/** 只 accept 然后不回应的 TCP 服务（弱网/代理挂起的形态）。 */
async function startBlackholeServer() {
  const sockets: Socket[] = []
  const srv = createServer((s) => sockets.push(s))
  await new Promise<void>((r) => srv.listen({ port: 0, host: "127.0.0.1" }, () => r()))
  const port = (srv.address() as { port: number }).port
  return {
    port,
    stop: async () => {
      for (const s of sockets) s.destroy()
      await new Promise<void>((r) => srv.close(() => r()))
    },
  }
}

describe("hurryReconnect：网络恢复即刻重连（不等退避到点）", () => {
  test("退避等待期间调用即立刻建连，且复位退避序列", async () => {
    const { srv, port, opens } = startWsServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${port}`, connectTimeoutMs: 2000 })
      await c.connect()
      expect(c.isConnected()).toBe(true)
      const before = opens()
      // 判死 → 进入退避（第一档 1s）
      c.dropConnection()
      // 不等退避：立刻催一把
      c.hurryReconnect()
      const ok = await waitFor(() => c.isConnected(), 3_000)
      expect(ok).toBe(true)
      expect(opens()).toBeGreaterThan(before)
      ;(c as unknown as { closeWs: () => void }).closeWs()
    } finally {
      srv.stop(true)
    }
  })

  test("未连接且从未连过时也能催（启动期离线 → online 事件直接建连）", async () => {
    const { srv, port } = startWsServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${port}`, connectTimeoutMs: 2000 })
      expect(c.isConnected()).toBe(false)
      c.hurryReconnect()
      expect(await waitFor(() => c.isConnected(), 3_000)).toBe(true)
      ;(c as unknown as { closeWs: () => void }).closeWs()
    } finally {
      srv.stop(true)
    }
  })

  test("主动关闭（登出）后催不动：不得把已登出的页面重新连回去", () => {
    const c = new GebaiClient({ baseUrl: "http://127.0.0.1:1", connectTimeoutMs: 200 })
    ;(c as unknown as { closeWs: () => void }).closeWs()
    c.hurryReconnect()
    expect(c.isConnected()).toBe(false)
    expect(c.canReconnect()).toBe(false)
  })
})

describe("REST 请求必须带超时（否则弱网下永久悬挂）", () => {
  test("有 TCP 无响应：按 requestTimeoutMs 失败，而不是永远挂着", async () => {
    const bh = await startBlackholeServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${bh.port}`, requestTimeoutMs: 300 })
      const t0 = Date.now()
      const err = await c.getSignupConfig().then(
        () => null,
        (e: Error) => e,
      )
      const elapsed = Date.now() - t0
      expect(err).not.toBeNull()
      expect(elapsed).toBeLessThan(3_000) // 关键：有上限（无超时会是永不返回）
      ;(c as unknown as { closeWs: () => void }).closeWs()
    } finally {
      await bh.stop()
    }
  })
})

describe("幂等读的重试：连接抖动不再抖出可见失败", () => {
  test("session.list 首次以 WS closed 失败 → 自动重试成功", async () => {
    const s = startWsServer({ failFirst: 1 })
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${s.port}`, connectTimeoutMs: 2000 })
      await c.connect()
      const list = await c.listSessions()
      expect(list.map((x) => x.id)).toEqual(["s1"])
      expect(s.counts.filter((t) => t === "session.list").length).toBe(2) // 失败一次 + 重试一次
      ;(c as unknown as { closeWs: () => void }).closeWs()
    } finally {
      s.srv.stop(true)
    }
  })

  test("协议/业务错误不重试（重试无意义，且写入类副作用不得重复）", async () => {
    const s = startWsServer({ failFirst: 99 }) // 一直失败
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${s.port}`, connectTimeoutMs: 2000 })
      await c.connect()
      const err = await c.listSessions().then(
        () => null,
        (e: Error) => e,
      )
      expect(err).not.toBeNull()
      // 重试只发生一次（"WS closed" 判为可重试）→ 共 2 次请求；不能无限重试
      expect(s.counts.filter((t) => t === "session.list").length).toBe(2)
      ;(c as unknown as { closeWs: () => void }).closeWs()
    } finally {
      s.srv.stop(true)
    }
  })
})

async function waitFor(fn: () => boolean, timeoutMs: number): Promise<boolean> {
  const t0 = Date.now()
  while (Date.now() - t0 < timeoutMs) {
    if (fn()) return true
    await new Promise((r) => setTimeout(r, 25))
  }
  return fn()
}
