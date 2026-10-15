import { describe, expect, test } from "bun:test"
import { editButton, modeSlot, readOnlyNext, readOnlyReason, readOnlyWhy, saveItem, saveRefusal, saveRefusalText, saveRefusalWhy } from "./status-core"

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

/**
 * 标签栏编辑按钮 ← 与状态栏模式格**同源同口径**（本单收口）。
 *
 * 原先 `renderTabActions` 自己判 `editable` 并写死一句「该文件类型不支持编辑」：不可写的三个原因
 * （根只读 / 截断 / 类型）被说成同一个，用户会去扩展名上找原因——而真正要做的是改服务端开关
 * 或下载后编辑。这里钉住「按钮说出的原因 == 状态栏说出的原因」。
 */
describe("editButton：不可写时按钮与徽标同一份原因，且编辑态必须能退出来", () => {
  test("可写 + 查看态：可点，去编辑，提示 Ctrl+E", () => {
    const b = editButton({ mode: "view", readOnly: null })
    expect(b.icon).toBe("edit")
    expect(b.disabled).toBe(false)
    expect(b.active).toBe(false)
    expect(b.title).toContain("Ctrl+E")
  })

  test("可写 + 编辑态：可点（回查看）、按下态", () => {
    const b = editButton({ mode: "edit", readOnly: null })
    expect(b.icon).toBe("eye")
    expect(b.disabled).toBe(false)
    expect(b.active).toBe(true)
  })

  test("不可写 + 查看态：禁用，且 title 点名**真正**的原因，不是一律「类型不支持」", () => {
    const root = editButton({ mode: "view", readOnly: "readonlyRoot" })
    const trunc = editButton({ mode: "view", readOnly: "truncated" })
    const kind = editButton({ mode: "view", readOnly: "notEditable" })
    expect([root.disabled, trunc.disabled, kind.disabled]).toEqual([true, true, true])
    // 三种原因必须给出三种不同的下一步（一律归到“类型不支持”正是原先的缺陷）
    expect(new Set([root.title, trunc.title, kind.title]).size).toBe(3)
    expect(root.title).toContain("GEBAI_FS_WRITE")
    expect(trunc.title).toContain("过大")
    expect(kind.title).toContain("不支持编辑")
    // 与状态栏徽标同一份 REASON（按钮 title 内含徽标 title 的核心措辞）
    for (const r of ["readonlyRoot", "truncated", "notEditable"] as const) {
      expect(editButton({ mode: "view", readOnly: r }).title).toContain(readOnlyWhy(r))
      expect(editButton({ mode: "view", readOnly: r }).title).toContain(readOnlyNext(r))
      expect(modeSlot({ mode: "view", readOnly: r }).title).toContain(readOnlyWhy(r))
    }
  })

  /**
   * 这一条是本单最有价值的回归：编辑态禁用 = 用户退不出来。
   * 路径真实存在——`Ctrl+E`（`toggleActiveMode`）不看可写性就能翻模式，进到编辑态后若那颗按钮是灰的，
   * 界面上就没有“回查看”的入口（只剩再按一次 Ctrl+E，而按钮的灰态正告诉用户这里关不掉）。
   */
  test("不可写 + **编辑态**：按钮必须仍可点（否则用户被困在编辑态）", () => {
    for (const r of ["readonlyRoot", "truncated", "notEditable"] as const) {
      const b = editButton({ mode: "edit", readOnly: r })
      expect(b.disabled, `${r}：编辑态下按钮被禁用 = 退不回查看态`).toBe(false)
      expect(b.active).toBe(true)
      // 但仍要把“存不回”说清楚，免得用户以为切回查看就等于问题解决了
      expect(b.title).toContain("存不回")
      expect(b.title).toContain(readOnlyWhy(r))
    }
  })

  test("按钮与徽标的口径不变量：按钮禁用 ⇔ 徽标为 warn（同屏不得一个说能改、一个说不能）", () => {
    const inputs = [
      { statEditable: true, truncated: false, rootWritable: true },
      { statEditable: false, truncated: false, rootWritable: true },
      { statEditable: true, truncated: true, rootWritable: true },
      { statEditable: true, truncated: false, rootWritable: false },
      { statEditable: true, truncated: false, rootWritable: null },
    ] as const
    for (const input of inputs) {
      const ro = readOnlyReason(input)
      const badge = modeSlot({ mode: "view", readOnly: ro })
      const btn = editButton({ mode: "view", readOnly: ro })
      expect(btn.disabled).toBe(badge.cls === "warn")
      // 徽标说不可编辑时，按钮 title 必须给出同一句话里的同一原因
      if (badge.cls === "warn") expect(btn.title).toContain(readOnlyWhy(ro!))
    }
  })

  test("readOnlyWhy / readOnlyNext 对三因都有非空值（漏分支时编译期就该报错）", () => {
    for (const r of ["readonlyRoot", "truncated", "notEditable"] as const) {
      expect(readOnlyWhy(r).length).toBeGreaterThan(0)
      expect(readOnlyNext(r).length).toBeGreaterThan(0)
    }
  })
})

