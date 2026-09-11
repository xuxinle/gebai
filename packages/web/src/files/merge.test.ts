/**
 * 冲突合并的纯逻辑测试（解析与解决）。
 *
 * 为什么值得专门测：合并会**改写用户文件**——解析漏掉一个块或替换错行，就是静默损坏代码。
 * 覆盖：标准两段式、diff3 三段式、多块、未闭合/畸形标记、CRLF、空侧、非冲突文本、
 * 批量解决的行号不偏移、`both` 合并顺序。
 */
import { describe, expect, test } from "bun:test"
import { applyResolution, blockSummary, hasConflictMarkers, parseConflictBlocks, resolutionLines } from "./merge"

const SAMPLE = [
  "line1", // 1
  "<<<<<<< HEAD", // 2
  "ours A", // 3
  "=======", // 4
  "theirs A", // 5
  ">>>>>>> feature", // 6
  "middle", // 7
  "<<<<<<< HEAD", // 8
  "ours B", // 9
  "=======", // 10
  "theirs B", // 11
  ">>>>>>> feature", // 12
  "tail", // 13
].join("\n")

describe("merge 解析冲突块", () => {
  test("标准两段式：块区间、两侧内容与标签", () => {
    const blocks = parseConflictBlocks(SAMPLE)
    expect(blocks).toHaveLength(2)
    expect(blocks[0]).toMatchObject({ startLine: 2, endLine: 6, ours: ["ours A"], theirs: ["theirs A"], oursLabel: "HEAD", theirsLabel: "feature" })
    expect(blocks[1]).toMatchObject({ startLine: 8, endLine: 12, ours: ["ours B"], theirs: ["theirs B"] })
    expect(blocks[0].base).toBeUndefined()
  })

  test("diff3 三段式：`|||||||` 段被识别为祖先内容", () => {
    const text = ["<<<<<<< HEAD", "ours", "||||||| merged common ancestors", "base", "=======", "theirs", ">>>>>>> feature"].join("\n")
    const [b] = parseConflictBlocks(text)
    expect(b).toMatchObject({ startLine: 1, endLine: 7, ours: ["ours"], base: ["base"], theirs: ["theirs"] })
    expect(blockSummary(b)).toContain("祖先 1 行")
  })

  test("空侧（一侧删除）→ 内容为空数组，仍算冲突块", () => {
    const text = ["<<<<<<< HEAD", "=======", "theirs only", ">>>>>>> other"].join("\n")
    const blocks = parseConflictBlocks(text)
    expect(blocks).toHaveLength(1)
    expect(blocks[0].ours).toEqual([])
    expect(blocks[0].theirs).toEqual(["theirs only"])
  })

  test("无冲突标记 → 空数组；hasConflictMarkers 一致", () => {
    const plain = "const a = 1\nconst b = 2\n"
    expect(parseConflictBlocks(plain)).toEqual([])
    expect(hasConflictMarkers(plain)).toBe(false)
    expect(hasConflictMarkers(SAMPLE)).toBe(true)
  })

  test("未闭合的 `<<<<<<<` 不算冲突块（宁可不识别，也不乱改内容）", () => {
    const broken = ["<<<<<<< HEAD", "ours", "=======", "theirs"].join("\n")
    expect(parseConflictBlocks(broken)).toEqual([])
    expect(applyResolution(broken, parseConflictBlocks(broken), "ours")).toBe(broken)
  })

  test("畸形嵌套：内层未闭合时不影响后续正常块", () => {
    const text = ["<<<<<<< HEAD", "ours", "<<<<<<< HEAD", "=======", "theirs", ">>>>>>> x", "after"].join("\n")
    const blocks = parseConflictBlocks(text)
    // 首个 `<<<<<<<` 到 `>>>>>>>` 之间存在嵌套标记，但按「首个开始 → 首个 >>>>>>> 结束」解析为一个块，
    // 内层标记作为内容行保留（git 正常不会产出嵌套，此处只保证不崩、不越界）
    expect(blocks.length).toBeGreaterThanOrEqual(1)
    expect(blocks[0].endLine).toBe(6)
  })

  test("CRLF 文件：标记行带 \\r 也能识别", () => {
    const text = ["a\r", "<<<<<<< HEAD\r", "ours\r", "=======\r", "theirs\r", ">>>>>>> x\r", "b\r"].join("\n")
    const blocks = parseConflictBlocks(text)
    expect(blocks).toHaveLength(1)
    expect(blocks[0].ours).toEqual(["ours\r"])
  })

  test("无标签的裸标记（`<<<<<<<` 后无空格）同样识别", () => {
    const text = ["<<<<<<<", "o", "=======", "t", ">>>>>>>"].join("\n")
    const blocks = parseConflictBlocks(text)
    expect(blocks).toHaveLength(1)
    expect(blocks[0].oursLabel).toBe("")
  })
})

