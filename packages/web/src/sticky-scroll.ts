/**
 * "跳到最新"浮动按钮 + 粘底滚动（纯逻辑工厂，注入滚动容器与按钮，可独立单测）：
 * 粘底锁定与按钮显隐完全一致（同一阈值）：按钮隐藏 = 锁定跟随（新内容持续滚动到底），
 * 按钮显示 = 用户在阅读历史（不打扰）。位置即状态：任何滚动事件按位置刷新锁定与按钮，
 * 仅有的例外是程序滚动事件（位置 = 我们设置的目标）——保持锁定；期间内容可能已增长使
 * 落位过期，则续滚到最新底部（否则迟到事件会把锁定悄悄解除，会话自动滚动到底失灵）。
 * 新内容（DOM 变化 / 图片加载）触发滚动，rAF 节流每帧至多一次（流式高频更新性能）。
 * jump-bottom.ts 以真实 DOM 绑定此工厂；测试直接注入 fake 滚动容器。
 */

/** 按钮显隐 / 粘底锁定共用阈值（距底部 <= 该值视为在底部）。 */
const BOTTOM_THRESHOLD = 64

export interface StickyScrollHandle {
  /** 是否在底部（按钮显隐判断，与锁定状态一致）。 */
  isAtBottom(): boolean
  /** 内容变化后调用：锁定中（按钮隐藏）则跟随滚动到底，否则不动。 */
  scrollIfSticky(): void
  /** 发送新消息 / 会话加载完成时调用：滚动到底并锁定（此前用户滚走阅读历史后，操作即恢复跟随）。 */
  lockToBottom(): void
  /** 新消息到达时由 messages / main 调用：按当前位置刷新按钮显隐。 */
  noteIncoming(): void
  /** 清空按钮状态（切换会话、流式结束时）：按当前位置刷新显隐。 */
  clearUnread(): void
  /** 按当前位置刷新按钮显隐。 */
  refresh(): void
}

export function createStickyScroll(el: HTMLElement, btn: HTMLElement): StickyScrollHandle {
  function isAtBottom(): boolean {
    return el.scrollHeight - el.scrollTop - el.clientHeight <= BOTTOM_THRESHOLD
  }

  /** 锁定跟随状态：与按钮显隐一致——隐藏（在底部）= 跟随，显示 = 用户阅读历史中。
   *  仅由滚动事件按位置刷新（程序滚动事件除外），不另设独立状态。 */
  let following = true

  function refresh() {
    btn.hidden = isAtBottom()
  }

  /** 最近一次程序滚动的实际落位（scrollTop 赋值后的读回值，浏览器已 clamp 到
   *  scrollHeight - clientHeight）。滚动事件异步送达，期间内容可能增长使落位过期；
   *  事件位置与目标一致即识别为程序滚动，保持锁定并续滚到最新底部。 */
  let programTarget: number | null = null

  /** 程序滚动到底：位置有变化才记录落位（无变化浏览器不产生滚动事件，不留残余目标）。 */
  function scrollBottom() {
    const before = el.scrollTop
    el.scrollTop = el.scrollHeight
    if (el.scrollTop !== before) programTarget = el.scrollTop
  }

  // rAF 节流：流式高频内容变化每帧至多滚动一次（性能）
  let scrollRaf = false
  function scrollIfSticky(): void {
    if (scrollRaf) return
    scrollRaf = true
    requestAnimationFrame(() => {
      scrollRaf = false
      // 以执行时锁定状态为准（排期期间可能被用户滚动翻转）
      if (following) scrollBottom()
      noteFollowActivity()
    })
  }

  /**
   * 粘底对齐保持：内容高度存在不触发 MutationObserver 的异步修正（content-visibility
   * 懒布局、字体/图片加载等）——长会话（低性能模式）中视口外消息的高度估算会在滚动后
   * 数百毫秒~数秒内陆续修正，单次滚动与短时收敛都会在修正到达前提前退出，把最新消息
   * 永久切在视口外（用户消息只显示一半）。跟随期间按帧续查对齐，任何滚动活动重置帧预算，
   * 预算耗尽（连续贴底无变化）后自停；用户上翻（following=false）立即停转。每帧仅三次
   * 属性读取（布局干净时不触发重排），成本可忽略。
   */
  const KEEP_ALIGN_FRAMES = 240
  let keepAligning = false
  let keepFrames = 0

  function noteFollowActivity() {
    keepFrames = 0
    if (keepAligning) return
    keepAligning = true
    requestAnimationFrame(keepTick)
  }

  function keepTick() {
    keepAligning = false
    if (!following) return
    keepFrames++
    if (el.scrollTop < el.scrollHeight - el.clientHeight) scrollBottom()
    if (keepFrames < KEEP_ALIGN_FRAMES) {
      keepAligning = true
      requestAnimationFrame(keepTick)
    }
  }

  function lockToBottom(): void {
    following = true
    scrollBottom()
    refresh()
    noteFollowActivity()
  }

  btn.onclick = () => {
    // 点击滚动到底：落位后按钮隐藏（锁定），由滚动事件按目标比对保持锁定
    following = true
    scrollBottom()
    noteFollowActivity()
  }

  el.addEventListener(
    "scroll",
    () => {
      const ours = programTarget !== null && el.scrollTop === programTarget
      programTarget = null
      if (ours) {
        // 程序滚动事件：本次滚动意图就是底部 → 保持锁定；期间内容增长使落位过期
        // （位置已不在当前底部）→ 续滚到最新底部
        following = true
        if (!isAtBottom()) scrollBottom()
      } else {
        // 用户滚动（滚轮 / 触控板 / 键盘 / 滚动条拖动）：位置即状态——
        // 在底部（阈值内，按钮隐藏）自动锁定跟随，脱离底部（按钮显示）立即解除
        following = isAtBottom()
      }
      refresh()
    },
    { passive: true },
  )

  // 任何 DOM 内容变化（含异步追加）后：锁定中则跟随滚动
  new MutationObserver(() => scrollIfSticky()).observe(el, { childList: true, subtree: true, characterData: true })

  // 图片异步加载改变高度（含 markdown 内嵌 <img>，无独立 onload 处理）：锁定中则跟随滚动到底
  el.addEventListener(
    "load",
    (e) => {
      if ((e.target as HTMLElement).tagName === "IMG") scrollIfSticky()
    },
    true,
  )

  const noteIncoming = refresh
  const clearUnread = refresh

  return { isAtBottom, scrollIfSticky, lockToBottom, noteIncoming, clearUnread, refresh }
}
