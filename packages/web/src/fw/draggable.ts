/**
 * 微框架 · 可拖浮层（通用件）：把「一个能拖、能记住位置、能钳回视口」的浮层收成一份实现。
 *
 * ## 为什么抽它
 * 待办弹窗（`todo-pop.ts`）与人设弹窗（`persona-pop.ts`）各自实现了同一套东西：
 * pointerdown 起手 → setPointerCapture → 位移钳制 → pointerup 落库 localStorage →
 * 窗口 resize 时钳回视口 → 「点按钮不启动拖动」。两份实现有 35+ 行逐字相同，
 * 且已经出现过「一边修了钳制边界、另一边没修」的分歧（窄屏下浮层被拖出屏幕抓不回来）。
 *
 * ## 设计取舍
 * - **不做通用弹层系统**（不接管显隐/遮罩/焦点）：这些面板的显隐语义各不相同
 *   （待办弹窗只有 ✕ 能关、设置抽屉点遮罩关、任务面板 Esc 关）。这里只负责"拖动与位置"，
 *   显隐仍由各面板自己控制——少给一层抽象，就少一处"框架猜错了你的意图"。
 * - 位置持久化**默认走 localStorage**：这是纯 UI 偏好（与设备/窗口尺寸相关），
 *   不属于"服务端为准"的数据（对比：闲时时段那种影响服务端行为的东西才进 prefs）。
 * - 钳制逻辑复用 `todo-core.ts` 的 `clampPos`（已有单测），不另写一份。
 */

import { clampPos, defaultPos, parsePos, type PopPos } from "../todo-core"
import { on } from "./dom"

/** 拖动把手选择器（默认标题栏）。 */
export interface DraggableOptions {
  /** 浮层根节点（位置用内联 left/top 控制）。 */
  root: HTMLElement
  /** 拖动把手（默认 `.popover-head`，各面板用同一类名即自动生效）。 */
  handle?: string
  /** localStorage 键（缺省不持久化位置）。 */
  storageKey?: string
  /** 首次显示时的默认位置（缺省靠右，标题栏下方）。 */
  initial?: (w: number) => PopPos
  /** 拖动开始时排除的元素（默认按钮：点按钮不该变成拖一下）。 */
  exclude?: string
}

export interface Draggable {
  /** 是否正在拖动（用于样式加 `dragging` 类）。 */
  isDragging: () => boolean
  /** 显示浮层：读出记忆位置 → 钳制 → 应用（首次显示时使浮层可见）。 */
  show: () => void
  /** 窗口尺寸变化时调用（钳回视口）。 */
  reclamp: () => void
  /** 解除全部监听（组件销毁时必须调用）。 */
  dispose: () => void
}

const DEFAULT_HANDLE = ".popover-head"
const DEFAULT_EXCLUDE = "button, a, input, textarea, select"

/** 本地存储读写（不可用时静默降级，位置只是"锦上添花"）。 */
function readPos(key: string): PopPos | null {
  try {
    return parsePos(localStorage.getItem(key))
  } catch {
    return null
  }
}

function writePos(key: string, pos: PopPos): void {
  try {
    localStorage.setItem(key, JSON.stringify(pos))
  } catch {
    /* 存储不可用：忽略（下次打开用默认位置，不影响功能） */
  }
}

/**
 * 让一个浮层可拖动。
 *
 * 用法：面板构建完 root 后调用一次；`show()` 用于打开时（读位置并钳制）；销毁时 `dispose()`。
 */
export function makeDraggable(opts: DraggableOptions): Draggable {
  const { root } = opts
  const handleSel = opts.handle ?? DEFAULT_HANDLE
  const excludeSel = opts.exclude ?? DEFAULT_EXCLUDE
  let drag: { dx: number; dy: number; pid: number } | null = null
  const offs: Array<() => void> = []

  /** 当前尺寸（未布局时为 0：用兜底值，避免钳制算出 0 宽而把浮层塞到左上角）。 */
  const rect = (): { w: number; h: number } => {
    const r = root.getBoundingClientRect()
    return { w: r.width || 360, h: r.height || 320 }
  }

  const clampNow = (pos: PopPos | null): PopPos => {
    const { w, h } = rect()
    const fallback = opts.initial ? opts.initial(window.innerWidth) : defaultPos(w, window.innerWidth)
    const base = pos ?? fallback
    return clampPos(base.x, base.y, w, h, window.innerWidth, window.innerHeight)
  }

  const applyPos = (pos: PopPos): void => {
    root.style.left = `${pos.x}px`
    root.style.top = `${pos.y}px`
  }

  const handle = root.querySelector<HTMLElement>(handleSel)
  if (handle) {
    offs.push(
      on(handle, "pointerdown", (e) => {
        const pe = e as PointerEvent
        if (pe.button !== 0) return
        if ((pe.target as HTMLElement).closest(excludeSel)) return
        const r = root.getBoundingClientRect()
        drag = { dx: pe.clientX - r.left, dy: pe.clientY - r.top, pid: pe.pointerId }
        root.classList.add("dragging")
        handle.setPointerCapture(pe.pointerId)
      }),
    )
    offs.push(
      on(handle, "pointermove", (e) => {
        const pe = e as PointerEvent
        if (!drag || drag.pid !== pe.pointerId) return
        applyPos(clampNow({ x: pe.clientX - drag.dx, y: pe.clientY - drag.dy }))
      }),
    )
    const end = (e: Event): void => {
      const pe = e as PointerEvent
      if (!drag || drag.pid !== pe.pointerId) return
      drag = null
      root.classList.remove("dragging")
      try {
        handle.releasePointerCapture(pe.pointerId)
      } catch {
        /* 已释放 */
      }
      if (opts.storageKey) {
        const r = root.getBoundingClientRect()
        writePos(opts.storageKey, { x: Math.round(r.left), y: Math.round(r.top) })
      }
    }
    offs.push(on(handle, "pointerup", end))
    offs.push(on(handle, "pointercancel", end))
    // 丢失捕获（如指针被系统抢走）兜底：不清 drag 状态的话，下一次 pointermove 会从旧位移继续跳
    offs.push(
      on(handle, "lostpointercapture", () => {
        drag = null
        root.classList.remove("dragging")
      }),
    )
  }

  return {
    isDragging: () => drag !== null,
    show: () => {
      const pos = clampNow(opts.storageKey ? readPos(opts.storageKey) : null)
      applyPos(pos)
      root.style.visibility = "visible"
    },
    reclamp: () => {
      const r = root.getBoundingClientRect()
      if (!r.width) return
      applyPos(clampPos(r.left, r.top, r.width, r.height, window.innerWidth, window.innerHeight))
    },
    dispose: () => {
      for (const off of offs.splice(0)) off()
      drag = null
    },
  }
}
