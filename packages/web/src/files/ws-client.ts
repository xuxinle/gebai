/**
 * 文件工作台 · 通用 WS 请求客户端（终端与语言服务器共用）。
 *
 * 两者形态相同：请求按 id 关联应答、服务端还会主动推送（终端输出 / 诊断）、
 * 连接建立后先补认证（服务模式 WS 无法带 Header）。抽成一份，避免两条链路各自走样。
 *
 * 连接是**惰性**的：第一次 request/send 才建连；断线后由调用方决定是否重建（closed 后不再重连）。
 */
import { appWsUrl, readToken as readContractToken } from "@gebai/sdk"

/** 认证令牌：经**凭证契约**读取（默认 localStorage `gebai.auth.token`，部署方可整体替换载体）。 */
export function readAuthToken(): string | null {
  return readContractToken()
}

export interface WsReply {
  ok: boolean
  payload?: Record<string, unknown>
  error?: string
}

export interface WorkbenchSocketOptions {
  /** 错误文案前缀（如「终端」/「语言服务器」）：同一条通道被两处复用时便于定位。 */
  label: string
  /** 每次请求附带的上下文（工作台会话 id；缺省不带）。 */
  context?: () => string | undefined
  /** 请求超时（毫秒，默认 15s）。 */
  timeoutMs?: number
  /** 建连超时（毫秒，默认 8s）。 */
  connectTimeoutMs?: number
}

/** WS 客户端：请求按 id 关联应答，推送按消息类型分发。 */
export class WorkbenchSocket {
  private ws: WebSocket | null = null
  private connecting: Promise<void> | null = null
  private seq = 0
  private pending = new Map<string, (r: WsReply) => void>()
  private pushHandlers = new Map<string, Set<(payload: Record<string, unknown>) => void>>()
  private closers = new Set<(reason: string) => void>()
  private closed = false

  constructor(private readonly opts: WorkbenchSocketOptions) {}

  private get label(): string {
    return this.opts.label
  }

  private url(): string {
    return appWsUrl("/ws")
  }

  on(type: string, cb: (payload: Record<string, unknown>) => void): () => void {
    let set = this.pushHandlers.get(type)
    if (!set) {
      set = new Set()
      this.pushHandlers.set(type, set)
    }
    set.add(cb)
    return () => set.delete(cb)
  }

  onClose(cb: (reason: string) => void): () => void {
    this.closers.add(cb)
    return () => this.closers.delete(cb)
  }

  /** 建立连接（并发调用共享同一次尝试）。 */
  ensureOpen(): Promise<void> {
    if (this.ws?.readyState === WebSocket.OPEN) return Promise.resolve()
    if (this.connecting) return this.connecting
    this.connecting = new Promise<void>((resolve, reject) => {
      let settled = false
      const ws = new WebSocket(this.url())
      const done = (err?: Error) => {
        if (settled) return
        settled = true
        this.connecting = null
        if (err) reject(err)
        else resolve()
      }
      const timer = setTimeout(() => {
        try {
          ws.close()
        } catch {
          /* 忽略 */
        }
        done(new Error(`${this.label}通道连接超时`))
      }, this.opts.connectTimeoutMs ?? 8000)
      ws.onopen = () => {
        clearTimeout(timer)
        this.ws = ws
        // 服务模式：WS 无法带 Header，连接后先认证（服务端按到达顺序串行处理）
        const token = readAuthToken()
        if (token) ws.send(JSON.stringify({ type: "auth.login", payload: { token } }))
        done()
      }
      ws.onmessage = (ev) => this.dispatch(String(ev.data))
      ws.onerror = () => done(new Error(`${this.label}通道连接失败`))
      ws.onclose = () => {
        clearTimeout(timer)
        if (this.ws === ws) this.ws = null
        done(new Error(`${this.label}通道已断开`))
        for (const cb of this.closers) cb("closed")
      }
    })
    return this.connecting
  }

  private dispatch(raw: string): void {
    let msg: { type?: string; id?: string; ok?: boolean; payload?: Record<string, unknown>; error?: string }
    try {
      msg = JSON.parse(raw)
    } catch {
      return
    }
    if (!msg.type) return
    if (msg.id) {
      const cb = this.pending.get(msg.id)
      if (cb) {
        this.pending.delete(msg.id)
        cb({ ok: msg.ok !== false, payload: msg.payload, error: msg.error })
        return
      }
    }
    const set = this.pushHandlers.get(msg.type)
    if (!set) return
    const payload = msg.payload ?? {}
    for (const cb of set) {
      try {
        cb(payload)
      } catch {
        /* 单个处理器异常不影响其它订阅 */
      }
    }
  }

  /** 发请求并等应答（服务端回执带同一 id）。 */
  async request(type: string, payload: Record<string, unknown> = {}): Promise<WsReply> {
    try {
      await this.ensureOpen()
    } catch (err) {
      return { ok: false, error: (err as Error).message }
    }
    const ws = this.ws
    if (!ws || ws.readyState !== WebSocket.OPEN) return { ok: false, error: `${this.label}通道未连接` }
    const id = `r${++this.seq}`
    const session = this.opts.context?.()
    const body = session ? { ...payload, session } : payload
    return await new Promise<WsReply>((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        resolve({ ok: false, error: `${this.label}请求超时` })
      }, this.opts.timeoutMs ?? 15000)
      this.pending.set(id, (r) => {
        clearTimeout(timer)
        resolve(r)
      })
      try {
        ws.send(JSON.stringify({ type, id, payload: body }))
      } catch (err) {
        clearTimeout(timer)
        this.pending.delete(id)
        resolve({ ok: false, error: (err as Error).message })
      }
    })
  }

  /** 单向发送（高频、不关心应答的消息）。 */
  send(type: string, payload: Record<string, unknown> = {}): void {
    const ws = this.ws
    if (!ws || ws.readyState !== WebSocket.OPEN) return
    try {
      ws.send(JSON.stringify({ type, payload }))
    } catch {
      /* 连接刚断：下一条会重新走 ensureOpen */
    }
  }

  close(): void {
    this.closed = true
    try {
      this.ws?.close()
    } catch {
      /* 忽略 */
    }
    this.ws = null
  }

  get isClosed(): boolean {
    return this.closed
  }
}
