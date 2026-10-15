/**
 * **判据矩阵**：把 `status-core` 的可写性判据在「输入三轴 × 模式两态」上的**每一条组合**列成一行表，
 * 逐行断言**文案 / 配色 / 按钮禁用 / 拒绝文案**四件套。
 *
 * 与 `status-core.test.ts` 的分工（两者都要有，不是重复）：
 *   - `status-core.test.ts` 钉的是**语义契约**——「中性态与警示态必须可区分」「编辑态按钮恒可点」
 *     「三口径同真同假」这类跨组合的*不变量*，用例是围绕**理由**组织的；
 *   - 本文件钉的是**取值表**——每一格具体长什么样（`查看` vs `不可编辑` vs `不可保存`到底是哪个
 *     配色、哪句 title、按钮灰不灰），用例是围绕**输入组合**组织的。
 * 为什么需要后者：不变量测试有个固有盲区——**它只覆盖它枚举到的那几格**。实现里新增/改动一个分支
 * （比如给 `readOnlyReason` 加第四因、或在 `modeSlot` 里多加一种文案）时，只要不变量在旧组合上
 * 仍成立，测试会**继续全绿**，而新分支从未被任何一行覆盖到。取值表把「有哪些组合」显式写死，
 * 缺行是**看得见**的。
 *
 * ## 缺行是「一眼看出」的，靠四道机关（不是靠人记得回来补）
 * ① **行名携带三轴取值**（`statEditable=true × truncated=false × rootWritable=true` 这种），
 *    表与三轴笛卡尔积逐名对账——少一行即报出缺失的那一格叫什么。
 * ② **`DISPLAY` / `REFUSAL_WORDING` 是 `Record<判据枚举, …>`**：给 `ReadOnlyReason` 加一个成员而
 *    不同步加行，TypeScript 直接报「Property '新成员' is missing」——`bun run typecheck` 拦下。
 * ③ **`satisfies` + `Equal` 类型断言**（文末）：表里**实际用到**的字面量集合必须恰好等于
 *    `ReadOnlyReason` / `SaveRefusal` 联合本身。新分支若没进表，「声明了却没人用」在编译期就红。
 * ④ **导出面契约**：`status-core.ts` 导出的每个函数都必须在本文件被**调用**过——新导出一个判据
 *    函数而忘了给它排矩阵，红。
 *
 * 另外锁住一条**刻意的不对称**（唯一的例外，来自 `saveRefusal`）：根可写性未知（`rootWritable`
 * 为 null/undefined）时，展示侧按可写算（否则每次冷启动都闪一个跟文件无关的假「不可编辑」），
 * 而保存侧一律拦住（无从判断目标根，放行后吃 403 或在只读站上真写一次都更难懂）。
 *
 * 纯函数、零 DOM——文案与配色就是验收点，故断言直接打在文案/配色上（不需要渲染）。
 *
 * > 名称消歧：任务描述里的 `tabSlotState` **不是本仓导出**（仓库与设计文档的全历史搜索均无命中；
 * > 模式格渲染规格的名字是 `modeSlot`——`DESIGN.md`「文件工作台」一节有这条消歧）。本矩阵按**实际
 * > 导出面**排表：`readOnlyReason` / `modeSlot` / `saveRefusal` 三个判据函数 + 它们的三处消费规格
 * > （`editButton` / `saveItem`）与文案取词（`readOnlyWhy` / `readOnlyNext` / `saveRefusalWhy` /
 * > `saveRefusalText`）全在内；导出面契约用例会卡住「有导出却漏进矩阵」的情况。
 */
import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"
import {
  editButton,
  modeSlot,
  readOnlyNext,
  readOnlyReason,
  readOnlyWhy,
  saveItem,
  saveRefusal,
  saveRefusalText,
  saveRefusalWhy,
  type ReadOnlyReason,
  type SaveRefusal,
  type StatusTone,
  type WritabilityInput,
} from "./status-core"

/** 本文件的源码（导出面契约④要扫它）：路径由 `import.meta` 取，不依赖 cwd。 */
const SELF = readFileSync(import.meta.path, "utf8")
const CORE = readFileSync(join(import.meta.dir, "status-core.ts"), "utf8")

/**
 * `saveItem` 三种 title 的**逐字形态**（给矩阵断到 `===`，也给机关⑤ 做前缀对账）。
 * 手写而不是从函数输出反推：让「源码里写了什么」与「我以为写了什么」两边各自独立。
 */
