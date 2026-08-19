/**
 * 构建时生成 playwright-core 内嵌产物（`src/core/pwcore.embedded.generated.json`）。
 *
 * 背景：bun --compile 单二进制形态在用户机器上不可依赖 node_modules（`Bun.resolveSync`
 * 锚定真实 CWD 的可达性），playwright 模块须随产物内嵌。浏览器本体不内嵌：Windows 默认
 * 经 channel=msedge 驱动系统自带 Edge（见 playwright_tools.ts resolveChannel），Linux
 * 服务端部署走 node_modules 的 playwright 包 + 已安装 chromium（回退路径）。
 * playwright-core 是多文件包树（无法像 driver.mjs/d2js 那样单文件内联），故整树按文件
 * gzip base64 内嵌，运行时物化到 `{GEBAI_HOME}/vendor/playwright-core/`（与 d2js 同思路
 * 的打包闭环）。
 *
 * 该文件为生成产物，已 gitignore，勿手改。
 */
import { gzipSync } from "node:zlib"
import { readdirSync, readFileSync, statSync, writeFileSync } from "node:fs"
import { dirname, join, relative } from "node:path"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/server
const outFile = join(root, "src", "core", "pwcore.embedded.generated.json")

// 定位 playwright-core：跟随 playwright 包解析位置（显式声明 playwright-core 依赖易与其版本错位）
const pwEntry = Bun.resolveSync("playwright", root)
const coreEntry = Bun.resolveSync("playwright-core", dirname(pwEntry))
const coreRoot = dirname(coreEntry)

// 运行时不需要的文件不内嵌（类型声明/源码映射/文档），省体积
function skip(rel: string): boolean {
  const segs = rel.split(/[\\/]/)
  if (segs.some((s) => s === "types" || s === ".github" || s === "node_modules")) return true
  return /\.(d\.(ts|mts|cts)|map|md)$/i.test(rel)
}

const files: Array<{ path: string; data: string }> = []
function walk(dir: string): void {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name)
    const rel = relative(coreRoot, p).split("\\").join("/")
    if (skip(rel)) continue
    if (e.isDirectory()) walk(p)
    else if (e.isFile()) files.push({ path: rel, data: gzipSync(readFileSync(p)).toString("base64") })
  }
}
walk(coreRoot)
if (files.length === 0) throw new Error(`[build-pwcore-embed] playwright-core 目录为空（${coreRoot}），产物异常`)

// 入口优先 ESM（index.mjs）；版本号对内容敏感（路径+gzip 数据全量哈希），升级即触发运行时重建
const entry = statSync(join(coreRoot, "index.mjs")).isFile() ? "index.mjs" : "index.js"
const version = String(Bun.hash(files.map((f) => `${f.path}:${f.data}`).join("\n")))
writeFileSync(outFile, JSON.stringify({ version, entry, files }))
console.log(
  `[build-pwcore-embed] embedded playwright-core (${files.length} files, base64 ${Math.round(files.reduce((n, f) => n + f.data.length, 0) / 1024)} KB) -> ${outFile}`,
)
