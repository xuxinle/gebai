/** Web UI 静态托管路由：单端口暴露（`/` 入口 + assets），dev-reload 占位页与二进制内嵌资源两形态。
 *  web bundle 构建期由 scripts/build-web-bundle.ts 生成；dev 模式文件不存在时回退空表（Web UI 走源码 webDist）。
 *  构建产物资源（/assets 指纹名、/vendor 引擎、/fonts 字体）由本模块统一托管：按 Accept-Encoding 协商
 *  Brotli/Gzip 压缩（结果内存缓存）并设置 HTTP 缓存头；其余根文件（favicon、预览页）仍走 serveStatic。 */
import { serveStatic } from "hono/bun"
import { existsSync, readFileSync, statSync } from "node:fs"
import { join, resolve, sep } from "node:path"
import { brotliCompressSync, constants as zlibConstants, gzipSync } from "node:zlib"
import { grammarBytes } from "@gebai/agents"
import { TREE_SITTER_GRAMMAR } from "@gebai/sdk"
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
function buildPlaceholderHtml(): string {
  const client = `(()=>{let ws;const go=()=>{const u=new URL("__gebai_hot",location.href);u.protocol=u.protocol==="https:"?"wss:":"ws:";ws=new WebSocket(u);ws.onmessage=e=>{try{if(JSON.parse(e.data).type==="reload")location.reload()}catch{}};ws.onclose=()=>setTimeout(()=>location.reload(),400)};go();setInterval(()=>location.reload(),3000)})()`
  return `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><title>前端构建中…</title><style>body{font-family:system-ui,sans-serif;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;background:#f5f5f7;color:#333}.card{text-align:center}.dots{display:inline-block;margin-top:8px}.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#888;margin:0 3px;animation:pulse 1.2s infinite}.dot:nth-child(2){animation-delay:.2s}.dot:nth-child(3){animation-delay:.4s}@keyframes pulse{0%,80%,100%{opacity:.25}40%{opacity:1}}</style></head><body><div class="card"><p style="font-size:18px;margin:0">前端构建中<span class="dots"><span class="dot"></span><span class="dot"></span><span class="dot"></span></span></p><p style="color:#999;font-size:13px">构建完成后将自动刷新（bun run dev --reload）</p></div><script>${client}</script></body></html>`
}

/** 无内嵌 Web UI 的构建（构建期裁剪 GEBAI_BUILD_WEB_UI=0）：`/` 的如实说明页——
 *  服务本体、API（`/api/*`）与 WebSocket（`/ws`）均正常，仅未内嵌浏览器界面。 */
function noUiHtml(): string {
  return `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><title>GEBAI · 无 Web UI 构建</title><style>body{font-family:system-ui,sans-serif;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0;background:#f5f5f7;color:#333}.card{max-width:560px;padding:32px;text-align:left;line-height:1.7}.card h1{font-size:20px;margin:0 0 12px}.card p{margin:8px 0;color:#555}.card code{background:#eaeaef;padding:2px 6px;border-radius:4px;font-size:13px}</style></head><body><div class="card"><h1>本构建未内嵌 Web UI</h1><p>镜像在构建期按裁剪参数 <code>GEBAI_BUILD_WEB_UI=0</code> 跳过了前端产物，因此没有浏览器界面。</p><p>服务本体、REST API（<code>/api/*</code>）与 WebSocket（<code>/ws</code>）不受影响，客户端可用 SDK 或 HTTP 直接接入。</p><p>需要界面请用完整构建（不带该裁剪项）重建镜像。</p></div></body></html>`
}

/** 构建产物资源前缀（其余根文件由 serveStatic 兜底，压缩与缓存策略不覆盖）。 */
const ASSET_PREFIXES = ["/assets/", "/vendor/", "/fonts/"]

/**
 * 浏览器侧符号提取用的语法 wasm：`/vendor/tree-sitter/lang/<grammar>.wasm`。
 *
 * 这些字节**不另存一份到 web 产物**（15 种语言原始体积约 25MB，会把二进制内嵌产物推高一大截），
 * 而是从已经内嵌在服务端的分析器语法集里取（两者本就是同一份资源）。
 * 白名单取自 `TREE_SITTER_GRAMMAR`（与 web 端加载表同一份真相），未知文件名直接 404，不碰文件系统。
 */
const GRAMMAR_FILES = new Set(Object.values(TREE_SITTER_GRAMMAR))
const GRAMMAR_PATH_RE = /^\/vendor\/tree-sitter\/lang\/([\w.-]+)$/
/** 语法字节缓存（同一语法只取一次 + 只压一次）。wasm 已接近不可压，压缩收益有限但胜在零代价。 */
const grammarCache = new Map<string, { raw: Uint8Array; gzip: Uint8Array }>()

