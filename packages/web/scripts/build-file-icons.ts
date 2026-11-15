/**
 * 生成文件类型图标表（`src/files/file-icons.generated.ts`）。
 *
 * 图标来源是 `material-icon-theme`（VSCode 同名图标主题，MIT），从中取**常用代码类型与常见文件类型**
 * 的一小份子集（见下方 ICON_IDS），把每个 SVG 的 viewBox 与内联图形抽成 TS 常量：
 * - **内联而不是走 URL**：目录树一次要画几十上百行，逐图标发请求会拖慢首屏并闪一下空图标；
 *   内联后 `fileIcon()` 与既有 `icon()` 同一套（缓存 + 克隆），且二进制形态无需额外静态资源。
 * - **只取子集**：上游 1256 个图标 / 1391 个扩展名映射远超需要，全量会把前端 chunk 推高一截。
 *
 * 产出的 `.generated.ts` **入库**（同 `env-embedded.generated.ts` 的策略）：干净克隆下
 * `bun test` / `tsc` 必须能直接跑，不能依赖「先跑一遍生成脚本」。上游版本变化时重跑本脚本即可。
 *
 * 用法：`bun run --cwd packages/web icons:build`
 */
import { readFileSync, writeFileSync } from "node:fs"
import { join } from "node:path"

const root = join(import.meta.dirname, "..")
const iconDir = join(root, "node_modules", "material-icon-theme", "icons")
const outFile = join(root, "src", "files", "file-icons.generated.ts")

/**
 * 取哪些图标（上游 `icons/<id>.svg`）。
 *
 * 口径是「常用」而不是「全」：按语言/构建工具各取一个代表，再补齐文档、媒体、压缩包、锁文件几类。
 * 新增类型时把 id 加进来重跑本脚本，再在 `files/file-icons.ts` 的映射表里指向它。
 */
const ICON_IDS = [
  // 代码
  "typescript", "react_ts", "react", "javascript", "vue", "svelte",
  "python", "go", "rust", "java", "kotlin", "swift", "c", "h", "hpp", "cpp", "csharp",
  "php", "ruby", "lua", "dart", "scala", "html", "css", "sass", "less",
  "console", "powershell", "database", "graphql", "proto", "lisp",
  // 配置与数据
  "json", "yaml", "toml", "xml", "settings", "tune",
  // 文档
  "markdown", "document", "pdf", "word", "table", "powerpoint", "readme", "license",
  // 媒体与字体
  "image", "svg", "video", "audio", "font",
  // 包与版本
  "zip", "lock", "log", "docker", "makefile", "cmake", "git", "nodejs", "npm", "bun", "pnpm",
  "tsconfig", "vite", "eslint", "prettier",
  // 兜底
  "file",
].filter((id, i, arr) => arr.indexOf(id) === i)

type Icon = { viewBox: string; body: string }

/** 抽 viewBox 与内联图形（去根元素，只留内容——外层 `<svg>` 由 `fileIcon()` 建）。 */
function readIcon(id: string): Icon {
  const svg = readFileSync(join(iconDir, `${id}.svg`), "utf8")
  const viewBox = /viewBox="([^"]+)"/.exec(svg)?.[1]
  const open = svg.indexOf(">", svg.startsWith("<?xml") ? svg.indexOf("?>") : 0)
  const close = svg.lastIndexOf("</svg>")
  if (!viewBox || open < 0 || close < 0) throw new Error(`图标格式无法识别：${id}.svg`)
  return { viewBox, body: svg.slice(open + 1, close).trim() }
}

const entries: string[] = []
for (const id of ICON_IDS) {
  const icon = readIcon(id)
  entries.push(`  ${JSON.stringify(id)}: { viewBox: ${JSON.stringify(icon.viewBox)}, body: ${JSON.stringify(icon.body)} },`)
}

/*
 * 映射表直接取上游的 `fileNames` / `fileExtensions`（VSCode 判定顺序也是「文件名优先、再扩展名」），
 * 只保留**图标在本子集内**的那些条：这样「哪个后缀用哪张图」不需要手抄，上游怎么分我们就怎么分。
 */
type Upstream = {
  fileNames: Record<string, string>
  fileExtensions: Record<string, string>
  languageIds: Record<string, string>
}
const upstream: Upstream = JSON.parse(readFileSync(join(root, "node_modules", "material-icon-theme", "dist", "material-icons.json"), "utf8"))
const picked = new Set(ICON_IDS)

/** 取映射并按图标过滤 + 键排序（排序使产物稳定，上游没变时重跑 diff 为空）。
 *  键统一转小写并去重（判定时也按小写查——上游存在 `Dockerfile`/`dockerfile` 这类大小写变体）。 */
function pickMap(src: Record<string, string>): [string, string][] {
  const out = new Map<string, string>()
  for (const [key, icon] of Object.entries(src)) {
    if (!picked.has(icon)) continue
    const k = key.toLowerCase()
    if (!out.has(k)) out.set(k, icon)
  }
  return [...out.entries()].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))
}

const byName = pickMap(upstream.fileNames)
const byExt = pickMap(upstream.fileExtensions)
/** 语言 id → 图标（`languageIds` 的键是小写语言名，如 `typescript`）。 */
const byLang = pickMap(upstream.languageIds)

function mapLiteral(rows: [string, string][]): string {
  return rows.map(([k, v]) => `  ${JSON.stringify(k)}: ${JSON.stringify(v)},`).join("\n")
}

const out = `// 由 scripts/build-file-icons.ts 生成（图标与映射表取自 material-icon-theme ${JSON.parse(readFileSync(join(root, "node_modules", "material-icon-theme", "package.json"), "utf8")).version}，MIT），请勿手改。
// 重新生成：bun run --cwd packages/web icons:build
export type FileIconSvg = { readonly viewBox: string; readonly body: string }

/** 图标 id → SVG（viewBox + 内联图形）。 */
export const FILE_ICON_SVG: Record<string, FileIconSvg> = {
${entries.join("\n")}
}

/** 完整文件名 → 图标 id（VSCode 同口径：文件名优先于扩展名）。 */
export const FILE_ICON_BY_NAME: Record<string, string> = {
${mapLiteral(byName)}
}

/** 扩展名（小写、不含点） → 图标 id。 */
export const FILE_ICON_BY_EXT: Record<string, string> = {
${mapLiteral(byExt)}
}

/** 语言 id（小写） → 图标 id。 */
export const FILE_ICON_BY_LANG: Record<string, string> = {
${mapLiteral(byLang)}
}
`
writeFileSync(outFile, out)
const bytes = Buffer.byteLength(out)
console.log(
  `[file-icons] ${ICON_IDS.length} 个图标 / ${byName.length} 条文件名 / ${byExt.length} 条扩展名 / ${byLang.length} 条语言 -> ${outFile}（${(bytes / 1024).toFixed(1)} KB）`,
)
