import type {
  AgentEvent,
  AttachmentInfo,
  AttachmentInput,
  ChatChunk,
  ContentBlock,
  EnvVarSource,
  FeedbackFilter,
  FeedbackInfo,
  FeedbackInput,
  FileEntry,
  MiniToolInfo,
  MiniToolMeta,
  SessionDetail,
  SessionInfo,
  SubAgentInfo,
  TodoItem,
  ToolInfo,
  UserInfo,
  UserPatch,
  WebhookInfo,
  WsSnapshot,
} from "./types"

export interface GebaiClientOptions {
  baseUrl: string
  token?: string
  /** WS 建连超时（毫秒），默认 8000。 */
  connectTimeoutMs?: number
  /** WS 心跳间隔（毫秒），默认 5000；周期 ping 保活，低于代理常见闲置断连阈值。 */
  heartbeatIntervalMs?: number
  /** 心跳应答（pong）超时（毫秒），默认 10000；超时判定死连（半开 TCP），主动断开触发自动重连。 */
  heartbeatTimeoutMs?: number
}

/** WS 建连超时（毫秒）：服务不可达/代理挂起时快速失败，避免初始化永久等待。 */
const CONNECT_TIMEOUT = 8000
/** WS 心跳间隔（毫秒）：低于常见代理闲置断开阈值，pong 未归判定死连。 */
const HEARTBEAT_INTERVAL_MS = 5_000
/** 心跳应答（pong）超时（毫秒）：代理静默掐断连接时快速感知并触发重连。 */
const HEARTBEAT_TIMEOUT_MS = 10_000
/** WS 意外断开后的自动重连退避（毫秒）：1s → 2s → 4s … 上限 30s。 */
const RECONNECT_BASE_MS = 1000
const RECONNECT_MAX_MS = 30_000
/** 请求应答超时（毫秒）：应答未归视为请求失败（心跳有独立超时）。 */
const REQUEST_TIMEOUT_MS = 60_000

interface WsMessage {
  type: string
  payload?: Record<string, unknown>
  id?: string
  ok?: boolean
  error?: string
  /** 服务端推送事件的每用户日志序号（断线重连后按 seq 重放补偿）。 */
  seq?: number
}

/** 发送消息的回调注册（send() 选项）：应答到达自动分派到对应回调。 */
export interface SendHandlers {
  /** 成功应答（ok:true）：payload 为应答数据。 */
  onOk?: (payload: Record<string, unknown>, reply: WsMessage) => void
  /** 错误应答（ok:false）/ 传输失败 / 超时。 */
  onError?: (error: Error) => void
  /** 应答超时（毫秒），默认 60s。 */
  timeoutMs?: number
  /** 离线排队：连接不可用时排队等待自动重连后发送（默认 false：自动建连后立即发送）。 */
  queueOffline?: boolean
}

interface OutboxItem {
  id: string
  type: string
  payload?: Record<string, unknown>
  handlers: SendHandlers
  timer: ReturnType<typeof setTimeout> | null
}

interface PendingEntry {
  resolve: (m: WsMessage) => void
  reject: (e: Error) => void
  timer?: ReturnType<typeof setTimeout>
}

/**
 * 解析 WS 连接地址：显式 baseUrl 优先；浏览器 DOM 下按 location.origin 解析为绝对地址
 * （WebSocket 构造要求可解析为 ws/wss 的绝对 URL，相对路径在基址非 http(s) 的文档
 * （WebView 内嵌 about:blank/srcdoc、file: 等）中直接抛 "The URL '/ws' is invalid"）；
 * 非 DOM 环境（Node/Bun 测试等）回退相对路径。
 */
export function resolveWsUrl(baseUrl: string, loc?: { protocol: string; host: string } | null): string {
  if (baseUrl) {
    const proto = baseUrl.startsWith("https") ? "wss" : "ws"
    return `${proto}://${baseUrl.replace(/^https?:\/\//, "")}/ws`
  }
  const origin = loc?.host ? `${loc.protocol === "https:" ? "wss" : "ws"}://${loc.host}/ws` : ""
  return origin || "/ws"
}

export class GebaiClient {
  private baseUrl: string
  private token?: string
  private ws?: WebSocket
  private connectPromise: Promise<void> | null = null
  /** 建连共享 promise 的 reject（onclose 无 onopen/onerror 极端路径 settle 用）。 */
  private connectReject: ((e: Error) => void) | undefined
  private connectTimeoutMs: number
  private eventHandlers: Array<(event: AgentEvent) => void> = []
  private statusHandlers: Array<(status: "connected" | "disconnected") => void> = []
  private snapshotHandlers: Array<(snapshot: WsSnapshot) => void> = []
  private pending = new Map<string, PendingEntry>()
  private outbox: OutboxItem[] = []
  private seq = 0
  /** 主动关闭（登录/登出/换令牌）：不自动重连。 */
  private manualClose = false
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private reconnectDelay = RECONNECT_BASE_MS
  private connected = false
  private heartbeatIntervalMs: number
  private heartbeatTimeoutMs: number
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null
  /** 当前心跳是否已发出等待 pong（超时未归判定死连）。 */
  private heartbeatPending = false
  /** 状态快照（MVC 模型）：state.snapshot 推送/请求维护，sendPrompt 恢复流程依赖。 */
  private snapshot: WsSnapshot = { currentSessionId: null, sessions: [], running: [], lastSeq: 0, maxContextTokens: 0 }
  /** 已处理的最大事件 seq（断线重连后按 seq 重放补偿）。 */
  private lastSeq = 0

  constructor(opts: GebaiClientOptions) {
    this.baseUrl = opts.baseUrl.replace(/\/+$/, "")
    this.token = opts.token
    this.connectTimeoutMs = opts.connectTimeoutMs ?? CONNECT_TIMEOUT
    this.heartbeatIntervalMs = opts.heartbeatIntervalMs ?? HEARTBEAT_INTERVAL_MS
    this.heartbeatTimeoutMs = opts.heartbeatTimeoutMs ?? HEARTBEAT_TIMEOUT_MS
  }

  login(username: string, password: string): Promise<void> {
    return this.post<{ token: string }>("/api/v1/auth/login", { username, password }).then((r) => {
      this.token = r.token
      this.closeWs()
    })
  }