const SAVE_ITEM_TITLES = {
  clean: "保存（Ctrl+S）",
  blockedPrefix: "保存（Ctrl+S）· 存不回：",
  dirty: "保存（Ctrl+S）· 有未保存的修改",
}

/* ------------------------------------------------------------------ *
 * 三轴枚举：矩阵的行由这三轴的笛卡尔积决定（18 组输入 × 2 个模式 = 36 格）
 * ------------------------------------------------------------------ */

/** 输入三轴的取值集（含「未知」这一哨兵——它是 `rootsPending` 与展示侧刻意分歧的来源）。 */
const AXIS = {
  /** 模式（状态栏「查看 / 编辑」格的两个态） */
  mode: ["view", "edit"],
  /** `stat.editable`：true / false / **undefined = stat 还没到** */
  statEditable: [true, false, undefined],
  /** 内容是否被截断 */
  truncated: [true, false],
  /** 当前根可写性：true / false / **null = 根清单未到（未知）** */
  rootWritable: [true, false, null],
} as const

/** 一格输入的**行名**：直接写明三轴取值——缺行时人一眼知道缺的是哪一格（机关①）。 */
function keyOf(se: boolean | undefined, t: boolean, rw: boolean | null): string {
  return `statEditable=${String(se)} × truncated=${String(t)} × rootWritable=${String(rw)}`
}

/* ------------------------------------------------------------------ *
 * 期望表：按判据（不是按 call site）组织，值全部手写
 * ------------------------------------------------------------------ */

/** 展示侧一格：文案 + 配色。 */
interface SlotExpect {
  text: string
  cls: StatusTone
}

/** 判据 → 展示与交互的期望。键是判据本身，故**加判据 = 编译期报缺键**（机关②）。 */
const DISPLAY: Record<"ok" | ReadOnlyReason, { view: SlotExpect; edit: SlotExpect }> = {
  /* 可写：查看态中性「查看」（**不是**「只读」——那正是本模块存在的理由：`只读` 在宣称文件写不了，
     而事实相反）、编辑态中性「编辑」。两者文案互斥、配色皆中性。 */
  ok: { view: { text: "查看", cls: "" }, edit: { text: "编辑", cls: "" } },
  /* 真不可写：一律 warn，且两个方向对称——查看态说「改不了」、编辑态说「存不下」。
     编辑态那一支不是冗余：`Ctrl+E` 不看可写性就能翻模式，此时若显示中性「编辑」，用户会一直
     以为改动存得下去。 */
  readonlyRoot: { view: { text: "不可编辑", cls: "warn" }, edit: { text: "不可保存", cls: "warn" } },
  truncated: { view: { text: "不可编辑", cls: "warn" }, edit: { text: "不可保存", cls: "warn" } },
  notEditable: { view: { text: "不可编辑", cls: "warn" }, edit: { text: "不可保存", cls: "warn" } },
}

/** 逐因措辞的**判定片断**（`toContain`）：「为什么」必须点到真正要改的东西，「下一步」必须可执行。 */
const REASON_WORDING: Record<ReadOnlyReason, { why: string; next: string }> = {
  // 根只读是**服务端开关**的事：必须点名 GEBAI_FS_WRITE，否则用户会去文件类型上找原因
  readonlyRoot: { why: "GEBAI_FS_WRITE", next: "服务端" },
  // 截断是**内容不完整**：为什么是「只加载了开头一段」，下一步是下载后编辑
  truncated: { why: "只加载了开头一段", next: "下载后编辑" },
  // 类型/权限：下一步是换本地编辑器
  notEditable: { why: "不支持编辑", next: "本地编辑器" },
}

/** `rootsPending` 的完整措辞（保存侧独有、**自成一句不给下一步**——它是「等一下」不是「去改什么」）。 */
const PENDING_TEXT = "根清单尚未就绪，请稍后重试"

