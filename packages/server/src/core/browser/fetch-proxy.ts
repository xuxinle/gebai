/**
 * 透明浏览器代理 fetch 垫片（GEBAI_BROWSER_PROXY=1 启用，服务启动时 boot/compose 安装）。
 *
 * 平台级能力，不依赖任何子Agent 的装载/裁剪（桥接基建同在 core/browser）；playwright/
 * reverse_site 子Agent 与本垫片经 createLazyBridge 共享同一浏览器会话。
 * 场景：内网部分站点仅允许浏览器访问（UA/登录态/网络策略校验），服务端进程直连被拒。
 * 启用后进程内 global fetch 被替换——工具执行作用域内（引擎 `runInToolFetchScope`，
 * 见 core/support/fetch-scope.ts）的 http(s) 请求经共享浏览器会话的 context.request
 * 发出（实测带浏览器 UA 与会话 cookie/登录态），响应构造回标准 Response。
 * 工具代码与模型零感知：工具 schema/提示词不变，self_optimize 生成的子Agent
 * 用普通 fetch 即可访问受限内网站点。
 *
 * 边界（详见 DESIGN「透明浏览器代理」）：
 * - 作用域外（LLM 请求/webhook/调度/启动）不受影响；嵌套引擎 LLM 请求经 runWithoutFetchProxy 豁免；
 * - 子进程（sh/py/js 沙箱）内的网络访问不经此垫片；
 * - Request 对象输入与非字符串请求体（FormData/Blob/流式）回退直连（multipart 上传等复杂形态）；
 * - 响应体经临时文件中转（二进制安全），上限沿用 driver BODY_FILE_MAX（20MB）；
 * - `redirect:"manual"` 映射 maxRedirects=0（逐跳校验守卫语义保留），其余自动跟随重定向。
 */
import { mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { currentToolFetchSession } from "../support/fetch-scope"
import { createLazyBridge, type BridgeLike } from "./bridge"

export const BROWSER_PROXY_ENV = "GEBAI_BROWSER_PROXY"

/** 环境变量开关解析：1/true/on/yes（大小写不敏感）为开，其余/缺省为关。 */
export function browserProxyEnabled(env: Record<string, string | undefined> = process.env): boolean {
  const v = (env[BROWSER_PROXY_ENV] ?? "").trim().toLowerCase()
  return v === "1" || v === "true" || v === "on" || v === "yes"
}

/** 无响应体状态码（Response 构造要求 body 为 null）。 */
const NULL_BODY_STATUS = new Set([204, 205, 304])

/** driver http_fetch 返回形态（outPath 模式：响应体已落盘，headers 未脱敏）。 */
interface HttpFetchResult {
  status: number
  headers: Record<string, string>
  path?: string
  size?: number
}

/** 代理请求的临时文件目录（driver 子进程写、宿主读后即删）。 */
function tempBodyPath(): string {
  const dir = join(tmpdir(), "gebai-fetch-proxy")
  mkdirSync(dir, { recursive: true })
  return join(dir, `body-${process.pid}-${Date.now()}-${Math.random().toString(36).slice(2)}`)
}

/** AbortSignal 语义保留：信号已触发/触发即以 AbortError 拒绝（桥接请求由 driver 超时收口）。 */
async function withAbort<T>(p: Promise<T>, signal?: AbortSignal | null): Promise<T> {
  if (!signal) return p
  if (signal.aborted) throw new DOMException("The operation was aborted.", "AbortError")
  return await Promise.race([
    p,
    new Promise<never>((_, reject) => {
      signal.addEventListener("abort", () => reject(new DOMException("The operation was aborted.", "AbortError")), { once: true })
    }),
  ])
}

/** 垫片主逻辑（导出供测试直接验证）：不可代理形态回退 direct，其余经桥接 http_fetch。 */
export async function proxyFetch(
  bridge: BridgeLike,
  direct: typeof fetch,
  input: string | URL | Request,
  init?: RequestInit,
): Promise<Response> {
  const url = typeof input === "string" ? input : input instanceof URL ? input.href : null
  const body = init?.body
  const sessionId = currentToolFetchSession()
  // 非工具作用域 / 非 http(s) / Request 对象输入 / 非字符串请求体：直连（保持原语义）
  if (
    !url || !/^https?:/i.test(url) || input instanceof Request || sessionId === undefined ||
    (body !== undefined && body !== null && typeof body !== "string")
  ) {
    return direct(input, init)
  }
  const method = (init?.method ?? "GET").toUpperCase()
  const headers: Record<string, string> = {}
  if (init?.headers !== undefined) for (const [k, v] of new Headers(init.headers)) headers[k] = v
  let outPath = ""
  try {
    outPath = tempBodyPath()
    const r = (await withAbort(
      bridge.request("http_fetch", {
        sessionId,
        url,
        method,
        headers,
        body: typeof body === "string" && body !== "" ? body : undefined,
        followRedirects: init?.redirect === "manual" ? false : undefined,
        outPath,
      }),
      init?.signal,
    )) as HttpFetchResult
    const buf = await Bun.file(r.path ?? outPath).arrayBuffer()
    return new Response(NULL_BODY_STATUS.has(r.status) ? null : buf, {
      status: r.status,
      headers: new Headers(r.headers ?? {}),
    })
  } finally {
    if (outPath) rmSync(outPath, { force: true })
  }
}

let installed = false

/** 安装垫片（幂等；环境变量未开启时 no-op）。构造零副作用：桥接惰性单例，真实浏览器
 *  在首次代理请求时才启动。force 供测试注入安装。 */
export function installBrowserFetchProxy(
  deps: { bridge?: BridgeLike; force?: boolean; env?: Record<string, string | undefined> } = {},
): void {
  if (installed) return
  if (!deps.force && !browserProxyEnabled(deps.env)) return
  const bridge = deps.bridge ?? createLazyBridge()
  const direct = globalThis.fetch.bind(globalThis)
  installed = true
  globalThis.fetch = ((input: string | URL | Request, init?: RequestInit): Promise<Response> =>
    proxyFetch(bridge, direct, input, init)) as typeof fetch
}
