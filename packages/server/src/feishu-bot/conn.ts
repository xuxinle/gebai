/**
 * 飞书长连接客户端：endpoint 发现 → WebSocket 连接 → 心跳/事件帧分发 → 自动重连。
 * 协议参照 lark_oapi 官方 SDK（新版 protobuf 帧协议）。依赖注入（fetch/WebSocket/时钟）可单测。
 */
import { FEISHU_BASE } from "./api"
import { buildAckFrame, buildPingFrame, FRAME_CONTROL, FRAME_DATA, FrameAssembler, parseClientConfig, parseDataFrame, parseEventPayload, type DataFrameInfo } from "./protocol"
import { feishuFetch, feishuWsOptions } from "./tls"

/** WebSocket 客户端的最小抽象（Bun 内置 WebSocket 满足该形状）。 */
export interface WsLike {
  send(data: string | ArrayBuffer | Uint8Array): void
  close(code?: number, reason?: string): void
  readonly readyState: number
  onopen?: () => void
  onmessage?: (ev: { data: unknown }) => void
  onclose?: (ev: { code?: number; reason?: string }) => void
  onerror?: (ev: unknown) => void
}

export interface WsFactory {
  connect(url: string): WsLike
}

/** Bun 运行时 WebSocket 工厂（默认实现）。 */
export const bunWsFactory: WsFactory = {
  connect(url: string): WsLike {
    const ws = new WebSocket(url, feishuWsOptions())
    ws.binaryType = "arraybuffer"
    return ws as unknown as WsLike
  },
}

/** 事件回调（payload 为 schema 2.0 事件 JSON）。
 *  返回值：普通事件不返回（void，立即回 `{"code":200}` ACK）；卡片回调事件（card.action.trigger）
 *  返回卡片响应体（toast/card）——ACK 封官方信封 `{"code":200,"data":"<base64(响应JSON)>"}`（lark_oapi ws/client.py 同构）。
 *  返回 Promise 时 conn 等待其落定后再回 ACK（卡片处理快，3 秒时限内）。 */
export type FeishuEventHandler = (
  event: Record<string, unknown>,
) => void | Record<string, unknown> | Promise<void | Record<string, unknown>>

/** 卡片交互回调（type="card" 帧，card.action.trigger）：返回响应 JSON（可含 card 更新/toast；缺省 `{}`）。 */
export type FeishuCardActionHandler = (payload: Record<string, unknown>) => Record<string, unknown> | Promise<Record<string, unknown>>

export interface FeishuConnOptions {
  appId: string
  appSecret: string
  onEvent: FeishuEventHandler
  /** 卡片交互回调（交互式消息卡片按钮点击）：响应 JSON 回填 ACK 帧（卡片更新/toast）。 */
  onCardAction?: FeishuCardActionHandler
  /** 连接状态日志（默认 console.log）。 */
  log?: (msg: string) => void
  fetchImpl?: (url: string, init: RequestInit) => Promise<{ ok: boolean; status: number; json(): Promise<unknown> }>
  wsFactory?: WsFactory
  clock?: () => number
  /** 心跳/重连间隔覆盖（测试用）。 */
  intervals?: { reconnectNonce?: number; reconnectInterval?: number; pingInterval?: number; handshakeTimeoutMs?: number; fastRetryMs?: number }
}

const DEFAULTS = { reconnectInterval: 120_000, pingInterval: 120_000, handshakeTimeoutMs: 15_000, fastRetryMs: 5_000 }

/** 重连失败后的快速重试次数（网络闪断秒级恢复，之后才按服务端 ReconnectInterval）。 */
const FAST_RETRY_ATTEMPTS = 3

export class FeishuConn {
  private ws: WsLike | null = null
  private serviceId = 0
  private pingTimer: ReturnType<typeof setTimeout> | null = null
  /** 最近收到服务端帧的时间（任意帧均视为链路活跃，静默死链检测见 startPing）。 */
  private lastAliveAt = 0
  /** 本次连接建立时间（断连日志统计存活时长）。 */
  private connectedAt = 0
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private stopped = false
  private assembler: FrameAssembler
  private log: (msg: string) => void
  private cfg: { reconnectCount: number; reconnectInterval: number; reconnectNonce: number; pingInterval: number; handshakeTimeoutMs: number; fastRetryMs: number }
  private fetchImpl: NonNullable<FeishuConnOptions["fetchImpl"]>
  private wsFactory: WsFactory
  private clock: () => number