  /**
   * 注册（仅服务模式开放）：注册用户恒为普通角色（admin 唯一入口是服务端启动参数哈希，不可注册创建）。
   * - open 模式（默认）：注册即登录（自动持有令牌）
   * - approval 模式（GEBAI_SIGNUP_MODE=approval）：返回 { pending: true }，用户待 admin 审批（不可登录，不签发令牌）
   * 失败抛 HTTP 错误（400 用户名非法/已存在）。
   */
  async register(username: string, password: string): Promise<{ user: UserInfo; pending: boolean }> {
    const r = await this.post<{ token?: string; user: UserInfo; pending: boolean }>("/api/v1/auth/register", { username, password })
    if (r.token) {
      this.token = r.token
      this.closeWs()
    }
    return { user: r.user, pending: r.pending }
  }

  /** 当前登录用户信息（服务模式；本地模式为 admin 用户）。 */
  getCurrentUser(): Promise<UserInfo> {
    return this.get<UserInfo>("/api/v1/auth/me")
  }

  /**
   * 外部身份兑换（同源集成扩展点）：网站把本地登录态换为歌白令牌，免二次登录。
   * credential 格式取决于服务端验证器：HMAC 为 "{exp}.{sig}"，回调为业务系统自己的登录 token。
   */
  exchangeExternalUser(username: string, credential: string): Promise<UserInfo> {
    return this.post<{ token: string; user: UserInfo }>("/api/v1/auth/exchange", { username, credential }).then((r) => {
      this.token = r.token
      this.closeWs()
      return r.user
    })
  }

  /** 外部身份扩展点探测：{ enabled, storageKey?, autocreate }（未启用时 enabled=false）。 */
  getExternalAuthConfig(): Promise<{ enabled: boolean; storageKey?: string | null; autocreate?: boolean }> {
    return this.get<{ enabled: boolean; storageKey?: string | null; autocreate?: boolean }>("/api/v1/auth/external-config")
  }

  /** 环境变量配置目录（前端面板白名单）：按全局/子Agent 分组，含变量作用说明；不含启动级/安全敏感变量。 */
  getEnvCatalog(): Promise<{ groups: Array<{ group: string; label: string; vars: Array<{ name: string; description: string }> }> }> {
    return this.get("/api/v1/env/catalog")
  }

  async logout(): Promise<void> {
    await this.post<{ ok: boolean }>("/api/v1/auth/logout", {})
    this.token = undefined
    this.closeWs()
  }

  setToken(token: string): void {
    this.token = token
    this.closeWs()
  }

  /** 当前令牌（UI 登录态持久化用）。 */
  getToken(): string | undefined {
    return this.token
  }

  /** WS 连接是否处于可用状态（UI 状态展示/SSE 兜底判定用）。 */
  isConnected(): boolean {
    return this.connected
  }

  /** 订阅连接状态变化（"connected"/"disconnected"）；返回退订函数。 */
  onStatusChange(handler: (status: "connected" | "disconnected") => void): () => void {
    this.statusHandlers.push(handler)
    return () => {
      const i = this.statusHandlers.indexOf(handler)
      if (i >= 0) this.statusHandlers.splice(i, 1)
    }
  }

  private setConnected(status: "connected" | "disconnected"): void {
    this.connected = status === "connected"
    for (const h of this.statusHandlers) h(status)
  }

  /** 主动关闭当前 WS（登录/登出/换令牌）：取消重连定时器，不触发自动重连。 */
  private closeWs(): void {
    this.manualClose = true
    this.stopHeartbeat()
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    this.ws?.close()
    this.setConnected("disconnected")
  }