/**
 * **三口径一致性回归**（本单的收口锁）：同一组输入（`statEditable` / `truncated` / `rootWritable`）
 * 配上同一个 mode，三处消费必须自洽——
 *   ① 状态栏模式格徽标 `modeSlot`；② 标签栏编辑按钮 `editButton`；③ `saveTab` 的拒绝判定 `saveRefusal`。
 *
 * 为什么值得单开一块：这三处原先**各自拼输入**（按钮只判 `editable`、保存只看根 `rootsResp.writable`），
 * 于是同一组事实能得出两个结论。实测形态是同屏矛盾：截断的大文件、类型不可编辑的文件上，
 * 徽标写着 warn「不可编辑」、按钮是灰的、轮盘保存项也灰着写「存不回」，而 `Ctrl+S` 却真写下去了——
 * 写下去的还是**坏数据**（截断文件回写会把磁盘上的文件截成已加载的那一段）。
 * 前两块单测各自只盯自家函数，抓不到“两处对同一事实得出不同结论”；这里用**矩阵跑三处**补上。
 *
 * 钉住的是一条**逐因同一**的不变量（不是“大方向一致”）：根可写性已知时，保存侧拒绝的原因必须与
 * 展示侧 `readOnlyReason` 返回的**同一个枚举值**；唯一允许的分歧是根清单未到（`rootsPending`），
 * 因为此时展示侧按可写算（防冷启动假「不可编辑」）而保存侧无从判断只能拦住——这条也显式钉死，
 * 免得日后有人把 `saveRefusal` 简化成直接转调 `readOnlyReason` 而静默放行。
 */
