/** WS 认证与连接域消息：auth.login/logout、ping。 */
import type { WsHandler } from "./context"
import type { AuthUser } from "../auth"
import { SERVICE_USER } from "../app"
import { TokenBucket } from "../core/security/ratelimit"

/** WS auth.login 密码路径限流（防 scrypt CPU DoS，与 REST login 同规则；令牌路径为廉价查表不限）。 */
const wsLoginRateLimit = new TokenBucket(30, 1)

export const authHandlers: Record<string, WsHandler> = {
  "auth.login": async ({ d, p, conn, ws, reply }) => {
    // 登录（多用户模式）：密码登录 / 已有令牌 / 服务 API Key 三种方式。
    // 令牌方式供 SDK 重连后自动重新认证（WS 连接无 Header，重连即恢复用户上下文）。
    let u: AuthUser | null = null
    let tok: string | undefined
    if (p.token != null) {
      tok = String(p.token)
      u = await d.auth.authorize(tok)
    } else {
      // 密码路径限流：轮换用户名可绕过按用户名锁定，桶级兜底总量（与 REST login 同动机）
      if (d.config.auth === "server" && !wsLoginRateLimit.allow("ws-login")) return reply(false, undefined, "rate limited: too many attempts")
      tok = (await d.auth.login(String(p.username), String(p.password))) ?? undefined
      if (tok) u = await d.auth.authorize(tok)
    }
    if (!u) return reply(false, undefined, "invalid credentials")
    conn.set(u)
    const replyPayload: Record<string, unknown> = { user: d.auth.strip(u) }
    if (tok) replyPayload.token = tok
    if (d.state) replyPayload.snapshot = await d.state.buildSnapshot(conn)
    reply(true, replyPayload)
    // 多用户模式登录成功推送状态快照（客户端模型基线）；单用户模式已在建连时推送
    if (d.state && d.config.auth === "server") void d.state.pushSnapshot(ws, conn).catch(() => {})
  },
  "auth.logout": ({ conn, d, ws, reply }) => {
    // 撤销服务端令牌走 REST（需 token）；WS 侧清空连接用户态（回到未登录）
    conn.set(SERVICE_USER)
    conn.setCurrent(undefined)
    reply(true)
    if (d.state && d.config.auth === "server") void d.state.pushSnapshot(ws, conn).catch(() => {})
  },
  ping: ({ reply }) => {
    // 心跳应答：客户端周期发送保活穿越代理（防止按闲置时间断连），回 pong
    return reply(true, { pong: true })
  },
}
