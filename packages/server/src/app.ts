/** Hono 应用工厂：全局中间件（CORS/鉴权）+ 各域路由装配（routes/ 目录，处理器按域拆分）+ AppDeps。
 *  域路由文件契约见 routes/context.ts（register{Domain}Routes）；装配顺序有意义——sessions 先于
 *  session-files（共享 /sessions/:id 白名单中间件），static 最后（`/` 与 assets 通配兜底）。 */
import { Hono } from "hono"
import type { Context } from "hono"
import type { AuthService, AuthUser } from "./auth"
import type { ExternalAuthProvider } from "./external-auth"
import type { SessionStore } from "./core/session/store"
import type { EnvManager } from "./core/session/env"
import type { Sandbox } from "./core/security/sandbox"
import type { ToolRegistry } from "./core/base/registry"
import type { AgentEngine } from "./core/engine/engine"
import type { EventBus } from "./core/base/event-bus"
import type { SubAgentManager } from "./core/agents/subagents"
import type { WebhookManager } from "./webhooks"
import type { ServerConfig } from "./core/base/config"
import type { RouteCtx } from "./routes/context"
import { registerAuthRoutes } from "./routes/auth"
import { registerUserRoutes } from "./routes/users"
import { registerSessionRoutes } from "./routes/sessions"
import { registerSessionFileRoutes } from "./routes/session-files"
import { registerToolRoutes } from "./routes/tools"
import { registerCronRoutes } from "./routes/cron"
import { registerFeedbackRoutes, registerWebhookRoutes, registerMiniToolRoutes } from "./routes/misc"
import { registerDocsRoutes } from "./routes/docs"
import { registerStaticRoutes } from "./routes/static"

export interface AppDeps {
  config: ServerConfig
  store: SessionStore
  env: EnvManager
  sandbox: Sandbox
  registry: ToolRegistry
  engine: AgentEngine
  auth: AuthService
  events: EventBus
  subAgents: SubAgentManager
  webhooks: WebhookManager
  /** 定时任务调度器（GEBAI_CRON_ENABLED=false 时不启动，为 null——REST 返回能力未启用）。 */
  cron?: import("./core/schedule/cron").CronManager | null
  /** 外部身份验证器（GEBAI_EXTERNAL_AUTH_* 配置；未配置为 null）。 */
  externalAuth: ExternalAuthProvider | null
  /** WS 状态服务（MVC 模型层：事件日志/连接状态/快照）；由 startServer 注入。 */
  state?: import("./ws-state").WsStateService
}

export type AppEnv = { Variables: { deps: AppDeps; user: AuthUser } }

/**
 * 未登录占位身份：仅用于「未认证请求」的上下文兜底与公开端点（登录/健康检查等）的身份占位，
 * 不作为任何认证结果——服务模式接口统一走账号密码登录（Bearer 令牌），无独立服务令牌。
 */
export const SERVICE_USER: AuthUser = {
  id: "service",
  username: "api-service",
  role: "admin",
  disabled: false,
  createdAt: 0,
  salt: "",
  hash: "",
}

async function resolveUser(d: AppDeps, c: Context): Promise<AuthUser | null> {
  if (d.config.auth === "local") return d.auth.defaultUser()
  const auth = c.req.header("Authorization")
  if (auth && /^bearer /i.test(auth)) {
    return d.auth.authorize(auth.slice(7))
  }
  // HTTP Basic（RFC 7617）：`Authorization: Basic base64(username:password)`，等价隐式登录——
  // 复用密码校验与登录限流（verifyCredentials 不签发令牌，适合单次调用场景）；失败统一 401 不泄露原因。
  // 注意：base64 非加密，须经 HTTPS 传输。scheme 大小写不敏感（RFC 7235）。
  if (auth && /^basic /i.test(auth)) {
    const decoded = Buffer.from(auth.slice(6), "base64").toString("utf8")
    const idx = decoded.indexOf(":")
    if (idx <= 0) return null
    return d.auth.verifyCredentials(decoded.slice(0, idx), decoded.slice(idx + 1))
  }
  return null
}

