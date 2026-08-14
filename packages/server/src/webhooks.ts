import { mkdir, readFile, writeFile } from "node:fs/promises"
import { dirname, join } from "node:path"
import type { AgentEvent } from "@gebai/sdk"
import type { EventBus } from "./core/event-bus"
import { hmacHex } from "./core/paths"
import { hostBlockReason } from "./core/ip"

export interface WebhookConfig {
  id: string
  url: string
  /** 订阅的事件类型白名单；为空数组时订阅全部事件。 */
  events: string[]
  /** HMAC 签名密钥（可选）。 */
  secret?: string
  /** 注册者用户 id；多用户模式下事件按会话归属过滤。 */
  userId?: string
  createdAt: number
}

export interface WebhookManagerOptions {
  home: string
  /**
   * 注入的 HTTP 发送函数（默认全局 fetch；测试注入 fake）。
   * 返回 { ok, status }，ok=false 或抛错触发重试。
   */
  deliver?: (url: string, body: unknown, headers: Record<string, string>) => Promise<{ ok: boolean; status: number }>
  /** 失败重试退避基数（毫秒，缺省 500；测试注入小值加速重试用例）。 */
  retryBaseMs?: number
}

const DEFAULT_EVENTS = ["event.task.done", "event.approval.request", "event.task.error"]
const MAX_RETRIES = 3

/** 是否放行内网/私网 Webhook 地址（默认拒绝回环与链路本地；SSRF 防护）。 */
const ALLOW_PRIVATE = process.env.GEBAI_WEBHOOK_ALLOW_PRIVATE === "true"

/** Webhook URL 校验：必须 http(s)，默认拒绝回环/链路本地/云元数据地址（SSRF 防护）。
 * 主机名判定统一走 `core/ip.ts`（覆盖 IPv4-mapped IPv6、尾点 FQDN、规范化 IPv4 字面量等
 * 绕过形式）；私网（10.x/192.168.x/172.16-31.x/ULA）默认放行——内网回调是 webhook 的
 * 常见合法场景，需全私网拒绝时使用 `GEBAI_WEBHOOK_ALLOW_PRIVATE=true` 之外另行网关管控。 */
function checkWebhookUrl(raw: string): void {
  let url: URL
  try {
    url = new URL(raw)
  } catch {
    throw new Error(`invalid webhook url: ${raw}`)
  }
  if (url.protocol !== "http:" && url.protocol !== "https:") throw new Error(`invalid webhook url: ${raw}`)
  if (ALLOW_PRIVATE) return
  const reason = hostBlockReason(url.hostname, { blockPrivate: false })
  if (reason) throw new Error(`webhook url not allowed: ${raw}`)
}

type DeliverFn = (url: string, body: unknown, headers: Record<string, string>) => Promise<{ ok: boolean; status: number }>

/** 默认投递：全局 fetch，超时 10s。 */
const defaultDeliver: DeliverFn = async (url, body, headers) => {
  const res = await fetch(url, {
    method: "POST",
    headers,
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(10000),
  })
  return { ok: res.ok, status: res.status }
}

export class WebhookManager {
  private configs: WebhookConfig[] = []
  private path: string
  private deliver: DeliverFn
  private retryBaseMs: number
  private unsub?: () => void

  constructor(opts: WebhookManagerOptions) {
    this.path = join(opts.home, "webhooks.json")
    this.deliver = opts.deliver || defaultDeliver
    this.retryBaseMs = opts.retryBaseMs ?? 500
  }

  /** 加载持久化配置并订阅事件总线（幂等：重复 start 先 stop）。 */
  async start(events: EventBus): Promise<void> {
    this.stop()
    try {
      this.configs = JSON.parse(await readFile(this.path, "utf8")) as WebhookConfig[]
    } catch {
      this.configs = []
    }
    this.unsub = events.subscribe((ev) => {
      void this.dispatch(ev)
    })
  }

  stop(): void {
    this.unsub?.()
    this.unsub = undefined
  }

  /** 列出 webhook；userId 非空时仅返回该用户注册的（管理员传 undefined 看全部）。 */
  list(userId?: string): WebhookConfig[] {
    const out = userId ? this.configs.filter((c) => c.userId === userId) : this.configs
    return out.map((c) => ({ ...c, secret: c.secret ? "***" : undefined }))
  }

  /** 注册 webhook；返回脱敏后的配置（含 id）。 */
  async register(input: { url: string; events?: string[]; secret?: string }, userId?: string): Promise<WebhookConfig> {
    const url = input.url.trim()
    checkWebhookUrl(url)
    const cfg: WebhookConfig = {
      id: crypto.randomUUID().replace(/-/g, ""),
      url,
      events: input.events?.length ? input.events : [...DEFAULT_EVENTS],
      secret: input.secret || undefined,
      userId,
      createdAt: Date.now(),
    }
    this.configs.push(cfg)
    await this.save()
    return { ...cfg, secret: cfg.secret ? "***" : undefined }
  }

  async remove(id: string): Promise<boolean> {
    const before = this.configs.length
    this.configs = this.configs.filter((c) => c.id !== id)
    if (this.configs.length === before) return false
    await this.save()
    return true
  }

  private async save(): Promise<void> {
    await mkdir(dirname(this.path), { recursive: true })
    await writeFile(this.path, JSON.stringify(this.configs, null, 2))
  }

  private async dispatch(ev: AgentEvent): Promise<void> {
    for (const cfg of this.configs) {
      if (cfg.events.length && !cfg.events.includes(ev.type)) continue
      if (cfg.userId) {
        // 多用户隔离：事件会话归属校验由 app 层注入的 ownerOf 回调完成
        const owner = await this.ownerOf?.(ev.sessionId)
        if (owner !== null && owner !== undefined && owner !== cfg.userId) continue
      }
      void this.deliverWithRetry(cfg, ev)
    }
  }

  /** 会话归属回调（app 层注入 store.load 查询；无则跳过归属过滤）。 */
  ownerOf?: (sessionId: string) => Promise<string | null>

  private async deliverWithRetry(cfg: WebhookConfig, ev: AgentEvent): Promise<void> {
    const body = { type: ev.type, sessionId: ev.sessionId, payload: ev.payload, timestamp: ev.timestamp }
    const raw = JSON.stringify(body)
    const headers: Record<string, string> = { "Content-Type": "application/json", "User-Agent": "gebai-webhook/1.0" }
    if (cfg.secret) headers["X-Gebai-Signature"] = `sha256=${hmacHex(cfg.secret, raw)}`
    for (let attempt = 1; attempt <= MAX_RETRIES; attempt++) {
      try {
        const res = await this.deliver(cfg.url, body, headers)
        if (res.ok) return
      } catch {
        /* 网络错误：按退避重试 */
      }
      if (attempt < MAX_RETRIES) await new Promise((r) => setTimeout(r, this.retryBaseMs * 2 ** (attempt - 1)))
    }
  }
}