/** 取语法字节（带缓存）；白名单外或资源缺失返回 null。 */
async function grammarAsset(name: string): Promise<{ raw: Uint8Array; gzip: Uint8Array } | null> {
  if (!GRAMMAR_FILES.has(name)) return null
  const hit = grammarCache.get(name)
  if (hit) return hit
  const raw = await grammarBytes(name)
  if (!raw) return null
  const entry = { raw, gzip: new Uint8Array(gzipSync(raw)) }
  grammarCache.set(name, entry)
  return entry
}

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

/** 入口 HTML 的响应头：**禁止存储**（no-store 而非 no-cache）。
 *
 * 两者的区别是实际影响结果：no-cache 只要求「用前校验」，但移动端浏览器（微信/UC/系统浏览器等）
 * 与部分反向代理会忽略它而强缓存 HTML——而入口 HTML 引用的是**内容 hash 命名的** /assets/*，
 * 一旦缓存住旧 HTML，它引用的旧 hash 资源已被新构建删除（clean-dist）→ 全部 404 →
 * 页面无样式、脚本不执行，看起来却像「代码改坏了」（无痕模式必然正常，因无缓存）。
 * no-store 明确禁止任何一环存储，是入口 HTML 的正确策略；HTML 仅数十 KB，每次重取无实际代价。 */
