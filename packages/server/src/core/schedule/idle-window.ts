/**
 * 闲时任务执行时间段（DESIGN「统一任务管理」）：把"什么时候算闲时"从
 * 「队列空闲即可」细化到「**同时**落在指定时间段内」。
 *
 * 为什么需要：闲时任务的意义是利用**便宜**的算力与谷价 token（夜间、周末、
 * 本地机器没人用的时候）。只判"队列空闲"会在白天有人排队时也抢着跑，
 * 既抢用户交互会话的额度，又白付峰价 token。时间段让用户直接说
 * 「每天 22:00 之后跑」「周末跑」。
 *
 * 语义与边界（都是踩过的坑，写在这里免得日后逐条重新推）：
 * - `start < end`：同一天内的窗口（如 09:00–18:00）；
 * - `start > end`：**跨零点**窗口（如 22:00–06:00）——按设计就是最常见的用法；
 * - `start === end`：视为无约束（全天）。否则这个窗口要么空、要么全天，
 *   没有第三种解释，取"全天"更符合用户直觉（填错也不至于任务永不执行）；
 * - `days` 指窗口**开始那一天**的星期（跨零点窗口的凌晨部分属于前一天开的那一场）；
 * - 时区：`timezone` 缺省服务器本地时区，与定时任务的 `Task.timezone` 同口径。
 *
 * 纯函数模块：不做存储、不读环境，时间来源一律由调用方传入（便于测试与回放）。
 */

/** 星期常量（与 `Date.getDay()` 一致：0=周日）。 */
export const IDLE_WEEK_DAYS = [0, 1, 2, 3, 4, 5, 6] as const

/** 闲时执行时间段：`days` 空/缺省 = 每天；`start`/`end` 缺省 = 全天。 */
export interface IdleWindow {
  /** 允许执行的星期（窗口开始那天，0=周日）。缺省 = 每天。 */
  days?: number[]
  /** 窗口开始时刻 "HH:MM"。 */
  start: string
  /** 窗口结束时刻 "HH:MM"；小于 start 表示跨零点。 */
  end: string
  /** IANA 时区（缺省服务器本地时区）。 */
  timezone?: string
}

/** 解析后的时刻（分钟数，0~1439）。 */
const MINUTES_PER_DAY = 24 * 60

/** "HH:MM" → 分钟数；非法返回 null。 */
export function parseClock(raw: string): number | null {
  const m = /^(\d{1,2}):(\d{2})$/.exec(String(raw).trim())
  if (!m) return null
  const h = Number(m[1])
  const mi = Number(m[2])
  if (!Number.isInteger(h) || !Number.isInteger(mi) || h < 0 || h > 23 || mi < 0 || mi > 59) return null
  return h * 60 + mi
}

/** 分钟数 → "HH:MM"（补零，供展示）。 */
export function formatClock(minutes: number): string {
  const m = ((minutes % MINUTES_PER_DAY) + MINUTES_PER_DAY) % MINUTES_PER_DAY
  return `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`
}

/**
 * 校验并归一化闲时时间段；非法抛错（调用方转成用户可见的 400）。
 * 归一化会丢掉重复星期、按升序排列，便于落盘后 diff 稳定。
 */
export function normalizeIdleWindow(raw: unknown): IdleWindow | undefined {
  if (raw === undefined || raw === null) return undefined
  if (typeof raw !== "object") throw new Error("idleWindow 须为对象：{ days?, start, end, timezone? }")
  const w = raw as Record<string, unknown>
  const start = typeof w.start === "string" ? w.start.trim() : ""
  const end = typeof w.end === "string" ? w.end.trim() : ""
  if (!start || !end) throw new Error("idleWindow 需要 start 与 end（格式 HH:MM）")
  if (parseClock(start) === null) throw new Error(`idleWindow.start 非法: ${start}（格式 HH:MM，00:00~23:59）`)
  if (parseClock(end) === null) throw new Error(`idleWindow.end 非法: ${end}（格式 HH:MM，00:00~23:59）`)
  let days: number[] | undefined
  if (w.days !== undefined) {
    const list = Array.isArray(w.days) ? w.days : [w.days]
    const set = new Set<number>()
    for (const d of list) {
      const n = Number(d)
      if (!Number.isInteger(n) || n < 0 || n > 6) throw new Error(`idleWindow.days 含非法星期: ${String(d)}（0~6，0=周日）`)
      set.add(n)
    }
    // 空数组 = 每天（与缺省同义）；全选也归一成"每天"，避免两套等价写法在数据里并存
    days = set.size === 0 || set.size === 7 ? undefined : [...set].sort((a, b) => a - b)
  }
  let timezone: string | undefined
  if (w.timezone !== undefined && w.timezone !== null && String(w.timezone).trim()) {
    timezone = String(w.timezone).trim()
    try {
      new Intl.DateTimeFormat("en-US", { timeZone: timezone })
    } catch {
      throw new Error(`idleWindow.timezone 非法: ${timezone}（须为 IANA 名称，如 Asia/Shanghai）`)
    }
  }
  return { ...(days ? { days } : {}), start: formatClock(parseClock(start)!), end: formatClock(parseClock(end)!), ...(timezone ? { timezone } : {}) }
}

