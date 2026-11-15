/**
 * PATH 查找（从 term-session 抽出的纯函数，终端域双模块共用：shell 探测与 PTY 编译器探测）。
 * Windows 按 PATHEXT 补扩展名探测；POSIX 直接查 PATH。
 *
 * **应用执行别名（app execution alias）必须能命中**：Microsoft Store 版的可执行文件（`pwsh.exe`、
 * 新版 `python.exe`、`wt.exe` 等）在 `%LOCALAPPDATA%\Microsoft\WindowsApps\` 下是一个**零长度
 * reparse point**，`statSync` 对它直接抛 `EACCES`（权限拒绝，因为它指向 WindowsApps 的受保护位置）。
 * 只按 `statSync().isFile()` 判定会把它们全部判成「不存在」——实测后果：本机装了 PowerShell 7
 * 却找不到 `pwsh.exe`，于是 `sh` 回落 Windows PowerShell 5.1，而 5.1 的 `-Encoding utf8` **必带 BOM**，
 * 命令写出的文件全都带上了 BOM（同时中文日志的编码路径也跟着变）。
 * 故：`statSync` 抛错但**路径存在**时按可执行候选接受（`lstatSync` 能看到 reparse point 本身）。
 */
import { existsSync, statSync } from "node:fs"
import { delimiter as pathDelimiter, join } from "node:path"

/** 该路径是否可作为一个命令候选：普通文件，或存在但 stat 不可达的重解析点（WindowsApps 别名）。 */
export function isCommandCandidate(path: string): boolean {
  try {
    return statSync(path).isFile()
  } catch {
    // statSync 失败但路径存在：WindowsApps 的应用执行别名（EACCES）/ 权限受限的等价物。
    // 不能用 lstatSync 再判 isFile()——reparse point 在那里是 symbolic link 形态（isFile() 为 false，
    // 而它实际可执行）。这里只确认「路径存在」，可执行性交给 spawn 的 ENOEXEC 报错。
    return existsSync(path)
  }
}

export function which(cmd: string): string | null {
  const isWin = process.platform === "win32"
  if (cmd.includes("/") || cmd.includes("\\")) return isCommandCandidate(cmd) ? cmd : null
  const exts = isWin ? (process.env.PATHEXT ?? ".COM;.EXE;.BAT;.CMD").split(";").filter(Boolean) : [""]
  const dirs = (process.env.PATH ?? "").split(isWin ? ";" : ":")
  for (const dir of dirs) {
    if (!dir) continue
    for (const ext of exts) {
      const name = ext && !cmd.toLowerCase().endsWith(ext.toLowerCase()) ? `${cmd}${ext}` : cmd
      const p = join(dir, name)
      if (isCommandCandidate(p)) return p
    }
  }
  return null
}

export { pathDelimiter as _pathDelimiterForTest }
