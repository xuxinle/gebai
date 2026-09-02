/**
 * 构建时生成 tree-sitter 语法 wasm 内嵌产物注册表（`src/core/analyzer-wasm.embedded.generated.json`）。
 *
 * 背景：analyzer 的语法解析依赖 `tree-sitter-wasms` 包（node_modules 内 wasm 文件），
 * bun --compile 单二进制运行时无 node_modules，`require.resolve` 无法定位——analyze/search_symbols
 * 在二进制模式下完全不可用（此前还误报「不支持的语言」）。本脚本把 LANG_WASM 映射的全部语法
 * wasm（gzip 压缩后 base64）内嵌进 JSON，随产物打进二进制；analyzer.ts 在 require.resolve
 * 失败（二进制/打包模式）时回退内嵌注册表（`Bun.gunzipSync` 还原字节交 `Language.load`）。
 *
 * 该文件为生成产物，已 gitignore，勿手改。
 */
import { readFileSync, writeFileSync, existsSync } from "node:fs"
import { join } from "node:path"
import { gzipSync } from "node:zlib"
import { LANG_WASM } from "../src/core/support/analyzer"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/server
const wasmDir = join(root, "node_modules", "tree-sitter-wasms", "out")
const outFile = join(root, "src", "core", "analyzer-wasm.embedded.generated.json")

if (!existsSync(wasmDir)) {
  console.warn("[build-analyzer-wasm] 未找到 tree-sitter-wasms（analyze/search_symbols 的二进制回退将不可用），跳过生成")
  process.exit(0)
}

// 与 analyzer.ts 的 LANG_WASM 映射保持一致（脚本直接导入该表，无重复维护的清单）
const names = [...new Set(Object.values(LANG_WASM))]
const files: Record<string, string> = {}
for (const name of names) {
  const p = join(wasmDir, name)
  if (!existsSync(p)) {
    console.warn(`[build-analyzer-wasm] 缺失 ${name}，跳过（该语言二进制回退不可用）`)
    continue
  }
  // wasm 经 gzip 压缩后 base64（约 2~3 倍压缩率）；运行时 Bun.gunzipSync 还原
  files[name] = gzipSync(readFileSync(p)).toString("base64")
}

const pkg = JSON.parse(readFileSync(join(root, "node_modules", "tree-sitter-wasms", "package.json"), "utf8")) as { version?: string }
writeFileSync(outFile, JSON.stringify({ version: pkg.version ?? "unknown", gzip: true, files }, null, 1))
console.log(`[build-analyzer-wasm] embedded ${Object.keys(files).length} wasm files (tree-sitter-wasms v${pkg.version}) -> ${outFile}`)
