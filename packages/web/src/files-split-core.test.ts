import { describe, expect, test } from "bun:test"
import { clampSplitWidth, normalizeSplitSide, splitWidthFromPointer, SPLIT_MIN_MAIN, SPLIT_MIN_PANEL } from "./files-split-core"

/** 分屏纯逻辑单测（无 DOM，与 files-split.ts 的宿主实现分离）。 */

describe("normalizeSplitSide（停靠侧归一化）", () => {
  test("只认 left/right", () => {
    expect(normalizeSplitSide("left")).toBe("left")
    expect(normalizeSplitSide("right")).toBe("right")
  })

  test("脏值一律落回缺省（左侧）", () => {
    for (const raw of [undefined, null, "", "LEFT", "top", 0, 1, {}, []]) {
      expect(normalizeSplitSide(raw)).toBe("left")
    }
  })
})

describe("clampSplitWidth（宽度夹取）", () => {
  test("常规区间原样（四舍五入）", () => {
    expect(clampSplitWidth(640, 1280)).toBe(640)
    expect(clampSplitWidth(640.4, 1280)).toBe(640)
    expect(clampSplitWidth(640.6, 1280)).toBe(641)
  })

  test("小于面板下限 → 面板下限；大于窗口留白 → 留白上限", () => {
    expect(clampSplitWidth(10, 1280)).toBe(SPLIT_MIN_PANEL)
    expect(clampSplitWidth(2000, 1280)).toBe(1280 - SPLIT_MIN_MAIN)
  })

  test("窗口窄到两边下限打架时以面板下限为准（不返回负宽度）", () => {
    expect(clampSplitWidth(500, 600)).toBe(SPLIT_MIN_PANEL)
    expect(clampSplitWidth(1, 300)).toBe(SPLIT_MIN_PANEL)
  })
})

describe("splitWidthFromPointer（拖分界换算）", () => {
  test("右停靠：宽度 = 窗口宽 - 指针 x", () => {
    expect(splitWidthFromPointer(1280, 860, 1280, "right")).toBe(0)
    expect(splitWidthFromPointer(420, 420, 1280, "right")).toBe(860)
  })

  test("左停靠：宽度 = 指针 x - 面板左缘（面板通常贴窗口左缘，但按左缘算才不依赖窗口偏移）", () => {
    expect(splitWidthFromPointer(0, 0, 1280, "left")).toBe(0)
    expect(splitWidthFromPointer(860, 0, 1280, "left")).toBe(860)
    // 窗口有 40px 偏移时（非全屏/多显示器），宽度仍取相对面板左缘的距离
    expect(splitWidthFromPointer(900, 40, 1280, "left")).toBe(860)
  })

  test("同一面板宽度：右停靠指针在「窗口宽-w」处，左停靠指针在「w」处", () => {
    const windowWidth = 1280
    const width = 640
    expect(splitWidthFromPointer(windowWidth - width, windowWidth - width, windowWidth, "right")).toBe(width)
    expect(splitWidthFromPointer(width, 0, windowWidth, "left")).toBe(width)
  })
})
