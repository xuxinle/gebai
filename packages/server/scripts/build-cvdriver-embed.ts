/**
 * 构建时生成 CV GPU sidecar 驱动（`src/core/cv/cv-driver.mjs`）的内嵌产物
 * （`src/core/cv/cvdriver.embedded.generated.json`）。
 *
 * 背景：cv-driver.mjs 需保持独立文件供 node 子进程运行（onnxruntime-node 原生推理），
 * bun build/--compile 无法内联；二进制形态下以 gzip base64 内嵌进产物，运行时物化到
 * `{GEBAI_HOME}/vendor/cv/`（与 build-driver-embed.ts 同思路的打包闭环）。服务端 dist
 * （非编译）形态仍由 build-subagents.ts 复制 cv-driver.mjs 到 dist/ 与入口同目录。
 * 注意：内嵌的只是驱动脚本本身——onnxruntime-node 依赖不随构建内嵌（体积/许可），
 * 运行时按 GEBAI_CV_ORT_NODE_DIR / node_modules 解析，缺失时检测自动回落 wasm CPU。
 *
 * 该文件为生成产物，已 gitignore，勿手改。
 */
import { gzipSync } from "node:zlib"
import { readFileSync, writeFileSync } from "node:fs"
import { join } from "node:path"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/server
const src = join(root, "src", "core", "cv", "cv-driver.mjs")
const outFile = join(root, "src", "core", "cv", "cvdriver.embedded.generated.json")

const raw = readFileSync(src)
writeFileSync(outFile, JSON.stringify({ gzip: true, driver: gzipSync(raw).toString("base64") }))
console.log(`[build-cvdriver-embed] embedded cv-driver.mjs (${raw.length} bytes) -> ${outFile}`)
