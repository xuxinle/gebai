/**
 * "跳到最新"浮动按钮 + 粘底滚动（jump-bottom.ts 的纯逻辑工厂，注入滚动容器与按钮，可独立单测）：
 * 按钮显隐 = 几何贴底（同一阈值），跟随切换机制见 sticky-follow.ts（意图驱动）——
 * 按钮隐藏 = 在底部（跟随中），按钮显示 = 用户在阅读历史（不打扰）。
 */

import { createStickyFollow } from "./sticky-follow"

/** 按钮显隐 / 贴底判定共用阈值（距底部 <= 该值视为在底部）。 */
const BOTTOM_THRESHOLD = 64

/** 粘底对齐保持帧预算（content-visibility 异步高度修正兜底）。 */
const KEEP_ALIGN_FRAMES = 240

export interface StickyScrollHandle {
  /** 是否在底部（按钮显隐判断，与跟随状态一致）。 */
  isAtBottom(): boolean
  /** 内容变化后调用：跟随中（按钮隐藏）滚动到底，否则不动。 */
  scrollIfSticky(): void
  /** 发送新消息 / 会话加载完成时调用：滚动到底并锁定（此前用户滚走阅读历史后，操作即恢复跟随）。 */
  lockToBottom(): void
  /** 程序恢复历史滚动位置（会话切回的阅读位置记忆）：落位后按几何同步跟随状态。 */
  restoreScroll(top: number): void
  /** 用户导航（消息导航跳转）前调用：显式解除跟随。 */
  stopFollowing(): void
  /** 新消息到达时由 messages / main 调用：按当前位置刷新按钮显隐。 */
  noteIncoming(): void
  /** 清空按钮状态（切换会话、流式结束时）：按当前位置刷新显隐。 */
  clearUnread(): void
  /** 按当前位置刷新按钮显隐。 */
  refresh(): void
}

export function createStickyScroll(el: HTMLElement, btn: HTMLElement): StickyScrollHandle {
  const core = createStickyFollow(el, {
    threshold: BOTTOM_THRESHOLD,
    keepFrames: KEEP_ALIGN_FRAMES,
    observeMutations: true,
    // 防御：测试环境可能存在缺 addEventListener 的 window 泄漏 stub
    keyTarget: typeof window !== "undefined" && typeof window.addEventListener === "function" ? window : undefined,
    onScroll: refresh,
    onFollowingChange: refresh,
  })

  // 按钮显隐 = 跟随状态（完全一致）：隐藏 = 跟随中（贴底），显示 = 用户阅读历史。
  // 跟随状态与几何贴底在稳态下双向同步（贴底事件恢复跟随、输入意图解除跟随），
  // 以状态而非几何刷新可避免输入事件先于位置变化的窗口内显隐过期。
  function refresh() {
    btn.hidden = core.isFollowing()
  }

  // 点击滚动到底并重新锁定
  btn.onclick = () => core.follow()

  // 图片异步加载改变高度（含 markdown 内嵌 <img>，无独立 onload 处理）：跟随中滚动到底
  el.addEventListener(
    "load",
    (e) => {
      if ((e.target as HTMLElement).tagName === "IMG") core.contentChanged()
    },
    true,
  )

  refresh()
  return {
    isAtBottom: core.isAtBottom,
    scrollIfSticky: core.contentChanged,
    lockToBottom: core.follow,
    restoreScroll: core.restore,
    stopFollowing: core.stopFollowing,
    noteIncoming: refresh,
    clearUnread: refresh,
    refresh,
  }
}
