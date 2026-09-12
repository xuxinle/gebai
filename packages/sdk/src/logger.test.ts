import { afterEach, describe, expect, test } from "bun:test"
import { getLogLevel, log, logEnabled, setLogLevel } from "./logger"

/** 捕获 console 输出（级别过滤只看是否调用下游）。 */
function capture<T>(fn: () => T): { out: string[]; err: string[]; ret: T } {
  const out: string[] = []
  const err: string[] = []
  const origLog = console.log
  const origWarn = console.warn
  const origError = console.error
  console.log = (...a: unknown[]) => void out.push(a.join(" "))
  console.warn = (...a: unknown[]) => void err.push(a.join(" "))
  console.error = (...a: unknown[]) => void err.push(a.join(" "))
  try {
    return { out, err, ret: fn() }
  } finally {
    console.log = origLog
    console.warn = origWarn
    console.error = origError
  }
}

afterEach(() => setLogLevel("info"))

describe("日志器：级别过滤", () => {
  test("默认 info：debug 被抑制，info/warn/error 分别落到 stdout/stderr", () => {
    setLogLevel(undefined)
    expect(getLogLevel()).toBe("info")
    const { out, err } = capture(() => {
      log.debug("d")
      log.info("i")
      log.warn("w")
      log.error("e")
    })
    expect(out).toEqual(["i"])
    expect(err).toEqual(["w", "e"])
  })

  test("warn：info 也被抑制（更安静）", () => {
    setLogLevel("warn")
    const { out, err } = capture(() => {
      log.debug("d")
      log.info("i")
      log.warn("w")
      log.error("e")
    })
    expect(out).toEqual([])
    expect(err).toEqual(["w", "e"])
  })

  test("debug：全量输出", () => {
    setLogLevel("debug")
    const { out, err } = capture(() => {
      log.debug("d")
      log.info("i")
      log.warn("w")
      log.error("e")
    })
    expect(out).toEqual(["d", "i"])
    expect(err).toEqual(["w", "e"])
  })

  test("error：仅 error", () => {
    setLogLevel("error")
    const { out, err } = capture(() => {
      log.info("i")
      log.warn("w")
      log.error("e")
    })
    expect(out).toEqual([])
    expect(err).toEqual(["e"])
  })

  test("非法值忽略（保持当前级别），大小写与空白容错", () => {
    setLogLevel("warn")
    expect(setLogLevel("verbose")).toBe("warn")
    expect(setLogLevel("")).toBe("warn")
    expect(setLogLevel("  DEBUG  ")).toBe("debug")
  })

  test("logEnabled 供调用点短路昂贵的参数构造", () => {
    setLogLevel("warn")
    expect(logEnabled("debug")).toBe(false)
    expect(logEnabled("info")).toBe(false)
    expect(logEnabled("warn")).toBe(true)
    expect(logEnabled("error")).toBe(true)
  })

  test("参数原样透传（含多参与非字符串）", () => {
    setLogLevel("info")
    const { out } = capture(() => log.info("[tag]", { a: 1 }, 42))
    expect(out[0]).toBe("[tag] [object Object] 42")
  })
})
