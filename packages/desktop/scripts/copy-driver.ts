import { copyFileSync, existsSync, mkdirSync } from "node:fs"
import { dirname, join } from "node:path"

// playwright 子Agent 的 node 桥接驱动：需作为独立文件随桌面端打包（Tauri resources 相对 src-tauri/）
const src = join(import.meta.dir, "..", "..", "server", "dist", "driver.mjs")
const dest = join(import.meta.dir, "..", "dist", "driver.mjs")
if (!existsSync(src)) {
  console.warn("[copy-driver] 未找到 server/dist/driver.mjs（playwright 子Agent 将不可用）")
} else {
  mkdirSync(dirname(dest), { recursive: true })
  copyFileSync(src, dest)
  console.log("[copy-driver] driver.mjs 已复制到 src-tauri/dist/")
}