/** 某时刻在指定时区（缺省本地）的墙上时钟分量。 */
function wallClock(epochMs: number, timezone?: string): { minutes: number; dow: number } {
  const d = new Date(epochMs)
  if (!timezone) {
    return { minutes: d.getHours() * 60 + d.getMinutes(), dow: d.getDay() }
  }
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: timezone,
    hour: "2-digit",
    minute: "2-digit",
    weekday: "short",
    hour12: false,
  }).formatToParts(d)
  const get = (t: string) => parts.find((p) => p.type === t)?.value ?? ""
  const h = Number(get("hour")) % 24
  const names: Record<string, number> = { Sun: 0, Mon: 1, Tue: 2, Wed: 3, Thu: 4, Fri: 5, Sat: 6 }
  return { minutes: h * 60 + Number(get("minute")), dow: names[get("weekday")] ?? 0 }
}

/** 窗口长度（分钟）；跨零点按加一天算。 */
export function windowLengthMinutes(w: Pick<IdleWindow, "start" | "end">): number {
  const s = parseClock(w.start)!
  const e = parseClock(w.end)!
  if (s === e) return MINUTES_PER_DAY
  return e > s ? e - s : MINUTES_PER_DAY - s + e
}

/** 该窗口是否"全天无约束"（每天 + 00:00–00:00）；用于省略展示与判定短路。 */
export function isAlwaysOpen(w?: IdleWindow | null): boolean {
  if (!w) return true
  return windowLengthMinutes(w) >= MINUTES_PER_DAY
}

/**
 * 判定某时刻是否落在窗口内。
 * `days` 语义：**窗口开始日**；跨零点窗口的凌晨部分归前一天那一场。
 */
export function isInIdleWindow(w: IdleWindow | undefined | null, atMs: number): boolean {
  if (!w) return true
  if (isAlwaysOpen(w)) return true
  const { minutes, dow } = wallClock(atMs, w.timezone)
  const s = parseClock(w.start)!
  const e = parseClock(w.end)!
  const days = w.days
  const dayOk = (d: number) => !days || days.includes(((d % 7) + 7) % 7)
  if (s < e) {
    // 同日窗口：要求"今天"在允许列表里
    return minutes >= s && minutes < e && dayOk(dow)
  }
  // 跨零点：凌晨段（< e）算作"昨天开的那一场"
  if (minutes < e) return dayOk(dow - 1)
  return minutes >= s && dayOk(dow)
}

/**
 * 下一次进入窗口的时刻（严格大于 `fromMs`；永不进入返回 null）。
 * 用于 UI 文案「下次 22:00 开始」与调度器的等待说明。
 */
export function nextIdleWindowStart(w: IdleWindow | undefined | null, fromMs: number): number | null {
  if (!w || isAlwaysOpen(w)) return fromMs
  // 已在窗内：直接返回当前时刻（调用方据此判「现在就能跑」，不会凭空等到下一分钟）
  if (isInIdleWindow(w, fromMs)) return fromMs
  // 窗口最短 1 分钟，因此逐分钟推进上限 8 天足够覆盖跨星期与 DST；先对齐到下一整分钟避免无效循环
  const startMinute = Math.floor(fromMs / 60_000) * 60_000 + 60_000
  const LIMIT = 8 * MINUTES_PER_DAY
  for (let i = 0; i < LIMIT; i++) {
    const t = startMinute + i * 60_000
    if (isInIdleWindow(w, t)) return t
  }
  return null
}

/** 人类可读描述（UI 与日志共用）：如「每天 22:00–06:00（跨零点）」「周六/周日 09:00–18:00」。 */
export function describeIdleWindow(w?: IdleWindow | null): string {
  if (!w || isAlwaysOpen(w)) return "不限时段（队列空闲即执行）"
  const names = ["周日", "周一", "周二", "周三", "周四", "周五", "周六"]
  const dayText = !w.days || w.days.length === 7 ? "每天" : w.days.map((d) => names[d] ?? `周${d}`).join("/")
  const cross = parseClock(w.start)! > parseClock(w.end)!
  return `${dayText} ${w.start}–${w.end}${cross ? "（跨零点）" : ""}${w.timezone ? ` · ${w.timezone}` : ""}`
}
