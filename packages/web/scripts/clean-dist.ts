/**
 * 构建前安全清空 dist 目录。
 *
 * 背景：vite 内置 emptyDir（rmSync，无重试）在 Windows 上删除刚写入的大文件
 * （如 public/vendor/plantuml.js，9MB）或目录句柄被瞬时占用（防病毒扫描/写缓存）
 * 时会抛 ENOTEMPTY，导致 `vite build --watch` 整体退出；本脚本带重试删除，
 * 规避该竞态。vite 配置 `emptyOutDir: false`，dist 清理统一在此完成。
 */
import { rmSync } from "node:fs"
import { join } from "node:path"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/web
const dist = join(root, "dist")

const MAX_RETRIES = 5
const RETRY_DELAY_MS = 300

for (let attempt = 1; ; attempt++) {
  try {
    rmSync(dist, { recursive: true, force: true })
    break
  } catch (err) {
    if (attempt >= MAX_RETRIES) throw err
    const code = (err as NodeJS.ErrnoException).code ?? "UNKNOWN"
    console.warn(`[clean-dist] 删除 dist 失败（${code}），第 ${attempt}/${MAX_RETRIES} 次重试…`)
    await Bun.sleep(RETRY_DELAY_MS)
  }
}
console.log("[clean-dist] dist 已清理")
