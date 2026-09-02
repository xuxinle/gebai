/** 目录递归遍历（自 core/tools.ts 抽取；grep 范围外路径与 code/explore 项目根遍历共用）。 */
import type { FileEntry } from "@gebai/sdk"

/** 目录递归遍历时跳过的大型/生成目录（grep 范围外路径与 code/explore 项目根遍历共用，防全量扫描拖慢）。 */
export const WALK_SKIP_DIRS = new Set([".git", "node_modules", "dist", "build", ".next", ".cache", "__pycache__", ".venv", "venv", "target", ".idea", ".vscode", "coverage", ".turbo"])
export const WALK_MAX_DEPTH = 10

/** 目录递归遍历（跳过大型/生成目录、深度上限；root 为单文件时直接返回单条）。pathBase 传入时输出路径带该前缀
 *  （tmp/项目根外搜索的结果路径可直接用于 read 等文件工具），缺省相对 root；root 不存在/不可读返回空。 */
export async function walkDirFiles(root: string, pathBase = ""): Promise<FileEntry[]> {
  const { readdir, stat } = await import("node:fs/promises")
  const st = await stat(root).catch(() => null)
  if (!st) return []
  if (st.isFile()) return [{ path: pathBase || root.replace(/\\/g, "/"), size: st.size, modifiedAt: st.mtimeMs, isDir: false }]
  const out: FileEntry[] = []
  const walk = async (dir: string, rel: string, depth: number): Promise<void> => {
    if (depth > WALK_MAX_DEPTH) return
    let entries
    try {
      entries = await readdir(dir, { withFileTypes: true })
    } catch {
      return
    }
    for (const e of entries) {
      if (e.isDirectory()) {
        if (WALK_SKIP_DIRS.has(e.name)) continue
        await walk(`${dir}/${e.name}`, rel ? `${rel}/${e.name}` : e.name, depth + 1)
      } else if (e.isFile()) {
        let size = 0
        try {
          size = (await stat(`${dir}/${e.name}`)).size
        } catch {
          /* stat 失败按 0 处理 */
        }
        const relPath = rel ? `${rel}/${e.name}` : e.name
        out.push({ path: pathBase ? `${pathBase}/${relPath}` : relPath, size, modifiedAt: 0, isDir: false })
      }
    }
  }
  await walk(root, "", 0)
  return out
}
