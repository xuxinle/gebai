import { sessionPath } from "../../core/paths"

/**
 * 飞书用户授权共享层（feishu_docs 子 Agent 工具与 REST OAuth 回调端点共用）。
 *
 * 覆盖 OAuth code 流程：授权链接生成（authorize）、授权码兑换（v3 token 端点）、
 * refresh_token 刷新、会话级 user_access_token 存取（会话目录 feishu_user_token.json）、
 * 授权中状态注册（state → 会话映射，回调端点据此写回对应会话）。
 */

/** OAuth token 兑换/刷新端点（v3，当前推荐；PKCE 未启用时与 v2 参数一致）。 */
export const OAUTH_TOKEN_URL = "https://accounts.feishu.cn/oauth/v3/token"
/** 用户授权页（OAuth code 流程第一步）。 */
export const AUTH_AUTHORIZE_URL = "https://accounts.feishu.cn/open-apis/authen/v1/authorize"
/** 会话级 user_access_token 存储文件名（随会话分片，见 sessionPath）。 */
export const USER_TOKEN_FILE = "feishu_user_token.json"
/** auth_user_authorize 缺省 scopes：文档读写 + 刷新令牌 + 绑定用户身份（可扩展）。 */
export const DEFAULT_USER_SCOPES = "docx:document offline_access auth:user.id:read"

/** OAuth 兑换/刷新错误码 → 可读提示（authen v3 token 端点）。 */
export const OAUTH_CODE_HINTS: Record<number, string> = {
  20002: "应用凭证（client_id/client_secret）校验失败",
  20003: "授权码不存在或已失效，请重新授权获取新 code",
  20004: "授权码已过期（5 分钟有效），请重新授权",
  20009: "应用未安装到该租户，请检查应用状态",
  20010: "当前用户无权限使用该应用，请检查应用可用范围",
  20024: "授权码/refresh_token 与应用不匹配（勿混用不同应用的凭证）",
  20026: "refresh_token 无效，请重新授权",
  20037: "refresh_token 已过期（授权满 365 天），需重新授权",
  20064: "refresh_token 已被吊销，请重新授权",
  20065: "授权码已被使用（一次性），请重新授权",
  20071: "redirect_uri 与授权时不一致：请检查与开发者后台「安全设置 → 重定向 URL」登记一致",
  20073: "refresh_token 已被使用（单次有效），请重新授权",
  20074: "应用未开启刷新开关：开发者后台「安全设置」开启 user_access_token 刷新（发布后生效）",
}

/** 会话级 user_access_token（存储于会话目录，绝不输出明文）。 */
export interface UserTokenEntry {
  accessToken: string
  expireAt: number
  refreshToken?: string
  refreshExpireAt?: number
  scopes: string[]
  name?: string
  openId?: string
}

/** 用户令牌存取抽象（默认落盘会话目录；测试可注入内存实现）。 */
export interface UserTokenStore {
  get(ctx: { home: string; user: string; sessionId: string }): Promise<UserTokenEntry | null>
  set(ctx: { home: string; user: string; sessionId: string }, entry: UserTokenEntry): Promise<void>
  clear(ctx: { home: string; user: string; sessionId: string }): Promise<void>
}

/** 默认用户令牌存储：会话目录 feishu_user_token.json（随会话分片与清理）。 */
export const defaultUserTokenStore: UserTokenStore = {
  async get(ctx) {
    try {
      const { readFile } = await import("node:fs/promises")
      const { join } = await import("node:path")
      const file = join(sessionPath(ctx.home, ctx.user, ctx.sessionId), USER_TOKEN_FILE)
      return JSON.parse(await readFile(file, "utf8")) as UserTokenEntry
    } catch {
      return null
    }
  },
  async set(ctx, entry) {
    const { mkdir, writeFile } = await import("node:fs/promises")
    const { join, dirname } = await import("node:path")
    const file = join(sessionPath(ctx.home, ctx.user, ctx.sessionId), USER_TOKEN_FILE)
    await mkdir(dirname(file), { recursive: true })
    await writeFile(file, JSON.stringify(entry))
  },
  async clear(ctx) {
    try {
      const { unlink } = await import("node:fs/promises")
      const { join } = await import("node:path")
      await unlink(join(sessionPath(ctx.home, ctx.user, ctx.sessionId), USER_TOKEN_FILE))
    } catch {
      /* 文件不存在忽略 */
    }
  },
}

/**
 * 授权中状态：auth_user_authorize 注册（state 关联目标会话与所用应用），
 * 回调端点凭 state 找到会话写回令牌；兑换成功后清除。
 */
