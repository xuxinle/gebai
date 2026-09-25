/**
 * 输出速率（tok/s）纯逻辑：展示格式与路况分级（无 DOM/无状态，便于单测）。
 *
 * 速率的**唯一数据源是服务端**（`event.session.tps`）：服务端在一次模型调用内按节流周期推送
 * 生成中的估算帧（`est: true, active: true`），调用结束时推送收尾帧（接口 usage 真值时为
 * `est: false`，接口不返回 usage 时仍为估算）。前端只做展示，不自算速率——两套口径并存会让
 * 数字来回跳，也说不清当前看到的是谁的量。
 *
 * 帧字段：
 * - `tps`：输出 tokens ÷ 生成窗口（tokens/秒）
 * - `outTokens`：窗口内输出 tokens（估算帧为服务端字符折算，收尾帧为接口 usage）
 * - `genMs`：生成窗口（首个输出 chunk 到末个输出 chunk，不含首 token 等待）
 * - `est`：true = 估算口径（颜色弱化展示），false = 接口 usage 实测
 * - `active`：true = 本次生成仍在进行（收尾帧为 false）
 */

/** 服务端推送的速率帧（`event.session.tps` payload）。 */
export interface TokenRatePayload {
  /** 输出 tokens ÷ 生成窗口（tokens/秒）。 */
  tps: number
  /** 该速率对应的输出 tokens。 */
  outTokens: number
  /** 该速率对应的生成窗口（毫秒）。 */
  genMs: number
  /** 估算口径（服务端字符折算）还是接口 usage 实测。 */
  est: boolean
  /** 本次生成是否仍在进行。 */
  active: boolean
}

/**
 * 速率分级（导航交通拥挤配色口径，六级从畅通到严重拥堵）：阈值按 tokens/秒。
 * 展示时映射为 --rate-color（绿→黄→红），级名随 data-tip 给出，纯色语义与导航路况同构。
 */
export interface RateBand {
  /** 等级（1=畅通…6=严重拥堵），即根元素/CSS 的 data-rate 取值。 */
  level: 1 | 2 | 3 | 4 | 5 | 6
  /** 该级下限（tokens/秒，含）；等级 6 为 0。 */
  min: number
  /** 级名（中文路况用语）。 */
  label: string
}

/** 速率分级表（从高到低判定，第一项命中即用）。 */
export const RATE_BANDS: readonly RateBand[] = [
  { level: 1, min: 80, label: "畅通" },
  { level: 2, min: 50, label: "顺畅" },
  { level: 3, min: 30, label: "良好" },
  { level: 4, min: 18, label: "缓行" },
  { level: 5, min: 8, label: "拥堵" },
  { level: 6, min: 0, label: "严重拥堵" },
]

/** 速率分级（越快越绿，与导航地图的速度配色同口径）。 */
export function rateLevel(tps: number): RateBand {
  for (const band of RATE_BANDS) {
    if (tps >= band.min) return band
  }
  return RATE_BANDS[RATE_BANDS.length - 1]!
}

/** 速率文本：<100 保留一位小数（42.3），≥100 取整（数字位数不再增长，避免标题栏宽度抖动）。 */
export function formatTps(tps: number): string {
  if (!Number.isFinite(tps) || tps <= 0) return "0"
  if (tps >= 100) return String(Math.round(tps))
  return (Math.round(tps * 10) / 10).toFixed(1)
}
