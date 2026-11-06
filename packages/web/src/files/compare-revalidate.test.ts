/**
 * 对比视图「切回标签即重验」的护栏。
 *
 * 守的问题（真实且已发生过）：对比标签页是个**快照**——建的时候拉一次差异，之后一直展示那份结果。
 * 文件被外部改（编辑器保存、git 操作、`sh` 脚本、别的工具）后不重验，用户会看到过期列表，
 * 以为"改了没生效"。
 *
 * 两条性质必须同时成立，缺一条都不可接受：
 * ① **真的重验了**：切回时必须向后端再拉一次（否则过期数据永远不会自己好）；
 * ② **没变就不动**：数据一致时不重绘、不显示加载态（否则每次切标签都闪一下、滚动位置全丢，
 *    比过期更难用——这是"正确地刷新"与"把界面刷新坏"的分界）。
 *
 * 这里的 diffSignature 是 ② 的判定依据，所以它必须**对内容敏感**（不能只看文件名数量）。
 */
import { describe, expect, test } from "bun:test"
import { diffSignature } from "./compare"

const f = (path: string, status = "modified", additions = 1, deletions = 0) => ({ path, status, additions, deletions })

describe("diffSignature：判断“重新拉到的结果与上次是否一致”", () => {
  test("完全相同的两份结果 → 签名相同（此时不应重绘）", () => {
    const a = [f("src/a.ts", "modified", 3, 2), f("src/b.ts", "added", 10, 0)]
    const b = [f("src/a.ts", "modified", 3, 2), f("src/b.ts", "added", 10, 0)]
    expect(diffSignature(a, 13, 2)).toBe(diffSignature(b, 13, 2))
  })

  test("**行数变了但文件数没变** → 签名必须不同（只看数量的实现会漏判，导致一直显示过期数据）", () => {
    const before = [f("src/a.ts", "modified", 3, 2)]
    const after = [f("src/a.ts", "modified", 7, 5)]
    expect(diffSignature(before, 3, 2)).not.toBe(diffSignature(after, 7, 5))
  })

  test("状态变化（M → D）→ 签名不同", () => {
    expect(diffSignature([f("a.ts", "modified")], 1, 0)).not.toBe(diffSignature([f("a.ts", "deleted", 0, 9)], 0, 9))
  })

  test("新增/删除文件 → 签名不同", () => {
    const before = [f("a.ts")]
    const after = [f("a.ts"), f("b.ts", "added")]
    expect(diffSignature(before, 1, 0)).not.toBe(diffSignature(after, 2, 0))
  })

  test("重命名带 oldPath → 签名不同（同名同状态但来源不同）", () => {
    const a = [{ path: "n.ts", status: "renamed", additions: 0, deletions: 0 }]
    const b = [{ path: "n.ts", status: "renamed", oldPath: "o.ts", additions: 0, deletions: 0 }]
    expect(diffSignature(a, 0, 0)).not.toBe(diffSignature(b, 0, 0))
  })

  test("共同祖先变化 → 签名不同（mergeBase 端点不同，差异含义也不同）", () => {
    const files = [f("a.ts")]
    expect(diffSignature(files, 1, 0, "abc123")).not.toBe(diffSignature(files, 1, 0, "def456"))
  })

  test("顺序敏感：同名集合但顺序不同视为不同（保序重绘才能与后端一致）", () => {
    const a = [f("a.ts"), f("b.ts")]
    const b = [f("b.ts"), f("a.ts")]
    expect(diffSignature(a, 2, 0)).not.toBe(diffSignature(b, 2, 0))
  })

  test("空结果（两端一致）签名稳定", () => {
    expect(diffSignature([], 0, 0)).toBe(diffSignature([], 0, 0))
    expect(diffSignature([], 0, 0)).not.toBe(diffSignature([f("a.ts")], 1, 0))
  })

  test("缺失字段（status/additions/deletions 为 undefined）不产生假差异", () => {
    const a = [{ path: "a.ts" }]
    const b = [{ path: "a.ts", status: undefined, additions: undefined, deletions: undefined }]
    expect(diffSignature(a, 0, 0)).toBe(diffSignature(b, 0, 0))
  })

  test("NaN/Infinity 等非有限值归一为 0（避免两次结果因同一个坏值被判成不同而反复重绘）", () => {
    expect(diffSignature([f("a.ts")], Number.NaN as unknown as number, 0)).toBe(diffSignature([f("a.ts")], 0, 0))
    expect(diffSignature([f("a.ts")], Number.POSITIVE_INFINITY as unknown as number, 0)).toBe(diffSignature([f("a.ts")], 0, 0))
  })
})
