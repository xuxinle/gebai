import { jumpBottom, msgEl } from "./state"
import { createStickyScroll } from "./sticky-scroll"
import { autoHideScrollbar } from "./ui"

/** 粘底滚动（逻辑见 sticky-scroll.ts）：真实 DOM 绑定入口，各模块经命名导出使用。 */
const sticky = createStickyScroll(msgEl, jumpBottom)
autoHideScrollbar(msgEl)

export const isAtBottom = sticky.isAtBottom
export const scrollIfSticky = sticky.scrollIfSticky
export const lockToBottom = sticky.lockToBottom
export const restoreScroll = sticky.restoreScroll
export const stopFollowing = sticky.stopFollowing
export const noteIncoming = sticky.noteIncoming
export const clearUnread = sticky.clearUnread
export const refreshJumpBottom = sticky.refresh
