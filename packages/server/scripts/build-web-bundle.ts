/**
 * 构建时生成 Web UI bundle 注册表（`src/core/web.bundle.generated.ts`）。
 *
 * 背景：`bun --compile` 单文件二进制模式下源码目录与 web dist 均不可用，
 * 服务端静态路由无法读取前端产物。本脚本把 `packages/web/dist` 全部文件
 * 以 base64 内联为常量映射（路径 → base64），随 bundle 内联进产物；
 * 二进制模式（`webDist` 不存在）时静态服务改从该映射读取，实现
 * 「单文件二进制自带 Web UI」。
 *
 * 幂等：由 server/desktop 构建前置调用；产物已 gitignore。
 */
import { readdirSync, readFileSync, existsSync } from "node:fs"
import { join, relative } from "node:path"

const webDist = join(import.meta.dirname, "..", "..", "web", "dist")
const outFile = join(import.meta.dirname, "..", "src", "core", "web.bundle.generated.ts")

if (!existsSync(webDist)) {
  console.warn("[build-web] web/dist 不存在，跳过 Web UI 内嵌（桌面端将无法提供 UI）")
  process.exit(0)
}

const files: string[] = []
function walk(dir: string) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name)
    const st = require("node:fs").statSync(p)
    if (st.isDirectory()) walk(p)
    else files.push(relative(webDist, p).replace(/\\/g, "/"))
  }
}
walk(webDist)

const entries = files
  .map((f) => `  ${JSON.stringify("/" + f)}: ${JSON.stringify(readFileSync(join(webDist, f), "base64"))},`)
  .join("\n")

const out = `// 由 scripts/build-web-bundle.ts 生成（gitignore）：web/dist 全量内嵌（base64）
export const webBundle: Record<string, string> = {
${entries}
}
`
require("node:fs").writeFileSync(outFile, out)
console.log(`[build-web] embedded ${files.length} web assets -> ${outFile}`)
