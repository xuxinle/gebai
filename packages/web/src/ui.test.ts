import { describe, expect, test } from "bun:test"

// state.ts 模块加载期访问 document（ui.ts → state.ts）：mock 最小 DOM（同 state.test.ts 模式）
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

const { autoHideScrollbar } = await import("./ui")

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
