import { describe, expect, test } from "bun:test"
import { clampPos, defaultPos, dropTargetIndex, moveItem, parsePos, POP_KEEP_X, POP_KEEP_Y } from "./todo-core"

/** 待办弹窗纯逻辑单测（无 DOM 依赖，同 press-gesture.test.ts 的拆法）。 */

describe("moveItem（拖动排序重排）", () => {
  test("向后移动 / 向前移动 / 原地不动", () => {
    expect(moveItem(["a", "b", "c", "d"], 0, 2)).toEqual(["b", "c", "a", "d"])
    expect(moveItem(["a", "b", "c", "d"], 3, 1)).toEqual(["a", "d", "b", "c"])
    const same = ["a", "b"]
    expect(moveItem(same, 1, 1)).toBe(same)
  })

  test("越界索引钳制；单元素/空数组原样返回", () => {
    expect(moveItem(["a", "b", "c"], -5, 99)).toEqual(["b", "c", "a"])
    const one = ["a"]
    expect(moveItem(one, 0, 5)).toBe(one)
    const empty: string[] = []
    expect(moveItem(empty, 0, 1)).toBe(empty)
  })

  test("不修改原数组", () => {
    const list = ["a", "b", "c"]
    moveItem(list, 0, 2)
    expect(list).toEqual(["a", "b", "c"])
  })
})

describe("dropTargetIndex（拖动落点换算）", () => {
  test("上移：插到目标行之前", () => {
    // 第 3 行拖到第 1 行上半区 → 最终索引 0
    expect(dropTargetIndex(2, 0, true)).toBe(0)
    // 第 3 行拖到第 1 行下半区 → 最终索引 1
    expect(dropTargetIndex(2, 0, false)).toBe(1)
  })

  test("下移：插到目标行之后并扣除自身占位", () => {
    // 第 0 行拖到第 2 行下半区 → 最终索引 2
    expect(dropTargetIndex(0, 2, false)).toBe(2)
    // 第 0 行拖到第 2 行上半区 → 最终索引 1
    expect(dropTargetIndex(0, 2, true)).toBe(1)
  })

  test("相邻行上下半区都落在合理位置", () => {
    expect(dropTargetIndex(1, 2, true)).toBe(1) // 与自身原位相同（无变化）
    expect(dropTargetIndex(1, 2, false)).toBe(2)
    expect(dropTargetIndex(2, 1, true)).toBe(1)
    expect(dropTargetIndex(2, 1, false)).toBe(2)
  })

  test("与 moveItem 组合：落点索引可直接搬运", () => {
    const list = ["a", "b", "c", "d"]
    const to = dropTargetIndex(0, 2, false)
    expect(moveItem(list, 0, to)).toEqual(["b", "c", "a", "d"])
  })
})

describe("clampPos（浮层位置钳制）", () => {
  test("常规范围原样返回", () => {
    expect(clampPos(100, 80, 320, 400, 1200, 800)).toEqual({ x: 100, y: 80 })
  })

  test("拖出右侧/底部：保留最小可见像素", () => {
    const p = clampPos(5000, 5000, 320, 400, 1200, 800)
    expect(p.x).toBe(1200 - POP_KEEP_X)
    expect(p.y).toBe(800 - POP_KEEP_Y)
  })

  test("拖出左侧：右侧保留可见宽度（可抓回）", () => {
    const p = clampPos(-5000, -5000, 320, 400, 1200, 800)
    expect(p.x).toBe(POP_KEEP_X - 320)
    expect(p.y).toBe(0)
  })

  test("小窗口下不会产生非法区间", () => {
    const p = clampPos(50, 50, 320, 400, 100, 60)
    expect(Number.isFinite(p.x)).toBe(true)
    expect(p.x).toBeLessThanOrEqual(Math.max(0, 100 - POP_KEEP_X))
    expect(p.y).toBeGreaterThanOrEqual(0)
  })
})

describe("parsePos / defaultPos", () => {
  test("合法持久化值解析；脏数据返回 null", () => {
    expect(parsePos('{"x":12,"y":34}')).toEqual({ x: 12, y: 34 })
    expect(parsePos(null)).toBeNull()
    expect(parsePos("")).toBeNull()
    expect(parsePos("not json")).toBeNull()
    expect(parsePos('{"x":"1","y":2}')).toBeNull()
    expect(parsePos('{"x":null}')).toBeNull()
    expect(parsePos('{"x":1e999,"y":0}')).toBeNull()
  })

  test("默认位置贴视口右侧", () => {
    expect(defaultPos(320, 1200)).toEqual({ x: 1200 - 320 - 16, y: 64 })
  })
})
