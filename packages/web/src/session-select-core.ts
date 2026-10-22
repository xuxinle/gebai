/**
 * 会话列表多选的**纯逻辑**（零 DOM、零副作用）：按当前渲染顺序算 Shift 点击的连续区间、
 * 算一个分类（置顶/今天/昨天/近7天/更早）的选中态。
 *
 * 为什么单独一个文件：区间的几种边界（无锚点 / 锚点已被删或被搜索过滤掉 / 锚点排在点击行之后 /
 * 锚点即点击行）与分类的三态（全未选 / 部分选中 / 全选）都是「算错了一眼看不出来、但一按删除就删掉
 * 一堆会话」的量——藏在事件回调里只能靠肉眼点，抽成纯函数就能直接测（见 session-select-core.test.ts）。
 */

/**
 * Shift 点击的选中区间：返回 `order`（当前渲染顺序的会话行 id）中从 `anchor` 到 `target` 的
 * **连续区段**——含两端、按列表顺序升序（锚点排在点击行之后时也升序，同一区间正反点结果一致）。
 *
 * 回退：锚点缺失，或锚点/点击行不在 `order` 里（锚点会话已被删除、被搜索词过滤、所在分组未渲染）
 * 时只返回点击行本身——此时用户看到的就是一次普通单行选中，而不是一段来源不明的区间。
 */
export function rangeSelection(order: readonly string[], anchor: string | null, target: string): string[] {
  const end = order.indexOf(target)
  const start = anchor === null ? -1 : order.indexOf(anchor)
  if (end < 0 || start < 0) return [target]
  return order.slice(Math.min(start, end), Math.max(start, end) + 1)
}

/** 一组条目的选中态：全未选 / 部分选中 / 全选（分类头勾选框的三态）。 */
export type SelectionState = "none" | "some" | "all"

/**
 * 算一组条目（一个分类下的会话）的选中态。`ids` 为空当全未选——空分类显示成「已全选」会让用户
 * 以为那一组已被勾上，进而误判批量删除的范围。
 */
export function selectionState(ids: readonly string[], selected: ReadonlySet<string>): SelectionState {
  if (ids.length === 0) return "none"
  let hit = 0
  for (const id of ids) if (selected.has(id)) hit++
  return hit === 0 ? "none" : hit === ids.length ? "all" : "some"
}
