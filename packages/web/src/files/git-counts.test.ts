/**
 * 状态栏分支格的计数呈现规则（`files/git-counts.ts`）：非零才显示、符号表意、冲突最优先。
 *
 * 重点守的是实测出来的那个缺陷：**只有冲突时不能显示成「三个零」**
 * （服务端 counts = {0,0,0,1} → 旧实现 `0± 0± 0?`，看着像工作区干净）。
 */
import { describe, expect, test } from "bun:test"
import { branchTitle, changeParts } from "./git-counts"

const counts = (staged: number, unstaged: number, untracked: number, conflicted: number) => ({ staged, unstaged, untracked, conflicted })

describe("变更计数片段", () => {
  test("只给非零项，且按严重度排序：冲突 → 已暂存 → 未暂存 → 未跟踪", () => {
    expect(changeParts(counts(3, 4, 5, 1)).map((p) => p.text)).toEqual(["1!", "3+", "4±", "5?"])
  })

  test("纯冲突：不再出现「三个零」，而是明确给出 1!", () => {
    expect(changeParts(counts(0, 0, 0, 1)).map((p) => p.text)).toEqual(["1!"])
  })

  test("只有未暂存：省掉两个零（旧实现是 0± 1± 0?）", () => {
    expect(changeParts(counts(0, 1, 0, 0)).map((p) => p.text)).toEqual(["1±"])
  })

  test("工作区干净：空数组（调用方据此不渲染这一段）", () => {
    expect(changeParts(counts(0, 0, 0, 0))).toEqual([])
  })

  test("符号各自表意：+ 已暂存 / ± 未暂存（不再靠位置区分两者）", () => {
    const onlyStaged = changeParts(counts(2, 0, 0, 0))
    const onlyUnstaged = changeParts(counts(0, 2, 0, 0))
    expect(onlyStaged.map((p) => p.text)).toEqual(["2+"])
    expect(onlyUnstaged.map((p) => p.text)).toEqual(["2±"])
  })
})

describe("分支格悬浮说明", () => {
  const base = { branch: "main", detached: false, ahead: 0, behind: 0, counts: counts(0, 0, 0, 0) }

  test("逐项写成中文（符号含义的唯一答案所在）", () => {
    const t = branchTitle({ ...base, counts: counts(3, 4, 5, 1) })
    expect(t).toContain("当前分支 main")
    expect(t).toContain("冲突待解决 1 个")
    expect(t).toContain("已暂存 3 个")
    expect(t).toContain("未暂存修改 4 个")
    expect(t).toContain("未跟踪文件 5 个")
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
