/**
 * 选项对话框（`files/ui.ts` 的 `choiceDialog`）里**与 DOM 无关的那部分**：初始选中项与危险项判定。
 *
 * 为什么抽出来：web 包无 jsdom，对话框本身起不来；而"默认选中谁""当前选中的是不是危险项"
 * 恰好是最容易写错、后果最直接的两件事——默认项错了会让最常见的路径变成点一下就执行别的动作，
 * 危险项判定错了会让不可逆操作绕过二次确认。两者都是纯计算，直接单测钉住。
 */

/** 选项：`value` 回传给调用方，`danger` 决定要不要二次确认。 */
export interface ChoiceLike {
  value: string
  danger?: boolean
  selected?: boolean
}

/**
 * 初始选中项的下标。
 *
 * 规则：取**第一个** `selected` 的项；没有任何标记时选第 0 项。
 * 为什么不取"最后一个"或"按危险项避开"：默认项是调用方表达的业务默认值（如 mixed），
 * 这里只负责忠实执行；替调用方躲危险项会让"我明明标了 selected"变成没生效。
 */
export function initialChoiceIndex(options: ChoiceLike[]): number {
  if (!options.length) return 0
  const i = options.findIndex((o) => o.selected)
  return i >= 0 ? i : 0
}

/** 当前下标是否为危险项（越界按非危险处理：越界时没有可执行的动作）。 */
export function isDangerChoice(options: ChoiceLike[], index: number): boolean {
  return !!options[index]?.danger
}
