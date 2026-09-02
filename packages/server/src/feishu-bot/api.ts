/**
 * 飞书开放平台 REST API 封装（机器人桥接用）：
 * tenant_access_token 获取与缓存、消息发送/撤回、图片资源下载、会话信息查询。
 * 全部依赖注入（fetch 可伪造），不依赖官方 SDK。
 */
import { hmacHex } from "../core/base/paths"
import { feishuFetch } from "./tls"

export const FEISHU_BASE = "https://open.feishu.cn"

export interface FeishuApiOptions {
  appId: string
  appSecret: string
  /** 注入的 HTTP 客户端（默认全局 fetch）。 */
  fetchImpl?: (url: string, init: RequestInit) => Promise<{ ok: boolean; status: number; json(): Promise<unknown>; arrayBuffer(): Promise<ArrayBuffer> }>
  /** 时钟（token 过期判断用，可注入）。 */
  clock?: () => number
}

export interface FeishuApiLike {
  /** 获取（并缓存）tenant_access_token。 */
  getTenantToken(): Promise<string>
  /** 发送消息；返回 message_id。 */
  sendMessage(opts: { receiveId: string; receiveIdType: "chat_id" | "open_id"; msgType: string; content: unknown; uuid?: string }): Promise<string>
  /** 回复指定消息（引用气泡）；返回 message_id。 */
  replyMessage(messageId: string, msgType: string, content: unknown): Promise<string>
  /** 撤回自己发送的消息（失败返回 false，不抛错——清理中间消息为尽力而为）。 */
  deleteMessage(messageId: string): Promise<boolean>
  /** 给消息添加表情反应（如「⌨️」）；返回 reaction_id，失败返回 null（飞书无 typing 接口，以表情反应模拟「正在输入」）。 */
  addMessageReaction(messageId: string, emojiType: string): Promise<string | null>
  /** 删除消息表情反应（输出完成后撤回「正在输入」表情；失败返回 false 不抛错）。 */
  deleteMessageReaction(messageId: string, reactionId: string): Promise<boolean>
  /** 下载消息资源（图片等）；失败抛错。 */
  downloadResource(messageId: string, fileKey: string, type: string): Promise<Uint8Array>
  /** 上传图片（msgType="image" 消息用）；返回 image_key。 */
  uploadImage(data: Uint8Array, mime: string, fileName?: string): Promise<string>
  /** 查询会话信息（群名/单聊对象名）；失败返回 null。 */
  getChatName(chatId: string): Promise<string | null>
}

type FetchLike = NonNullable<FeishuApiOptions["fetchImpl"]>

function textOf(v: unknown): string {
  if (typeof v === "string") return v
  if (v && typeof v === "object") return JSON.stringify(v)
  return String(v)
}

const DEFAULT_HEADERS = { "Content-Type": "application/json; charset=utf-8", "User-Agent": "gebai-feishu-bot/1.0" }

