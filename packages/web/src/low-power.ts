/**
 * 低性能模式：无 GPU / 低配机器自动开启，可手动强制开启（设置面板「外观」）。
 * - 自动检测（默认）：WebGL 不可用（GPU 缺失/驱动禁用）、CPU ≤4 核、内存 ≤4GB（Chrome/Edge 支持 deviceMemory），任一命中即开启
 * - 手动覆盖：localStorage `gebai.ui.lowPower` = "on"（强制开启）；不存/其它值 = 自动检测（旧三态 "auto"/"off" 兼容映射为自动检测）
 * - 生效方式：根元素 `data-low-power="on"`（CSS 降级动画/毛玻璃等特效）；PlantUML 渲染降采样经 isLowPower() 读取
 */

export type LowPowerSetting = "on" | "off"

const KEY = "gebai.ui.lowPower"

/** 探测 WebGL 是否可用（无 GPU 或驱动禁用时为 false）。结果缓存：页面生命周期内可用性不变。 */
let webglProbe: boolean | null = null
export function probeWebGL(): boolean {
  if (webglProbe !== null) return webglProbe
  try {
    const c = document.createElement("canvas")
    const gl = c.getContext("webgl") || c.getContext("experimental-webgl" as "webgl")
    webglProbe = !!gl
  } catch {
    webglProbe = false
  }
  return webglProbe
}

/** 纯判定函数（可测试）：无 WebGL / 核数 ≤4 / 内存 ≤4GB，任一命中即低性能。 */
export function detectLowPower(signals: { hasWebGL: boolean; cores?: number; memoryGb?: number }): boolean {
  if (!signals.hasWebGL) return true
  if (signals.cores !== undefined && signals.cores > 0 && signals.cores <= 4) return true
  if (signals.memoryGb !== undefined && signals.memoryGb > 0 && signals.memoryGb <= 4) return true
  return false
}

/** 用户设置（默认 off=自动检测；旧三态存储值 "auto"/"off" 统一映射为自动检测）。 */
export function getLowPowerSetting(): LowPowerSetting {
  try {
    if (localStorage.getItem(KEY) === "on") return "on"
  } catch {
    /* 隐私模式等场景忽略 */
  }
  return "off"
}

/** 当前是否处于低性能模式（设置 on 强制开启；off 按自动检测）。 */
export function isLowPower(): boolean {
  if (getLowPowerSetting() === "on") return true
  return detectLowPower({
    hasWebGL: probeWebGL(),
    cores: navigator.hardwareConcurrency,
    memoryGb: (navigator as Navigator & { deviceMemory?: number }).deviceMemory,
  })
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

/** 手动设置（设置面板「外观」）：on=强制开启；off=恢复自动检测。持久化 + 立即生效。 */
export function setLowPowerSetting(v: LowPowerSetting): void {
  try {
    if (v === "on") localStorage.setItem(KEY, v)
    else localStorage.removeItem(KEY) // off = 默认（自动检测），不留冗余存储
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
    // 设置已变（其他标签）：即使 data-low-power 结果未变（如 on→自动且检测仍开启）也通知 UI 刷新
    document.dispatchEvent(new CustomEvent("gebai:low-power-change", { detail: { low: isLowPower() } }))
  })
}
