/**
 * PATH 查找（`which`）的命令候选判定：必须认 **WindowsApps 的应用执行别名**。
 *
 * 为什么值得钉：Microsoft Store 版可执行文件（`pwsh.exe`、新版 `python.exe`、`wt.exe`）在
 * `%LOCALAPPDATA%\Microsoft\WindowsApps\` 下是零长度 reparse point，`statSync` 对它**抛 EACCES**。
 * 只按 `statSync().isFile()` 判定会把它们判成「不存在」——实测后果是本机装了 PowerShell 7
 * 却回落 Windows PowerShell 5.1，而 5.1 的 `-Encoding utf8` **必带 BOM**：`sh` 写出的文件全都带上 BOM。
 *
 * 判定逻辑导出为 `isCommandCandidate` 正是为了让这条口径可测：真别名造不出来，但装了 Store 版
 * PowerShell 的机器上就有一份现成样本（statSync 抛 EACCES、existsSync 为 true），用它钉住兜底分支。
 */
import { describe, expect, test } from "bun:test"
import { existsSync, mkdtempSync, rmSync, statSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { isCommandCandidate, which } from "./which"

describe("which：命令候选解析", () => {
  test("PATH 里的普通可执行文件能命中；不存在的不命中", () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-which-"))
    const prevPath = process.env.PATH
    const prevExt = process.env.PATHEXT
    try {
      writeFileSync(join(dir, "gebai-probe-tool.exe"), "")
      process.env.PATH = dir
      process.env.PATHEXT = ".EXE;.CMD"
      // PATHEXT 的扩展名按原样拼接（与 Windows 查找语义一致），故大小写不敏感比对
      const hit = which("gebai-probe-tool")
      expect(hit?.toLowerCase()).toBe(join(dir, "gebai-probe-tool.exe").toLowerCase())
      expect(which("gebai-no-such-tool")).toBeNull()
    } finally {
      process.env.PATH = prevPath
      if (prevExt === undefined) delete process.env.PATHEXT
      else process.env.PATHEXT = prevExt
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("带路径分隔符的命令按同一口径判定（普通文件命中、目录与缺失不命中）", () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-which-abs-"))
    try {
      const file = join(dir, "tool.exe")
      writeFileSync(file, "")
      expect(which(file)).toBe(file)
      expect(which(dir)).toBeNull() // 目录不是命令
      expect(which(join(dir, "missing.exe"))).toBeNull()
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("候选判定：普通文件算，目录与缺失路径不算", () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-which-cand-"))
    try {
      const file = join(dir, "a.exe")
      writeFileSync(file, "")
      expect(isCommandCandidate(file)).toBe(true)
      expect(isCommandCandidate(dir)).toBe(false)
      expect(isCommandCandidate(join(dir, "nope.exe"))).toBe(false)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("statSync 失败但路径存在时仍算候选（应用执行别名分支）", () => {
    /* 真别名造不出来（要 Store 应用的 reparse point），但**装了 Store 版 PowerShell 的机器上
     * 就有一份现成样本**：它对 statSync 抛 EACCES、existsSync 为 true。
     * 样本缺失时（无 Store 应用 / 非 Windows）跳过——不拿一个恒真断言冒充覆盖。 */
    const alias = process.env.LOCALAPPDATA
      ? join(process.env.LOCALAPPDATA, "Microsoft", "WindowsApps", "Microsoft.PowerShell_8wekyb3d8bbwe", "pwsh.exe")
      : ""
    const isAliasSample =
      !!alias &&
      existsSync(alias) &&
      (() => {
        try {
          statSync(alias)
          return false
        } catch {
          return true
        }
      })()
    if (!isAliasSample) return // 环境无此样本：跳过（不算「验过了」）
    expect(isCommandCandidate(alias)).toBe(true)
    // 也验证走 PATH 的完整查找同样能命中它
    expect(which("pwsh.exe")).toBe(alias)
  })
})
