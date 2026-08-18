/**
 * 粘底跟随核心（意图驱动；sticky-scroll 主消息列 / reasoning-scroll 推理体 / messages 新会话容器共用）：
 *
 * 跟随状态（following）只由三类信号翻转，不做滚动事件位置取证（目标位置比对 / 程序落位差值）——
 * 浏览器 scroll 事件异步合并送达，内容增长/收缩引发的钳制与布局调整同样产生滚动事件，
 * 取证式分类必然存在误判窗口：迟到事件被判为用户滚动 → 跟随悄悄失效（「滚动一段后失灵」的根因），
 * 过期的程序落位参与比对 → 内容收缩/会话切换后按位置解除跟随（贴底状态静默死亡）。
 *
 * 1. 用户输入意图（同步，必然先于其滚动效果到达，无竞态）：滚轮上滚 / 触摸上滑 /
 *    向上滚动键 / 滚动条拖动 / 显式导航（stopFollowing）→ 立即解除跟随。
 * 2. 几何贴底：任何滚动事件落在阈值内 → 恢复跟随（滚回底部 / 浏览器收缩钳制到底收敛）。
 * 3. 静默窗口兜底：距底部超阈值、且距最近一次程序滚动 / DOM 变化超过 INTERNAL_QUIET_MS →
 *    无法归因为内部动作，视为未知输入（中键自动滚动、查找定位、覆盖式滚动条拖动）→ 解除跟随；
 *    窗口内的未贴底事件视为程序滚动/钳制的迟到事件（与引发它的赋值/变化同帧或下一帧送达）：
 *    保持跟随并回正到底。
 *
 * 粘底对齐保持（帧预算循环）沿用：内容高度存在不触发 MutationObserver 的异步修正
 * （content-visibility 懒布局、字体/图片加载），跟随期间按帧续查对齐，预算耗尽自停。
 */

/** 程序滚动 / DOM 变化后的静默窗口（毫秒）：窗口内的未贴底滚动事件归因为内部动作。 */
const INTERNAL_QUIET_MS = 80
/** 触摸上滑判定：手指下移超过该值（px）视为向上翻阅。 */
const TOUCH_SLOP = 8
/** 明确向上的滚动键（立即解除跟随；向下键由几何贴底恢复跟随）。 */
const UP_KEYS = new Set(["ArrowUp", "PageUp", "Home"])

export interface StickyFollowOptions {
  /** 距底部阈值（<= 该值视为贴底），默认 64。 */
  threshold?: number
  /** 粘底对齐保持帧预算（异步高度修正兜底），0 = 关闭，默认 0。 */
  keepFrames?: number
  /** 监听 DOM 变化自动跟随（主消息列 true；推理体/新会话容器由调用方显式触发）。 */
  observeMutations?: boolean
  /** 键盘滚动监听目标（如 window）；省略不监听键盘。 */
  keyTarget?: Window
  /** 时钟注入（测试）。 */
  now?: () => number
  /** 跟随状态翻转回调（按钮显隐等）。 */
  onFollowingChange?: (following: boolean) => void
  /** 滚动事件回调（按几何刷新按钮显隐）。 */
  onScroll?: () => void
}

export interface StickyFollowHandle {
  isAtBottom(): boolean
  isFollowing(): boolean
  /** 锁定跟随并落底（发送消息 / 会话加载 / 点击跳到最新）。 */
  follow(): void
  /** 内容变化：跟随中 rAF 节流落底，未跟随不动。 */
  contentChanged(): void
  /** 程序恢复历史滚动位置：落位后按几何同步跟随状态（未决跟随回调按执行时状态自然失效）。 */
  restore(top: number): void
  /** 用户导航（消息导航跳转等）显式解除跟随。 */
  stopFollowing(): void
}

