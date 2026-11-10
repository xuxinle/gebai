/**
 * 「分支」栏小切换的绘制状态短路判据（`files/refs-tab.ts`）。
 *
 * 回归点：切到标签 / 储存 / 远程后再切回分支，分支数据没变也必须重绘——否则内容停在别的视图上，
 * 表现为「切不回分支」。web 包无 jsdom，无法直接构造容器，故把不变量抽成纯函数在此钉住。
 */
import { describe, expect, test } from "bun:test"
import { shouldSkipRepaint, type RefsTab } from "./refs-tab"

describe("shouldSkipRepaint（切 tab 后能否重绘）", () => {
  test("当前画的正是本视图且指纹没变 → 跳过（不抖 DOM）", () => {
    expect(shouldSkipRepaint("branches", "branches", "k1", "k1")).toBe(true)
  })

  test("当前画的是别的视图，即使指纹相同 → 必须重绘（切回分支的场景）", () => {
    // 从储存切回分支：分支指纹与上次画分支时相同，但容器现在是储存内容
    expect(shouldSkipRepaint("stash", "branches", "k1", "k1")).toBe(false)
    expect(shouldSkipRepaint("remotes", "branches", "k1", "k1")).toBe(false)
  })

  test("本视图数据变了 → 重绘", () => {
    expect(shouldSkipRepaint("branches", "branches", "k2", "k1")).toBe(false)
  })

  test("尚未画过（占位态 / 首屏）→ 重绘", () => {
    const tabs: RefsTab[] = ["branches", "tags", "stash", "remotes"]
    for (const t of tabs) expect(shouldSkipRepaint(null, t, "k1", "k1")).toBe(false)
  })
})
