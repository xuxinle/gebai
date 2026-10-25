import { jumpBottom } from "./state"
import { onPaneActivate, type MsgPane } from "./msg-panes"

/**
 * 粘底滚动与「回到最新」按钮（逻辑见 sticky-scroll.ts / sticky-follow.ts，按容器实例化）。
 *
 * 粘底跟随是**容器级**几何量（scrollTop / scrollHeight / 用户滚动意图），随会话容器各自
 * 存在：每个 pane 一份实例，切换会话即切换实例。对外导出（scrollIfSticky / lockToBottom /
 * restoreScroll …）保持原签名——调用点（messages / stream / events / sessions 共 100+ 处）
 * 一律无需关心 pane，内部转发到活动 pane 的实例，无 pane 时空转。
 *
 * 「回到最新」按钮全局唯一（固定在聊天区右下角）：各实例显隐同一按钮，因此只有活动 pane
 * 的实例在驱动它——非活动实例的按钮回调被忽略，切回时按新实例状态重刷（见 activate）。
 */

let current: MsgPane | null = null

/** 切到活动 pane 的实例：重新装配窗口化跟随源（落点/内容变更回到新容器）。 */
function onActivate(pane: MsgPane | null): void {
  current = pane
  if (!pane) return
  pane.virtualizer.setFollowSource(pane.sticky.pinTarget)
  pane.virtualizer.setContentChangedHook(pane.sticky.scrollIfSticky)
  pane.sticky.refresh()
}

onPaneActivate(onActivate)

/** 活动 pane 的粘底实例（无则 null）。 */
function handle() {
  return current?.sticky ?? null
}

export function isAtBottom(): boolean {
  return handle()?.isAtBottom() ?? true
}
export function isFollowing(): boolean {
  return handle()?.isFollowing() ?? true
}
export function scrollIfSticky(): void {
  handle()?.scrollIfSticky()
}
export function lockToBottom(): void {
  handle()?.lockToBottom()
}
export function restoreScroll(top: number): void {
  handle()?.restoreScroll(top)
}
export function stopFollowing(): void {
  handle()?.stopFollowing()
}
export function noteIncoming(): void {
  handle()?.noteIncoming()
}
export function clearUnread(): void {
  handle()?.clearUnread()
}
export function refreshJumpBottom(): void {
  handle()?.refresh()
}

/**
 * 按钮显隐必须只由活动 pane 驱动：非活动 pane 的实例同样会调 refresh（内容变更时），
 * 若不拦截，后台会话的内容增长会把按钮在用户当前会话里点亮/熄灭。
 */
export { jumpBottom }