export function createFeishuApi(opts: FeishuApiOptions): FeishuApiLike {
  const appId = opts.appId
  const appSecret = opts.appSecret
  const clock = opts.clock ?? Date.now
  const fetchImpl: FetchLike = opts.fetchImpl ?? (async (url, init) => {
    const res = await feishuFetch(url, init)
    return {
      ok: res.ok,
      status: res.status,
      json: () => res.json(),
      arrayBuffer: () => res.arrayBuffer(),
    }
  })

  let tokenCache: { token: string; expiresAt: number } | null = null
  let tokenPromise: Promise<string> | null = null

  /** POST 统一入口：携带 tenant token，解析飞书业务码。 */
  async function request<T>(path: string, init: RequestInit, expectedCode = 0): Promise<T> {
    const res = await fetchImpl(`${FEISHU_BASE}${path}`, init)
    let body: Record<string, unknown>
    try {
      body = (await res.json()) as Record<string, unknown>
    } catch {
      throw new Error(`飞书 API ${init.method ?? "GET"} ${path} 响应非 JSON (HTTP ${res.status})`)
    }
    if (body.code !== expectedCode) {
      throw new Error(`飞书 API ${init.method ?? "GET"} ${path}: code=${textOf(body.code)} ${textOf(body.msg)}`)
    }
    return body as unknown as T
  }

  async function getTenantToken(): Promise<string> {
    const now = clock()
    if (tokenCache && tokenCache.expiresAt > now + 60_000) return tokenCache.token
    if (tokenPromise) return tokenPromise
    tokenPromise = (async () => {
      const body = await request<{ tenant_access_token: string; expire: number }>("/open-apis/auth/v3/tenant_access_token/internal", {
        method: "POST",
        headers: DEFAULT_HEADERS,
        body: JSON.stringify({ app_id: appId, app_secret: appSecret }),
      })
      // expire 单位秒；提前 60s 过期
      tokenCache = { token: body.tenant_access_token, expiresAt: now + Math.max(60, body.expire - 60) * 1000 }
      return body.tenant_access_token
    })().finally(() => {
      tokenPromise = null
    })
    return tokenPromise
  }

  async function sendMessage(opts: { receiveId: string; receiveIdType: "chat_id" | "open_id"; msgType: string; content: unknown; uuid?: string }): Promise<string> {
    const token = await getTenantToken()
    const query = `receive_id_type=${opts.receiveIdType}`
    const body = await request<{ data: { message_id: string } }>(
      `/open-apis/im/v1/messages?${query}`,
      {
        method: "POST",
        headers: { ...DEFAULT_HEADERS, Authorization: `Bearer ${token}` },
        body: JSON.stringify({
          receive_id: opts.receiveId,
          msg_type: opts.msgType,
          content: typeof opts.content === "string" ? opts.content : JSON.stringify(opts.content),
          uuid: opts.uuid,
        }),
      },
    )
    return body.data.message_id
  }

  /** 回复指定消息（引用气泡）：POST /im/v1/messages/{id}/reply。 */
  async function replyMessage(messageId: string, msgType: string, content: unknown): Promise<string> {
    const token = await getTenantToken()
    const body = await request<{ data: { message_id: string } }>(
      `/open-apis/im/v1/messages/${encodeURIComponent(messageId)}/reply`,
      {
        method: "POST",
        headers: { ...DEFAULT_HEADERS, Authorization: `Bearer ${token}` },
        body: JSON.stringify({
          msg_type: msgType,
          content: typeof content === "string" ? content : JSON.stringify(content),
        }),
      },
    )
    return body.data.message_id
  }

  async function deleteMessage(messageId: string): Promise<boolean> {
    try {
      const token = await getTenantToken()
      await request(`/open-apis/im/v1/messages/${encodeURIComponent(messageId)}`, {
        method: "DELETE",
        headers: { ...DEFAULT_HEADERS, Authorization: `Bearer ${token}` },
      })
      return true
    } catch {
      return false
    }
  }

  /** 给消息添加表情反应（如「⌨️」）；返回 reaction_id，失败返回 null（不抛错——「正在输入」提示为尽力而为）。 */
  async function addMessageReaction(messageId: string, emojiType: string): Promise<string | null> {
    try {
      const token = await getTenantToken()
      const body = await request<{ data: { reaction_id: string } }>(
        `/open-apis/im/v1/messages/${encodeURIComponent(messageId)}/reactions`,
        {
          method: "POST",
          headers: { ...DEFAULT_HEADERS, Authorization: `Bearer ${token}` },
          body: JSON.stringify({ reaction_type: { emoji_type: emojiType } }),
        },
      )
      return body.data.reaction_id ?? null
    } catch {
      return null
    }
  }

  /** 删除消息表情反应（输出完成后撤回「正在输入」表情；失败返回 false 不抛错）。 */
  async function deleteMessageReaction(messageId: string, reactionId: string): Promise<boolean> {
    try {
      const token = await getTenantToken()
      await request(`/open-apis/im/v1/messages/${encodeURIComponent(messageId)}/reactions/${encodeURIComponent(reactionId)}`, {
        method: "DELETE",
        headers: { ...DEFAULT_HEADERS, Authorization: `Bearer ${token}` },
      })
      return true
    } catch {
      return false
    }
  }

  async function downloadResource(messageId: string, fileKey: string, type: string): Promise<Uint8Array> {
    const token = await getTenantToken()
    const res = await fetchImpl(
      `${FEISHU_BASE}/open-apis/im/v1/messages/${encodeURIComponent(messageId)}/resources/${encodeURIComponent(fileKey)}?type=${encodeURIComponent(type)}`,
      { method: "GET", headers: { Authorization: `Bearer ${token}` } },
    )
    const buf = new Uint8Array(await res.arrayBuffer())
    // 飞书错误响应为 JSON（可能 HTTP 200 + 业务码非 0）：以 `{` 开头时探测并抛错
    if (buf.length > 0 && buf[0] === 0x7b /* { */) {
      try {
        const obj = JSON.parse(new TextDecoder().decode(buf)) as { code?: unknown; msg?: unknown }
        if (obj && typeof obj === "object" && obj.code !== 0) {
          throw new Error(`飞书资源下载失败: code=${String(obj.code)} ${String(obj.msg ?? "")}`.trim())
        }
      } catch (err) {
        if (err instanceof Error && err.message.startsWith("飞书资源下载失败")) throw err
        /* 非 JSON 资源体：按二进制继续 */
      }
    }
    if (!res.ok) throw new Error(`飞书资源下载失败: HTTP ${res.status}`)
    return buf
  }

  async function getChatName(chatId: string): Promise<string | null> {
    try {
      const token = await getTenantToken()
      const body = await request<{ data: { name?: string } }>(
        `/open-apis/im/v1/chats/${encodeURIComponent(chatId)}`,
        { method: "GET", headers: { ...DEFAULT_HEADERS, Authorization: `Bearer ${token}` } },
      )
      const name = body.data?.name
      return name && name.trim() ? name.trim() : null
    } catch {
      return null
    }
  }

  /** 上传图片（multipart/form-data，image_type=message）：返回 image_key 供 msgType="image" 消息使用。 */
  async function uploadImage(data: Uint8Array, mime: string, fileName = "image.png"): Promise<string> {
    const token = await getTenantToken()
    const fd = new FormData()
    fd.append("image_type", "message")
    fd.append("image", new Blob([data], { type: mime }), fileName)
    const body = await request<{ data: { image_key: string } }>(
      "/open-apis/im/v1/images",
      {
        method: "POST",
        // multipart 边界由 fetch 自动生成：不能带手动 Content-Type
        headers: { "User-Agent": DEFAULT_HEADERS["User-Agent"] as string, Authorization: `Bearer ${token}` },
        body: fd,
      },
    )
    const key = body.data?.image_key
    if (!key) throw new Error("飞书图片上传响应缺少 image_key")
    return key
  }

  return { getTenantToken, sendMessage, replyMessage, deleteMessage, addMessageReaction, deleteMessageReaction, downloadResource, uploadImage, getChatName }
}

/** 事件订阅回调的飞书签名校验（Webhook 模式预留；本期长连接不使用）。 */
export function verifyFeishuSignature(secret: string, timestamp: string, nonce: string, body: string, signature: string): boolean {
  const digest = hmacHex(secret, `${timestamp}${nonce}${body}`)
  return digest === signature
}
