/**
 * 授权提醒的判定逻辑（纯函数，零依赖，可独立单测）。
 *
 * 从 `compose.ts` 的 `personaExpiryTimer` 里抽出来的唯一理由：
 * **这段判定决定什么时候叫醒用户，错了代价很具体**。
 *
 * - 判成「只在过期后叫」→ 用户看到弹窗时，刚才那段时间的消息已经没人回了
 * - 判成「永远叫」→ 每 5 分钟弹一次，用户很快学会无视它（那比不提醒更糟）
 * - 忘了「卡过期后要重推」→ 用户错过 10 分钟就永远收不到提醒（实测踩过）
 *
 * 定时器里只剩「读状态 → 调判定 → 执行动作」，判定本身在这里被用例钉死。
 */

/** 提前多少毫秒开始提醒。 */
export const WARN_BEFORE_MS = 10 * 60_000

/** 授权卡在桌面上停留多久（链接本身 10 分钟失效）。 */
export const AUTH_CARD_TTL_MS = 10 * 60_000

/**
 * 卡片过期判定的宽限：控制器剔过期卡用它自己的时钟，
 * 服务若比它慢一点，卡可能已被剔除而我们仍以为在。提前 30 秒当它过期。
 */
export const AUTH_CARD_SKEW_MS = 30_000

export interface ExpiryDecision {
  /** 该不该把授权卡推到桌面上。 */
  needCard: boolean
  /**
   * 用什么状态描述当前凭据。
   *
   * `expiring` 与 `expired` 的区别是**文案责任**：
   * - `expiring`：现在还能发，但马上不行 → 文案说「即将失效」
   * - `expired`：已经不能发了 → 文案要说清「刚才那段时间发不出去」
   *
   * 混用会让用户以为消息丢了、或以为还来得及。
   */
  state: "ok" | "expiring" | "expired"
}

/**
 * 该不该推授权卡。
 *
 * `ready` 为真但剩余时间在警告窗口内也算「要提醒」——
 * 这是没有 `offline_access` 时的关键：飞书 user_access_token 最长 2 小时
 * 且不可续期，等过期再叫就已经晚了。
 */
export function decideExpiry(ready: boolean, remainingMs: number | undefined): ExpiryDecision {
  if (!ready) return { needCard: true, state: "expired" }
  // 拿不到剩余时间时不要乱叫：宁可少提醒一次，也不要周期性地打扰
  if (remainingMs === undefined || !Number.isFinite(remainingMs)) return { needCard: false, state: "ok" }
  if (remainingMs <= WARN_BEFORE_MS) return { needCard: true, state: "expiring" }
  return { needCard: false, state: "ok" }
}

/**
 * 现在该不该**新推**一张卡（`cardId` 为 null 表示当前没有卡在桌面上）。
 *
 * 这里的核心是「过期后允许重推」：卡只有 10 分钟 TTL，而 `cardId`
 * 不会自己变 null。只判 `cardId !== null` 的写法会让「错过 10 分钟」
 * 变成「永远收不到提醒」——这是实测踩到的 bug，必须在用例里钉住。
 */
export function shouldPushAuthCard(
  cardId: string | null,
  cardExpiresAt: number,
  now: number,
): boolean {
  if (cardId === null) return true
  return now >= cardExpiresAt
}

/**
 * 推卡成功后算出这张卡「对我们来说」的过期时刻。
 *
 * 减掉宽限期：宁可早推一次（桌面上短暂两张同样的卡），
 * 也不要在卡已消失后还等着——那才是用户真正收不到提醒的情形。
 */
export function authCardDeadline(now: number): number {
  return now + AUTH_CARD_TTL_MS - AUTH_CARD_SKEW_MS
}
