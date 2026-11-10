/** 会话输入队列的端到端契约（真服务 + 真引擎 + 假模型）：
 *
 *  队列从前端挪到服务端后，真正要守住的是四件事——运行中提交被受理而不是被拒、
 *  任务收尾后自动按序执行、执行时用户消息上屏事件与会话落盘一致、刷新/重连后队列不丢。
 *  这些都属于「接线」缺陷（各自单独都正确，漏一根就表现为「排队输入永远不执行」），
 *  故以真实 WS + 引擎跑而不是单测桩。
 */
import { afterAll, beforeAll, describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"
import type { ChatOptions, LLMChunk, LLMProvider } from "./core/llm/llm"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"

/** 可控假模型：被 hold() 的那一次调用挂起直到放行，其余直出文本（记录收到的用户输入）。 */
class GatedProvider implements LLMProvider {
  readonly id = "fake"
  calls = 0
  userTexts: string[] = []
  private gate: Promise<void> | null = null
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: false, multimodal: false, maxContextTokens: 100_000 }
  }
  /** 让下一次调用卡住，返回放行函数（一次一个：每次 hold 只管一次调用）。 */
  hold(): () => void {
    let open!: () => void
    this.gate = new Promise<void>((r) => (open = r))
    return () => open()
  }
  async *chat(msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.calls++
    const last = [...msgs].reverse().find((m) => m.role === "user")
    if (last && typeof last.content === "string") this.userTexts.push(last.content)
    const gate = this.gate
    this.gate = null // 本次调用消费掉挂起开关
    if (gate) await gate
    yield { type: "text", text: `回答#${this.calls}` }
    yield { type: "done" }
  }
}

const home = mkdtempSync(join(tmpdir(), "gebai-queue-e2e-"))
let server: ServerHandle
let provider: GatedProvider