/**
 * **期望表本体**：18 行 = 三轴笛卡尔积的每一格，逐行手写「判据」与「保存侧拒绝」。
 *
 * 三轴组合各自代表什么（写在这里是为了让下一眼看到某行时知道它在测什么）：
 *   - `rootWritable=false` → 整站只读根源，对每个文件都成立，优先级最高；
 *   - `truncated=true` → 只加载了前一段，保存会把文件截短（真数据损坏），仅次于根；
 *   - `statEditable=false` → 服务端按类型/权限判不可写，最后报；
 *   - `statEditable=undefined` → stat 未到，展示上**不当成不可写**（只有 `false` 才算）；
 *   - `rootWritable=null` → 根清单未到：展示按可写算，保存一律 `rootsPending`（唯一刻意分歧）。
 * 判序要求被这张表钉成事实：`readonlyRoot` 必须压过更局部的两类（否则用户以为「换个文件就能写」），
 * 截断必须压过类型不可编辑（连内容都不完整）。
 *
 * 用 `satisfies`（而非给变量加类型注解）是**有意的**：注解会把推断类型抹成声明类型，下面机关③
 * 的「表里实际用到哪些字面量」就取不到了——那正是要用来发现「加了分支却没进表」的东西。
 */
const TABLE = [
  // ── statEditable = true（stat 已到，文件本身可写）──────────────────────
  { statEditable: true, truncated: false, rootWritable: true, reason: null, refusal: null },
  { statEditable: true, truncated: false, rootWritable: false, reason: "readonlyRoot", refusal: "readonlyRoot" },
  // 根未到：展示说「可写」（不闪假警告），保存说「稍后重试」
  { statEditable: true, truncated: false, rootWritable: null, reason: null, refusal: "rootsPending" },
  { statEditable: true, truncated: true, rootWritable: true, reason: "truncated", refusal: "truncated" },
  { statEditable: true, truncated: true, rootWritable: false, reason: "readonlyRoot", refusal: "readonlyRoot" },
  // 叠加态（最坏形态：冷启动窗口里打开一个截断的大文件）：展示报已知的截断，保存报根未到
  { statEditable: true, truncated: true, rootWritable: null, reason: "truncated", refusal: "rootsPending" },
  // ── statEditable = false（服务端判不可写）───────────────────────────────
  { statEditable: false, truncated: false, rootWritable: true, reason: "notEditable", refusal: "notEditable" },
  { statEditable: false, truncated: false, rootWritable: false, reason: "readonlyRoot", refusal: "readonlyRoot" },
  { statEditable: false, truncated: false, rootWritable: null, reason: "notEditable", refusal: "rootsPending" },
  // 截断压过类型不可编辑：同为文件级，截断在前（它连内容都不完整）
  { statEditable: false, truncated: true, rootWritable: true, reason: "truncated", refusal: "truncated" },
  { statEditable: false, truncated: true, rootWritable: false, reason: "readonlyRoot", refusal: "readonlyRoot" },
  { statEditable: false, truncated: true, rootWritable: null, reason: "truncated", refusal: "rootsPending" },
  // ── statEditable = undefined（stat 未到：只有 false 才算不可写）────────
  { statEditable: undefined, truncated: false, rootWritable: true, reason: null, refusal: null },
  { statEditable: undefined, truncated: false, rootWritable: false, reason: "readonlyRoot", refusal: "readonlyRoot" },
  { statEditable: undefined, truncated: false, rootWritable: null, reason: null, refusal: "rootsPending" },
  { statEditable: undefined, truncated: true, rootWritable: true, reason: "truncated", refusal: "truncated" },
  { statEditable: undefined, truncated: true, rootWritable: false, reason: "readonlyRoot", refusal: "readonlyRoot" },
  { statEditable: undefined, truncated: true, rootWritable: null, reason: "truncated", refusal: "rootsPending" },
] as const satisfies readonly {
  statEditable: boolean | undefined
  truncated: boolean
  rootWritable: boolean | null
  reason: ReadOnlyReason | null
  refusal: SaveRefusal | null
}[]

/** 表的一行 → 送进被测函数的输入（**只取三轴**，防止行里混进别的字段当作输入被误用）。 */
function inputOf(r: (typeof TABLE)[number]): WritabilityInput {
  return { statEditable: r.statEditable, truncated: r.truncated, rootWritable: r.rootWritable }
}

/* ================================================================== *
 * 机关①：表必须**恰好**覆盖三轴笛卡尔积——缺一行即报出缺的那一格叫什么
 * ================================================================== */

