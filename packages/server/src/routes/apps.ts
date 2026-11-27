/**
 * 托管应用（Hosted Apps）对外面：索引页 `/apps`、静态资源 `/apps/{id}/...`、后端 `/api/apps/{id}/*`、
 * 管理面 `/api/v1/apps`。
 *
 * ## 路由形态与鉴权位置（两个 URL 前缀，不是两套机制）
 *
 * | 路径 | 内容 | 鉴权姿态 |
 * |---|---|---|
 * | `GET /apps` | 应用索引页（服务端渲染，无需改前端构建） | 同 Web UI：非 `/api/` 一律免鉴权（未登录也能看到列表与页面） |
 * | `GET /apps/{id}[/...]` | 应用前端产物（HTML/JS/CSS/图片…） | 同上 |
 * | `ALL /api/apps/{id}/*` | 应用后端模块 | 同 REST：走完整凭证链（服务模式下需登录） |
 * | `GET /api/v1/apps` | 应用清单（含来源层/后端状态/警告） | 同 REST |
 *
 * 为什么前端产物免鉴权：与 `packages/web` 的 `index.html`/`assets` 同等待遇——**页面本身不是秘密**，
 * 界面里要数据得走 API（未登录照样 401 → 前端引导登录）。反过来，若静态层也要令牌，
 * `<img src>`/`<script src>` 这类无法插 Authorization 头的请求会全挂（见 credential-sources.ts 的同类论证）。
 *
 * 为什么后端挂在 `/api/apps/` 而不是 `/apps/{id}/api/`：**鉴权中间件只认 `/api/*`**（app.ts）。
 * 挂在应用前缀下等于让后端模块跑在「无身份、无 CORS 校验」的位置；挂在 API 域下则自动继承
 * 跨源校验与凭证链——不需要为应用后端再写一遍安全判定。
 *
 * 为什么管理面另用 `/api/v1/apps` 而不用 `/api/apps` 的 GET：`/api/apps/*` 是应用自己的 URL 空间
 * （应用后端可以定义任意路径），宿主的知识（有哪些应用、后端开没开、目录在哪）留在 v1 资源域，
 * 两者不抢路径。
 *
 * ## 静态分发
 *
 * - 扩展名→MIME 走 `core/fs/mime.ts` 同一张表（不另建一套）；
 * - `ETag` + `If-None-Match` → 304；文本类按 `Accept-Encoding` 协商 gzip（内嵌/磁盘同一管道）；
 * - 缓存头**一律 `no-cache`**：托管应用的开发形态是「改文件→刷新页面」，强缓存会让改动看起来没生效
 *   （与文件工作台 `fs/raw` 同一条教训）；
 * - SPA 回退：清单 `spa: true` 且请求路径**无扩展名**时回落到 entry（前端路由需要）；
 *   有扩展名却不存在 → 404（不回落）——否则缺失的 `.js` 会拿到一份 HTML，浏览器报「MIME 不符」，
 *   看起来像构建坏了。
 */
import type { Context } from "hono"
import { gzipSync } from "node:zlib"
import { extOf, mimeForPath } from "../core/fs/mime"
import { looksLikePagePath, APP_ID_RE } from "../core/apps/manifest"
import type { AppEntry, AppFile, AppsStore } from "../core/apps/sources"
import { AppBackendError, type AppBackendContext, type AppBackendLoader } from "../core/apps/backend"
import type { RouteCtx } from "./context"
import type { AppEnv } from "../app"
import { log } from "@gebai/sdk/node"

/** 参与 gzip 的扩展名（与 Web UI 静态管道同口径：已压缩格式不白压 CPU）。 */
const COMPRESSIBLE_EXT = new Set(["js", "mjs", "cjs", "css", "html", "htm", "svg", "json", "map", "txt", "webmanifest", "xml"])

/** 低于此体积不压缩（编码与头部开销可能反超收益）。 */
const MIN_COMPRESS_BYTES = 1400

/** gzip 结果缓存上限（字节）：应用产物改动频繁，缓存按 ETag 键，超限整体清空即可。 */
const COMPRESS_CACHE_LIMIT = 8 * 1024 * 1024

const compressCache = new Map<string, { body: Uint8Array; bytes: number }>()
let compressCacheBytes = 0

/** 按 Accept-Encoding 判断是否 gzip（托管应用不支持 br：收益与复杂度不划算）。 */
function wantsGzip(header: string | null): boolean {
  if (!header) return false
  return header.split(",").some((s) => s.trim().split(";")[0].toLowerCase() === "gzip")
}

