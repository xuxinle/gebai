import { describe, expect, test } from "bun:test"
import { gutterEligible, marksFromLineChanges, type GitLineChange } from "./git-gutter"

/** 便捷构造（现行空区间惯例：start = end + 1）。 */
function ch(o: [number, number], m: [number, number]): GitLineChange {
  return { originalStartLineNumber: o[0], originalEndLineNumber: o[1], modifiedStartLineNumber: m[0], modifiedEndLineNumber: m[1] }
}

describe("marksFromLineChanges", () => {
  test("纯插入：新行逐行标绿", () => {
    const marks = marksFromLineChanges([ch([3, 2], [3, 5])], 10)
    expect(marks).toEqual([
      { kind: "added", line: 3 },
      { kind: "added", line: 4 },
      { kind: "added", line: 5 },
    ])
  })

  test("纯插入（旧惯例 end=0）等价覆盖", () => {
    // 0.2x DiffComputer 输出：insertion 时 originalEndLineNumber = 0
    const marks = marksFromLineChanges([{ originalStartLineNumber: 3, originalEndLineNumber: 0, modifiedStartLineNumber: 3, modifiedEndLineNumber: 5 }], 10)
    expect(marks).toEqual([
      { kind: "added", line: 3 },
      { kind: "added", line: 4 },
      { kind: "added", line: 5 },
    ])
  })

  test("纯删除：删除点后第一行挂红三角", () => {
    const marks = marksFromLineChanges([ch([4, 6], [4, 3])], 10)
    expect(marks).toEqual([{ kind: "deleted", line: 4 }])
  })

  test("纯删除（旧惯例 end=0）等价覆盖", () => {
    const marks = marksFromLineChanges([{ originalStartLineNumber: 4, originalEndLineNumber: 6, modifiedStartLineNumber: 4, modifiedEndLineNumber: 0 }], 10)
    expect(marks).toEqual([{ kind: "deleted", line: 4 }])
  })

  test("修改：新行整块标蓝", () => {
    const marks = marksFromLineChanges([ch([2, 3], [2, 3])], 10)
    expect(marks).toEqual([
      { kind: "modified", line: 2 },
      { kind: "modified", line: 3 },
    ])
  })

  test("删除发生在文件末尾：锚行夹到最后一行", () => {
    // 原文件 5 行删掉末 2 行：modifiedStart = 4 > modifiedEnd = 3，文件现在共 3 行
    const marks = marksFromLineChanges([ch([4, 5], [4, 3])], 3)
    expect(marks).toEqual([{ kind: "deleted", line: 3 }])
  })

  test("多块按顺序输出；相邻块（前块纯删除 + 后块修改）锚行可同挂一行", () => {
    const marks = marksFromLineChanges([ch([2, 2], [2, 1]), ch([4, 4], [3, 3])], 6)
    expect(marks).toEqual([
      { kind: "deleted", line: 2 },
      { kind: "modified", line: 3 },
    ])
  })

  test("装饰行越界被夹取（diff 结果与 model 行数短暂不一致的窗口）", () => {
    const marks = marksFromLineChanges([ch([1, 9], [1, 9])], 4)
    expect(marks).toEqual([
      { kind: "modified", line: 1 },
      { kind: "modified", line: 2 },
      { kind: "modified", line: 3 },
      { kind: "modified", line: 4 },
    ])
  })

  test("双空区间（不存在于真实 diff）被丢弃，不产出越界标记", () => {
    expect(marksFromLineChanges([ch([3, 2], [3, 2])], 10)).toEqual([])
  })

  test("非数值输入按空区间丢弃", () => {
    expect(
      marksFromLineChanges([{ originalStartLineNumber: Number.NaN, originalEndLineNumber: 2, modifiedStartLineNumber: 3, modifiedEndLineNumber: 4 }], 10),
    ).toEqual([])
  })

  test("空差异 = 空标记（内容与 HEAD 一致）", () => {
    expect(marksFromLineChanges([], 100)).toEqual([])
  })
})

describe("gutterEligible", () => {
  test("全满足才适用", () => {
    const ok = { isRepo: true, headAvailable: true, truncated: false, editorKind: "monaco" as const }
    expect(gutterEligible(ok)).toBe(true)
    expect(gutterEligible({ ...ok, isRepo: false })).toBe(false)
    expect(gutterEligible({ ...ok, headAvailable: false })).toBe(false)
    expect(gutterEligible({ ...ok, truncated: true })).toBe(false)
    expect(gutterEligible({ ...ok, editorKind: "fallback" })).toBe(false)
  })
})
