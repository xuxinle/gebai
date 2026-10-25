import { describe, expect, test } from "bun:test"
import { summarizePassedCheck, tailForFailure } from "./check-output"

describe("summarizePassedCheck（通过的检查项只留判定信号）", () => {
  test("逐条 (pass) 明细压成计数，失败/跳过用例逐条保留", () => {
    const text = [
      "bun test v1.3.14",
      "",
      "src/a.test.ts:",
      "(pass) 用例一 [1.2ms]",
      "(pass) 用例二 [0.3ms]",
      "(skipped) 用例三",
      "",
      " 5 pass",
      " 1 skip",
    ].join("\n")
    const out = summarizePassedCheck(text)
    expect(out).toContain("（通过 2 个用例，明细略）")
    expect(out).toContain("(skipped) 用例三")
    expect(out).toContain("5 pass")
    expect(out).not.toContain("用例一")
  })

  test("turbo 任务流水与构建脚本日志不进摘要", () => {
    const text = [
      "@gebai/server:typecheck: cache miss, executing 5bf3864cd6a3bbe6",
      "@gebai/server:typecheck: $ tsc --noEmit",
      "@gebai/server:build: [build-env-embed] 空态（还原）",
      "   • Packages in scope: @gebai/server, @gebai/web",
      "  • Remote caching disabled",
      "@gebai/sdk:build:   index.js  43.98 KB  (entry point)",
      "实际错误：src/x.ts(3,5): error TS2322",
    ].join("\n")
    const out = summarizePassedCheck(text)
    expect(out).toBe("实际错误：src/x.ts(3,5): error TS2322")
  })

  test("无用例时原样保留正文（typecheck/lint 的输出）", () => {
    const text = "  \nchecked 6 packages in 3.2s\n"
    expect(summarizePassedCheck(text)).toBe("checked 6 packages in 3.2s")
  })

  test("空输入给出明确文案", () => {
    expect(summarizePassedCheck("   \n\n")).toBe("（无输出）")
  })
})

describe("tailForFailure（失败项保留尾部与省略说明）", () => {
  test("超过上限只留尾部，并注明省略行数", () => {
    const lines = Array.from({ length: 300 }, (_, i) => `line ${i}`)
    const out = tailForFailure(lines.join("\n"))
    expect(out).toContain("前 180 行省略")
    expect(out).toContain("line 299")
    expect(out).not.toContain("line 100\n")
  })

  test("未超上限原样返回（去空行）", () => {
    expect(tailForFailure("a\n\nb\n")).toBe("a\nb")
  })
})
