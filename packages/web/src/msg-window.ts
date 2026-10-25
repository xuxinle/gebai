/**
 * 消息列窗口化实例（DOM 装配）：几何宿主是**活动会话 pane 的滚动容器**（见 msg-panes.ts），
 * 坐标算术见 virtual-window.ts、DOM 编排见 virtualize.ts。
 *
 * 本模块只做「转发到活动 pane」这一层：`msgWindow` 的 API 与单容器时代完全一致，
 * 调用方（sessions）不必关心当前是哪个会话的容器。窗口化实例本身按会话实例化
 * （`createVirtualizer` 的 container 在构造期固定），pane 切换即换实例。
 *
 * 块渲染回调由 sessions 侧注册——渲染一块需要会话消息数组、运行态引用与历史分组逻辑，
 * 放在本模块会形成 messages ↔ sessions 的模块循环。
 */

import { activePane, setPaneBlockRenderer, type MsgPane } from "./msg-panes"
import type { Virtualizer } from "./virtualize"
import type { VzSlotSeed } from "./virtual-window"

/** 注册块渲染回调（sessions 装配）：回调按 pane 分派，用该会话自己的消息数据渲染。 */
export function setBlockRenderer(fn: (pane: MsgPane, index: number, host: HTMLElement) => void): void {
  setPaneBlockRenderer(fn)
}

/** 活动 pane 的窗口化实例（无 pane 时为 null：草稿页/未建会话）——所有方法在此为空转。 */
function active(): Virtualizer | null {
  return activePane()?.virtualizer ?? null
}

/**
 * 活动会话的窗口化句柄：方法全部转发到当前 pane 的实例，无 pane 时空转。
 * 与粘底跟随（jump-bottom）的装配在 pane 激活时完成（见 bindWindowFollow）。
 */
export const msgWindow: Virtualizer = {
  reset(seeds: VzSlotSeed[]): void {
    active()?.reset(seeds)
  },
  sync(): void {
    active()?.sync()
  },
  appendTail(node: Node): void {
    active()?.appendTail(node)
  },
  anchor(): { key: string; offset: number } | null {
    return active()?.anchor() ?? null
  },
  scrollToAnchor(key: string, offset: number): number | null {
    return active()?.scrollToAnchor(key, offset) ?? null
  },
  posOfKey(key: string): number | null {
    return active()?.posOfKey(key) ?? null
  },
  heightOfKey(key: string): number | null {
    return active()?.heightOfKey(key) ?? null
  },
  setFollowSource(fn: (() => number | null) | null): void {
    active()?.setFollowSource(fn)
  },
  setContentChangedHook(fn: (() => void) | null): void {
    active()?.setContentChangedHook(fn)
  },
  contentTop(): number {
    return active()?.contentTop() ?? 0
  },
}

/** 尾部活动区挂载（新消息 / 在途流 / 工具卡 / 异常提示）：恒挂载在窗口化 spacer 之后。
 *  省略 pane 时挂到当前活动会话（运行期新消息的正常路径）。 */
export function appendTail(node: Node): void {
  activePane()?.virtualizer.appendTail(node)
}
