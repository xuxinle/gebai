import { describe, expect, test } from "bun:test"
import { modeSlot, readOnlyReason } from "./status-core"

/**
 * 本文件钉住的是**行为契约**而不是实现细节：中性态与警示态必须可区分，
 * 且「不可写」的四种输入口径要与标签栏编辑按钮、saveTab 的拒绝口径一致（见 status-core.ts 顶部说明）。
 */

describe("readOnlyReason：什么才算真正不可写", () => {
  test("普通文件（可编辑 + 根可写 + 未截断）→ 不是只读", () => {
    expect(readOnlyReason({ statEditable: true, truncated: false, rootWritable: true })).toBeNull()
  })

  test("只读根源（roots.writable=false）→ readonlyRoot", () => {
    expect(readOnlyReason({ statEditable: true, rootWritable: false })).toBe("readonlyRoot")
  })

  test("文件类型不可编辑（stat.editable=false）→ notEditable", () => {
    expect(readOnlyReason({ statEditable: false, rootWritable: true })).toBe("notEditable")
  })

  test("超限截断 → truncated", () => {
    expect(readOnlyReason({ statEditable: true, truncated: true, rootWritable: true })).toBe("truncated")
  })

  test("根可写性未知（根清单未到，null/undefined）按可写算——否则冷启动会闪不可编辑", () => {
    expect(readOnlyReason({ statEditable: true, rootWritable: null })).toBeNull()
    expect(readOnlyReason({ statEditable: true, rootWritable: undefined })).toBeNull()
  })

  test("优先级：先全局后文件级 —— 根只读 > 截断 > 类型不可编辑", () => {
    // 根只读对每个文件都成立：不能被更局部的 editable=false 抢先，否则用户以为「换个文件就能写」
    expect(readOnlyReason({ statEditable: false, truncated: true, rootWritable: false })).toBe("readonlyRoot")
    expect(readOnlyReason({ statEditable: false, rootWritable: false })).toBe("readonlyRoot")
    // 同为文件级：截断在前（连内容都不完整）
    expect(readOnlyReason({ statEditable: false, truncated: true, rootWritable: true })).toBe("truncated")
  })
})

describe("modeSlot：两态文案/配色必须可区分", () => {
  test("普通源码文件打开即查看态 → 中性，不带 warn，且提示 Ctrl+E", () => {
    const s = modeSlot({ mode: "view", readOnly: null })
    expect(s.text).toBe("查看")
    expect(s.cls).toBe("")
    expect(s.title).toContain("Ctrl+E")
  })

  test("中性态的文案**不含**「只读」二字（那正是误判来源）", () => {
    const s = modeSlot({ mode: "view", readOnly: null })
    expect(s.text.includes("只读")).toBe(false)
    expect(s.title.includes("只读")).toBe(false)
  })

  test("编辑态（可保存）→ 中性，文案为「编辑」", () => {
    const s = modeSlot({ mode: "edit", readOnly: null })
    expect(s.text).toBe("编辑")
    expect(s.cls).toBe("")
  })

  test("真正不可写的三种原因 → 一律 warn，且 title 给出原因", () => {
    const reasons = ["truncated", "readonlyRoot", "notEditable"] as const
    const titles: string[] = []
    for (const r of reasons) {
      const s = modeSlot({ mode: "view", readOnly: r })
      expect(s.cls).toBe("warn")
      expect(s.text).toBe("不可编辑")
      expect(s.title.length).toBeGreaterThan(0)
      titles.push(s.title)
    }
    // 三种原因的解释必须各不相同（否则用户看不出该改什么）
    expect(new Set(titles).size).toBe(3)
  })

  test("三种原因的 title 各自点到具体动作", () => {
    expect(modeSlot({ mode: "view", readOnly: "truncated" }).title).toContain("过大")
    expect(modeSlot({ mode: "view", readOnly: "readonlyRoot" }).title).toContain("GEBAI_FS_WRITE")
    expect(modeSlot({ mode: "view", readOnly: "notEditable" }).title).toContain("不支持编辑")
  })

  test("中性态与警示态在任何输入组合下都可区分（文案或配色至少一项不同）", () => {
    const cases = [
      { mode: "view" as const, readOnly: null },
      { mode: "edit" as const, readOnly: null },
      { mode: "view" as const, readOnly: "truncated" as const },
      { mode: "view" as const, readOnly: "readonlyRoot" as const },
      { mode: "view" as const, readOnly: "notEditable" as const },
      { mode: "edit" as const, readOnly: "truncated" as const },
      { mode: "edit" as const, readOnly: "readonlyRoot" as const },
      { mode: "edit" as const, readOnly: "notEditable" as const },
    ]
    const slots = cases.map(modeSlot)
    for (const s of slots) {
      expect(["查看", "编辑", "不可编辑", "不可保存"]).toContain(s.text)
      expect(["", "warn"]).toContain(s.cls)
      // 不变量：warn 一定配着原因说明，中性一定不带 warn
      if (s.cls === "warn") expect(s.title).not.toBe("")
    }
    // 「查看态（可写）」与「不可编辑（只读根源）」必须不同
    const viewOk = modeSlot({ mode: "view", readOnly: null })
    const viewRo = modeSlot({ mode: "view", readOnly: "readonlyRoot" })
    expect(viewOk.cls !== viewRo.cls || viewOk.text !== viewRo.text).toBe(true)
  })
})

