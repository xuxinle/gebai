import { describe, expect, test } from "bun:test"

// state.ts 模块加载期访问 document：mock 最小 DOM（同 messages.test.ts 模式）
const base = {
  classList: { add() {}, remove() {}, contains: () => false, toggle() {} },
  style: {},
  dataset: {},
  childNodes: [],
  children: [],
  append() {},
  appendChild() {},
  prepend() {},
  remove() {},
  insertAdjacentHTML() {},
  addEventListener() {},
  removeEventListener() {},
  setAttribute() {},
  getAttribute: () => null,
  querySelector: () => null,
  querySelectorAll: () => [],
  textContent: "",
  innerHTML: "",
  value: "",
  isConnected: true,
  open: true,
}
const doc = {
  getElementById: () => base,
  createElement: () => base,
  querySelector: () => null,
  querySelectorAll: () => [],
  addEventListener() {},
  body: base,
  documentElement: base,
  currentScript: null,
  baseURI: "http://localhost/",
}
;(globalThis as Record<string, unknown>).document = new Proxy(doc, {
  get(t, k) {
    if (typeof k === "string" && k in t) return (t as Record<string, unknown>)[k]
    return () => {}
  },
})
;(globalThis as Record<string, unknown>).window = globalThis
;(globalThis as Record<string, unknown>).navigator = { onLine: true }
;(globalThis as Record<string, unknown>).location = { protocol: "http:", host: "localhost" }

const { pendingTools, pendingToolsKey, clearPendingTools, setCurrentSession, getCurrentSession, isDraftView } = await import("./state")

function entry(sessionId: string, _toolCallId: string) {
  return { wrapper: base as unknown as HTMLElement, body: base as unknown as HTMLElement, session: sessionId, kind: "tool" as const, name: "sh" }
}

describe("草稿态标志（新会话懒创建）", () => {
  test("setCurrentSession(null) 进入草稿态，指定会话即退出", () => {
    const s = { id: "abc123", name: "新会话", userId: "admin", createdAt: 0, updatedAt: 0 }
    setCurrentSession(s)
    expect(getCurrentSession()?.id).toBe("abc123")
    expect(isDraftView()).toBe(false)
    // 点击「新会话」：进入空白草稿页（不创建会话）
    setCurrentSession(null)
    expect(getCurrentSession()).toBeNull()
    expect(isDraftView()).toBe(true)
    // 首条消息发送时创建会话：退出草稿态
    setCurrentSession({ ...s, id: "def456" })
    expect(isDraftView()).toBe(false)
    setCurrentSession(null)
  })
})

describe("pendingTools（会话隔离工具调用配对）", () => {
  test("key 按会话隔离：同名 toolCallId 跨会话不冲突；runId 区分子Agent 容器内调用", () => {
    expect(pendingToolsKey("aaa", "call_1")).toBe("aaa::call_1")
    expect(pendingToolsKey("bbb", "call_1")).toBe("bbb::call_1")
    expect(pendingToolsKey("aaa", "call_1")).not.toBe(pendingToolsKey("bbb", "call_1"))
    // 子Agent 容器内调用（带 runId）：与主循环同会话同名调用隔离
    expect(pendingToolsKey("aaa", "call_1", "r1")).toBe("aaa:r1:call_1")
    expect(pendingToolsKey("aaa", "call_1", "r1")).not.toBe(pendingToolsKey("aaa", "call_1"))
  })

  test("clearPendingTools 只清理指定会话的配对（后台结果残留不串台）", () => {
    pendingTools.set(pendingToolsKey("aaa", "call_1"), entry("aaa", "call_1"))
    pendingTools.set(pendingToolsKey("bbb", "call_1"), entry("bbb", "call_1"))
    pendingTools.set(pendingToolsKey("bbb", "call_2"), entry("bbb", "call_2"))
    clearPendingTools("aaa")
    expect(pendingTools.has("aaa::call_1")).toBe(false)
    expect(pendingTools.has("bbb::call_1")).toBe(true)
    expect(pendingTools.has("bbb::call_2")).toBe(true)
    clearPendingTools("bbb")
    expect(pendingTools.size).toBe(0)
    pendingTools.clear()
  })
})
