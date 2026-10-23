/**
 * 拟人模式的飞书 OAuth 授权流程：**独立于 `feishu_docs` 的会话级授权**。
 *
 * ## 为什么不能复用 `feishu_docs` 的 OAuth
 *
 * 那套是「用户在某个对话里让 AI 帮忙读写云文档」：
 *
 * | | `feishu_docs` | 本模块 |
 * |---|---|---|
 * | pending 状态归属 | 某个 `sessionId` | 拟人运行时（无会话） |
 * | 兑换后写给谁 | 该会话目录的 token 文件 | `{GEBAI_HOME}/persona/feishu-user-token.json` |
 * | 回调端点 | `/api/v1/oauth/feishu/callback`（会话态） | `/api/v1/persona/feishu/callback` |
 * | 触发时机 | 用户在对话中主动发起 | **启动时自动检查，缺了就提示** |
 *
 * 硬要复用会让「拟人模式的凭据」和「某个对话的凭据」纠缠在一起——
 * 用户删掉那个对话，人格就失忆了。
 *
 * ## 流程
 *
 * ```
 * ① 启动检查 / 运行期发现缺 token
 *      ↓
 * ② startAuthorize() → 生成授权链接（含 state）
 *      ↓  前端顶部横幅提示「需要飞书授权」+ 一键打开
 * ③ 用户在飞书授权 → 跳回 /api/v1/persona/feishu/callback?code=…&state=…
 *      ↓
 * ④ exchange(code) → 写入 PersonaTokenStore → 发布事件（前端横幅消失）
 * ```
 *
 * 回调打不到歌白时（用户配的 redirect_uri 指向别处），支持**手动粘贴 code**
 * 作为兜底——这是真实会遇到的情况（回调地址被别的东西占用）。
 */

import type { PersonaUserToken } from "./user-token"

/** 授权端点（accounts.feishu.cn，与 open.feishu.cn 不同域）。 */
const AUTHORIZE_URL = "https://accounts.feishu.cn/open-apis/authen/v1/authorize"
const OAUTH_TOKEN_URL = "https://accounts.feishu.cn/oauth/v3/token"
const USER_INFO_URL = "https://open.feishu.cn/open-apis/authen/v1/user_info"

/** pending 有效期（授权链接 5 分钟有效，这里给足但不过长）。 */
const PENDING_TTL_MS = 10 * 60 * 1000
const PENDING_MAX = 8

/**
 * 拟人模式的基础 scopes：
 *
 * 只要「收发消息 + 读群」这几类——**不申请云文档权限**（那是 `feishu_docs` 的事）。
 * 少要权限的好处是真实的：授权页勾选项越少，用户越愿意点同意。
 *
 * ## 为何默认不包含 `offline_access`
 *
 * `offline_access`（飞书后台叫「持续访问已授权的数据」）是换取 `refresh_token`
 * 的前提。但它**需要应用在后台显式开通**，没开通时授权页会直接报 20027
 * 把用户挡在外面——连基本授权都完不成。
 *
 * 所以默认只要能用起来的权限；需要长期免除重新授权时，部署方在后台开通后
 * 经 `GEBAI_PERSONA_SCOPES` 追加即可（见 `scopesFromEnv`）。
 *
 * 代价（如实告知）：无 `offline_access` 时飞书 user_access_token 最长
 * 2 小时（`expires_in: 7200`），这是服务端硬限制，到期后需重新授权。
 */
export const PERSONA_SCOPES = [
  "im:message",
  "im:message:send_as_bot",
  "im:message:readonly",
  "im:chat:readonly",
  "im:chat.members:read",
].join(" ")

/** 换取 refresh_token 所需的 scope（需应用在飞书后台开通后才可用）。 */
export const OFFLINE_ACCESS_SCOPE = "offline_access"

/**
 * 从 `GEBAI_PERSONA_SCOPES` 读 scopes 覆盖（未设时用基础集）。
 *
 * 只做**并集**而不是全量替换：环境变量里写漏一个基础权限会让拟人模式
 * 静默少一项能力（例如漏了 `im:message:readonly` 就读不到入站消息，
 * 而表面上看授权是“成功”的）。
 */
