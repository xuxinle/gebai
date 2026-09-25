import { describe, expect, test } from "bun:test"
import { formatTps, rateLevel, RATE_BANDS } from "./token-rate-core"

describe("token-rate-core: 速率格式化与路况分级", () => {
  test("formatTps：<100 一位小数、≥100 取整（位数不再增长，标题栏宽度稳定）", () => {
    expect(formatTps(42.34)).toBe("42.3")
    expect(formatTps(42)).toBe("42.0")
    expect(formatTps(123.6)).toBe("124")
    expect(formatTps(0)).toBe("0")
    expect(formatTps(Number.NaN)).toBe("0")
  })

  test("rateLevel：六级路况分级（阈值表从高到低命中）", () => {
    expect(rateLevel(120).level).toBe(1)
    expect(rateLevel(80).level).toBe(1)
    expect(rateLevel(79.9).level).toBe(2)
    expect(rateLevel(50).level).toBe(2)
    expect(rateLevel(35).level).toBe(3)
    expect(rateLevel(20).level).toBe(4)
    expect(rateLevel(10).level).toBe(5)
    expect(rateLevel(7.9).level).toBe(6)
    expect(rateLevel(0).level).toBe(6)
    // 级名齐备且末级为 0 兜底（判定表必然命中）
    expect(RATE_BANDS.map((b) => b.label)).toEqual(["畅通", "顺畅", "良好", "缓行", "拥堵", "严重拥堵"])
    expect(RATE_BANDS[RATE_BANDS.length - 1]!.min).toBe(0)
  })
})
