/**
 * 应用基准路径：同源部署下 REST / WS / 静态资源路径的**前缀**，由当前页面 URL 推出。
 *
 * 页面只出现在两个路径上——`<基准>/`（主界面，index.html）与 `<基准>/files`（文件工作台，
 * 只改 query 不改 pathname）——据此从文档 URL 反推基准，因此部署在反向代理子路径
 * （`/gebai/`、`/gebai`、`/a/b/` 等）下**无需任何配置**：请求自带该前缀，代理剥离前缀即可。
 *
 * 非层级文档（WebView srcdoc/about:blank、file: 等无站点基准）回落根部署（空基准）。
 */

/** 末段属于「页面文件」的名字：应用只在这几种路径上呈现，其余末段视为挂载根本身。 */
const PAGE_SEGMENTS = new Set(["", "files", "index.html"])

/**
 * 末段是否是一个**页面文件**（而不是挂载目录）。
 *
 * 为何要单独判 `.html`：服务端除了 `/` 与 `/files` 两个「美化路径」，静态兜底还会直接以
 * `/files.html`（以及构建期其它入口页）提供同一页面——这是用户实际会落在的 URL（收藏、
 * 外部链接、直接敲文件名）。如果只认固定几个名字，`/files.html` 的末段就会被当成**挂载根**，
 * 整个页面的请求变成 `/files.html/api/v1/…` 而全部 404（页面上表现为“一直转圈 + 满屏 404”）。
 */
function isPageFile(seg: string): boolean {
  return PAGE_SEGMENTS.has(seg) || /\.html?$/i.test(seg)
}

/** 从页面 pathname 推出应用基准：""（根部署）或 "/gebai"（子路径部署，无尾斜杠）。 */
export function resolveAppBase(pathname: string): string {
  if (!pathname.startsWith("/")) return ""
  const cut = pathname.lastIndexOf("/")
  const seg = pathname.slice(cut + 1)
  if (isPageFile(seg)) return cut === 0 ? "" : pathname.slice(0, cut).replace(/\/+$/, "")
  // 末段不是页面文件：整个 pathname 即挂载根（`/gebai` 这类不带尾斜杠的访问形式）
  return pathname.replace(/\/+$/, "")
}

/** 当前文档 URL（DOM 环境可解析时返回，否则 null）。 */
function docUrl(): URL | null {
  const g = globalThis as Record<string, unknown>
  const doc = g.document as { baseURI?: string } | undefined
  const href = doc?.baseURI || (g.location as { href?: string } | undefined)?.href
  if (!href) return null
  try {
    return new URL(href)
  } catch {
    return null
  }
}

/** 文档位置（协议/主机/路径）：WS 绝对地址与页面基准推导共用，非 DOM 环境返回 null。 */
export function docLocation(): { protocol: string; host: string; pathname: string } | null {
  const u = docUrl()
  return u ? { protocol: u.protocol, host: u.host, pathname: u.pathname } : null
}

/** 应用基准路径：""（根部署）或 "/gebai"（反代子路径部署）。 */
export function appBase(): string {
  return resolveAppBase(docUrl()?.pathname ?? "")
}

/** 应用内路径（`/api/v1/x` → 带基准的 `/gebai/api/v1/x`；根部署下原样返回）。 */
export function appPath(path: string): string {
  return `${appBase()}${path}`
}

/**
 * WS 绝对地址（WebSocket 构造要求可解析为 ws/wss 的绝对 URL）：同源 + 应用基准 + path。
 * 非层级文档下返回相对 path（调用方按需再解析）。
 */
export function appWsUrl(path: string): string {
  const loc = docLocation()
  if (!loc?.host || (loc.protocol !== "http:" && loc.protocol !== "https:")) return path
  return `${loc.protocol === "https:" ? "wss" : "ws"}://${loc.host}${appBase()}${path}`
}

/**
 * 解析 WS 连接地址：显式 baseUrl 优先（http→ws / https→wss，用于跨源与 Node 集成）；
 * 否则按文档协议/主机 + 应用基准解析为绝对地址（见 appWsUrl）。
 */
export function resolveWsUrl(baseUrl: string, loc?: { protocol: string; host: string; pathname?: string } | null): string {
  if (baseUrl) {
    const proto = baseUrl.startsWith("https") ? "wss" : "ws"
    return `${proto}://${baseUrl.replace(/^https?:\/\//, "")}/ws`
  }
  if (!loc?.host || (loc.protocol !== "http:" && loc.protocol !== "https:")) return "/ws"
  return `${loc.protocol === "https:" ? "wss" : "ws"}://${loc.host}${resolveAppBase(loc.pathname ?? "/")}/ws`
}
