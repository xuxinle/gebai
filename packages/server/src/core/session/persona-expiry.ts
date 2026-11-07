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
 *
 * ## 但重推必须有上限（否则变成永久骚扰）
 *
 * 真实故障：飞书回调地址没登记（错误码 20029），授权**永远不可能成功**。
 * 而凭据状态永远是 `expired` → 每 10 分钟又推一张卡 → 用户点了又报错，
 * 无限循环。这种「注定失败的重试」比不提醒更糟：用户很快学会无视所有弹卡，
 * 包括以后真的需要他处理的那种。
 *
 * 所以引入 `consecutiveFailed`（连续失败次数）：超过上限就不再自动重推，
 * 只在日志/面板提示——让人在**修好配置后自己**触发一次（面板按钮）。
 * 为什么是“人手动”而不是“过更久再试”：回调地址配错这类问题是**确定性**的，
 * 等再久也不会自己好；继续自动重试只是继续打扰。
 *
 * @param consecutiveFailed 连续失败次数（0 = 没失败过；由调用方维护）
 */
export function shouldPushAuthCard(
  cardId: string | null,
  cardExpiresAt: number,
  now: number,
  consecutiveFailed = 0,
): boolean {
  // 连续失败达上限 → 不再自动推（用户在面板上手动触发不受此限）
  if (consecutiveFailed >= MAX_AUTO_PUSH_FAILURES) return false
  if (cardId === null) return true
  return now >= cardExpiresAt
}

/**
 * 自动重推的失败上限（超过就需要人来修配置/手动触发）。
 *
 * 取 3 的依据：第一张卡用户可能没看见（相期没开），第二张可能是他没空，
 * 第三张还失败基本就是确定性问题（配置错、授权被撤）——此时继续推只是噪音。
 */
export const MAX_AUTO_PUSH_FAILURES = 3

/**
 * 回调地址的预检：只报**地址本身的形态问题**，不替用户判断“你会从哪里授权”。
 *
 * ## 为何不能把 localhost 一律判死（上一版这里错了）
 *
 * 上一版把回环地址直接当成“结构性不可能成功”。实测发现这个推断是**过度自信**：
 *
 * 飞书 OAuth 的重定向发生在**发起授权的那台设备**上。如果用户就在本机浏览器里
 * 点开授权链接（而不是手机扫码），`http://localhost:3000/...` 会回到**本机**——
 * 完全可行，而且是本地部署最省事的形态。
 *
 * 把 localhost 判死会导致更严重的后果：**把本来能用的部署拦死**（不推卡、
 * 只说“必须配公网”——而用户根本不需要公网）。
 *
 * 所以现在的态度：地址合法就放行，只在**日志里**提醒一句
 * “回环地址仅在你就用这台机器的浏览器授权时可行”，让人自己判断而不是被拦。
 *
 * @returns 真正的错误（空数组 = 可以用）；回环地址不再计为错误
 */
export function redirectUriProblems(redirectUri: string): string[] {
  try {
    new URL(redirectUri)
  } catch {
    return [`回调地址不是合法 URL：${redirectUri}`]
  }
  return []
}

/**
 * 这个回调地址是不是回环（仅供日志提醒，**不参与放行判定**）。
 *
 * 回环地址能用，但有个前提：授权必须在**跑歌白的那台机器**上完成。
 * 用户拿手机扫码就会跑到手机的 localhost 去（那里没有服务）——
 * 这不是“配置错”，而是“用错了设备”，提示就该这么说。
 */
export function isLoopbackRedirect(redirectUri: string): boolean {
  try {
    const host = new URL(redirectUri).hostname.toLowerCase()
    return host === "localhost" || host === "127.0.0.1" || host === "::1" || host === "[::1]" || host === "0.0.0.0"
  } catch {
    return false
  }
}

/**
 * 回环地址的用法提醒（只在日志里说，不拦）。
 */
export function loopbackRedirectHint(redirectUri: string): string | null {
  if (!isLoopbackRedirect(redirectUri)) return null
  const host = new URL(redirectUri).host
  return (
    `回调地址是回环地址（${host}）：**只能用这台机器的浏览器完成授权**` +
    `（在相期卡片上点链接会用系统默认浏览器打开，那就在本机，可行）。` +
    `若用手机飞书扫码，回调会打到手机自己的 localhost，那里没有服务——` +
    `那种情况才需要设 GEBAI_PUBLIC_URL 为公网/穿透地址。`
  )
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
