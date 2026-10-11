/**
 * 消息列窗口化实例（DOM 装配）：容器为 `#messages`，坐标算术见 virtual-window.ts、
 * DOM 编排见 virtualize.ts。
 *
 * 块渲染回调由 sessions 侧注册——渲染一块需要会话消息数组、运行态引用与历史分组逻辑，
 * 放在本模块会形成 messages ↔ sessions 的模块循环。
 */

import { msgEl } from "./state"
import { createVirtualizer, type Virtualizer } from "./virtualize"

let blockRenderer: ((index: number, host: HTMLElement) => void) | null = null

export const msgWindow: Virtualizer = createVirtualizer({
  container: msgEl,
  renderBlock: (index, host) => blockRenderer?.(index, host),
})

/** 注册块渲染回调（sessions 装配）。 */
export function setBlockRenderer(fn: (index: number, host: HTMLElement) => void): void {
  blockRenderer = fn
}

/** 装配粘底跟随（jump-bottom）：`pinTarget` 给出程序回正落点（含用户留下的距底位移），
 *  `contentChanged` 在其后接手对齐。两者均由跟随核心提供。 */
export function bindWindowFollow(pinTarget: () => number | null, contentChanged: () => void): void {
  msgWindow.setFollowSource(pinTarget)
  msgWindow.setContentChangedHook(contentChanged)
}

/** 尾部活动区挂载（新消息 / 在途流 / 工具卡 / 异常提示）：恒挂载在窗口化 spacer 之后。 */
export function appendTail(node: Node): void {
  msgWindow.appendTail(node)
}
