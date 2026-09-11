/** Web UI 静态托管路由：单端口暴露（`/` 入口 + assets），dev-reload 占位页与二进制内嵌资源两形态。
 *  web bundle 构建期由 scripts/build-web-bundle.ts 生成；dev 模式文件不存在时回退空表（Web UI 走源码 webDist）。
 *  构建产物资源（/assets 指纹名、/vendor 引擎、/fonts 字体）由本模块统一托管：按 Accept-Encoding 协商
 *  Brotli/Gzip 压缩（结果内存缓存）并设置 HTTP 缓存头；其余根文件（favicon、预览页）仍走 serveStatic。 */
import { serveStatic } from "hono/bun"
import { existsSync, readFileSync, statSync } from "node:fs"
import { join, resolve, sep } from "node:path"
import { brotliCompressSync, constants as zlibConstants, gzipSync } from "node:zlib"
import { extOf, mimeForPath } from "../core/fs/mime"
import type { RouteCtx } from "./context"

/** 内嵌 web bundle（构建期 `scripts/build-web-bundle.ts` 生成，体积数十 MB）：**按需加载**——
 *  仅二进制模式读取内嵌资源时经 embeddedWebAssets() 触发；源码/dev 形态走 webDist，不进启动路径。 */
let webBundleCache: Record<string, string> | null = null
function webBundle(): Record<string, string> {
  if (!webBundleCache) {
    let loaded: Record<string, string> = {}
    try {
      loaded = require("../core/web.bundle.generated").webBundle
    } catch {
      /* bundle 缺失（纯源码/dev 形态）：空表 */
    }
    webBundleCache = loaded
  }
  return webBundleCache
}