/** 压缩并按 ETag 缓存。 */
function gzipped(etag: string, body: Uint8Array): Uint8Array {
  const hit = compressCache.get(etag)
  if (hit) return hit.body
  const out = new Uint8Array(gzipSync(body))
  compressCache.set(etag, { body: out, bytes: out.byteLength })
  compressCacheBytes += out.byteLength
  while (compressCacheBytes > COMPRESS_CACHE_LIMIT && compressCache.size > 1) {
    const oldest = compressCache.keys().next().value as string
    compressCacheBytes -= compressCache.get(oldest)!.bytes
    compressCache.delete(oldest)
  }
  return out
}

/** 应用内文件 → HTTP 响应（MIME / ETag / gzip 协商 / no-cache）。 */
function fileResponse(rel: string, file: AppFile, acceptEncoding: string | null, ifNoneMatch: string | null): Response {
  const headers = new Headers({
    "Content-Type": mimeForPath(rel),
    "Cache-Control": "no-cache",
    ETag: file.etag,
    Vary: "Accept-Encoding",
  })
  if (ifNoneMatch && ifNoneMatch === file.etag) return new Response(null, { status: 304, headers })
  const ext = extOf(rel)
  if (COMPRESSIBLE_EXT.has(ext) && file.body.byteLength >= MIN_COMPRESS_BYTES && wantsGzip(acceptEncoding)) {
    headers.set("Content-Encoding", "gzip")
    return new Response(gzipped(file.etag, file.body), { status: 200, headers })
  }
  return new Response(file.body, { status: 200, headers })
}

/** 应用不可用（入口缺失等）的说明页：如实说清原因，而不是让人对着空白页猜。 */
function brokenHtml(entry: AppEntry): string {
  const esc = (s: string): string => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
  const list = entry.warnings.map((w) => `<li>${esc(w)}</li>`).join("")
  return `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><title>应用不可用 · ${esc(entry.manifest.id)}</title>
<style>body{font-family:system-ui,sans-serif;display:flex;justify-content:center;padding:48px 16px;margin:0;background:#f5f5f7;color:#222}.card{max-width:640px;line-height:1.7}.card h1{font-size:18px;margin:0 0 12px}.card code{background:#eaeaef;padding:2px 6px;border-radius:4px}ul{color:#666}</style>
</head><body><div class="card"><h1>托管应用「${esc(entry.manifest.id)}」不可用</h1>
<p>${esc(entry.error ?? "未知原因")}</p>
<p>应用目录：<code>${esc(entry.dir ?? "（内嵌层）")}</code></p>${list ? `<ul>${list}</ul>` : ""}
<p>修正后刷新本页即可（无需重启服务）。</p></div></body></html>`
}

/** 索引页 HTML（服务端渲染：应用是数据，不该逼前端构建期就知道它们存在）。 */
function indexHtml(entries: AppEntry[], backendEnabled: boolean): string {
  const esc = (s: string): string => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;")
  const cards = entries
    .map((e) => {
      const m = e.manifest
      const badges = [
        e.layer === "embedded" ? "内嵌" : "运行时",
        e.hasBackend && backendEnabled ? "含后端" : "",
        e.error ? "不可用" : "",
      ].filter(Boolean)
      const desc = m.description ? `<p class="desc">${esc(m.description)}</p>` : ""
      const err = e.error ? `<p class="err">${esc(e.error)}</p>` : ""
      const ver = m.version ? `<span class="ver">v${esc(m.version)}</span>` : ""
      return `<a class="card" href="/apps/${esc(m.id)}/">
  <div class="head"><span class="title">${esc(m.title)}</span>${ver}</div>
  <div class="id">${esc(m.id)}</div>
  ${desc}${err}
  <div class="badges">${badges.map((b) => `<span>${b}</span>`).join("")}</div>
</a>`
    })
    .join("\n")
  const body = entries.length
    ? cards
    : `<div class="empty"><p>还没有托管应用。</p><p>把应用目录放到 <code>{GEBAI_HOME}/data/apps/{id}/</code>（内含 <code>index.html</code> 与可选 <code>app.json</code>），刷新本页即可看到。</p></div>`
  return `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>应用 · 歌白</title>
<style>
:root{color-scheme:light dark}
body{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;margin:0;padding:32px 20px 64px;background:#f5f5f7;color:#1d1d1f}
@media (prefers-color-scheme:dark){body{background:#141416;color:#e8e8ed}.card{background:#1e1e22!important;border-color:#2c2c31!important}.id{color:#8e8e93!important}.badges span{background:#2c2c31!important;color:#c7c7cc!important}.empty{color:#a1a1a6!important}code{background:#2c2c31!important}}
.wrap{max-width:980px;margin:0 auto}
h1{font-size:22px;margin:0 0 6px}
.sub{color:#86868b;font-size:13px;margin:0 0 24px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:14px}
.card{display:block;text-decoration:none;color:inherit;background:#fff;border:1px solid #e3e3e8;border-radius:12px;padding:16px;transition:transform .12s ease,box-shadow .12s ease}
.card:hover{transform:translateY(-2px);box-shadow:0 6px 20px rgba(0,0,0,.08)}
.head{display:flex;align-items:baseline;gap:8px}
.title{font-weight:600;font-size:15px}
.ver{font-size:11px;color:#86868b}
.id{font-size:11px;color:#a1a1a6;margin:2px 0 8px;font-family:ui-monospace,monospace}
.desc{margin:0 0 8px;font-size:13px;color:#6e6e73;line-height:1.55}
.err{margin:0 0 8px;font-size:12px;color:#c9302c}
.badges{display:flex;gap:6px;flex-wrap:wrap}
.badges span{font-size:11px;background:#eeeef2;color:#6e6e73;border-radius:999px;padding:2px 8px}
.empty{color:#6e6e73;line-height:1.8}
code{background:#eeeef2;padding:2px 6px;border-radius:4px;font-size:12px}
</style></head><body><div class="wrap">
<h1>托管应用</h1>
<p class="sub">共 ${entries.length} 个${backendEnabled ? "" : "（应用后端未启用：GEBAI_APPS_BACKEND）"}。目录变化后刷新即生效。</p>
<div class="grid">
${body}
</div></div></body></html>`
}

