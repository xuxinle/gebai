/**
 * 页面不可见时段的计时（浏览器后台 / 手机切 App / 锁屏）：
 * 不可见期间定时器被冻结或重度节流，页面自身无从判定「服务端是否还在跑」——这段时长必须从
 * 「空闲多久」里扣除，否则切回前台时会把整段后台时间算作无数据挂起。
 *
 * 用法：空闲看门狗判定前先问 `isPageHidden()`（隐藏中一切不发），再以
 * `effectiveIdleMs(lastActivity)` 与阈值比较。
 *
 * 按区间求值（而非单一累计量）：`from` 可能晚于某些已结束的隐藏时段，只有与 [from, now]
 * 重叠的隐藏时长才该扣——用累计总量去减会把更早的后台时间也算进来，扣成负数、看门狗永不触发。
 *
 * 懒结算：隐藏时长在**查询时求值**，不依赖「回前台先跑 visibilitychange 回调」——页面恢复
 * 瞬间被冻结的定时器可能先于 visibilitychange 回调执行，先结算就会把整段后台当成活跃缺口。
 */

/** 一段不可见时间（`end` 为 null 表示仍在进行中）。按 `start` 升序，进行中的时段必在末尾。 */
interface HiddenInterval {
  start: number
  end: number | null
}

/** 时段条数上限：仅防长页面（数天不刷新 + 反复前台/后台）无界增长；正常使用远低于此，不触发裁剪。
 *  不按时间窗口裁：仍在相关区间内的旧时段也会被误丢（超长任务的可见时长就会算错）。 */
const MAX_INTERVALS = 512

let intervals: HiddenInterval[] = []

/** 页面当前是否不可见（后台标签页 / 最小化 / 锁屏）。无 document 环境按可见处理。 */
export function isPageHidden(): boolean {
  if (typeof document === "undefined") return false
  return document.hidden === true || document.visibilityState === "hidden"
}

/** 开始一段不可见时段（幂等：已有进行中的时段则不重复登记）。 */
export function markHidden(now: number = Date.now()): void {
  const last = intervals.at(-1)
  if (last && last.end === null) return
  intervals.push({ start: now, end: null })
  prune()
}

/** 结束当前进行中的不可见时段（无进行中的时段时 no-op）。 */
export function markVisible(now: number = Date.now()): void {
  const last = intervals.at(-1)
  if (!last || last.end !== null) return
  last.end = Math.max(last.start, now)
}

/** 截止时刻 `t` 为止累计的不可见时长（毫秒）。 */
function hiddenUpTo(t: number, now: number): number {
  let sum = 0
  for (const iv of intervals) {
    if (iv.start >= t) break
    sum += Math.min(iv.end ?? now, t) - iv.start
  }
  return sum
}

/** 超出上限时丢弃最早的已结束时段（进行中的始终保留）。 */
function prune(): void {
  if (intervals.length <= MAX_INTERVALS) return
  const excess = intervals.length - MAX_INTERVALS
  let dropped = 0
  intervals = intervals.filter((iv) => {
    if (dropped >= excess || iv.end === null) return true
    dropped += 1
    return false
  })
}

/**
 * 时间戳 `from` 到「现在」之间的**可见时长**（毫秒）：墙钟间隔扣除与之重叠的不可见时段。
 * 进行中的隐藏时段按与 [from, now] 的重叠部分扣（不必等它结束）。
 */
export function visibleElapsedMs(from: number, now: number = Date.now()): number {
  const span = Math.max(0, now - from)
  const hidden = hiddenUpTo(now, now) - hiddenUpTo(from, now)
  return Math.max(0, span - Math.max(0, hidden))
}

/** 距 `lastActivity` 的**有效空闲时长**（毫秒）：不可见期间不计时。 */
export function effectiveIdleMs(lastActivity: number, now: number = Date.now()): number {
  return visibleElapsedMs(lastActivity, now)
}

/** 订阅可见性变化：页面变为可见时回调（等待中的逻辑可据此复核状态）。返回退订函数。 */
export function onPageVisible(cb: () => void): () => void {
  if (typeof document === "undefined") return () => {}
  const handler = () => {
    if (!isPageHidden()) cb()
  }
  document.addEventListener("visibilitychange", handler)
  return () => document.removeEventListener("visibilitychange", handler)
}

/** 绑定可见性事件（模块初始化时调用一次）：维护隐藏时段起止。 */
export function initPageIdle(): void {
  if (typeof document === "undefined") return
  if (isPageHidden()) markHidden()
  document.addEventListener("visibilitychange", () => {
    if (isPageHidden()) markHidden()
    else markVisible()
  })
}

/** 清空计时状态并按当前可见态重新起算（测试与「重新接管计时段」用）。 */
export function resetPageIdle(now: number = Date.now()): void {
  intervals = isPageHidden() ? [{ start: now, end: null }] : []
}
