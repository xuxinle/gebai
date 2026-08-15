/**
 * 构建时生成 playwright 子Agent node 桥接驱动的内嵌产物（`src/core/driver.embedded.generated.json`）。
 *
 * 背景：driver.mjs 需保持独立文件供 node 子进程运行，bun build/--compile 无法内联；
 * 二进制形态下以 gzip base64 内嵌进产物，运行时物化到 `{GEBAI_HOME}/vendor/playwright/`
 * （与 build-d2js.ts 同思路的打包闭环）。服务端 dist（非编译）形态仍由 build-subagents.ts
 * 复制 driver.mjs 到 dist/ 与入口同目录。
 *
 * 该文件为生成产物，已 gitignore，勿手改。
 */
import { gzipSync } from "node:zlib"
import { readFileSync, writeFileSync } from "node:fs"
import { join } from "node:path"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/server
const src = join(root, "src", "sub-agents", "playwright", "driver.mjs")
const outFile = join(root, "src", "core", "driver.embedded.generated.json")

const raw = readFileSync(src)
writeFileSync(outFile, JSON.stringify({ gzip: true, driver: gzipSync(raw).toString("base64") }))
console.log(`[build-driver-embed] embedded driver.mjs (${raw.length} bytes) -> ${outFile}`)
