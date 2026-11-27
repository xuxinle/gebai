/**
 * 「你好，歌白」示例应用的后端模块（目录形态的 server.js）。
 *
 * 三种导出形态任一即可（本文件演示 `routes` 与 `default` 混用）：
 * - `export default function (ctx)` —— 单函数，最自由；
 * - `export function fetch(ctx)` —— 同上，语义更明确；
 * - `export const routes = { "GET /health": … }` —— 路由表，最省事。
 *
 * `ctx` 只读：请求信息 + 响应助手 + 清单 env + 最小身份。它**不含**工具/引擎/会话存储——
 * 后端模块是部署方自己的代码，能力边界靠「不给」而不是「拦住」。
 *
 * 改这个文件后**下一次请求即生效**（按 mtime 热重载，不需要重启服务）。
 */

/** 健康检查：同时也是「后端到底有没有启用」的第一条线索（未启用时宿主会直接回 503 并说明原因）。 */
export const routes = {
  "GET /health": (ctx) => ({
    ok: true,
    app: ctx.appId,
    greeting: ctx.env.DEMO_GREETING ?? "你好",
    user: ctx.user,
    method: ctx.method,
    path: ctx.path,
  }),

  "POST /greet": async (ctx) => {
    const body = await ctx.json().catch(() => null)
    const name = String((body && body.name) || "世界").slice(0, 60)
    return {
      greeting: `${ctx.env.DEMO_GREETING ?? "你好"}，${name}！`,
      at: new Date().toISOString(),
      // 证明它跑在宿主进程里：同一个进程的 pid（与宿主一致）
      pid: typeof process !== "undefined" ? process.pid : null,
    }
  },
}

/**
 * 兜底：未命中路由表时由这里接住（返回 Response 直接发出；返回对象按 JSON 响应）。
 * 演示 `ctx.respond` 定制状态码与头。
 */
export default function fallback(ctx) {
  return ctx.respond({ error: `未定义的路由：${ctx.method} /${ctx.path}`, hint: "可用：GET /health、POST /greet" }, { status: 404 })
}
