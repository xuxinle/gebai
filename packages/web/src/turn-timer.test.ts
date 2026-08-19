import { describe, expect, test } from "bun:test"
import { applyTurnTimer, getTurnTimerSetting, isTurnTimerEnabled, setTurnTimerSetting } from "./turn-timer"

/** bun test 无 DOM：提供最小 localStorage mock（turn-timer 内部 try/catch 兜底，mock 用于验证存取语义）。 */
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
// setTurnTimerSetting → applyTurnTimer 需要 document：最小桩
;(globalThis as Record<string, unknown>).document = {
  documentElement: { dataset: {} },
  dispatchEvent: () => true,
} as unknown as Document

describe("turn-timer setting", () => {
  test("默认开启；不存/其它存储值均视为开启", () => {
    store.clear()
    expect(getTurnTimerSetting()).toBe("on")
    expect(isTurnTimerEnabled()).toBe(true)
    store.set("gebai.ui.turnTimer", "garbage")
    expect(getTurnTimerSetting()).toBe("on")
  })

  test("set off 持久化并标记根元素；set on 清除存储（恢复默认开启）", () => {
    store.clear()
    const root = (globalThis as { document: Document }).document.documentElement
    setTurnTimerSetting("off")
    expect(store.get("gebai.ui.turnTimer")).toBe("off")
    expect(isTurnTimerEnabled()).toBe(false)
    expect(root.dataset.turnTimer).toBe("off")
    setTurnTimerSetting("on")
    expect(store.has("gebai.ui.turnTimer")).toBe(false)
    expect(isTurnTimerEnabled()).toBe(true)
    expect(root.dataset.turnTimer).toBeUndefined()
  })

  test("applyTurnTimer 按当前设置幂等应用根元素标记", () => {
    store.clear()
    applyTurnTimer()
    expect(root2().dataset.turnTimer).toBeUndefined()
    store.set("gebai.ui.turnTimer", "off")
    applyTurnTimer()
    expect(root2().dataset.turnTimer).toBe("off")
    store.clear()
    applyTurnTimer()
    expect(root2().dataset.turnTimer).toBeUndefined()
  })
})

function root2() {
  return (globalThis as { document: Document }).document.documentElement
}
