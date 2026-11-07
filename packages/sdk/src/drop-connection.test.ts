/**
 * `GebaiClient.dropConnection()`：断网时主动判死连接的语义护栏。
 *
 * 为什么这方法必须单独测：它与既有的 `closeWs()` 长得像、用途却相反——
 * - `closeWs()`（私有）：登录/登出/换令牌用，把 `manualClose = true`，**不再重连**；
 * - `dropConnection()`（本次新增）：网络断开用，**必须保留重连意图**——网络恢复后要靠
 *   退避重连接回去。
 *
 * 若谁图省事把 offline 处理写成 `closeWs()`（或让 `dropConnection` 复用它的实现），
 * 症状是「断网一次后再也不自己连回来」，而单测若不盯这条，改坏了也不会红。
 */
import { describe, expect, test } from "bun:test"
import { GebaiClient } from "./client"

/** 起一个最简 WS 服务（只统计连接次数，回 pong）。 */
function startWsServer() {
  let opens = 0
  const srv = Bun.serve({
    port: 0,
    fetch(req, server) {
      if (server.upgrade(req)) return
      return new Response("ok")
    },
    websocket: {
      open(ws) {
        opens++
        ws.send(JSON.stringify({ type: "state.snapshot", payload: {} }))
      },
      message(ws, raw) {
        try {
          const m = JSON.parse(String(raw)) as { type?: string; id?: string }
          if (m.type === "ping") ws.send(JSON.stringify({ type: "ping", id: m.id, ok: true, payload: { pong: true } }))
        } catch {
          /* 忽略 */
        }
      },
    },
  })
  return { srv, port: srv.port, opens: () => opens }
}

describe("dropConnection：主动判死但保留重连意图", () => {
  test("未连接时是 no-op（幂等，不抛错）", () => {
    const c = new GebaiClient({ baseUrl: "http://127.0.0.1:1", connectTimeoutMs: 200 })
    expect(() => c.dropConnection()).not.toThrow()
    expect(c.isConnected()).toBe(false)
  })

  test("连接后调用：立刻上报 disconnected（不等 onclose 异步到达）", async () => {
    const { srv, port } = startWsServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${port}`, connectTimeoutMs: 2000 })
      const statuses: string[] = []
      c.onStatusChange((s) => statuses.push(s))
      await c.connect()
      expect(c.isConnected()).toBe(true)
      c.dropConnection()
      // 同步就应已断开——界面不能等 onclose 才变红
      expect(c.isConnected()).toBe(false)
      expect(statuses).toContain("disconnected")
      ;(c as unknown as { closeWs: () => void }).closeWs()
    } finally {
      srv.stop(true)
    }
  })

  test("**保留重连意图**：断开后会自己连回来（不是 closeWs 那种永久关闭）", async () => {
    const { srv, port, opens } = startWsServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${port}`, connectTimeoutMs: 2000 })
      await c.connect()
      const first = opens()
      expect(first).toBe(1)
      c.dropConnection()
      // 退避基线是 1s：等够一次重连
      await new Promise((r) => setTimeout(r, 1800))
      expect(opens()).toBeGreaterThan(first)
      expect(c.isConnected()).toBe(true)
      ;(c as unknown as { closeWs: () => void }).closeWs()
    } finally {
      srv.stop(true)
    }
  })

  test("与 closeWs 对照：closeWs 之后**不再**自动重连（证明两者语义确实不同）", async () => {
    const { srv, port, opens } = startWsServer()
    try {
      const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${port}`, connectTimeoutMs: 2000 })
      await c.connect()
      const first = opens()
      ;(c as unknown as { closeWs: () => void }).closeWs()
      await new Promise((r) => setTimeout(r, 1800))
      expect(opens()).toBe(first) // 没有新连接
      expect(c.isConnected()).toBe(false)
    } finally {
      srv.stop(true)
    }
  })
})