export function registerAppRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const store: AppsStore | null = d.apps ?? null
  const backend: AppBackendLoader | null = d.appBackend ?? null
  if (!store && !backend) return // 能力未启用：一条路由都不注册（与文件工作台同一姿态）

  // ── 索引页（`/apps` 与 `/apps/`）──
  if (store) {
    const serveIndex = (c: Context<AppEnv>): Response => {
      const entries = store.list(true).filter((e) => e.manifest.listed)
      return c.html(indexHtml(entries, backend?.enabled ?? false), 200, { "Cache-Control": "no-store" })
    }
    app.get("/apps", serveIndex)
    app.get("/apps/", serveIndex)

    // ── 前端产物：`/apps/{id}` 与 `/apps/{id}/{path}` ──
    // 必须同时注册「不带尾段」「带尾段」「通配」三条：`/apps/{id}` 不带斜杠时不会命中 `/*`。
    const serveApp = async (c: Context<AppEnv>, id: string, rawPath: string): Promise<Response> => {
      if (!APP_ID_RE.test(id)) return c.notFound()
      const entry = store.get(id)
      if (!entry) return c.notFound()
      if (entry.error) return c.html(brokenHtml(entry), 503, { "Cache-Control": "no-store" })
      let rel = rawPath.replace(/^\/+/, "")
      try {
        rel = decodeURIComponent(rel)
      } catch {
        return c.text("bad request: invalid url encoding", 400)
      }
      let file: AppFile | null = null
      if (rel) file = await store.readFile(entry, rel)
      if (!file && (!rel || (entry.manifest.spa && looksLikePagePath(rel)))) {
        // 根路径直接用入口；无扩展名的页面路径按 SPA 回落到入口
        file = await store.readFile(entry, entry.manifest.entry)
        if (!file) return c.html(brokenHtml({ ...entry, error: `入口文件缺失：${entry.manifest.entry}` }), 503, { "Cache-Control": "no-store" })
      }
      if (!file) return c.notFound()
      return fileResponse(rel || entry.manifest.entry, file, c.req.header("accept-encoding") ?? null, c.req.header("if-none-match") ?? null)
    }

    app.get("/apps/:id", (c) => serveApp(c, c.req.param("id") ?? "", ""))
    app.get("/apps/:id/", (c) => serveApp(c, c.req.param("id") ?? "", ""))
    app.get("/apps/:id/*", (c) => {
      // `:id/*` 通配段：余下部分从 c.req.path 剥前缀取（不依赖通配 param 的形态）
      const id = c.req.param("id") ?? ""
      const prefix = `/apps/${id}`
      const rest = c.req.path.startsWith(prefix) ? c.req.path.slice(prefix.length) : ""
      return serveApp(c, id, rest)
    })
  }

  // ── 后端模块：`ALL /api/apps/{id}` 与 `/api/apps/{id}/*` ──
  const handleBackend = async (c: Context<AppEnv>): Promise<Response> => {
    const id = c.req.param("id") ?? ""
    if (!APP_ID_RE.test(id)) return c.json({ error: `invalid app id: ${id}` }, 400)
    const entry = store?.get(id) ?? null
    if (!entry) return c.json({ error: `unknown app: ${id}` }, 404)
    if (entry.error) return c.json({ error: entry.error, warnings: entry.warnings }, 503)
    if (!backend) return c.json({ error: "托管应用后端未装配" }, 503)
    if (!backend.enabled) return c.json({ error: backend.disabledReason }, 503)
    if (!entry.hasBackend) return c.json({ error: `应用「${id}」未声明后端模块或后端模块缺失` }, 404)

    const prefix = `/api/apps/${id}`
    const rawPath = c.req.path.startsWith(prefix) ? c.req.path.slice(prefix.length).replace(/^\/+/, "") : ""
    let rel: string
    try {
      rel = decodeURIComponent(rawPath)
    } catch {
      return c.json({ error: "invalid url encoding" }, 400)
    }
    const user = rc.userOf(c)
    let textCache: string | null = null
    const query: Record<string, string> = {}
    for (const [k, v] of new URL(c.req.url).searchParams) query[k] = v
    const ctx: AppBackendContext = {
      appId: id,
      method: c.req.method.toUpperCase(),
      path: rel,
      query,
      header: (name) => c.req.header(name),
      text: async () => (textCache ??= await c.req.text()),
      json: async () => {
        const raw = textCache ?? (textCache = await c.req.text())
        return raw ? JSON.parse(raw) : null
      },
      user: { id: user?.id ?? "", role: user?.role ?? "" },
      env: { ...entry.manifest.env },
      respond: (data, init) =>
        new Response(JSON.stringify(data), {
          status: init?.status ?? 200,
          headers: { "Content-Type": "application/json; charset=utf-8", ...(init?.headers ?? {}) },
        }),
    }
    try {
      const out = await backend.invoke(entry, ctx)
      if (out instanceof Response) return out
      return ctx.respond(out ?? null)
    } catch (err) {
      const e = err instanceof AppBackendError ? err : new AppBackendError(String((err as Error)?.message || err), "run")
      // 后端故障是**部署方的代码问题**：如实进日志（便于排查），对外只给原因不给栈
      log.warn(`[apps] 「${id}」后端${e.stage === "timeout" ? "超时" : e.stage === "load" ? "加载失败" : "异常"}：${e.message}`)
      return c.json({ error: e.message, stage: e.stage, app: id }, e.stage === "load" ? 503 : e.stage === "timeout" ? 504 : 500)
    }
  }

  // 按方法逐条注册而不是 app.all：`/api/docs` 的端点表生成器跳过 `ALL`（中间件/通配的惯例），
  // 用 all 注册会让“应用后端到底有哪些入口”在集成方那份文档里完全看不到。逐个方法注册后
  // 既进了 OpenAPI，也保持“任何方法都交给应用”的语义（应用自己决定支持哪些方法与路径）。
  // HEAD 不单列：Hono 对每个 GET 自动派生 HEAD（单列反而与派生路由重复）。
  const BACKEND_METHODS = [
    (p: string, h: typeof handleBackend) => app.get(p, h),
    (p: string, h: typeof handleBackend) => app.post(p, h),
    (p: string, h: typeof handleBackend) => app.put(p, h),
    (p: string, h: typeof handleBackend) => app.patch(p, h),
    (p: string, h: typeof handleBackend) => app.delete(p, h),
    (p: string, h: typeof handleBackend) => app.options(p, h),
  ]
  for (const reg of BACKEND_METHODS) {
    reg("/api/apps/:id", handleBackend)
    reg("/api/apps/:id/*", handleBackend)
  }

  // ── 管理面：应用清单与刷新（宿主知识留在 v1 资源域，不与应用自身的 URL 空间抢路径）──
  app.get("/api/v1/apps", (c) => {
    const entries = store ? store.list(true) : []
    return c.json({
      apps: entries.map((e) => ({
        id: e.manifest.id,
        title: e.manifest.title,
        description: e.manifest.description,
        version: e.manifest.version,
        entry: e.manifest.entry,
        spa: e.manifest.spa,
        listed: e.manifest.listed,
        layer: e.layer,
        backend: e.hasBackend && (backend?.enabled ?? false),
        url: `/apps/${e.manifest.id}/`,
        api: e.hasBackend ? `/api/apps/${e.manifest.id}` : null,
        ...(e.error ? { error: e.error } : {}),
        ...(e.warnings.length ? { warnings: e.warnings } : {}),
      })),
      backend: { enabled: backend?.enabled ?? false, ...(backend && !backend.enabled ? { reason: backend.disabledReason } : {}) },
      roots: store ? store.roots() : [],
    })
  })
  app.post("/api/v1/apps/refresh", (c) => {
    if (!store) return c.json({ error: "托管应用未启用（GEBAI_APPS）" }, 503)
    store.invalidate()
    return c.json({ ok: true, apps: store.list(true).map((e) => e.manifest.id) })
  })
}
