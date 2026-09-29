/**
 * 文件工作台 · URL 基准（页面内一切请求与资源路径的唯一入口）。
 *
 * 工作台页面位于 `<基准>/files`（根部署 `/files`，反向代理子路径 `/gebai/files`），页面内 URL
 * 一律写成**相对当前文档**的形式（`./api/v1/…`、`./vendor/…`），由浏览器按页面位置解析：
 * 代理剥离前缀转发即可，页面侧不做前缀推导，改前缀、加层级都不用动代码。
 *
 * 两种形态的分工：
 * - `wbUrl`：相对路径字符串，直接交给浏览器解析（fetch、`<script src>`、`<link href>`、iframe src、window.open）；
 * - `wbAbsUrl`：按文档基准解析后的绝对 URL，供 `import()` 与 Monaco / wasm 使用——动态导入的相对说明符
 *   相对**模块文件**而非页面，Monaco 的 `paths.vs` 与 `locateFile` 也要能被内部二次拼接。
 *
 * WebSocket 例外：`new WebSocket()` 只接受绝对 URL，走 SDK 的 `appWsUrl`（见 `ws-client.ts`）。
 *
 * 回落：页面被宿主放在非 `<基准>/files` 位置时相对解析会落在别处——此时改用与前端其它推导同一口径的
 * SDK 基准前缀（`appPath`），不因页面位置异常而请求错位。
 */
import { appPath } from "@gebai/sdk"

/** 端点规范化（`api/x` 与 `/api/x` 等价）：去掉前导斜杠后两种写法得到同一结果。 */
function normalize(endpoint: string): string {
  return `/${endpoint.replace(/^\/+/, "")}`
}

/** 相对路径（`/api/v1/x` → `./api/v1/x`）：浏览器按当前文档解析；页面位置异常时回落基准前缀写法。 */
export function wbUrl(endpoint: string): string {
  const ep = normalize(endpoint)
  const rel = `.${ep}`
  return samePlace(rel, ep) ? rel : appPath(ep)
}

/** 相对路径按文档基准解析为绝对 URL（动态 `import()`、Monaco 基准、wasm `locateFile` 用）。 */
export function wbAbsUrl(endpoint: string): string {
  const doc = docHref()
  const rel = wbUrl(endpoint)
  if (!doc) return rel
  try {
    return new URL(rel, doc).href
  } catch {
    return appPath(normalize(endpoint))
  }
}

/** 文档地址（浏览器相对解析的基准；非 DOM 环境返回 null）。 */
function docHref(): string | null {
  const g = globalThis as { document?: { baseURI?: string }; location?: { href?: string } }
  return g.document?.baseURI || g.location?.href || null
}

/**
 * 相对写法与前端基准前缀是否落在同一处。
 * 页面在 `<基准>/files`（或根部署 `/files`）下两者一致；页面位置非标准时不一致，调用方改用基准前缀。
 * 无文档（单测等非 DOM 环境）时视为一致——相对写法原样保留，由调用方按需解析。
 */
function samePlace(rel: string, path: string): boolean {
  const doc = docHref()
  if (!doc) return true
  try {
    return new URL(rel, doc).pathname === new URL(appPath(path), doc).pathname
  } catch {
    return false
  }
}
