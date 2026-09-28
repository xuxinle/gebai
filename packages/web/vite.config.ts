import { defineConfig, type Plugin } from "vite"
import { fileURLToPath } from "node:url"
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync } from "node:fs"
import { join } from "node:path"

/** 页面入口绝对路径（多入口：主界面 index.html + 文件工作台 files.html）。 */
const page = (name: string): string => fileURLToPath(new URL(`./${name}`, import.meta.url))

/** 二开前端脚本目录（仓库根 `custom/web/`）：二开域资产，与上游包解耦、随 `custom/` 整体迁移。 */
const CUSTOM_WEB_DIR = fileURLToPath(new URL("../../custom/web", import.meta.url))
/** 示例模板后缀：`*.example.js` 不参与接入——上游更新复制 `custom/` 时只刷新示例，不动已启用的脚本。 */
const EXAMPLE_SUFFIX = ".example.js"
/** 源文件名 → 产物根文件名（未列出的同名输出）：`init.js` 以并列命名 `gebai.custom.js` 输出。 */
const CUSTOM_WEB_OUTPUT: Record<string, string> = { "init.js": "gebai.custom.js" }
/** HTML 中引入的二开脚本（产物根名，配置在前、初始化脚本在后）——源文件存在才注入。 */
const HTML_INJECTED = ["gebai.config.js", "gebai.custom.js"]

/**
 * 二开域前端脚本（`custom/web/`）接入——已启用的脚本「放置即生效」，无需任何清单；
 * `*.example.js` 仅是示例模板（上游发布内容），不参与接入——启用方式是复制改名为脚本本身：
 * - dev：中间件按产物根路径伺服（`/gebai.config.js`、`/gebai.custom.js`…），改文件刷新页面即生效；
 * - 构建：结束时复制到 web 产物根（目录内没有已启用脚本则不产出）；
 * - HTML：在 `</body>` 前注入**存在的那几个**脚本引用。普通 script 同步执行，而入口模块脚本为
 *   deferred，故两者均早于应用初始化，使二开的本地存储初始化 / 注册登录先于应用初始化运行。
 */
function customWebPlugin(): Plugin {
  /** 产物根（`configResolved` 后取 vite 解析值，`--outDir` 覆盖同样生效）。 */
  let outDir = fileURLToPath(new URL("./dist", import.meta.url))
  /** 当前已启用的源文件与产物根名（每次调用实时读目录——新增/删除文件无需改配置）。 */
  const entries = (): Array<{ src: string; out: string }> => {
    if (!existsSync(CUSTOM_WEB_DIR)) return []
    return readdirSync(CUSTOM_WEB_DIR)
      .filter((name) => name.endsWith(".js") && !name.endsWith(EXAMPLE_SUFFIX))
      .sort()
      .map((name) => ({ src: join(CUSTOM_WEB_DIR, name), out: CUSTOM_WEB_OUTPUT[name] ?? name }))
  }
  return {
    name: "gebai-custom-web",
    configResolved(config) {
      outDir = config.build.outDir
    },
    // dev：按产物根路径伺服已启用的二开脚本；未命中交给 vite 常规处理
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const path = (req.url ?? "").split("?")[0]
        const name = path.startsWith("/") ? path.slice(1) : path
        const hit = entries().find((e) => e.out === name)
        if (!hit) return next()
        res.setHeader("Content-Type", "text/javascript; charset=utf-8")
        res.setHeader("Cache-Control", "no-cache")
        res.end(readFileSync(hit.src))
      })
    },
    // 构建：二开脚本纳入 watch（`vite build --watch` 下改动触发重建；非 watch 构建为空操作）
    buildStart() {
      for (const e of entries()) this.addWatchFile(e.src)
    },
    // 构建：复制到产物根（与 public/ 同等待遇；在 clean-dist 之后、产物写盘收尾时执行）
    closeBundle() {
      for (const e of entries()) {
        mkdirSync(outDir, { recursive: true })
        copyFileSync(e.src, join(outDir, e.out))
      }
    },
    // HTML：只注入存在的脚本（`./` 相对路径，反向代理子路径挂载下同样成立）
    transformIndexHtml: {
      order: "post",
      handler(html) {
        const present = new Set(entries().map((e) => e.out))
        const tags = HTML_INJECTED.filter((out) => present.has(out)).map((out) => `<script src="./${out}"></script>`)
        if (tags.length === 0) return html
        return html.replace("</body>", `${tags.join("\n  ")}\n  </body>`)
      },
    },
  }
}

export default defineConfig({
  // 产物内引用（HTML 的 script/link、分块互引、CSS 内的字体）一律相对：
  // 部署在反向代理子路径（如 /gebai/）下与页面同目录解析，无需任何基准配置；dev 下 vite 按根伺服，不受影响
  base: "./",
  server: {
    port: 5173,
    proxy: {
      "/api": "http://127.0.0.1:3000",
      // target 用 http:// 前缀（http-proxy 自动升级 WS）：ws:// 前缀在 vite 6.4 下 WS 转发失效
      "/ws": { target: "http://127.0.0.1:3000", ws: true },
    },
  },
  plugins: [customWebPlugin()],
  build: {
    outDir: "dist",
    // 现代浏览器/WebView（WebView2/WKWebView/WebKitGTK 均支持 es2022）：减少转译
    target: "es2022",
    // dist 清理统一由 scripts/clean-dist.ts 前置完成（Windows 上 vite 内置 emptyDir
    // 无重试，删除瞬时占用文件会抛 ENOTEMPTY 导致 `vite build --watch` 崩溃）
    emptyOutDir: false,
    // 跳过产物 gzip 预计算：@plantuml/core 大 chunk 的压缩预计算是构建耗时大头
    reportCompressedSize: false,
    chunkSizeWarningLimit: 7000,
    // 关闭 rollup tree-shaking：构建耗时大头是 rollup 对 6.4MB @plantuml/core 的副作用分析
    // （该依赖是已打包单文件，tree-shake 无收益）；全局关闭后应用产物体积影响极小（index.js +0.5KB）
    // 多入口：index.html（主界面）+ files.html（文件工作台，独立页面 /files）；
    // 共享同一份构建流水线（hash 资源、vendor 静态资源、dev-reload 热刷新）。
    rollupOptions: { treeshake: false, input: { main: page("index.html"), files: page("files.html") } },
  },
})
