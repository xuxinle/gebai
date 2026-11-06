/**
 * 闲时时间段纯函数单测（零 IO）：解析/归一化/跨零点判定/星期归属/下次开始时刻。
 *
 * 这些用例的意义是把"跨零点窗口的凌晨算前一天"这条容易想当然的语义钉死——
 * 它直接决定「周六 22:00–06:00」会不会在周日凌晨白跑（用户设的"周末夜间"
 * 变成"周六晚上 + 周日凌晨"还是"含周一凌晨"，只差这一个判据）。
 */
import { describe, expect, test } from "bun:test"
import {
  describeIdleWindow,
  formatClock,
  isAlwaysOpen,
  isInIdleWindow,
  nextIdleWindowStart,
  normalizeIdleWindow,
  parseClock,
  windowLengthMinutes,
} from "./idle-window"

/** 构造本地时区某时刻（测试免时区漂移：一律用本地时间构造，判定也走本地时区）。 */
const at = (y: number, mo: number, d: number, h: number, mi = 0): number => new Date(y, mo - 1, d, h, mi).getTime()

describe("时刻解析与格式化", () => {
  test("parseClock：合法/非法", () => {
    expect(parseClock("00:00")).toBe(0)
    expect(parseClock("9:05")).toBe(545)
    expect(parseClock("23:59")).toBe(1439)
    expect(parseClock("24:00")).toBeNull()
    expect(parseClock("12:60")).toBeNull()
    expect(parseClock("abc")).toBeNull()
    expect(parseClock("")).toBeNull()
  })

  test("formatClock：补零与越界回绕", () => {
    expect(formatClock(0)).toBe("00:00")
    expect(formatClock(545)).toBe("09:05")
    expect(formatClock(1440)).toBe("00:00")
  })
})

describe("normalizeIdleWindow 校验与归一", () => {
  test("缺省/空 = 不限（undefined），不是空对象", () => {
    expect(normalizeIdleWindow(undefined)).toBeUndefined()
    expect(normalizeIdleWindow(null)).toBeUndefined()
  })

  test("基本形态：补零 + 星期升序去重", () => {
    expect(normalizeIdleWindow({ start: "9:00", end: "18:00", days: [5, 1, 1] })).toEqual({ days: [1, 5], start: "09:00", end: "18:00" })
  })

  test("星期空数组 = 每天（归一为省略，避免两套等价写法并存）", () => {
    expect(normalizeIdleWindow({ start: "22:00", end: "06:00", days: [] })).toEqual({ start: "22:00", end: "06:00" })
    expect(normalizeIdleWindow({ start: "22:00", end: "06:00", days: [0, 1, 2, 3, 4, 5, 6] })).toEqual({ start: "22:00", end: "06:00" })
  })

  test("非法值一律抛错并说清哪一段错了", () => {
    expect(() => normalizeIdleWindow({ start: "", end: "06:00" })).toThrow(/需要 start 与 end/)
    expect(() => normalizeIdleWindow({ start: "25:00", end: "06:00" })).toThrow(/start 非法/)
    expect(() => normalizeIdleWindow({ start: "22:00", end: "6点" })).toThrow(/end 非法/)
    expect(() => normalizeIdleWindow({ start: "22:00", end: "06:00", days: [7] })).toThrow(/days 含非法星期/)
    expect(() => normalizeIdleWindow({ start: "22:00", end: "06:00", timezone: "Mars/Base" })).toThrow(/timezone 非法/)
  })

  test("合法 IANA 时区保留", () => {
    expect(normalizeIdleWindow({ start: "22:00", end: "06:00", timezone: "Asia/Shanghai" })).toEqual({
      start: "22:00",
      end: "06:00",
      timezone: "Asia/Shanghai",
    })
  })
})

describe("窗口长度与全天判定", () => {
  test("同日窗口长度", () => {
    expect(windowLengthMinutes({ start: "09:00", end: "18:00" })).toBe(9 * 60)
  })

  test("跨零点窗口长度按加一天算", () => {
    expect(windowLengthMinutes({ start: "22:00", end: "06:00" })).toBe(8 * 60)
  })

  test("start === end 视为全天（填错也不至于任务永不执行）", () => {
    expect(windowLengthMinutes({ start: "00:00", end: "00:00" })).toBe(1440)
    expect(isAlwaysOpen({ start: "00:00", end: "00:00" })).toBe(true)
    expect(isAlwaysOpen({ start: "09:00", end: "18:00" })).toBe(false)
    expect(isAlwaysOpen(undefined)).toBe(true)
  })
})