  /** 启动心跳（连接建立后）：周期发送 ping，pong 超时判定死连主动断开（触发自动重连）。 */
  private startHeartbeat(): void {
    this.stopHeartbeat()
    this.heartbeatTimer = setInterval(() => this.sendHeartbeat(), this.heartbeatIntervalMs)
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer)
      this.heartbeatTimer = null
    }
    this.heartbeatPending = false
  }

  /**
   * 发送一拍心跳：复用请求-应答机制（send() 注册回调，服务端回 `{type:"ping", id, ok:true}`）。
   * pong 未归（onError：应答错误/超时/断线）判定死连（代理静默掐断/半开 TCP）：
   * 主动 close 触发 onclose → 自动重连。
   */
  private sendHeartbeat(): void {
    const ws = this.ws
    if (!ws || ws.readyState !== WebSocket.OPEN || this.manualClose) return
    // 上一拍 pong 未归：不立即判死（判死阈值是 heartbeatTimeoutMs，不是下一拍间隔——
    // interval < timeout 的高延迟链路上，pong 5~10s 往返会被间隔误杀成「连上→杀→重连」循环），
    // 交给该拍请求自身的超时 onError 触发关闭
    if (this.heartbeatPending) return
    this.heartbeatPending = true
    this.send(
      "ping",
      {},
      {
        timeoutMs: this.heartbeatTimeoutMs,
        onOk: () => {
          this.heartbeatPending = false
        },
        onError: () => {
          // pong 超时未归：判定死连，主动断开 → onclose → 自动重连
          this.heartbeatPending = false
          ws.close()
        },
      },
    )
  }

  /** 意外断开后的自动重连（指数退避）。 */
  private scheduleReconnect(): void {
    if (this.manualClose || this.reconnectTimer) return
    const delay = this.reconnectDelay
    this.reconnectDelay = Math.min(this.reconnectDelay * 2, RECONNECT_MAX_MS)
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null
      this.connect()
        .then(() => {
          this.reconnectDelay = RECONNECT_BASE_MS
        })
        .catch(() => {
          /* 失败由下一次 onclose 的退避重连覆盖；若建连失败未触发 close，直接再试 */
          this.scheduleReconnect()
        })
    }, delay)
  }

  async connect(): Promise<void> {
    if (this.ws?.readyState === WebSocket.OPEN) return
    // 显式 connect（登录/初始化等）= 期望建立连接：复位主动关闭标记，
    // 此后连接意外断开将触发自动重连
    this.manualClose = false
    // 并发调用共享同一连接尝试：等待实际 open/error/超时，避免各自短路造成虚假成功与忙等
    if (this.connectPromise) return this.connectPromise
    let url: string
    if (this.baseUrl) {
      const proto = this.baseUrl.startsWith("https") ? "wss" : "ws"
      url = `${proto}://${this.baseUrl.replace(/^https?:\/\//, "")}/ws`
    } else {
      // same-host static hosting / Vite proxy：浏览器下解析为绝对地址（见 resolveWsUrl）
      const loc = (globalThis as Record<string, unknown>).location as { protocol: string; host: string } | undefined
      url = resolveWsUrl(this.baseUrl, loc ?? null)
    }
    this.connectPromise = new Promise<void>((resolve, reject) => {
        this.connectReject = reject
      const ws = new WebSocket(url)
      // 连接超时保护：服务不可达（代理挂起等）时避免页面永久卡在初始化
      const timer = setTimeout(() => {
        ws.close()
        finish(new Error("WS connect timeout"))
      }, this.connectTimeoutMs)
      const finish = (err?: Error) => {
        clearTimeout(timer)
        // 仅清理属于本次连接的共享状态：旧连接的迟到回调（this.ws 已被新连接覆盖）不会误伤
        if (this.ws === ws) {
          this.connectPromise = null
          if (err) this.ws = undefined
        }
        if (err) reject(err)
        else resolve()
      }
      ws.onopen = () => {
        this.setConnected("connected")
        // 认证与离线队列按序上线：先发送令牌/Key 认证（服务端顺序处理消息，
        // 此后排队的请求即带用户上下文），再冲刷离线排队的消息
        this.authenticate()
        this.flushOutbox()
        this.startHeartbeat()
        finish()
      }
      ws.onerror = () => finish(new Error("WS connect failed"))
      ws.onmessage = (m) => this.handleMessage(m)
      // 运行期断开（正常 close frame 不触发 onerror）：失败在途请求并清空连接状态以便重连
      ws.onclose = () => {
        if (this.ws !== ws) return // 旧连接的迟到 close：新连接的在途请求不受影响
        // 建连期单独触发 close（无 onopen/onerror 的极端路径）：settle 共享 promise，否则等待方永久挂起
        this.connectPromise = null
        this.connectReject?.(new Error("WS closed"))
        this.connectReject = undefined
        // 在途请求统一失败并清超时 timer（不清会悬挂触发第二次 onError——误导性的「WS 请求超时」）
        for (const { reject, timer } of this.pending.values()) {
          if (timer) clearTimeout(timer)
          reject(new Error("WS closed"))
        }
        this.pending.clear()
        this.ws = undefined
        this.stopHeartbeat()
        this.setConnected("disconnected")
        // 意外断开自动重连（指数退避）；主动关闭（登录/登出/换令牌）不重连
        if (!this.manualClose) this.scheduleReconnect()
      }
      this.ws = ws
    })
    return this.connectPromise
  }

  onEvent(handler: (event: AgentEvent) => void): () => void {
    this.eventHandlers.push(handler)
    return () => {
      const i = this.eventHandlers.indexOf(handler)
      if (i >= 0) this.eventHandlers.splice(i, 1)
    }
  }

  /** 订阅状态快照（连接建立/登录/重连后推送，含当前会话/会话列表/运行中会话/事件基线 seq）。 */
  onSnapshot(handler: (snapshot: WsSnapshot) => void): () => void {
    this.snapshotHandlers.push(handler)
    handler(this.snapshot)
    return () => {
      const i = this.snapshotHandlers.indexOf(handler)
      if (i >= 0) this.snapshotHandlers.splice(i, 1)
    }
  }

  /** 最近一次状态快照（MVC 模型只读视图）。 */
  getSnapshot(): WsSnapshot {
    return this.snapshot
  }

  private handleMessage(m: MessageEvent) {
    // 防御性解析：畸形/非预期消息静默跳过，不中断事件流处理
    let data: unknown
    try {
      data = JSON.parse(m.data as string)
    } catch {
      return
    }
    if (typeof data !== "object" || data === null) return
    const msg = data as WsMessage
    // 服务端推送的状态快照（id 为空）：更新模型并通知订阅者
    if (msg.type === "state.snapshot" && msg.id == null) {
      this.applySnapshot(msg.payload || {})
      return
    }
    if (typeof msg.id === "string" && this.pending.has(msg.id)) {
      const { resolve, reject, timer } = this.pending.get(msg.id)!
      this.pending.delete(msg.id)
      if (timer) clearTimeout(timer)
      if (msg.ok === false) {
        // 错误码透传（协议级判定依据，如补发幂等识别 already_running——不再依赖错误文案正则）
        const err = new Error(msg.error || "WS error") as Error & { code?: string }
        err.code = typeof msg.payload?.code === "string" ? msg.payload.code : undefined
        reject(err)
      } else resolve(msg)
      return
    }
    if (typeof msg.type === "string" && msg.type.startsWith("event.")) {
      const event: AgentEvent = {
        type: msg.type,
        sessionId: (msg.payload?.sessionId as string) || "",
        payload: msg.payload || {},
        timestamp: Date.now(),
      }
      if (typeof msg.seq === "number") event.seq = msg.seq
      this.dispatchEvent(event)
    }
  }

  /**
   * 事件分发（在线推送与断线重放共用）：更新 seq/运行态模型基线后通知全部订阅者。
   * 断线补偿的重放事件同样经此分发——离线期间的审批/选择/工具事件全局订阅者（前端卡片渲染）
   * 也能看到，而非只进 sendPrompt 的 chunk 通道（否则重连后审批卡不出现、任务卡死至超时）。
   */
  private dispatchEvent(event: AgentEvent): void {
    if (typeof event.seq === "number") {
      // seq 去重：并发 sendPrompt 恢复（多会话同时流式输出时断线重连）会各自重放同一批用户级事件，
      // 已分发过的 seq 直接跳过（chunk 重复拼接/卡片双份渲染）；seq 单调，旧 seq 同样视为已见
      if (event.seq <= this.lastSeq) return
      this.lastSeq = event.seq
      if (event.seq > this.snapshot.lastSeq) this.snapshot.lastSeq = event.seq
    }
    // 模型增量：运行中会话集合随任务事件更新（快照 running 的实时延续）
    if (event.type === "event.task.done" || event.type === "event.task.error") {
      this.snapshot.running = this.snapshot.running.filter((id) => id !== event.sessionId)
    }
    for (const h of this.eventHandlers) h(event)
  }

  private applySnapshot(payload: Record<string, unknown>): void {
    const sessions = Array.isArray(payload.sessions) ? (payload.sessions as SessionInfo[]) : []
    const running = Array.isArray(payload.running) ? payload.running.map(String) : []
    const lastSeq = Number(payload.lastSeq ?? this.lastSeq)
    // 非数字防护：对齐 lastSeq 的 Number.isFinite 处理，防 NaN 直通标题栏显示
    const maxCtx = Number(payload.maxContextTokens ?? 0)
    this.snapshot = {
      currentSessionId: payload.currentSessionId != null ? String(payload.currentSessionId) : null,
      sessions,
      running,
      // 快照基线不倒退：事件可能先于快照到达（lastSeq 只增不减）
      lastSeq: Math.max(Number.isFinite(lastSeq) ? lastSeq : 0, this.lastSeq),
      // 模型上下文窗口（0=未知）：标题栏上下文占比显示用，重建快照时不得丢弃
      maxContextTokens: Number.isFinite(maxCtx) ? maxCtx : 0,
    }
    this.lastSeq = this.snapshot.lastSeq
    for (const h of this.snapshotHandlers) h(this.snapshot)
  }

  /** 请求当前状态快照（主动收敛模型，幂等）。 */
  private requestSnapshot(): Promise<WsSnapshot> {
    return this.request<Partial<WsSnapshot>>("state.snapshot", {}).then((p) => {
      const maxCtx = Number(p.maxContextTokens ?? 0)
      const snap: WsSnapshot = {
        currentSessionId: p.currentSessionId != null ? String(p.currentSessionId) : null,
        sessions: p.sessions ?? [],
        running: (p.running ?? []).map(String),
        lastSeq: Number(p.lastSeq ?? 0),
        maxContextTokens: Number.isFinite(maxCtx) ? maxCtx : 0,
      }
      this.applySnapshot(snap as unknown as Record<string, unknown>)
      return snap
    })
  }

  /**
   * 发送消息并注册应答回调（WebSocket RPC 复用入口）：
   * 应答（ok/error）到达自动分派到 onOk/onError；返回取消函数（取消后不再回调）。
   * queueOffline 时连接不可用则排队，连接恢复（含自动重连）后按序发送。
   */
  send(type: string, payload?: Record<string, unknown>, handlers: SendHandlers = {}): () => void {
    let cancelled = false
    const timeoutMs = handlers.timeoutMs ?? REQUEST_TIMEOUT_MS
    const fail = (err: Error) => {
      if (!cancelled) handlers.onError?.(err)
    }
    const deliver = (ws: WebSocket, id: string): void => {
      const entry: PendingEntry = {
        resolve: (m) => {
          if (!cancelled) handlers.onOk?.(m.payload ?? {}, m)
        },
        reject: (e) => {
          if (!cancelled) handlers.onError?.(e)
        },
      }
      if (timeoutMs > 0) {
        entry.timer = setTimeout(() => {
          this.pending.delete(id)
          fail(new Error(`WS 请求超时: ${type}`))
        }, timeoutMs)
      }
      this.pending.set(id, entry)
      ws.send(JSON.stringify({ type, payload, id }))
    }
    let id: string | undefined
    if (this.ws?.readyState === WebSocket.OPEN) {
      id = `r${++this.seq}`
      deliver(this.ws, id)
    } else if (handlers.queueOffline) {
      id = `q${++this.seq}`
      const item: OutboxItem = { id, type, payload, handlers, timer: null }
      if (timeoutMs > 0) {
        item.timer = setTimeout(() => {
          const i = this.outbox.indexOf(item)
          if (i >= 0) this.outbox.splice(i, 1)
          fail(new Error(`WS 请求超时: ${type}`))
        }, timeoutMs)
      }
      this.outbox.push(item)
      // 尝试建连：失败保持排队（等下一次连接/重连冲刷），不回调错误
      void this.connect().catch(() => {})
    } else {
      // 默认：自动建连后发送（连接失败 → onError）
      void this.connect()
        .then(() => {
          if (!this.ws || this.ws.readyState !== WebSocket.OPEN) throw new Error("WS closed")
          id = `r${++this.seq}`
          deliver(this.ws!, id)
        })
        .catch((e) => fail(e))
    }
    return () => {
      cancelled = true
      if (id) {
        const p = this.pending.get(id)
        if (p?.timer) clearTimeout(p.timer)
        this.pending.delete(id)
        const i = this.outbox.findIndex((o) => o.id === id)
        if (i >= 0) {
          const item = this.outbox[i]
          if (item.timer) clearTimeout(item.timer)
          this.outbox.splice(i, 1)
        }
      }
    }
  }

  /** Promise 版 RPC（send() 的封装）：应答 payload 即结果，错误应答/超时/断线 → reject。 */
  request<T>(type: string, payload?: Record<string, unknown>): Promise<T> {
    return new Promise<T>((resolve, reject) => {
      this.send(type, payload, {
        onOk: (p) => resolve(p as T),
        onError: reject,
      })
    })
  }

  /** 断线补偿：按 seq 请求重放离线期间错过的日志事件；返回 null 表示缺口（走全量重同步）。 */
  private async syncEvents(lastSeq: number): Promise<{ events: AgentEvent[]; overrun: boolean; lastSeq: number }> {
    const r = await this.request<{ events: unknown; overrun?: boolean; lastSeq?: number }>("sync.request", { lastSeq })
    const entries = (Array.isArray(r.events) ? r.events : []) as Array<{ type: string; sessionId: string; payload: Record<string, unknown>; seq: number }>
    return {
      events: entries.map((e) => ({ type: e.type, sessionId: e.sessionId, payload: e.payload, seq: e.seq, timestamp: Date.now() })),
      overrun: r.overrun === true || !Array.isArray(r.events),
      lastSeq: Number(r.lastSeq ?? this.lastSeq),
    }
  }

  /**
   * 连接建立后的自动认证：WS 连接无法携带 Header，重连后服务端用户上下文丢失——
   * 已持有令牌时自动发送 auth.login（token 形式）恢复用户身份，
   * 此后排队的请求即带用户上下文（服务模式断线重连不再掉回未登录态）。
   */
  private authenticate(): void {
    if (this.token) this.send("auth.login", { token: this.token }, { onError: () => {} })
  }

  /** 冲刷离线队列：按入队顺序发送（认证消息已先行，服务端顺序处理）。 */
  private flushOutbox(): void {
    while (this.outbox.length) {
      const item = this.outbox.shift()!
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
        // 冲刷期间连接又断开：放回队首，等下次重连再发
        this.outbox.unshift(item)
        return
      }
      if (item.timer) clearTimeout(item.timer)
      const id = `r${++this.seq}`
      const entry: PendingEntry = {
        resolve: (m) => item.handlers.onOk?.(m.payload ?? {}, m),
        reject: (e) => item.handlers.onError?.(e),
      }
      const t = item.handlers.timeoutMs ?? REQUEST_TIMEOUT_MS
      if (t > 0) {
        entry.timer = setTimeout(() => {
          this.pending.delete(id)
          item.handlers.onError?.(new Error(`WS 请求超时: ${item.type}`))
        }, t)
      }
      this.pending.set(id, entry)
      this.ws.send(JSON.stringify({ type: item.type, payload: item.payload, id }))
    }
  }

  private headers(): Record<string, string> {
    const h: Record<string, string> = { "Content-Type": "application/json" }
    if (this.token) h["Authorization"] = `Bearer ${this.token}`
    return h
  }

  private async post<T>(path: string, body: unknown): Promise<T> {
    const res = await fetch(`${this.baseUrl}${path}`, {
      method: "POST",
      headers: this.headers(),
      body: JSON.stringify(body),
    })
    if (!res.ok) throw new Error(`HTTP ${res.status}: ${await res.text()}`)
    return res.json() as Promise<T>
  }

  private async get<T>(path: string): Promise<T> {
    const res = await fetch(`${this.baseUrl}${path}`, { headers: this.headers() })
    if (!res.ok) throw new Error(`HTTP ${res.status}: ${await res.text()}`)
    return res.json() as Promise<T>
  }

  private async del<T>(path: string): Promise<T> {
    const res = await fetch(`${this.baseUrl}${path}`, { method: "DELETE", headers: this.headers() })
    if (!res.ok) throw new Error(`HTTP ${res.status}: ${await res.text()}`)
    return res.json() as Promise<T>
  }

  // ---- Sessions ----
  listSessions(): Promise<SessionInfo[]> {
    return this.request<{ sessions: SessionInfo[] }>("session.list").then((r) => r.sessions)
  }
  createSession(name?: string): Promise<SessionInfo> {
    return this.request<{ session: SessionInfo }>("session.create", name ? { name } : {}).then((r) => r.session)
  }
  getSession(id: string): Promise<SessionDetail> {
    return this.request<{ session: SessionDetail }>("session.get", { id }).then((r) => r.session)
  }
  deleteSession(id: string): Promise<void> {
    return this.request<void>("session.delete", { id })
  }
  /** 撤回消息：删除该消息及其后的所有消息（REST 截断接口）。 */
  truncateSession(id: string, beforeMsgId: string): Promise<void> {
    return this.post(`/api/v1/sessions/${id}/truncate`, { before: beforeMsgId }).then(() => undefined)
  }
  renameSession(id: string, name: string): Promise<void> {
    return this.request<void>("session.rename", { id, name })
  }
  switchSession(id: string): Promise<void> {
    return this.request<void>("session.switch", { id })
  }
  /** 获取连接级当前会话（WS session.current；未设置返回 null）。 */
  getCurrentSession(): Promise<SessionInfo | null> {
    return this.request<{ session: SessionInfo | null }>("session.current").then((r) => r.session)
  }

  getSessionEnv(sessionId: string): Promise<EnvVarSource[]> {
    return this.request<{ env: EnvVarSource[] }>("session.env.get", { id: sessionId }).then((r) => r.env)
  }
  setSessionEnv(sessionId: string, vars: Record<string, string | null>): Promise<void> {
    return this.request<void>("session.env.set", { id: sessionId, vars })
  }

  compactSession(sessionId: string, scope?: "all" | { from: number; to: number }): Promise<void> {
    return this.request<void>("session.compact", { id: sessionId, scope })
  }

  cancelTask(sessionId: string): Promise<void> {
    return this.request<void>("session.cancel", { id: sessionId })
  }
  decideApproval(sessionId: string, toolCallId: string, approve: boolean): Promise<void> {
    return this.request<void>("approval.decide", { id: sessionId, toolCallId, approve })
  }
  /** 提交用户选择（ask_user 工具阻塞等待的选择）；字符串为单选，数组为多选，null 表示拒绝回答。 */
  decideChoice(sessionId: string, choiceId: string, selection: string | string[] | null): Promise<void> {
    const payload = selection == null ? { choiceId, refuse: true } : Array.isArray(selection) ? { choiceId, options: selection } : { choiceId, option: selection }
    return this.request<void>("choice.decide", { id: sessionId, ...payload })
  }
  /** 提交用户填写的环境变量值（ask_env 工具阻塞等待）；value 为 null 表示拒绝提供。 */
  decideEnv(sessionId: string, envId: string, value: string | null): Promise<void> {
    return this.request<void>("env.decide", { id: sessionId, envId, value })
  }
  /** 提交前端渲染结果（draw 工具阻塞等待的渲染回传）。 */
  submitDrawResult(sessionId: string, renderId: string, ok: boolean, error?: string): Promise<void> {
    return this.request<void>("draw.result", { id: sessionId, renderId, ok, error })
  }
  /** 提交前端页面捕获结果（page_capture 工具阻塞等待的捕获回传）：html + 截图 base64（png，data URL 或裸 base64）。 */
  submitCaptureResult(sessionId: string, captureId: string, result: { html: string; imageBase64?: string; error?: string }): Promise<void> {
    return this.request<void>("capture.result", { id: sessionId, captureId, ...result })
  }
  submitFeedback(feedback: FeedbackInput): Promise<void> {
    return this.request<void>("feedback.submit", { feedback })
  }
  listFeedback(filter?: FeedbackFilter): Promise<FeedbackInfo[]> {
    return this.request<{ feedback: FeedbackInfo[] }>("feedback.list", { filter }).then((r) => r.feedback)
  }

  // ---- Webhooks（REST） ----
  listWebhooks(): Promise<WebhookInfo[]> {
    return this.get<WebhookInfo[]>("/api/v1/webhooks")
  }
  registerWebhook(input: { url: string; events?: string[]; secret?: string }): Promise<WebhookInfo> {
    return this.post<WebhookInfo>("/api/v1/webhooks", input)
  }
  deleteWebhook(id: string): Promise<void> {
    return this.del(`/api/v1/webhooks/${id}`).then(() => undefined)
  }

  listUsers(): Promise<UserInfo[]> {
    return this.request<UserInfo[]>("user.list")
  }
  createUser(username: string, password: string, role?: "user" | "admin"): Promise<UserInfo> {
    return this.request<UserInfo>("user.create", { username, password, role })
  }
  updateUser(id: string, patch: UserPatch): Promise<UserInfo> {
    return this.request<UserInfo>("user.update", { id, patch })
  }
  deleteUser(id: string): Promise<void> {
    return this.request<void>("user.delete", { id })
  }

  listSubAgents(): Promise<SubAgentInfo[]> {
    return this.request<{ subAgents: SubAgentInfo[] }>("sub_agent.list").then((r) => r.subAgents)
  }
  /** 装载子Agent：sessionId 传入时装载到该会话（注册工具 + 提示词消息写入会话记录）；缺省仅全局注册工具。 */
  loadSubAgent(name: string, sessionId?: string): Promise<void> {
    return this.request<void>("sub_agent.load", { name, ...(sessionId ? { sessionId } : {}) })
  }
  /** 卸载子Agent：sessionId 传入时同步清理该会话内已持久化的装载提示词与记录；缺省仅注销全局工具注册。 */
  unloadSubAgent(name: string, sessionId?: string): Promise<void> {
    return this.request<void>("sub_agent.unload", { name, ...(sessionId ? { sessionId } : {}) })
  }
  /** 从回收站（GC 归档，保留期 7 天）恢复会话；归属用户或 admin 可操作。 */
  restoreSession(id: string): Promise<void> {
    return this.request<void>("session.restore", { id })
  }

  // ---- HTML 小工具库（REST） ----
  /** 列出对当前用户可见的小工具（公用全部 + 本人私有；同名时私有覆盖公用）。 */
  listMiniTools(): Promise<MiniToolMeta[]> {
    return this.get<MiniToolMeta[]>("/api/v1/mini-tools")
  }
  /** 读取单个小工具（含 HTML 源码；解析顺序：用户私有 → 公用）。 */
  getMiniTool(name: string): Promise<MiniToolInfo> {
    return this.get<MiniToolInfo>(`/api/v1/mini-tools/${encodeURIComponent(name)}`)
  }
  /** 删除小工具（私有仅本人，公用需谨慎；?scope=private|public）。 */
  deleteMiniTool(name: string, scope: "public" | "private" = "private"): Promise<void> {
    return this.del(`/api/v1/mini-tools/${encodeURIComponent(name)}?scope=${scope}`).then(() => undefined)
  }

  listTodos(sessionId: string): Promise<TodoItem[]> {
    return this.request<{ todos: TodoItem[] }>("session.todo.get", { id: sessionId }).then((r) => r.todos)
  }
  listTools(): Promise<ToolInfo[]> {
    return this.request<{ tools: ToolInfo[] }>("session.tool.get", {}).then((r) => r.tools)
  }
  /** 工具启停（REST PATCH /api/v1/tools）。 */
  setToolEnabled(name: string, enabled: boolean): Promise<void> {
    return fetch(`${this.baseUrl}/api/v1/tools`, {
      method: "PATCH",
      headers: this.headers(),
      body: JSON.stringify({ name, enabled }),
    }).then(async (res) => {
      if (!res.ok) throw new Error(`HTTP ${res.status}: ${await res.text()}`)
    })
  }

  listSessionFiles(sessionId: string): Promise<FileEntry[]> {
    return this.request<{ files: FileEntry[] }>("session.files.list", { id: sessionId }).then((r) => r.files)
  }
  async readSessionFile(sessionId: string, path: string): Promise<string> {
    const q = new URLSearchParams({ path })
    return this.get<string>(`/api/v1/sessions/${sessionId}/files/content?${q}`)
  }
  async downloadSessionFile(sessionId: string, path: string): Promise<Blob | Uint8Array> {
    const q = new URLSearchParams({ path })
    const res = await fetch(`${this.baseUrl}/api/v1/sessions/${sessionId}/files/download?${q}`, {
      headers: this.headers(),
    })
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    const buf = new Uint8Array(await res.arrayBuffer())
    return typeof Blob !== "undefined" ? new Blob([buf]) : buf
  }

  /** 多选打包下载：返回 zip 归档字节（REST POST /files/download）。 */
  async downloadFilesZip(sessionId: string, paths: string[]): Promise<Blob | Uint8Array> {
    const res = await fetch(`${this.baseUrl}/api/v1/sessions/${sessionId}/files/download`, {
      method: "POST",
      headers: this.headers(),
      body: JSON.stringify({ paths }),
    })
    if (!res.ok) throw new Error(`HTTP ${res.status}: ${await res.text()}`)
    const buf = new Uint8Array(await res.arrayBuffer())
    return typeof Blob !== "undefined" ? new Blob([buf]) : buf
  }

  async uploadAttachment(sessionId: string, file: Blob | Uint8Array, name: string): Promise<AttachmentInfo> {
    const form = new FormData()
    let part: Blob
    if (file instanceof Blob) {
      part = file
    } else {
      const copy = new Uint8Array(file.byteLength)
      copy.set(file)
      part = new Blob([copy.buffer])
    }
    form.append("file", part, name)
    const res = await fetch(`${this.baseUrl}/api/v1/sessions/${sessionId}/attachments`, {
      method: "POST",
      headers: this.token ? { Authorization: `Bearer ${this.token}` } : {},
      body: form,
    })
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    return res.json() as Promise<AttachmentInfo>
  }

  /** 是否会自动重连（主动关闭（登录/登出/换令牌）为 false）。 */
  private autoReconnecting(): boolean {
    return !this.manualClose
  }

  /**
   * 发送消息（WebSocket 通道）。任务经 WS `session.prompt` 发起，流式输出由 WS 事件推送
   * （event.message.delta/reasoning、event.tool.call/result、event.approval.request、
   * event.task.done/error）转换为 ChatChunk 迭代返回。
   * env：浏览器本地环境变量，随请求临时注入，服务端不持久化。
   * messageId：客户端生成的消息 id（撤回/反馈定位用）；不传由服务端生成。
   * signal：中止时结束迭代（调用方应同时 cancelTask 停止服务端任务）。
   *
   * 断线恢复（状态一致性）：WS 断开时迭代**挂起**（不抛错），等待自动重连后按
   * 事件 seq 向服务端请求重放离线期间错过的日志事件（sync.request），事件流无缝
   * 续上；任务未确认接收时先以状态快照判定（运行中 → 恢复，已跑完 → 从存储合成
   * 收尾，未开始 → 补发）；日志缺口（缓冲溢出）走全量重同步（resume 重置 + 存储内容）。
   */
  async *sendPrompt(
    sessionId: string,
    prompt: string,
    opts?: { attachments?: AttachmentInput[]; env?: Record<string, string>; messageId?: string; signal?: AbortSignal; interactionMode?: "none" | "multi_turn" | "realtime"; stream?: boolean },
  ): AsyncIterable<ChatChunk> {
    const queue: ChatChunk[] = []
    const waiters: Array<() => void> = []
    let finished = false
    let fail: Error | null = null
    let suspended = false
    let resuming = false
    let resyncing = false
    let held: AgentEvent[] = []
    let accepted = false
    let promptSent = false
    const wake = () => waiters.shift()?.()
    const push = (c: ChatChunk | null) => {
      if (!c) return
      queue.push(c)
      wake()
    }
    const markRunning = () => {
      if (!this.snapshot.running.includes(sessionId)) this.snapshot.running.push(sessionId)
    }
    const processEvent = (ev: AgentEvent) => {
      if (ev.sessionId !== sessionId) return
      if (resyncing) {
        held.push(ev)
        return
      }
      push(wsEventToChunk(ev))
      if (ev.type === "event.task.done" || ev.type === "event.task.error") {
        finished = true
        wake()
      }
    }
    const unsub = this.onEvent(processEvent)
    // WS 断开：挂起等待自动重连（不会自动重连的关闭 → 流中断报错）
    const statusUnsub = this.onStatusChange((status) => {
      if (status === "disconnected") {
        if (!this.autoReconnecting()) {
          fail = new Error("WS 连接已关闭，流式输出中断")
          wake()
          return
        }
        suspended = true
      } else if (status === "connected" && suspended) {
        suspended = false
        void resume()
      }
    })
    const onAbort = () => {
      fail = new Error("aborted")
      wake()
    }
    if (opts?.signal?.aborted) onAbort()
    else opts?.signal?.addEventListener("abort", onAbort, { once: true })

    const sendPromptReq = async (): Promise<void> => {
      promptSent = true
      try {
        await this.request("session.prompt", {
          id: sessionId,
          prompt,
          attachments: opts?.attachments?.map((a) => ({ ...a, data: a.data ? Array.from(a.data) : undefined })),
          env: opts?.env,
          messageId: opts?.messageId,
          interactionMode: opts?.interactionMode,
          stream: opts?.stream,
        })
        accepted = true
        markRunning()
      } catch (e) {
        // 任务已在运行（重连补发/并发）：视为已接受，转入恢复流程。
        // 以协议错误码判定（不再依赖服务端错误文案正则——跨包隐式契约，改文案即静默破坏恢复）
        if ((e as Error & { code?: string }).code === "already_running") {
          accepted = true
          return
        }
        throw e
      }
    }

    /** 快任务在离线期间跑完：从存储合成最终内容（resume 重置后重建，不重复渲染）。 */
    const finishOffline = async (): Promise<boolean> => {
      const session = await this.request<{ session: SessionDetail }>("session.get", { id: sessionId })
      const msgs = session?.session?.messages ?? []
      const msgId = opts?.messageId
      const idx = msgId
        ? msgs.findIndex((m) => m.role === "user" && m.id === msgId)
        : msgs.findLastIndex((m) => m.role === "user" && m.content === prompt)
      if (idx < 0) return false
      const after = msgs.slice(idx + 1)
      if (!after.length) return false // 无后续消息：任务从未开始
      push({ kind: "resume" })
      const last = after.filter((m) => m.role === "assistant" && m.content).at(-1)
      if (last) push({ kind: "text", text: last.content, messageId: last.id })
      push({ kind: "done" })
      finished = true
      wake()
      // 本路径不经 syncEvents：后台收敛 seq 基线（防下次断线恢复重复重放旧区间）
      void this.syncEvents(this.lastSeq).then((r) => {
        this.lastSeq = Math.max(this.lastSeq, r.lastSeq)
      }).catch(() => {})
      return true
    }

    /** 日志缺口（overrun）：全量重同步——resume 重置后从存储恢复已持久化内容，继续实时流。 */
    const resyncOverrun = async (): Promise<void> => {
      const session = await this.request<{ session: SessionDetail }>("session.get", { id: sessionId })
      const msgs = session?.session?.messages ?? []
      push({ kind: "resume" })
      const last = msgs.filter((m) => m.role === "assistant" && m.content).at(-1)
      if (last) push({ kind: "text", text: last.content, messageId: last.id })
      if (!this.snapshot.running.includes(sessionId)) {
        push({ kind: "done" })
        finished = true
        wake()
      }
    }

    /** 重连恢复：判定任务状态 → 补发/合成收尾 → 按 seq 重放离线事件。 */
    const resume = async (): Promise<void> => {
      if (resuming || finished) return
      resuming = true
      try {
        // 1) 任务未确认接收：以状态快照判定（运行中 → 恢复；已跑完 → 合成收尾；未开始 → 补发）
        if (!accepted) {
          const snap = await this.requestSnapshot()
          if (snap.running.includes(sessionId)) {
            accepted = true
            markRunning()
          } else if (promptSent && (await finishOffline())) {
            return
          }
        }
        if (!accepted) {
          await sendPromptReq() // 补发（原请求在断线时丢失）
        }
        // 2) 同步：重放离线期间错过的日志事件；缺口（overrun）走全量重同步
        resyncing = true
        const r = await this.syncEvents(this.lastSeq)
        resyncing = false
        const live = held
        held = []
        if (r.overrun) {
          // overrun 分支不分发日志事件（全量重同步）：直接收敛基线，防下次恢复重复判定缺口
          this.lastSeq = Math.max(this.lastSeq, r.lastSeq)
          await resyncOverrun()
          if (!finished) {
            for (const ev of live) processEvent(ev) // 重放范围外的新事件继续
          }
          return
        }
        // 重放事件经 dispatchEvent 分发：sendPrompt 的 chunk 通道与全局 onEvent 订阅者
        // （前端审批/选择/工具卡片渲染）都收到——离线期间的交互卡片重连后可恢复
        for (const ev of r.events) this.dispatchEvent(ev)
        // 分发后收敛 seq 基线：缺口后方「无新事件」窗口内 lastSeq 停留在断线前的滞后值，
        // 下次断线恢复会重复重放旧区间（含旧 task.done 截断新任务流）；并发 resume 的重复
        // 事件也由 dispatchEvent 的 seq 去重过滤（重放事件已在此前分发推进过 lastSeq）
        this.lastSeq = Math.max(this.lastSeq, r.lastSeq)
        if (finished) return
        // 实时事件中超出重放范围的（重放应答期间到达的）继续处理，避免重复
        for (const ev of live) {
          if (!ev.seq || ev.seq > r.lastSeq) processEvent(ev)
        }
      } catch (e) {
        // 恢复过程中连接再次断开：挂起等待下一次重连
        if (this.autoReconnecting() && !this.isConnected()) {
          suspended = true
          return
        }
        fail = new Error(String((e as Error).message || e))
        wake()
      } finally {
        resuming = false
      }
    }

    try {
      await this.connect()
      // 首次请求：断线时请求失败不立即报错，转入挂起等待重连恢复
      await sendPromptReq().catch((e) => {
        if (!this.autoReconnecting() || this.isConnected()) throw e
        suspended = true
      })
      for (;;) {
        if (queue.length) {
          const c = queue.shift()!
          yield c
          if (c.kind === "done" || c.kind === "error") return
          continue
        }
        if (finished) return
        // 失败仅在任务未完成时抛出（done/error 已入队则正常结束）
        if (fail) throw fail
        await new Promise<void>((resolve) => waiters.push(resolve))
      }
    } finally {
      unsub()
      statusUnsub()
      opts?.signal?.removeEventListener("abort", onAbort)
    }
  }
}

