/**
 * 拟人模式的凭据路由：状态查询 / 发起授权 / 回调兑换 / 撤销。
 *
 * ## 为什么需要独立的回调端点
 *
 * 既有的 `/api/v1/oauth/feishu/callback` 是**会话级**的（把 token 写回发起授权的那个
 * 对话目录）。拟人模式的凭据是**进程级**的（人格跨会话常驻），两处的归属不同，
 * 共用一个端点会让「令牌到底写给了谁」变得模糊。
 *
 * ## 为什么状态查询要能自动生成授权链接
 *
 * 用户的心智是「我点一下就能修好」。若状态接口只回 `needsAuth: true` 而不给链接，
 * 前端就得再发一次请求——多一次往返、还多一处状态（链接什么时候生成）。
 * 让状态接口惰性生成链接，前端拿到即可直接渲染按钮。
 *
 * ## 安全
 *
 * - 状态接口**不返回任何令牌明文**（只给「授权人是谁、何时过期」）
 * - 回调是公开端点（免鉴权）：`state` 即一次性凭证（随机不可猜，兑换后作废）
 * - 回调返回 HTML 页面（浏览器跳转场景），不做 JSON——用户看到的是网页
 */

import type { Context, Hono } from "hono"
import type { AppEnv } from "../app"
import type { PersonaCredentials } from "@gebai/persona"

export interface PersonaRouteDeps {
  /** 凭据编排（拟人模式未启用时为 null → 全部端点返回「未启用」）。 */
  credentials: PersonaCredentials | null
  /** 事件总线（授权完成/失败通知前端刷新状态）。 */
  publish: (type: string, payload: Record<string, unknown>) => void
  /** 用于拼默认回调地址（`GEBAI_PUBLIC_URL` → `http://localhost:{GEBAI_PORT}`）。 */
  publicBase: () => string
}

/** HTML 转义（反射型 XSS 防护：错误消息可能含外部可控内容）。 */
function esc(s: string): string {
  return s.replace(/[&<>"']/g, (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[ch] ?? ch)
}

/** 结果页（浏览器跳转场景：用户看到的是网页，不是 JSON）。 */
function page(title: string, body: string, ok: boolean): Response {
  const html = `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><title>${esc(title)}</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;background:#f5f5f7;color:#333}
.card{text-align:center;background:#fff;padding:36px 48px;border-radius:12px;box-shadow:0 4px 20px rgba(0,0,0,.08);max-width:560px}
.icon{font-size:44px}.title{font-size:20px;font-weight:600;margin:12px 0 8px}
.detail{color:#666;font-size:14px;line-height:1.7;word-break:break-all}
.err{color:#c0392b}
</style></head><body><div class="card"><div class="icon">${ok ? "✅" : "❌"}</div>
<div class="title">${esc(title)}</div><div class="detail ${ok ? "" : "err"}">${body}</div>
<script>setTimeout(function(){location.href="/"},1800)</script></div></body></html>`
  return new Response(html, { status: 200, headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-cache" } })
}

export function registerPersonaRoutes(app: Hono<AppEnv>, deps: PersonaRouteDeps): void {
  const notEnabled = (c: Context) => c.json({ error: "拟人模式未启用（GEBAI_PERSONA=true 后可用）" }, 404)

  /**
   * 当前凭据状态（前端横幅据此决定是否提示授权）。
   *
   * 返回里的 `authorizeUrl` 是**惰性生成**的：只有在需要授权时才有值。
   */
  app.get("/api/v1/persona/status", async (c) => {
    if (!deps.credentials) return notEnabled(c)
    const status = await deps.credentials.status()
    return c.json(status)
  })

  /** 重新发起授权（生成新链接；旧链接随之失效）。 */
  app.post("/api/v1/persona/authorize", async (c) => {
    if (!deps.credentials) return notEnabled(c)
    try {
      const ticket = deps.credentials.startAuthorize()
      return c.json({ ok: true, url: ticket.url, scopes: ticket.scopes, expiresInMs: ticket.expiresInMs })
    } catch (err) {
      return c.json({ ok: false, error: String((err as Error)?.message || err) }, 400)
    }
  })

  /**
   * 手动提交授权码（回调打不到歌白时的兜底）。
   *
   * 真实场景：用户配的 redirect_uri 指向别处（如 `http://localhost` 无端口），
   * 浏览器的跳转落到别的服务上——此时用户可以把地址栏里带 code 的 URL 粘进来。
   */
  app.post("/api/v1/persona/authorize/complete", async (c) => {
    if (!deps.credentials) return notEnabled(c)
    const body = await c.req.json<{ code?: string; state?: string }>().catch(() => ({}) as { code?: string; state?: string })
    const code = String(body.code ?? "").trim()
    if (!code) return c.json({ ok: false, error: "缺少 code（粘贴授权后的完整回调地址或 code）" }, 400)
    try {
      const status = await deps.credentials.complete(code, body.state)
      deps.publish("event.persona.credentials", { ready: status.ready, state: status.state })
      return c.json({ ok: true, status })
    } catch (err) {
      const message = String((err as Error)?.message || err)
      deps.publish("event.persona.credentials", { ready: false, state: "error", message })
      return c.json({ ok: false, error: message }, 400)
    }
  })

  /** 撤销授权（清除落盘令牌，人格将停止对外动作并重新提示授权）。 */
  app.delete("/api/v1/persona/authorize", async (c) => {
    if (!deps.credentials) return notEnabled(c)
    await deps.credentials.revoke()
    deps.publish("event.persona.credentials", { ready: false, state: "missing" })
    return c.json({ ok: true })
  })

  /**
   * 飞书授权回调（**公开端点**，免鉴权）。
   *
   * `state` 即会话关联凭证（随机、一次性、10 分钟 TTL）：兑换成功/失败后都作废，
   * 防重放。没有 state 也要能兑换（用户可能手动开了只带 code 的链接），
   * 此时用最近一次 pending 的 redirect_uri——否则飞书会因 redirect_uri 不匹配拒绝。
   */
  app.get("/api/v1/persona/feishu/callback", async (c) => {
    if (!deps.credentials) return page("拟人模式未启用", "请先设置 GEBAI_PERSONA=true 并重启歌白。", false)
    const { code = "", state = "", error = "", error_description = "" } = c.req.query()
    if (error) {
      return page("飞书授权失败", `飞书返回错误：${esc(String(error_description || error))}`, false)
    }
    if (!code) {
      return page("授权参数缺失", "回调地址里没有 code。请回到歌白重新发起授权，或把完整回调地址粘贴到设置页。", false)
    }
    try {
      const status = await deps.credentials.complete(String(code), state ? String(state) : undefined)
      deps.publish("event.persona.credentials", { ready: status.ready, state: status.state, user: status.authorizedUser ?? "" })
      return page(
        "飞书授权成功",
        `人格已获得飞书访问权限${status.authorizedUser ? `（${esc(status.authorizedUser)}）` : ""}，可以开始处理消息了。`,
        true,
      )
    } catch (err) {
      const message = String((err as Error)?.message || err)
      deps.publish("event.persona.credentials", { ready: false, state: "error", message })
      return page("飞书授权失败", esc(message), false)
    }
  })

  /** 默认回调地址（供前端展示「需要在飞书后台登记哪个地址」）。 */
  app.get("/api/v1/persona/callback-url", (c) => {
    if (!deps.credentials) return notEnabled(c)
    const url = `${deps.publicBase()}/api/v1/persona/feishu/callback`
    return c.json({ url })
  })
}
