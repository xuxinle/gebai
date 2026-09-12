/**
 * 分屏的**纯逻辑**（零 DOM、零副作用）：停靠侧解析、宽度夹取、拖分界时的宽度换算。
 *
 * 为什么单独一个文件：这三件事都是"算错了一眼看不出来"的量——换到左侧后拖分界拖反、
 * 夹取漏掉一边、持久化的脏值（老版本写入的其它字符串）导致布局诡异。它们藏在 DOM 事件里时
 * 只能靠肉眼看，抽成纯函数就能直接测（见 files-split-core.test.ts）。
 */

/** 分屏停靠侧：文件工作区停在窗口的哪一边（另一边给会话区）。 */
export type SplitSide = "left" | "right"

/** 缺省停靠侧：文件工作区在**左**（会话区在右）。 */
export const SPLIT_DEFAULT_SIDE: SplitSide = "left"

/** 分屏面板最小宽度（窄于此 ID/编辑器就没意义，此时不如新标签打开）。 */
export const SPLIT_MIN_PANEL = 360
/** 会话区最小宽度（再窄就没法看消息了）。 */
export const SPLIT_MIN_MAIN = 420
/** 低于此窗口宽度不提供分屏（左右都挤成条），改为新标签打开。 */
export const SPLIT_MIN_WINDOW = 1100

/** 归一化停靠侧：只认 "left"/"right"，其余（含 localStorage 里的脏值）一律落回缺省。 */
export function normalizeSplitSide(raw: unknown): SplitSide {
  return raw === "left" || raw === "right" ? raw : SPLIT_DEFAULT_SIDE
}

/** 把宽度夹进「面板不小于 SPLIT_MIN_PANEL、会话区不小于 SPLIT_MIN_MAIN」区间（窗口过窄时以面板下限为准）。 */
export function clampSplitWidth(w: number, windowWidth: number): number {
  const max = Math.max(SPLIT_MIN_PANEL, windowWidth - SPLIT_MIN_MAIN)
  return Math.round(Math.min(Math.max(w, SPLIT_MIN_PANEL), max))
}

/**
 * 拖分界时由指针位置算宽度。
 * 面板贴着窗口的哪一侧，就用「指针到那一侧边缘」的距离当宽度：
 * 右停靠 → 窗口宽 - 指针 x；左停靠 → 指针 x - 面板左缘（不是恒等于 x：窗口可能有偏移）。
 */
export function splitWidthFromPointer(clientX: number, panelLeft: number, windowWidth: number, side: SplitSide): number {
  return side === "right" ? windowWidth - clientX : clientX - panelLeft
}