describe("判据矩阵 · 表自身的完备性（缺行要看得见）", () => {
  test("表覆盖三轴笛卡尔积的每一格（3 × 2 × 3 = 18 行，不多不少）", () => {
    const expected: string[] = []
    for (const se of AXIS.statEditable) for (const t of AXIS.truncated) for (const rw of AXIS.rootWritable) expected.push(keyOf(se, t, rw))
    const actual = TABLE.map((r) => keyOf(r.statEditable, r.truncated, r.rootWritable))
    // 双向对账：缺行（expected 有而 actual 无）与多余/重复行（actual 有而 expected 无）都会列出名字
    expect([...actual].sort(), "表的行与三轴笛卡尔积不一致（左 = 实际，右 = 应有）").toEqual([...expected].sort())
    expect(actual.length, "行数必须 = 3 × 2 × 3 = 18").toBe(18)
    expect(new Set(actual).size, "同一格不得出现两行（重复行会让「有覆盖」变成错觉）").toBe(18)
  })

  test("组合格数 = 18 行 × 2 模式 = 36 格（本文件的断言规模是显式钉住的）", () => {
    expect(TABLE.length * AXIS.mode.length).toBe(36)
  })

  test("表里用到的**每一个**判据与拒绝原因都有对应的期望文案（机关②/④的运行期对账）", () => {
    // 展示侧：表里出现过的 null 与非 null 判据，`DISPLAY` 必须都能查到（Record 的键已由 TS 保证，
    // 这里再对一次，防的是「表里写了新判据字符串」这类绕过 TS 的写法）
    for (const r of TABLE) {
      const key = r.reason ?? "ok"
      const exp = DISPLAY[key]
      expect(exp, `${keyOf(r.statEditable, r.truncated, r.rootWritable)}：判据「${key}」没有期望文案`).toBeDefined()
      if (r.reason) expect(REASON_WORDING[r.reason], `判据「${r.reason}」缺逐因措辞片断`).toBeDefined()
    }
    // 反向：期望表里的键不得空转（判据删了、期望表没删 = 留着一份会腐坏的陈旧期望）
    const used = new Set<string>(TABLE.map((r) => r.reason ?? "ok"))
    expect([...Object.keys(DISPLAY)].sort()).toEqual([...used].sort())
    expect([...Object.keys(REASON_WORDING)].sort()).toEqual([...TABLE.flatMap((r) => (r.reason ? [r.reason] : []))].filter((v, i, a) => a.indexOf(v) === i).sort())
  })
})

/* ================================================================== *
 * 主体：36 格逐格断言「文案 / 配色 / 按钮禁用 / 拒绝文案」
 * ================================================================== */

