import { Hono } from "hono"
import type { Context } from "hono"
import { serveStatic } from "hono/bun"
import { existsSync, readFileSync, statSync } from "node:fs"
import { rename } from "node:fs/promises"
import { join } from "node:path"
import type { AttachmentInput, EnvVarSource, FeedbackInfo, FeedbackInput, FileEntry, TodoItem } from "@gebai/sdk"
import type { AuthService, AuthUser } from "./auth"
import type { ExternalAuthProvider } from "./external-auth"
import type { SessionStore } from "./core/store"
import { toSessionInfo } from "./core/store"
import { validateEnvVars, maskEnv, filterEnvInjection } from "./core/env"
import { getEnvCatalog } from "./core/env-catalog"
import type { EnvManager } from "./core/env"
import type { Sandbox } from "./core/sandbox"
import type { ToolRegistry } from "./core/registry"
import type { AgentEngine } from "./core/engine"
import type { EventBus } from "./core/event-bus"
import type { SubAgentManager } from "./core/subagents"
import type { WebhookManager } from "./webhooks"
import type { ServerConfig } from "./core/config"
import { readFeedback, writeFeedback } from "./feedback"
import { basenameName, isValidSessionId, sessionPath } from "./core/paths"
import { findInTrash } from "./core/gc"
import { TokenBucket } from "./core/ratelimit"
import { buildZip } from "./zip"
import { deleteMiniTool, getMiniTool, listMiniTools } from "./core/mini-tools"
import { consumePendingAuth, defaultUserTokenStore, exchangeOAuthToken, fetchFeishuUserInfo, getPendingAuth, toUserTokenEntry } from "./sub-agents/feishu_docs/oauth"
import { feishuFetch } from "./feishu-bot/tls"
// 构建期由 scripts/build-web-bundle.ts 生成；dev 模式文件不存在时回退空表（Web UI 走源码 webDist）
function loadWebBundle(): Record<string, string> {
  try {
    return require("./core/web.bundle.generated").webBundle
  } catch {
    return {}
  }
}
const webBundle = loadWebBundle()

/** 二进制模式内嵌静态资源访问（web bundle 为空时返回 null）。 */
function embeddedWebAssets(): { get: (p: string) => Uint8Array<ArrayBuffer> | null } | null {
  const keys = Object.keys(webBundle)
  if (!keys.length) return null
  const byPath = new Map(keys.map((k) => [k, Buffer.from(webBundle[k], "base64")]))
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
  cron?: import("./core/cron").CronManager | null
  /** 外部身份验证器（GEBAI_EXTERNAL_AUTH_* 配置；未配置为 null）。 */
  externalAuth: ExternalAuthProvider | null
  /** WS 状态服务（MVC 模型层：事件日志/连接状态/快照）；由 startServer 注入。 */
  state?: import("./ws-state").WsStateService
}

