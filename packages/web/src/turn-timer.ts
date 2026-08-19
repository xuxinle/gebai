/**
 * 单轮计时器：任务（单轮对话）运行期间在当前流式助手消息 meta 上显示本轮耗时，任务结束定格显示。
 * - 默认开启；设置面板「外观」tab 可关闭（根元素 data-turn-timer="off"，CSS 隐藏全部计时元素）
 * - localStorage `gebai.ui.turnTimer` 仅存 "off"（关闭）；不存/其它值 = 开启（默认）
 * - 跨标签页 storage 事件同步
 */

export type TurnTimerSetting = "on" | "off"

const KEY = "gebai.ui.turnTimer"

/** 用户设置（默认 on）。 */
export function getTurnTimerSetting(): TurnTimerSetting {
  try {
    if (localStorage.getItem(KEY) === "off") return "off"
  } catch {
    /* 隐私模式等场景忽略 */
  }
  return "on"
}

export function isTurnTimerEnabled(): boolean {
  return getTurnTimerSetting() === "on"
}

/** 应用设置：off 时在根元素标记 data-turn-timer（CSS 据此隐藏计时元素）。 */
export function applyTurnTimer(): void {
  const el = document.documentElement
  if (getTurnTimerSetting() === "off") el.dataset.turnTimer = "off"
  else delete el.dataset.turnTimer
}

/** 手动设置（设置面板「外观」）：on=开启（默认，清存储不留冗余）；off=关闭。持久化 + 立即生效。 */
export function setTurnTimerSetting(v: TurnTimerSetting): void {
  try {
    if (v === "off") localStorage.setItem(KEY, v)
    else localStorage.removeItem(KEY)
  } catch {
    /* ignore */
  }
  applyTurnTimer()
}

/** 初始化：应用当前设置，并跨标签页同步（其他标签修改设置后本标签即时生效）。 */
export function initTurnTimer(): void {
  applyTurnTimer()
  window.addEventListener("storage", (e) => {
    if (e.key !== KEY) return
    applyTurnTimer()
  })
}
