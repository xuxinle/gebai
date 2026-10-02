/**
 * 自绘浮层的「随滚动关闭」判定（会话右键菜单等浮层共用；纯逻辑、零 DOM 依赖可独立单测）：
 *
 * 只有**浮层宿主的滚动容器自身滚动**（或页面级滚动）才算「宿主漂移、浮层该收」。
 * 无关容器的程序滚动不得关闭浮层——生成中的消息流为咬住底部每帧写 scrollTop，
 * 滚的是另一个容器，而浮层是 fixed 定位、并不会跟着漂移；按「任意滚动一律关闭」
 * 会让菜单在运行中会话里刚打开就被冲掉（表现为右键闪退）。
 *
 * 与 tooltip 的滚动隐藏同口径（ui.ts bindTooltips）。
 */

/** 判定所需的最小宿主接口（HTMLElement 自然满足；测试可用替身）。 */
export interface ScrollHostNode {
  contains(node: unknown): boolean
}

/**
 * 本次滚动是否应当关闭挂在 `host` 里的浮层。
 *
 * @param target 滚动事件的 target（页面级滚动时是 document，nodeType 9）
 * @param host   浮层宿主所在的滚动容器；null 表示宿主不在任何滚动容器内（此时只有页面级滚动才算）
 */
export function dismissOnScroll(target: unknown, host: ScrollHostNode | null): boolean {
  if (!target || typeof target !== "object") return false
  // 页面级滚动：整个视口位移，fixed 浮层失去参照
  if ((target as { nodeType?: number }).nodeType === 9) return true
  if (!host) return false
  return target === host || host.contains(target)
}