  constructor(private opts: FeishuConnOptions) {
    this.log = opts.log ?? ((m) => console.log(`[feishu-conn] ${m}`))
    this.clock = opts.clock ?? Date.now
    this.assembler = new FrameAssembler(this.clock)
    this.fetchImpl =
      opts.fetchImpl ??
      (async (url, init) => {
        const res = await feishuFetch(url, init)
        return { ok: res.ok, status: res.status, json: () => res.json() }
      })
    this.wsFactory = opts.wsFactory ?? bunWsFactory
    const iv = opts.intervals ?? {}
    this.cfg = {
      reconnectCount: -1,
      reconnectInterval: iv.reconnectInterval ?? DEFAULTS.reconnectInterval,
      reconnectNonce: iv.reconnectNonce ?? 0,
      pingInterval: iv.pingInterval ?? DEFAULTS.pingInterval,
      handshakeTimeoutMs: iv.handshakeTimeoutMs ?? DEFAULTS.handshakeTimeoutMs,
      fastRetryMs: iv.fastRetryMs ?? DEFAULTS.fastRetryMs,
    }
  }

  /** 启动：endpoint 发现 + 连接（异步返回后连接已建立或进入重连流程）。 */
  async start(): Promise<void> {
    this.stopped = false
    try {
      await this.connect()
    } catch (err) {
      this.log(`connect failed: ${String((err as Error).message || err)}`)
      this.scheduleReconnect(0)
    }
  }

