import type { ToolSchema } from "@gebai/sdk"
import type { Tool, ToolResult, ToolSet } from "../../core/types"
import { truncate, assertPublicHttpUrl, fetchWithRedirectGuard } from "../../core/tools"
import { createLazyBridge, type BridgeLike } from "../playwright/playwright_tools"

/**
 * reverse_site 子Agent 工具集：接口逆向。
 *
 * - `http_request`：直接发送 HTTP 请求探测接口（任意方法/请求头/请求体/查询参数），
 *   返回状态码、响应头（敏感字段脱敏）与响应体（超长截断）；服务端部署限公网地址（防 SSRF）。
 * - `capture_*`：浏览器网络请求录制（start/stop/clear/list），与 playwright 工具共享
 *   同一桥接进程与浏览器会话——录制接口驱动侧实现（driver.mjs network_* 操作），
 *   请求头/体与响应体预览在录制时自动脱敏。
 */

const HTTP_BODY_CAP = 50_000 // http_request 响应体展示上限（字符）
const HTTP_TIMEOUT_DEFAULT = 15_000
const HTTP_METHODS = ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]
/** 输出时脱敏的敏感响应头（值替换为 ***）。 */
const SENSITIVE_HEADERS = new Set([
  "authorization", "cookie", "proxy-authorization", "x-api-key", "x-auth-token",
  "x-csrf-token", "x-access-token", "set-cookie", "www-authenticate",
])

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

function num(v: unknown, dflt: number): number {
  const n = Number(v)
  return Number.isFinite(n) && n > 0 ? n : dflt
}

/** JSON 对象字符串（或对象字面量）解析为 Record<string,string>；非法 JSON 抛错。 */
export function parseJsonObject(v: unknown): Record<string, string> | null {
  if (v === undefined || v === null) return null
  if (typeof v === "object" && !Array.isArray(v)) {
    return Object.fromEntries(Object.entries(v as Record<string, unknown>).map(([k, val]) => [k, String(val)]))
  }
  const raw = String(v).trim()
  if (!raw) return null
  let obj: unknown
  try {
    obj = JSON.parse(raw)
  } catch {
    throw new Error(`参数必须是 JSON 对象字符串: ${raw}`)
  }
  if (!obj || typeof obj !== "object" || Array.isArray(obj)) throw new Error(`参数必须是 JSON 对象字符串: ${raw}`)
  return Object.fromEntries(Object.entries(obj as Record<string, unknown>).map(([k, val]) => [k, String(val)]))
}

/** 敏感响应头脱敏（值替换为 ***）。 */
export function redactHeaders(headers: Record<string, string>): Record<string, string> {
  const out: Record<string, string> = {}
  for (const [k, v] of Object.entries(headers)) out[k] = SENSITIVE_HEADERS.has(k.toLowerCase()) ? "***" : v
  return out
}

/** 格式化 http_request 结果（状态行 + 响应头 + 响应体）。纯函数，可单测。 */
export function formatHttpResult(status: number, statusText: string, headers: Record<string, string>, body: string): string {
  const lines = [`HTTP ${status}${statusText ? ` ${statusText}` : ""}`]
  for (const [k, v] of Object.entries(headers)) lines.push(`${k}: ${v}`)
  if (body) lines.push("", body)
  return lines.join("\n")
}

/** fetch 的最小可注入签名（测试替身用，避免依赖全局 fetch 类型）。 */
export type FetchLike = (url: string, init?: RequestInit) => Promise<Response>

