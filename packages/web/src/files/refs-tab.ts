/**
 * 「分支」栏内小切换（分支 / 标签 / 储存 / 远程）的**绘制状态**短路判据（纯函数，可单测）。
 *
 * 为什么需要它：这四个视图共用同一个内容容器 `colRefs`，每个渲染函数都带一个「数据指纹没变就跳过
 * 重绘」的守卫（避免每次自动刷新心跳都重建列表、抖掉 hover / 焦点 / 滚动位置）。但这条短路的隐含
 * 前提是「容器里现在画的正是本视图」——**共用容器下这个前提会被切换打破**：从分支切到储存再切回
 * 分支时，分支数据没变（指纹相同），旧守卫据此跳过重绘，于是容器停在储存内容上，表现为「切不回
 * 分支」。远程同理。
 *
 * 判据因此从「指纹相同」收紧为「**容器里画的正是本视图** 且 指纹相同」：只要当前绘制视图不是本视图，
 * 即使数据没变也必须重绘，把容器抢回来。
 */

/** 「分支」栏内的四个视图。 */
export type RefsTab = "branches" | "tags" | "stash" | "remotes"

/**
 * 当前是否应跳过重绘。
 *
 * @param drawnTab 容器里**现在**画的视图（null = 尚未画过，例如首屏 / 刚被「不是仓库」「状态失败」等占位）
 * @param tab 本渲染函数要画的视图
 * @param key 本次数据指纹
 * @param drawnKey 上次为 `drawnTab` 记录的指纹
 * @returns true = 容器已是本视图且数据没变，跳过；false = 需要重绘
 */
export function shouldSkipRepaint(
  drawnTab: RefsTab | null,
  tab: RefsTab,
  key: string,
  drawnKey: string,
): boolean {
  return drawnTab === tab && key === drawnKey
}