describe("isInIdleWindow：同日窗口", () => {
  const w = { start: "09:00", end: "18:00" }
  test("窗口内 / 边界（左闭右开）/ 窗口外", () => {
    expect(isInIdleWindow(w, at(2026, 3, 4, 9, 0))).toBe(true)
    expect(isInIdleWindow(w, at(2026, 3, 4, 17, 59))).toBe(true)
    expect(isInIdleWindow(w, at(2026, 3, 4, 18, 0))).toBe(false) // 右端开区间：18:00 已出窗
    expect(isInIdleWindow(w, at(2026, 3, 4, 8, 59))).toBe(false)
    expect(isInIdleWindow(w, at(2026, 3, 4, 0, 30))).toBe(false)
  })

  test("无窗口 / 全天窗口：恒真", () => {
    expect(isInIdleWindow(undefined, at(2026, 3, 4, 3, 0))).toBe(true)
    expect(isInIdleWindow({ start: "00:00", end: "00:00" }, at(2026, 3, 4, 3, 0))).toBe(true)
  })
})

describe("isInIdleWindow：跨零点窗口", () => {
  const w = { start: "22:00", end: "06:00" }
  test("当日深夜与次日凌晨都在窗内", () => {
    expect(isInIdleWindow(w, at(2026, 3, 4, 22, 0))).toBe(true)
    expect(isInIdleWindow(w, at(2026, 3, 4, 23, 59))).toBe(true)
    expect(isInIdleWindow(w, at(2026, 3, 5, 0, 0))).toBe(true)
    expect(isInIdleWindow(w, at(2026, 3, 5, 5, 59))).toBe(true)
    expect(isInIdleWindow(w, at(2026, 3, 5, 6, 0))).toBe(false)
    expect(isInIdleWindow(w, at(2026, 3, 4, 21, 59))).toBe(false)
  })
})

describe("isInIdleWindow：星期归属（跨零点的凌晨算前一天那一场）", () => {
  // 2026-03-07 是周六
  test("只在周六开窗 22:00–06:00 → 周日凌晨仍算窗内，周一凌晨不算", () => {
    const w = { days: [6], start: "22:00", end: "06:00" }
    expect(isInIdleWindow(w, at(2026, 3, 7, 22, 30))).toBe(true) // 周六深夜
    expect(isInIdleWindow(w, at(2026, 3, 8, 3, 0))).toBe(true) // 周日凌晨：属周六那一场
    expect(isInIdleWindow(w, at(2026, 3, 8, 22, 30))).toBe(false) // 周日晚：不在列表
    expect(isInIdleWindow(w, at(2026, 3, 9, 3, 0))).toBe(false) // 周一凌晨：属周日那一场，不算
  })

  test("只在周日开窗的同日窗口 → 只周日白天", () => {
    const w = { days: [0], start: "09:00", end: "12:00" }
    expect(isInIdleWindow(w, at(2026, 3, 8, 10, 0))).toBe(true) // 周日
    expect(isInIdleWindow(w, at(2026, 3, 7, 10, 0))).toBe(false) // 周六
  })
})

describe("nextIdleWindowStart：下次进入窗口", () => {
  test("全天窗口立即返回（视为永远开启）", () => {
    const from = at(2026, 3, 4, 10, 0)
    expect(nextIdleWindowStart(undefined, from)).toBe(from)
  })

  test("在窗口内 → 返回当前时刻（供马上可跑判定）", () => {
    const from = at(2026, 3, 4, 10, 0)
    expect(nextIdleWindowStart({ start: "09:00", end: "18:00" }, from)).toBe(from)
  })

  test("窗口外 → 返回当天边界；已过边界 → 次日边界", () => {
    expect(nextIdleWindowStart({ start: "22:00", end: "06:00" }, at(2026, 3, 4, 10, 0))).toBe(at(2026, 3, 4, 22, 0))
    expect(nextIdleWindowStart({ start: "22:00", end: "06:00" }, at(2026, 3, 4, 23, 0))).toBe(at(2026, 3, 4, 23, 0))
  })

  test("指定星期：跳过不允许的日子", () => {
    // 2026-03-04 是周三；只在周六开窗 → 下次是 03-07 22:00
    expect(nextIdleWindowStart({ days: [6], start: "22:00", end: "06:00" }, at(2026, 3, 4, 10, 0))).toBe(at(2026, 3, 7, 22, 0))
  })
})

describe("describeIdleWindow 人类可读文案", () => {
  test("不限时段", () => {
    expect(describeIdleWindow(undefined)).toContain("不限时段")
    expect(describeIdleWindow({ start: "00:00", end: "00:00" })).toContain("不限时段")
  })

  test("每天跨零点 / 指定星期同日窗口", () => {
    expect(describeIdleWindow({ start: "22:00", end: "06:00" })).toBe("每天 22:00–06:00（跨零点）")
    // 星期按数字升序展示（与归一化后的存储顺序一致，便于 diff 稳定）
    expect(describeIdleWindow({ days: [0, 6], start: "09:00", end: "18:00" })).toBe("周日/周六 09:00–18:00")
    expect(describeIdleWindow({ days: [1, 2, 3, 4, 5], start: "22:00", end: "02:00" })).toBe("周一/周二/周三/周四/周五 22:00–02:00（跨零点）")
  })

  test("带时区时一并标注", () => {
    expect(describeIdleWindow({ start: "22:00", end: "06:00", timezone: "Asia/Shanghai" })).toContain("Asia/Shanghai")
  })
})
