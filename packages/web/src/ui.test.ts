import { describe, expect, test } from "bun:test"

// state.ts 模块加载期访问 document（ui.ts → state.ts）：mock 最小 DOM（同 state.test.ts 模式）。
// createElement 每次返回独立可追踪节点（tooltip 移除观察用）；document 监听器记录供测试触发。
const createdNodes: Array<{ removed: boolean }> = []
function makeTipNode(): { removed: boolean } & Record<string, unknown> {
  const node = {
    classList: { add() {}, remove() {}, contains: () => false, toggle() {} },
    style: {},
    dataset: {},
    textContent: "",
    removed: false,
    offsetWidth: 100,
    offsetHeight: 30,
    appendChild() {},
    remove() {
      node.removed = true
    },
  }
  createdNodes.push(node)
  return node as { removed: boolean } & Record<string, unknown>
}
const docListeners: Array<{ type: string; cb: (e: never) => void }> = []
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
  createElement: () => makeTipNode(),
  querySelector: () => null,
  querySelectorAll: () => [],
  addEventListener(type: string, cb: (e: never) => void) {
    docListeners.push({ type, cb })
  },
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

const { autoHideScrollbar, bindTooltips } = await import("./ui")

/** 触发 bindTooltips 注册在 document 上的监听器。 */
function fireDoc(type: string, e: { target: unknown }): void {
  for (const l of docListeners) if (l.type === type) l.cb(e as never)
}

/** 悬浮宿主 fake：自身携带 data-tip（closest 命中自身）。 */
function makeTipHost(): Record<string, unknown> {
  const host: Record<string, unknown> = {
    dataset: { tip: "上下文 50,000 / 100,000 tokens（50%）" },
    getBoundingClientRect: () => ({ top: 12, left: 400, width: 24, height: 24, bottom: 36 }),
  }
  host.closest = (sel: string) => (sel === "[data-tip]" ? host : null)
  return host
}

/** 滚动容器 fake：contains 仅对传入宿主返回 true（host 为 null 表示不含任何宿主）。 */
function makeScroller(host: unknown): { contains: (n: unknown) => boolean } {
  return { contains: (n: unknown) => host !== null && n === host }
}

/** 容器 fake：捕获 scroll 监听 + 真实语义 classList（Set 增删查）。 */
function makeEl() {
  const listeners: Array<() => void> = []
  const classes = new Set<string>()
  return {
    addEventListener(_type: string, cb: () => void) {
      listeners.push(cb)
    },
    classList: {
      add: (c: string) => void classes.add(c),
      remove: (c: string) => void classes.delete(c),
      contains: (c: string) => classes.has(c),
    },
    emitScroll() {
      for (const cb of listeners) cb()
    },
  }
}

describe("autoHideScrollbar（滚动条自动显隐）", () => {
  test("滚动中加 .scrolling，停止 hideDelayMs 后移除", async () => {
    const el = makeEl()
    autoHideScrollbar(el as unknown as HTMLElement, { hideDelayMs: 10 })
    expect(el.classList.contains("scrolling")).toBe(false)
    el.emitScroll()
    expect(el.classList.contains("scrolling")).toBe(true)
    await Bun.sleep(30)
    expect(el.classList.contains("scrolling")).toBe(false)
  })

  test("连续滚动重置计时：滚动未停滑块不消失", async () => {
    const el = makeEl()
    autoHideScrollbar(el as unknown as HTMLElement, { hideDelayMs: 40 })
    el.emitScroll()
    await Bun.sleep(25)
    el.emitScroll() // 重置计时
    await Bun.sleep(25)
    expect(el.classList.contains("scrolling")).toBe(true) // 距上次滚动 25ms < 40ms
    await Bun.sleep(40)
    expect(el.classList.contains("scrolling")).toBe(false)
  })

  test("默认延迟路径（不注入选项）：滚动即加类，不抛错", () => {
    const el = makeEl()
    autoHideScrollbar(el as unknown as HTMLElement)
    el.emitScroll()
    expect(el.classList.contains("scrolling")).toBe(true)
  })
})

describe("bindTooltips 滚动隐藏收窄（无关容器的滚动不打断悬浮）", () => {
  test("悬浮宿主所在容器滚动：隐藏（原语义保留）", () => {
    bindTooltips()
    const host = makeTipHost()
    fireDoc("pointerover", { target: host })
    const tip = createdNodes[createdNodes.length - 1]
    expect(tip.removed).toBe(false)
    // 宿主所在滚动容器（如会话列表）滚动 → tooltip 随宿主漂移，隐藏
    fireDoc("scroll", { target: makeScroller(host) })
    expect(tip.removed).toBe(true)
  })

  test("无关容器滚动（生成中消息流自动滚动 vs 标题栏圆环）：不隐藏", () => {
    bindTooltips() // 单测过滤运行时自足注册（重复注册幂等：同宿主 showTooltip 早退、hideTooltip 空安全）
    const host = makeTipHost()
    fireDoc("pointerover", { target: host })
    const tip = createdNodes[createdNodes.length - 1]
    // 其它容器滚动（不含宿主）：此前一律隐藏导致「信号灯闪烁（生成）期间悬浮刚出现即被冲掉」
    fireDoc("scroll", { target: makeScroller(null) })
    fireDoc("scroll", { target: makeScroller({}) })
    expect(tip.removed).toBe(false)
    // 页面级滚动（target 为 document）：保守保留原隐藏语义
    fireDoc("scroll", { target: document })
    expect(tip.removed).toBe(true)
  })
})
