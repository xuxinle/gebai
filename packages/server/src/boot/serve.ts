/** 网络监听与 WS 运行时：Bun.serve（fetch 升级 + websocket 生命周期）+ 连接上下文/背压 sink/
 *  按序串行化。组合装配见 compose.ts；自原单文件 index.ts 的监听段拆分，行为不变。 */
import type { ServerWebSocket } from "bun"
import type { AuthUser } from "../auth"
import { SERVICE_USER, type AppDeps } from "../app"
import { handleWsMessage, type WsConn, type WsSink } from "../ws"
import { WsStateService } from "../ws-state"
import type { Composed } from "./compose"

/** 每个 WS 连接的事件总线退订函数（以连接为键，避免与 ws.data 的 user 字段互相覆盖）。 */
const wsSubs = new WeakMap<object, () => void>()
/** 每个 WS 连接的连接上下文（open/message 共享同一实例，保证用户变更回调一致）。 */
const wsConns = new WeakMap<object, WsConn>()
/**
 * 每个 WS 连接的消息处理链（以连接为键）：Bun 对 async `websocket.message` 处理器
 * **不保证串行**（前一条 await 磁盘/网络 I/O 时下一条即并发进入）——认证（auth.login
 * 依赖磁盘读用户注册表）、会话切换等消息的顺序语义会被打乱，导致紧跟在认证后的
 * 请求以未登录态被拒绝（`unauthorized: login required`）。此处按到达顺序串行处理，
 * 对齐 SDK 假设的「服务端顺序处理消息」契约（认证消息先于其后请求生效）。
 */
const wsMsgChains = new WeakMap<object, Promise<void>>()

/** 单连接发送缓冲上限（字节）：Bun WebSocket 发送缓冲超限说明客户端消费速度远低于
 *  推送速率（慢客户端/高频流式），继续发送缓冲无界增长——断开让其走自动重连 + seq 重放收敛。 */
const WS_MAX_BUFFERED = 16 * 1024 * 1024

/** 构造带背压保护的发送 sink：超限断开连接（客户端自动重连后按事件 seq 重放补偿）。 */
export function makeWsSink(ws: ServerWebSocket<unknown>): WsSink {
  return {
    send: (data: string) => {
      if (ws.readyState !== WebSocket.OPEN) return
      const buffered = (ws as unknown as { getBufferedAmount?: () => number }).getBufferedAmount?.() ?? 0
      if (buffered > WS_MAX_BUFFERED) {
        console.warn("[ws] 慢客户端发送缓冲超限（16MB），断开连接——客户端将自动重连并按 seq 重放")
        ws.close()
        return
      }
      ws.send(data)
    },
  }
}

/**
 * 构建 WS 连接上下文：连接级用户 + 连接级当前会话。
 * 当前会话为「连接覆盖值 ?? 每用户持久化状态」（WsStateService.connState）——
 * 重连/重启后自动恢复，连接级显式切换（session.switch）优先。
 */
export function makeWsConn(ws: ServerWebSocket<unknown>, d: AppDeps, state: WsStateService | undefined): WsConn {
  const existing = wsConns.get(ws)
  if (existing) return existing
  const get = (): AuthUser =>
    d.config.auth === "local" ? d.auth.defaultUser() : ((ws.data as { user?: AuthUser } | undefined)?.user) || SERVICE_USER
  const userChangeCbs = new Set<() => void>()
  const conn: WsConn = {
    get,
    set: (u: AuthUser) => {
      ws.data = { ...((ws.data as object) || {}), user: u }
      for (const cb of userChangeCbs) cb()
    },
    onUserChange: (cb: () => void) => {
      userChangeCbs.add(cb)
      return () => {
        userChangeCbs.delete(cb)
      }
    },
    getCurrent: () => {
      const local = (ws.data as { currentSessionId?: string } | undefined)?.currentSessionId
      if (local !== undefined) return local
      return state?.connState.getCurrent(get().id)
    },
    setCurrent: (id: string | undefined) => {
      if (id === undefined) {
        const data = ws.data as { currentSessionId?: string } | undefined
        if (data && "currentSessionId" in data) {
          const { currentSessionId: _drop, ...rest } = data
          ws.data = rest
        }
      } else {
        ws.data = { ...((ws.data as object) || {}), currentSessionId: id }
      }
      state?.connState.setCurrent(get().id, id)
    },
  }
  wsConns.set(ws, conn)
  return conn
}

