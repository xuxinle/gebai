/**
 * 按下-阈值-拖动/点击手势状态机（msg-nav 短横线搓动用，零 DOM 依赖可独立单测）：
 * 按下只记录起点，位移超过 slop 才进入拖动（明确手势——扩展命中区上的轻触/扫过不构成意图），
 * 拖动中每次移动回调 onDrag；自然松开时若从未进入拖动视为点击回调 onClick。
 * 系统取消（pointercancel / 窗口失焦）只复位状态，不触发点击。
 */

export interface GesturePoint {
  x: number
  y: number
}

export interface PressGestureOptions {
  /** 进入拖动的位移阈值（px），默认 6。 */
  slop?: number
  /** 进入拖动瞬间（按下点）。调用方在此接管指针 capture / 切换拖动视觉。 */
  onDragStart?(start: GesturePoint): void
  /** 拖动中移动（当前点；进入拖动的首 move 也会回调）。 */
  onDrag?(point: GesturePoint): void
  /** 明确点击（按下未拖动即松开，松开点）。 */
  onClick?(point: GesturePoint): void
}

export interface PressGesture {
  /** pointerdown：记录起点并复位拖动态。 */
  down(point: GesturePoint): void
  /** pointermove：拖动中回调 onDrag；未超阈值不动作。 */
  move(point: GesturePoint): void
  /** pointerup（自然松开）：拖动结束或触发点击。 */
  up(point: GesturePoint): void
  /** pointercancel（系统取消）：只复位，不触发点击。 */
  cancel(): void
  /** 当前是否拖动中。 */
  isDragging(): boolean
}

export function createPressGesture(opts: PressGestureOptions = {}): PressGesture {
  const slop = opts.slop ?? 6
  let start: GesturePoint | null = null
  let dragging = false

  return {
    down(p) {
      start = { x: p.x, y: p.y }
      dragging = false
    },
    move(p) {
      if (!start) return
      if (dragging) {
        opts.onDrag?.(p)
        return
      }
      if (Math.hypot(p.x - start.x, p.y - start.y) > slop) {
        dragging = true
        opts.onDragStart?.(start)
        opts.onDrag?.(p)
      }
    },
    up(p) {
      if (!start) return
      const wasClick = !dragging
      start = null
      dragging = false
      if (wasClick) opts.onClick?.(p)
    },
    cancel() {
      start = null
      dragging = false
    },
    isDragging: () => dragging,
  }
}
