/** WS 消息分发器：入口守卫（未登录拦截/会话 id 白名单）+ 分发表查找 + 异常兜底。
 *  各消息类型的处理器按域拆分在 ws-handlers/（契约见 ws-handlers/context.ts）。 */
import type { AuthUser } from "./auth"
import { SERVICE_USER, type AppDeps } from "./app"
import { isValidSessionId } from "./core/base/paths"
import type { WsHandler } from "./ws-handlers/context"
import { authHandlers } from "./ws-handlers/auth"
import { sessionHandlers } from "./ws-handlers/session"
import { promptHandlers } from "./ws-handlers/prompt"
import { interactionHandlers } from "./ws-handlers/interaction"
import { userHandlers, feedbackHandlers, subAgentHandlers } from "./ws-handlers/admin"

/** 消息类型 → 处理器分发表（域文件导出汇总）。 */
const handlers: Record<string, WsHandler> = {
  ...authHandlers,
  ...sessionHandlers,
  ...promptHandlers,
  ...interactionHandlers,
  ...userHandlers,
  ...feedbackHandlers,
  ...subAgentHandlers,
}

/** 需要合法会话 ID 的 WS 消息类型（入口统一校验，防 sessionId 路径穿越）。 */
const SESSION_ID_MSGS = new Set([
  "session.get", "session.delete", "session.rename", "session.switch", "session.compact",
  "session.prompt", "session.attachment.upload", "session.todo.get", "session.env.get", "session.env.set",
  "session.files.list", "session.files.get", "session.cancel", "session.restore", "session.attach",
  "approval.decide", "choice.decide", "env.decide", "draw.result", "capture.result",
])

/** WS 连接上下文：用户 + 连接级当前会话（session.switch/current 用）。 */
export interface WsConn {
  get: () => AuthUser
  set: (u: AuthUser) => void
  getCurrent: () => string | undefined
  setCurrent: (id: string | undefined) => void
  /** 连接用户变更回调注册（auth.login/logout 后事件订阅重绑用）；返回退订函数。 */
  onUserChange?: (cb: () => void) => () => void
}

export interface WsSink {
  send(data: string): void
}

export async function handleWsMessage(
  d: AppDeps,
  ws: WsSink,
  msg: { type: string; payload?: Record<string, unknown>; id?: string },
  conn: WsConn,
) {
  const reply = (ok: boolean, payload?: Record<string, unknown>, error?: string) => {
    ws.send(JSON.stringify({ type: msg.type, id: msg.id, ok, payload, error }))
  }
  const p = (msg.payload || {}) as Record<string, unknown>
  const user = conn.get()

  // 多用户模式：未登录（SERVICE_USER 兜底）的连接仅允许 auth.login
  if (d.config.auth === "server" && user.id === SERVICE_USER.id && msg.type !== "auth.login") {
    return reply(false, undefined, "unauthorized: login required")
  }

  // 会话 ID 格式白名单：穿越串/畸形 id 统一拒绝（多用户隔离防线，与 REST/存储层同规则）
  if (SESSION_ID_MSGS.has(msg.type)) {
    const sid = String(p.id ?? "")
    if (!isValidSessionId(sid)) return reply(false, undefined, `invalid session id: ${sid}`)
  }

  const handler = handlers[msg.type]
  if (!handler) return reply(false, undefined, `unknown message type: ${msg.type}`)
  try {
    return await handler({ d, ws, p, user, conn, reply })
  } catch (err) {
    // 分支内异常（如路径沙箱拒绝）统一转为错误应答，不中断连接
    return reply(false, undefined, String((err as Error).message || err))
  }
}