  stop(): void {
    this.stopped = true
    if (this.pingTimer) clearInterval(this.pingTimer)
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer)
    this.pingTimer = null
    this.reconnectTimer = null
    this.ws?.close(1000, "shutdown")
    this.ws = null
  }

  private async discoverEndpoint(): Promise<string> {
    const res = await this.fetchImpl(`${FEISHU_BASE}/callback/ws/endpoint`, {
      method: "POST",
      headers: { "Content-Type": "application/json; charset=utf-8", "User-Agent": "gebai-feishu-bot/1.0", locale: "zh" },
      body: JSON.stringify({ AppID: this.opts.appId, AppSecret: this.opts.appSecret }),
    })
    let body: Record<string, unknown>
    try {
      body = (await res.json()) as Record<string, unknown>
    } catch {
      throw new Error(`endpoint 响应非 JSON (HTTP ${res.status})`)
    }
    if (body.code !== 0) {
      throw new Error(`endpoint 发现失败: code=${String(body.code)} ${String(body.msg ?? "")}`)
    }
    const data = (body.data ?? {}) as { URL?: string; ClientConfig?: { ReconnectCount?: number; ReconnectInterval?: number; ReconnectNonce?: number; PingInterval?: number } }
    if (!data.URL) throw new Error("endpoint 响应缺少 URL")
    const conf = data.ClientConfig
    if (conf) {
      if (typeof conf.ReconnectCount === "number") this.cfg.reconnectCount = conf.ReconnectCount
      if (typeof conf.ReconnectInterval === "number") this.cfg.reconnectInterval = conf.ReconnectInterval * 1000
      if (typeof conf.ReconnectNonce === "number") this.cfg.reconnectNonce = conf.ReconnectNonce * 1000
      if (typeof conf.PingInterval === "number") this.cfg.pingInterval = conf.PingInterval * 1000
    }
    return data.URL
  }

  private async connect(): Promise<void> {
    const url = await this.discoverEndpoint()
    const q = new URL(url).searchParams
    const deviceId = q.get("device_id") ?? ""
    const serviceId = Number(q.get("service_id") ?? "0")
    this.serviceId = serviceId
    this.log(`connecting (device_id=${deviceId.slice(0, 8)}…, service=${serviceId})`)
    const ws = this.wsFactory.connect(url)
    this.ws = ws
    // 握手超时：防火墙/代理黑洞下 open/error/close 均不触发，无超时会永久挂起且不再重连
    await new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => {
        cleanup()
        try { ws.close() } catch { /* 已死 */ }
        reject(new Error(`websocket 握手超时 (${Math.round(this.cfg.handshakeTimeoutMs / 1000)}s)`))
      }, this.cfg.handshakeTimeoutMs)
      const onOpen = () => {
        cleanup()
        resolve()
      }
      const onError = () => {
        cleanup()
        ws.close()
        reject(new Error("websocket 握手失败"))
      }
      const onClose = () => {
        cleanup()
        reject(new Error("websocket 连接关闭"))
      }
      const cleanup = () => {
        clearTimeout(timer)
        ws.onopen = undefined
        ws.onerror = undefined
        ws.onclose = undefined
      }
      ws.onopen = onOpen
      ws.onerror = onError
      ws.onclose = onClose
    })
    this.log("connected")
    this.connectedAt = Date.now()
    ws.onmessage = (ev) => this.handleMessage(ev.data)
    ws.onclose = (ev) => this.handleClose(ev.code, ev.reason)
    ws.onerror = () => {
      /* 错误由 onclose 触发重连 */
    }
    this.startPing()
  }

  private startPing(): void {
    if (this.pingTimer) clearInterval(this.pingTimer)
    this.lastAliveAt = Date.now()
    this.pingTimer = setInterval(() => {
      if (this.ws && this.ws.readyState === 1) {
        // 静默死链检测：NAT 超时/网络切换/半开 TCP 下 onclose 不触发、ping 写入死 socket 不报错，
        // 连接实际已死却"在线"——连续 pingInterval*3 无任何服务端帧主动断开走重连
        if (Date.now() - this.lastAliveAt > this.cfg.pingInterval * 3) {
          this.forceReconnect(`server silent for ${Math.round((Date.now() - this.lastAliveAt) / 1000)}s (>3x ping interval), forcing reconnect`)
          return
        }
        try {
          this.ws.send(buildPingFrame(this.serviceId))
        } catch {
          /* 发送失败由 onclose 处理 */
        }
      }
    }, this.cfg.pingInterval)
  }

  /** 判死主动重连：不依赖 onclose 回调（半开 socket close 可能无回调），直接清理并排重连（handleClose 幂等跳过）。 */
  private forceReconnect(reason: string): void {
    const ws = this.ws
    this.ws = null
    if (this.pingTimer) {
      clearInterval(this.pingTimer)
      this.pingTimer = null
    }
    this.log(reason)
    try { ws?.close() } catch { /* 已死 */ }
    if (this.stopped) return
    this.scheduleReconnect(this.cfg.reconnectNonce)
  }

  private handleMessage(data: unknown): void {
    let buf: Uint8Array
    if (typeof data === "string") {
      buf = new TextEncoder().encode(data)
    } else if (data instanceof Uint8Array) {
      buf = data
    } else if (data instanceof ArrayBuffer) {
      buf = new Uint8Array(data)
    } else if (typeof Blob !== "undefined" && data instanceof Blob) {
      // 同步路径无法 await；Bun 客户端默认 arraybuffer，此处兜底忽略
      return
    } else {
      return
    }
    // 任何服务端帧（pong/事件/配置推送）都证明链路活着——不只 pong，防 pong 丢失被误判死链
    this.lastAliveAt = Date.now()
    try {
      this.dispatch(buf)
    } catch (err) {
      this.log(`handle frame failed: ${String((err as Error).message || err)}`)
    }
  }

  private dispatch(buf: Uint8Array): void {
    const info = parseDataFrame(buf)
    if (info.frame.method === FRAME_CONTROL) {
      if (info.type === "pong" && info.payload.length > 0) {
        const conf = parseClientConfig(info.payload)
        if (conf) this.applyConfig(conf)
      }
      return
    }
    if (info.frame.method !== FRAME_DATA) return
    // 卡片交互帧（type="card"，card.action.trigger）：解析 → 回调 → 回 ACK（响应携带卡片回调返回体）
    if (info.type === "card") {
      // 送达诊断：卡片按钮转圈不消时据此判断回调是否到达（未到达=开发者后台未订阅 card.action.trigger）
      this.log("card action frame received")
      if (!this.opts.onCardAction) this.log("card action dropped: no handler")
      void this.handleCardFrame(buf, info)
      return
    }
    // 事件帧（im.message.receive_v1 / card.action.trigger 等）：回 ACK（回调返回响应体时封 data 信封）
    if (info.type !== "event") return
    const payload = this.assembler.push(info)
    if (!payload) return
    const event = parseEventPayload(payload)
    if (!event) return
    const start = this.clock()
    let result: void | Record<string, unknown> | Promise<void | Record<string, unknown>>
    try {
      result = this.opts.onEvent(event)
    } catch {
      /* 事件处理异常仍回 200 ACK，避免飞书重推 */
      this.sendEventAck(buf, this.elapsedMs(start))
      return
    }
    // 异步回调（卡片事件）：等待响应体回 ACK（普通消息事件 fire-and-forget 立即 ACK）
    if (result && typeof (result as Promise<unknown>).then === "function") {
      void (result as Promise<void | Record<string, unknown>>).then(
        (resp) => this.sendEventAck(buf, this.elapsedMs(start), resp),
        () => this.sendEventAck(buf, this.elapsedMs(start)),
      )
      return
    }
    this.sendEventAck(buf, this.elapsedMs(start), result as void | Record<string, unknown>)
  }

  private elapsedMs(start: number): number {
    return this.clock() - start
  }

  /** 事件 ACK：回调返回响应体（卡片回调）时封官方信封 `{"code":200,"data":"<base64(JSON)>"}`。 */
  private sendEventAck(buf: Uint8Array, bizMs: number, resp?: Record<string, unknown> | void): void {
    const responseJson =
      resp && typeof resp === "object" ? `{"code":200,"data":${JSON.stringify(Buffer.from(JSON.stringify(resp)).toString("base64"))}}` : '{"code":200}'
    try {
      this.ws?.send(buildAckFrame(buf, bizMs, responseJson))
    } catch {
      /* ACK 发送失败忽略（连接关闭时） */
    }
  }

  /** 卡片交互帧：异步回调（bot 可能触发引擎决策），回填响应 JSON（默认 `{}`）；异常仍回空响应避免飞书重推。 */
  private async handleCardFrame(buf: Uint8Array, info: DataFrameInfo): Promise<void> {
    const payload = this.assembler.push(info)
    if (!payload) return
    const obj = parseEventPayload(payload)
    if (!obj) return
    const start = this.clock()
    let response: Record<string, unknown> = {}
    try {
      if (this.opts.onCardAction) response = await this.opts.onCardAction(obj)
    } catch {
      /* 处理异常回空响应（卡片不更新，业务侧兜底） */
    }
    const elapsed = this.clock() - start
    try {
      this.ws?.send(buildAckFrame(buf, elapsed, JSON.stringify(response)))
    } catch {
      /* ACK 发送失败忽略（连接关闭时） */
    }
  }

  private applyConfig(conf: { ReconnectCount?: number; ReconnectInterval?: number; ReconnectNonce?: number; PingInterval?: number }): void {
    if (typeof conf.ReconnectCount === "number") this.cfg.reconnectCount = conf.ReconnectCount
    if (typeof conf.ReconnectInterval === "number") this.cfg.reconnectInterval = conf.ReconnectInterval * 1000
    if (typeof conf.ReconnectNonce === "number") this.cfg.reconnectNonce = conf.ReconnectNonce * 1000
    if (typeof conf.PingInterval === "number") {
      this.cfg.pingInterval = conf.PingInterval * 1000
      this.startPing()
    }
  }

  private handleClose(code?: number, reason?: string): void {
    // forceReconnect 已置空 ws（主动判死路径），close 回调至此直接跳过，防双重排重连
    if (!this.ws) return
    this.ws = null
    if (this.pingTimer) {
      clearInterval(this.pingTimer)
      this.pingTimer = null
    }
    if (this.stopped) return
    const aliveSec = this.connectedAt ? Math.round((Date.now() - this.connectedAt) / 1000) : 0
    this.log(`connection closed (code=${code ?? "?"}${reason ? ` ${reason}` : ""}, alive ${aliveSec}s), reconnecting…`)
    this.scheduleReconnect(this.cfg.reconnectNonce)
  }

  private scheduleReconnect(delayMs: number, attempt = 0): void {
    if (this.stopped || this.reconnectTimer) return
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null
      void this.tryReconnect(attempt)
    }, delayMs)
  }

  private async tryReconnect(attempt: number): Promise<void> {
    if (this.stopped) return
    try {
      await this.connect()
      this.log(`reconnected (attempt ${attempt + 1})`)
    } catch (err) {
      this.log(`reconnect failed (attempt ${attempt + 1}): ${String((err as Error).message || err)}`)
      if (this.stopped) return
      const count = this.cfg.reconnectCount
      if (count >= 0 && attempt + 1 >= count) {
        // 达到服务端配置上限后不永久停摆（进程仍"健康"存活、bot 失联无告警）：降频续试（60s 封顶退避）
        this.log("reconnect limit reached, retrying at reduced cadence")
        this.scheduleReconnect(60_000, 0)
        return
      }
      // 前几次快速重试（网络闪断秒级恢复，不干等服务端 ReconnectInterval——常下发 60s+），
      // 仍失败再按服务端间隔，避免持续轰炸网关
      const delay =
        attempt < FAST_RETRY_ATTEMPTS ? Math.min(this.cfg.reconnectInterval, this.cfg.fastRetryMs) : this.cfg.reconnectInterval
      this.scheduleReconnect(delay, attempt + 1)
    }
  }
}