export function createStickyFollow(el: HTMLElement, opts: StickyFollowOptions = {}): StickyFollowHandle {
  const threshold = opts.threshold ?? 64
  const keepBudget = opts.keepFrames ?? 0
  // 不能直接取 performance.now：脱离宿主的 Performance 方法在浏览器抛 Illegal invocation（测试注入覆盖不到该路径）
  const now = opts.now ?? (() => performance.now())

  let following = true
  let lastInternalAt = 0
  let scrollbarDrag = false

  const isAtBottom = () => el.scrollHeight - el.scrollTop - el.clientHeight <= threshold

  function setFollowing(v: boolean) {
    if (following === v) return
    following = v
    opts.onFollowingChange?.(v)
  }

  /** 程序落底：记录时间戳，其后的迟到滚动事件在静默窗口内归因为内部动作。 */
  function pin() {
    el.scrollTop = el.scrollHeight
    lastInternalAt = now()
  }

  let followRaf = false
  function contentChanged() {
    lastInternalAt = now()
    if (!following) return
    if (followRaf) return
    followRaf = true
    requestAnimationFrame(() => {
      followRaf = false
      if (!following) return // 排期期间用户已上翻（输入事件先于 rAF 送达）：不拽回
      pin()
      noteActivity()
    })
  }

  let keepAligning = false
  let keepFrames = 0
  function noteActivity() {
    keepFrames = 0
    if (keepAligning || !keepBudget) return
    keepAligning = true
    requestAnimationFrame(keepTick)
  }
  function keepTick() {
    keepAligning = false
    if (!following) return
    // 几何不可用（NaN：未布局元素/测试替身）无从对齐，停转防死循环
    const distance = el.scrollHeight - el.scrollTop - el.clientHeight
    if (!Number.isFinite(distance)) return
    if (distance > threshold) {
      pin()
      keepFrames = 0
    }
    keepFrames++
    if (keepFrames < keepBudget) {
      keepAligning = true
      requestAnimationFrame(keepTick)
    }
  }

  function follow() {
    setFollowing(true)
    pin()
    noteActivity()
  }

  function restore(top: number) {
    el.scrollTop = top
    lastInternalAt = now() // 恢复赋值的迟到事件归因为内部动作（following 已按落位同步）
    setFollowing(isAtBottom())
  }

  /* ---------- 用户输入意图（先于其滚动效果到达，无竞态） ---------- */

  el.addEventListener(
    "wheel",
    (e) => {
      if ((e as WheelEvent).deltaY < 0 && el.scrollHeight > el.clientHeight) setFollowing(false)
    },
    { passive: true },
  )

  let touchY = 0
  el.addEventListener(
    "touchstart",
    (e) => {
      touchY = (e as TouchEvent).touches[0]?.clientY ?? 0
    },
    { passive: true },
  )
  el.addEventListener(
    "touchmove",
    (e) => {
      if (((e as TouchEvent).touches[0]?.clientY ?? 0) - touchY > TOUCH_SLOP) setFollowing(false)
    },
    { passive: true },
  )

  // 滚动条拖动：pointerdown 命中滚动条槽区（经典滚动条宽 = offsetWidth - clientWidth；
  // 覆盖式滚动条宽 0 不命中，由静默窗口兜底路径解除）。拖动期间未贴底即用户意图，即时解除。
  const endDrag = () => {
    scrollbarDrag = false
  }
  el.addEventListener("pointerdown", (e) => {
    const sbw = el.offsetWidth - el.clientWidth
    if (sbw <= 0 || typeof window === "undefined") return
    const rect = el.getBoundingClientRect()
    if ((e as PointerEvent).clientX > rect.right - sbw - 4) {
      scrollbarDrag = true
      window.addEventListener("pointerup", endDrag, { once: true })
      window.addEventListener("pointercancel", endDrag, { once: true })
    }
  })

  if (opts.keyTarget) {
    opts.keyTarget.addEventListener("keydown", (e) => {
      const ev = e as KeyboardEvent
      if (!ev.key || ev.defaultPrevented || !UP_KEYS.has(ev.key)) return
      // 输入框内的方向键滚动的是文本光标，不是列表
      const t = ev.target as { closest?: (sel: string) => unknown } | null
      if (t && typeof t.closest === "function" && t.closest("input, textarea, [contenteditable='true']")) return
      setFollowing(false)
    })
  }

  /* ---------- 滚动事件：贴底恢复 / 内部归因保持并回正 / 未知输入兜底解除 ---------- */

  el.addEventListener(
    "scroll",
    () => {
      opts.onScroll?.()
      if (isAtBottom()) {
        setFollowing(true) // 滚回底部 / 收缩钳制到底：恢复跟随
        return
      }
      if (scrollbarDrag) {
        setFollowing(false) // 滚动条拖动中未贴底：即时解除（不受静默窗口延迟）
        return
      }
      if (!following) return // 已在阅读历史
      if (now() - lastInternalAt <= INTERNAL_QUIET_MS) {
        // 迟到的程序滚动/钳制事件（期间内容变化使位置离开底部）：保持跟随并回正
        pin()
        noteActivity()
        return
      }
      setFollowing(false) // 未知输入（中键自动滚动 / 查找定位 / 覆盖式滚动条）
    },
    { passive: true },
  )

  if (opts.observeMutations) {
    new MutationObserver(() => contentChanged()).observe(el, { childList: true, subtree: true, characterData: true })
  }
  if (typeof ResizeObserver !== "undefined") {
    new ResizeObserver(() => contentChanged()).observe(el)
  }

  return {
    isAtBottom,
    isFollowing: () => following,
    follow,
    contentChanged,
    restore,
    stopFollowing: () => setFollowing(false),
  }
}