describe("判据矩阵 · 36 格逐格断言（mode × 三轴）", () => {
  for (const r of TABLE) {
    const label = keyOf(r.statEditable, r.truncated, r.rootWritable)
    const input = inputOf(r)

    test(`${label} → 判据 ${r.reason ?? "null"} / 保存 ${r.refusal ?? "null"}`, () => {
      /* ① 判据本身（展示侧三口径共用的那一个） */
      expect(readOnlyReason(input), `${label}：readOnlyReason 与表不符`).toBe(r.reason)
      /* ② 保存侧拒绝（唯一允许分歧：根未知 → rootsPending） */
      expect(saveRefusal(input), `${label}：saveRefusal 与表不符`).toBe(r.refusal)

      const exp = DISPLAY[r.reason ?? "ok"]

      for (const mode of AXIS.mode) {
        const tag = `${label} / mode=${mode}`

        /* ③ 模式格徽标：文案 + 配色 */
        const slot = modeSlot({ mode, readOnly: r.reason })
        expect({ text: slot.text, cls: slot.cls }, `${tag}：模式格文案/配色与表不符`).toEqual(exp[mode])

        /* ④ 悬浮说明：warn 时必须点名原因；中性态不得宣称「不可」（那是不实提示） */
        if (r.reason) {
          expect(slot.title, `${tag}：title 未点名原因`).toContain(readOnlyWhy(r.reason))
          // 编辑态专有：必须说清「编辑了也存不回」，否则用户以为改动存得下去
          if (mode === "edit") expect(slot.title, `${tag}：编辑态 title 未说「存不回」`).toContain("存不回")
        } else {
          expect(slot.title, `${tag}：可写态 title 不得出现「不可」`).not.toContain("不可")
        }

        /* ⑤ 标签栏编辑按钮：**查看态**禁用 ⇔ 真不可写；**编辑态恒可点**（否则用户被困在编辑态退不出来） */
        const button = editButton({ mode, readOnly: r.reason })
        if (mode === "view") {
          expect(button.disabled, `${tag}：查看态按钮禁用态与判据不一致`).toBe(r.reason !== null)
          expect(button.active, `${tag}：查看态不应是按下态`).toBe(false)
          expect(button.icon, `${tag}：查看态图标应为 edit（去编辑）`).toBe("edit")
          if (r.reason) expect(button.title, `${tag}：按钮 title 未点名真正的原因`).toContain(readOnlyWhy(r.reason))
        } else {
          expect(button.disabled, `${tag}：编辑态禁用 = 用户退不回查看态`).toBe(false)
          expect(button.active, `${tag}：编辑态应为按下态`).toBe(true)
          expect(button.icon, `${tag}：编辑态图标应为 eye（回查看）`).toBe("eye")
          if (r.reason) expect(button.title, `${tag}：编辑态按钮未说「存不回」`).toContain("存不回")
        }
      }

      /* ⑥ 轮盘保存项：禁用态与 `saveTab` 同生共死；三种 title 逐字断（不只看子串） */
      const dirty = saveItem({ dirty: true, refusal: r.refusal })
      expect(dirty.disabled, `${label}：脏文件时保存项可用性与判据不一致`).toBe(r.refusal !== null)
      if (r.refusal) {
        /* 灰着的理由 == 真按 Ctrl+S 收到的理由（同源取词，否则同屏又出现两种解释）。
           这里断**逐字全等**而不是 `toContain`：子串断会放过「前缀拼错但后缀对」这类漂移，
           而这条 title 是用户想知道“为什么按不动”时唯一读得到的字。 */
        expect(dirty.title, `${label}：保存项 title 与拒绝原因不同源`).toBe(`${SAVE_ITEM_TITLES.blockedPrefix}${saveRefusalWhy(r.refusal)}`)
        expect(dirty.title, `${label}：保存项 title 不得带「下一步」（轮盘只有一行，下一步由扇形与 toast 承担）`).not.toContain(readOnlyNext(r.refusal === "rootsPending" ? "notEditable" : r.refusal))
      } else {
        expect(dirty.title, `${label}：可保存且有改动时的 title`).toBe(SAVE_ITEM_TITLES.dirty)
      }
      const clean = saveItem({ dirty: false, refusal: r.refusal })
      expect(clean.disabled, `${label}：无改动时保存项应恒灰`).toBe(true)
      expect(clean.title, `${label}：无改动时 title 不得随可写性变（否则一开文件就说「存不回」）`).toBe(SAVE_ITEM_TITLES.clean)
      expect(clean.title, `${label}：无改动时不得报「存不回」`).not.toContain("存不回")

      /* ⑦ 保存被拒的 toast 全文：有原因表的走「无法保存：why；next」 */
      if (r.refusal) {
        const text = saveRefusalText(r.refusal)
        if (r.refusal === "rootsPending") {
          expect(text, `${label}：根未就绪的提示应自成一句、不带「下一步」`).toBe(`无法保存：${PENDING_TEXT}`)
        } else {
          expect(text, `${label}：拒绝文案未点名原因`).toContain(readOnlyWhy(r.refusal))
          expect(text, `${label}：拒绝文案未给出下一步`).toContain(readOnlyNext(r.refusal))
          /* 模板顺序也是契约：「无法保存：**为什么**；**下一步**」。只查子串的话，把两者调换过来
             （`无法保存：请下载后编辑；文件过大…`）仍然全部命中——那句子读起来是把「怎么办」
             顶在「为什么」前面，且「下一步」在句首不像动作提示。故另断一次先后。 */
          expect(text.indexOf(readOnlyWhy(r.refusal)), `${label}：拒绝文案里「为什么」应排在「下一步」之前`).toBeLessThan(text.indexOf(readOnlyNext(r.refusal)))
        }
      }
    })
  }
})

/* ================================================================== *
 * 逐因措辞：文案是验收点，故把「点到什么」也钉进表
 * ================================================================== */

