/**
 * 执行表达式解析（5 段 cron 与 @ 别名）——**全仓唯一一份**。
 *
 * 纯函数模块：不做存储与调度，时间计算全部按表达式自身语义（缺省服务器本地时区，
 * 可指定 IANA 时区）。零 node 内建依赖（只用 Date / Intl）。
 *
 * ## 为什么放在人格包而不是服务端
 *
 * 两个消费者需要一个口径：
 *
 * | 消费者 | 用它做什么 |
 * |---|---|
 * | `core/schedule/tasks.ts`（统一任务管理） | 算定时任务的下次触发点 |
 * | `persona/schedules.ts`（人格定时工作面） | 加载期校验人格声明的 cron、算档期 |
 *
 * 加载期校验与运行期计算必须是同一套语义——否则「加载期接受了，运行期算出永不触发」
 * 会表现为人格的定时任务永远不响，且没有任何报错。人格包的加载期校验不能依赖服务端
 * （那会反向依赖），所以实现落在这一侧，服务端经 `core/schedule/expr.ts` re-export 复用。
 */

/** 表达式解析结果：给定时刻之后（严格大于）的下一次执行时间。 */
export interface TaskSchedule {
  next(fromMs: number): number
}

/** 解析后的单个 cron 字段。 */
interface CronField {
  values: Set<number>
  /** 是否 * 全匹配（占满合法区间）。 */
  all: boolean
}

interface CronFieldRecord {
  minute: CronField
  hour: CronField
  dom: CronField
  month: CronField
  dow: CronField
}

const CRON_ALIASES: Record<string, string> = {
  "@hourly": "0 * * * *",
  "@daily": "0 0 * * *",
  "@midnight": "0 0 * * *",
  "@weekly": "0 0 * * 0",
  "@monthly": "0 0 1 * *",
  "@yearly": "0 0 1 1 *",
  "@annually": "0 0 1 1 *",
}

const DOW_NAMES: Record<string, number> = { Sun: 0, Mon: 1, Tue: 2, Wed: 3, Thu: 4, Fri: 5, Sat: 6 }

/** 是否一次性表达式（@at）。 */
export function isOneShotSchedule(raw: string): boolean {
  return /^@at\s/i.test(String(raw).trim())
}

function parseField(raw: string, min: number, max: number, label: string): CronField {
  const values = new Set<number>()
  const parts = raw.split(",")
  for (const part of parts) {
    const m = part.match(/^(\*|\d+)(?:-(\d+))?(?:\/(\d+))?$/)
    if (!m) throw new Error(`无效的 cron 表达式（${label} 段）: ${raw}`)
    const loRaw = m[1]
    const hiRaw = m[2]
    const stepRaw = m[3]
    const step = stepRaw ? Number(stepRaw) : 1
    if (step < 1) throw new Error(`无效的 cron 步长（${label} 段）: ${raw}`)
    if (loRaw === "*") {
      if (hiRaw) throw new Error(`无效的 cron 范围（${label} 段）: ${raw}`)
      for (let v = min; v <= max; v += step) values.add(v)
    } else {
      const lo = Number(loRaw)
      const hi = hiRaw ? Number(hiRaw) : lo
      if (lo < min || hi > max || lo > hi) throw new Error(`cron 字段越界（${label} 段）: ${raw}`)
      for (let v = lo; v <= hi; v += step) values.add(v)
    }
  }
  if (!values.size) throw new Error(`cron 字段无有效值（${label} 段）: ${raw}`)
  return { values, all: values.size === max - min + 1 }
}

/** 校验 IANA 时区名（非法抛友好错误）。 */
function validateTimezone(tz: string): void {
  try {
    new Intl.DateTimeFormat("en-US", { timeZone: tz })
  } catch {
    throw new Error(`无效的时区: ${tz}（须为 IANA 名称，如 Asia/Shanghai）`)
  }
}

/** 时区墙上时钟分量。 */
interface TzWall {
  y: number
  mo: number
  d: number
  h: number
  mi: number
  dow: number
}

function tzWall(tz: string, epochMs: number): TzWall {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: tz,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    weekday: "short",
    hour12: false,
  }).formatToParts(new Date(epochMs))
  const get = (t: string) => parts.find((p) => p.type === t)?.value ?? ""
  const h = Number(get("hour"))
  return {
    y: Number(get("year")),
    mo: Number(get("month")),
    d: Number(get("day")),
    h: h === 24 ? 0 : h,
    mi: Number(get("minute")),
    dow: DOW_NAMES[get("weekday")] ?? 0,
  }
}

/** 时区在某时刻的 UTC 偏移（毫秒）。 */
function tzOffsetMs(tz: string, epochMs: number): number {
  const w = tzWall(tz, epochMs)
  return Date.UTC(w.y, w.mo - 1, w.d, w.h, w.mi) - epochMs
}

/** 时区墙上时间 → epoch（两次偏移校正，兼容 DST 切换）。 */
function wallToEpochMs(w: { y: number; mo: number; d: number; h: number; mi: number }, tz: string): number {
  const guess = Date.UTC(w.y, w.mo - 1, w.d, w.h, w.mi)
  let t = guess - tzOffsetMs(tz, guess)
  const off2 = tzOffsetMs(tz, t)
  if (off2 !== guess - t) t = guess - off2
  return t
}