export function createApp(deps: AppDeps): Hono<AppEnv> {
  const app = new Hono<AppEnv>()
  const d = deps
  // CORS（GEBAI_CORS_ORIGINS，缺省 * 允许所有来源）：浏览器跨源接入 REST API 用；
  // WS 通道不受 CORS 约束（WebSocket 不执行同源策略，靠 token 鉴权）。
  // 手写中间件而非 hono/cors：cors 包会重建响应体，导致 Bun.file 自动推断的
  // content-type 丢失（files/content 二进制下载损坏）；c.header() 原地追加头不影响响应。
  const corsOrigins = (d.config.corsOrigins ?? []).length ? d.config.corsOrigins : ["*"]
  app.use("/api/*", async (c: Context<AppEnv>, next) => {
    const reqOrigin = c.req.header("origin") ?? ""
    // 本地/桌面免登录形态的跨站防护：CORS * + 免鉴权 = 任意网页可跨源读写全部 API（等效 RCE）。
    // 浏览器跨源请求必带 Origin——服务模式有令牌鉴权豁免；显式配置 GEBAI_CORS_ORIGINS 视为
    // 有意开放（按配置放行），仅缺省 * 且本地模式时要求 Origin 与 Host 同源（非浏览器无 Origin 不受限）
    if (d.config.auth !== "server" && corsOrigins.includes("*") && reqOrigin) {
      const host = c.req.header("host") ?? ""
      try {
        if (new URL(reqOrigin).host !== host) return c.json({ error: "cross-origin rejected" }, 403)
      } catch {
        return c.json({ error: "invalid origin" }, 403)
      }
    }
    const allow = corsOrigins.includes("*") ? "*" : corsOrigins.includes(reqOrigin) ? reqOrigin : corsOrigins[0]
    c.header("Access-Control-Allow-Origin", allow)
    c.header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS")
    c.header("Access-Control-Allow-Headers", "Content-Type, Authorization")
    c.header("Access-Control-Max-Age", "86400")
    c.header("Vary", "Origin")
    if (c.req.method === "OPTIONS") return new Response(null, { status: 204 })
    await next()
  })
  app.use(async (c: Context<AppEnv>, next) => {
    c.set("deps", deps)
    const user = await resolveUser(deps, c)
    if (!user && deps.config.auth === "server") {
      // 公开端点豁免鉴权：登录/注册/登出（服务模式首登无令牌）、健康检查（探活/负载均衡探测）
      if (
        c.req.path === "/api/v1/auth/login" ||
        c.req.path === "/api/v1/auth/register" ||
        c.req.path === "/api/v1/auth/logout" ||
        c.req.path === "/api/v1/auth/exchange" ||
        c.req.path === "/api/v1/auth/external-config" ||
        c.req.path === "/api/v1/oauth/feishu/callback" ||
        c.req.path === "/api/health" ||
        c.req.path === "/api/docs" // OpenAPI 文档：公开（无敏感信息），便于集成方调试
      ) {
        c.set("user", SERVICE_USER)
        return next()
      }
      // Web UI 静态资源（非 /api/* 路径：`/`、`/assets/*`、favicon 等）免鉴权——
      // 未登录也须能加载页面，前端随后经 API 401 触发登录页（修复服务模式访问 / 直接 401 无法出登录页）
      if (!c.req.path.startsWith("/api/")) {
        c.set("user", SERVICE_USER)
        return next()
      }
      c.set("user", SERVICE_USER)
      return c.json({ error: "unauthorized" }, 401)
    }
    c.set("user", user ?? SERVICE_USER)
    await next()
  })

  const userOf = (c: Context): AuthUser => (c as Context<AppEnv>).var.user

  /** 多用户模式管理员校验：非管理员返回 403（auth=none 时默认用户即 admin，不拦截）。 */
  const requireAdmin = (c: Context): Response | null => {
    const user = userOf(c)
    if (d.config.auth === "server" && user.role !== "admin") {
      return c.json({ error: "admin only" }, 403)
    }
    return null
  }

  const rc: RouteCtx = { app, d, userOf, requireAdmin }

  // Health
  app.get("/api/health", (c) => c.json({ ok: true, ts: Date.now() }))

  // 各域路由（装配顺序：sessions 的 :id 白名单中间件须先于 session-files 注册）
  registerAuthRoutes(rc)
  registerUserRoutes(rc)
  registerSessionRoutes(rc)
  registerSessionFileRoutes(rc)
  registerToolRoutes(rc)
  registerCronRoutes(rc)
  registerFeedbackRoutes(rc)
  registerWebhookRoutes(rc)
  registerMiniToolRoutes(rc)
  registerDocsRoutes(rc)
  registerStaticRoutes(rc)

  return app
}
