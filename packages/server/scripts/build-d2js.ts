/**
 * 构建时生成 D2.js（@terrastruct/d2 node-esm 构建）内嵌产物注册表（`src/core/d2js.embedded.generated.json`）。
 *
 * 背景：@terrastruct/d2 的 node-esm 构建通过「文件路径 Worker」运行（`new Worker(join(dirname(import.meta.url), "worker.js"))`，
 * 运行时按相对路径读取 d2.wasm/elk.js 等文件），bun build/--compile 无法内联该 Worker 及其文件依赖。
 * 本脚本把 node-esm 构建的全部文件（wasm 经 gzip 压缩）以 base64 形式内嵌进 JSON——
 * 该 JSON 被 `core/diagram-render.ts` 静态 import 随产物打进二进制；二进制运行时物化到
 * `{GEBAI_HOME}/vendor/d2js/{version}/` 后动态 import（dev 模式不经此路径，直接 import 包）。
 *
 * 该文件为生成产物，已 gitignore，勿手改。
 */
import { readFileSync, writeFileSync, readdirSync, existsSync } from "node:fs"
import { join } from "node:path"
import { gzipSync } from "node:zlib"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/server
const srcDir = join(root, "node_modules", "@terrastruct", "d2", "dist", "node-esm")
const outFile = join(root, "src", "core", "d2js.embedded.generated.json")

/** 运行时 import 图涉及的全部文件（worker.js 读取 wasm/import wasm_exec；index.js import chunk/setup，elk.js 供 elk 布局动态加载）。 */
const D2JS_FILES = ["index.js", "worker.js", "wasm_exec.js", "setup.js", "chunk-ctcfg68w.js", "elk.js", "d2.wasm"]

const pkg = JSON.parse(readFileSync(join(root, "node_modules", "@terrastruct", "d2", "package.json"), "utf8"))
if (!existsSync(srcDir)) {
  console.warn("[build-d2js] 未找到 @terrastruct/d2 node-esm 构建（后端 D2 渲染将不可用），跳过生成")
  process.exit(0)
}
const files: Record<string, string> = {}
for (const name of D2JS_FILES) {
  const raw = readFileSync(join(srcDir, name))
  // 全部文件 gzip 后 base64（wasm 22MB → ~7MB；JS 文本同样受益）；运行时 Bun.gunzipSync 还原
  files[name] = gzipSync(raw).toString("base64")
}
writeFileSync(outFile, JSON.stringify({ version: pkg.version, gzip: true, files }, null, 1))
console.log(`[build-d2js] embedded ${D2JS_FILES.length} files (v${pkg.version}) -> ${outFile}`)
