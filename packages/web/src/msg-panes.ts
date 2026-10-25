/**
 * 会话消息视图（pane）：每个会话一份独立的滚动容器 + 几何实例。
 *
 * 为什么要按会话分容器：滚动区、窗口化、粘底跟随、消息导航都是**以容器为单位**的几何量
 * （视口高度、scrollTop、块坐标）。共用单一容器时，切走会话只能清空重建（等一次网络往返），
 * 后台会话的 DOM 也无处安放——工具卡/流式正文只能在切回后从历史重拉。
 *
 * 结构与样式：每个 pane 的滚动容器带 `.messages` 类（与历史单容器的 `#messages` 共用同一套
 * CSS 规则），宿主 `#messages` 本身不再滚动——它是「容器栈」，同一时刻只有一个子容器参与
 * 布局，其余 `hidden`（DOM 保留、滚动位置保留、窗口化与在途流引用不失效）。
 *
 * 生命周期：运行中会话的 pane 一直保留（后台事件照常渲染进它自己的容器）；**已停止会话按 LRU 缓存**——
 * 最近用过的 `IDLE_PANE_CACHE` 个容器继续留着，来回切换都是纯显隐（不再重拉重建），超出才释放，
 * 释放后再切回走一次性 `loadMessages`。
 *
 * 容器自带**消息数据**（`chunks`）：窗口化会根据滚动位置按需渲染块，而块渲染需要该会话的
 * 消息清单与块表——数据只存一份全局的话，缓存下来的容器重新显示时拿不到自己的消息（表现为
 * “切回去是空白”）。
 */

import type { Message } from "@gebai/sdk"
import { jumpBottom, msgEl } from "./state"
import { createStickyScroll, type StickyScrollHandle } from "./sticky-scroll"
import { createVirtualizer, type Virtualizer } from "./virtualize"

/** 已停止会话的容器缓存上限（个）：「来回切」的体感关键在于切回不用重拉，而容器本身不贵——
 *  窗口化只挂载视口附近的块（长会话也不是整篇 DOM），真正的开销在重拉与全量重建。
 *  超限按 LRU 释放最久未用的（运行中的永不释放）。 */
export const IDLE_PANE_CACHE = 12

/** 消息视图的块表：消息清单 + 块边界 + id 下标（窗口化与导航定位的依据）。 */
export interface PaneChunks {
  msgs: Message[]
  bounds: number[]
  sessionId: string
  index: Map<string, number>
}

/** 一个会话的消息视图。 */
export interface MsgPane {
  sessionId: string
  /** 滚动容器（`.messages`）：窗口化与粘底跟随的几何宿主。 */
  container: HTMLElement
  virtualizer: Virtualizer
  sticky: StickyScrollHandle
  /** 任务是否仍在运行：运行中永不释放；已停止的按 LRU 缓存。 */
  running: boolean
  /** 最近一次显示的时刻（LRU 排序依据）。 */
  lastUsed: number
  /** 本会话的块表（缓存页切回时靠它重建历史块）。 */
  chunks: PaneChunks | null
}

const panes = new Map<string, MsgPane>()
/** 当前显示的 pane（无会话/草稿页时为 null）。 */
let active: MsgPane | null = null
/** 块渲染回调：一块怎么渲染依赖会话消息数组与运行态，由 sessions 侧注册。 */
let blockRenderer: ((pane: MsgPane, index: number, host: HTMLElement) => void) | null = null
/** pane 切换监听（jump-bottom 据此重绑跟随源、msg-nav 据此重绑滚动监听）。 */
const activateListeners = new Set<(pane: MsgPane | null) => void>()

/** 注册块渲染回调（sessions 装配；pane 参数决定用哪份会话数据渲染）。 */
export function setPaneBlockRenderer(fn: (pane: MsgPane, index: number, host: HTMLElement) => void): void {
  blockRenderer = fn
}

/** 订阅活动 pane 变化；返回退订函数。 */
export function onPaneActivate(cb: (pane: MsgPane | null) => void): () => void {
  activateListeners.add(cb)
  return () => {
    activateListeners.delete(cb)
  }
}

/** 当前显示的 pane（无则 null）。 */
export function activePane(): MsgPane | null {
  return active
}

/** 取会话的 pane（不存在返回 undefined，**不创建**——调用方据此走一次性加载路径）。 */
export function paneOf(sessionId: string): MsgPane | undefined {
  return panes.get(sessionId)
}