describe("merge 解决冲突", () => {
  test("resolutionLines：ours/theirs/both/base 四种选择", () => {
    const [b] = parseConflictBlocks(["<<<<<<< H", "o1", "o2", "||||||| base", "b1", "=======", "t1", ">>>>>>> T"].join("\n"))
    expect(resolutionLines(b, "ours")).toEqual(["o1", "o2"])
    expect(resolutionLines(b, "theirs")).toEqual(["t1"])
    expect(resolutionLines(b, "base")).toEqual(["b1"])
    expect(resolutionLines(b, "both")).toEqual(["o1", "o2", "t1"]) // 我方在前，保持阅读顺序
    // 无 base 段时选 base → 空（等价「删除该块」）
    const [b2] = parseConflictBlocks(["<<<<<<< H", "o", "=======", "t", ">>>>>>> T"].join("\n"))
    expect(resolutionLines(b2, "base")).toEqual([])
  })

  test("单块解决：块被替换为选定内容，标记消失、其余内容不变", () => {
    const blocks = parseConflictBlocks(SAMPLE)
    const resolved = applyResolution(SAMPLE, [blocks[0]], "ours")
    expect(resolved.split("\n")).toEqual([
      "line1", "ours A", "middle",
      "<<<<<<< HEAD", "ours B", "=======", "theirs B", ">>>>>>> feature",
      "tail",
    ])
    expect(parseConflictBlocks(resolved)).toHaveLength(1)
  })

  test("批量解决全部：多块倒序替换，行号不偏移", () => {
    const blocks = parseConflictBlocks(SAMPLE)
    const resolved = applyResolution(SAMPLE, blocks, "theirs")
    expect(resolved.split("\n")).toEqual(["line1", "theirs A", "middle", "theirs B", "tail"])
    expect(hasConflictMarkers(resolved)).toBe(false)
  })

  test("两边都保留：内容顺序为我方 → 对方", () => {
    const resolved = applyResolution(SAMPLE, parseConflictBlocks(SAMPLE), "both")
    expect(resolved.split("\n")).toEqual(["line1", "ours A", "theirs A", "middle", "ours B", "theirs B", "tail"])
  })

  test("CRLF 文件解决后换行风格不变（每行仍以 \\r 结尾）", () => {
    const text = ["a\r", "<<<<<<< HEAD\r", "ours\r", "=======\r", "theirs\r", ">>>>>>> x\r", "b\r"].join("\n")
    const resolved = applyResolution(text, parseConflictBlocks(text), "theirs")
    expect(resolved.split("\n")).toEqual(["a\r", "theirs\r", "b\r"])
  })

  test("空侧 + 我方：整块被删除（不留空行）", () => {
    const text = ["keep1", "<<<<<<< HEAD", "=======", "theirs", ">>>>>>> x", "keep2"].join("\n")
    const resolved = applyResolution(text, parseConflictBlocks(text), "ours")
    expect(resolved.split("\n")).toEqual(["keep1", "keep2"])
  })

  test("传入空块列表 → 原文返回（幂等，不误改）", () => {
    expect(applyResolution("abc\ndef", [], "ours")).toBe("abc\ndef")
  })

  test("解决后再次解析为 0 冲突（假阴性防线）", () => {
    for (const choice of ["ours", "theirs", "both"] as const) {
      const resolved = applyResolution(SAMPLE, parseConflictBlocks(SAMPLE), choice)
      expect(hasConflictMarkers(resolved)).toBe(false)
    }
  })
})
