/**
 * 构建/开发前把图表渲染引擎的依赖产物原样拷贝到 public/vendor/（gitignore），运行时由 diagram.ts
 * 以稳定文件名按需加载：
 * - plantuml.js：@plantuml/core 上游 TeaVM 编译单文件（约 6.9MB 自包含 ESM）。若走 vite/rollup
 *   打包链路，构建耗时约 11s（占 web 构建 90%+）；改以静态资源原样伺服，构建时间降至 ~1s。
 * - viz-global.js：PlantUML 依赖的 Graphviz 布局（classic script 注入全局）。
 * - mermaid.js：mermaid 官方 dist/mermaid.min.js（约 3.5MB 自包含 UMD，含全部图型）。
 * - echarts.js：echarts 官方 dist/echarts.min.js（约 1MB 自包含 UMD，含 SVG 渲染器，SSR 模式输出 SVG 字符串）。
 * - d2js/：@terrastruct/d2 官方浏览器构建目录（index.js + worker.js + wasm 等，内部相对路径引用）。
 *
 * 背景：mermaid/@terrastruct/d2 若走 vite 自动分包会生成**带内容 hash 的文件名**，开发模式重建后旧页面
 * 仍引用旧 hash 分块 → 404「Failed to fetch dynamically imported module」。稳定文件名 + 静态伺服后
 * 重建 URL 不变，动态加载资源 404 从根上消除（diagram.ts 仍保留整页刷新兜底）。
 *
 * 这些文件为依赖产物，已 gitignore，勿手改。
 */
import { copyFileSync, existsSync, mkdirSync, readdirSync, rmSync, statSync } from "node:fs"
import { dirname, join } from "node:path"
import { VENDOR_GROUPS, resolveVendorGroups, type VendorGroup } from "./vendor-groups"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/web
const vendor = join(root, "public", "vendor")

/* ---------- 构建期裁剪：`GEBAI_WEB_VENDOR` 逗号包含清单（缺省 = 全量，解析见 vendor-groups.ts）----------
 * 裁剪单位是「引擎组」，供领域专用镜像剔除前端用不到的资源（monaco 24MB / plantuml+viz 8MB /
 * d2js 8MB …）。未包含的组会被删除（残留旧文件会让裁掉的能力仍可用，与清单不符）；前端对各
 * 引擎均为懒加载 + 失败降级（如 Monaco 失败降级为轻量编辑器），缺失只降能力面、不破主界面。 */
let groups: Set<VendorGroup>
try {
  groups = resolveVendorGroups(process.env.GEBAI_WEB_VENDOR)
} catch (err) {
  console.error(`[build-vendor] ${err instanceof Error ? err.message : String(err)}`)
  process.exit(1)
}
/** 组 → 落盘路径（供未包含组的清理）。 */
const GROUP_TARGETS: Record<VendorGroup, string[]> = {
  monaco: [join(vendor, "monaco")],
  plantuml: [join(vendor, "plantuml.js"), join(vendor, "viz-global.js")],
  mermaid: [join(vendor, "mermaid.js")],
  echarts: [join(vendor, "echarts.js")],
  d2js: [join(vendor, "d2js")],
  xterm: [join(vendor, "xterm")],
  tree_sitter: [join(vendor, "tree-sitter")],
}

/** 拷贝单文件：内容大小未变化时跳过写入，避免 mtime 扰动（vite 监视 public/ 变更会触发页面 reload）。 */
function copyFileIfChanged(src: string, out: string, label: string): void {
  if (!existsSync(src)) {
    console.error(`[build-vendor] 找不到 ${src}，请先执行 bun install`)
    process.exit(1)
  }
  if (existsSync(out) && statSync(out).size === statSync(src).size) {
    console.log(`[build-vendor] ${label} 已就绪，跳过拷贝`)
    return
  }
  mkdirSync(dirname(out), { recursive: true })
  copyFileSync(src, out)
  console.log(`[build-vendor] ${label} -> ${out}`)
}

/** 拷贝目录（逐文件大小比对跳过）：d2 浏览器构建内部按相对路径引用 worker/wasm/chunk 文件，必须整体伺服。 */
function copyDirIfChanged(srcDir: string, outDir: string, label: string): void {
  if (!existsSync(srcDir)) {
    console.warn(`[build-vendor] 未找到 ${label}（${srcDir}），跳过拷贝，前端 ${label} 渲染不可用`)
    return
  }
  mkdirSync(outDir, { recursive: true })
  let copied = 0
  for (const name of readdirSync(srcDir)) {
    const s = join(srcDir, name)
    if (!statSync(s).isFile()) continue
    const o = join(outDir, name)
    if (existsSync(o) && statSync(o).size === statSync(s).size) continue
    copyFileSync(s, o)
    copied++
  }
  console.log(`[build-vendor] ${label} -> ${outDir}${copied ? `（${copied} 个文件）` : "（已就绪，跳过）"}`)
}

/** 递归拷贝目录（逐文件大小比对跳过；可用于含子目录的树）。filter 可选：返回 false 的文件跳过。 */
function copyTreeIfChanged(srcDir: string, outDir: string, label: string, filter?: (rel: string) => boolean): number {
  if (!existsSync(srcDir)) {
    console.error(`[build-vendor] 找不到 ${srcDir}，请先执行 bun install`)
    process.exit(1)
  }
  mkdirSync(outDir, { recursive: true })
  let copied = 0
  const walk = (s: string, rel: string): void => {
    for (const name of readdirSync(s)) {
      const sub = join(s, name)
      const relPath = rel ? `${rel}/${name}` : name
      if (statSync(sub).isDirectory()) {
        mkdirSync(join(outDir, relPath), { recursive: true })
        walk(sub, relPath)
        continue
      }
      if (filter && !filter(relPath)) continue
      const out = join(outDir, relPath)
      if (existsSync(out) && statSync(out).size === statSync(sub).size) continue
      copyFileSync(sub, out)
      copied++
    }
  }
  walk(srcDir, "")
  console.log(`[build-vendor] ${label} -> ${outDir}${copied ? `（${copied} 个文件）` : "（已就绪，跳过）"}`)
  return copied
}

