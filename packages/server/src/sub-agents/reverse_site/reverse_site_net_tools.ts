import type { ToolContext, ToolSet } from "../../core/base/types"
import { truncate, artifactBlocks } from "../../core/tools"
import type { ToolSchema } from "@gebai/sdk"
import { createLazyBridge, withSessionLock, type BridgeLike } from "../../core/browser/bridge"
import { parseJsonObject, type CapturedRequest } from "./reverse_site_tools"

/**
 * reverse_site 子Agent 网络增强工具集：HAR 导出 / WebSocket 帧捕获 / 请求拦截。
 *
 * 与 capture_* 共享同一惰性桥接单例与浏览器会话（createLazyBridge 全进程共享）。
 * 安全口径：本文件工具均为只读或浏览器上下文内的行为修改（拦截规则不产生新的
 * 网络出口，mock 响应不出浏览器），免审批；route add 由工具级函数按 action 判定。
 */

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

function num(v: unknown, dflt: number): number {
  const n = Number(v)
  return Number.isFinite(n) && n > 0 ? n : dflt
}

function fail(err: unknown): string {
  return err instanceof Error ? err.message : String(err)
}

/** 网络录制单条详情（network_list detail=true 返回形态）。 */
type CapturedDetail = CapturedRequest & Required<Pick<CapturedRequest, "requestHeaders" | "postData" | "responseHeaders" | "body">>

/** 录制条目 → HAR 1.2（Chrome DevTools 可打开）。敏感字段沿用录制时的脱敏值（***）。纯函数，可单测。 */
export function buildHar(entries: CapturedDetail[]): Record<string, unknown> {
  return {
    log: {
      version: "1.2",
      creator: { name: "GEBAI reverse_site capture", version: "1" },
      entries: entries.map((e) => {
        let u: URL
        try {
          u = new URL(e.url)
        } catch {
          u = new URL("http://unknown.invalid/")
        }
        return {
          startedDateTime: new Date(e.time).toISOString(),
          time: 0,
          request: {
            method: e.method,
            url: e.url,
            httpVersion: "HTTP/1.1",
            cookies: [],
            headers: Object.entries(e.requestHeaders ?? {}).map(([name, value]) => ({ name, value })),
            queryString: [...u.searchParams].map(([name, value]) => ({ name, value })),
            postData: e.postData
              ? { mimeType: e.requestHeaders?.["content-type"] ?? "", text: e.postData }
              : undefined,
            headersSize: -1,
            bodySize: e.postData ? e.postData.length : 0,
          },
          response: {
            status: e.status || 0,
            statusText: "",
            httpVersion: "HTTP/1.1",
            cookies: [],
            headers: Object.entries(e.responseHeaders ?? {}).map(([name, value]) => ({ name, value })),
            content: {
              text: e.body ?? "",
              size: (e.body ?? "").length,
              mimeType: e.responseHeaders?.["content-type"] ?? "",
              ...(e.error ? { comment: e.error } : {}),
            },
            redirectURL: e.responseHeaders?.["location"] ?? "",
            headersSize: -1,
            bodySize: (e.body ?? "").length,
          },
          cache: {},
          timings: { send: 0, wait: 0, receive: 0 },
        }
      }),
    },
  }
}

