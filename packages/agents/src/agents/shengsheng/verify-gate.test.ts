/**
 * 反向验证门禁判据（`verify-gate.ts`）的断言面。
 *
 * 本文件的重心不是「正常情况判对」，而是**对空壳的判别力**：每一条假货
 * 都要被某一档挡住，且**把那一档的判据退回旧写法必须变红**——否则
 * 一个恒绿的判定器会让「三缺一不算交付」在形式上成立而实际上全空。
 */
import { describe, expect, test } from "bun:test"
import { isPassing, isUndecidable, judge, looksEmpty, type Observation, type ReverseSpec } from "./verify-gate"

const spec: ReverseSpec = {
  cmd: "bun test x.test.ts",
  file: "src/x.ts",
  rollback: "return computeGuard(input)",
  defect: "return true",
}

const green: Observation = { code: 0 }
const red: Observation = { code: 1 }

/** 四档全过的观察集。 */
const allGood = { original: green, rolled: red, restored: green, original2: green, rolled2: red }

describe("反向验证门禁判据", () => {
  test("四档全过 ⇒ genuine，且被认作通过", () => {
    const r = judge(spec, allGood)
    expect(r.verdict).toBe("genuine")
    expect(isPassing(r)).toBe(true)
    expect(isUndecidable(r)).toBe(false)
  })

  // ── S0：最廉价的假货 ────────────────────────────────────────────
  test("S0：恒真动作不算反向验证（`exit 1` 这类黑名单之外，`true` 也挡）", () => {
    for (const cmd of ["true", ":", "exit 0", "true; true", "true && exit 0"]) {
      const r = judge({ ...spec, cmd }, allGood)
      expect(r.verdict).toBe("empty")
      // 关键：**哪怕观察集全绿也不能放过**——否则一个恒真命令靠「original 绿」就蒙过去
      expect(isPassing(r)).toBe(false)
    }
  })

  test("S0：剥离注释后为空也挡（`# 待补` 不是命令）", () => {
    expect(looksEmpty("# 待补")).toBe(true)
    expect(judge({ ...spec, cmd: "# 待补" }, allGood).verdict).toBe("empty")
  })

  test("S0：回退是空操作（rollback === defect）⇒ 干预不成立", () => {
    // 本仓真案例：空操作回退 ⇒ 行为没变 ⇒ 0 条红 ⇒ 假绿。
    const r = judge({ ...spec, defect: spec.rollback }, allGood)
    expect(r.verdict).toBe("empty")
    expect(r.reason).toContain("空操作")
  })

  test("S0：缺 rollback / defect ⇒ 没有干预就没有判定", () => {
    expect(judge({ ...spec, rollback: "  " }, allGood).verdict).toBe("empty")
    expect(judge({ ...spec, defect: "" }, allGood).verdict).toBe("empty")
  })

  test("反向：正常命令不得被 S0 误判为空（否则会挡住真交付）", () => {
    for (const cmd of ["bun test x.test.ts", "pytest -k smoke", "node --test a.mjs", "./build.sh && ./check.sh"]) {
      expect(looksEmpty(cmd)).toBe(false)
    }
  })

  // ── S1：交上来的东西本身不对 ────────────────────────────────────
  test("S1：original 是红的 ⇒ broken（不是「这单没通过」而是「它交的东西是坏的」）", () => {
    const r = judge(spec, { ...allGood, original: red, original2: red })
    expect(r.verdict).toBe("broken")
    expect(r.reason).toContain("本身就不对")
  })

  test("S1：命令没跑起来（code=null）⇒ not-run，绝不当成通过", () => {
    const r = judge(spec, { ...allGood, original: { code: null } })
    expect(r.verdict).toBe("not-run")
    expect(isPassing(r)).toBe(false)
    expect(isUndecidable(r)).toBe(true) // 不可判定 ≠ 失败
  })

  // ── S2：最核心的一档 —— 恒绿 ────────────────────────────────────
  test("S2：退回缺陷写法仍绿 ⇒ vacuous（这条反向验证没有判别力）", () => {
    const r = judge(spec, { ...allGood, rolled: green, rolled2: green })
    expect(r.verdict).toBe("vacuous")
    expect(r.reason).toContain("没有判别力")
  })

  test("S2：回退版编译不过（rolled code=null）⇒ not-run，不当 vacuous 也不当通过", () => {
    // 本仓已记形态：回退版跑不起来 ⇒ Bun 加载失败 ⇒ 看起来像「全部通过」的假绿。
    const r = judge(spec, { ...allGood, rolled: { code: null } })
    expect(r.verdict).toBe("not-run")
    expect(isPassing(r)).toBe(false)
  })

  // ── S3：红的归因 ────────────────────────────────────────────────
  test("S3：把 rollback 放回仍红 ⇒ wrong-cause（红不是 rollback 引起的）", () => {
    const r = judge(spec, { ...allGood, restored: red })
    expect(r.verdict).toBe("wrong-cause")
    expect(r.reason).toContain("不是 rollback 引起的")
    expect(isPassing(r)).toBe(false)
  })

  test("S3 判据的必要性：只有「放回又绿」才能把「命令本来就红」筛掉", () => {
    // 一个永不存在的文件：original 红、rolled 也红 —— 若没有 S3，S2 会把它当「退回会红」通过
    const bogus = { ...allGood, original: { code: 1 }, restored: { code: 1 }, original2: { code: 1 } }
    const r = judge(spec, bogus)
    // S1 先挡住它（original 就红）。把 S1 退掉后 S3 仍会挡——两档互为兜底的证据见下条。
    expect(r.verdict).toBe("broken")
    // 构造「original 绿、rolled 红、放回仍红」：只能由 S3 挡
    const onlyS3 = judge(spec, { ...allGood, restored: { code: 2 } })
    expect(onlyS3.verdict).toBe("wrong-cause")
  })

  // ── S4：抖动 ────────────────────────────────────────────────────
  test("S4：同态两次不一致 ⇒ unstable（不判通过也不判失败）", () => {
    const r = judge(spec, { ...allGood, rolled2: green })
    expect(r.verdict).toBe("unstable")
    expect(isPassing(r)).toBe(false)
    expect(isUndecidable(r)).toBe(true)
  })

  test("S4：只做一轮观察仍可通过，但要给「抖动无法排除」的提示", () => {
    const r = judge(spec, { original: green, rolled: red, restored: green })
    expect(r.verdict).toBe("genuine")
    expect(r.hints.join()).toContain("抖动无法排除")
  })

  // ── S5：只提示，不判失败（代价不对称）──────────────────────────
  test("S5：rollback 落在注释里给提示但**不判失败**", () => {
    const r = judge({ ...spec, rollback: "// return computeGuard(input)" }, allGood)
    expect(r.verdict).toBe("genuine")
    expect(r.hints.join()).toContain("注释行")
  })

  test("S5：rollback 段过短 / 目标不是源码 ⇒ 提示不改判决", () => {
    const short = judge({ ...spec, rollback: "return 1" }, allGood)
    expect(short.verdict).toBe("genuine")
    expect(short.hints.join()).toContain("过短")

    const doc = judge({ ...spec, file: "docs/x.md" }, allGood)
    expect(doc.verdict).toBe("genuine")
    expect(doc.hints.join()).toContain("不是源码文件")
  })

  // ── 判据整体：缺任何一档都不能通过 ──────────────────────────────
  test("缺任一档观察 ⇒ 一律不通过（四档是合取，不是任一）", () => {
    const keys = ["original", "rolled", "restored"] as const
    for (const k of keys) {
      const obs: Record<string, Observation> = { ...allGood }
      delete obs[k]
      const r = judge(spec, obs)
      expect(isPassing(r)).toBe(false)
      expect(r.verdict).toBe("not-run")
    }
  })

  test("空观察集 ⇒ not-run（绝不当成「没问题」）", () => {
    const r = judge(spec, {})
    expect(r.verdict).toBe("not-run")
    expect(isPassing(r)).toBe(false)
  })
})
