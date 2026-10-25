/** 页面不可见时段的计时（后台标签页 / 手机切 App / 锁屏）：空闲看门狗据此把后台时长从「无数据」里扣除。
 *  纯逻辑测试——全部函数接受显式时间戳，不依赖真实定时器与 document 状态（后者单独用例覆盖）。 */
import { beforeEach, describe, expect, test } from "bun:test"
import { effectiveIdleMs, isPageHidden, markHidden, markVisible, onPageVisible, resetPageIdle, visibleElapsedMs } from "./page-idle"

const T0 = 1_700_000_000_000

beforeEach(() => {
  resetPageIdle(T0)
})

describe("隐藏时长从空闲计算中扣除", () => {
  test("已结束的隐藏时段：整段扣除", () => {
    markVisible(T0) // 起算（此前无进行中时段，no-op）
    markHidden(T0 + 10_000)
    markVisible(T0 + 200_000)
    // 墙钟 200s，其中隐藏 190s → 有效空闲 10s
    expect(effectiveIdleMs(T0, T0 + 200_000)).toBe(10_000)
  })

  test("进行中的隐藏时段：按与查询区间的重叠部分扣（不必等它结束）", () => {
    markHidden(T0 + 10_000)
    // 未 markVisible：now 处的隐藏仍在进行，只扣 [T0+10s, now] 这段
    expect(effectiveIdleMs(T0, T0 + 200_000)).toBe(10_000)
  })

  test("多段隐藏累加扣除", () => {
    markHidden(T0 + 10_000)
    markVisible(T0 + 30_000) // 隐藏 20s
    markHidden(T0 + 50_000)
    markVisible(T0 + 100_000) // 隐藏 50s
    expect(effectiveIdleMs(T0, T0 + 120_000)).toBe(120_000 - 70_000)
  })

  test("早于区间的隐藏时段不参与扣除（防「扣成负数 → 看门狗永不触发」）", () => {
    // 页面在任务开始前就隐藏过一大段：不得从后来的空闲里扣
    markHidden(T0)
    markVisible(T0 + 100_000)
    const from = T0 + 150_000
    expect(effectiveIdleMs(from, from + 10_000)).toBe(10_000)
  })

  test("部分重叠：只扣落在区间内的那一截", () => {
    markHidden(T0 - 60_000)
    markVisible(T0 - 30_000) // 区间开始前已结束 → 不扣
    markHidden(T0 + 5_000)
    markVisible(T0 + 25_000) // 区间内隐藏 20s
    expect(effectiveIdleMs(T0, T0 + 30_000)).toBe(10_000)
  })

  test("全程隐藏：有效空闲为 0（切后台多久都不算挂起）", () => {
    markHidden(T0 - 1_000)
    expect(effectiveIdleMs(T0, T0 + 3_600_000)).toBe(0)
  })

  test("从未隐藏：有效空闲等于墙钟间隔", () => {
    expect(visibleElapsedMs(T0, T0 + 42_000)).toBe(42_000)
  })

  test("时间倒流（系统时钟回拨）不为负", () => {
    expect(visibleElapsedMs(T0 + 10_000, T0)).toBe(0)
  })
})

describe("可见性状态", () => {
  test("document 可见时 isPageHidden 为假（测试基线 document 可见）", () => {
    expect(isPageHidden()).toBe(false)
  })

  test("document.hidden 为真时 isPageHidden 为真，且随后可恢复", () => {
    const doc = document as unknown as { hidden: boolean; visibilityState: string }
    const prevHidden = doc.hidden
    const prevState = doc.visibilityState
    try {
      doc.hidden = true
      doc.visibilityState = "hidden"
      expect(isPageHidden()).toBe(true)
    } finally {
      doc.hidden = prevHidden
      doc.visibilityState = prevState
    }
    expect(isPageHidden()).toBe(false)
  })

  test("onPageVisible 注册与退订不抛错（基线 document 的事件为 no-op）", () => {
    const off = onPageVisible(() => {})
    expect(typeof off).toBe("function")
    off()
  })
})

describe("幂等", () => {
  test("重复 markHidden 不重置起点；重复 markVisible 不多算", () => {
    markHidden(T0)
    markHidden(T0 + 50_000) // 幂等：起点仍是 T0
    markVisible(T0 + 100_000)
    markVisible(T0 + 999_999) // 无进行中时段 → no-op
    expect(effectiveIdleMs(T0, T0 + 100_000)).toBe(0) // 全程隐藏 100s，墙钟也是 100s
    expect(effectiveIdleMs(T0 + 100_000, T0 + 200_000)).toBe(100_000) // 之后全可见
  })
})
