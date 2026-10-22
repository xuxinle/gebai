import { describe, expect, test } from "bun:test"
import { rangeSelection, selectionState } from "./session-select-core"

/** 会话列表多选算子的单测（纯函数，不涉 DOM）。 */

const ORDER = ["a", "b", "c", "d", "e"]
const sel = (...ids: string[]) => new Set(ids)

describe("rangeSelection（Shift 点击的连续区间）", () => {
  test("自上而下：含两端", () => {
    expect(rangeSelection(ORDER, "b", "d")).toEqual(["b", "c", "d"])
  })

  test("自下而上：同一区间（升序），正反点结果一致", () => {
    expect(rangeSelection(ORDER, "d", "b")).toEqual(["b", "c", "d"])
  })

  test("锚点即点击行：只回该行", () => {
    expect(rangeSelection(ORDER, "c", "c")).toEqual(["c"])
  })

  test("首尾两端", () => {
    expect(rangeSelection(ORDER, "a", "e")).toEqual(["a", "b", "c", "d", "e"])
    expect(rangeSelection(ORDER, "e", "a")).toEqual(["a", "b", "c", "d", "e"])
  })

  test("无锚点（首次 Shift 点击）：只回点击行", () => {
    expect(rangeSelection(ORDER, null, "c")).toEqual(["c"])
  })

  test("锚点已被删除 / 被搜索过滤 / 所在分组未渲染：只回点击行", () => {
    expect(rangeSelection(ORDER, "gone", "c")).toEqual(["c"])
    // 折叠分组里的行不在渲染顺序里（锚点不在 order）→ 同样退化为单行
    expect(rangeSelection(["a", "b"], "d", "a")).toEqual(["a"])
  })

  test("点击行不在渲染顺序里：仍回该行（调用方据此落选中，不至于静默无反应）", () => {
    expect(rangeSelection(ORDER, "a", "zzz")).toEqual(["zzz"])
  })

  test("空列表：回点击行", () => {
    expect(rangeSelection([], "a", "b")).toEqual(["b"])
  })
})

describe("selectionState（分类的选中态三态）", () => {
  test("全未选", () => {
    expect(selectionState(["a", "b"], sel())).toBe("none")
    expect(selectionState(["a", "b"], sel("c"))).toBe("none")
  })

  test("部分选中（分类头显半选）", () => {
    expect(selectionState(["a", "b", "c"], sel("b"))).toBe("some")
    expect(selectionState(["a", "b", "c"], sel("a", "b"))).toBe("some")
  })

  test("全选", () => {
    expect(selectionState(["a", "b"], sel("a", "b"))).toBe("all")
    // 选中集里还有别的分类的会话不影响本分类的判定
    expect(selectionState(["a", "b"], sel("a", "b", "z"))).toBe("all")
  })

  test("空分类当全未选（不显示成「已全选」，免得误判删除范围）", () => {
    expect(selectionState([], sel())).toBe("none")
    expect(selectionState([], sel("a"))).toBe("none")
  })
})