describe("判据矩阵 · 逐因文案表（为什么 + 下一步）", () => {
  for (const reason of Object.keys(REASON_WORDING) as ReadOnlyReason[]) {
    test(`${reason}：why 点到要害、next 可执行，且徽标/按钮/拒绝 toast 三处同源`, () => {
      const { why, next } = REASON_WORDING[reason]
      expect(readOnlyWhy(reason), `${reason}：why 未点出真正的阻碍`).toContain(why)
      expect(readOnlyNext(reason), `${reason}：next 未给出可执行动作`).toContain(next)
      // 三处共用同一份措辞：按钮 title / 徽标 title / 拒绝 toast 都必须含同一句 why
      expect(editButton({ mode: "view", readOnly: reason }).title).toContain(readOnlyWhy(reason))
      expect(modeSlot({ mode: "view", readOnly: reason }).title).toContain(readOnlyWhy(reason))
      expect(saveRefusalText(reason)).toContain(readOnlyWhy(reason))
      expect(saveRefusalText(reason)).toContain(readOnlyNext(reason))
      expect(saveRefusalText(reason).indexOf(readOnlyWhy(reason)), `${reason}：文案里「为什么」应在「下一步」之前`).toBeLessThan(saveRefusalText(reason).indexOf(readOnlyNext(reason)))
    })
  }

  test("三种原因的措辞两两不同（否则用户看不出该改什么——历史缺陷是一律说「类型不支持」）", () => {
    const reasons = Object.keys(REASON_WORDING) as ReadOnlyReason[]
    expect(new Set(reasons.map(readOnlyWhy)).size, "why 有重复 = 有两个原因在说同一句话").toBe(reasons.length)
    expect(new Set(reasons.map(readOnlyNext)).size, "next 有重复 = 下一步动作分不出来").toBe(reasons.length)
  })
})

/* ================================================================== *
 * 唯一刻意分歧：根可写性未知
 * ================================================================== */

describe("判据矩阵 · 根可写性未知（唯一刻意分歧：展示按可写算、保存一律拦住）", () => {
  const pendingRows = TABLE.filter((r) => r.rootWritable === null)

  test("根未就绪一族恰好 6 格（true/false/undefined × truncated），保存侧一律 rootsPending", () => {
    expect(pendingRows.length, "根未知的行数应为 statEditable 三取值 × truncated 两取值 = 6").toBe(6)
    for (const r of pendingRows) {
      expect(r.refusal, `${keyOf(r.statEditable, r.truncated, r.rootWritable)}：根未知时保存必须拦住`).toBe("rootsPending")
    }
  })

  test("展示侧不把「未知」当成「不可写」——否则每次冷启动都闪一个跟文件无关的假「不可编辑」", () => {
    // 无文件级阻碍时，根未知的格子展示必须是中性「查看」，而不是 warn
    const cleanPending = pendingRows.filter((r) => r.reason === null)
    expect(cleanPending.length, "根未知且无文件级阻碍应恰有 2 格（statEditable=true/undefined）").toBe(2)
    for (const r of cleanPending) {
      const slot = modeSlot({ mode: "view", readOnly: r.reason })
      expect({ text: slot.text, cls: slot.cls }, `${keyOf(r.statEditable, r.truncated, r.rootWritable)}：根未知被误报为不可写`).toEqual(DISPLAY.ok.view)
      expect(editButton({ mode: "view", readOnly: r.reason }).disabled, "根未知时按钮不得禁用（展示按可写算）").toBe(false)
    }
  })

  test("叠加态（根未知 × 截断/类型不可编辑）：两侧都拦住，各自按已知层级措辞", () => {
    const stacked = pendingRows.filter((r) => r.reason !== null)
    expect(stacked.length, "根未知且带文件级阻碍应恰有 4 格").toBe(4)
    for (const r of stacked) {
      const label = keyOf(r.statEditable, r.truncated, r.rootWritable)
      // 硬要求：展示侧 warn（拦住）且保存侧也拒绝——叠加态绝不能变成「徽标 warn、保存却放行」（放行即坏数据）
      expect(modeSlot({ mode: "view", readOnly: r.reason }).cls, `${label}：展示侧未拦住`).toBe("warn")
      expect(r.refusal, `${label}：保存侧未拦住`).not.toBeNull()
      // 展示侧报的是**已知的**那一个文件级阻碍（截断压过类型不可编辑），不含「未知」
      expect(r.reason, `${label}：展示侧应报已知的文件级阻碍`).toBe(r.truncated ? "truncated" : "notEditable")
    }
  })

  test("展示侧与保存侧的结论绝不同时为「放行」以外的矛盾态：拒绝方永远是超集", () => {
    for (const r of TABLE) {
      const shownWritable = r.reason === null
      const saveAllowed = r.refusal === null
      // 契约：可保存 ⇒ 展示为可写（反过来不成立——根未就绪那一族正是那个例外）
      if (saveAllowed) expect(shownWritable, `${keyOf(r.statEditable, r.truncated, r.rootWritable)}：保存放行但展示说不可写`).toBe(true)
    }
  })
})