export function createNetTools(deps: { bridge?: BridgeLike } = {}): ToolSet {
  const bridge: BridgeLike = deps.bridge ?? createLazyBridge()
  const request = (sessionId: string, op: string, args: Record<string, unknown>): Promise<unknown> =>
    withSessionLock(sessionId, () => bridge.request(op, { sessionId, ...args }))

  return {
    capture_har: {
      name: "capture_har",
      description: "把录制的网络请求导出为 HAR 1.2 文件（Chrome DevTools/Charles 等可直接打开对照分析）。敏感字段与 capture_list 同口径脱敏（authorization/cookie 等为 ***）。",
      parameters: schema({
        file: { type: "string", description: "保存的会话相对路径（默认 tmp/capture.har）" },
      }),
      async execute(args, ctx: ToolContext) {
        const rel = String(args.file ?? "").trim() || "tmp/capture.har"
        try {
          const r = (await request(ctx.sessionId, "network_list", { detail: true, limit: 500 })) as { entries: CapturedDetail[] }
          if (!r.entries || r.entries.length === 0) return { output: "没有可导出的录制记录（先 capture_start 并浏览页面）" }
          const har = buildHar(r.entries)
          await ctx.writeFile(ctx.resolvePath(rel), JSON.stringify(har, null, 2))
          return { output: `已导出 ${r.entries.length} 条请求为 HAR: ${rel}`, blocks: artifactBlocks(rel) }
        } catch (err) {
          return { output: `HAR 导出失败: ${fail(err)}` }
        }
      },
    },

    capture_ws: {
      name: "capture_ws",
      description: "查看录制的 WebSocket 帧（随 capture_start/stop 开关联动）——实时推送类接口（行情/聊天/通知）的逆向入口。url 按子串过滤，last 返回最近 N 帧（默认 100）。",
      parameters: schema({
        url: { type: "string", description: "按 WebSocket 地址过滤（子串）" },
        last: { type: "number", description: "返回最近多少帧（默认 100，上限 300）" },
      }),
      async execute(args, ctx) {
        try {
          const r = (await request(ctx.sessionId, "network_ws_list", {
            url: args.url === undefined ? "" : String(args.url),
            last: num(args.last, 100),
          })) as {
            frames: Array<{ id: number; time: number; dir: string; url: string; payload: string }>
            total: number
            recording: boolean
          }
          if (r.frames.length === 0) {
            return { output: `未捕获到 WebSocket 帧（共 ${r.total} 帧，${r.recording ? "录制中" : "已停止"}）。实时接口需先 capture_start，再让页面建立连接（刷新/操作触发）。` }
          }
          const lines = r.frames.map((f) => {
            const time = new Date(f.time).toISOString().slice(11, 19)
            return `[${f.id}] ${time} ${f.dir === "sent" ? "↑发" : "↓收"} ${f.url}\n    ${f.payload}`
          })
          return truncate(`WebSocket 帧 ${r.frames.length}/${r.total}（${r.recording ? "录制中" : "已停止"}）：\n${lines.join("\n")}`, "capture_ws", ctx)
        } catch (err) {
          return { output: `读取 WebSocket 帧失败: ${fail(err)}` }
        }
      },
    },

    route: {
      name: "route",
      description:
        "请求拦截（作用于浏览器内全部请求，持续到 clear 或会话结束）：mode=block 中止匹配请求（屏蔽广告/埋点/干扰资源，加速加载）；mode=mock 伪造响应（status/content_type/body——前端联调、绕过客户端 gating 验证服务端行为）；mode=modify 改写请求头后放行（headers 覆盖合并）。action=add（默认）/list/clear。",
      parameters: schema({
        action: { type: "string", description: "add（默认）/ list / clear" },
        pattern: { type: "string", description: "add 时的匹配 glob，如 **/api/** 或 https://a.com/*.js" },
        mode: { type: "string", description: "拦截方式：block（默认）/ mock / modify" },
        status: { type: "number", description: "mock 时的响应状态码（默认 200）" },
        content_type: { type: "string", description: "mock 时的响应 Content-Type（默认 application/json）" },
        body: { type: "string", description: "mock 时的响应体" },
        headers: { type: "string", description: 'modify 时覆盖合并的请求头 JSON 对象字符串，如 {"x-custom":"1"}' },
      }),
      requiresApproval: (args) => args.action === "list" || args.action === "clear" ? false : true,
      async execute(args, ctx) {
        const action = String(args.action ?? "add")
        try {
          if (action === "clear") {
            const r = (await request(ctx.sessionId, "route_clear", {})) as { cleared: number }
            return { output: `已清空 ${r.cleared} 条拦截规则` }
          }
          if (action === "list") {
            const r = (await request(ctx.sessionId, "route_list", {})) as {
              routes: Array<{ pattern: string; mode: string; status?: number; contentType?: string; headers?: Record<string, string> | null }>
            }
            if (r.routes.length === 0) return { output: "当前没有拦截规则" }
            const lines = r.routes.map((r2) => `[${r2.mode}] ${r2.pattern}${r2.status ? ` → ${r2.status}` : ""}${r2.headers ? ` headers=${JSON.stringify(r2.headers)}` : ""}`)
            return { output: `共 ${r.routes.length} 条拦截规则：\n${lines.join("\n")}` }
          }
          if (action !== "add") return { output: `action 必须是 add/list/clear: ${action}` }
          let headers: Record<string, string> | null = null
          if (args.headers !== undefined && String(args.headers).trim()) {
            try {
              headers = parseJsonObject(args.headers)
            } catch (err) {
              return { output: (err as Error).message }
            }
          }
          const r = (await request(ctx.sessionId, "route_add", {
            pattern: String(args.pattern ?? ""),
            mode: String(args.mode ?? "block"),
            status: args.status,
            contentType: args.content_type === undefined ? "" : String(args.content_type),
            body: args.body === undefined ? "" : String(args.body),
            headers,
          })) as { added: string; mode: string; total: number }
          return { output: `已添加拦截规则 [${r.mode}] ${r.added}（当前 ${r.total} 条，clear 清除）` }
        } catch (err) {
          return { output: `拦截规则操作失败: ${fail(err)}` }
        }
      },
    },
  }
}