export function scopesFromEnv(env: NodeJS.ProcessEnv = process.env): string {
  const extra = (env.GEBAI_PERSONA_SCOPES ?? "").trim()
  if (!extra) return PERSONA_SCOPES
  const merged = new Set([...PERSONA_SCOPES.split(/\s+/), ...extra.split(/\s+/)])
  merged.delete("")
  return [...merged].join(" ")
}

export interface PendingAuthorize {
  state: string
  createdAt: number
  redirectUri: string
  scopes: string
  appId: string
  appSecret: string
}

export interface AuthorizeOptions {
  /** 表应用 AppID（生成链接用）。 */
  appId: string
  appSecret: string
  /** 回调地址（须在飞书后台登记）。 */
  redirectUri: string
  /** 覆盖 scopes（缺省 PERSONA_SCOPES）。 */
  scopes?: string
  now?: () => number
}

export interface AuthorizeTicket {
  /** 打开这个链接让用户授权。 */
  url: string
  state: string
  scopes: string
  redirectUri: string
  /** 链接有效期（毫秒）——前端据此提示「请尽快授权」。 */
  expiresInMs: number
}

export interface ExchangeOptions {
  code: string
  fetchImpl?: typeof fetch
  now?: () => number
}

/**
 * 拟人模式 OAuth 授权管理器。
 *
 * pending 状态**进程内存**：授权是「点一下链接、几十秒内完成」的短流程，
 * 落盘没有价值（重启后重新生成一个链接即可，用户体验一样）。
 */
export class PersonaOAuth {
  private readonly pending = new Map<string, PendingAuthorize>()
  private readonly fetchImpl: typeof fetch
  private readonly now: () => number

  constructor(opts: { fetchImpl?: typeof fetch; now?: () => number } = {}) {
    this.fetchImpl = opts.fetchImpl ?? fetch
    this.now = opts.now ?? Date.now
  }

  /** 生成授权链接（含 state）。 */
  startAuthorize(opts: AuthorizeOptions): AuthorizeTicket {
    const now = opts.now?.() ?? this.now()
    this.gc(now)
    const state = crypto.randomUUID().replace(/-/g, "").slice(0, 16)
    const scopes = (opts.scopes ?? PERSONA_SCOPES).trim()
    const pending: PendingAuthorize = {
      state,
      createdAt: now,
      redirectUri: opts.redirectUri,
      scopes,
      appId: opts.appId,
      appSecret: opts.appSecret,
    }
    this.pending.set(state, pending)
    while (this.pending.size > PENDING_MAX) {
      // 超容量淘汰最旧（反复生成链接不完成授权不会无界累积）
      const oldest = [...this.pending.entries()].sort((a, b) => a[1].createdAt - b[1].createdAt)[0]
      if (!oldest) break
      this.pending.delete(oldest[0])
    }

    const url = new URL(AUTHORIZE_URL)
    url.searchParams.set("client_id", opts.appId)
    url.searchParams.set("response_type", "code")
    url.searchParams.set("redirect_uri", opts.redirectUri)
    url.searchParams.set("scope", scopes)
    url.searchParams.set("state", state)
    url.searchParams.set("prompt", "consent") // 强制显示授权页（否则已授权过的用户会直接跳回，看不到权限变化）

    return { url: url.toString(), state, scopes, redirectUri: opts.redirectUri, expiresInMs: PENDING_TTL_MS }
  }

  /** 最近一次 pending（诊断/手动兑换时的默认值）。 */
  latest(): PendingAuthorize | null {
    const all = [...this.pending.values()].sort((a, b) => b.createdAt - a.createdAt)
    return all[0] ?? null
  }

