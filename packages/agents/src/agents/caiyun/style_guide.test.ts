import { describe, expect, test } from "bun:test"
import { styleGuideTool } from "./style_guide"

/** 调一次工具，拿 output 文本。 */
async function run(args: Record<string, unknown>): Promise<string> {
  const r = await styleGuideTool.execute(args, {} as never)
  return String((r as { output: string }).output)
}

describe("style_guide 工具", () => {
  test("不传 name 时列出全部文档并给出读取提示", async () => {
    const out = await run({})
    expect(out).toContain("overview")
    for (const name of [
      "01-capability-list",
      "02-single-point",
      "03-causal-chain",
      "04-scale-dive",
      "05-contrast-split",
      "06-spine",
      "07-pointer",
    ]) {
      expect(out).toContain(name)
    }
    // 列表页必须把"先读 overview、再读一个"的用法写出来，
    // 否则智能体会一次读七套（那正是把它做成工具要避免的事）
    expect(out).toContain("先读 overview 选型")
    expect(out).toContain("不要一次全读")
  })

  test("按名读取返回全文（不是摘要）", async () => {
    const out = await run({ name: "03-causal-chain" })
    expect(out.startsWith("# 因果链")).toBe(true)
    expect(out).toContain("用裁云怎么落地")
    expect(out.length).toBeGreaterThan(1200)
  })

  test("overview 能读到选型表与硬门槛", async () => {
    const out = await run({ name: "overview" })
    expect(out).toContain("选型表")
    expect(out).toContain("承接判据")
    expect(out.startsWith("# 裁云 · 风格模板库（总纲）")).toBe(true)
  })

  test("未知名字给出可读清单，不抛错也不编造", async () => {
    const out = await run({ name: "no-such" })
    expect(out).toContain("未知文档")
    expect(out).toContain("overview")
  })

  test("名字归一：大小写与 .md 后缀都能读", async () => {
    expect(await run({ name: " 03-Causal-Chain.MD " })).toContain("# 因果链")
  })

  test("safeMode 为 true（只读文档，无副作用）", () => {
    expect(styleGuideTool.safeMode).toBe(true)
  })
})
