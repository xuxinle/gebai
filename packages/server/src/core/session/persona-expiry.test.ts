/**
 * 授权提醒判定：什么时候该叫醒用户。
 *
 * 这些用例守住的都是**已经出过问题**的具体形态，不是假想风险：
 *
 * 1. 只在过期后叫 → 用户看到弹窗时，刚才那段时间的消息已经没人回了
 * 2. 永远叫 → 每 5 分钟弹一次，用户学会无视（比不提醒更糟）
 * 3. 卡过期后不重推 → 错过 10 分钟就永远收不到（实测：id 非 null 就 return 的写法）
 */
import { describe, expect, test } from "bun:test"
import {
  AUTH_CARD_SKEW_MS,
  AUTH_CARD_TTL_MS,
  authCardDeadline,
  decideExpiry,
  shouldPushAuthCard,
  WARN_BEFORE_MS,
} from "./persona-expiry"

describe("decideExpiry：什么时候该弹窗", () => {
  test("剩余充足（2 小时）→ 不打扰", () => {
    expect(decideExpiry(true, 2 * 60 * 60_000)).toEqual({ needCard: false, state: "ok" })
  })

  test("恰好 10 分钟 → 弹（边界含在内，宁可早一步）", () => {
    expect(decideExpiry(true, WARN_BEFORE_MS)).toEqual({ needCard: true, state: "expiring" })
  })

  test("剩余 8 分钟 → 弹，且标 expiring 而非 expired", () => {
    // 区别是文案责任：expiring 说「即将失效」，expired 要说「已经不能发了」
    const d = decideExpiry(true, 8 * 60_000)
    expect(d.needCard).toBe(true)
    expect(d.state).toBe("expiring")
  })

  test("窗口外（11 分钟）→ 还不弹", () => {
    expect(decideExpiry(true, 11 * 60_000).needCard).toBe(false)
  })

  test("已过期 → 弹，标 expired", () => {
    expect(decideExpiry(false, 0)).toEqual({ needCard: true, state: "expired" })
  })

  test("ready 但拿不到剩余时间 → 不弹（不拿不准的数据乱叫）", () => {
    expect(decideExpiry(true, undefined)).toEqual({ needCard: false, state: "ok" })
    expect(decideExpiry(true, Number.POSITIVE_INFINITY)).toEqual({ needCard: false, state: "ok" })
  })
})

describe("shouldPushAuthCard：过期后必须允许重推", () => {
  const now = 1_000_000

  test("没有卡 → 推", () => {
    expect(shouldPushAuthCard(null, 0, now)).toBe(true)
  })

  test("卡还在有效期内 → 不重复推（避免桌面上两张一样的卡）", () => {
    expect(shouldPushAuthCard("card-1", now + 60_000, now)).toBe(false)
  })

  test("卡已过期 → 重推（「到期就弹窗」能反复生效的前提）", () => {
    expect(shouldPushAuthCard("card-1", now, now)).toBe(true)
    expect(shouldPushAuthCard("card-1", now - 1, now)).toBe(true)
  })
})

describe("authCardDeadline：宁早勿晚", () => {
  test("比 TTL 提前一个宽限（时钟漂移下不空等）", () => {
    const now = 5_000_000
    expect(authCardDeadline(now)).toBe(now + AUTH_CARD_TTL_MS - AUTH_CARD_SKEW_MS)
  })

  test("宽限期确实比 TTL 短（否则刚推就算过期，会连环重推）", () => {
    expect(AUTH_CARD_SKEW_MS).toBeLessThan(AUTH_CARD_TTL_MS)
  })
})
