import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { EventBus } from "./core/base/event-bus"
import { WebhookManager, type WebhookConfig } from "./webhooks"
import type { AgentEvent } from "@gebai/sdk"

interface Delivery {
  url: string
  body: { type: string; sessionId: string; payload: Record<string, unknown> }
  headers: Record<string, string>
}

function fakeDeliver(results: Array<{ ok: boolean; status: number }>) {
  const deliveries: Delivery[] = []
  const fn = async (url: string, body: unknown, headers: Record<string, string>) => {
    deliveries.push({ url, body: body as Delivery["body"], headers })
    const r = results.shift() ?? { ok: true, status: 200 }
    return r
  }
  return { fn, deliveries }
}

function ev(type: string, sessionId = "s1"): AgentEvent {
  return { type, sessionId, payload: { sessionId }, timestamp: Date.now() }
}

async function setup(results: Array<{ ok: boolean; status: number }> = []) {
  const home = mkdtempSync(join(tmpdir(), "gebai-wh-"))
  const bus = new EventBus()
  const { fn, deliveries } = fakeDeliver(results)
  const mgr = new WebhookManager({ home, deliver: fn, retryBaseMs: 5 })
  await mgr.start(bus)
  return { home, bus, mgr, deliveries }
}

describe("WebhookManager", () => {
  test("register/list/remove roundtrip persists to disk", async () => {
    const { home, mgr } = await setup()
    const cfg = await mgr.register({ url: "https://example.com/hook", events: ["event.task.done"], secret: "s3cret" })
    expect(cfg.id).toBeTruthy()
    expect(cfg.secret).toBe("***") // 脱敏返回
    const list = mgr.list()
    expect(list).toHaveLength(1)
    expect(list[0].url).toBe("https://example.com/hook")
    expect(await mgr.remove(cfg.id)).toBe(true)
    expect(mgr.list()).toHaveLength(0)
    // 持久化：重新实例化仍能读到
    const mgr2 = new WebhookManager({ home })
    await mgr2.start(new EventBus())
    expect(mgr2.list()).toHaveLength(0)
    rmSync(home, { recursive: true, force: true })
  })

  test("of() returns raw config by id including secret (cron notify reference resolution)", async () => {
    const { mkdtempSync, rmSync } = await import("node:fs")
    const { tmpdir } = await import("node:os")
    const { join } = await import("node:path")
    const home = mkdtempSync(join(tmpdir(), "gebai-wh-of-"))
    try {
      const mgr = new WebhookManager({ home, deliver: async () => ({ ok: true, status: 200 }) })
      await mgr.start(new (await import("./core/base/event-bus")).EventBus())
      const cfg = await mgr.register({ url: "https://example.com/h", secret: "s1" }, "alice")
      // list 视图脱敏；of() 原样返回（含 secret 与归属）供内部解析
      expect(mgr.list("alice")[0].secret).toBe("***")
      expect(mgr.of(cfg.id)).toMatchObject({ id: cfg.id, url: "https://example.com/h", secret: "s1", userId: "alice" })
      expect(mgr.of("missing")).toBeUndefined()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("rejects non-http(s) urls", async () => {
    const { mgr } = await setup()
    await expect(mgr.register({ url: "ftp://x" })).rejects.toThrow(/invalid webhook url/)
  })

  test("rejects loopback / link-local urls (SSRF guard)", async () => {
    const { mgr } = await setup()
    for (const url of [
      "http://localhost:8080/hook",
      "http://127.0.0.1:3000/hook",
      "http://169.254.169.254/latest/meta-data",
      "http://[::1]:9000/hook",
      "http://[0:0:0:0:0:0:0:1]:9000/hook", // IPv6 回环完整形式（URL 规范化后同 ::1）
      "http://[::ffff:127.0.0.1]:9000/hook", // IPv4-mapped → 127.0.0.1
      "http://[fe80::1]:9000/hook", // 链路本地
    ]) {
      await expect(mgr.register({ url })).rejects.toThrow(/not allowed/)
    }
    // 合法公网地址可注册
    const cfg = await mgr.register({ url: "https://hooks.example.com/cb" })
    expect(cfg.id).toBeTruthy()
  })

  test("defaults events to task.done / approval.request / task.error", async () => {
    const { mgr } = await setup()
    const cfg = await mgr.register({ url: "https://example.com/hook" })
    expect(cfg.events).toEqual(["event.task.done", "event.approval.request", "event.task.error"])
  })

  test("pushes matching events with HMAC signature header", async () => {
    const { bus, mgr, deliveries } = await setup()
    await mgr.register({ url: "https://example.com/hook", secret: "k" })
    bus.publish(ev("event.task.done"))
    await new Promise((r) => setTimeout(r, 30))
    expect(deliveries).toHaveLength(1)
    expect(deliveries[0].url).toBe("https://example.com/hook")
    expect(deliveries[0].body.type).toBe("event.task.done")
    const sig = deliveries[0].headers["X-Gebai-Signature"]
    expect(sig).toMatch(/^sha256=[0-9a-f]{64}$/)
    // 签名应为 HMAC-SHA256(secret, body) —— 用同 secret 复算验证
    const { createHmac } = await import("node:crypto")
    const expected = createHmac("sha256", "k").update(JSON.stringify(deliveries[0].body)).digest("hex")
    expect(sig).toBe(`sha256=${expected}`)
  })

  test("filters out non-subscribed events", async () => {
    const { bus, mgr, deliveries } = await setup()
    await mgr.register({ url: "https://example.com/hook", events: ["event.task.done"] })
    bus.publish(ev("event.tool.call"))
    bus.publish(ev("event.task.error"))
    await new Promise((r) => setTimeout(r, 30))
    expect(deliveries).toHaveLength(0)
  })

  test("retries failed deliveries with backoff", async () => {
    const { bus, mgr, deliveries } = await setup([{ ok: false, status: 500 }, { ok: false, status: 500 }, { ok: true, status: 200 }])
    await mgr.register({ url: "https://example.com/hook" })
    bus.publish(ev("event.task.done"))
    await new Promise((r) => setTimeout(r, 200))
    expect(deliveries).toHaveLength(3)
  })

  test("filters events by session ownership in multi-user mode", async () => {
    const { bus, mgr, deliveries } = await setup()
    await mgr.register({ url: "https://example.com/hook" }, "alice")
    mgr.ownerOf = async () => "bob"
    bus.publish(ev("event.task.done", "other-session"))
    await new Promise((r) => setTimeout(r, 30))
    expect(deliveries).toHaveLength(0)
    // 归属匹配则推送
    mgr.ownerOf = async () => "alice"
    bus.publish(ev("event.task.done", "alice-session"))
    await new Promise((r) => setTimeout(r, 30))
    expect(deliveries).toHaveLength(1)
  })

  test("persists registered configs across restarts", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-wh-persist-"))
    const mgr1 = new WebhookManager({ home })
    await mgr1.start(new EventBus())
    await mgr1.register({ url: "https://example.com/hook" })
    const mgr2 = new WebhookManager({ home })
    await mgr2.start(new EventBus())
    const cfg: WebhookConfig[] = mgr2.list()
    expect(cfg).toHaveLength(1)
    expect(cfg[0].url).toBe("https://example.com/hook")
    rmSync(home, { recursive: true, force: true })
  })
})