/**
 * WS 事件 → ChatChunk 转换（sendPrompt 内部使用；导出便于单测）。
 * 字段对齐原 SSE toChatChunk 契约（SDK/前端消费 ChatChunk）。
 */
export function wsEventToChunk(ev: AgentEvent): ChatChunk | null {
  const p = ev.payload
  // 新会话执行过程事件透传标记：reasoning/工具/审批事件与主循环共用，前端据此识别渲染到新会话容器
  const sub = p.session === true ? { session: true as const } : {}
  const runId = typeof p.sessionRunId === "string" ? { sessionRunId: p.sessionRunId } : {}
  switch (ev.type) {
    case "event.session.start":
      return {
        kind: "session_start",
        session: true,
        sessionRunId: String(p.runId ?? ""),
        sessionMeta: { agents: Array.isArray(p.agents) ? p.agents.map(String) : [], input: String(p.input ?? "") },
      }
    case "event.session.done":
      return {
        kind: "session_done",
        session: true,
        sessionRunId: String(p.runId ?? ""),
        // 异常/取消路径：output 为空时携带 error 说明（前端折叠容器显示中断原因）
        sessionMeta: { agents: Array.isArray(p.agents) ? p.agents.map(String) : [], output: String((p.output as string) || (p.error ? `（已中断: ${String(p.error)}）` : "")) },
      }
    case "event.message.delta": {
      const chunk: ChatChunk = { kind: "text", text: String(p.text ?? ""), messageId: p.messageId as string | undefined }
      // 新会话执行过程标记透传（前端据此分段显示新会话输出）
      if (p.session === true) chunk.session = true
      if (typeof p.sessionRunId === "string") chunk.sessionRunId = p.sessionRunId
      return chunk
    }
    case "event.message.reasoning":
      return { kind: "reasoning", text: String(p.text ?? ""), ...sub, ...runId }
    case "event.tool.call":
      return {
        kind: "tool_call",
        toolCall: { id: String(p.toolCallId ?? ""), name: String(p.name ?? ""), arguments: (p.arguments as Record<string, unknown>) || {} },
        ...sub,
        ...runId,
      }
    case "event.tool.result":
      return {
        kind: "tool_result",
        toolCall: { id: String(p.toolCallId ?? ""), name: String(p.name ?? ""), arguments: {} },
        output: String(p.output ?? ""),
        blocks: (p.blocks as ContentBlock[] | undefined) ?? undefined,
        ...sub,
        ...runId,
      }
    case "event.approval.request":
      return { kind: "approval", approval: { toolCallId: String(p.toolCallId ?? ""), retries: Number(p.retries ?? 0), tool: String(p.tool ?? "") }, ...sub, ...runId }
    case "event.task.done":
      return { kind: "done" }
    case "event.task.error":
      return { kind: "error", error: String(p.error ?? "unknown error") }
    case "event.model.error":
      // 模型服务异常（引擎将自动重试）：非终态瞬时提示，任务继续
      return {
        kind: "model_error",
        error: String(p.error ?? "模型服务异常"),
        retry: typeof p.retry === "number" ? p.retry : undefined,
        maxRetry: typeof p.maxRetry === "number" ? p.maxRetry : undefined,
      }
    default:
      return null
  }
}

