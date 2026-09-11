import { defineConfig } from "vite"
import { fileURLToPath } from "node:url"

/** 页面入口绝对路径（多入口：主界面 index.html + 文件工作台 files.html）。 */
const page = (name: string): string => fileURLToPath(new URL(`./${name}`, import.meta.url))

export default defineConfig({
  server: {
    port: 5173,
    proxy: {
      "/api": "http://127.0.0.1:3000",
      // target 用 http:// 前缀（http-proxy 自动升级 WS）：ws:// 前缀在 vite 6.4 下 WS 转发失效
      "/ws": { target: "http://127.0.0.1:3000", ws: true },
    },
  },
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
