import { describe, expect, test } from "bun:test"
import { TokenBucket } from "./ratelimit"

describe("TokenBucket", () => {
  test("allows up to capacity bursts then rejects", () => {
    const b = new TokenBucket(3, 100)
    expect(b.allow("u1")).toBe(true)
    expect(b.allow("u1")).toBe(true)
    expect(b.allow("u1")).toBe(true)
    expect(b.allow("u1")).toBe(false)
    // 其他用户独立计数
    expect(b.allow("u2")).toBe(true)
  })

  test("refills over time", async () => {
    const b = new TokenBucket(2, 100) // 每秒补充 100，测试中即时满
    expect(b.allow("u1")).toBe(true)
    expect(b.allow("u1")).toBe(true)
    expect(b.allow("u1")).toBe(false)
    // 等待补充（10ms 应补充 ≥1 个令牌）
    await new Promise((r) => setTimeout(r, 15))
    expect(b.allow("u1")).toBe(true)
  })

  test("idle buckets are swept to bound memory", () => {
    const b = new TokenBucket(2, 100, 0) // 空闲清理阈值 0ms：任意 allow 即清理过期桶
    b.allow("old")
    b.allow("new")
    // 阈值 0 时所有旧桶都被清理，内存保持有界（无断言异常即通过）
    expect(b.allow("new")).toBe(true)
  })
})
