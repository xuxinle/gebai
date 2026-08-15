import { copyFileSync, mkdirSync } from "node:fs"
import { dirname, join } from "node:path"

// 原生 WebView 启动器（cargo release 产物）复制到 dist/ 与 gebai.exe 同目录分发
const src = join(import.meta.dir, "..", "launcher", "target", "release", "gebai-desktop.exe")
const dest = join(import.meta.dir, "..", "dist", "gebai-desktop.exe")
mkdirSync(dirname(dest), { recursive: true })
copyFileSync(src, dest)
console.log("[copy-launcher] gebai-desktop.exe -> dist/")
