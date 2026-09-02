/** WS 消息处理器契约（ws-handlers/ 目录内部）：每个域文件导出 `Record<消息类型, WsHandler>`，
 *  由 ws.ts 的 handleWsMessage 汇总为分发表——入口守卫（未登录拦截/会话 id 白名单）与异常兜底
 *  统一在 ws.ts，处理器只关心单个消息类型。处理器体自原单文件 ws.ts 的 switch 分支原样拆分。 */
import type { AuthUser } from "../auth"
import type { AppDeps } from "../app"
import type { WsConn, WsSink } from "../ws"

export interface WsMsgCtx {
  d: AppDeps
  /** 连接发送通道（带背压保护的 sink）。 */
  ws: WsSink
  /** 消息 payload（缺省空对象）。 */
  p: Record<string, unknown>
  /** 连接当前用户（入口已通过未登录守卫）。 */
  user: AuthUser
  conn: WsConn
  /** 应答（回执同 type/id；ok=false 时带 error）。 */
  reply: (ok: boolean, payload?: Record<string, unknown>, error?: string) => void
}

export type WsHandler = (ctx: WsMsgCtx) => Promise<void> | void