export interface FeishuPendingAuth {
  state: string
  sessionId: string
  user: string
  scopes: string
  redirectUri: string
  appId: string
  appSecret: string
  createdAt: number
}

const pendingByState = new Map<string, FeishuPendingAuth>()
const pendingBySession = new Map<string, FeishuPendingAuth>()

export function registerPendingAuth(pending: FeishuPendingAuth): void {
  pendingByState.set(pending.state, pending)
  pendingBySession.set(`${pending.user}|${pending.sessionId}`, pending)
}

export function getPendingAuth(state: string): FeishuPendingAuth | undefined {
  return pendingByState.get(state)
}

/** 会话最近一次授权的 pending（auth_user_token 手动粘贴 flow 的 state 校验用）。 */
export function getSessionPendingAuth(sessionId: string, user: string): FeishuPendingAuth | undefined {
  return pendingBySession.get(`${user}|${sessionId}`)
}

/** 兑换成功/失败后清除（state 单次有效，防重放）。 */
export function consumePendingAuth(state: string): void {
  const pending = pendingByState.get(state)
  if (pending) pendingBySession.delete(`${pending.user}|${pending.sessionId}`)
  pendingByState.delete(state)
}

/** 从回调地址/文本中提取授权码（兼容整段 URL 与裸 code；无 code 参数时原样返回）。 */
export function extractOAuthCode(raw: string): string {
  const m = raw.match(/(?:[?&]code=)([^&]+)/)
  return m ? decodeURIComponent(m[1]) : raw.trim()
}

export interface OAuthTokenRequest {
  clientId: string
  clientSecret: string
  grantType: "authorization_code" | "refresh_token"
  code?: string
  refreshToken?: string
  redirectUri?: string
}

/** OAuth token 端点请求（v3：授权码兑换 / refresh_token 刷新共用，自动携带应用凭证）。 */
export async function exchangeOAuthToken(fetchFn: typeof fetch, req: OAuthTokenRequest): Promise<Record<string, unknown>> {
  const body: Record<string, unknown> = {
    client_id: req.clientId,
    client_secret: req.clientSecret,
    grant_type: req.grantType,
    code: req.code,
    refresh_token: req.refreshToken,
    redirect_uri: req.redirectUri,
  }
  const res = await fetchFn(OAUTH_TOKEN_URL, {
    method: "POST",
    headers: { "Content-Type": "application/json; charset=utf-8" },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(30_000),
  })
  const json = (await res.json().catch(() => ({}))) as Record<string, unknown>
  const code = Number(json.code ?? 0)
  if (code !== 0) {
    const hint = OAUTH_CODE_HINTS[code]
    const detail = String(json.error_description ?? json.msg ?? "")
    throw new Error(`飞书 OAuth 失败: code=${code} ${detail}${hint ? `（${hint}）` : ""}`.trim())
  }
  if (!json.access_token) throw new Error("飞书 OAuth 响应缺少 access_token")
  return json
}

/** OAuth 响应 → 会话令牌条目（access 提前 60s 过期；refresh 单次有效，未返回时沿用旧值）。 */
export function toUserTokenEntry(json: Record<string, unknown>, prev?: UserTokenEntry): UserTokenEntry {
  const scopes = String(json.scope ?? "").split(" ").filter(Boolean)
  return {
    accessToken: String(json.access_token ?? ""),
    expireAt: Date.now() + Number(json.expires_in ?? 7200) * 1000 - 60_000,
    refreshToken: json.refresh_token ? String(json.refresh_token) : prev?.refreshToken,
    refreshExpireAt: json.refresh_token_expires_in ? Date.now() + Number(json.refresh_token_expires_in) * 1000 - 60_000 : prev?.refreshExpireAt,
    scopes: scopes.length ? scopes : (prev?.scopes ?? []),
    name: prev?.name,
    openId: prev?.openId,
  }
}

/** 尽力获取授权用户信息（需 auth:user.id:read scope；失败静默返回 null，不阻塞流程）。 */
export async function fetchFeishuUserInfo(fetchFn: typeof fetch, userToken: string): Promise<{ name?: string; openId?: string } | null> {
  try {
    const res = await fetchFn("https://open.feishu.cn/open-apis/authen/v1/user_info", {
      headers: { Authorization: `Bearer ${userToken}` },
      signal: AbortSignal.timeout(30_000),
    })
    const json = (await res.json().catch(() => null)) as { data?: { name?: string; open_id?: string } } | null
    return json?.data ? { name: json.data.name, openId: json.data.open_id } : null
  } catch {
    return null
  }
}
