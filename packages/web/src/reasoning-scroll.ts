/**
 * 推理体（.reasoning-body）内部滚动条粘底跟随：
 * 推理流式更新时，只要用户没有主动向上翻阅，滚动条自动贴底（最新推理始终可见）；
 * 用户滚动脱离底部立即解除跟随，滚回底部自动恢复。核心机制见 sticky-follow.ts（意图驱动）。
 */

import { createStickyFollow, type StickyFollowHandle } from "./sticky-follow"

/** 吸底阈值（严格贴着底部才视为锁定：小幅上滚即解除，防止被拽回底部）。 */
const PIN_THRESHOLD = 4

/** 每个推理体的跟随核心：首次调用前视为跟随。 */
const followers = new WeakMap<HTMLElement, StickyFollowHandle>()

/** 推理内容流式更新后调用：跟随中则滚动到底，用户翻阅历史时不动。 */
export function scrollReasoningSticky(body: HTMLElement): void {
  if (!body.isConnected) return
  let h = followers.get(body)
  if (!h) {
    h = createStickyFollow(body, { threshold: PIN_THRESHOLD, keepFrames: 120 })
    followers.set(body, h)
  }
  h.contentChanged()
}
