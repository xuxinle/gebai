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
  /** 程序恢复历史滚动位置（会话切回的阅读位置记忆）：落位后按新位置同步锁定状态，
   *  并使未决的跟随回调（排期中的 rAF / 对齐保持循环）失效——它们晚于恢复执行时
   *  会把刚恢复的历史位置拽到底部（loadMessages 尾部竞态）。 */
  restoreScroll(top: number): void
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

  /** 最近一次程序滚动后的落位（scrollTop 赋值后的读回值）。scroll 事件异步送达，rAF 可能
   *  先行执行（keepTick / scrollIfSticky 回调读到旧 following）：位置距程序落位的跌幅超过
   *  底部阈值 = 用户已上翻（上翻只会减小 scrollTop），立即按位置解除跟随，不拽回底部。 */
  let programTop = 0

  /** 程序滚动到底：位置有变化才记录落位（无变化浏览器不产生滚动事件，不留残余目标）。 */
  function scrollBottom() {
    const before = el.scrollTop
    el.scrollTop = el.scrollHeight
    if (el.scrollTop !== before) programTarget = el.scrollTop
    programTop = el.scrollTop
  }

  /** 跟随回调执行前的用户滚动检测（scroll 事件未送达窗口）：上翻只会减小 scrollTop——
   *  距程序落位的跌幅在底部阈值内（等价 scroll 事件按事件时几何判 isAtBottom：小幅上滚
   *  仍算贴底）保持跟随；跌幅超阈值 = 用户已上翻，按位置解除跟随并返回 false。不能按
   *  当前几何判 isAtBottom——rAF 前内容增长会把「离开时还贴底」的位置误判为远离底部。 */
  function stillFollowing(): boolean {
    if (el.scrollTop >= programTop - BOTTOM_THRESHOLD - 1) return true
    following = false
    refresh()
    return false
  }

  // rAF 节流：流式高频内容变化每帧至多滚动一次（性能）
  let scrollRaf = false
  function scrollIfSticky(): void {
    if (scrollRaf) return
    scrollRaf = true
    requestAnimationFrame(() => {
      scrollRaf = false
      // 以执行时锁定状态为准（排期期间可能被用户滚动翻转）；用户已上翻但 scroll 事件
      // 未送达（rAF 先行）→ 按程序落位比对识别，不拽回
      if (following && stillFollowing()) scrollBottom()
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
    // 用户已上翻但 scroll 事件未送达（rAF 先行执行）：按程序落位比对识别，立即停转不拽回
    // （否则 lockToBottom 后的 240 帧窗口内用户滚一下即被拽回底部）
    if (!stillFollowing()) return
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

  function restoreScroll(top: number): void {
    el.scrollTop = top
    programTarget = null // 恢复产生的 scroll 事件按用户分支处理（following 已按下落位同步）
    following = isAtBottom()
    refresh()
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

  return { isAtBottom, scrollIfSticky, lockToBottom, restoreScroll, noteIncoming, clearUnread, refresh }
}
