/**
 * "跳到最新"浮动按钮 + 粘底滚动（jump-bottom.ts 的纯逻辑工厂，注入滚动容器与按钮，可独立单测）：
 * 按钮显隐 = 不在追最新（sticky-follow.ts 的三态：follow 隐藏，hold/reading 显示）——
 * 用户离开底部（含就地在底部附近停住）时提供一键回到底部与最新内容的入口。
 */

import { createStickyFollow, type FollowState } from "./sticky-follow"

/** 按钮显隐 / 贴底判定共用阈值（距底部 <= 该值视为在底部）。 */
const BOTTOM_THRESHOLD = 64

/** 粘底对齐保持帧预算（异步高度修正兜底：图片/图表/字体异步加载）。 */
const KEEP_ALIGN_FRAMES = 240

export interface StickyScrollHandle {
  /** 是否在底部（几何判定）。 */
  isAtBottom(): boolean
  /** 是否处于追最新（窗口化据此决定 DOM 变更后是否保持贴底）。 */
  isFollowing(): boolean
  /** 三态（follow / hold / reading；按钮显隐按此）。 */
  state(): FollowState
  /** 是否显示「回到最新」按钮。 */
  shouldShowJump(): boolean
  /** 内容变化后调用：跟随中（按钮隐藏）滚动到底，否则不动。 */
  scrollIfSticky(): void
  /** 程序回正的落点（距底 = 用户留下的位移）；不在追最新时为 null。 */
  pinTarget(): number | null
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
  /** 释放实例（容器销毁时）：注销按钮点击分发与容器监听。 */
  dispose(): void
}

/** 按钮点击分发的实例登记项。 */
interface JumpOwner {
  isActive: () => boolean
  follow: () => void
}

/**
 * 按钮点击分发（按按钮分桶）：
 * 同一按钮服务多个滚动容器实例（多 pane 并存），但点击只能落到**活动**实例的几何上——
 * 按实例闭包绑 onclick 会让按钮永久归最后创建的那个容器（切回先创建的会话后点击无反应），
 * 容器被 LRU 释放后更是无主。故此处按按钮汇总全部实例，点击时执行活动实例的跟随。
 */
const ownersByBtn = new WeakMap<HTMLElement, Set<JumpOwner>>()

function registerJumpOwner(btn: HTMLElement, owner: JumpOwner): void {
  let owners = ownersByBtn.get(btn)
  if (!owners) {
    owners = new Set()
    ownersByBtn.set(btn, owners)
    btn.onclick = () => {
      for (const candidate of ownersByBtn.get(btn) ?? []) {
        if (!candidate.isActive()) continue
        candidate.follow()
        return
      }
    }
  }
  owners.add(owner)
}

function unregisterJumpOwner(btn: HTMLElement, owner: JumpOwner): void {
  const owners = ownersByBtn.get(btn)
  if (!owners) return
  owners.delete(owner)
  if (owners.size === 0) ownersByBtn.delete(btn)
}

export function createStickyScroll(el: HTMLElement, btn: HTMLElement, opts?: { isActive?: () => boolean }): StickyScrollHandle {
  // 按钮全局唯一（固定在聊天区右下角）：只有活动会话的实例能驱动它的显隐与点击——
  // 否则后台会话的内容增长会在用户当前会话里点亮/熄灭按钮（多 pane 并存后的必然问题）
  const isActive = opts?.isActive ?? (() => true)
  const core = createStickyFollow(el, {
    threshold: BOTTOM_THRESHOLD,
    keepFrames: KEEP_ALIGN_FRAMES,
    observeMutations: true,
    // 防御：测试环境可能存在缺 addEventListener 的 window 泄漏 stub
    keyTarget: typeof window !== "undefined" && typeof window.addEventListener === "function" ? window : undefined,
    onScroll: refresh,
    onFollowingChange: refresh,
    onStateChange: refresh,
  })

  // 按钮显隐 = 不在追最新，或用户已把视口拉开（上翻位移，确认期内）：
  // 用户一滚开就有回到最新的入口；追最新期间的跟随瞬态（内容增长的一帧）不闪出按钮
  function refresh() {
    if (!isActive()) return
    btn.hidden = !core.shouldShowJump()
  }

  // 点击滚动到底并重新锁定（分发给活动实例，见 registerJumpOwner）
  const owner: JumpOwner = { isActive, follow: () => core.follow() }
  registerJumpOwner(btn, owner)

  // 图片异步加载改变高度（含 markdown 内嵌 <img>，无独立 onload 处理）：跟随中滚动到底
  const onLoad = (e: Event) => {
    if ((e.target as HTMLElement).tagName === "IMG") core.contentChanged()
  }
  el.addEventListener("load", onLoad, true)

  refresh()
  return {
    isAtBottom: core.isAtBottom,
    isFollowing: core.isFollowing,
    pinTarget: core.pinTarget,
    state: core.state,
    shouldShowJump: core.shouldShowJump,
    scrollIfSticky: core.contentChanged,
    lockToBottom: core.follow,
    restoreScroll: core.restore,
    stopFollowing: core.stopFollowing,
    noteIncoming: refresh,
    clearUnread: refresh,
    refresh,
    dispose() {
      el.removeEventListener("load", onLoad, true)
      unregisterJumpOwner(btn, owner)
    },
  }
}
