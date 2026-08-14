import { describe, expect, test } from "bun:test"
import { detectLowPower, getLowPowerSetting, setLowPowerSetting } from "./low-power"

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
// setLowPowerSetting → applyLowPower 需要 document/navigator：最小桩
;(globalThis as Record<string, unknown>).document = {
  documentElement: { dataset: {} },
  createElement: () => ({ getContext: () => null }),
  dispatchEvent: () => true,
} as unknown as Document
;(globalThis as Record<string, unknown>).navigator = { hardwareConcurrency: 8, deviceMemory: 8 }

describe("low-power detection", () => {
  test("no WebGL（无 GPU）→ 低性能，即使核数/内存充足", () => {
    expect(detectLowPower({ hasWebGL: false })).toBe(true)
    expect(detectLowPower({ hasWebGL: false, cores: 16, memoryGb: 64 })).toBe(true)
  })

  test("低核数（≤4）→ 低性能", () => {
    expect(detectLowPower({ hasWebGL: true, cores: 2 })).toBe(true)
    expect(detectLowPower({ hasWebGL: true, cores: 4, memoryGb: 8 })).toBe(true)
  })

  test("低内存（≤4GB）→ 低性能", () => {
    expect(detectLowPower({ hasWebGL: true, cores: 8, memoryGb: 4 })).toBe(true)
    expect(detectLowPower({ hasWebGL: true, cores: 8, memoryGb: 2 })).toBe(true)
  })

  test("正常机器（WebGL + 8 核 + 8GB+）→ 标准模式", () => {
    expect(detectLowPower({ hasWebGL: true, cores: 8, memoryGb: 16 })).toBe(false)
    expect(detectLowPower({ hasWebGL: true, cores: 16, memoryGb: 64 })).toBe(false)
    expect(detectLowPower({ hasWebGL: true, cores: 8 })).toBe(false)
  })

  test("未上报的信号（undefined/0）不参与判定", () => {
    expect(detectLowPower({ hasWebGL: true, cores: 0, memoryGb: 0 })).toBe(false)
    expect(detectLowPower({ hasWebGL: true, cores: 4, memoryGb: undefined })).toBe(true)
  })
})

describe("low-power setting (single switch)", () => {
  test("默认 off（自动检测）；旧三态存储值兼容映射为 off", () => {
    store.clear()
    expect(getLowPowerSetting()).toBe("off")
    store.set("gebai.ui.lowPower", "auto") // 旧三态
    expect(getLowPowerSetting()).toBe("off")
    store.set("gebai.ui.lowPower", "off") // 旧三态「始终关闭」→ 自动检测
    expect(getLowPowerSetting()).toBe("off")
    store.set("gebai.ui.lowPower", "on")
    expect(getLowPowerSetting()).toBe("on")
  })

  test("set on 持久化；set off 清除存储（恢复自动检测）", () => {
    store.clear()
    setLowPowerSetting("on")
    expect(store.get("gebai.ui.lowPower")).toBe("on")
    setLowPowerSetting("off")
    expect(store.has("gebai.ui.lowPower")).toBe(false)
    expect(getLowPowerSetting()).toBe("off")
  })
})
