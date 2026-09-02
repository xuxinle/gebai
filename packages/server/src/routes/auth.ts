/** 认证与身份域路由：/auth/*（登录/注册/登出/me/外部身份兑换与探测）+ 飞书 OAuth 回调。 */
import type { RouteCtx } from "./context"
import { TokenBucket } from "../core/security/ratelimit"
import { consumePendingAuth, defaultUserTokenStore, exchangeOAuthToken, fetchFeishuUserInfo, getPendingAuth, toUserTokenEntry } from "../sub-agents/feishu_docs/oauth"
import { feishuFetch } from "../feishu-bot/tls"

export function registerAuthRoutes(rc: RouteCtx): void {
  const { app, d } = rc

  /** 登录/兑换端点限流（防 CPU DoS：scrypt 即使异步化仍耗 CPU，轮换用户名即可绕过按用户名锁定）：
   *  全局桶兜底总量，来源桶按客户端标识（GEBAI_TRUST_PROXY=true 时取 X-Forwarded-For 首段，否则共桶）。 */
  const loginGlobalLimit = new TokenBucket(60, 2)
  const loginSourceLimit = new TokenBucket(10, 0.2)
  /** 注册独立桶（scrypt 同动机；不复用登录小来源桶——正常「登录多次+注册一次」的用量不互相挤占）。 */
  const registerGlobalLimit = new TokenBucket(30, 0.5)
  const registerSourceLimit = new TokenBucket(10, 0.1)
  const loginSourceKey = (c: { req: { header: (h: string) => string | undefined } }): string => {
    if (!d.config.trustProxy) return "local"
    const fwd = c.req.header("x-forwarded-for")
    return (fwd ? fwd.split(",")[0].trim() : "") || "local"
  }

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
    const user = await rc.userOf(c)
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
}
