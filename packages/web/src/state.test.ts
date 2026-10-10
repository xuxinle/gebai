import { afterAll, describe, expect, test } from "bun:test"

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
// 本文件在模块顶层整体替换了 document/window/navigator/location/localStorage——按基线约定
// （scripts/test-preload.ts）**用完必须放回**：整体替换而不还原会把测试顺序变回变量（后续文件
// 读到本文件的桩，表现为单文件全过、全量随文件执行顺序报错）。
const prevGlobals = {
  document: (globalThis as Record<string, unknown>).document,
  window: (globalThis as Record<string, unknown>).window,
  navigator: (globalThis as Record<string, unknown>).navigator,
  location: (globalThis as Record<string, unknown>).location,
  localStorage: (globalThis as Record<string, unknown>).localStorage,
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
afterAll(() => {
  const g = globalThis as Record<string, unknown>
  // 只在原本存在时才写回：若本文件是在无基线环境（如跨包混合运行、preload 未生效）下首个装桩者，
  // 把 document 写回为 undefined 比原来的「不还原」更糟——后续文件会拿到一个存在但为 undefined 的全局。
  const restore = (key: string, prev: unknown) => {
    if (prev !== undefined) g[key] = prev
  }
  restore("document", prevGlobals.document)
  restore("window", prevGlobals.window)
  restore("navigator", prevGlobals.navigator)
  restore("location", prevGlobals.location)
  restore("localStorage", prevGlobals.localStorage)
})

// headerCtxEl 经导入断言（bun test 全仓单进程共享模块缓存：state.ts 可能已被更早的测试文件以其
// mock 的 document 先加载，模块级 DOM 引用固定为那份数据集——断言必须落在模块实际持有的元素上）
// updateTitle 回归组在此导入（标志空 = 未定制）；定制组见文件末尾的二次动态 import。
const { pendingTools, pendingToolsKey, clearPendingTools, setCurrentSession, getCurrentSession, isDraftView, lastSessionId, setConn, setMaxCtxTokens, headerCtxEl, runs, syncConnThinking, filesPreview, updateTitle } = await import("./state")

function entry(sessionId: string, _toolCallId: string) {
  return { wrapper: base as unknown as HTMLElement, body: base as unknown as HTMLElement, session: sessionId, kind: "tool" as const, name: "sh" }
}

describe("会话运行信号（全屏特效降频）", () => {
  const rootDataset = () => (base as unknown as { dataset: Record<string, string> }).dataset

  test("当前会话运行中设 data-fx-busy，运行结束清除", () => {
    const s = { id: "busy1", name: "会话", userId: "admin", createdAt: 0, updatedAt: 0 }
    setCurrentSession(s)
    runs.set(s.id, {} as never)
    syncConnThinking()
    expect(rootDataset().fxBusy).toBe("on")
    runs.delete(s.id)
    syncConnThinking()
    expect(rootDataset().fxBusy).toBeUndefined()
    setCurrentSession(null)
  })

  test("后台会话运行不降频当前视图（仅当前会话运行才算运行中）", () => {
    const s = { id: "busy2", name: "会话", userId: "admin", createdAt: 0, updatedAt: 0 }
    setCurrentSession(s)
    runs.set("other-session", {} as never)
    syncConnThinking()
    expect(rootDataset().fxBusy).toBeUndefined()
    runs.delete("other-session")
    setCurrentSession(null)
  })
})

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

describe("文件预览取数 URL（按页面基准解析）", () => {
  test("根部署：/api/v1/... 原样", () => {
    expect(filesPreview("s1", "tmp/a.txt")).toBe("/api/v1/sessions/s1/files/preview?path=tmp%2Fa.txt")
    expect(filesPreview("s1", "tmp/a.txt", true)).toContain("download=1")
  })

  test("反代子路径：链接带页面基准前缀（无需配置）", () => {
    const doc = (globalThis as unknown as { document: { baseURI: string } }).document
    const prev = doc.baseURI
    doc.baseURI = "http://localhost/gebai/files?root=proj"
    try {
      expect(filesPreview("s1", "tmp/a.txt")).toBe("/gebai/api/v1/sessions/s1/files/preview?path=tmp%2Fa.txt")
    } finally {
      doc.baseURI = prev
    }
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

describe("浏览器 tab 标题（GEBAI_TITLE → window.__GEBAI_TITLE__，模块加载期固化）", () => {
  test("未注入时回归默认「歌白」（无论切换会话与否）", () => {
    const doc = (globalThis as Record<string, unknown>).document as { title: string }
    updateTitle()
    expect(doc.title).toBe("歌白")
    setCurrentSession({ id: "t1", name: "会话A", userId: "admin", createdAt: 0, updatedAt: 0 })
    updateTitle()
    expect(doc.title).toBe("歌白")
    setCurrentSession(null)
  })

  test("注入自定义标题时 updateTitle 用定制值（空白注入回落默认）", async () => {
    // 与上面的回归组同进程同模块缓存：换标志须 query-bust 重新动态 import 才能重建模块级 BRAND_TITLE；
    // 说明符经变量拼出（非字面量）——TS 不静态解析带 query 的路径，避免 TS2307
    const bust = (q: string) => import(/* @vite-ignore */ "./state?" + q) as Promise<typeof import("./state")>
    ;(window as unknown as { __GEBAI_TITLE__?: string }).__GEBAI_TITLE__ = "  我的工作台  "
    try {
      const mod = await bust("title")
      const doc = (globalThis as Record<string, unknown>).document as { title: string }
      mod.updateTitle()
      expect(doc.title).toBe("我的工作台")
      // 空白值视为未定制：回落「歌白」（与 config 侧 trim-空-undefined 同口径）
      ;(window as unknown as { __GEBAI_TITLE__?: string }).__GEBAI_TITLE__ = "   "
      const mod2 = await bust("title-blank")
      mod2.updateTitle()
      expect(doc.title).toBe("歌白")
    } finally {
      delete (window as unknown as { __GEBAI_TITLE__?: string }).__GEBAI_TITLE__
    }
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
