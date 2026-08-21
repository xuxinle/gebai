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
import { copyFileSync, existsSync, mkdirSync, readdirSync, statSync } from "node:fs"
import { dirname, join } from "node:path"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/web
const vendor = join(root, "public", "vendor")

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

copyFileIfChanged(join(root, "node_modules", "@plantuml", "core", "plantuml.js"), join(vendor, "plantuml.js"), "plantuml.js")
copyFileIfChanged(join(root, "node_modules", "@plantuml", "core", "viz-global.js"), join(vendor, "viz-global.js"), "viz-global.js")
copyFileIfChanged(join(root, "node_modules", "mermaid", "dist", "mermaid.min.js"), join(vendor, "mermaid.js"), "mermaid.js")
copyFileIfChanged(join(root, "node_modules", "echarts", "dist", "echarts.min.js"), join(vendor, "echarts.js"), "echarts.js")
copyDirIfChanged(join(root, "node_modules", "@terrastruct", "d2", "dist", "browser"), join(vendor, "d2js"), "d2js")
