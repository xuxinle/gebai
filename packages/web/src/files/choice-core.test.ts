/**
 * 选项对话框的纯逻辑（`files/choice-core.ts`）。
 *
 * 回归点：默认选中项决定"回车一下会执行什么"，危险项判定决定"不可逆操作会不会被二次确认拦住"。
 * 两者写错都不会报错，只会在用户按下回车的那一刻才暴露——所以在这里钉死。
 */
import { describe, expect, test } from "bun:test"
import { initialChoiceIndex, isDangerChoice, type ChoiceLike } from "./choice-core"

describe("initialChoiceIndex（默认选中项）", () => {
  test("有 selected 标记时选它（不一定是第一项）", () => {
    const opts: ChoiceLike[] = [{ value: "soft" }, { value: "mixed", selected: true }, { value: "hard" }]
    expect(initialChoiceIndex(opts)).toBe(1)
  })

  test("没有标记时选第一项", () => {
    expect(initialChoiceIndex([{ value: "a" }, { value: "b" }])).toBe(0)
  })

  test("多个标记时取第一个（不静默跳到后面的）", () => {
    const opts: ChoiceLike[] = [{ value: "a", selected: true }, { value: "b", selected: true }]
    expect(initialChoiceIndex(opts)).toBe(0)
  })

  test("空清单回 0（调用方据 value 判空后返回，不会越界取到 undefined 当默认值）", () => {
    expect(initialChoiceIndex([])).toBe(0)
  })
})

describe("isDangerChoice（危险项判定）", () => {
  const opts: ChoiceLike[] = [{ value: "mixed", selected: true }, { value: "hard", danger: true }]

  test("危险项被认出（hard reset 这类要二次确认的）", () => {
    expect(isDangerChoice(opts, 1)).toBe(true)
  })

  test("普通项与危险项相邻时不能串位", () => {
    expect(isDangerChoice(opts, 0)).toBe(false)
  })

  test("越界按非危险（没有可执行动作时不弹二次确认）", () => {
    expect(isDangerChoice(opts, 5)).toBe(false)
    expect(isDangerChoice([], 0)).toBe(false)
  })
})
