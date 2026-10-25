/**
 * 粘底跟随（sticky-scroll 主消息列 / reasoning-scroll 推理体 / messages 新会话容器共用）：
 *
 * 跟随状态由**意图识别核**（scroll-intent.ts）判定——只看用户真实造成的位移量，不看事件来源，
 * 也不做滚动事件的位置取证（浏览器 scroll 事件异步合并送达，内容增长/收缩引发的钳制与布局调整
 * 同样产生滚动事件，取证式分类必然存在误判窗口：迟到事件被判为用户滚动 → 跟随悄悄失效；
 * 过期的程序落位参与比对 → 内容收缩/会话切换后按位置解除跟随）。三态（follow/hold/reading）
 * 与判定规则见 scroll-intent.ts 头注释，本模块只负责 DOM 侧职责：
 *
 * - 程序落底（pin）记录时间戳：其后的迟到滚动事件在静默窗口内归因为内部动作（不参与意图判定）。
 * - 显式输入即时表态（不等位移判定）：向上滚动键、中键自动滚动、滚动条拖动；拖动/自动滚动
 *   结束按几何归位（此时不再有 scroll 事件）。
 * - 粘底对齐保持（帧预算循环）：内容高度存在不触发 MutationObserver 的异步修正（图片/图表/字体
 *   异步加载、容器展开折叠后的延迟布局），跟随期间按帧续查对齐，预算耗尽自停。
 */

import { createScrollIntent, type FollowState, type ScrollIntentOptions } from "./scroll-intent"

/** 程序滚动 / DOM 变化后的静默窗口（毫秒）：窗口内的未贴底滚动事件归因为内部动作。 */
const INTERNAL_QUIET_MS = 80
/** 用户上翻输入后的宽限期（毫秒）：期间不把视口拽回底部——滚动是合成器异步派发的，
 *  输入事件与位置变化之间内容增长的 rAF 可能先行，靠位移判定会慢一帧。 */
const UP_INTENT_GRACE_MS = 120
/** 触摸上滑判定：手指下移超过该值（px）视为向上翻阅。 */
const TOUCH_SLOP = 8
/** 明确向上的滚动键（立即解除跟随；向下键不解除，由「向下滚到底」恢复）。 */
const UP_KEYS = new Set(["ArrowUp", "PageUp", "Home"])
/** 无高度变化的连续帧数上限：内容高度连续这么多帧没变就认为已稳定，停止续查。
 *  对齐保持预算（keepFrames）本为「异步长高后继续滚回底部」准备，而每帧都要读
 *  scrollHeight/clientHeight——布局 dirty 时这些读取就是强制重排。无条件跑满预算会在
 *  切换会话 / 点击发送后占着主线程几秒，后续滚动与点击抢不到帧（体感：不跟手）。
 *  真正长高时 ResizeObserver / MutationObserver 会再次触发 contentChanged 重开循环，
 *  提前停不会漏掉图片/图表的异步修正。 */
const KEEP_IDLE_FRAMES = 4

export type { FollowState }

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
  /** 意图识别核的参数覆盖（测试注入可控时钟/定时器；生产用默认值）。 */
  intentOptions?: Partial<ScrollIntentOptions>
  /** 是否处于跟随的翻转回调（按钮显隐等）。 */
  onFollowingChange?: (following: boolean) => void
  /** 三态变化的回调（按钮显隐/提示按此刷新）。 */
  onStateChange?: (state: FollowState) => void
  /** 滚动事件回调（按几何刷新按钮显隐）。 */
  onScroll?: () => void
}

export interface StickyFollowHandle {
  isAtBottom(): boolean
  /** 追最新（窗口化据此判定 DOM 变更后是否保持贴底）。 */
  isFollowing(): boolean
  /** 程序回正的落点（距底 = 用户在跟随态内留下的位移）：窗口化直接用它写 scrollTop，
   *  不得各自写绝对底部——那会把用户的小步微调抹掉（滚不出去的回归）。
   *  不在追最新时返回 null（调用方不得回正）。 */
  pinTarget(): number | null
  /** 三态（follow / hold / reading）。 */
  state(): FollowState
  /** 是否显示「回到最新」按钮：不在追最新，或用户已把视口拉开（上翻位移）但尚未确认。 */
  shouldShowJump(): boolean
  /** 锁定跟随并落底（发送消息 / 会话加载 / 点击跳到最新）。 */
  follow(): void
  /** 内容变化：跟随中 rAF 节流落底，未跟随不动。 */
  contentChanged(): void
  /** 程序恢复历史滚动位置：落位后按几何同步状态。 */
  restore(top: number): void
  /** 用户导航（消息导航跳转等）显式解除跟随。 */
  stopFollowing(): void
}