/**
 * 边界缺口（本单闭合）：编辑态 + 文件真不可写。
 * 路径真实存在——标签栏编辑按钮在不可写时 disabled，但 Ctrl+E（toggleActiveMode）不看
 * editable 直接翻模式，编辑器仍是只读，保存也会被拒。三因各锁一条，文案/配色/title 三重可辨。
 */
describe("编辑态 + 真不可写：必须 warn 且明说「存不回」，不得显示中性「编辑」", () => {
  const reasons = ["readonlyRoot", "truncated", "notEditable"] as const

  for (const r of reasons) {
    test(`编辑态 + ${r} → warn「不可保存」，title 说明原因与下一步`, () => {
      const s = modeSlot({ mode: "edit", readOnly: r })
      expect(s.cls).toBe("warn")
      expect(s.text).toBe("不可保存")
      expect(s.text).not.toBe("编辑")
      expect(s.title).toContain("存不回")
      // title 与查看态同因的措辞一致（同一套原因表），且给出下一步
      expect(s.title.length).toBeGreaterThan(20)
    })
  }

  test("编辑态可保存 vs 不可保存：文案与配色都不同（可区分）", () => {
    const ok = modeSlot({ mode: "edit", readOnly: null })
    const bad = modeSlot({ mode: "edit", readOnly: "readonlyRoot" })
    expect(ok.cls).toBe("")
    expect(bad.cls).toBe("warn")
    expect(ok.text).not.toBe(bad.text)
    // 中性编辑态不得宣称「不可保存」以外的误导：它就是「编辑」+ 可保存
    expect(ok.title).toContain("可保存")
  })

  test("不可写时两个方向的措辞对称：查看「不可编辑」/ 编辑「不可保存」", () => {
    expect(modeSlot({ mode: "view", readOnly: "truncated" }).text).toBe("不可编辑")
    expect(modeSlot({ mode: "edit", readOnly: "truncated" }).text).toBe("不可保存")
  })

  test("端到端口径：edit 态经 readOnlyReason 判定后必定落到 warn（不漏判）", () => {
    const unwritable = [
      { statEditable: false, truncated: false, rootWritable: true },
      { statEditable: true, truncated: true, rootWritable: true },
      { statEditable: true, truncated: false, rootWritable: false },
    ] as const
    for (const input of unwritable) {
      const s = modeSlot({ mode: "edit", readOnly: readOnlyReason(input) })
      expect(s.cls).toBe("warn")
      expect(s.title).toContain("存不回")
    }
  })
})