if (groups.has("plantuml")) {
  copyFileIfChanged(join(root, "node_modules", "@plantuml", "core", "plantuml.js"), join(vendor, "plantuml.js"), "plantuml.js")
  copyFileIfChanged(join(root, "node_modules", "@plantuml", "core", "viz-global.js"), join(vendor, "viz-global.js"), "viz-global.js")
}
if (groups.has("mermaid")) {
  copyFileIfChanged(join(root, "node_modules", "mermaid", "dist", "mermaid.min.js"), join(vendor, "mermaid.js"), "mermaid.js")
}
if (groups.has("echarts")) {
  copyFileIfChanged(join(root, "node_modules", "echarts", "dist", "echarts.min.js"), join(vendor, "echarts.js"), "echarts.js")
}
if (groups.has("d2js")) {
  copyDirIfChanged(join(root, "node_modules", "@terrastruct", "d2", "dist", "browser"), join(vendor, "d2js"), "d2js")
}
// Monaco（VSCode 同款编辑器内核）：monaco-editor/min/vs 的 AMD 构建原样拷到 vendor/monaco/vs。
// 为何不走 vite 打包：AMD 构建文件名稳定（loader.js / editor/editor.main.js），构建后 URL 不变——
// 与 vendor 其它引擎同一理由（vite 分包会生成带 hash 的文件名，dev-reload 重建后旧页面引用旧 chunk 即 404）；
// 且 editor.main 自无需转译，vite 当普通 JS 处理反而会解析/改写其巨大的内部模块表。
// 体积约 24MB（含 ts/json/css/html worker 与全部语言高亮），已 gitignore，仅构建期生成。
if (groups.has("monaco")) {
  copyTreeIfChanged(join(root, "node_modules", "monaco-editor", "min", "vs"), join(vendor, "monaco", "vs"), "monaco/vs", (rel) => !rel.endsWith(".map"))
}
// xterm.js（VSCode 同款终端内核）+ 三个官方 addon：ESM 构建按稳定文件名伺服、运行时动态 import
// （与 mermaid/d2 同款理由：不走打包链，dev-reload 重建后 URL 不变）。为何不用 UMD 构建：
// xterm 6 的 UMD 包靠 `for (var s in exports)` 把导出挂到全局，而这批导出是不可枚举属性，
// 全局拿不到 Terminal（实测 undefined）；ESM 的命名导出无此问题，与动态 import 配合也更直接。
if (groups.has("xterm")) {
  copyFileIfChanged(join(root, "node_modules", "@xterm", "xterm", "lib", "xterm.mjs"), join(vendor, "xterm", "xterm.mjs"), "xterm.mjs")
  copyFileIfChanged(join(root, "node_modules", "@xterm", "xterm", "css", "xterm.css"), join(vendor, "xterm", "xterm.css"), "xterm.css")
  copyFileIfChanged(join(root, "node_modules", "@xterm", "addon-fit", "lib", "addon-fit.mjs"), join(vendor, "xterm", "addon-fit.mjs"), "addon-fit.mjs")
  copyFileIfChanged(join(root, "node_modules", "@xterm", "addon-search", "lib", "addon-search.mjs"), join(vendor, "xterm", "addon-search.mjs"), "addon-search.mjs")
  copyFileIfChanged(join(root, "node_modules", "@xterm", "addon-web-links", "lib", "addon-web-links.mjs"), join(vendor, "xterm", "addon-web-links.mjs"), "addon-web-links.mjs")
}
// tree-sitter（文件工作台的符号提取内核，wasm 版）：只伺服**运行时**（ESM 与核心 wasm，稳定文件名）；
// **各语言语法 wasm 不走这里**——15 种语言原始体积约 25MB，放进 public/ 会被内嵌进二进制产物；
// 改由服务端静态路径 `/vendor/tree-sitter/lang/<grammar>.wasm` 从已内嵌的语法集按需回源（见 routes/static.ts）。
if (groups.has("tree_sitter")) {
  copyFileIfChanged(join(root, "node_modules", "web-tree-sitter", "tree-sitter.js"), join(vendor, "tree-sitter", "tree-sitter.js"), "tree-sitter.js")
  copyFileIfChanged(join(root, "node_modules", "web-tree-sitter", "tree-sitter.wasm"), join(vendor, "tree-sitter", "tree-sitter.wasm"), "tree-sitter.wasm")
}

// 未包含组：删除既有产物（避免残留旧文件让裁掉的能力仍可用）并如实列出降级面
for (const g of VENDOR_GROUPS) {
  if (groups.has(g)) continue
  for (const p of GROUP_TARGETS[g]) {
    if (!existsSync(p)) continue
    rmSync(p, { recursive: true, force: true })
    console.log(`[build-vendor] 已移除未包含的组 ${g}: ${p}`)
  }
}
const skippedGroups = VENDOR_GROUPS.filter((g) => !groups.has(g))
if (skippedGroups.length) {
  console.log(`[build-vendor] 裁剪生效，未包含: ${skippedGroups.join(", ")}（前端对应渲染/编辑器能力降级，引擎缺失时按懒加载失败降级）`)
}