/** http_request 工具（fetch 可注入，便于测试；默认路径带逐跳 SSRF 守卫）。 */
export function createHttpRequestTool(deps: { fetch?: FetchLike } = {}): Tool {
  return {
    name: "http_request",
    description:
      "直接发送 HTTP 请求探测接口（任意方法/请求头/请求体/查询参数），返回状态码、响应头（敏感字段脱敏）与响应体（超长截断）。用于验证逆向出的接口与参数。",
    parameters: schema(
      {
        url: { type: "string", description: "接口地址（http/https）" },
        method: { type: "string", description: `请求方法（默认 GET）: ${HTTP_METHODS.join("/")}`, enum: HTTP_METHODS },
        headers: { type: "string", description: `请求头 JSON 对象字符串，如 {"Content-Type":"application/json"}` },
        body: { type: "string", description: "请求体（字符串或 JSON 字符串）" },
        params: { type: "string", description: "查询参数 JSON 对象字符串，合并进 URL" },
        timeout: { type: "number", description: "超时毫秒（默认 15000）" },
      },
      ["url"]
    ),
    async execute(args, ctx): Promise<ToolResult> {
      let url = String(args.url ?? "").trim()
      if (!url) return { output: "缺少 url 参数" }
      const params = parseJsonObject(args.params)
      if (params) {
        try {
          const u = new URL(url)
          for (const [k, v] of Object.entries(params)) u.searchParams.set(k, v)
          url = u.toString()
        } catch {
          return { output: `无效的 URL: ${url}` }
        }
      }
      if (ctx.sandboxed) {
        try {
          assertPublicHttpUrl(url)
        } catch (err) {
          return { output: `http_request 失败: ${(err as Error).message}` }
        }
      }
      let headers: Record<string, string> = {}
      try {
        headers = parseJsonObject(args.headers) ?? {}
      } catch (err) {
        return { output: (err as Error).message }
      }
      const method = String(args.method ?? "GET").toUpperCase()
      const body = args.body === undefined || args.body === null ? undefined : String(args.body)
      if (body !== undefined && headers["Content-Type"] === undefined && headers["content-type"] === undefined) {
        headers["Content-Type"] = "application/json"
      }
      try {
        // 默认路径：逐跳校验重定向（初始 URL 与每跳 Location 均须通过公网校验，防 302 跳板绕过）；
        // 测试注入的 fetcher 原样使用（测试场景自行控制）
        const fetcher: FetchLike = deps.fetch ?? ((url, init) => fetchWithRedirectGuard(url, init ?? {}, (u) => { if (ctx.sandboxed) assertPublicHttpUrl(u) }))
        const res = await fetcher(url, {
          method,
          headers,
          body,
          signal: AbortSignal.timeout(num(args.timeout, HTTP_TIMEOUT_DEFAULT)),
        })
        const respHeaders = redactHeaders(Object.fromEntries(res.headers.entries()))
        const ct = res.headers.get("content-type") || ""
        let bodyText = ""
        if (!/text|json|xml|javascript|html|form/i.test(ct)) {
          bodyText = `(非文本响应体: ${ct || "未知类型"}，已跳过)`
        } else {
          const text = await res.text()
          bodyText = text.length > HTTP_BODY_CAP ? `${text.slice(0, HTTP_BODY_CAP)}\n…[响应体超长已截断]` : text
        }
        return truncate(formatHttpResult(res.status, res.statusText, respHeaders, bodyText), "http_request", ctx)
      } catch (err) {
        return { output: `http_request 失败: ${(err as Error).message}` }
      }
    },
  }
}

/** 网络录制中的单条请求记录（driver network_list 返回）。 */
export interface CapturedRequest {
  id: number
  time: number
  method: string
  url: string
  resourceType?: string
  status: number
  error?: string | null
  requestHeaders?: Record<string, string>
  postData?: string
  responseHeaders?: Record<string, string>
  body?: string
}

