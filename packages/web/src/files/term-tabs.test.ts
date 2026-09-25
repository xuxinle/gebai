/**
 * 终端标签（`files/term-tabs.ts`，纯函数）。
 */
import { describe, expect, test } from "bun:test"
import { BUSY_WINDOW_MS, cleanTitle, resolveTabLabel, shouldConfirmClose, tabTooltip } from "./term-tabs"

describe("term-tabs（终端标签）", () => {
  test("标签名优先级：重命名 > shell 标题 > shell 名", () => {
    expect(resolveTabLabel({ shellName: "Bash", oscTitle: "npm run dev", custom: "构建" })).toBe("构建")
    expect(resolveTabLabel({ shellName: "Bash", oscTitle: "npm run dev" })).toBe("npm run dev")
    expect(resolveTabLabel({ shellName: "Bash" })).toBe("Bash")
    expect(resolveTabLabel({ shellName: "" })).toBe("终端")
  })

  test("标题清洗：去控制字符、去首尾空白、超长截断（程序把整条命令行当标题时标签不至于被撑破）", () => {
    expect(cleanTitle("  npm run dev \u0007 ")).toBe("npm run dev")
    expect(cleanTitle("a".repeat(200)).length).toBe(60)
    expect(cleanTitle(undefined)).toBe("")
  })

  test("tooltip 带出 shell、位置与退出码", () => {
    expect(tabTooltip({ label: "Bash", shellName: "Bash", cwd: "packages/web", alive: true })).toBe("Bash（Bash）· packages/web")
    expect(tabTooltip({ label: "构建", shellName: "Bash", cwd: "", alive: false, exitCode: 3 })).toBe("构建（Bash）· 会话根目录 · 已退出（退出码 3）")
    expect(tabTooltip({ label: "构建", shellName: "Bash", cwd: "", alive: false, exitCode: 0 })).toBe("构建（Bash）· 会话根目录 · 已退出")
  })

  test("关闭确认：仅在「活着且刚有输出」时问一次", () => {
    const now = 10_000
    expect(shouldConfirmClose({ alive: true, lastOutputAt: now - 100, now })).toBe(true)
    expect(shouldConfirmClose({ alive: true, lastOutputAt: now - BUSY_WINDOW_MS - 1, now })).toBe(false)
    expect(shouldConfirmClose({ alive: false, lastOutputAt: now, now })).toBe(false)
    expect(shouldConfirmClose({ alive: true, lastOutputAt: 0, now })).toBe(false)
  })
})
