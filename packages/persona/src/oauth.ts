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
import { feishuFetch } from "./tls"

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
 * ## 关于 `offline_access`（长期授权）：探测后决定，不盲要
 *
 * `offline_access`（飞书后台叫「持续访问已授权的数据」）是换取 `refresh_token`
 * 的前提：有它 → 长期（自动续期，不用再点授权）；没它 → 短期
 *（令牌最长 2 小时，到期需重新授权）。
 *
 * 它**要求应用在飞书后台开通**；没开通时飞书会在授权页**直接渲染 20027 错误页**
 *（「当前应用权限不足」），并且**不回跳 `redirect_uri`**——所以“先要长期、被拒再降级”
 * 在授权页层面就是断的：用户只会看到死页。于是改成**事前探测**：
 * ```
 * 1. 读应用权限清单（`offline-access.ts`），看里面有没有 offline_access
 * 2. 有 → 授权链接带它（长期）；没有/读不到 → 不带（短期，但一定能授权成功）
 * 3. 探测在后台跑，确认支持后会把手上的短期票据换成长期票
 * 4. 兑换后以**实际拿到的 refresh_token** 为准判定（飞书可以不理请求）
 * 5. 短期到期前主动弹卡提醒，并在文案里告诉人怎么去后台开长期
 * ```
 *
 * 于是“短期/长期”是**探测 + 观测**出来的结果，不是配置项：同一个部署换个应用就自动变了。
 */

/**
 * 长期授权 scope。带上它才能拿到 `refresh_token`；应用没开通时带进链接会撞 20027 死页，
 * 所以**是否真带进去由探测决定**（见 `credentials.ts#mintTicket`）。
 */
export const OFFLINE_ACCESS_SCOPE = "offline_access"

/** 飞书“应用未开通 offline_access”（或授权链上权限不足）的错误码。 */
export const ERR_OFFLINE_ACCESS_NOT_ENABLED = 20027

export const PERSONA_SCOPES = [
  "im:message",
  "im:message:send_as_bot",
  "im:message:readonly",
  // 以用户身份读群历史（`get_as_user` 是用户侧权限，默认可申请；
  // 无它则渠道 observe 只能回落到机器人身份——机器人读群历史需 `im:message.group_msg`，多数应用未开）
  "im:message.group_msg:get_as_user",
  "im:chat:readonly",
  "im:chat.members:read",
  // 长期授权：**是否真带进链接由探测决定**（应用没开通时带进去必撞 20027 死页）。
  // 这里列为「想要」：探测到支持才用，见 `credentials.ts#mintTicket`。
  OFFLINE_ACCESS_SCOPE,
].join(" ")

/**
 * scopes 合成：基础集 + 部署方额外声明的（**并集**）。
 *
 * 为何是并集而不是全量替换：漏写一个基础权限会让拟人模式静默少一项能力
 * （例如漏了 `im:message:readonly` 就读不到入站消息，而表面上看授权是“成功”的）。
 *
 * 入参来自**渠道实例的 `scopes` 字段**（不是环境变量）：
 * `offline_access` 是“这个应用能不能长期授权”的属性，
 * 跟着应用/实例走才对——同类配两份时全局变量根本表达不了。
 */
export function mergeScopes(extra: readonly string[] = []): string {
  const base = PERSONA_SCOPES.split(/\s+/).filter(Boolean)
  const add = extra.flatMap((s) => s.split(/\s+/)).filter(Boolean)
  return [...new Set([...base, ...add])].join(" ")
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
  /**
   * 生成时刻（ms）。
   *
   * 有了它，持有链接的一方（授权卡/前端横幅）才能自己判断“还能用多久”，
   * 而不是盲信一个可能已失效的 URL——那会把用户送去白跑一趟授权页。
   */
  issuedAtMs: number
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
    // 缺省走 feishuFetch（不是原生 fetch）：内网出口改写证书链时，原生 fetch 报
    // `unable to verify the first certificate`，而 GEBAI_FEISHU_INSECURE_TLS=true
    // 只对 feishuFetch 生效——用原生 fetch 会让这个开关在授权链上完全失效。
    this.fetchImpl = opts.fetchImpl ?? feishuFetch
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

    return { url: url.toString(), state, scopes, redirectUri: opts.redirectUri, expiresInMs: PENDING_TTL_MS, issuedAtMs: now }
  }

  /** 最近一次 pending（诊断/手动兑换时的默认值）。 */
  latest(): PendingAuthorize | null {
    const all = [...this.pending.values()].sort((a, b) => b.createdAt - a.createdAt)
    return all[0] ?? null
  }

  /**
   * 用授权码换令牌。
   *
   * `state` 可选但受限制：回调流程必带（用于校验）；手动粘贴 code 不带时，
   * **仅当进程内 pending 恰好只有一个**才允许用它的 redirectUri 兑换（单用户本地场景），
   * 多于一个时拒绝——否则无法区分这次兑换属于哪次授权。
   *
   * 为什么这个限制是必要的（防 account mix-up）：回调端点是公开的，攻击者可以
   * 用同一个 appId 发起自己的授权、把 code 打到回调地址且不带 state。若无条件
   * 回落 `latest()`，就会拿攻击者的 code 兑换成功——**攻击者的飞书账号成为人格身份**
   *（以他名义读群、发消息）。单 pending 例外仅限本地单用户场景（自己刚生成的链接
   * 自己粘 code），公网多用户场景应始终带 state。
   */
  async exchange(opts: ExchangeOptions & { state?: string }): Promise<PersonaUserToken> {
    const now = opts.now?.() ?? this.now()
    this.gc(now)

    const code = extractCode(opts.code)
    if (!code) throw new Error("未从输入中提取到授权码（应为 code=xxx 或纯 code）")

    // state 从显式参数或 code 原文里提取（手动粘贴完整回调 URL 时 URL 里就有 state）
    const state = opts.state ?? extractState(opts.code)
    let pending: PendingAuthorize | null = state ? (this.pending.get(state) ?? null) : null
    if (!pending) {
      if (state) {
        throw new Error("授权会话已过期或不存在：请重新发起授权（授权链接 10 分钟内有效）")
      }
      // 无 state：仅当 pending 唯一才允许兑（见上方注释；多余一个时拒绝，防 account mix-up）
      const all = [...this.pending.values()].sort((a, b) => b.createdAt - a.createdAt)
      if (all.length === 1) pending = all[0]!
      else if (all.length > 1)
        throw new Error(
          `当前存在 ${all.length} 个进行中的授权会话，无法确定本次兑换属于哪一个——` +
            "请带上完整回调地址（含 state 参数）重试，或先只保留一个授权链接",
        )
    }
    if (!pending) {
      throw new Error("授权会话已过期或不存在：请重新发起授权（授权链接 10 分钟内有效）")
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

/**
 * 从完整回调地址里提取 state（手动粘贴兑底用）。
 *
 * 粘贴的通常就是整个回调 URL，里面本来就有 state——之前只提 code 把 state 丢了，
 * 导致「明明能对上号却当成无 state 兑换」。纯 code 输入返回 undefined。
 */
export function extractState(raw: string): string | undefined {
  const text = raw.trim()
  if (!text) return undefined
  const m = /[?&]state=([^&]+)/.exec(text)
  const state = m ? decodeURIComponent(m[1]!) : ""
  return /^[A-Za-z0-9_-]+$/.test(state) && state ? state : undefined
}