beforeAll(async () => {
  server = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
  provider = new GatedProvider()
  ;(server.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = provider
})

afterAll(() => {
  server.gc?.stop()
  server.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

async function waitFor(cond: () => boolean, timeout = 5000): Promise<void> {
  const start = Date.now()
  while (!cond()) {
    if (Date.now() - start > timeout) throw new Error("timeout")
    await new Promise((r) => setTimeout(r, 20))
  }
}

/** 一条带事件收集的 WS 连接。 */
async function connect() {
  const ws = new WebSocket(`ws://127.0.0.1:${server.server.port}/ws`)
  await new Promise<void>((resolve, reject) => {
    ws.onopen = () => resolve()
    ws.onerror = () => reject(new Error("ws error"))
  })
  const events: Array<{ type: string; sessionId: string; payload: Record<string, unknown> }> = []
  const replies = new Map<string, { ok: boolean; payload?: Record<string, unknown>; error?: string }>()
  ws.onmessage = (m) => {
    const data = JSON.parse(String(m.data)) as { type: string; id?: string; ok?: boolean; payload?: Record<string, unknown>; error?: string }
    if (data.id) {
      replies.set(data.id, { ok: data.ok === true, payload: data.payload, error: data.error })
      return
    }
    if (data.type.startsWith("event.")) events.push({ type: data.type, sessionId: String((data.payload as { sessionId?: string })?.sessionId ?? ""), payload: (data.payload ?? {}) as Record<string, unknown> })
  }
  const call = async (type: string, payload?: Record<string, unknown>, id = crypto.randomUUID()) => {
    ws.send(JSON.stringify({ type, payload, id }))
    await waitFor(() => replies.has(id))
    return replies.get(id)!
  }
  return { ws, events, call }
}

describe("服务端会话输入队列（端到端）", () => {
  test("运行中提交进入服务端队列，任务收尾后自动按序执行并带上屏事件", async () => {
    const s = await (await fetch(`http://127.0.0.1:${server.server.port}/api/v1/sessions`, { method: "POST" })).json() as { id: string }
    const c = await connect()
    try {
      const open = provider.hold() // 第 1 轮卡住 → 会话保持运行中
      const first = await c.call("session.prompt", { id: s.id, prompt: "第一条" })
      expect(first.ok).toBe(true)
      await waitFor(() => server.engine.isRunning(s.id))

      // 运行中提交：默认姿态是排队（reply 带 queued 标记，不报 already_running）
      const q1 = await c.call("session.prompt", { id: s.id, prompt: "排队甲" })
      const q2 = await c.call("session.prompt", { id: s.id, prompt: "排队乙" })
      expect(q1.ok).toBe(true)
      expect(q1.payload?.queued).toBe(true)
      expect(q2.payload?.queued).toBe(true)
      // 队列事件：两条都在，顺序即执行顺序
      const update = c.events.filter((e) => e.type === "event.queue.update").at(-1)
      expect((update?.payload.items as Array<{ text: string }>).map((x) => x.text)).toEqual(["排队甲", "排队乙"])

      // 放行第 1 轮：任务收尾，队列自动派发「排队甲」并继续（第 2 轮不卡）
      open()
      await waitFor(() => provider.userTexts.includes("排队甲") && provider.userTexts.includes("排队乙"), 8000)
      // 排队项开始执行时推 dequeued 事件（前端据此把用户消息上屏）
      expect(c.events.some((e) => e.type === "event.queue.dequeued" && e.payload.text === "排队甲")).toBe(true)
      expect(c.events.some((e) => e.type === "event.queue.dequeued" && e.payload.text === "排队乙")).toBe(true)
      await waitFor(() => !server.engine.isRunning(s.id), 8000)
      // 队列已清空，且全部用户消息按序落盘（执行时落盘，不是入队时）
      const r = await c.call("session.queue.list", { id: s.id })
      expect(r.payload?.items).toEqual([])
      const detail = await (await fetch(`http://127.0.0.1:${server.server.port}/api/v1/sessions/${s.id}`)).json() as { messages: Array<{ role: string; content: string }> }
      const users = detail.messages.filter((m) => m.role === "user").map((m) => m.content)
      expect(users).toEqual(["第一条", "排队甲", "排队乙"])
    } finally {
      c.ws.close()
    }
  }, 30_000)

  test("编辑 / 撤回 / 立即执行 作用于服务端队列", async () => {
    const s = await (await fetch(`http://127.0.0.1:${server.server.port}/api/v1/sessions`, { method: "POST" })).json() as { id: string }
    const c = await connect()
    try {
      const open = provider.hold()
      await c.call("session.prompt", { id: s.id, prompt: "占住会话" })
      await waitFor(() => server.engine.isRunning(s.id))
      const a = await c.call("session.prompt", { id: s.id, prompt: "甲" })
      await c.call("session.prompt", { id: s.id, prompt: "乙" })
      const aId = String((a.payload?.item as { id?: string })?.id ?? "")

      // 编辑
      const edited = await c.call("session.queue.edit", { id: s.id, itemId: aId, text: "甲（改）" })
      expect(edited.ok).toBe(true)
      expect((edited.payload?.items as Array<{ text: string }>)[0].text).toBe("甲（改）")
      // 空内容拒绝
      const rejected = await c.call("session.queue.edit", { id: s.id, itemId: aId, text: "   " })
      expect(rejected.ok).toBe(false)

      // 立即执行（乙置队首，并取消当前任务）
      const items = edited.payload?.items as Array<{ id: string; text: string }>
      const bId = items.find((x) => x.text === "乙")!.id
      const ran = await c.call("session.queue.run", { id: s.id, itemId: bId })
      expect(ran.ok).toBe(true)
      expect((ran.payload?.items as Array<{ text: string }>)[0].text).toBe("乙")

      open()
      // 乙先执行（被置顶），随后甲（改）
      await waitFor(() => provider.userTexts.includes("乙") && provider.userTexts.includes("甲（改）"), 8000)
      expect(provider.userTexts.indexOf("乙")).toBeLessThan(provider.userTexts.indexOf("甲（改）"))
      await waitFor(() => !server.engine.isRunning(s.id), 8000)

      // 撤回：先排队再造一条，然后撤回，队列复位为空
      const open2 = provider.hold()
      await c.call("session.prompt", { id: s.id, prompt: "又占住" })
      await waitFor(() => server.engine.isRunning(s.id))
      const x = await c.call("session.prompt", { id: s.id, prompt: "待撤回" })
      const xId = String((x.payload?.item as { id?: string })?.id ?? "")
      const removed = await c.call("session.queue.remove", { id: s.id, itemId: xId })
      expect(removed.ok).toBe(true)
      expect(removed.payload?.items).toEqual([])
      open2()
      await waitFor(() => !server.engine.isRunning(s.id), 8000)
      expect(provider.userTexts).not.toContain("待撤回")
    } finally {
      c.ws.close()
    }
  }, 30_000)

  test("新连接从快照恢复排队条（刷新/换设备不丢）", async () => {
    const s = await (await fetch(`http://127.0.0.1:${server.server.port}/api/v1/sessions`, { method: "POST" })).json() as { id: string }
    const c = await connect()
    try {
      const open = provider.hold()
      await c.call("session.prompt", { id: s.id, prompt: "占住" })
      await waitFor(() => server.engine.isRunning(s.id))
      await c.call("session.prompt", { id: s.id, prompt: "跨连接可见" })
      // 新连接（模拟刷新）：连接建立即自动推快照，queues 里应有该会话的排队项
      const snap = await new Promise<Record<string, unknown>>((resolve, reject) => {
        const ws = new WebSocket(`ws://127.0.0.1:${server.server.port}/ws`)
        const t = setTimeout(() => reject(new Error("snapshot not pushed")), 3000)
        ws.onmessage = (m) => {
          const data = JSON.parse(String(m.data)) as { type: string; payload?: Record<string, unknown> }
          if (data.type !== "state.snapshot") return
          clearTimeout(t)
          ws.close()
          resolve(data.payload ?? {})
        }
      })
      const queues = snap.queues as Record<string, Array<{ text: string }>>
      expect(queues[s.id]?.map((x) => x.text)).toEqual(["跨连接可见"])
      open()
      await waitFor(() => !server.engine.isRunning(s.id), 8000)
    } finally {
      c.ws.close()
    }
  }, 30_000)

  test("queue:false 保留原语义（运行中直接拒绝 already_running）", async () => {
    const s = await (await fetch(`http://127.0.0.1:${server.server.port}/api/v1/sessions`, { method: "POST" })).json() as { id: string }
    const c = await connect()
    try {
      const open = provider.hold()
      await c.call("session.prompt", { id: s.id, prompt: "占住" })
      await waitFor(() => server.engine.isRunning(s.id))
      const r = await c.call("session.prompt", { id: s.id, prompt: "直发", queue: false })
      expect(r.ok).toBe(false)
      expect(r.error).toContain("already running")
      open()
      await waitFor(() => !server.engine.isRunning(s.id), 8000)
    } finally {
      c.ws.close()
    }
  }, 30_000)

  test("会话删除释放其排队项", async () => {
    const s = await (await fetch(`http://127.0.0.1:${server.server.port}/api/v1/sessions`, { method: "POST" })).json() as { id: string }
    const c = await connect()
    try {
      const open = provider.hold()
      await c.call("session.prompt", { id: s.id, prompt: "占住" })
      await waitFor(() => server.engine.isRunning(s.id))
      await c.call("session.prompt", { id: s.id, prompt: "会被释放" })
      await c.call("session.delete", { id: s.id })
      expect(server.inputQueue.size(s.id)).toBe(0)
      open()
    } finally {
      c.ws.close()
    }
  }, 30_000)
})
