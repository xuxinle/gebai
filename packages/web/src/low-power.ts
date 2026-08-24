/**
 * 低性能模式：纯手动开关（设置面板「外观」），不做硬件/系统偏好自动检测。
 * - 手动设置：localStorage `gebai.ui.lowPower` = "on"（开启）；不存/其它值 = 关闭（旧三态 "auto"/"off" 统一视为关闭）
 * - 生效方式：根元素 `data-low-power="on"`（CSS 降级动画/毛玻璃等特效）；图表导出降采样等经 isLowPower() 读取
 */

export type LowPowerSetting = "on" | "off"

const KEY = "gebai.ui.lowPower"

/** 用户设置（默认 off；旧三态存储值 "auto"/"off" 统一映射为关闭）。 */
export function getLowPowerSetting(): LowPowerSetting {
  try {
    if (localStorage.getItem(KEY) === "on") return "on"
  } catch {
    /* 隐私模式等场景忽略 */
  }
  return "off"
}

/** 当前是否处于低性能模式（纯手动：仅由用户设置决定）。 */
export function isLowPower(): boolean {
  return getLowPowerSetting() === "on"
}

/** 应用低性能模式：设置根元素 data-low-power 标记；值变化时派发事件（设置面板等刷新显示）。 */
export function applyLowPower(): void {
  const on = isLowPower()
  const el = document.documentElement
  const prev = el.dataset.lowPower
  const next = on ? "on" : undefined
  if (next) el.dataset.lowPower = next
  else delete el.dataset.lowPower
  if (prev !== next) {
    document.dispatchEvent(new CustomEvent("gebai:low-power-change", { detail: { low: on } }))
  }
}

/** 手动设置（设置面板「外观」）：on=开启；off=关闭。持久化 + 立即生效。 */
export function setLowPowerSetting(v: LowPowerSetting): void {
  try {
    if (v === "on") localStorage.setItem(KEY, v)
    else localStorage.removeItem(KEY) // off = 默认关闭，不留冗余存储
  } catch {
    /* ignore */
  }
  applyLowPower()
}

/** 初始化：应用当前设置，并跨标签页同步（其他标签修改设置后本标签即时生效）。 */
export function initLowPower(): void {
  applyLowPower()
  window.addEventListener("storage", (e) => {
    if (e.key !== KEY) return
    applyLowPower()
  })
}
