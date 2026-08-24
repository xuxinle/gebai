import { describe, expect, test } from "bun:test"
import { getLowPowerSetting, isLowPower, setLowPowerSetting } from "./low-power"

/** bun test 无 DOM：提供最小 localStorage mock（low-power 内部 try/catch 兜底，mock 用于验证存取语义）。 */
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
// setLowPowerSetting → applyLowPower 需要 document：最小桩
;(globalThis as Record<string, unknown>).document = {
  documentElement: { dataset: {} },
  dispatchEvent: () => true,
} as unknown as Document

describe("low-power setting (manual switch)", () => {
  test("默认关闭（不做硬件自动检测）；旧三态存储值兼容映射为关闭", () => {
    store.clear()
    expect(getLowPowerSetting()).toBe("off")
    expect(isLowPower()).toBe(false)
    store.set("gebai.ui.lowPower", "auto") // 旧三态
    expect(getLowPowerSetting()).toBe("off")
    expect(isLowPower()).toBe(false)
    store.set("gebai.ui.lowPower", "off") // 旧三态
    expect(getLowPowerSetting()).toBe("off")
    expect(isLowPower()).toBe(false)
    store.set("gebai.ui.lowPower", "on")
    expect(getLowPowerSetting()).toBe("on")
    expect(isLowPower()).toBe(true)
  })

  test("set on 持久化；set off 清除存储（恢复关闭）", () => {
    store.clear()
    setLowPowerSetting("on")
    expect(store.get("gebai.ui.lowPower")).toBe("on")
    setLowPowerSetting("off")
    expect(store.has("gebai.ui.lowPower")).toBe(false)
    expect(getLowPowerSetting()).toBe("off")
    expect(isLowPower()).toBe(false)
  })
})
