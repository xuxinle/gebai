/**
 * 状态栏分支格的计数呈现规则（`files/git-counts.ts`）：非零才显示、用中文短词、冲突最优先。
 *
 * 守两件事：① 实测出来的那个缺陷——**只有冲突时不能显示成「三个零」**
 *（服务端 counts = {0,0,0,1} → 旧实现 `0± 0± 0?`，看着像工作区干净）；
 * ② 不再用 `!` `+` `±` `?` 这类需要查表才知道含义的符号（`±` 字形就是 "+−"，与 `+` 并排尤其难分）。
 */
import { describe, expect, test } from "bun:test"
import { branchTitle, changeParts } from "./git-counts"

const counts = (staged: number, unstaged: number, untracked: number, conflicted: number) => ({ staged, unstaged, untracked, conflicted })

describe("变更计数短词", () => {
  test("只给非零项，且按严重度排序：冲突 → 已暂存 → 未暂存 → 未跟踪", () => {
    expect(changeParts(counts(3, 4, 5, 1)).map((p) => p.text)).toEqual(["1冲突", "3暂存", "4改动", "5新增"])
  })

  test("纯冲突：不再出现「三个零」，而是明确给出「1冲突」", () => {
    expect(changeParts(counts(0, 0, 0, 1)).map((p) => p.text)).toEqual(["1冲突"])
  })

  test("只有未暂存：省掉两个零（旧实现是 0± 1± 0?）", () => {
    expect(changeParts(counts(0, 1, 0, 0)).map((p) => p.text)).toEqual(["1改动"])
  })

  test("工作区干净：空数组（调用方据此不渲染这一段）", () => {
    expect(changeParts(counts(0, 0, 0, 0))).toEqual([])
  })

  test("已暂存与未暂存各自成词（不再靠位置区分，也不再共用 ±）", () => {
    expect(changeParts(counts(2, 0, 0, 0)).map((p) => p.text)).toEqual(["2暂存"])
    expect(changeParts(counts(0, 2, 0, 0)).map((p) => p.text)).toEqual(["2改动"])
  })

  test("旧符号全部不再出现（! + ± ?）", () => {
    const all = changeParts(counts(2, 3, 4, 1)).map((p) => p.text).join("")
    expect(all).toBe("1冲突2暂存3改动4新增")
    for (const sym of ["!", "+", "±", "?"]) expect(all).not.toContain(sym)
  })
})

describe("分支格悬浮说明", () => {
  const base = { branch: "main", detached: false, ahead: 0, behind: 0, counts: counts(0, 0, 0, 0) }

  test("逐项写成中文（符号含义的唯一答案所在）", () => {
    const t = branchTitle({ ...base, counts: counts(3, 4, 5, 1) })
    expect(t).toContain("当前分支 main")
    expect(t).toContain("冲突待解决 1 个")
    expect(t).toContain("已暂存 3 个")
    expect(t).toContain("未暂存改动 4 个")
    expect(t).toContain("未跟踪新文件 5 个")
    expect(t).toContain("Alt+G")
  })

  test("领先/落后远程各自成句，为 0 时不出现", () => {
    expect(branchTitle({ ...base, ahead: 2, behind: 1 })).toContain("领先远程 2 个提交")
    expect(branchTitle({ ...base, ahead: 2, behind: 1 })).toContain("落后远程 1 个提交")
    const clean = branchTitle(base)
    expect(clean).not.toContain("领先远程")
    expect(clean).not.toContain("落后远程")
  })

  test("没有变更时说「工作区干净」（而不是留一句只有分支名的悬浮）", () => {
    expect(branchTitle(base)).toContain("工作区干净")
  })

  test("游离状态（detached）单独说清，不再当成一个分支名", () => {
    expect(branchTitle({ ...base, detached: true })).toContain("游离状态")
  })
})