/** capture_* 网络录制工具（与 playwright 工具共享桥接进程/浏览器会话，默认惰性单例）。 */
export function createCaptureTools(deps: { bridge?: BridgeLike } = {}): ToolSet {
  const bridge: BridgeLike = deps.bridge ?? createLazyBridge()
  const request = (sessionId: string, op: string, args: Record<string, unknown>): Promise<unknown> =>
    bridge.request(op, { sessionId, ...args })

  return {
    capture_start: {
      name: "capture_start",
      description:
        "开始记录浏览器发出的网络请求（XHR/fetch 等，含请求头/请求体/响应状态与响应体预览，敏感字段自动脱敏）。录制开启后再浏览/操作页面，随后用 capture_list 分析。幂等；同一会话可与 open/click 等工具共享浏览器。",
      parameters: schema({}),
      async execute(_args, ctx) {
        try {
          const r = (await request(ctx.sessionId, "network_start", {})) as { captured: number }
          return { output: `已开始记录网络请求（已捕获 ${r.captured} 条）。` }
        } catch (err) {
          return { output: `开始记录失败: ${err instanceof Error ? err.message : String(err)}` }
        }
      },
    },

    capture_stop: {
      name: "capture_stop",
      description: "停止记录网络请求（已记录的数据保留，可用 capture_list 查看）。",
      parameters: schema({}),
      async execute(_args, ctx) {
        try {
          const r = (await request(ctx.sessionId, "network_stop", {})) as { captured: number }
          return { output: `已停止记录，共捕获 ${r.captured} 条请求。` }
        } catch (err) {
          return { output: `停止记录失败: ${err instanceof Error ? err.message : String(err)}` }
        }
      },
    },

    capture_clear: {
      name: "capture_clear",
      description: "清空已记录的请求，重新开始（不影响录制开关状态）。",
      parameters: schema({}),
      async execute(_args, ctx) {
        try {
          const r = (await request(ctx.sessionId, "network_clear", {})) as { cleared: number }
          return { output: `已清空 ${r.cleared} 条记录。` }
        } catch (err) {
          return { output: `清空失败: ${err instanceof Error ? err.message : String(err)}` }
        }
      },
    },

    capture_list: {
      name: "capture_list",
      description:
        "列出录制的网络请求。默认仅摘要（序号/时间/方法/地址/状态/类型）；detail=true 时包含请求/响应头与体预览。",
      parameters: schema({
        detail: { type: "boolean", description: "是否包含请求/响应头与体（默认 false）" },
        method: { type: "string", description: "按请求方法过滤（GET/POST/...）" },
        url: { type: "string", description: "按 URL 过滤（正则或子串）" },
        status: { type: "number", description: "按响应状态码过滤" },
        file: { type: "string", description: "可选：完整记录保存为会话 tmp/ 下 JSON 文件名（如 api_capture.json）" },
      }),
      async execute(args, ctx) {
        try {
          const r = (await request(ctx.sessionId, "network_list", {
            detail: args.detail === true,
            method: args.method === undefined ? "" : String(args.method),
            url: args.url === undefined ? "" : String(args.url),
            status: args.status,
          })) as { entries: CapturedRequest[]; captured: number; recording: boolean }
          if (r.entries.length === 0) {
            return { output: `未捕获到请求（共 ${r.captured} 条，${r.recording ? "录制中" : "已停止"}）。请先 capture_start 再浏览/操作页面。` }
          }
          const detail = args.detail === true
          const lines = r.entries.map((e) => {
            const time = new Date(e.time).toISOString().slice(11, 19)
            const status = e.status ? String(e.status) : "-"
            const line = `[${e.id}] ${time} ${e.method} ${e.url}  ${status}  ${e.resourceType ?? ""}${e.error ? `  ${e.error}` : ""}`
            if (!detail) return line
            const parts = [line]
            if (e.requestHeaders && Object.keys(e.requestHeaders).length) parts.push(`    请求头: ${JSON.stringify(e.requestHeaders)}`)
            if (e.postData) parts.push(`    请求体: ${e.postData}`)
            if (e.responseHeaders && Object.keys(e.responseHeaders).length) parts.push(`    响应头: ${JSON.stringify(e.responseHeaders)}`)
            if (e.body) parts.push(`    响应体: ${e.body}`)
            return parts.join("\n")
          })
          let output = `捕获 ${r.entries.length} 条请求（共 ${r.captured} 条，${r.recording ? "录制中" : "已停止"}）：\n${lines.join("\n")}`
          if (args.file) {
            const rel = String(args.file)
            const abs = ctx.resolvePath(rel)
            await ctx.writeFile(abs, JSON.stringify(r.entries, null, 2))
            output += `\n完整记录（含详情）已保存: ${rel}`
          }
          return truncate(output, "capture_list", ctx)
        } catch (err) {
          return { output: `读取失败: ${err instanceof Error ? err.message : String(err)}` }
        }
      },
    },
  }
}
