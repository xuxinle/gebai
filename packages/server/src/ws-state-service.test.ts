import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { EventBus } from "./core/event-bus"
import { WsStateService, UserJournal } from "./ws-state"
import type { AgentEvent } from "@gebai/sdk"

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))

function ev(type: string, sessionId: string, payload: Record<string, unknown> = {}): AgentEvent {
  return { type, sessionId, payload, timestamp: Date.now() }
}

/** 最小 fake AppDeps：归属解析命中内存（不落盘），events 为真实 EventBus。 */
function makeDeps(owners: Record<string, string>) {
  const events = new EventBus()
  const store = {
    ownerOf: (id: string) => owners[id] ?? null,
    load: async (id: string) => (owners[id] ? { id, userId: owners[id], messages: [], todos: [], createdAt: 0, updatedAt: 0 } : null),
    listSessions: async () => [],
  }
  const engine = { runningIds: async () => [] as string[], contextWindow: () => 0 }
  const d = { store, engine, events, config: {}, auth: {}, env: {}, sandbox: {}, registry: {}, subAgents: {}, webhooks: {}, externalAuth: null } as never
  return { events, d }
}

describe("WsStateService delta 合并", () => {
  test("同一消息的连续 delta 合并为一条日志事件；非增量事件先冲刷保持顺序", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-wsstate-"))
    const { events, d } = makeDeps({ s1: "u1" })
    const state = new WsStateService(home, d)
    const got: Array<{ type: string; payload: Record<string, unknown> }> = []
    state.subscribe("u1", (e) => got.push({ type: e.type, payload: e.payload }))
    try {
      const base = { messageId: "m1", sessionId: "s1" }
      events.publish(ev("event.message.delta", "s1", { text: "你", ...base }))
      events.publish(ev("event.message.delta", "s1", { text: "好", ...base }))
      events.publish(ev("event.message.delta", "s1", { text: "吗", ...base }))
      events.publish(ev("event.tool.call", "s1", { name: "read", toolCallId: "tc1" }))
      await sleep(80)
      // 合并后的 delta 只有一条且文本聚合；tool.call 在其后（顺序保持）
      expect(got.map((g) => g.type)).toEqual(["event.message.delta", "event.tool.call"])
      expect(got[0].payload.text).toBe("你好吗")
      expect(got[0].payload.messageId).toBe("m1")
      // 合并事件入日志（重放同样只有一条 delta）
      const j = state.journal("u1")
      expect(j.replay(0)!.entries.filter((e) => e.type === "event.message.delta")).toHaveLength(1)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("不同消息的 delta 不被合并（messageId 变化先冲刷）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-wsstate-"))
    const { events, d } = makeDeps({ s1: "u1" })
    const state = new WsStateService(home, d)
    const got: Array<{ type: string; payload: Record<string, unknown> }> = []
    state.subscribe("u1", (e) => got.push({ type: e.type, payload: e.payload }))
    try {
      events.publish(ev("event.message.delta", "s1", { text: "第一段", messageId: "m1", sessionId: "s1" }))
      events.publish(ev("event.message.delta", "s1", { text: "第二段", messageId: "m2", sessionId: "s1" }))
      await sleep(80)
      expect(got.map((g) => g.payload.text)).toEqual(["第一段", "第二段"])
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("UserJournal 持久化", () => {
  test("重启后 seq 连续、断线×重启窗口内的事件仍可重放", () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-journal-"))
    const file = join(home, "users", "u1", "ws-journal.jsonl")
    try {
      const j1 = new UserJournal(1000, file)
      j1.append(ev("event.message.delta", "s1", { text: "a" }))
      j1.append(ev("event.tool.call", "s1", { name: "read" }))
      j1.append(ev("event.task.done", "s1"))
      expect(j1.lastSeq()).toBe(3)
      // 模拟服务重启：新实例从持久化文件恢复，seq 从 4 继续
      const j2 = new UserJournal(1000, file)
      expect(j2.lastSeq()).toBe(3)
      expect(j2.replay(1)!.entries.map((e) => e.seq)).toEqual([2, 3])
      const e4 = j2.append(ev("event.task.error", "s1", { error: "x" }))
      expect(e4.seq).toBe(4)
      // 第三次恢复：全量可重放（客户端断线跨过重启窗口也不丢事件）
      const j3 = new UserJournal(1000, file)
      expect(j3.replay(0)!.entries.map((e) => e.seq)).toEqual([1, 2, 3, 4])
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("WsStateService chains 清理", () => {
  test("事件路由完成后会话链被清理（Map 不随事件流常驻增长）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-wsstate-"))
    const { events, d } = makeDeps({ s1: "u1", s2: "u2" })
    const state = new WsStateService(home, d)
    try {
      for (let i = 0; i < 5; i++) {
        events.publish(ev("event.message.delta", "s1", { text: `x${i}` }))
        events.publish(ev("event.message.delta", "s2", { text: `y${i}` }))
      }
      await sleep(120)
      expect(state.pendingChainCount()).toBe(0)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
