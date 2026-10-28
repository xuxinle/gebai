/**
 * 自发送消息记账（SentMessageRegistry）单测。
 *
 * 覆盖三件事：
 * 1. 登记后可命中（入站据此丢弃自己发出的消息）
 * 2. TTL 过期后不再命中（重投窗口之外自然失效）
 * 3. 容量有界（长跑不膨胀）
 */
import { describe, expect, test } from "bun:test"
import { SentMessageRegistry } from "./outbox"

describe("SentMessageRegistry", () => {
  test("登记后可命中，未登记不命中", () => {
    const reg = new SentMessageRegistry()
    expect(reg.hit("om_1")).toBe(false)
    reg.record("om_1")
    expect(reg.hit("om_1")).toBe(true)
    expect(reg.hit("om_2")).toBe(false)
  })

  test("重复登记只刷新时间戳（幂等）", () => {
    const reg = new SentMessageRegistry(4, 10_000)
    reg.record("om_1", 1000)
    reg.record("om_1", 5000)
    expect(reg.size()).toBe(1)
    expect(reg.hit("om_1", 6000)).toBe(true)
  })

  test("TTL 过期后不再命中", () => {
    const reg = new SentMessageRegistry(4, 1000)
    reg.record("om_1", 0)
    expect(reg.hit("om_1", 999)).toBe(true)
    expect(reg.hit("om_1", 1001)).toBe(false) // 过期即失效
    expect(reg.size()).toBe(0) // 惰性清理
  })

  test("容量有界：超出驱逐最旧", () => {
    const reg = new SentMessageRegistry(2, 60_000)
    reg.record("a")
    reg.record("b")
    expect(reg.hit("a")).toBe(true)
    reg.record("c")
    expect(reg.hit("a")).toBe(false) // 最旧被驱逐
    expect(reg.hit("b")).toBe(true)
    expect(reg.hit("c")).toBe(true)
  })

  test("空 id 忽略（不记不查）", () => {
    const reg = new SentMessageRegistry()
    reg.record("")
    expect(reg.size()).toBe(0)
    expect(reg.hit("")).toBe(false)
  })
})