/* ================================================================== *
 * 闭集：取值不得越界（新文案若没进期望表，会在这两处被看见）
 * ================================================================== */

/**
 * 取某个 `export function <name>(…) {…}` 的**函数体**（到行首的 `}` 为止）。
 *
 * 为什么要按函数体取而不是全文扫：闭集检查必须只扫**这一个渲染函数**内部写下的文案。
 * 全文扫会把 `REASON` 表、`PENDING`、`SaveItem` 的文案全扫进来，而它们各有自己的断言通道；
 * 更糟的是用关键词（`查看|编辑|保存|未就绪`）筛——**筛选条件本身就是一个漏口**，一个
 * 叫「加密」的新文案正好不含这些词，会静默通过（本轮实测撞到）。按函数体取就没有这个漏口。
 */
function fnBody(src: string, name: string): string {
  const start = src.indexOf(`export function ${name}(`)
  if (start < 0) return ""
  const end = src.indexOf("\n}", start)
  return end < 0 ? src.slice(start) : src.slice(start, end)
}

/** 函数体里全部 `text:` / `title:` 的**字面量部分**（去掉 `${…}` 插值，只留模板的字面前缀）。 */
function literalParts(body: string): string[] {
  const out: string[] = []
  for (const m of body.matchAll(/\b(?:text|title):\s*("(?:[^"\\]|\\.)*"|`(?:[^`\\]|\\.)*`)/g)) {
    out.push(m[1]!.slice(1, -1).replace(/\$\{[^}]*\}/g, ""))
  }
  return out
}

describe("判据矩阵 · 取值闭集（文案与配色的可能值就此穷举）", () => {
  test("模式格文案只有四种、配色只有两种，且四种文案**都被用到**（无死分支）", () => {
    const texts = new Set<string>()
    const classes = new Set<StatusTone>()
    for (const r of TABLE) {
      for (const mode of AXIS.mode) {
        const slot = modeSlot({ mode, readOnly: r.reason })
        texts.add(slot.text)
        classes.add(slot.cls)
      }
    }
    expect([...texts].sort(), "文案超出已知闭集 = 新增了一种状态没进期望表").toEqual(["不可保存", "不可编辑", "查看", "编辑"])
    expect([...classes].sort(), "配色超出闭集").toEqual(["", "warn"])
  })

  test("**编辑态 + 不可写**这一支必须真的被走到（否则「不可保存」是死文案）", () => {
    const editBlocked = TABLE.filter((r) => r.reason !== null).map((r) => modeSlot({ mode: "edit", readOnly: r.reason }))
    expect(editBlocked.length, "没有一行覆盖「编辑态 + 不可写」，则该分支是死代码").toBeGreaterThan(0)
    for (const s of editBlocked) expect(s.text).toBe("不可保存")
  })

  /**
   * 机关⑤：**运行期新分支闭集**——堵住机关③的已知盲区（见 DESIGN.md「变异验证 · 已知盲区」）。
   *
   * 盲区的形状：给 `modeSlot` 加一支「输入类型里根本构造不出来」的运行期分支（`readOnly as string`
   * 判定 / 新字段）时，表里没有一行能走到它，`DISPLAY` 里也没它的期望文案；而机关③ 比的是**表的
   * 字面量联合**，那种分支不会出现在表里 → 无论走 `tsc` 还是 `bun test` 全都**看不见它**。
   *
   * 这里换一个方向扫：把 `modeSlot` / `saveItem` 两个渲染函数的**函数体**取出来，函数体里所有
   * `text:` / `title:` 字面量必须**逐条**在矩阵的期望表里被断言过。新写一句用户能读到的文案、
   * 却没有任何一行矩阵断言它，就会在这里以「表外文案」列出来。
   *
   * 为什么按**函数体**取而不是全文扫 + 关键词筛：筛选条件本身就是个漏口——本轮实测的一个变异体
   * （`if ((o.readOnly as string) === "encrypted") return { text: "加密" … }`）正因为「加密」不含
   * `查看|编辑|保存` 任一关键词而静默通过。按函数体取就没有这个漏口：进了 `modeSlot` 体就是它的事。
   *
   * 口径边界（想清楚了再改）：
   *   - **只扫这两支渲染函数**——`REASON` 表的 why/next 已由 `REASON_WORDING` 逐因断过；
   *   - `PENDING_TEXT` 与 `saveItem` 的 title 也各自有断言通道（`===` / 前缀）；
   *   - 插值模板只取**字面前缀**（`编辑模式，但本文件存不回：` 之类）——`why`/`next` 的取值
   *     已分头断过，不必在建文案时再断一次。
   */
  test("机关⑤：`modeSlot` / `saveItem` 函数体内所有文案字面量都必须进过矩阵的期望表", () => {
    const covered = new Set<string>(Object.values(DISPLAY).flatMap((d) => [d.view.text, d.edit.text]))
    for (const t of Object.values(SAVE_ITEM_TITLES)) covered.add(t)

    const uncovered: string[] = []
    for (const fn of ["modeSlot", "saveItem"]) {
      const body = fnBody(CORE, fn)
      expect(body.length, `取不到 \`${fn}\` 的函数体 = 这道契约会空转`).toBeGreaterThan(0)
      for (const lit of literalParts(body)) {
        // 标题/文案模板：表里有完整版就用完整版，否则取前缀对账（插值部分已另有断言）
        const hit = [...covered].some((c) => c === lit || c.startsWith(lit) || lit.startsWith(c))
        if (!hit) uncovered.push(`${fn}: ${JSON.stringify(lit)}`)
      }
    }
    expect(uncovered, "这些文案在渲染函数里、却没有任何一行矩阵断言过（新增分支请补行）").toEqual([])
  })
})