const HTML_NO_STORE = { "Cache-Control": "no-store" } as const

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
    // 注入全局默认 UI 风格（GEBAI_UI_STYLE），前端按 会话/URL > 用户 > 全局 优先级解析。
    // 白名单须覆盖前端主题表（packages/web/src/theme-core.ts 的 THEMES）——主题增删时同步此处，
    // 否则该主题经环境变量设置会被静默回落为 acrylic（前端面板手动切换不经此白名单）。
    const UI_STYLES = ["acrylic", "aether", "cyberpunk", "aurora", "synthwave", "matrix", "tokyo-night", "ink", "cny", "qinhan"]
    const style = UI_STYLES.includes(d.config.uiStyle) ? d.config.uiStyle : "acrylic"
    // 浏览器页面标题（GEBAI_TITLE）：未设置则不注入任何东西（前端内置默认「歌白」保持不动）
    const title = d.config.title || ""
    /** 标题入 HTML 属性上下文（<title> 标签内）的实体转义。 */
    const escapeHtml = (s: string): string =>
      s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;")
    let cachedHtml: string | null = null
    // HTML 缓存的 mtime 伴随值（-1 = 尚未缓存；二进制内嵌模式下恒 0）
    let cachedHtmlMtime = -1

    /** 注入 UI 风格 + 页面标题 + dev-reload 热刷新脚本 + 重启自动刷新脚本（`/` 与 `/files` 共用）。 */
    const inject = (raw: string): string => {
      let html = raw
      if (title) {
        // ① 静态 <title>：files 页为「文件工作台 · 歌白」形态，前缀保留、名字替换；其余整条替换。
        //    未匹配到 <title>（异常 HTML）不动，运行时脚本（②）仍保底生效。
        html = html.replace(
          /<title>\s*([^<]*?)\s*歌白\s*<\/title>/,
          (_m, prefixRaw: string) => {
            // 前缀形如「文件工作台 · 」：剥离尾部分隔符（· / | / - / — 后随空白）后重组，避免双分隔符
            const prefix = prefixRaw.replace(/[\s·|\-—]+$/, "")
            return `<title>${prefix ? `${escapeHtml(prefix)} · ` : ""}${escapeHtml(title)}</title>`
          },
        )
      }
      let injected = `<script>window.__GEBAI_UI_STYLE__=${JSON.stringify(style)}</script>`
      if (title) {
        // ② 运行时标题（前端 updateTitle 读取）：JSON 入 <script>，`<` 转义为 \u003c 防
        //    标题含 </script> 时截断页面（JSON.stringify 不转义 <，需手动补）
        injected += `<script>window.__GEBAI_TITLE__=${JSON.stringify(title).replace(/</g, "\\u003c")}</script>`
      }
      // 开发模式热刷新（--reload）：监听 /__gebai_hot，收到 reload 或连接断开（服务端重启）即刷新页面
      if (d.config.devReload) {
        const client = `(()=>{let ws;const go=()=>{const u=new URL("__gebai_hot",location.href);u.protocol=u.protocol==="https:"?"wss:":"ws:";ws=new WebSocket(u);ws.onmessage=e=>{try{if(JSON.parse(e.data).type==="reload")location.reload()}catch{}};ws.onclose=()=>setTimeout(()=>location.reload(),400)};go()})()`
        injected += `<script>${client}</script>`
      }
      // 服务重启后页面自动重新加载（本地模式）：轮询 /api/health 的进程启动标识 bootId——变化即说明
      // 服务已被重启（重启时前端产物可能已重建），自动重载页面取新产物，免除手工 F5；服务模式不注入
      // （多用户部署下不打扰他人页面）。与 dev-reload 的 ws 刷新互补：ws 广播只覆盖本进程内的构建完成，
      // 换进程的重启不经过它（旧 ws 断开虽会刷新，但那只在 dev-reload 模式注入）。
      if (d.config.auth === "local") {
        const client = `(()=>{let boot=null;const check=()=>{fetch(new URL("api/health",location.href),{cache:"no-store"}).then(r=>r.ok?r.json():null).then(j=>{if(!j||!j.boot)return;if(boot===null){boot=j.boot;return}if(j.boot!==boot)location.reload()}).catch(()=>{})};check();setInterval(check,3000)})()`
        injected += `<script>${client}</script>`
      }
      return html.replace("</head>", `${injected}</head>`)
    }

    /** 读取 webDist（或内嵌资源）中的某个 HTML 页面；缺失返回 null。 */
    const readPage = (name: string): string | null => {
      try {
        return embedded ? new TextDecoder().decode(embedded.get(`/${name}`) ?? new Uint8Array()) : readFileSync(join(d.config.webDist, name), "utf8")
      } catch {
        return null
      }
    }

    /** 页面文件的 mtime（二进制内嵌模式或读取失败返回 0）——HTML 缓存的失效判据。 */
    const pageMtime = (name: string): number => {
      if (embedded) return 0
      try {
        return statSync(join(d.config.webDist, name)).mtimeMs
      } catch {
        return 0
      }
    }

    app.get("/", (c) => {
      // dev-reload：每次请求重读；其余模式按 index.html 的 **mtime 失效**——
      // 前端重新构建后（vite 产出新 hash 资源、clean-dist 删掉旧资源）若不失效，服务端会返回引用
      // 已删除资源的旧 HTML（页面样式与脚本全 404），看起来却像「刚改的代码有 bug」。
      const mtime = pageMtime("index.html")
      if (d.config.devReload || cachedHtml === null || mtime !== cachedHtmlMtime) {
        const raw = readPage("index.html")
        if (raw === null) {
          // 构建窗口期 index.html 暂缺：返回占位页（构建完成后自动刷新），不抛异常崩溃服务
          return c.html(buildPlaceholderHtml(), 503, HTML_NO_STORE)
        }
        cachedHtml = inject(raw)
        cachedHtmlMtime = mtime
      }
      return c.html(cachedHtml, 200, HTML_NO_STORE)
    })

    // 文件工作台（DESIGN「文件工作台」）：独立页面 `/files`（vite 多入口 files.html），
    // 与主界面同等待遇（同一端口、同一注入、同一 dev-reload 通道）；缺失时不注册（不影响主界面）。
    if (d.config.fsEnabled !== false && readPage("files.html") !== null) {
      app.get("/files", (c) => {
        const raw = readPage("files.html")
        if (raw === null) return c.notFound()
        return c.html(inject(raw), 200, HTML_NO_STORE)
      })
    }
    // 构建产物静态资源：压缩协商与缓存头在 assetResponse 统一承担；未命中时非二进制模式
    // 落到下方 serveStatic 兜底（favicon、预览页等根文件），二进制模式按原样从内嵌表提供其余资源。
    app.use("*", async (c, next) => {
      const path = c.req.path
      const acceptEncoding = c.req.header("accept-encoding") ?? null
      // 语法 wasm：单独一条（字节来自内嵌语法集而非 web 产物），命中后不进下面的资源管道
      const grammarHit = GRAMMAR_PATH_RE.exec(path)
      if (grammarHit) {
        const asset = await grammarAsset(grammarHit[1]!)
        if (!asset) return c.notFound()
        // 响应编码按协商：支持 gzip 就回压缩字节（python 465KB → 72KB），否则回原始字节
        const useGzip = (acceptEncoding ?? "").split(",").some((s) => s.trim().split(";")[0].toLowerCase() === "gzip")
        const headers = new Headers({
          "Content-Type": "application/wasm",
          "Cache-Control": cacheControlFor(path, d.config.devReload),
          Vary: "Accept-Encoding",
        })
        if (useGzip) headers.set("Content-Encoding", "gzip")
        return new Response(useGzip ? asset.gzip : asset.raw, { status: 200, headers })
      }
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
  } else if (d.config.binaryMode) {
    // 裁剪构建（GEBAI_BUILD_WEB_UI=0）：二进制形态既无内嵌资源也无磁盘产物，访问 `/` 给出切实说明页而非裸 404
    app.get("/", (c) => c.html(noUiHtml(), 200, HTML_NO_STORE))
  }
}