/** 启动监听（compose 之后）：Bun.serve + WS 生命周期 + 退出钩子；返回 server 实例。 */
export function serveComposed(c: Composed): ReturnType<typeof Bun.serve> {
  const { config, deps, app, state, devReload, devReloadClients } = c
  const server = Bun.serve<unknown>({
    hostname: config.host,
    port: config.port,
    idleTimeout: 240,
    fetch: (req, srv) => {
      const url = new URL(req.url)
      // 跨站来源防护（本地/桌面免登录形态）：WebSocket 不受同源策略约束，恶意网页可直接连
      // ws://127.0.0.1:* 以 admin 身份建会话执行命令。浏览器发起的 WS 必带 Origin——
      // 与请求 Host 不同源即拒绝升级；非浏览器客户端（无 Origin）不受影响。
      const wsOrigin = req.headers.get("origin")
      if (wsOrigin) {
        const host = req.headers.get("host") ?? url.host
        try {
          if (new URL(wsOrigin).host !== host) return new Response("cross-origin ws rejected", { status: 403 })
        } catch {
          return new Response("invalid origin", { status: 403 })
        }
      }
      const wsPath = `${config.basePath === "/" ? "" : config.basePath}/ws`
      if (url.pathname === wsPath && srv.upgrade(req, { data: {} })) return
      // 开发热刷新通道（仅 --reload 模式注册）：页面经此接收 reload 广播
      const hotPath = `${config.basePath === "/" ? "" : config.basePath}/__gebai_hot`
      if (config.devReload && url.pathname === hotPath && srv.upgrade(req, { data: { hot: true } })) return
      return app.fetch(req, srv)
    },
    websocket: {
      open(ws) {
        // 热刷新通道连接（无消息语义，仅接收广播）；主 /ws 通道逻辑见下
        if ((ws.data as { hot?: boolean } | undefined)?.hot) {
          devReloadClients.add(ws)
          return
        }
        // 连接级事件推送：订阅该用户的事件日志（在线推送 = 日志条目实时投递，带 seq）。
        // 退订函数存于 WeakMap，避免被 auth.login 的 ws.data 覆盖而泄漏订阅。
        // 发送统一走背压保护 sink（慢客户端超限断开，见 makeWsSink）
        const conn = makeWsConn(ws, deps, state)
        const sink = makeWsSink(ws)
        let unsub = state.subscribe(conn.get().id, (entry) => sink.send(JSON.stringify(entry)))
        // 用户变更（auth.login/logout）：事件订阅重绑到新用户
        conn.onUserChange?.(() => {
          unsub()
          unsub = state.subscribe(conn.get().id, (entry) => sink.send(JSON.stringify(entry)))
        })
        wsSubs.set(ws, () => unsub())
        // 本地模式：建连即推送状态快照（服务模式在 auth.login 后推送）
        if (config.auth === "local") {
          void state.pushSnapshot(sink, conn).catch(() => {})
        }
      },
      async message(ws, raw) {
        let msg: { type: string; payload?: Record<string, unknown>; id?: string }
        try {
          msg = JSON.parse(raw as string)
        } catch {
          makeWsSink(ws).send(JSON.stringify({ type: "error", ok: false, error: "bad json" }))
          return
        }
        const conn = makeWsConn(ws, deps, state)
        // 按到达顺序串行处理（见 wsMsgChains 注释）：前一条消息完成后再处理下一条，
        // 保证 auth.login 的 conn.set(u) 先于后续请求生效（Bun 不保证 async handler 串行）
        const prev = wsMsgChains.get(ws) ?? Promise.resolve()
        const next = prev.then(() => handleWsMessage(deps, makeWsSink(ws), msg, conn)).catch(() => {})
        wsMsgChains.set(ws, next)
        await next
      },
      close(ws) {
        wsSubs.get(ws)?.()
        wsSubs.delete(ws)
        devReloadClients.delete(ws)
      },
    },
  })

  // 实际监听端口以 Bun.serve 结果为准（port=0 时为 OS 分配的空闲端口，桌面形态用）
  console.log(
    `[gebai] listening on http://${server.hostname}:${server.port} (GEBAI_HOME=${config.gebaiHome}, auth=${config.auth}, sandbox=${c.sandbox.enabled}${config.devReload ? ", dev-reload" : ""})`,
  )
  // 进程退出时终止 vite build --watch 子进程（防孤儿）
  process.on("exit", () => devReload?.stop())
  return server
}