describe("三口径一致性：同一组输入下 徽标 / 编辑按钮 / 保存拒绝 必须自洽", () => {
  /** 全量输入矩阵：三个来源 × 取值（含「未知」这一哨兵）的关键组合。 */
  const INPUTS = [
    { label: "普通可写文件", input: { statEditable: true, truncated: false, rootWritable: true } },
    { label: "根只读（GEBAI_FS_WRITE=false）", input: { statEditable: true, truncated: false, rootWritable: false } },
    { label: "超大文件截断", input: { statEditable: true, truncated: true, rootWritable: true } },
    { label: "类型/权限不可编辑", input: { statEditable: false, truncated: false, rootWritable: true } },
    { label: "根可写性未知（根清单未到）", input: { statEditable: true, truncated: false, rootWritable: null } },
  ] as const
  const MODES = ["view", "edit"] as const

  test("要求的三个基准形态：可写查看 / 不可写查看 / 不可写编辑", () => {
    // ① 可写查看 → 徽标**中性**「查看」，按钮可点
    const ok = INPUTS[0]!
    expect(modeSlot({ mode: "view", readOnly: readOnlyReason(ok.input) })).toMatchObject({ text: "查看", cls: "" })
    expect(editButton({ mode: "view", readOnly: readOnlyReason(ok.input) }).disabled).toBe(false)

    // ② 不可写查看（三因各一遍）→ 徽标 warn「不可编辑」+ 按钮 disabled + title 含原因
    for (const c of [INPUTS[1]!, INPUTS[2]!, INPUTS[3]!]) {
      const ro = readOnlyReason(c.input)
      expect(ro, `${c.label}：应当判为不可写`).not.toBeNull()
      const badge = modeSlot({ mode: "view", readOnly: ro })
      const btn = editButton({ mode: "view", readOnly: ro })
      expect(badge.text, c.label).toBe("不可编辑")
      expect(badge.cls, c.label).toBe("warn")
      expect(btn.disabled, c.label).toBe(true)
      expect(btn.title, `${c.label}：按钮 title 必须点名真正的原因`).toContain(readOnlyWhy(ro!))
    }

    // ③ 不可写编辑（Ctrl+E 不看可写性能进编辑态）→ 徽标 warn「不可保存」
    for (const c of [INPUTS[1]!, INPUTS[2]!, INPUTS[3]!]) {
      const badge = modeSlot({ mode: "edit", readOnly: readOnlyReason(c.input) })
      expect(badge.text, c.label).toBe("不可保存")
      expect(badge.cls, c.label).toBe("warn")
    }
  })

  test("逐因同一：根可写性已知时，保存侧拒绝的原因 == 展示侧判定的原因（同一枚举值）", () => {
    for (const c of INPUTS) {
      if (c.input.rootWritable == null) continue // 未知是刻意分歧，另见下一条
      const ro = readOnlyReason(c.input)
      expect(saveRefusal(c.input), `${c.label}：保存侧与展示侧给出了不同的原因`).toBe(ro)
    }
  })

  test("唯一的刻意分歧：根清单未到时展示按可写算（不闪假警告）、保存拦住（只能拦住）", () => {
    const pending = INPUTS[4]!
    expect(readOnlyReason(pending.input), "展示侧：未知按可写算，否则每次冷启动都会闪「不可编辑」").toBeNull()
    expect(saveRefusal(pending.input), "保存侧：无从判断只能拦住，放行会吃 403 或在只读站上真写一次").toBe("rootsPending")
    // 且这条分歧是显式具名的，不是靠“恰好都返回 null”蒙出来的
    expect(saveRefusalText("rootsPending")).toContain("未就绪")
  })

  test("同一组输入 × 两个 mode 全跑：三处的「不可写」判断必须同真同假", () => {
    for (const c of INPUTS) {
      if (c.input.rootWritable == null) continue
      const ro = readOnlyReason(c.input)
      const refusal = saveRefusal(c.input)
      for (const mode of MODES) {
        const badge = modeSlot({ mode, readOnly: ro })
        const btn = editButton({ mode, readOnly: ro })
        const tag = `${c.label} / ${mode}`
        // 徽标 warn ⇔ 判为不可写 ⇔ 保存侧同样拒绝（逐因同一）
        expect(badge.cls === "warn", `${tag}：徽标配色与判据不一致`).toBe(ro !== null)
        expect(refusal === null, `${tag}：存不存在拒绝与徽标不一致`).toBe(ro === null)
        // 查看态：按钮禁用 ⇔ 徽标 warn（同屏不得一个说能改、一个说不能）
        if (mode === "view") expect(btn.disabled, `${tag}：按钮禁用态与徽标不一致`).toBe(badge.cls === "warn")
        // 编辑态：按钮恒可点（否则用户被困在编辑态退不回来）
        else expect(btn.disabled, `${tag}：编辑态禁用 = 退不回查看态`).toBe(false)
        // warn 时三处必须说同一个原因（按钮 title / 徽标 title / 保存拒绝文案）
        if (ro) {
          expect(btn.title, `${tag}：按钮 title 未点名原因`).toContain(readOnlyWhy(ro))
          expect(badge.title, `${tag}：徽标 title 未点名原因`).toContain(readOnlyWhy(ro))
          expect(saveRefusalText(refusal!), `${tag}：保存拒绝文案未点名原因`).toContain(readOnlyWhy(ro))
          expect(saveRefusalText(refusal!), `${tag}：保存拒绝文案未给出下一步`).toContain(readOnlyNext(ro))
        }
      }
    }
  })

  test("轮盘保存项与 saveTab 同一判据：按钮灰着的理由 == 真按 Ctrl+S 收到的拒绝文案", () => {
    for (const c of INPUTS) {
      const refusal = saveRefusal(c.input)
      // 有未保存改动时，保存项禁用 ⇔ 保存会被拒（同生共死：点了才被拒 = 工具像坏了）
      const item = saveItem({ dirty: true, refusal })
      expect(item.disabled, `${c.label}：保存按钮可用性未跟随判据`).toBe(refusal !== null)
      // 保存项 title 只给「为什么」（一行放不下下一步）；「下一步」由拒绝 toast 承担。
      // 但**原因咬字必须逐字同源**：否则同屏又出现两种解释。
      if (refusal) expect(item.title, `${c.label}：保存项 title 与拒绝原因不同源`).toContain(saveRefusalWhy(refusal))
      // 无改动时按钮恒灰（与可写性无关），且不得误报「存不回」
      const clean = saveItem({ dirty: false, refusal })
      expect(clean.disabled, `${c.label}：无改动时保存按钮应恒灰`).toBe(true)
      expect(clean.title, `${c.label}：无改动时不应报存不回`).not.toContain("存不回")
    }
  })

  test("展示侧中性态：可写时一律不带 warn，也不宣称不可编辑/不可保存", () => {
    for (const mode of MODES) {
      const badge = modeSlot({ mode, readOnly: null })
      expect(badge.cls, `${mode}：可写态不得带 warn`).toBe("")
      expect(["查看", "编辑"]).toContain(badge.text)
      expect(badge.title).not.toContain("不可")
      expect(editButton({ mode, readOnly: null }).disabled, `${mode}：可写态按钮必须可点`).toBe(false)
    }
  })
})
