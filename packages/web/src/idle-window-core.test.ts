/** 闲时时间段前端纯逻辑单测：文案/表单双向映射/快捷预设/跨零点标注。
 *
 * 守的口径与服务端 `idle-window.ts` 一致（两边各有一份实现，因为前端不能 import 服务端模块）——
 * 这里逐条对齐关键语义，任一侧改了规则而另一侧没跟，用例就会红。
 */
import { describe, expect, test } from "bun:test"
import {
  describeIdleWindow,
  emptyIdleWindowForm,
  formFromIdleWindow,
  formatClock,
  idleWindowFromForm,
  IDLE_PRESETS,
  isAlwaysOpen,
  isCrossMidnight,
  matchPreset,
  parseClock,
} from "./idle-window-core"

describe("时刻解析（与服务端同规则）", () => {
  test("合法/非法", () => {
    expect(parseClock("00:00")).toBe(0)
    expect(parseClock("9:05")).toBe(545)
    expect(parseClock("23:59")).toBe(1439)
    expect(parseClock("24:00")).toBeNull()
    expect(parseClock("12:60")).toBeNull()
    expect(parseClock("")).toBeNull()
    expect(parseClock("abc")).toBeNull()
  })

  test("格式化补零", () => {
    expect(formatClock(0)).toBe("00:00")
    expect(formatClock(545)).toBe("09:05")
    expect(formatClock(1440)).toBe("00:00")
  })
})

describe("跨零点与全天", () => {
  test("start > end 才是跨零点", () => {
    expect(isCrossMidnight({ start: "22:00", end: "06:00" })).toBe(true)
    expect(isCrossMidnight({ start: "09:00", end: "18:00" })).toBe(false)
    expect(isCrossMidnight({ start: "00:00", end: "00:00" })).toBe(false) // 全天不算跨零点
  })

  test("start === end 视为全天（与服务端一致：填错也不至于永不执行）", () => {
    expect(isAlwaysOpen({ start: "00:00", end: "00:00" })).toBe(true)
    expect(isAlwaysOpen({ start: "08:00", end: "08:00" })).toBe(true)
    expect(isAlwaysOpen({ start: "08:00", end: "09:00" })).toBe(false)
    expect(isAlwaysOpen(undefined)).toBe(true)
  })
})

describe("人话描述", () => {
  test("不限时段 / 每天跨零点 / 指定星期", () => {
    expect(describeIdleWindow(undefined)).toBe("不限时段（空闲即跑）")
    expect(describeIdleWindow({ start: "00:00", end: "00:00" })).toBe("不限时段（空闲即跑）")
    expect(describeIdleWindow({ start: "22:00", end: "06:00" })).toBe("每天 22:00–06:00（跨零点）")
    // 按给定顺序展示（表单勾选顺序即用户认知顺序）；归一升序是服务端存储侧的事
    expect(describeIdleWindow({ days: [6, 0], start: "09:00", end: "18:00" })).toBe("周六/周日 09:00–18:00")
  })

  test("七个星期全选 = 每天（不显示七个名字）", () => {
    expect(describeIdleWindow({ days: [0, 1, 2, 3, 4, 5, 6], start: "22:00", end: "06:00" })).toBe("每天 22:00–06:00（跨零点）")
  })
})

describe("表单 ↔ IdleWindow 双向映射", () => {
  test("未设时段：表单关闭但预填了常用夜间值（用户一开就能用）", () => {
    const f = formFromIdleWindow(undefined)
    expect(f.enabled).toBe(false)
    expect(f.start).toBe("22:00")
    expect(f.end).toBe("06:00")
    expect(f.days).toEqual([])
  })

  test("已设时段：完整回显", () => {
    const f = formFromIdleWindow({ days: [1, 3], start: "23:00", end: "05:30", timezone: "Asia/Shanghai" })
    expect(f).toEqual({ enabled: true, start: "23:00", end: "05:30", days: [1, 3], timezone: "Asia/Shanghai" })
  })

  test("关闭开关 = 清除时段（value 为 null）", () => {
    const f = { ...emptyIdleWindowForm(), enabled: false, start: "22:00", end: "06:00" }
    expect(idleWindowFromForm(f)).toEqual({ value: null })
  })

  test("非法时刻给出可读错误（而不是静默提交坏值）", () => {
    const f = { ...emptyIdleWindowForm(), enabled: true, start: "25:00", end: "06:00" }
    const r = idleWindowFromForm(f)
    expect(r.value).toBeNull()
    expect(r.error).toContain("开始时刻非法")
  })

  test("空星期 = 每天：不写 days（避免两套等价写法并存）", () => {
    const r = idleWindowFromForm({ enabled: true, start: "22:00", end: "06:00", days: [], timezone: "" })
    expect(r.value).toEqual({ start: "22:00", end: "06:00" })
  })

  test("七天全选也归一为每天", () => {
    const r = idleWindowFromForm({ enabled: true, start: "22:00", end: "06:00", days: [0, 1, 2, 3, 4, 5, 6], timezone: "" })
    expect(r.value).toEqual({ start: "22:00", end: "06:00" })
  })

  test("星期去重升序；时区空不落字段", () => {
    const r = idleWindowFromForm({ enabled: true, start: "9:00", end: "18:00", days: [5, 1, 1], timezone: "  " })
    expect(r.value).toEqual({ days: [1, 5], start: "09:00", end: "18:00" })
  })

  test("往返一致：IdleWindow → 表单 → IdleWindow", () => {
    const w = { days: [1, 2], start: "22:30", end: "06:15", timezone: "Asia/Shanghai" }
    expect(idleWindowFromForm(formFromIdleWindow(w)).value).toEqual(w)
  })
})

describe("快捷预设", () => {
  test("预设都能被解析成合法时段", () => {
    for (const p of IDLE_PRESETS) {
      expect(parseClock(p.window.start)).not.toBeNull()
      expect(parseClock(p.window.end)).not.toBeNull()
    }
  })

  test("命中预设回显，未命中返回 null", () => {
    expect(matchPreset({ start: "22:00", end: "06:00" })?.key).toBe("night")
    expect(matchPreset({ start: "03:00", end: "04:00" })).toBeNull()
    expect(matchPreset(undefined)).toBeNull()
  })
})
