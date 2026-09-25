/**
 * 终端偏好（`files/term-prefs.ts`，纯函数）。
 *
 * 这里断言的是**容错**：单项脏数据只回落该项，不牵连同套偏好；旧键（字号 / 跟随根）
 * 在新键缺失时仍然生效（升级不该把用户字号重置）。
 */
import { describe, expect, test } from "bun:test"
import {
  DEFAULT_PREFS,
  PREFS_KEY,
  FONT_SIZE_KEY,
  FOLLOW_ROOT_KEY,
  clampFontSize,
  clampLineHeight,
  nextInCycle,
  parsePrefs,
  serializePrefs,
} from "./term-prefs"

describe("term-prefs（终端偏好）", () => {
  test("存储键：字号 / 跟随根沿用降级实现的键，其余收在 JSON 键里", () => {
    expect(FONT_SIZE_KEY).toBe("gebai.ui.termFontSize")
    expect(FOLLOW_ROOT_KEY).toBe("gebai.ui.termFollowRoot")
    expect(PREFS_KEY).toBe("gebai.ui.termPrefs")
  })

  test("空输入回落默认值", () => {
    expect(parsePrefs(null)).toEqual(DEFAULT_PREFS)
    expect(parsePrefs("")).toEqual(DEFAULT_PREFS)
    expect(parsePrefs("{不是 JSON")).toEqual(DEFAULT_PREFS)
  })

  test("往返稳定：序列化后解析回同一份", () => {
    const p = { ...DEFAULT_PREFS, fontSize: 17, lineHeight: 1.5, cursorStyle: "block" as const, copyOnSelection: true, wheelZoom: false }
    expect(parsePrefs(serializePrefs(p))).toEqual(p)
  })

  test("单项脏数据只回落该项：行高非法不影响字号/光标", () => {
    const p = parsePrefs(JSON.stringify({ fontSize: 20, lineHeight: "高一点", cursorStyle: "arrow", cursorBlink: "yes" }))
    expect(p.fontSize).toBe(20)
    expect(p.lineHeight).toBe(DEFAULT_PREFS.lineHeight)
    expect(p.cursorStyle).toBe(DEFAULT_PREFS.cursorStyle)
    expect(p.cursorBlink).toBe(DEFAULT_PREFS.cursorBlink)
  })

  test("字号夹取到 8–28（远超范围/非数字都不产生不可用字号）", () => {
    expect(clampFontSize(100)).toBe(28)
    expect(clampFontSize(1)).toBe(8)
    expect(clampFontSize("13")).toBe(13)
    expect(clampFontSize("abc")).toBe(DEFAULT_PREFS.fontSize)
    expect(clampFontSize(13.4)).toBe(13)
    // null/空串是「没有这个值」：Number(null)=0 不管的话首次打开会得到最小字号 8
    expect(clampFontSize(null)).toBe(DEFAULT_PREFS.fontSize)
    expect(clampFontSize(undefined)).toBe(DEFAULT_PREFS.fontSize)
    expect(clampFontSize("")).toBe(DEFAULT_PREFS.fontSize)
  })

  test("行高夹取到 1–2", () => {
    expect(clampLineHeight(0.2)).toBe(1)
    expect(clampLineHeight(9)).toBe(2)
    expect(clampLineHeight("1.4")).toBe(1.4)
    expect(clampLineHeight(undefined)).toBe(DEFAULT_PREFS.lineHeight)
    expect(clampLineHeight(null)).toBe(DEFAULT_PREFS.lineHeight)
    expect(clampLineHeight("")).toBe(DEFAULT_PREFS.lineHeight)
  })

  test("首次打开（新旧键都缺）用默认字号，不是最小字号", () => {
    expect(parsePrefs(null, { fontSize: null, followRoot: null }).fontSize).toBe(DEFAULT_PREFS.fontSize)
  })

  test("旧键兜底：新键缺字号/跟随根时读旧键（升级不重置用户设置）", () => {
    const p = parsePrefs(null, { fontSize: "18", followRoot: "0" })
    expect(p.fontSize).toBe(18)
    expect(p.followRoot).toBe(false)
  })

  test("新键优先于旧键（用户在新版改过之后以新版为准）", () => {
    const p = parsePrefs(JSON.stringify({ fontSize: 15, followRoot: true }), { fontSize: "22", followRoot: "0" })
    expect(p.fontSize).toBe(15)
    expect(p.followRoot).toBe(true)
  })

  test("循环切换：命中表内项取下一个，不在表内取第一个", () => {
    expect(nextInCycle([1, 1.25, 1.5], 1.25)).toBe(1.5)
    expect(nextInCycle([1, 1.25, 1.5], 1.5)).toBe(1)
    expect(nextInCycle(["bar", "block"], "underline" as never)).toBe("bar")
  })
})