  /**
   * 用授权码换令牌。
   *
   * `state` 可选：回调流程会带上（用于校验）；手动粘贴 code 时用户可能不带，
   * 此时用 `latest()` 的 redirectUri（否则飞书会因 redirect_uri 不匹配拒绝兑换）。
   */
  async exchange(opts: ExchangeOptions & { state?: string }): Promise<PersonaUserToken> {
    const now = opts.now?.() ?? this.now()
    this.gc(now)

    const code = extractCode(opts.code)
    if (!code) throw new Error("未从输入中提取到授权码（应为 code=xxx 或纯 code）")

    const pending = opts.state ? this.pending.get(opts.state) : this.latest()
    if (!pending) {
      throw new Error("授权会话已过期或不存在：请重新发起授权（授权链接 10 分钟内有效）")
    }
    if (opts.state && pending.state !== opts.state) {
      throw new Error("state 不匹配：该回调不是本次授权的结果，请重新发起授权")
    }

    const res = await this.fetchImpl(OAUTH_TOKEN_URL, {
      method: "POST",
      headers: { "Content-Type": "application/json; charset=utf-8" },
      body: JSON.stringify({
        grant_type: "authorization_code",
        client_id: pending.appId,
        client_secret: pending.appSecret,
        code,
        redirect_uri: pending.redirectUri,
      }),
    })
    const body = (await res.json().catch(() => ({}))) as Record<string, unknown>
    const codeNum = Number(body.code ?? 0)
    if (!res.ok || (codeNum && codeNum !== 0)) {
      throw new Error(`兑换失败：code=${codeNum} ${String(body.error_description ?? body.msg ?? body.error ?? "")}`.trim())
    }
    const accessToken = String(body.access_token ?? "")
    if (!accessToken) throw new Error("兑换响应缺少 access_token")

    const expiresIn = Number(body.expires_in ?? 7200)
    const refreshExpiresIn = Number(body.refresh_token_expires_in ?? 0)
    const entry: PersonaUserToken = {
      accessToken,
      expireAt: now + expiresIn * 1000,
      scopes: Array.isArray(body.scope) ? (body.scope as string[]) : pending.scopes.split(/\s+/).filter(Boolean),
      appId: pending.appId,
      ...(body.refresh_token ? { refreshToken: String(body.refresh_token) } : {}),
      ...(refreshExpiresIn > 0 ? { refreshExpireAt: now + refreshExpiresIn * 1000 } : {}),
    }

    // 补用户信息（失败不影响令牌可用性：只是少了「授权人是谁」这行诊断）
    const info = await this.fetchUserInfo(accessToken).catch(() => null)
    if (info) {
      if (info.name) entry.name = info.name
      if (info.openId) entry.openId = info.openId
    }

    this.pending.delete(pending.state) // state 一次性
    return entry
  }

  /** 读授权用户信息（名字/open_id，仅诊断与展示用）。 */
  private async fetchUserInfo(token: string): Promise<{ name?: string; openId?: string } | null> {
    const res = await this.fetchImpl(USER_INFO_URL, { headers: { authorization: `Bearer ${token}` } })
    const body = (await res.json().catch(() => ({}))) as { code?: number; data?: { name?: string; open_id?: string } }
    if (body.code !== 0 || !body.data) return null
    return {
      ...(body.data.name ? { name: body.data.name } : {}),
      ...(body.data.open_id ? { openId: body.data.open_id } : {}),
    }
  }

  /** 清理过期 pending（惰性：每次调用顺带做，无需定时器）。 */
  private gc(now: number): void {
    for (const [state, p] of this.pending) {
      if (now - p.createdAt > PENDING_TTL_MS) this.pending.delete(state)
    }
  }
}

/** 从「纯 code」或「完整回调地址」里提取 code。 */
export function extractCode(raw: string): string {
  const text = raw.trim()
  if (!text) return ""
  const m = /[?&]code=([^&]+)/.exec(text)
  const code = m ? decodeURIComponent(m[1]!) : text
  // code 只含字母数字与 -_（飞书规范）；不符合的当无效，避免把整段 URL 当 code 提交
  return /^[A-Za-z0-9_-]+$/.test(code) ? code : ""
}
