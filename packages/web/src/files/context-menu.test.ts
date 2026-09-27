/**
 * 编辑器右键菜单剪枝的纯逻辑（`files/context-menu.ts` 的 `menuPrunePlan`）。
 *
 * 只测「删哪些行」——DOM 装配（observer 挂载、shadow root 拿取）由浏览器实测覆盖，
 * 单测的 DOM 桩给不了 shadow DOM 与真实菜单结构。
 */
import { describe, expect, test } from "bun:test"
import { MENU_DROP_LABELS, menuPrunePlan, type MenuRow } from "./context-menu"

const item = (label: string): MenuRow => ({ kind: "item", label })
const sep: MenuRow = { kind: "sep", label: "" }

/** 实测到的原始菜单顺序（Go / References / Symbol / Peek / Copy / 复制路径 / 溯源×3 / Command Palette）。 */
const REAL_MENU: MenuRow[] = [
  item("Go to Definition"),
  item("Go to References"),
  item("Go to Symbol..."),
  item("Peek"),
  sep,
  item("Copy"),
  sep,
  item("复制路径"),
  sep,
  item("行尾溯源"),
  item("侧边溯源"),
  item("文件历史"),
  sep,
  item("Command Palette"),
]

describe("菜单剪枝计划", () => {
  test("剔除 Peek / Copy / Command Palette，保留导航与工作台各项", () => {
    const plan = menuPrunePlan(REAL_MENU)
    const dropped = plan.map((i) => REAL_MENU[i]!.label || "<sep>")
    expect(dropped.sort()).toEqual(["<sep>", "<sep>", "Command Palette", "Copy", "Peek"].sort())
  })

  test("删完条目后收拾分隔线：相邻两条只留一条，末条悬空也去掉", () => {
    // Peek / Copy 之间的分隔线贴在一起 → 删一条；Command Palette 在末位 → 它前面那条分隔线悬空 → 删
    const plan = new Set(menuPrunePlan(REAL_MENU))
    const kept = REAL_MENU.filter((_, i) => !plan.has(i)).map((r) => (r.kind === "sep" ? "──" : r.label))
    expect(kept).toEqual([
      "Go to Definition",
      "Go to References",
      "Go to Symbol...",
      "──",
      "复制路径",
      "──",
      "行尾溯源",
      "侧边溯源",
      "文件历史",
    ])
    // 结构不变式：首尾不是分隔线、且没有相邻分隔线
    expect(kept[0]).not.toBe("──")
    expect(kept[kept.length - 1]).not.toBe("──")
    for (let i = 1; i < kept.length; i++) expect(kept[i] === "──" && kept[i - 1] === "──").toBe(false)
  })

  test("原样保留：没有要删的条目时不产生任何计划", () => {
    const rows = [item("Go to Definition"), sep, item("复制路径"), sep, item("行尾溯源"), item("文件历史")]
    expect(menuPrunePlan(rows)).toEqual([])
  })

  test("首行分隔线（删掉第一个条目后可能露出来）也会被清掉", () => {
    const rows = [item("Copy"), sep, item("复制路径")]
    expect(menuPrunePlan(rows)).toEqual([0, 1])
  })

  test("三个标题都在剔除清单里（守卫文案/大小写）", () => {
    expect([...MENU_DROP_LABELS].sort()).toEqual(["Command Palette", "Copy", "Peek"])
  })

  test("只按标题精确匹配：含相同词但不同标题的条目不受影响", () => {
    const rows = [item("Copy Path"), sep, item("Peek Definition"), item("Command Palette Extra")]
    expect(menuPrunePlan(rows)).toEqual([])
  })
})
