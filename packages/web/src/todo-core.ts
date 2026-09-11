/** 待办弹窗纯逻辑（零 DOM 依赖，独立单测）：拖动排序重排、拖动落点换算、浮层位置钳制与持久化值解析。
 *  与渲染/DOM 解耦的理由同 press-gesture.ts：交互状态机可在无 jsdom 的环境下直接断言。 */

/** 浮层拖动位置钳制时，左右至少保留在视口内的可见宽度（像素）。 */
export const POP_KEEP_X = 48
/** 浮层顶部至少保留的可见高度（标题栏可抓取，保证拖出视口后还能拖回来）。 */
export const POP_KEEP_Y = 32

/** 拖动排序：把 from 位置的条目移动到 to 位置（越界自动钳制；无变化时返回原数组引用）。 */
export function moveItem<T>(list: T[], from: number, to: number): T[] {
  const n = list.length
  if (n < 2) return list
  const f = Math.max(0, Math.min(n - 1, from))
  const t = Math.max(0, Math.min(n - 1, to))
  if (f === t) return list
  const next = [...list]
  const [item] = next.splice(f, 1)
  next.splice(t, 0, item)
  return next
}

/**
 * 拖动落点换算：把 from 行拖到 over 行时，条目在数组中的最终索引。
 * before = 指针位于 over 行上半区（插到其前），否则插到其后；下移场景需扣掉自身占位。
 */
export function dropTargetIndex(from: number, over: number, before: boolean): number {
  let to = before ? over : over + 1
  if (from < to) to -= 1
  return to
}

export interface PopPos {
  x: number
  y: number
}

/** 浮层位置钳制：顶部保留 KEEP_Y、左右各保留 KEEP_X 可见像素，避免拖出视口后无法抓回。 */
export function clampPos(x: number, y: number, w: number, h: number, vw: number, vh: number): PopPos {
  const keepX = Math.min(POP_KEEP_X, Math.max(8, w))
  const keepY = Math.min(POP_KEEP_Y, Math.max(8, h))
  const maxX = Math.max(0, vw - keepX)
  const maxY = Math.max(0, vh - keepY)
  return {
    x: Math.round(Math.max(keepX - w, Math.min(maxX, x))),
    y: Math.round(Math.max(0, Math.min(maxY, y))),
  }
}

/** 解析持久化的浮层位置（脏数据/缺字段返回 null，由调用方回退默认位置）。 */
export function parsePos(raw: string | null | undefined): PopPos | null {
  if (!raw) return null
  try {
    const v = JSON.parse(raw) as { x?: unknown; y?: unknown }
    if (typeof v?.x !== "number" || typeof v?.y !== "number") return null
    if (!Number.isFinite(v.x) || !Number.isFinite(v.y)) return null
    return { x: v.x, y: v.y }
  } catch {
    return null
  }
}

/** 默认位置：标题栏下方、视口右侧（贴近轮盘入口），钳制前由调用方补 clamp。 */
export function defaultPos(w: number, vw: number): PopPos {
  return { x: vw - w - 16, y: 64 }
}
