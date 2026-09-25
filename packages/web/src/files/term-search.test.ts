/**
 * 终端搜索选项（`files/term-search.ts`，纯函数）。
 *
 * 断言重点是两处易漏：① 三开关与计数文案的边界（空关键词 / 零命中 / 有命中未选中）；
 * ② **decoration 取值必须带底色**——只给 overview ruler 时命中在滚动缓冲里不可见。
 */
import { describe, expect, test } from "bun:test"
import {
  DEFAULT_SEARCH_OPTIONS,
  SEARCH_TOGGLES,
  parseSearchOptions,
  searchDecorations,
  searchStatusText,
  toggleSearchOption,
} from "./term-search"

describe("term-search（终端搜索选项）", () => {
  test("默认三项全关（与 VSCode 终端查找一致）", () => {
    expect(DEFAULT_SEARCH_OPTIONS).toEqual({ caseSensitive: false, regex: false, wholeWord: false })
  })

  test("开关表顺序与 VSCode 一致：Aa → ab → .*", () => {
    expect(SEARCH_TOGGLES.map((t) => t.key)).toEqual(["caseSensitive", "wholeWord", "regex"])
    expect(SEARCH_TOGGLES.map((t) => t.label)).toEqual(["Aa", "ab", ".*"])
  })

  test("切换只动一项且不可变（原对象不被改）", () => {
    const cur = { ...DEFAULT_SEARCH_OPTIONS }
    const next = toggleSearchOption(cur, "regex")
    expect(next.regex).toBe(true)
    expect(cur.regex).toBe(false)
    expect(toggleSearchOption(next, "regex").regex).toBe(false)
  })

  test("解析持久化开关：脏数据逐项回落", () => {
    expect(parseSearchOptions(null)).toEqual(DEFAULT_SEARCH_OPTIONS)
    expect(parseSearchOptions("{坏 JSON")).toEqual(DEFAULT_SEARCH_OPTIONS)
    expect(parseSearchOptions(JSON.stringify({ caseSensitive: true, regex: "是", wholeWord: null }))).toEqual({
      caseSensitive: true,
      regex: false,
      wholeWord: false,
    })
  })

  test("decoration 取值齐备且命中底色非空（漏传底色 = 搜索没有可见高亮）", () => {
    const d = searchDecorations({ matchBackground: "rgba(1,2,3,.3)", activeMatchBackground: "#f5f543", activeMatchBorder: "#f5f543" })
    expect(d.matchBackground).toBe("rgba(1,2,3,.3)")
    expect(d.activeMatchBackground).toBe("#f5f543")
    expect(d.activeMatchBorder).toBe("#f5f543")
    expect(d.matchOverviewRuler).toBe("#f5f543")
    expect(d.activeMatchColorOverviewRuler).toBe("#f5f543")
  })

  test("状态文案：空关键词不占位 / 零命中 / 第 n 项 / 有命中未选中 / 计数", () => {
    expect(searchStatusText(null, "")).toBe("")
    expect(searchStatusText({ resultIndex: -1, resultCount: 0 }, "x")).toBe("无匹配")
    expect(searchStatusText({ resultIndex: 2, resultCount: 17 }, "x")).toBe("3/17")
    expect(searchStatusText({ resultIndex: -1, resultCount: 4 }, "x")).toBe("4 处")
  })
})