/** 经典 cron 语义：日与周均受限时任一命中即可（OR），否则须同时命中（AND）。 */
function dayMatches(f: CronFieldRecord, month: number, dom: number, dow: number, domRestricted: boolean, dowRestricted: boolean): boolean {
  if (!f.month.values.has(month)) return false
  const domMatch = f.dom.values.has(dom)
  const dowMatch = f.dow.values.has(dow) || f.dow.values.has(dow + 7)
  return domRestricted && dowRestricted ? domMatch || dowMatch : domMatch && dowMatch
}

/** 5 段 cron 的下次执行时间：按天扫描（本地时区，跨 DST 由 Date 构造器处理），最多扫 5 年。 */
function nextCronTime(f: CronFieldRecord, fromMs: number, raw: string): number {
  const start = new Date(fromMs)
  const domRestricted = !f.dom.all
  const dowRestricted = !f.dow.all
  const hours = [...f.hour.values].sort((a, b) => a - b)
  const minutes = [...f.minute.values].sort((a, b) => a - b)
  const MAX_DAYS = 366 * 5
  for (let i = 0; i < MAX_DAYS; i++) {
    const d0 = new Date(start.getFullYear(), start.getMonth(), start.getDate() + i)
    if (!dayMatches(f, d0.getMonth() + 1, d0.getDate(), d0.getDay(), domRestricted, dowRestricted)) continue
    for (const hour of hours) {
      for (const minute of minutes) {
        const ts = new Date(d0.getFullYear(), d0.getMonth(), d0.getDate(), hour, minute).getTime()
        if (ts > fromMs) return ts
      }
    }
  }
  throw new Error(`无法计算下次执行时间（cron 表达式可能永不触发）: ${raw}`)
}

/** 指定时区的下次执行时间：墙上时钟按天扫描（UTC 域日历推进 + 双次偏移换算，跨 DST 正确）。 */
function nextCronTimeTz(f: CronFieldRecord, fromMs: number, raw: string, tz: string): number {
  const start = tzWall(tz, fromMs)
  const domRestricted = !f.dom.all
  const dowRestricted = !f.dow.all
  const hours = [...f.hour.values].sort((a, b) => a - b)
  const minutes = [...f.minute.values].sort((a, b) => a - b)
  const MAX_DAYS = 366 * 5
  for (let i = 0; i < MAX_DAYS; i++) {
    const cur = new Date(Date.UTC(start.y, start.mo - 1, start.d + i))
    if (!dayMatches(f, cur.getUTCMonth() + 1, cur.getUTCDate(), cur.getUTCDay(), domRestricted, dowRestricted)) continue
    for (const hour of hours) {
      for (const minute of minutes) {
        const ts = wallToEpochMs({ y: cur.getUTCFullYear(), mo: cur.getUTCMonth() + 1, d: cur.getUTCDate(), h: hour, mi: minute }, tz)
        if (ts > fromMs) return ts
      }
    }
  }
  throw new Error(`无法计算下次执行时间（cron 表达式可能永不触发）: ${raw}`)
}

/** 解析执行表达式：5 段 cron（分 时 日 月 周，按 timezone 指定时区、缺省本地）或
 *  @daily/@hourly/@weekly/@monthly/@every <n>s|m|h|d/@at <时间>（一次性，入队后自动停用）。 */
export function parseSchedule(raw: string, timezone?: string): TaskSchedule {
  const s = String(raw).trim()
  if (!s) throw new Error("定时表达式不能为空")
  if (timezone) validateTimezone(timezone)
  if (s.startsWith("@")) {
    const alias = CRON_ALIASES[s.toLowerCase()]
    if (alias) return parseSchedule(alias, timezone)
    const every = s.match(/^@every\s+(\d+)\s*(s|m|h|d)$/i)
    if (every) {
      const n = Number(every[1])
      if (n < 1) throw new Error(`无效的定时间隔: ${raw}`)
      const unitMs: Record<string, number> = { s: 1000, m: 60_000, h: 3_600_000, d: 86_400_000 }
      const interval = n * unitMs[every[2].toLowerCase()]
      return { next: (fromMs) => fromMs - (fromMs % interval) + interval }
    }
    const at = s.match(/^@at\s+(.+)$/i)
    if (at) {
      const normalized = at[1].trim().replace(" ", "T")
      const t = Date.parse(normalized)
      if (!Number.isFinite(t)) throw new Error(`无效的 @at 时间: ${raw}（示例 @at 2026-09-01T09:00）`)
      return { next: () => t }
    }
    throw new Error(`无效的定时表达式: ${raw}（支持 5 段 cron 如 "0 9 * * *"、@daily/@hourly/@weekly/@monthly、@every 30m、@at 2026-09-01T09:00）`)
  }
  const parts = s.split(/\s+/)
  if (parts.length !== 5) throw new Error(`无效的 cron 表达式: ${raw}（需要 5 段: 分 时 日 月 周）`)
  const fields: CronFieldRecord = {
    minute: parseField(parts[0]!, 0, 59, "分"),
    hour: parseField(parts[1]!, 0, 23, "时"),
    dom: parseField(parts[2]!, 1, 31, "日"),
    month: parseField(parts[3]!, 1, 12, "月"),
    dow: parseField(parts[4]!, 0, 7, "周"),
  }
  if (timezone) return { next: (fromMs) => nextCronTimeTz(fields, fromMs, raw, timezone) }
  return { next: (fromMs) => nextCronTime(fields, fromMs, raw) }
}