export function createStickyFollow(el: HTMLElement, opts: StickyFollowOptions = {}): StickyFollowHandle {
  const threshold = opts.threshold ?? 64
  const keepBudget = opts.keepFrames ?? 0
  // 不能直接取 performance.now：脱离宿主的 Performance 方法在浏览器抛 Illegal invocation（测试注入覆盖不到该路径）
  const now = opts.now ?? (() => performance.now())

  let lastInternalAt = 0
  /** 最近一次「用户上翻」输入的时刻（滚轮上滑 / 触摸上滑）：宽限期内不把视口拽回底部。 */
  let upIntentAt = -Infinity
  /** 跟随态内用户留下的距底位移（px）：内容增长按它保留偏移，不写死到底部。 */
  let followOffset = 0
  /** 滚动条拖动进行中：期间位移不参与「向下滚到底恢复」（也不回正）。 */
  let gestureHold = false

  const isAtBottom = () => el.scrollHeight - el.scrollTop - el.clientHeight <= threshold

  let lastState: FollowState = "follow"
  const intent = createScrollIntent({ threshold, now, ...opts.intentOptions })

  function setState(next: FollowState) {
    if (lastState === next) return
    const wasFollowing = lastState === "follow"
    lastState = next
    if (wasFollowing !== (next === "follow")) opts.onFollowingChange?.(next === "follow")
    opts.onStateChange?.(next)
  }

  /** 跟随时同步用户的距底位移（进入跟随态的位置就是他要待的地方）：内容增长按它保留偏移。 */
  function syncFollowOffset() {
    followOffset = Math.max(0, el.scrollHeight - el.clientHeight - el.scrollTop)
  }

  /** 程序落底：记录时间戳（其后的迟到滚动事件在静默窗口内归因为内部动作）+ 重设基准。
   *  落点 = 底部**减去用户在跟随态内留下的距底位移**（`followOffset`）：用户的微调上翻是意图，
   *  内容增长（图表/图片异步长高）不得把它抹掉——否则每次长高都把视口弹回绝对底部，
   *  小步上翻永远累计不到容忍区之外（体感：来回卡几下才滚得出去）。
   *  未上翻时 `followOffset` 为 0，落点即绝对底部（与内容增长一起走）。 */
  function pin() {
    const max = el.scrollHeight - el.clientHeight
    el.scrollTop = Math.max(0, max - followOffset)
    lastInternalAt = now()
    intent.rebase(el.scrollTop)
  }

  /** 是否允许程序回正（把视口拉到底）：追最新中、且用户没有刚把视口拉开。
   *  - 已在底部：一律允许（回正无害）——容忍区内上翻也在此列，此时靠 `followOffset` 保住
   *    用户的距底位移（硬拉到底会把每一次微调都抹掉，小步上翻永远滚不出去）；
   *  - 刚发生上翻输入：不抢位置（内容增长的 rAF 可能先于用户的 scroll 事件）；
   *  - 其余：允许回正。
   *  注意：内容增长会让位置相对底部变远，故不能只看「是否还在阈值内」——用户的
   *  向上位移才是「已把视口拉开」的凭据；且该抑制必须随手势窗结束而失效，否则
   *  一次微调会让后续内容增长永久不再贴底（状态仍是追最新，名不副实）。 */
  function canPin(): boolean {
    if (intent.state() !== "follow") return false
    if (isAtBottom()) return true
    if (intent.hasUpIntent() && intent.movingRecently()) return false
    return now() - upIntentAt > UP_INTENT_GRACE_MS
  }

  let followRaf = false
  function contentChanged() {
    // 仅跟随中刷新静默窗口：未跟随时持续的内容变更（流式每 120ms 重解析）会让窗口永不关闭，
    // 非滚轮类滚动输入（中键自动滚动/查找定位/覆盖式滚动条）无法归因、每滚一下都被回正拽底——「滚动卡死」
    if (!canPin()) return
    lastInternalAt = now()
    if (followRaf) return
    followRaf = true
    requestAnimationFrame(() => {
      followRaf = false
      if (!canPin()) return // 排期期间用户已上翻（输入事件先于 rAF 送达）：不拽回
      pin()
      noteActivity()
    })
  }

  let keepAligning = false
  let keepFrames = 0
  /** 上一帧的内容高度：连续 KEEP_IDLE_FRAMES 帧不变即判稳定停转（防空转占主线程）。 */
  let lastKeepHeight = -1
  let idleFrames = 0
  function noteActivity() {
    keepFrames = 0
    if (keepAligning || !keepBudget) return
    keepAligning = true
    requestAnimationFrame(keepTick)
  }
  function keepTick() {
    keepAligning = false
    if (!canPin()) return
    // 几何不可用（NaN：未布局元素/测试替身）无从对齐，停转防死循环
    const height = el.scrollHeight
    const distance = height - el.scrollTop - el.clientHeight
    if (!Number.isFinite(distance)) return
    // 距底超过容忍区且超出用户自己留下的位移 → 异步高度修正把它推远了，按偏移续滚回位
    if (distance > threshold && distance > followOffset + 1) {
      pin()
      keepFrames = 0
      idleFrames = 0
    } else if (height === lastKeepHeight && distance <= threshold) {
      // 高度未变 **且已在容忍区内**才算真正稳定：切回会话时 spacer 逐帧修正，
      // 只看「高度没变」会在两次修正之间误判稳定而提前停转，之后高度再变就没人续滚
      // （位置被钳开→跟随核心再拉回，来回几下就是切换时的抖动）
      if (++idleFrames >= KEEP_IDLE_FRAMES) return
    } else {
      idleFrames = 0
    }
    lastKeepHeight = height
    keepFrames++
    if (keepFrames < keepBudget) {
      keepAligning = true
      requestAnimationFrame(keepTick)
    }
  }

  function follow() {
    gestureHold = false
    upIntentAt = -Infinity // 明确要求到最新：清掉上翻宽限，后续内容增长照常回正
    followOffset = 0 // 明确要到最新：清掉距底位移
    setState("follow")
    intent.follow()
    pin()
    noteActivity()
  }

  function restore(top: number) {
    el.scrollTop = top
    lastInternalAt = now() // 恢复赋值的迟到事件归因为内部动作
    const next = intent.sync(el.scrollTop, el.scrollHeight, el.clientHeight)
    if (next === "follow") syncFollowOffset()
    setState(next)
  }

  /* ---------- 输入预判（只用于短期抑制程序回正，状态由位移判定） ---------- */

  el.addEventListener(
    "wheel",
    (e) => {
      // 用户在场：此后 80ms 内的位移不再归因为程序动作（否则锁底后紧跟的用户滚动会被吞掉）
      lastInternalAt = -Infinity
      if ((e as WheelEvent).deltaY < 0) {
        upIntentAt = now()
        opts.onScroll?.() // 输入即时表态：按钮/提示随「用户已上翻」立刻刷新
      }
    },
    { passive: true },
  )

  let touchY = 0
  el.addEventListener(
    "touchstart",
    (e) => {
      lastInternalAt = -Infinity
      touchY = (e as TouchEvent).touches[0]?.clientY ?? 0
    },
    { passive: true },
  )
  el.addEventListener(
    "touchmove",
    (e) => {
      lastInternalAt = -Infinity
      // 手指下移 = 内容上滚
      if (((e as TouchEvent).touches[0]?.clientY ?? 0) - touchY > TOUCH_SLOP) {
        upIntentAt = now()
        opts.onScroll?.()
      }
    },
    { passive: true },
  )

  /* ---------- 显式输入（不等位移判定，立即表态） ---------- */

  /** 拖动 / 自动滚动结束：此时不再有 scroll 事件，显式按几何归位（拖到底 = 明确要到最新）。 */
  function settleGesture() {
    gestureHold = false
    const next = intent.sync(el.scrollTop, el.scrollHeight, el.clientHeight)
    if (next === "follow") syncFollowOffset()
    setState(next)
  }

  el.addEventListener("pointerdown", (e) => {
    const ev = e as PointerEvent
    if (ev.button === 1) {
      // 中键自动滚动：明确是「要滚动」而非「追最新」——已在阅读位置就即时表态
      // （位移判定也能解除，这里只是不等它）
      if (!isAtBottom()) setState(intent.stop())
      return
    }
    // 滚动条拖动：命中滚动条槽区（经典滚动条宽 = offsetWidth - clientWidth；覆盖式滚动条宽 0
    // 不命中，由位移判定兜底——拖动期间的位移不满足「向下滚到底」的恢复条件）
    const sbw = el.offsetWidth - el.clientWidth
    if (sbw <= 0 || typeof window === "undefined") return
    const rect = el.getBoundingClientRect()
    if (ev.clientX > rect.right - sbw - 4) {
      gestureHold = true
      window.addEventListener("pointerup", settleGesture, { once: true })
      window.addEventListener("pointercancel", settleGesture, { once: true })
    }
  })

  if (opts.keyTarget) {
    opts.keyTarget.addEventListener("keydown", (e) => {
      const ev = e as KeyboardEvent
      if (!ev.key || ev.defaultPrevented || !UP_KEYS.has(ev.key)) return
      // 输入框内的方向键滚动的是文本光标，不是列表
      const t = ev.target as { closest?: (sel: string) => unknown } | null
      if (t && typeof t.closest === "function" && t.closest("input, textarea, [contenteditable='true']")) return
      setState(intent.stop())
    })
  }

  /* ---------- 滚动事件：交意图识别核判定 ---------- */

  el.addEventListener(
    "scroll",
    () => {
      opts.onScroll?.()
      // quiet：程序落底/内容变化的迟到事件（用户在场时由 wheel/touch 监听清掉时间戳，
      // 故用户自己的滚动始终参与判定，不会被归因为内部动作而反复拽回）
      const quiet = now() - lastInternalAt <= INTERNAL_QUIET_MS
      if (quiet) {
        // 追最新期间的过期位置（内容已增长、事件与引发它的变化同帧/下一帧送达）→ 回正到底：
        // 内容高度的异步修正不总能触发观察器，迟到事件是最后一道兼容旧契约的兜底
        if (canPin() && !isAtBottom()) {
          pin()
          noteActivity()
        }
        return
      }
      // 拖动（gestureHold）不跳过判定——用户的位移照常可解除跟随，只是不允许「向下滚到底恢复」
      // （拖到阈值区内不能被拽到底）
      const next = intent.observe(el.scrollTop, el.scrollHeight, el.clientHeight, { quiet: false, allowResume: !gestureHold })
      // 用户已往最新方向滚（向下位移）：上翻宽限失效，后续内容增长照常回正
      if (intent.movingDown()) upIntentAt = -Infinity
      // 跟随态内的位置就是用户的意图：记下他的距底位移，内容增长按它保留偏移
      // （向下滚到真正到底时 offset 自然归 0）
      if (next === "follow") syncFollowOffset()
      setState(next)
    },
    { passive: true },
  )

  // 防御：测试环境可能无 MutationObserver（同下方 ResizeObserver 的守卫）
  if (opts.observeMutations && typeof MutationObserver !== "undefined") {
    new MutationObserver(() => contentChanged()).observe(el, { childList: true, subtree: true, characterData: true })
  }
  if (typeof ResizeObserver !== "undefined") {
    new ResizeObserver(() => contentChanged()).observe(el)
  }

  return {
    isAtBottom,
    isFollowing: () => intent.state() === "follow",
    pinTarget: () => (canPin() ? el.scrollHeight - el.clientHeight - followOffset : null),
    state: () => intent.state(),
    shouldShowJump: () =>
      intent.state() !== "follow" ||
      (!isAtBottom() && (intent.hasUpIntent() || now() - upIntentAt <= UP_INTENT_GRACE_MS)),
    follow,
    contentChanged,
    restore,
    stopFollowing: () => setState(intent.stop()),
  }
}
