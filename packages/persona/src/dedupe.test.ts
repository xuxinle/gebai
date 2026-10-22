/**
 * 幂等环用例：每个驱动源都会重投，重复事件 = 重复副作用（重复发消息/重复提交）。
 */
import { describe, expect, test } from "bun:test"
import { EventDedupe } from "./dedupe"

describe("EventDedupe", () => {
  test("首次登记返回 true，重复返回 false", () => {
    const d = new EventDedupe(8)
    expect(d.admit("om_1")).toBe(true)
    expect(d.admit("om_1")).toBe(false)
    expect(d.admit("om_1")).toBe(false)
    expect(d.admit("om_2")).toBe(true)
    expect(d.size()).toBe(2)
  })

  test("容量满时淘汰最旧；重复命中会刷新位置（热 id 不被冷事件冲掉）", () => {
    const d = new EventDedupe(3)
    d.admit("a")
    d.admit("b")
    d.admit("c")
    // 刷新 a（命中 → 移到最新）
    expect(d.admit("a")).toBe(false)
    // 插入 d → 淘汰最旧的 b（a 已被刷新）
    d.admit("d")
    expect(d.size()).toBe(3)
    expect(d.admit("a")).toBe(false) // a 仍在
    expect(d.admit("b")).toBe(true) // b 已被淘汰 → 视为新事件（语义正确：超出窗口即重放）
  })

  test("空 id 直接抛错（幂等键必填，不能静默放过）", () => {
    const d = new EventDedupe(4)
    expect(() => d.admit("")).toThrow()
  })

  test("非法容量抛错", () => {
    expect(() => new EventDedupe(0)).toThrow()
    expect(() => new EventDedupe(-1)).toThrow()
    expect(() => new EventDedupe(1.5)).toThrow()
  })

  test("clear 后同一 id 可再次登记（人格重载场景）", () => {
    const d = new EventDedupe(4)
    d.admit("x")
    d.clear()
    expect(d.admit("x")).toBe(true)
  })
})
