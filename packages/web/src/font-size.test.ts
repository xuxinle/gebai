import { describe, expect, test } from "bun:test"
import { applyFontSize, getFontSizeSetting, setFontSizeSetting } from "./font-size"

/** bun test 无 DOM：提供最小 localStorage mock（font-size 内部 try/catch 兜底，mock 用于验证存取语义）。 */
const store = new Map<string, string>()
;(globalThis as Record<string, unknown>).localStorage = {
  getItem: (k: string) => store.get(k) ?? null,
  setItem: (k: string, v: string) => void store.set(k, v),
  removeItem: (k: string) => void store.delete(k),
  clear: () => store.clear(),
  key: (i: number) => [...store.keys()][i] ?? null,
  get length() {
    return store.size
  },
} as unknown as Storage
// applyFontSize 需要 document：最小桩
const dataset: Record<string, string> = {}
;(globalThis as Record<string, unknown>).document = {
  documentElement: { dataset },
} as unknown as Document

describe("font-size setting (三档)", () => {
  test("默认标准；非法存储值按标准处理", () => {
    store.clear()
    expect(getFontSizeSetting()).toBe("std")
    store.set("gebai.ui.fontSize", "xxl") // 非法值
    expect(getFontSizeSetting()).toBe("std")
    store.set("gebai.ui.fontSize", "lg")
    expect(getFontSizeSetting()).toBe("lg")
    store.set("gebai.ui.fontSize", "xl")
    expect(getFontSizeSetting()).toBe("xl")
  })

  test("apply：lg/xl 写根元素 data-fontsize，std 不留属性", () => {
    store.clear()
    applyFontSize()
    expect(dataset.fontsize).toBeUndefined()
    store.set("gebai.ui.fontSize", "lg")
    applyFontSize()
    expect(dataset.fontsize).toBe("lg")
    store.set("gebai.ui.fontSize", "xl")
    applyFontSize()
    expect(dataset.fontsize).toBe("xl")
  })

  test("set 持久化；set std 清除存储（恢复默认）", () => {
    store.clear()
    setFontSizeSetting("xl")
    expect(store.get("gebai.ui.fontSize")).toBe("xl")
    expect(dataset.fontsize).toBe("xl")
    setFontSizeSetting("std")
    expect(store.has("gebai.ui.fontSize")).toBe(false)
    expect(dataset.fontsize).toBeUndefined()
  })
})
