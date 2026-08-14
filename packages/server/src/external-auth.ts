import { createHmac, timingSafeEqual } from "node:crypto"
import type { ServerConfig } from "./core/config"

/**
 * 外部身份扩展点：网站（同源部署）把本地登录态作为用户身份兑换歌白令牌，免二次登录。
 * 验证器可插拔：配置 `GEBAI_EXTERNAL_AUTH_SECRET`（HMAC 验签）或 `GEBAI_EXTERNAL_AUTH_URL`（HTTP 回调），
 * 两者互斥（同设启动报错）。验证通过返回外部用户名，失败返回 null（统一 401，不泄露原因）。
 */

export interface ExternalCredential {
  /** 外部用户名（业务系统账号）。 */
  username: string
  /** 外部凭证（HMAC: "{exp}.{sig}"；回调: 业务系统自己的登录 token）。 */
  credential: string
}

export interface ExternalAuthProvider {
  readonly kind: "hmac" | "callback"
  /** 验证凭证并返回外部用户名（回调可覆盖）；失败/异常一律 null。 */
  verify(c: ExternalCredential): Promise<string | null>
}

/** 外部凭证有效窗口：签发时间距今超过该时长拒绝（防重放）。 */
const CREDENTIAL_TTL_MS = 10 * 60 * 1000
const CALLBACK_TIMEOUT_MS = 5000

/** HMAC 共享密钥验证器：credential = "{exp}.{sig}"，sig = HMAC-SHA256(secret, "{username}.{exp}")。 */
export class HmacExternalAuth implements ExternalAuthProvider {
  readonly kind = "hmac" as const
  constructor(private secret: string) {}

  async verify(c: ExternalCredential): Promise<string | null> {
    const [expStr, sig] = c.credential.split(".")
    const exp = Number(expStr)
    if (!Number.isFinite(exp) || !sig) return null
    if (exp < Date.now() - CREDENTIAL_TTL_MS || exp > Date.now() + CREDENTIAL_TTL_MS) return null
    const expected = createHmac("sha256", this.secret).update(`${c.username}.${exp}`).digest("hex")
    const a = Buffer.from(sig)
    const b = Buffer.from(expected)
    if (a.length !== b.length || !timingSafeEqual(a, b)) return null
    return c.username
  }
}

/** 可注入的 fetch 实现（标准签名，测试替身友好）。 */
export type FetchLike = (url: string, init?: RequestInit) => Promise<Response>

/** HTTP 回调验证器：把凭证转交业务系统自行校验（可复用其现有鉴权逻辑），响应 2xx + {"ok":true} 即通过。 */
export class CallbackExternalAuth implements ExternalAuthProvider {
  readonly kind = "callback" as const
  constructor(
    private url: string,
    private fetchImpl: FetchLike = fetch,
  ) {
    // 强制 HTTPS：明文 http 回调可被中间人伪造 {ok:true} 响应完全接管用户（localhost/127.0.0.1 放行本地调试）
    const u = new URL(url)
    if (u.protocol !== "https:" && u.hostname !== "localhost" && u.hostname !== "127.0.0.1" && u.hostname !== "::1") {
      throw new Error(`GEBAI_EXTERNAL_AUTH_URL 必须为 https: ${u.protocol}//${u.hostname}`)
    }
  }

  async verify(c: ExternalCredential): Promise<string | null> {
    try {
      const res = await this.fetchImpl(this.url, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: c.username, credential: c.credential }),
        signal: AbortSignal.timeout(CALLBACK_TIMEOUT_MS),
      })
      if (!res.ok) return null
      const body = (await res.json()) as { ok?: boolean; username?: string }
      if (!body || body.ok !== true) return null
      // 回调返回的用户名必须与请求一致：允许业务系统规范化（如去空白），
      // 但拒绝返回与请求不同的用户名——防止宽松回调被利用接管任意用户（如请求 alice 返回 admin）
      const name = body.username != null ? body.username.trim() : c.username.trim()
      if (!name) return null
      if (body.username != null && name !== c.username.trim()) return null
      return name
    } catch {
      return null
    }
  }
}

/** 按配置创建验证器；未配置返回 null。两者同设视为配置错误抛异常。 */
export function createExternalAuthProvider(config: ServerConfig): ExternalAuthProvider | null {
  const hasSecret = !!config.externalAuthSecret
  const hasUrl = !!config.externalAuthUrl
  if (hasSecret && hasUrl) {
    throw new Error("GEBAI_EXTERNAL_AUTH_SECRET 与 GEBAI_EXTERNAL_AUTH_URL 互斥，只能配置其一")
  }
  if (hasSecret) return new HmacExternalAuth(config.externalAuthSecret!)
  if (hasUrl) return new CallbackExternalAuth(config.externalAuthUrl!)
  return null
}
