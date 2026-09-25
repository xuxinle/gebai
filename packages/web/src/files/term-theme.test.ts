/**
 * 终端配色（`files/term-theme.ts`，纯函数）。
 *
 * 关注点：① 16 色 ANSI 调色板**不重复**（亮色与常规色两档不同）；② 按背景亮度择暗/亮两套；
 * ③ 主题色写法多样（`#rgb` / `#rrggbb` / `rgb()`）都要解析得出，解析不出时按暗底兜底。
 */
import { describe, expect, test } from "bun:test"
import { MIN_CONTRAST_RATIO, isDarkColor, parseColor, searchMatchColors, terminalTheme } from "./term-theme"

describe("term-theme（终端配色）", () => {
  test("parseColor：三种写法 + 非法输入", () => {
    expect(parseColor("#1e1e1e")).toEqual([30, 30, 30])
    expect(parseColor("#fff")).toEqual([255, 255, 255])
    expect(parseColor("rgb(24, 24, 24)")).toEqual([24, 24, 24])
    expect(parseColor("rgba(120,160,255,0.35)")).toEqual([120, 160, 255])
    expect(parseColor("var(--x)")).toBeNull()
    expect(parseColor("")).toBeNull()
  })

  test("isDarkColor：暗底/亮底判定，解析不出按暗底", () => {
    expect(isDarkColor("#181818")).toBe(true)
    expect(isDarkColor("#ffffff")).toBe(false)
    expect(isDarkColor("rgb(240,240,240)")).toBe(false)
    expect(isDarkColor("不是颜色")).toBe(true)
  })

  test("terminalTheme：16 色齐备且亮色与常规色不重复", () => {
    const t = terminalTheme({ background: "#181818", foreground: "#d4d4d4" })
    const names = ["black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"]
    for (const n of names) {
      expect(typeof t[n]).toBe("string")
      expect(typeof t[`bright${n[0]!.toUpperCase()}${n.slice(1)}`]).toBe("string")
      // 同一色系的两档必须不同——重复取值等于浪费 8 个颜色位（旧实现的毛病）
      expect(t[`bright${n[0]!.toUpperCase()}${n.slice(1)}`]).not.toBe(t[n])
    }
    expect(t.background).toBe("#181818")
    expect(t.foreground).toBe("#d4d4d4")
    expect(t.cursor).toBe("#d4d4d4")
  })

  test("terminalTheme：亮底走亮色调色板（同一逻辑色在两套下取值不同）", () => {
    const dark = terminalTheme({ background: "#101010", foreground: "#eee" })
    const light = terminalTheme({ background: "#f7f7f7", foreground: "#111" })
    expect(dark.white).not.toBe(light.white)
    expect(light.brightWhite).not.toBe(light.white)
  })

  test("terminalTheme：缺省背景/前景回落可用值（服务端或主题没给色时不至于白底白字）", () => {
    const t = terminalTheme({ background: "", foreground: "" })
    expect(t.background).toBe("#181818")
    expect(t.foreground).toBe("#d4d4d4")
  })

  test("searchMatchColors：命中底色随明暗切换，当前命中另有边框色", () => {
    const d = searchMatchColors("#181818")
    const l = searchMatchColors("#ffffff")
    expect(d.matchBackground).not.toBe(l.matchBackground)
    expect(d.activeMatchBorder).toBe(d.activeMatchBackground)
  })

  test("最小对比度常量与 VSCode 默认一致", () => {
    expect(MIN_CONTRAST_RATIO).toBe(4.5)
  })
})