/** 创建 pane 的 DOM 与几何实例（不激活）。 */
function createPane(sessionId: string): MsgPane {
  const container = document.createElement("div")
  container.className = "messages"
  container.dataset.sessionId = sessionId
  container.hidden = true
  // pane 引用先占位：「回到最新」按钮的 isActive 回调在 createStickyScroll 构造期就会被调用
  // （实例化末尾会 refresh 一次），此时下面的常量尚未初始化（TDZ）
  let pane: MsgPane | null = null
  const created: MsgPane = {
    sessionId,
    container,
    virtualizer: createVirtualizer({
      container,
      renderBlock: (index, host) => blockRenderer?.(created, index, host),
    }),
    // 「回到最新」按钮全局唯一：只有活动 pane 的实例驱动它（否则后台会话内容增长会
    // 在用户当前会话里点亮/熄灭按钮）
    sticky: createStickyScroll(container, jumpBottom, { isActive: () => pane !== null && active === pane }),
    running: false,
    lastUsed: Date.now(),
    chunks: null,
  }
  pane = created
  msgEl.appendChild(container)
  panes.set(sessionId, created)
  return created
}

/** 取会话 pane，不存在则创建（调用方表示「该会话需要自己的容器」）。 */
export function ensurePane(sessionId: string): MsgPane {
  return panes.get(sessionId) ?? createPane(sessionId)
}

/** 标记会话运行态（运行中的 pane 永不释放）。 */
export function setPaneRunning(sessionId: string, running: boolean): void {
  const pane = panes.get(sessionId)
  if (pane) pane.running = running
}

/** 激活会话 pane：显示它、隐藏其余，并通知订阅者重绑几何监听。
 *  顺序很关键：**先隐藏旧、再显示新**，且只写真正需要改的那个（缓存页切回时，其余大多
 *  已是 hidden）。先显后隐会让两个容器同时参与布局（可见容器数短暂翻倍，长会话下
 *  整页重算翻倍，切换手感变钝）。 */
export function activatePane(sessionId: string): MsgPane {
  const pane = ensurePane(sessionId)
  pane.lastUsed = Date.now()
  if (active === pane) return pane
  const prev = active
  if (prev && prev !== pane) prev.container.hidden = true
  for (const other of panes.values()) {
    if (other === pane || other === prev) continue
    if (!other.container.hidden) other.container.hidden = true
  }
  pane.container.hidden = false
  active = pane
  for (const cb of activateListeners) cb(pane)
  return pane
}

/** 取消激活（草稿页/登出）：全部隐藏。 */
export function deactivatePane(): void {
  if (!active) return
  active.container.hidden = true
  active = null
  for (const cb of activateListeners) cb(null)
}

/**
 * 销毁会话 pane（释放容器与几何实例）。**运行中的 pane 不销毁**——后台仍需持续渲染，
 * 由任务结束后的切走路径回收。
 */
export function destroyPane(sessionId: string): boolean {
  const pane = panes.get(sessionId)
  if (!pane || pane.running) return false
  if (active === pane) deactivatePane()
  pane.container.remove()
  panes.delete(sessionId)
  return true
}

/**
 * 按 LRU 释放已停止会话的容器：保留最近用过的 `keep` 个（含活动 pane，永不释放）与全部运行中会话，
 * 其余从最久未用开始释放。返回释放的数量。
 *
 * 调用时机是**切换完成之后**（切走旧会话、新会话已就位）——不能在切换前调：那会把用户
 * 正要切过去的那个容器一并收掉，快速路径永远不命中（表现就是“每次都卡一下”）。
 */
export function reapIdlePanes(keep = IDLE_PANE_CACHE): number {
  const idle = [...panes.values()].filter((p) => !p.running && p !== active)
  const excess = idle.length - keep
  if (excess <= 0) return 0
  idle.sort((a, b) => a.lastUsed - b.lastUsed) // 最久未用在前
  let n = 0
  for (const pane of idle.slice(0, excess)) {
    pane.container.remove()
    panes.delete(pane.sessionId)
    n += 1
  }
  return n
}

/** 现有 pane 数（测试与诊断用）。 */
export function paneCount(): number {
  return panes.size
}

/** 清空全部 pane（登出/测试）：不做运行态保护。 */
export function resetPanes(): void {
  for (const pane of panes.values()) pane.container.remove()
  panes.clear()
  active = null
  for (const cb of activateListeners) cb(null)
}
