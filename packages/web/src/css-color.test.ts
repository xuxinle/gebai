import { describe, expect, test } from "bun:test"
import { cssVarToHex } from "./css-color"

describe("cssVarToHex", () => {
  test("hex 原样返回", () => {
    expect(cssVarToHex("#141418", "", "#0d1117")).toBe("#141418")
  })

  test("亮色主题：半透明黑叠浅色背景 → 浅灰（不再退化暗色）", () => {
    // 亮色默认主题：--bg-hover rgba(0,0,0,0.05) 叠 body rgb(232,232,238)
    expect(cssVarToHex("rgba(0, 0, 0, 0.05)", "rgb(232, 232, 238)", "#232a3d")).toBe("#dcdce2")
  })

  test("暗色主题：半透明白叠近黑背景 → 深灰（保持原观感）", () => {
    // 暗色默认主题：--bg-hover rgba(255,255,255,0.05) 叠 body rgb(8,8,10)
    expect(cssVarToHex("rgba(255, 255, 255, 0.05)", "rgb(8, 8, 10)", "#232a3d")).toBe("#141416")
  })

  test("rgb 无 alpha：直接取色", () => {
    expect(cssVarToHex("rgb(8, 8, 10)", "", "#0d1117")).toBe("#08080a")
  })

  test("现代空格/斜杠语法 rgba(0 0 0 / 0.16)", () => {
    expect(cssVarToHex("rgba(0 0 0 / 0.16)", "rgb(232, 232, 238)", "#35405c")).toBe("#c3c3c8")
  })

  test("无 body 背景（透明）时按近黑合成兜底", () => {
    expect(cssVarToHex("rgba(255, 255, 255, 0.05)", "transparent", "#232a3d")).toBe("#191d23")
  })

  test("非法值回退 fallback", () => {
    expect(cssVarToHex("linear-gradient(180deg, #000, #fff)", "", "#0d1117")).toBe("#0d1117")
    expect(cssVarToHex("", "", "#0d1117")).toBe("#0d1117")
  })
})