/** 二进制模式内嵌静态资源访问（web bundle 为空时返回 null）。 */
function embeddedWebAssets(): EmbeddedAssets | null {
  const keys = Object.keys(webBundle())
  if (!keys.length) return null
  const byPath = new Map(keys.map((k) => [k, Buffer.from(webBundle()[k], "base64")]))
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

/** 构建产物资源前缀（其余根文件由 serveStatic 兜底，压缩与缓存策略不覆盖）。 */
const ASSET_PREFIXES = ["/assets/", "/vendor/", "/fonts/"]

/** 参与压缩协商的扩展名（文本类；woff2/wasm/图片等已压缩或二进制格式跳过，白压 CPU）。 */
const COMPRESSIBLE_EXT = new Set(["js", "mjs", "css", "html", "htm", "svg", "json", "map", "txt", "webmanifest"])

/** 低于此体积不压缩（编码与头部开销可能反超收益）。 */
const MIN_COMPRESS_BYTES = 1400

/** 压缩结果缓存上限（压缩后字节）：超限按插入顺序淘汰最早条目。 */
const COMPRESS_CACHE_LIMIT = 32 * 1024 * 1024

/** 二进制模式内嵌资源表。 */
type EmbeddedAssets = { get: (p: string) => Uint8Array<ArrayBuffer> | null }

/** 资源字节 + 缓存键（磁盘资源以 size+mtime 入键，重建后自动失效）。 */
type AssetSource = { body: Uint8Array<ArrayBuffer>; cacheKey: string; ext: string }

const compressCache = new Map<string, { body: Uint8Array<ArrayBuffer>; bytes: number }>()
let compressCacheBytes = 0

/** 按 Accept-Encoding 选编码（br 优先，其次 gzip；均不支持则不压缩）。 */
function negotiatedEncoding(header: string | null): "br" | "gzip" | null {
  if (!header) return null
  const tokens = header.split(",").map((s) => s.trim().split(";")[0].toLowerCase())
  if (tokens.includes("br")) return "br"
  if (tokens.includes("gzip")) return "gzip"
  return null
}

/** 压缩并按 cacheKey 缓存（同一资源同一编码只压一次——vendor 引擎单体数 MB，压缩是一次性 CPU 换长期带宽）。 */
function compressedBody(cacheKey: string, body: Uint8Array<ArrayBuffer>, encoding: "br" | "gzip"): Uint8Array<ArrayBuffer> {
  const hit = compressCache.get(cacheKey)
  if (hit) return hit.body
  const raw = encoding === "br" ? brotliCompressSync(body, { params: { [zlibConstants.BROTLI_PARAM_QUALITY]: 5 } }) : gzipSync(body)
  const out = new Uint8Array(raw)
  compressCache.set(cacheKey, { body: out, bytes: out.byteLength })
  compressCacheBytes += out.byteLength
  while (compressCacheBytes > COMPRESS_CACHE_LIMIT && compressCache.size > 1) {
    const oldest = compressCache.keys().next().value as string
    compressCacheBytes -= compressCache.get(oldest)!.bytes
    compressCache.delete(oldest)
  }
  return out
}

/** HTTP 缓存策略：vite 指纹资源（/assets/*）内容与文件名绑定，可长期强缓存；vendor/fonts 为稳定名，短缓存；
 *  dev-reload 下重建会覆盖同名文件，一律 no-cache（新页面内容即时可见）。 */
function cacheControlFor(path: string, devReload: boolean): string {
  if (devReload) return "no-cache"
  return path.startsWith("/assets/") ? "public, max-age=31536000, immutable" : "public, max-age=86400"
}

/** 解析静态资源磁盘路径（钳制在 webDist 内，防目录穿越）；越界或非法编码返回 null。 */
function resolveAssetPath(webDist: string, pathname: string): string | null {
  let rel: string
  try {
    rel = decodeURIComponent(pathname)
  } catch {
    return null
  }
  const root = resolve(webDist)
  const abs = resolve(root, `.${rel}`)
  if (abs !== root && !abs.startsWith(root + sep)) return null
  return abs
}

/** 读取静态资源字节（二进制模式取内嵌表，否则读磁盘）；不存在返回 null。 */
async function loadAsset(embedded: EmbeddedAssets | null, webDist: string, path: string): Promise<AssetSource | null> {
  if (embedded) {
    const body = embedded.get(path)
    return body ? { body, cacheKey: path, ext: extOf(path) } : null
  }
  const abs = resolveAssetPath(webDist, path)
  if (!abs) return null
  let st: ReturnType<typeof statSync>
  try {
    st = statSync(abs)
  } catch {
    return null
  }
  if (!st.isFile()) return null
  const body = new Uint8Array(await Bun.file(abs).arrayBuffer())
  return { body, cacheKey: `${path}|${st.size}|${st.mtimeMs}`, ext: extOf(path) }
}

/** 静态资源响应：Content-Type / 缓存头 / 压缩协商（Vary 保证中间代理按编码正确分流）。 */
function assetResponse(path: string, src: AssetSource, acceptEncoding: string | null, devReload: boolean): Response {
  const headers = new Headers({
    "Content-Type": mimeForPath(path),
    "Cache-Control": cacheControlFor(path, devReload),
    Vary: "Accept-Encoding",
  })
  const encoding = COMPRESSIBLE_EXT.has(src.ext) && src.body.byteLength >= MIN_COMPRESS_BYTES ? negotiatedEncoding(acceptEncoding) : null
  if (!encoding) return new Response(src.body, { status: 200, headers })
  headers.set("Content-Encoding", encoding)
  return new Response(compressedBody(`${src.cacheKey}|${encoding}`, src.body, encoding), { status: 200, headers })
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

    /** 注入 UI 风格 + dev-reload 热刷新脚本（`/` 与 `/files` 共用）。 */
    const inject = (raw: string): string => {
      let injected = `<script>window.__GEBAI_UI_STYLE__=${JSON.stringify(style)}</script>`
      // 开发模式热刷新（--reload）：监听 /__gebai_hot，收到 reload 或连接断开（服务端重启）即刷新页面
      if (d.config.devReload) {
        const hotPath = `${d.config.basePath === "/" ? "" : d.config.basePath}/__gebai_hot`
        const client = `(()=>{let ws;const go=()=>{ws=new WebSocket((location.protocol==="https:"?"wss":"ws")+"://"+location.host+${JSON.stringify(hotPath)});ws.onmessage=e=>{try{if(JSON.parse(e.data).type==="reload")location.reload()}catch{}};ws.onclose=()=>setTimeout(()=>location.reload(),400)};go()})()`
        injected += `<script>${client}</script>`
      }
      return raw.replace("</head>", `${injected}</head>`)
    }

    /** 读取 webDist（或内嵌资源）中的某个 HTML 页面；缺失返回 null。 */
    const readPage = (name: string): string | null => {
      try {
        return embedded ? new TextDecoder().decode(embedded.get(`/${name}`) ?? new Uint8Array()) : readFileSync(join(d.config.webDist, name), "utf8")
      } catch {
        return null
      }
    }

    app.get("/", (c) => {
      // dev-reload 模式下每次请求重读 dist/index.html：vite build --watch 每次重建产出新 hash
      // 资源，若缓存启动时的旧 HTML，页面刷新后仍加载旧资源（改动永不生效）；生产/二进制模式缓存即可
      if (d.config.devReload || !cachedHtml) {
        const raw = readPage("index.html")
        if (raw === null) {
          // 构建窗口期 index.html 暂缺：返回占位页（构建完成后自动刷新），不抛异常崩溃服务
          return c.html(buildPlaceholderHtml(d.config.basePath), 503, { "Cache-Control": "no-cache" })
        }
        cachedHtml = inject(raw)
      }
      return c.html(cachedHtml, 200, { "Cache-Control": "no-cache" })
    })

    // 文件工作台（DESIGN「文件工作台」）：独立页面 `/files`（vite 多入口 files.html），
    // 与主界面同等待遇（同一端口、同一注入、同一 dev-reload 通道）；缺失时不注册（不影响主界面）。
    if (d.config.fsEnabled !== false && readPage("files.html") !== null) {
      const served = new Set<string>()
      for (const p of ["/files", `${d.config.basePath === "/" ? "" : d.config.basePath}/files`]) {
        if (served.has(p)) continue
        served.add(p)
        app.get(p, (c) => {
          const raw = readPage("files.html")
          if (raw === null) return c.notFound()
          return c.html(inject(raw), 200, { "Cache-Control": "no-cache" })
        })
      }
    }
    // 构建产物静态资源：压缩协商与缓存头在 assetResponse 统一承担；未命中时非二进制模式
    // 落到下方 serveStatic 兜底（favicon、预览页等根文件），二进制模式按原样从内嵌表提供其余资源。
    app.use("*", async (c, next) => {
      const path = c.req.path
      const acceptEncoding = c.req.header("accept-encoding") ?? null
      const asset = ASSET_PREFIXES.some((p) => path.startsWith(p)) ? await loadAsset(embedded, d.config.webDist, path) : null
      if (asset) return assetResponse(path, asset, acceptEncoding, d.config.devReload)
      if (embedded) {
        const inline = embedded.get(path)
        if (!inline) return c.notFound()
        return assetResponse(path, { body: inline, cacheKey: path, ext: extOf(path) }, acceptEncoding, d.config.devReload)
      }
      return next()
    })
    if (!embedded) app.use("*", serveStatic({ root: d.config.webDist }))
  }
}
