/** Web UI 静态托管路由：单端口暴露（`/` 入口 + assets），dev-reload 占位页与二进制内嵌资源两形态。
 *  web bundle 构建期由 scripts/build-web-bundle.ts 生成；dev 模式文件不存在时回退空表（Web UI 走源码 webDist）。 */
import { serveStatic } from "hono/bun"
import { existsSync, readFileSync } from "node:fs"
import { join } from "node:path"
import type { RouteCtx } from "./context"

function loadWebBundle(): Record<string, string> {
  try {
    return require("../core/web.bundle.generated").webBundle
  } catch {
    return {}
  }
}
const webBundle = loadWebBundle()

/** 二进制模式内嵌静态资源访问（web bundle 为空时返回 null）。 */
function embeddedWebAssets(): { get: (p: string) => Uint8Array<ArrayBuffer> | null } | null {
  const keys = Object.keys(webBundle)
  if (!keys.length) return null
  const byPath = new Map(keys.map((k) => [k, Buffer.from(webBundle[k], "base64")]))
  const asUint8 = (buf: Buffer | null): Uint8Array<ArrayBuffer> | null => (buf ? Uint8Array.from(buf) : null)
  return {
    get: (p: string) => {
      const norm = (p || "/").split("?")[0]
      return asUint8(byPath.get(norm) ?? byPath.get("/index.html") ?? null)
    },
  }
}

/**
 * dev-reload 首轮构建窗口期的占位页：clean-dist 清空 dist 后、vite 尚未重建完成时，
 * `GET /` 读取 index.html 会失败——此时返回本页而非抛异常崩溃服务。
 * 复用 /__gebai_hot WebSocket：构建完成（服务端广播 reload）或连接断开（服务端重启）
 * 即刷新；另以 3s 定时刷新兜底，确保构建完成后自动加载真实页面。
 */
function buildPlaceholderHtml(basePath: string): string {
  const hotPath = `${basePath === "/" ? "" : basePath}/__gebai_hot`
  const client = `(()=>{let ws;const go=()=>{ws=new WebSocket((location.protocol==="https:"?"wss":"ws")+"://"+location.host+${JSON.stringify(hotPath)});ws.onmessage=e=>{try{if(JSON.parse(e.data).type==="reload")location.reload()}catch{}};ws.onclose=()=>setTimeout(()=>location.reload(),400)};go();setInterval(()=>location.reload(),3000)})()`
  return `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><title>前端构建中…</title><style>body{font-family:system-ui,sans-serif;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;background:#f5f5f7;color:#333}.card{text-align:center}.dots{display:inline-block;margin-top:8px}.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#888;margin:0 3px;animation:pulse 1.2s infinite}.dot:nth-child(2){animation-delay:.2s}.dot:nth-child(3){animation-delay:.4s}@keyframes pulse{0%,80%,100%{opacity:.25}40%{opacity:1}}</style></head><body><div class="card"><p style="font-size:18px;margin:0">前端构建中<span class="dots"><span class="dot"></span><span class="dot"></span><span class="dot"></span></span></p><p style="color:#999;font-size:13px">构建完成后将自动刷新（bun run dev --reload）</p></div><script>${client}</script></body></html>`
}

export function registerStaticRoutes(rc: RouteCtx): void {
  const { app, d } = rc

  // Static Web UI (single-port exposure). Only registered if the build exists.
  // dev-reload 模式下即使 dist 刚被 clean-dist 清空（首轮构建窗口期）也注册，
  // 由 `/` 路由在 index.html 暂缺时返回占位页，避免服务崩溃或 UI 整体缺失。
  const embedded = d.config.binaryMode ? embeddedWebAssets() : null
  if (existsSync(d.config.webDist) || embedded || d.config.devReload) {
    // 注入全局默认 UI 风格（GEBAI_UI_STYLE），前端按 会话/URL > 用户 > 全局 优先级解析
    const UI_STYLES = ["acrylic", "aether", "cyberpunk", "aurora", "synthwave", "matrix", "tokyo-night", "ink", "cny"]
    const style = UI_STYLES.includes(d.config.uiStyle) ? d.config.uiStyle : "acrylic"
    let cachedHtml: string | null = null
    app.get("/", (c) => {
      // dev-reload 模式下每次请求重读 dist/index.html：vite build --watch 每次重建产出新 hash
      // 资源，若缓存启动时的旧 HTML，页面刷新后仍加载旧资源（改动永不生效）；生产/二进制模式缓存即可
      if (d.config.devReload || !cachedHtml) {
        let raw: string
        try {
          raw = embedded
            ? new TextDecoder().decode(embedded.get("/index.html") ?? new Uint8Array())
            : readFileSync(join(d.config.webDist, "index.html"), "utf8")
        } catch {
          // 构建窗口期 index.html 暂缺：返回占位页（构建完成后自动刷新），不抛异常崩溃服务
          return c.html(buildPlaceholderHtml(d.config.basePath), 503, { "Cache-Control": "no-cache" })
        }
        let injected = `<script>window.__GEBAI_UI_STYLE__=${JSON.stringify(style)}</script>`
        // 开发模式热刷新（--reload）：监听 /__gebai_hot，收到 reload 或连接断开（服务端重启）即刷新页面
        if (d.config.devReload) {
          const hotPath = `${d.config.basePath === "/" ? "" : d.config.basePath}/__gebai_hot`
          const client = `(()=>{let ws;const go=()=>{ws=new WebSocket((location.protocol==="https:"?"wss":"ws")+"://"+location.host+${JSON.stringify(hotPath)});ws.onmessage=e=>{try{if(JSON.parse(e.data).type==="reload")location.reload()}catch{}};ws.onclose=()=>setTimeout(()=>location.reload(),400)};go()})()`
          injected += `<script>${client}</script>`
        }
        cachedHtml = raw.replace("</head>", `${injected}</head>`)
      }
      return c.html(cachedHtml, 200, { "Cache-Control": "no-cache" })
    })
    if (embedded) {
      // 二进制模式：内嵌资源直接提供，无需磁盘
      app.use("*", async (c) => {
        const buf = embedded.get(c.req.path)
        if (!buf) return c.notFound()
        const ext = c.req.path.split(".").pop() ?? ""
        const mime =
          ext === "js"
            ? "text/javascript"
            : ext === "css"
              ? "text/css"
              : ext === "html"
                ? "text/html"
                : ext === "woff2"
                  ? "font/woff2"
                  : ext === "svg"
                    ? "image/svg+xml"
                    : "application/octet-stream"
        return new Response(buf, { status: 200, headers: { "Content-Type": mime } })
      })
    } else {
      app.use("*", serveStatic({ root: d.config.webDist }))
    }
  }
}