/* ================================================================== *
 * 机关③ / ④：编译期与源码级的两道「新分支必须进表」
 * ================================================================== */

/** 严格相等（对联合类型敏感：`A` 与 `A | B` 不相等）。 */
type Equal<A, B> = (<T>() => T extends A ? 1 : 2) extends <T>() => T extends B ? 1 : 2 ? true : false
/** 断言为真——写不出 `true` 就是编译错误。 */
type Assert<T extends true> = T

/**
 * 机关③：**表里实际用到的字面量集合，必须恰好等于判据联合本身**。
 *
 * `satisfies` 保留了字面量推断，故 `(typeof TABLE)[number]["reason"]` 是「表里写过的原因」这个窄联合。
 * 于是给 `ReadOnlyReason` 加第四因、却在表里一行都没用上时，这里会报 `Type 'false' does not satisfy
 * the constraint 'true'`——把「声明了但没人用」变成 `bun run typecheck` 的失败（`bun test` 不做类型
 * 检查，故这道与机关②互补：② 管「新增判据缺期望行」，③ 管「新增判据没进输入矩阵」）。
 */
export type _ReadOnlyReasonFullyTabulated = Assert<Equal<ReadOnlyReason, NonNullable<(typeof TABLE)[number]["reason"]>>>
/** 同上：`SaveRefusal` 的四个成员（含保存侧独有的 `rootsPending`）都必须出现在表的「保存侧」列里。 */
export type _SaveRefusalFullyTabulated = Assert<Equal<SaveRefusal, NonNullable<(typeof TABLE)[number]["refusal"]>>>

describe("判据矩阵 · 导出面契约（机关④：新判据函数必须被排进矩阵）", () => {
  /** `status-core.ts` 里 `export function` 出来的函数名。 */
  const exportedFns = [...CORE.matchAll(/^export function (\w+)/gm)].map((m) => m[1]!)

  test("status-core 的每个导出函数都在本矩阵里被调用过（新增判据不得静默漏测）", () => {
    expect(exportedFns.length, "导出函数清单取空了 = 正则失效，这道契约会空转").toBeGreaterThanOrEqual(9)
    const untested = exportedFns.filter((name) => !new RegExp(`\\b${name}\\s*\\(`).test(SELF.replace(/\/\*[\s\S]*?\*\//g, "")))
    expect(untested, "这些导出函数没有出现在判据矩阵里（请为它补一张表）").toEqual([])
  })

  test("导出函数清单与设计文档记载的 9 个一致（改导出面必须同步 DESIGN.md）", () => {
    expect([...exportedFns].sort()).toEqual(["editButton", "modeSlot", "readOnlyNext", "readOnlyReason", "readOnlyWhy", "saveItem", "saveRefusal", "saveRefusalText", "saveRefusalWhy"])
  })
})
