import { afterAll, beforeEach, describe, expect, test } from "bun:test"
import { applyTokenRate, getTokenRateSetting, isTokenRateEnabled, noteTpsFrame, renderTokenRate, setTokenRateSetting } from "./token-rate"
import { tokenRateEl, setCurrentSession } from "./state"

/** 用完全局存储要放回基线那一份（见 scripts/test-preload.ts 的说明）。 */
const prevLocalStorage = (globalThis as Record<string, unknown>).localStorage
afterAll(() => {
  ;(globalThis as Record<string, unknown>).localStorage = prevLocalStorage
})

/** 速率文本节点桩（基线 DOM 的 querySelector 恒为 null）：断言常驻显示内容用；用毕恢复。 */
const textStub = { textContent: "" }
const prevQuerySelector = tokenRateEl.querySelector
;(tokenRateEl as unknown as { querySelector: unknown }).querySelector = (sel: string) => (sel === ".tr-text" ? textStub : null)
afterAll(() => {
  ;(tokenRateEl as unknown as { querySelector: unknown }).querySelector = prevQuerySelector
})

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

/** 根元素（applyTokenRate 写 data-token-rate 的落点）。 */
function root(): HTMLElement & { dataset: Record<string, string | undefined> } {
  return document.documentElement as never
}

/** 服务端帧（默认实测收尾帧；估算/生成中帧由用例显式传）。 */
function frame(tps: number, extra: Partial<{ outTokens: number; genMs: number; est: boolean; active: boolean }> = {}) {
  return { tps, outTokens: extra.outTokens ?? Math.round(tps * 10), genMs: extra.genMs ?? 10_000, est: extra.est ?? false, active: extra.active ?? false }
}

beforeEach(() => {
  store.clear()
  setCurrentSession(null)
  applyTokenRate()
})

describe("token-rate 设置开关", () => {
  test("默认开启；不存/其它存储值均视为开启；set off 持久化并标记根元素；set on 清存储", () => {
    expect(getTokenRateSetting()).toBe("on")
    expect(isTokenRateEnabled()).toBe(true)
    store.set("gebai.ui.tokenRate", "garbage")
    expect(getTokenRateSetting()).toBe("on")

    setTokenRateSetting("off")
    expect(store.get("gebai.ui.tokenRate")).toBe("off")
    expect(isTokenRateEnabled()).toBe(false)
    expect(root().dataset.tokenRate).toBe("off")
    expect(tokenRateEl.hidden).toBe(true) // 关闭即隐藏

    setTokenRateSetting("on")
    expect(store.has("gebai.ui.tokenRate")).toBe(false)
    expect(isTokenRateEnabled()).toBe(true)
    expect(root().dataset.tokenRate).toBeUndefined()
  })

  test("applyTokenRate 按当前设置幂等应用根元素标记", () => {
    applyTokenRate()
    expect(root().dataset.tokenRate).toBeUndefined()
    store.set("gebai.ui.tokenRate", "off")
    applyTokenRate()
    expect(root().dataset.tokenRate).toBe("off")
  })
})

describe("token-rate 展示（服务端帧为唯一数据源）", () => {
  test("帧到达即渲染：路况分级、估算标记与提示文案", () => {
    setCurrentSession({ id: "s-rate-1" } as never)
    noteTpsFrame("s-rate-1", frame(90, { outTokens: 900 }))
    expect(tokenRateEl.hidden).toBe(false)
    expect(tokenRateEl.dataset.rate).toBe("1") // ≥80：畅通（绿）
    expect(tokenRateEl.dataset.est).toBeUndefined() // 实测帧不带估算标记
    expect(textStub.textContent).toBe("90.0") // 常驻只显示数值（无单位）
    // 提示两行：速率行（单位/路况/口径）+ 依据行（输出 tokens / 生成窗口）
    expect(String(tokenRateEl.dataset.tip)).toBe("90.0 tok/s · 畅通（实测）\n输出 900 tokens / 10.0s")

    noteTpsFrame("s-rate-1", frame(12, { est: true, active: true }))
    expect(tokenRateEl.dataset.rate).toBe("5") // 8-18：拥堵
    expect(tokenRateEl.dataset.est).toBe("1") // 估算帧：弱化展示
    expect(String(tokenRateEl.dataset.tip)).toContain("（估算）")
  })

  test("仪表常驻：两帧之间保持上一帧的值与配色（工具执行/等待期不闪没）", () => {
    setCurrentSession({ id: "s-rate-keep" } as never)
    noteTpsFrame("s-rate-keep", frame(55, { active: true, est: true }))
    expect(textStub.textContent).toBe("55.0")
    // 期间没有任何新帧（工具执行 / 审批等待 / 思考停顿）：重渲染（如会话切换回来）仍是上一帧
    renderTokenRate()
    expect(tokenRateEl.hidden).toBe(false)
    expect(textStub.textContent).toBe("55.0")
    expect(tokenRateEl.dataset.rate).toBe("2") // 50-80：顺畅
  })

  test("切到无速率帧的会话隐藏，切回有帧的会话恢复该会话的值", () => {
    setCurrentSession({ id: "s-rate-a" } as never)
    noteTpsFrame("s-rate-a", frame(35))
    setCurrentSession({ id: "s-rate-b" } as never)
    renderTokenRate() // 真实环境由 gebai:session-view 事件触发（测试基线 document 为 no-op）
    expect(tokenRateEl.hidden).toBe(true)
    setCurrentSession({ id: "s-rate-a" } as never)
    renderTokenRate()
    expect(tokenRateEl.hidden).toBe(false)
    expect(textStub.textContent).toBe("35.0")
  })

  test("关闭后不再渲染（帧到达也停摆）", () => {
    setCurrentSession({ id: "s-rate-off" } as never)
    setTokenRateSetting("off")
    noteTpsFrame("s-rate-off", frame(66))
    expect(tokenRateEl.hidden).toBe(true)
  })
})
