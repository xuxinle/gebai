/**
 * 仓库 BOM 自检的**判定核**（纯逻辑，无文件系统）：从「待检文件清单」里挑出以 UTF-8 BOM 开头的文件。
 *
 * 为什么需要：UTF-8 BOM（`EF BB BF`）在 UTF-8 里是**可选的签名**，留着没有编码收益，却会污染文件
 * 的第一个字符——JSON 解析器在部分实现下报「Unexpected token」、shebang 不再被 shell 认作第一行、
 * `git diff` 首行多一个零宽字符、构建脚本按首字符判定类型时误判。而它最容易从两处进来：
 * ① PowerShell 5.1 的 `Set-Content`/`Out-File -Encoding utf8`（该版本固有一行为，pwsh 7 才不带）；
 * ② Windows 工具（记事本旧版、Visual Studio 的「带签名保存」）保存的文件。
 * 工具侧的写入口径已统一为「不带 BOM」（见 `core/tools/fs.ts` 的 write/edit），本闸门守的是
 * **仓库自身**：任何人用上面那些工具改过的源码不应进库。
 *
 * 只在**文本类**文件上判定：二进制/图片/字体等本来就可能有 `EF BB BF` 字节（那是数据，不是 BOM），
 * 按扩展名白名单排除——误报会让闸门被无视，比漏报更糟。
 */

/** 可能带 BOM 的文本扩展名（其余一律不检：二进制里出现同样三字节是数据）。 */
export const BOM_CHECK_EXTENSIONS: ReadonlySet<string> = new Set([
  ".ts",
  ".tsx",
  ".mts",
  ".cts",
  ".js",
  ".mjs",
  ".cjs",
  ".jsx",
  ".json",
  ".jsonc",
  ".md",
  ".markdown",
  ".css",
  ".scss",
  ".sass",
  ".less",
  ".html",
  ".htm",
  ".vue",
  ".svelte",
  ".py",
  ".go",
  ".rs",
  ".java",
  ".kt",
  ".kts",
  ".swift",
  ".c",
  ".h",
  ".cc",
  ".cpp",
  ".hpp",
  ".cs",
  ".rb",
  ".php",
  ".sh",
  ".bash",
  ".zsh",
  ".yml",
  ".yaml",
  ".toml",
  ".ini",
  ".cfg",
  ".conf",
  ".xml",
  ".sql",
  ".txt",
  ".mmd",
  ".puml",
  ".d2",
  ".cmake",
  ".pkg",
])

/**
 * **可选的 BOM 例外：PowerShell 脚本必须保留 BOM**。
 *
 * 与“无 BOM 更干净”相反——PowerShell 5.1 读 `.ps1` 时，**无 BOM 按 ANSI 代码页（简中系统为 GBK）解码**，
 * 于是文件里的中文被解成乱码；更糟的是它不一定报错：实测一段带中文赋值的脚本去 BOM 后
 * **整个脚本静默无输出**（解析失败而不抛可见错误）。故 `.ps1` 是**必须带 BOM** 的例外，
 * `.bat`/`.cmd`（cmd.exe 同样按代码页读）也一并归入例外，不进本闸门的检出范围。
 * `.sh` 不在此列：POSIX shell 不看 BOM（且带 BOM 会误读 shebang，反而必须无 BOM）。
 */
export const BOM_REQUIRED_EXTENSIONS: ReadonlySet<string> = new Set([".ps1", ".bat", ".cmd"])

/** UTF-8 BOM 三个字节。 */
export const UTF8_BOM = [0xef, 0xbb, 0xbf] as const

/** 该路径是否需要 BOM 检查（按扩展名，大小写不敏感）。PowerShell/批处理脚本须带 BOM，不检。 */
export function needsBomCheck(relative: string): boolean {
  const dot = relative.lastIndexOf(".")
  if (dot < 0) return false
  const ext = relative.slice(dot).toLowerCase()
  if (BOM_REQUIRED_EXTENSIONS.has(ext)) return false
  return BOM_CHECK_EXTENSIONS.has(ext)
}

/** 字节流是否以 UTF-8 BOM 开头。 */
export function hasUtf8Bom(bytes: Uint8Array | ArrayLike<number>): boolean {
  const n = bytes.length
  return n >= 3 && bytes[0] === UTF8_BOM[0] && bytes[1] === UTF8_BOM[1] && bytes[2] === UTF8_BOM[2]
}

/**
 * 从「待检文件清单」里挑出带 BOM 的（判定核入口）。
 * `readHead` 只读每个文件的**前 3 字节**（大仓库不该为了查文件头把整个文件读进来）。
 */
export function findBomOffenders(files: readonly string[], readHead: (rel: string) => Uint8Array | ArrayLike<number> | null): string[] {
  const out: string[] = []
  for (const rel of files) {
    if (!needsBomCheck(rel)) continue
    const head = readHead(rel)
    if (head && hasUtf8Bom(head)) out.push(rel)
  }
  return out
}
