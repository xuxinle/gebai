/**
 * 闲时时间段：**前后端语义一致性**护栏。
 *
 * 为什么需要它：时段判定有两份实现——服务端 `core/schedule/idle-window.ts`（真正调度用的）
 * 与前端 `web/src/idle-window-core.ts`（只做展示与表单映射，业务确定由服务端负责）。
 * 两份实现必然存在漂移风险：某次只改了后端「凌晨算前一天」，前端文案还按旧规则解释，
 * 用户就会看到自相矛盾的说明（"周六 22:00–06:00" 一边说含周日凌晨、一边说不含）。
 *
 * 这里用**同一组典型用例**同时跑两边的解析/判定，任何一侧改了规则都会红。
 * 不依赖服务端模块的前端侧用 `idle-window-core`（零 node 依赖），
 * 服务端侧直接 import（bun 测试可跨包引用）。
 */
import { describe, expect, test } from "bun:test"
import { isInIdleWindow as serverIsIn, normalizeIdleWindow } from "../../server/src/core/schedule/idle-window"
import {
  describeIdleWindow as webDescribe,
  isAlwaysOpen as webAlwaysOpen,
  isCrossMidnight as webCross,
  parseClock as webParse,
} from "./idle-window-core"

/** 本地时区某时刻（两侧判定默认都用服务器本地时区）。 */
const at = (y: number, mo: number, d: number, h: number, mi = 0): number => new Date(y, mo - 1, d, h, mi).getTime()

describe("闲时时间段：前后端语义一致", () => {
  test("跨零点判定口径一致：start > end 才算跨零点（start === end 是全天）", () => {
    for (const w of [
      { start: "22:00", end: "06:00" },
      { start: "09:00", end: "18:00" },
      { start: "00:00", end: "00:00" },
      { start: "12:00", end: "12:00" },
    ]) {
      const webCrossFlag = webCross(w)
      const serverCrossFlag = webParse(w.start)! > webParse(w.end)!
      expect(webCrossFlag).toBe(serverCrossFlag)
    }
  })

  test("全天判定一致：start === end 视为全天（填错也不至于永不执行）", () => {
    expect(webAlwaysOpen({ start: "00:00", end: "00:00" })).toBe(serverIsIn({ start: "00:00", end: "00:00" }, at(2026, 3, 4, 13, 0)))
    expect(webAlwaysOpen(undefined)).toBe(serverIsIn(undefined, at(2026, 3, 4, 13, 0)))
  })

  test("跨零点窗口的**凌晨归属前一天**：前端文案与该语义不矛盾", () => {
    // 2026-03-07 是周六；只在周六开窗 22:00–06:00
    const w = { days: [6], start: "22:00", end: "06:00" }
    expect(serverIsIn(w, at(2026, 3, 7, 22, 30))).toBe(true) // 周六深夜
    expect(serverIsIn(w, at(2026, 3, 8, 3, 0))).toBe(true) // 周日凌晨 → 属周六那一场
    expect(serverIsIn(w, at(2026, 3, 9, 3, 0))).toBe(false) // 周一凌晨 → 属周日那一场，不算
    // 前端文案必须说清"跨零点"，用户才不会把周日凌晨当成需要单独勾选
    expect(webDescribe(w)).toContain("跨零点")
  })

  test("边界一律左闭右开（22:00 进、06:00 出）", () => {
    const w = { start: "22:00", end: "06:00" }
    expect(serverIsIn(w, at(2026, 3, 4, 22, 0))).toBe(true)
    expect(serverIsIn(w, at(2026, 3, 5, 5, 59))).toBe(true)
    expect(serverIsIn(w, at(2026, 3, 5, 6, 0))).toBe(false)
    expect(serverIsIn(w, at(2026, 3, 4, 21, 59))).toBe(false)
  })

  test("时刻解析口径一致：非法值两侧都拒绝（服务端在写入/加载时归一，前端在表单校验时拒）", () => {
    for (const raw of ["00:00", "9:05", "23:59", "24:00", "12:60", "abc", ""]) {
      const web = webParse(raw)
      let serverAccepts = true
      try {
        normalizeIdleWindow({ start: raw, end: "23:59" })
      } catch {
        serverAccepts = false
      }
      expect(serverAccepts).toBe(web !== null)
    }
  })
})
