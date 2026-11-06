/** 闲时执行时间段的**前端纯逻辑**（零 DOM，独立单测）：文案、表单值 ↔ IdleWindow、快捷预设。
 *
 * 为什么这些必须在共享模块里而不是散在面板代码中：三处都要用同一套口径
 * （待办弹窗、任务面板、偏好设置），三份手写必然漂移——一处把"跨零点"标成"同日"、
 * 另一处把 `start === end` 当空窗口，用户看到的就是互相矛盾的解释。
 *
 * **判定"现在是否在时段内"不在前端做**：那是服务端的职责（时钟、时区、调度都在那边），
 * 前端只展示后端给的 `queueView().idle`（blocked 原因 / nextOpenAt）。这里只做
 * "把结构化时段渲染成人话"与"把人填的表单拼成 IdleWindow"。
 */
import type { IdleWindow } from "@gebai/sdk"

/** 星期文案（0=周日，与 Date.getDay() 一致）。 */
export const WEEKDAY_LABELS = ["周日", "周一", "周二", "周三", "周四", "周五", "周六"] as const

/** 把 "HH:MM" 解析为分钟数；非法返回 null（与服务端同规则，前端只用于本地校验收敛输入）。 */
export function parseClock(raw: string): number | null {
  const m = /^(\d{1,2}):(\d{2})$/.exec(String(raw ?? "").trim())
  if (!m) return null
  const h = Number(m[1])
  const mi = Number(m[2])
  if (!Number.isInteger(h) || !Number.isInteger(mi) || h < 0 || h > 23 || mi < 0 || mi > 59) return null
  return h * 60 + mi
}

/** 分钟数 → "HH:MM"（补零）。 */
export function formatClock(minutes: number): string {
  const m = ((minutes % 1440) + 1440) % 1440
  return `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`
}

/** 是否跨零点（start > end）；start === end 视为全天，不算跨零点。 */
export function isCrossMidnight(w: Pick<IdleWindow, "start" | "end">): boolean {
  const s = parseClock(w.start)
  const e = parseClock(w.end)
  return s !== null && e !== null && s > e
}

/** 是否"全天/不限时段"（start === end）。 */
export function isAlwaysOpen(w?: Pick<IdleWindow, "start" | "end"> | null): boolean {
  if (!w) return true
  const s = parseClock(w.start)
  const e = parseClock(w.end)
  return s !== null && s === e
}

/**
 * 人话描述：`每天 22:00–06:00（跨零点）` / `周六/周日 09:00–18:00` / `不限时段`。
 * 星期按数字升序（与服务端归一化后的存储顺序一致，两边显示不会一顺一逆）。
 */
export function describeIdleWindow(w?: IdleWindow | null): string {
  if (!w || isAlwaysOpen(w)) return "不限时段（空闲即跑）"
  const dayText = !w.days || w.days.length === 7 ? "每天" : w.days.map((d) => WEEKDAY_LABELS[d] ?? `周${d}`).join("/")
  return `${dayText} ${w.start}–${w.end}${isCrossMidnight(w) ? "（跨零点）" : ""}`
}

/** 表单值（全部字符串：直接绑 input，空白表示不填）。 */
export interface IdleWindowForm {
  /** 是否启用时段（false = 不限时段）。 */
  enabled: boolean
  start: string
  end: string
  /** 选中的星期（0~6）；空数组 = 每天。 */
  days: number[]
  /** IANA 时区（空 = 服务器本地）。 */
  timezone: string
}

/** 空表单（不限时段）。 */
export function emptyIdleWindowForm(): IdleWindowForm {
  return { enabled: false, start: "22:00", end: "06:00", days: [], timezone: "" }
}

/** IdleWindow → 表单值（未设时段 = enabled: false，但把常用值预填好，用户一开就能用）。 */
export function formFromIdleWindow(w?: IdleWindow | null): IdleWindowForm {
  const base = emptyIdleWindowForm()
  if (!w) return base
  return {
    enabled: true,
    start: w.start || base.start,
    end: w.end || base.end,
    days: [...(w.days ?? [])].sort((a, b) => a - b),
    timezone: w.timezone ?? "",
  }
}

/**
 * 表单值 → IdleWindow（校验失败返回 `{ error }`，调用方把 error 显示给用户）。
 * `enabled: false` 返回 `{ value: null }` —— 语义是"清除时段"，调用方传 null 给后端。
 */
export function idleWindowFromForm(f: IdleWindowForm): { value: IdleWindow | null; error?: string } {
  if (!f.enabled) return { value: null }
  const s = parseClock(f.start)
  if (s === null) return { value: null, error: `开始时刻非法：${f.start}（格式 HH:MM，00:00~23:59）` }
  const e = parseClock(f.end)
  if (e === null) return { value: null, error: `结束时刻非法：${f.end}（格式 HH:MM，00:00~23:59）` }
  const days = [...new Set(f.days)].filter((d) => Number.isInteger(d) && d >= 0 && d <= 6).sort((a, b) => a - b)
  const tz = f.timezone.trim()
  return {
    value: {
      start: formatClock(s),
      end: formatClock(e),
      // 空数组 = 每天（不落 days，避免"每天"出现两套等价写法）
      ...(days.length && days.length !== 7 ? { days } : {}),
      ...(tz ? { timezone: tz } : {}),
    },
  }
}

/** 快捷预设（覆盖最常见的几种"谷价/夜间"用法）。 */
export interface IdlePreset {
  key: string
  label: string
  window: IdleWindow
}

export const IDLE_PRESETS: IdlePreset[] = [
  { key: "night", label: "每天夜间 22:00–06:00", window: { start: "22:00", end: "06:00" } },
  { key: "late-night", label: "每天深夜 00:00–07:00", window: { start: "00:00", end: "07:00" } },
  { key: "weekend", label: "周末 09:00–22:00", window: { days: [0, 6], start: "09:00", end: "22:00" } },
  { key: "off-hours", label: "工作日夜间 19:00–08:00", window: { days: [1, 2, 3, 4, 5], start: "19:00", end: "08:00" } },
]

/** 命中哪个预设（用于表单回显；未命中返回 null）。 */
export function matchPreset(w?: IdleWindow | null): IdlePreset | null {
  if (!w) return null
  const norm = (x?: IdleWindow | null): string =>
    x ? `${(x.days ?? []).join(",")}|${x.start}|${x.end}|${x.timezone ?? ""}` : ""
  const target = norm(w)
  return IDLE_PRESETS.find((p) => norm(p.window) === target) ?? null
}