type AppEnv = { Variables: { deps: AppDeps; user: AuthUser } }

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

  /** 每用户 prompt 速率限制（容量 60 突发、30/秒补充；防单用户刷 LLM 配额，与 WS 同规则）。 */
  const promptRateLimit = new TokenBucket(60, 30)
  /** 登录/兑换端点限流（防 CPU DoS：scrypt 即使异步化仍耗 CPU，轮换用户名即可绕过按用户名锁定）：
   *  全局桶兜底总量，来源桶按客户端标识（GEBAI_TRUST_PROXY=true 时取 X-Forwarded-For 首段，否则共桶）。 */
  const loginGlobalLimit = new TokenBucket(60, 2)
  const loginSourceLimit = new TokenBucket(10, 0.2)
  /** 注册独立桶（scrypt 同动机；不复用登录小来源桶——正常「登录多次+注册一次」的用量不互相挤占）。 */
  const registerGlobalLimit = new TokenBucket(30, 0.5)
  const registerSourceLimit = new TokenBucket(10, 0.1)
  const loginSourceKey = (c: Context): string => {
    if (!d.config.trustProxy) return "local"
    const fwd = c.req.header("x-forwarded-for")
    return (fwd ? fwd.split(",")[0].trim() : "") || "local"
  }

  // 会话 ID 格式白名单（多用户隔离防线）：`:id` 段必须为 32 位小写 hex，
  // 畸形/穿越形态一律 400。Hono 路由匹配前已整体 decodeURI，`%2F` 不可能进入单段，
  // 此处兜底 `..`/非 hex 等异常形态（与 WS/存储层同规则）。
  const validateSessionId = async (c: Context<AppEnv>, next: () => Promise<void>) => {
    const id = c.req.param("id") ?? ""
    if (!isValidSessionId(id)) return c.json({ error: `invalid session id: ${id}` }, 400)
    await next()
  }
  app.use("/api/v1/sessions/:id", validateSessionId)
  app.use("/api/v1/sessions/:id/*", validateSessionId)

  /** 多用户模式管理员校验：非管理员返回 403（auth=none 时默认用户即 admin，不拦截）。 */
  const requireAdmin = (c: Context): Response | null => {
    const user = userOf(c)
    if (d.config.auth === "server" && user.role !== "admin") {
      return c.json({ error: "admin only" }, 403)
    }
    return null
  }

  // Health
  app.get("/api/health", (c) => c.json({ ok: true, ts: Date.now() }))

  // 飞书 OAuth 回调：auth_user_authorize 生成授权链接（redirect_uri 指向本端点）→
  // 用户授权后飞书跳回 → 本端点兑换 user_access_token 并写回发起授权的会话目录
  // （token 文件与 feishu_docs 子 Agent 工具共用，会话内资源操作自动以用户身份生效）。
  // 公开端点（免鉴权）：state 即会话关联凭证（随机不可猜，兑换后一次性消费防重放）。
  app.get("/api/v1/oauth/feishu/callback", async (c) => {
    // HTML 转义（反射型 XSS 防护）：飞书用户显示名（昵称可含任意字符）与接口错误消息
    // 均为外部可控内容，直接插值进公开页面即可在同源执行脚本（窃取 localStorage 令牌）
    const esc = (s: string): string => s.replace(/[&<>"']/g, (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[ch] ?? ch)
    const page = (title: string, body: string): Response => {
      const html: string = `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><title>${title}</title><style>body{font-family:system-ui,sans-serif;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;background:#f5f5f7;color:#333}.card{text-align:center;background:#fff;padding:36px 48px;border-radius:12px;box-shadow:0 4px 20px rgba(0,0,0,.08);max-width:560px}.ok{font-size:44px}.title{font-size:20px;font-weight:600;margin:12px 0 8px}.detail{color:#666;font-size:14px;line-height:1.7;word-break:break-all}.err{color:#c0392b}</style></head><body><div class="card"><div class="ok">${title.includes("成功") ? "✅" : "❌"}</div><div class="title">${title}</div><div class="detail ${title.includes("成功") ? "" : "err"}">${body}</div><script>setTimeout(()=>location.href="/",1600)</script></div></body></html>`
      return c.html(html, 200, { "Cache-Control": "no-cache" })
    }
    const { code = "", state = "" } = c.req.query()
    const pending = getPendingAuth(state)
    if (!code || !pending) {
      return page(
        "飞书授权失败",
        "回调参数无效或授权已过期（state 不匹配/已消费）。请回到歌白会话，重新执行 auth_user_authorize 生成新授权链接后再试。",
      )
    }
    try {
      const json = await exchangeOAuthToken(feishuFetch, {
        clientId: pending.appId,
        clientSecret: pending.appSecret,
        grantType: "authorization_code",
        code,
        redirectUri: pending.redirectUri,
      })
      const entry = toUserTokenEntry(json)
      const info = await fetchFeishuUserInfo(feishuFetch, entry.accessToken)
      if (info) {
        entry.name = info.name
        entry.openId = info.openId
      }
      await defaultUserTokenStore.set({ home: d.config.gebaiHome, user: pending.user, sessionId: pending.sessionId }, entry)
      consumePendingAuth(state)
      d.events.publish({ type: "oauth.completed", sessionId: pending.sessionId, payload: { ok: true, user: entry.name ?? "", openId: entry.openId ?? "" }, timestamp: Date.now() })
      const who = entry.name ?? entry.openId ?? "未知用户"
      return page("飞书授权成功", `已绑定用户「${esc(who)}」，user_access_token 已保存到当前会话。<br>回到歌白会话即可继续操作（将自动以该用户身份执行）。`)
    } catch (err) {
      consumePendingAuth(state)
      return page("飞书授权失败", `${esc(String((err as Error).message || err))}<br>请回到歌白会话，重新执行 auth_user_authorize 后重试。`)
    }
  })

  // Auth
  app.post("/api/v1/auth/login", async (c) => {
    if (!loginGlobalLimit.allow("global") || !loginSourceLimit.allow(loginSourceKey(c))) {
      return c.json({ error: "rate limited: too many requests" }, 429)
    }
    const body = await c.req.json<{ username?: string; password?: string }>().catch(() => ({ username: "", password: "" }))
    const { username = "", password = "" } = body
    const token = await d.auth.login(username, password)
    if (!token) return c.json({ error: "invalid credentials" }, 401)
    return c.json({ token })
  })
  // 开放注册（仅服务模式）：注册用户恒为普通角色（admin 唯一入口是 GEBAI_ADMIN_PASSWORD_HASH，不可注册创建）。
  // open（默认）=注册即登录；approval=待 admin 审批（disabled+pending，不签发令牌）
  app.post("/api/v1/auth/register", async (c) => {
    if (d.config.auth !== "server") return c.json({ error: "not found" }, 404)
    // 注册同样走 scrypt（CPU DoS 同动机）且可无限制造用户条目：独立限流桶
    if (!registerGlobalLimit.allow("global") || !registerSourceLimit.allow(loginSourceKey(c))) {
      return c.json({ error: "rate limited: too many requests" }, 429)
    }
    const { username, password } = await c.req.json<{ username?: string; password?: string }>()
    try {
      const { user, token, pending } = await d.auth.register(String(username ?? ""), String(password ?? ""), d.config.signupMode)
      return c.json({ token, user: d.auth.strip(user), pending }, 201)
    } catch (err) {
      return c.json({ error: (err as Error).message }, 400)
    }
  })
  app.post("/api/v1/auth/logout", async (c) => {
    const auth = c.req.header("Authorization")
    if (auth?.startsWith("Bearer ")) await d.auth.logout(auth.slice(7))
    return c.json({ ok: true })
  })
  app.get("/api/v1/auth/me", async (c) => {
    const user = await userOf(c)
    return c.json(d.auth.strip(user))
  })

  // 外部身份兑换：同源部署网站把本地登录态换为歌白令牌（扩展点见 external-auth.ts）
  app.post("/api/v1/auth/exchange", async (c) => {
    if (!loginGlobalLimit.allow("global") || !loginSourceLimit.allow(loginSourceKey(c))) {
      return c.json({ error: "rate limited: too many requests" }, 429)
    }
    if (d.config.auth !== "server" || !d.externalAuth) return c.json({ error: "not found" }, 404)
    const { username, credential } = await c.req.json<{ username?: string; credential?: string }>()
    if (!username || !credential) return c.json({ error: "invalid request" }, 400)
    const token = await d.auth.exchangeExternal(username, d.externalAuth, credential, d.config.externalAuthAutocreate)
    if (!token) return c.json({ error: "invalid credentials" }, 401)
    return c.json({ token, user: d.auth.strip((await d.auth.authorize(token))!) })
  })

  // 外部身份扩展点探测（Web UI 启动时读取；不泄露密钥，仅暴露启用状态与前端需要的信息）
  app.get("/api/v1/auth/external-config", (c) => {
    if (d.config.auth !== "server" || !d.externalAuth) return c.json({ enabled: false })
    return c.json({ enabled: true, storageKey: d.config.externalAuthStorageKey ?? null, autocreate: d.config.externalAuthAutocreate })
  })

  // Users (admin; WS 侧 user.* 同权限，REST 与 WS 双通道一致——修复 REST 无管理员校验的越权）
  app.get("/api/v1/users", async (c) => {
    const denied = requireAdmin(c)
    if (denied) return denied
    const users = await d.auth.listUsers()
    return c.json(users.map((u) => d.auth.strip(u)))
  })
  app.post("/api/v1/users", async (c) => {
    const denied = requireAdmin(c)
    if (denied) return denied
    const { username, password, role } = await c.req.json<{ username: string; password: string; role?: "user" | "admin" }>()
    const user = await d.auth.createUser(username, password, role)
    return c.json(d.auth.strip(user), 201)
  })
  app.patch("/api/v1/users/:id", async (c) => {
    const denied = requireAdmin(c)
    if (denied) return denied
    const patch = await c.req.json()
    const user = await d.auth.updateUser(c.req.param("id"), patch)
    return c.json(d.auth.strip(user))
  })
  app.delete("/api/v1/users/:id", async (c) => {
    const denied = requireAdmin(c)
    if (denied) return denied
    await d.auth.deleteUser(c.req.param("id"))
    return c.json({ ok: true })
  })

  // Sessions
  app.get("/api/v1/sessions", async (c) => {
    const user = await userOf(c)
    const sessions = await d.store.listSessions(user.id)
    return c.json(sessions.map(toSessionInfo))
  })
  app.post("/api/v1/sessions", async (c) => {
    const user = await userOf(c)
    const body = (await c.req.json().catch(() => ({}))) as { name?: string }
    const session = await d.store.createSession(user.id, body.name)
    return c.json({ id: session.id, name: session.name, userId: session.userId, createdAt: session.createdAt, updatedAt: session.updatedAt }, 201)
  })
  app.get("/api/v1/sessions/:id", async (c) => {
    const user = await userOf(c)
    const session = await d.store.load(c.req.param("id"), user.id)
    if (!session) return c.json({ error: "not found" }, 404)
    return c.json(session)
  })
  app.delete("/api/v1/sessions/:id", async (c) => {
    const user = await userOf(c)
    await d.store.delete(c.req.param("id"), user.id)
    d.engine.forgetSession(c.req.param("id"))
    return c.json({ ok: true })
  })
  // 从 GC 归档（trash/，保留期 7 天）恢复会话：归属用户或 admin 可操作；
  // 恢复 = 目录整体移回分片存储位置（会话数据/tmp 附件/env 一并恢复）
  app.post("/api/v1/sessions/:id/restore", async (c) => {
    const user = await userOf(c)
    const id = c.req.param("id")
    const hit = await findInTrash(d.config.gebaiHome, id)
    // 归属不符与未找到同应答（不泄露他人会话存在性）
    if (!hit || (user.role !== "admin" && hit.owner !== user.id)) return c.json({ error: "not found" }, 404)
    const target = sessionPath(d.config.gebaiHome, hit.owner, id)
    if (existsSync(target)) return c.json({ error: "session already exists" }, 409)
    try {
      await rename(hit.trashDir, target)
    } catch (err) {
      return c.json({ error: `restore failed: ${String((err as Error).message || err)}` }, 500)
    }
    d.store.evict(id)
    return c.json({ ok: true })
  })
  app.patch("/api/v1/sessions/:id", async (c) => {
    const user = await userOf(c)
    const body = await c.req.json<{ name?: string }>()
    if (body.name) await d.store.rename(c.req.param("id"), body.name, user.id)
    return c.json({ ok: true })
  })

  // Env
  // 环境变量配置目录：前端面板白名单（全局静态组 + 各子Agent 导出 envVars 汇总组；不含启动级/安全敏感变量）。
  // 无敏感信息，local/server 模式均可用（服务模式需登录）。
  app.get("/api/v1/env/catalog", async (c) => {
    await userOf(c)
    return c.json({ groups: getEnvCatalog(d.subAgents.allDefs()) })
  })
  app.get("/api/v1/sessions/:id/env", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const envs: EnvVarSource[] = await d.env.describe(sessionId, user.id)
    return c.json(envs)
  })
  // 会话 env 写入（内存态，不落盘——用户环境变量只存浏览器本地，此处仅供运行中即时生效类开关使用；
  // 进程重启即空，前端每次加载会话自行重新同步）
  app.put("/api/v1/sessions/:id/env", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    let vars: unknown
    try {
      vars = await c.req.json()
    } catch {
      return c.json({ error: "请求体必须是 JSON 对象" }, 400)
    }
    // 名称合法性校验：仅允许标识符形式且拒绝 __proto__（原型污染，防注入/防污染会话 env）
    const err = validateEnvVars(vars)
    if (err) return c.json({ error: err }, 400)
    const env = await d.store.setEnv(sessionId, user.id, vars as Record<string, string | null>)
    // 敏感键脱敏返回（与 GET describe 同规则，防明文密钥回读）
    return c.json(maskEnv(env))
  })

  // Todos
  app.get("/api/v1/sessions/:id/todos", async (c) => {
    const user = await userOf(c)
    const todos: TodoItem[] = await d.store.getTodos(c.req.param("id"), user.id)
    return c.json(todos)
  })

  // Files
  app.get("/api/v1/sessions/:id/files", async (c) => {
    const user = await userOf(c)
    const files: FileEntry[] = await d.store.listSessionFiles(c.req.param("id"), user.id)
    return c.json(files)
  })
  app.get("/api/v1/sessions/:id/files/content", async (c) => {
    const user = await userOf(c)
    const path = c.req.query("path") || ""
    // 文件接口以会话 tmp/ 为根（DESIGN：文件操作严格限定在会话 tmp/ 内），兼容 tmp/ 前缀路径
    const safe = d.store.resolveSessionTmpFile(c.req.param("id"), user.id, path, d.sandbox.enforcedFor(user.id))
    // 原始字节流式返回（Bun.file 自动按扩展名设置 Content-Type，如 image/png）：
    // 图片等二进制经 text() 会被 UTF-8 解码损坏，前端 <img> 将无法解码显示
    return new Response(Bun.file(safe))
  })
  app.get("/api/v1/sessions/:id/files/download", async (c) => {
    const user = await userOf(c)
    const path = c.req.query("path") || ""
    const safe = d.store.resolveSessionTmpFile(c.req.param("id"), user.id, path, d.sandbox.enforcedFor(user.id))
    const file = Bun.file(safe)
    return new Response(file.stream(), { headers: { "Content-Disposition": `attachment; filename="${encodeURIComponent(path)}"` } })
  })
  // 多选打包下载：POST body 指定 paths 列表，返回 zip（DESIGN REST 协议表）
  app.post("/api/v1/sessions/:id/files/download", async (c) => {
    const user = await userOf(c)
    const body = (await c.req.json().catch(() => ({}))) as { paths?: string[] }
    const files: Array<{ name: string; data: Uint8Array }> = []
    for (const p of body.paths ?? []) {
      const safe = d.store.resolveSessionTmpFile(c.req.param("id"), user.id, p, d.sandbox.enforcedFor(user.id))
      if (!existsSync(safe)) return c.json({ error: `file not found: ${p}` }, 404)
      const buf = await Bun.file(safe).arrayBuffer()
      files.push({ name: p, data: new Uint8Array(buf) })
    }
    const zip = buildZip(files)
    return new Response(zip, {
      headers: { "Content-Type": "application/zip", "Content-Disposition": 'attachment; filename="files.zip"' },
    })
  })

  // 文件预览（DESIGN「文件链接弹窗查看」）：read/write/edit/patch 等文件工具卡片链接的取数入口——
  // 会话相对路径以 tmp/ 为根；绝对路径（code 项目文件）按用户隔离边界放行（沙箱用户限本用户数据目录内）。
  // ?download=1 时以附件形式返回（文件卡工具栏下载对项目文件同样经此入口）。
  app.get("/api/v1/sessions/:id/files/preview", async (c) => {
    const user = await userOf(c)
    const path = c.req.query("path") || ""
    let safe: string
    try {
      safe = d.store.resolvePreviewFile(c.req.param("id"), user.id, path, d.sandbox.enforcedFor(user.id))
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 403)
    }
    let isFile = false
    try {
      isFile = statSync(safe).isFile()
    } catch {
      return c.json({ error: `file not found: ${path}` }, 404)
    }
    if (!isFile) return c.json({ error: `not a file: ${path}` }, 404)
    // Office 阅读视图（wps 文档预览，DESIGN「文件链接弹窗查看」）：docx/xlsx/xlsm/pptx 按本参数返回
    // 结构化 HTML（前端文件卡/弹窗 iframe 渲染）；渲染器惰性引入（exceljs/docx 解析较重，不拖启动），
    // 解析单一真相源在 wps 子Agent。非法/损坏文件 422（前端回退二进制占位与下载引导）。
    if (c.req.query("render") === "office") {
      const { renderOfficeReadingView } = await import("./sub-agents/wps/preview")
      try {
        const html = await renderOfficeReadingView(safe)
        return new Response(html, { headers: { "Content-Type": "text/html; charset=utf-8" } })
      } catch (err) {
        return c.json({ error: `office 阅读视图渲染失败: ${(err as Error).message}` }, 422)
      }
    }
    const headers: Record<string, string> = {}
    if (c.req.query("download") === "1") headers["Content-Disposition"] = `attachment; filename="${encodeURIComponent(path.replace(/\\/g, "/").split("/").pop() || "file")}"`
    return new Response(Bun.file(safe), { headers })
  })

  // Tools
  app.get("/api/v1/tools", async (c) => {
    return c.json(
      d.registry.list(false).map((rt) => ({
        name: rt.name,
        description: rt.tool.description,
        enabled: rt.enabled,
        group: rt.agent || "global",
        approvalRequired: !!rt.tool.requiresApproval,
      })),
    )
  })
  app.patch("/api/v1/tools", async (c) => {
    // 工具启停为服务端全局状态（所有用户/会话共享）：多用户模式下仅管理员可操作
    const denied = requireAdmin(c)
    if (denied) return denied
    const { name, enabled } = await c.req.json<{ name: string; enabled: boolean }>()
    d.registry.setEnabled(name, enabled)
    return c.json({ ok: true })
  })

  // Sub-agents
  app.get("/api/v1/sub-agents", async (c) => c.json(d.subAgents.list()))

  // 定时任务（用户级资源，DESIGN「定时任务」）：REST 管理面（第三方集成/脚本管理），与 cron_* 工具同源同权
  // （写操作不经审批——REST 已有身份认证边界，与 sessions/env 等既有资源管理端点姿态一致）。
  // 任务 id 格式白名单（32 位 hex，与生成规则一致）：畸形/穿越形态 400。
  const validateCronId = async (c: Context<AppEnv>, next: () => Promise<void>) => {
    const id = c.req.param("id") ?? ""
    if (!/^[a-f0-9]{32}$/.test(id)) return c.json({ error: `invalid cron task id: ${id}` }, 400)
    await next()
  }
  app.use("/api/v1/cron/:id", validateCronId)
  app.use("/api/v1/cron/:id/*", validateCronId)
  app.get("/api/v1/cron", async (c) => {
    if (!d.cron) return c.json({ error: "cron disabled (GEBAI_CRON_ENABLED=false)" }, 503)
    const user = await userOf(c)
    return c.json(await d.cron.list(user.id))
  })
  app.post("/api/v1/cron", async (c) => {
    if (!d.cron) return c.json({ error: "cron disabled (GEBAI_CRON_ENABLED=false)" }, 503)
    const user = await userOf(c)
    try {
      const body = await c.req.json()
      const task = await d.cron.add(user.id, body, typeof body?.originSessionId === "string" && isValidSessionId(body.originSessionId) ? body.originSessionId : undefined)
      return c.json(task, 201)
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })
  app.patch("/api/v1/cron/:id", async (c) => {
    if (!d.cron) return c.json({ error: "cron disabled (GEBAI_CRON_ENABLED=false)" }, 503)
    const user = await userOf(c)
    try {
      const body = await c.req.json()
      const task = await d.cron.update(user.id, c.req.param("id"), body)
      if (!task) return c.json({ error: "not found" }, 404)
      return c.json(task)
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })
  app.delete("/api/v1/cron/:id", async (c) => {
    if (!d.cron) return c.json({ error: "cron disabled (GEBAI_CRON_ENABLED=false)" }, 503)
    const user = await userOf(c)
    const removed = await d.cron.remove(user.id, c.req.param("id"))
    return removed ? c.json({ ok: true }) : c.json({ error: "not found" }, 404)
  })
  app.post("/api/v1/cron/:id/run", async (c) => {
    if (!d.cron) return c.json({ error: "cron disabled (GEBAI_CRON_ENABLED=false)" }, 503)
    const user = await userOf(c)
    const task = await d.cron.trigger(user.id, c.req.param("id"))
    if (!task) return c.json({ error: "not found" }, 404)
    return c.json(task)
  })

  // Attachments
  app.post("/api/v1/sessions/:id/truncate", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const s = await d.store.load(sessionId, user.id)
    if (!s) return c.json({ error: "not found" }, 404)
    // 运行中任务持有自己的上下文快照并继续追加消息，中途截断会产生交错历史：拒绝，先停止或等任务完成
    if (d.engine.isRunning(sessionId)) return c.json({ error: "task already running" }, 409)
    const body = await c.req.json<{ before: string }>()
    if (!body.before) return c.json({ error: "before required" }, 400)
    await d.store.truncateMessages(sessionId, user.id, body.before)
    return c.json({ ok: true })
  })
  app.post("/api/v1/sessions/:id/attachments", async (c) => {
    const user = await userOf(c)
    const body = await c.req.formData()
    const file = body.get("file")
    if (!file) return c.json({ error: "no file" }, 400)
    const tmp = d.store.getTmpDir(c.req.param("id"), user.id)
    // 文件名消毒：仅 basename，拒绝路径穿越
    const rawName = typeof file === "object" && "name" in file ? (file as File).name : "upload"
    const name = basenameName(rawName) || "upload"
    const buf = await (file as Blob).arrayBuffer()
    await Bun.write(join(tmp, name), new Uint8Array(buf))
    return c.json({ id: name, name, mime: (file as Blob).type, size: buf.byteLength, path: `tmp/${name}` })
  })

  // Cancel / Approval / Compact
  app.post("/api/v1/sessions/:id/cancel", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const s = await d.store.load(sessionId, user.id)
    if (!s) return c.json({ error: "not found" }, 404)
    d.engine.cancel(sessionId)
    return c.json({ ok: true })
  })
  app.post("/api/v1/sessions/:id/approval", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const s = await d.store.load(sessionId, user.id)
    if (!s) return c.json({ error: "not found" }, 404)
    const { toolCallId, approve } = await c.req.json<{ toolCallId: string; approve: boolean }>()
    await d.engine.decideApproval(sessionId, toolCallId, approve)
    return c.json({ ok: true })
  })
  // 选择决策（ask 选项询问分支等待的用户选择）；option 单选 / options 数组多选 / refuse=true 拒绝回答
  app.post("/api/v1/sessions/:id/choice", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const s = await d.store.load(sessionId, user.id)
    if (!s) return c.json({ error: "not found" }, 404)
    const { choiceId, option, options, refuse } = await c.req.json<{ choiceId: string; option?: string; options?: string[]; refuse?: boolean }>()
    const multi = Array.isArray(options)
    if (multi && !options.length) return c.json({ error: "options must not be empty" }, 400)
    if (option == null && !multi && refuse !== true) return c.json({ error: "option, options or refuse required" }, 400)
    await d.engine.decideChoice(sessionId, choiceId, refuse === true ? null : multi ? options!.map(String) : String(option))
    return c.json({ ok: true })
  })
  // 画图渲染结果回传（show 图表分支等待的前端渲染结果）
  app.post("/api/v1/sessions/:id/draw", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const s = await d.store.load(sessionId, user.id)
    if (!s) return c.json({ error: "not found" }, 404)
    const { renderId, ok, error } = await c.req.json<{ renderId: string; ok: boolean; error?: string }>()
    await d.engine.decideDrawResult(sessionId, renderId, { ok, error })
    return c.json({ ok: true })
  })
  app.post("/api/v1/sessions/:id/compact", async (c) => {
    const user = await userOf(c)
    const body = (await c.req.json().catch(() => ({}))) as { scope?: "all" | { from: number; to: number } }
    const result = await d.engine.compactSession(c.req.param("id"), user.id, body.scope)
    return c.json(result)
  })

  // Feedback
  app.post("/api/v1/feedback", async (c) => {
    const user = await userOf(c)
    const fb = await c.req.json<FeedbackInput>()
    const id = await writeFeedback(d.config.gebaiHome, user.id, fb)
    return c.json({ ok: true, id })
  })
  app.get("/api/v1/feedback", async (c) => {
    const user = await userOf(c)
    // 管理员可查询全部用户反馈；普通用户仅自己的（DESIGN REST 协议表：查询/导出）
    let list: FeedbackInfo[] = []
    if (user.role === "admin") {
      const users = await d.auth.listUsers()
      // auth=none 的默认用户（default）同样纳入扫描
      const targets = [...users, d.auth.defaultUser(), SERVICE_USER]
      const seen = new Set<string>()
      for (const u of targets) {
        if (seen.has(u.id)) continue
        seen.add(u.id)
        list = list.concat(await readFeedback(d.config.gebaiHome, u.id))
      }
    } else {
      list = await readFeedback(d.config.gebaiHome, user.id)
    }
    const q = c.req.query()
    if (q.messageId) list = list.filter((f) => f.messageId === q.messageId)
    if (q.sessionId) list = list.filter((f) => f.sessionId === q.sessionId)
    if (q.type) list = list.filter((f) => f.type === q.type)
    return c.json(list)
  })

  // Webhooks（DESIGN REST 协议表；签名校验与重试见 webhooks.ts）
  app.get("/api/v1/webhooks", async (c) => {
    const user = await userOf(c)
    return c.json(d.webhooks.list(user.role === "admin" ? undefined : user.id))
  })
  app.post("/api/v1/webhooks", async (c) => {
    const user = await userOf(c)
    const body = await c.req.json<{ url: string; events?: string[]; secret?: string }>()
    try {
      const cfg = await d.webhooks.register(body, user.role === "admin" ? undefined : user.id)
      return c.json(cfg, 201)
    } catch (e) {
      return c.json({ error: (e as Error).message }, 400)
    }
  })
  app.delete("/api/v1/webhooks/:id", async (c) => {
    const user = await userOf(c)
    const mine = d.webhooks.list(user.role === "admin" ? undefined : user.id)
    if (!mine.some((w) => w.id === c.req.param("id"))) return c.json({ error: "not found" }, 404)
    await d.webhooks.remove(c.req.param("id"))
    return c.json({ ok: true })
  })

  // HTML 小工具库（Agent 经 save_tool 保存；列表/读取/删除供 UI 弹窗加载）
  app.get("/api/v1/mini-tools", async (c) => {
    const user = await userOf(c)
    const tools = await listMiniTools(d.config.gebaiHome, user.id)
    return c.json(tools)
  })
  app.get("/api/v1/mini-tools/:name", async (c) => {
    const user = await userOf(c)
    const tool = await getMiniTool(d.config.gebaiHome, user.id, c.req.param("name"))
    if (!tool) return c.json({ error: "tool not found" }, 404)
    return c.json(tool)
  })
  app.delete("/api/v1/mini-tools/:name", async (c) => {
    const user = await userOf(c)
    const scope = c.req.query("scope") === "public" ? "public" : "private"
    // 多用户模式公共工具仅管理员可删（与 save_tool/delete_tool 同规则，防共享资源投毒/破坏）
    const removed = await deleteMiniTool(d.config.gebaiHome, user.id, c.req.param("name"), scope, { mode: d.config.auth, role: user.role })
    if (!removed) return c.json({ error: "tool not found" }, 404)
    return c.json({ ok: true })
  })

  // Prompt (SSE)
  app.post("/api/v1/sessions/:id/prompt", async (c) => {
    const user = await userOf(c)
    // 每用户消息速率限制（防高频 prompt 消耗 LLM 配额/资源）
    if (!promptRateLimit.allow(user.id)) return c.json({ error: "rate limited: too many requests" }, 429)
    const sessionId = c.req.param("id")
    const body = await c.req.json<{ prompt: string; attachments?: Array<{ name: string; mime?: string; path?: string; data?: number[] }>; env?: Record<string, string | null>; messageId?: string; interactionMode?: string; stream?: boolean }>()
    if (d.engine.isRunning(sessionId)) return c.json({ error: "task already running" }, 409)
    // 请求层交互模式与输出方式配置（服务端全部支持，接入方按需选择）：
    // interactionMode 默认 none（无交互）；stream=false（默认）仅最终响应，true 流式输出（经 WS 事件订阅消费）
    const interactionMode = body.interactionMode ?? "none"
    if (!["none", "multi_turn", "realtime"].includes(interactionMode)) return c.json({ error: `interactionMode 非法: ${interactionMode}（可选 none/multi_turn/realtime）` }, 400)

    const attachments: AttachmentInput[] | undefined = body.attachments?.map((a) => ({
      name: a.name,
      mime: a.mime,
      path: a.path,
      data: a.data ? new Uint8Array(a.data) : undefined,
    }))
    // 浏览器本地环境变量注入（与 WS prompt 同规则）：不支持/非法的变量直接跳过（宽容过滤，
    // 防 localStorage 残留旧版目录外键阻断整个任务），其余随任务临时生效、不持久化
    const envOverride: Record<string, string> | undefined = body.env
      ? filterEnvInjection(body.env)
      : undefined

    // 同步等待任务完成，返回最终 assistant 消息；任务错误（LLM 失败等）经 event.task.error 捕获返回。
    // 交互模式与输出方式均为请求层配置（服务端全部支持）：默认无交互 + 仅最终响应——
    // 依赖前端/多轮交互的工具（声明 interaction 高于 none）由引擎自动禁用，需审批工具自动通过（无人可询问）；
    // stream=true 时推送流式事件（event.message.delta 等，接入方经 WS 事件订阅消费）；需要完整交互能力请使用 WS 通道（/ws）。
    let taskError: string | null = null
    const unsub = d.events.subscribe((ev) => {
      if (ev.sessionId !== sessionId || ev.type !== "event.task.error") return
      taskError = String(ev.payload.error ?? "unknown error")
    })
    try {
      await d.engine.run(sessionId, user.id, body.prompt, { attachments, envOverride, messageId: body.messageId, interactionMode: interactionMode as "none" | "multi_turn" | "realtime", outputMode: body.stream === true ? "streaming" : "final_only", role: user.role })
    } catch (e) {
      // 会话不存在/任务冲突等引擎级错误：与任务错误同一返回形态（200 + error 字段）
      return c.json({ error: String((e as Error).message || e) })
    } finally {
      unsub()
    }
    const session = await d.store.load(sessionId, user.id)
    const last = session ? [...session.messages].reverse().find((m) => m.role === "assistant") : undefined
    return c.json({
      message: last
        ? { id: last.id, content: typeof last.content === "string" ? last.content : "", createdAt: last.createdAt }
        : null,
      ...(taskError ? { error: taskError } : {}),
    })
  })

  // OpenAPI 文档（DESIGN REST 协议表：/api/docs）
  app.get("/api/docs", (c) =>
    c.json({
      openapi: "3.0.3",
      info: { title: "歌白智能体 API", version: "1.0.0", description: "歌白智能体 REST API（WebSocket 实时通道见 /ws）" },
      paths: {
        "/api/health": { get: { summary: "健康检查", responses: { "200": { description: "ok" } } } },
        "/api/v1/auth/login": { post: { summary: "登录（多用户模式）", responses: { "200": { description: "token" } } } },
        "/api/v1/auth/logout": { post: { summary: "登出", responses: { "200": { description: "ok" } } } },
        "/api/v1/auth/exchange": { post: { summary: "外部身份兑换令牌（同源集成扩展点）", responses: { "200": { description: "token+user" } } } },
        "/api/v1/auth/external-config": { get: { summary: "外部身份扩展点探测", responses: { "200": { description: "enabled/storageKey/autocreate" } } } },
        "/api/v1/users": { get: { summary: "用户列表（管理员）" }, post: { summary: "创建用户（管理员）" } },
        "/api/v1/sessions": { get: { summary: "会话列表" }, post: { summary: "创建会话" } },
        "/api/v1/sessions/{id}": { get: { summary: "会话详情" }, delete: { summary: "删除会话" }, patch: { summary: "重命名会话" } },
        "/api/v1/sessions/{id}/restore": { post: { summary: "从回收站恢复会话（GC 归档保留期内）" } },
        "/api/v1/sessions/{id}/prompt": { post: { summary: "发送消息（SSE 流）" } },
        "/api/v1/sessions/{id}/attachments": { post: { summary: "上传附件（multipart）" } },
        "/api/v1/sessions/{id}/cancel": { post: { summary: "取消任务" } },
        "/api/v1/sessions/{id}/approval": { post: { summary: "审批决策" } },
        "/api/v1/sessions/{id}/choice": { post: { summary: "选择决策（ask 选项询问分支）" } },
        "/api/v1/sessions/{id}/draw": { post: { summary: "画图渲染结果回传（show 图表分支）" } },
        "/api/v1/sessions/{id}/compact": { post: { summary: "主动压缩上下文" } },
        "/api/v1/sessions/{id}/truncate": { post: { summary: "截断会话消息" } },
        "/api/v1/sessions/{id}/env": { get: { summary: "会话环境变量" }, put: { summary: "设置会话环境变量" } },
        "/api/v1/sessions/{id}/todos": { get: { summary: "会话待办清单" } },
        "/api/v1/sessions/{id}/files": { get: { summary: "会话临时文件列表" } },
        "/api/v1/sessions/{id}/files/content": { get: { summary: "读取文件内容" } },
        "/api/v1/sessions/{id}/files/download": { get: { summary: "下载单文件" }, post: { summary: "多选打包下载（zip）" } },
        "/api/v1/sessions/{id}/files/preview": { get: { summary: "文件预览（会话相对/项目绝对路径，点击弹窗查看用；?render=office 返回 docx/xlsx/xlsm/pptx 阅读视图 HTML）" } },
        "/api/v1/tools": { get: { summary: "工具集查询" }, patch: { summary: "工具启停" } },
        "/api/v1/sub-agents": { get: { summary: "子Agent 能力列表" } },
        "/api/v1/feedback": { get: { summary: "反馈查询（管理员可全部）" }, post: { summary: "提交反馈" } },
        "/api/v1/webhooks": { get: { summary: "Webhook 列表" }, post: { summary: "注册 Webhook" } },
        "/api/v1/webhooks/{id}": { delete: { summary: "删除 Webhook" } },
        "/api/v1/mini-tools": { get: { summary: "HTML 小工具列表（公用 + 本人私有）" } },
        "/api/v1/mini-tools/{name}": { get: { summary: "读取 HTML 小工具（含源码）" }, delete: { summary: "删除 HTML 小工具（?scope=private|public）" } },
      },
    }),
  )

  // Static Web UI (single-port exposure). Only registered if the build exists.
  // dev-reload 模式下即使 dist 刚被 clean-dist 清空（首轮构建窗口期）也注册，
  // 由 `/` 路由在 index.html 暂缺时返回占位页，避免服务崩溃或 UI 整体缺失。
  const embedded = d.config.binaryMode ? embeddedWebAssets() : null
  if (existsSync(d.config.webDist) || embedded || d.config.devReload) {
    // 注入全局默认 UI 风格（GEBAI_UI_STYLE），前端按 会话/URL > 用户 > 全局 优先级解析
    const UI_STYLES = ["acrylic", "aether", "cyberpunk", "aurora", "synthwave", "matrix", "tokyo-night", "ink", "cny"]
    const style = UI_STYLES.includes(d.config.uiStyle) ? d.config.uiStyle : "acrylic"
    let cachedHtml: string | null = null
    app.get("/", (c) => {
      // dev-reload 模式下每次请求重读 dist/index.html：vite build --watch 每次重建产出新 hash
      // 资源，若缓存启动时的旧 HTML，页面刷新后仍加载旧资源（改动永不生效）；生产/二进制模式缓存即可
      if (d.config.devReload || !cachedHtml) {
        let raw: string
        try {
          raw = embedded
            ? new TextDecoder().decode(embedded.get("/index.html") ?? new Uint8Array())
            : readFileSync(join(d.config.webDist, "index.html"), "utf8")
        } catch {
          // 构建窗口期 index.html 暂缺：返回占位页（构建完成后自动刷新），不抛异常崩溃服务
          return c.html(buildPlaceholderHtml(d.config.basePath), 503, { "Cache-Control": "no-cache" })
        }
        let injected = `<script>window.__GEBAI_UI_STYLE__=${JSON.stringify(style)}</script>`
        // 开发模式热刷新（--reload）：监听 /__gebai_hot，收到 reload 或连接断开（服务端重启）即刷新页面
        if (d.config.devReload) {
          const hotPath = `${d.config.basePath === "/" ? "" : d.config.basePath}/__gebai_hot`
          const client = `(()=>{let ws;const go=()=>{ws=new WebSocket((location.protocol==="https:"?"wss":"ws")+"://"+location.host+${JSON.stringify(hotPath)});ws.onmessage=e=>{try{if(JSON.parse(e.data).type==="reload")location.reload()}catch{}};ws.onclose=()=>setTimeout(()=>location.reload(),400)};go()})()`
          injected += `<script>${client}</script>`
        }
        cachedHtml = raw.replace("</head>", `${injected}</head>`)
      }
      return c.html(cachedHtml, 200, { "Cache-Control": "no-cache" })
    })
    if (embedded) {
      // 二进制模式：内嵌资源直接提供，无需磁盘
      app.use("*", async (c) => {
        const buf = embedded.get(c.req.path)
        if (!buf) return c.notFound()
        const ext = c.req.path.split(".").pop() ?? ""
        const mime =
          ext === "js"
            ? "text/javascript"
            : ext === "css"
              ? "text/css"
              : ext === "html"
                ? "text/html"
                : ext === "woff2"
                  ? "font/woff2"
                  : ext === "svg"
                    ? "image/svg+xml"
                    : "application/octet-stream"
        return new Response(buf, { status: 200, headers: { "Content-Type": mime } })
      })
    } else {
      app.use("*", serveStatic({ root: d.config.webDist }))
    }
  }

  return app
}

