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
  // renderHeaderCtx（setConn 联动）取 #header-ctx .ctx-fill：返回 base（style/dataset 可写）
  querySelector: (sel: string) => (sel === "#header-ctx .ctx-fill" ? base : null),
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
// bun test 无 localStorage 全局：内存版 mock（setCurrentSession 的会话记忆读写用）
{
  const store = new Map<string, string>()
  ;(globalThis as Record<string, unknown>).localStorage = {
    getItem: (k: string) => (store.has(k) ? store.get(k)! : null),
    setItem: (k: string, v: string) => void store.set(k, String(v)),
    removeItem: (k: string) => void store.delete(k),
    clear: () => store.clear(),
  }
}

// headerCtxEl 经导入断言（bun test 全仓单进程共享模块缓存：state.ts 可能已被更早的测试文件以其
// mock 的 document 先加载，模块级 DOM 引用固定为那份数据集——断言必须落在模块实际持有的元素上）
const { pendingTools, pendingToolsKey, clearPendingTools, setCurrentSession, getCurrentSession, isDraftView, lastSessionId, setConn, setMaxCtxTokens, headerCtxEl } = await import("./state")

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

  test("草稿态清除记忆的会话：刷新后保持空白草稿页，而非跳回旧会话", () => {
    setCurrentSession({ id: "sess1", name: "会话", userId: "admin", createdAt: 0, updatedAt: 0 })
    expect(lastSessionId()).toBe("sess1")
    // 进入草稿页：清除当前会话记忆（init 刷新恢复读到空 → enterDraftView，草稿跨刷新保持）
    setCurrentSession(null)
    expect(lastSessionId()).toBeNull()
    // 切换到会话重新记忆；再进草稿再清除
    setCurrentSession({ id: "sess2", name: "会话2", userId: "admin", createdAt: 0, updatedAt: 0 })
    expect(lastSessionId()).toBe("sess2")
    setCurrentSession(null)
    expect(lastSessionId()).toBeNull()
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

describe("上下文圆环悬浮文案（#conn 纯状态载体，整个圆环区域悬浮）", () => {
  const fmt = (n: number) => n.toLocaleString()
  /** #header-ctx 的 data-tip 读取（模块持有的 DOM 引用在 mock 环境下经 unknown 取 dataset）。 */
  const tip = (): string | undefined => (headerCtxEl as unknown as { dataset: { tip?: string } }).dataset.tip

  test("已连接：上下文数值 + 缓存行；断开：首行断开原因、数值保留；恢复：原因移除", () => {
    setMaxCtxTokens(100000)
    setCurrentSession({ id: "ctx1", name: "会话", userId: "admin", createdAt: 0, updatedAt: 0, ctxTokens: 50000, ctxCachedTokens: 10000 })
    setConn("已连接")
    const ok = tip() as string
    expect(ok.startsWith(`上下文 ${fmt(50000)} / ${fmt(100000)} tokens（50%）`)).toBe(true)
    expect(ok).toContain(`缓存命中 ${fmt(10000)} tokens（20%）`)
    // 断开：原因置首行，上下文数值仍可见（#conn 铺满圆环但不以自有 tip 遮蔽整环悬浮）
    setConn("已断开，自动重连中…", false)
    const bad = tip() as string
    expect(bad.split("\n")[0]).toBe("已断开，自动重连中…")
    expect(bad).toContain(`上下文 ${fmt(50000)} / ${fmt(100000)} tokens（50%）`)
    // 恢复连接：断开原因移除，数值悬浮如常
    setConn("已连接")
    expect(tip()!.startsWith("上下文")).toBe(true)
    setCurrentSession(null)
  })

  test("无上下文数据 + 断开：悬浮仅剩断开原因；恢复且无数据：无悬浮", () => {
    setCurrentSession(null) // 草稿/无会话 → 无 ctx 数据
    setConn("连接失败: timeout", false)
    expect(tip()).toBe("连接失败: timeout")
    setConn("已连接")
    expect(tip()).toBeUndefined()
  })
})
