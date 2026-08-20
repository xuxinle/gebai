import type { Tool, ToolContext, ToolResult } from "../../core/types"
import { truncate } from "../../core/tools"
import type { ToolSchema } from "@gebai/sdk"
import { feishuFetch } from "../../feishu-bot/tls"
import {
  AUTH_AUTHORIZE_URL,
  DEFAULT_USER_SCOPES,
  defaultUserTokenStore,
  exchangeOAuthToken,
  toUserTokenEntry,
  fetchFeishuUserInfo,
  extractOAuthCode,
  registerPendingAuth,
  getSessionPendingAuth,
  consumePendingAuth,
  type UserTokenEntry,
  type UserTokenStore,
} from "./oauth"
export { extractOAuthCode, type UserTokenEntry, type UserTokenStore } from "./oauth"

/**
 * 飞书云文档 API 工具集（feishu_docs 子 Agent 专用）。
 *
 * 覆盖：文档（docx v1）、云空间（drive v1）、电子表格（sheets v2/v3）、
 * 多维表格（bitable v1）、知识库（wiki v2）、搜索、权限、导出、思维导图/画板（board v1）。
 * 双身份：默认 tenant_access_token（应用身份）；会话配置 user_access_token 后
 * （auth_user_authorize → 授权 → auth_user_token，见「用户授权配置」）资源类接口自动以用户身份调用，
 * 创建资源归用户所有、读写用户文档无需添加应用协作。凭证从环境变量读取：
 *   FEISHU_DOCS_APP_ID / FEISHU_DOCS_APP_SECRET（子Agent 前缀规范）
 *   兼容全局 GEBAI_FEISHU_APP_ID / GEBAI_FEISHU_APP_SECRET
 * 接口 URL 与块类型枚举以飞书开放平台官方文档为准（docx-v1 / drive-v1 / sheets-v2/3 / bitable-v1 / wiki-v2 / board-v1 / authen-v1）。
 */

const BASE_URL = "https://open.feishu.cn"
const API_TIMEOUT_MS = 30_000
/** tenant_access_token 有效期 7200s，提前 200s 刷新。 */
const TOKEN_REFRESH_EARLY_MS = 200_000

/** 常见错误码 → 可读提示（源自飞书开放平台全局错误码）。 */
const CODE_HINTS: Record<number, string> = {
  1770001: "参数不合法：请核对 document_id/block_id/token 是否准确、字段格式是否正确",
  99991661: "token 不存在或无效：请检查凭证配置",
  99991663: "token 已过期（将自动刷新重试）",
  99991668: "应用无该资源权限：请确认资源已授权给应用（用户文档需文档所有者添加应用协作）",
  99991672: "应用未开通该接口权限（scope）",
  99991679: "user_access_token 缺少权限：需用户重新授权补充 scope",
}

/** 权限类错误码区间（9999166x/9999167x）：命中时自动附「所需 scope + 授权链接」引导，减少 AI 反复试错。 */
const PERMISSION_CODE_RANGE: [number, number] = [99991660, 99991679]

/** 接口路径特征 → 可能缺失的权限 scope（权限类错误时追加引导；第一项为推荐开通项）。 */
const PATH_SCOPE_HINTS: Array<{ re: RegExp; scopes: string[] }> = [
  { re: /\/docx\/v1\//, scopes: ["docx:document"] },
  { re: /\/drive\/v1\/export_tasks/, scopes: ["docs:document:export", "drive:export:readonly"] },
  { re: /\/drive\/v1\//, scopes: ["drive:drive"] },
  { re: /\/sheets\/v[23]\//, scopes: ["sheets:spreadsheet"] },
  { re: /\/bitable\/v1\//, scopes: ["bitable:app"] },
  { re: /\/wiki\/v2\//, scopes: ["wiki:wiki"] },
  { re: /\/board\/v1\//, scopes: ["board:whiteboard"] },
  { re: /\/suite\/docs-api\/search/, scopes: ["docs:search"] },
]

/** 资源类接口路径特征：会话配置 user_access_token 后自动以用户身份调用（其余路径始终应用身份）。 */
const USER_TOKEN_PATH_RES: RegExp[] = [/\/docx\//, /\/drive\//, /\/sheets\//, /\/bitable\//, /\/wiki\//, /\/board\//, /\/suite\/docs-api\//]

/** 权限类错误的引导文案：所需 scope + 开发者后台授权链接（app_id 非敏感，可入提示）。 */
function permissionHint(appId: string, path: string): string {
  const scopes = PATH_SCOPE_HINTS.find((h) => h.re.test(path))?.scopes ?? ["对应权限"]
  const link = `https://open.feishu.cn/app/${appId}/auth?q=${encodeURIComponent(scopes[0])}`
  return `请在开发者后台为应用开通权限（建议 ${scopes.join(" / ")}），或打开授权链接快速开通: ${link}；同时确认目标文档/资源已授权给应用`
}

export interface TokenEntry {
  token: string
  expireAt: number
}

export interface FeishuDeps {
  fetchFn: typeof fetch
  /** token 缓存（可注入便于测试隔离；默认模块级共享缓存）。 */
  tokenCache: Map<string, TokenEntry>
  /** 用户令牌存取（默认落盘会话目录；测试可注入内存实现）。 */
  userTokenStore?: UserTokenStore
}

const moduleTokenCache: Map<string, TokenEntry> = new Map()

export function createFeishuTools(deps: FeishuDeps = { fetchFn: feishuFetch, tokenCache: moduleTokenCache, userTokenStore: defaultUserTokenStore }): Record<string, Tool> {
  function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
    return { type: "object", properties, required }
  }

  /** 会话级用户令牌缓存（仅缓存有效条目；失效/未配置实时读盘，兼容回调端点落盘）。 */
  const userTokenCache = new Map<string, UserTokenEntry>()

  async function loadUserToken(ctx: ToolContext): Promise<UserTokenEntry | null> {
    const cached = userTokenCache.get(ctx.sessionId)
    if (cached && cached.expireAt > Date.now()) return cached
    const entry = await deps.userTokenStore!.get({ home: ctx.home, user: ctx.user, sessionId: ctx.sessionId })
    if (entry && entry.expireAt > Date.now()) userTokenCache.set(ctx.sessionId, entry)
    else userTokenCache.delete(ctx.sessionId)
    return entry
  }

  async function saveUserToken(ctx: ToolContext, entry: UserTokenEntry | null): Promise<void> {
    if (entry) userTokenCache.set(ctx.sessionId, entry)
    else userTokenCache.delete(ctx.sessionId)
    if (entry) await deps.userTokenStore!.set({ home: ctx.home, user: ctx.user, sessionId: ctx.sessionId }, entry)
    else await deps.userTokenStore!.clear({ home: ctx.home, user: ctx.user, sessionId: ctx.sessionId })
  }

  /** 读取应用凭证（子Agent 前缀优先，兼容全局 GEBAI_FEISHU_* 命名）。 */
  function readConfig(ctx: ToolContext): { appId: string; appSecret: string } {
    const appId = ctx.env.FEISHU_DOCS_APP_ID || ctx.env.GEBAI_FEISHU_APP_ID
    const appSecret = ctx.env.FEISHU_DOCS_APP_SECRET || ctx.env.GEBAI_FEISHU_APP_SECRET
    if (!appId || !appSecret) {
      throw new Error(`缺少飞书应用凭证：请配置环境变量 FEISHU_DOCS_APP_ID 与 FEISHU_DOCS_APP_SECRET（或全局兼容的 GEBAI_FEISHU_APP_ID / GEBAI_FEISHU_APP_SECRET）；可调用 ask_env 工具（name=FEISHU_DOCS_APP_ID，secret=true）请求用户直接填写`)
    }
    return { appId, appSecret }
  }

  async function getTenantToken(ctx: ToolContext): Promise<string> {
    const { appId, appSecret } = readConfig(ctx)
    const cached = deps.tokenCache.get(appId)
    if (cached && cached.expireAt > Date.now()) return cached.token
    const res = await deps.fetchFn(`${BASE_URL}/open-apis/auth/v3/tenant_access_token/internal`, {
      method: "POST",
      headers: { "Content-Type": "application/json; charset=utf-8" },
      body: JSON.stringify({ app_id: appId, app_secret: appSecret }),
      signal: AbortSignal.timeout(API_TIMEOUT_MS),
    })
    const json = (await res.json()) as { code?: number; msg?: string; tenant_access_token?: string; expire?: number }
    if (json.code !== 0 || !json.tenant_access_token) {
      throw new Error(`获取 tenant_access_token 失败: code=${json.code} ${json.msg ?? ""}`.trim())
    }
    const expireAt = Date.now() + ((json.expire ?? 7200) * 1000 - TOKEN_REFRESH_EARLY_MS)
    deps.tokenCache.set(appId, { token: json.tenant_access_token, expireAt })
    return json.tenant_access_token
  }

  /** 会话级刷新单飞（refresh_token 单次有效：并发两个调用各自携带同一条 refresh_token 刷新，
   *  其一成功轮换、另一条必失败——失败侧静默回退 tenant 身份，创建的资源归属应用而非用户且无提示）。 */
  const refreshInFlight = new Map<string, Promise<UserTokenEntry | null>>()

  /** 用 refresh_token 换取新令牌并落盘（单次有效；失败返回 null 由调用方决定回退）。 */
  function refreshUserToken(ctx: ToolContext): Promise<UserTokenEntry | null> {
    const inflight = refreshInFlight.get(ctx.sessionId)
    if (inflight) return inflight
    const p = (async () => {
      try {
        const entry = await loadUserToken(ctx)
        if (!entry?.refreshToken) return null
        try {
          const { appId, appSecret } = readConfig(ctx)
          const json = await exchangeOAuthToken(deps.fetchFn, { clientId: appId, clientSecret: appSecret, grantType: "refresh_token", refreshToken: entry.refreshToken })
          const fresh = toUserTokenEntry(json, entry)
          await saveUserToken(ctx, fresh)
          return fresh
        } catch {
          return null
        }
      } finally {
        refreshInFlight.delete(ctx.sessionId)
      }
    })()
    refreshInFlight.set(ctx.sessionId, p)
    return p
  }

  /** 获取会话可用的 user_access_token：未配置返回 null；临近过期自动刷新（refresh 不可用/失败也返回 null）。 */
  async function getValidUserToken(ctx: ToolContext): Promise<UserTokenEntry | null> {
    const entry = await loadUserToken(ctx)
    if (!entry) return null
    if (entry.expireAt > Date.now() + 60_000) return entry
    return await refreshUserToken(ctx)
  }

  /** 飞书 API 错误（携带业务码与权限缺失清单，供上层引导）。 */
  class FeishuApiError extends Error {
    code = 0
    violations?: string[]
    /** tenant token 失效重试标记（防循环重试）。 */
    retryTenantOnce?: boolean
  }

  interface ApiOptions {
    method?: string
    query?: Record<string, unknown>
    body?: unknown
    /** multipart form（upload 用）；body 与 form 互斥。 */
    form?: FormData
    /** 额外请求头（下载 Range 等）。 */
    headers?: Record<string, string>
    /** 是否跳过 JSON 解析（下载场景直接返回 Response）。 */
    raw?: boolean
  }

  /** 统一请求封装：自动携带 token、解析飞书业务码，错误转为可读异常。
   *  资源类接口在会话配置 user_access_token 后自动以用户身份调用（创建资源归用户所有）；
   *  用户令牌失效自动刷新一次、刷新失败回退应用身份并提示；用户令牌缺权限（99991679）附缺失 scope 重新授权引导。 */
  async function api(ctx: ToolContext, path: string, opts: ApiOptions = {}): Promise<unknown | Response> {
    const userPath = USER_TOKEN_PATH_RES.some((re) => re.test(path))
    const attempt = async (token: string): Promise<unknown | Response> => {
      const url = new URL(BASE_URL + path)
      for (const [k, v] of Object.entries(opts.query ?? {})) {
        if (v !== undefined && v !== null && v !== "") url.searchParams.set(k, String(v))
      }
      const headers: Record<string, string> = { Authorization: `Bearer ${token}`, ...opts.headers }
      let body: Bun.BodyInit | undefined
      if (opts.form) {
        body = opts.form
      } else if (opts.body !== undefined) {
        headers["Content-Type"] = "application/json; charset=utf-8"
        body = JSON.stringify(opts.body)
      }
      let res: Response
      try {
        res = await deps.fetchFn(url.toString(), { method: opts.method ?? "GET", headers, body, signal: AbortSignal.timeout(API_TIMEOUT_MS) })
      } catch (err) {
        throw new Error(`请求失败: ${(err as Error).message}`)
      }
      if (opts.raw) return res
      const json = (await res.json().catch(() => ({}))) as {
        code?: number
        msg?: string
        data?: unknown
        error?: { permission_violations?: Array<{ subject?: string }> }
      }
      if (!res.ok || json.code !== 0) {
        const msg = json.msg || res.statusText
        const code = json.code ?? res.status
        let hint = CODE_HINTS[code]
        // 权限类错误（9999166x/7x）：统一附「所需 scope + 授权链接」引导（含未覆盖的具体码）
        if (code >= PERMISSION_CODE_RANGE[0] && code <= PERMISSION_CODE_RANGE[1]) {
          hint = `${hint ? `${hint}；` : "应用权限不足"}` + permissionHint(readConfig(ctx).appId, path)
        }
        // 用户令牌缺权限（99991679）：附缺失 scope 清单 + 重新授权引导
        const violations = (json.error?.permission_violations ?? []).map((v) => v.subject).filter(Boolean) as string[]
        if (code === 99991679 && violations.length) {
          hint = `user_access_token 缺少权限（${violations.join(" / ")}）：请在开发者后台为应用开通对应权限，并用 auth_user_authorize 重新生成授权链接（scope 补充缺失项）→ 用户重新授权后自动生效`
        }
        const err = new FeishuApiError(`飞书 API 错误 ${opts.method ?? "GET"} ${path}: code=${code} ${msg}${hint ? `（${hint}）` : ""}`.trim())
        err.code = code
        err.violations = violations
        throw err
      }
      return json.data
    }

    if (userPath) {
      const user = await getValidUserToken(ctx)
      if (user) {
        try {
          return await attempt(user.accessToken)
        } catch (err) {
          const fe = err as FeishuApiError
          if (fe.code === 99991663) {
            // 用户令牌失效：刷新一次重试；刷新失败回退应用身份并提示
            const fresh = await refreshUserToken(ctx)
            if (fresh) return await attempt(fresh.accessToken)
            const tenant = await getTenantToken(ctx)
            try {
              return await attempt(tenant)
            } catch (err2) {
              throw new Error(`${(err2 as Error).message}（user_access_token 已失效且刷新失败，本次已回退应用身份——如需继续以用户身份操作请重新授权）`)
            }
          }
          if (fe.code === 99991679) {
            // 可能刚重新授权（scope/令牌已更新）：失效缓存重读一次再试，避免旧令牌误导
            userTokenCache.delete(ctx.sessionId)
            const reloaded = await loadUserToken(ctx)
            if (reloaded && reloaded.accessToken !== user.accessToken) return await attempt(reloaded.accessToken)
          }
          throw err
        }
      }
    }
    const tenantAttempt = async (): Promise<unknown> => {
      const token = await getTenantToken(ctx)
      try {
        return await attempt(token)
      } catch (err) {
        // tenant token 远端已失效（提前吊销/时钟偏移，缓存仍按 expireAt 判活）：逐出缓存取新 token 重试一次，
        // 否则最长约 2 小时所有调用持续失败（错误文案还承诺「将自动刷新重试」）
        const fe = err as FeishuApiError
        if ((fe.code === 99991661 || fe.code === 99991663) && typeof fe.retryTenantOnce === "undefined") {
          fe.retryTenantOnce = true
          const { appId } = readConfig(ctx)
          deps.tokenCache.delete(appId)
          return await attempt(await getTenantToken(ctx))
        }
        throw err
      }
    }
    return await tenantAttempt()
  }

  /** JSON 字符串参数解析（兼容已解析对象）。 */
  function jsonArg(v: unknown, name: string): unknown {
    if (v === undefined || v === null) return undefined
    if (typeof v === "string") {
      try {
        return JSON.parse(v)
      } catch {
        // 长嵌套 JSON 常见失败：模型输出被截断导致 JSON 不完整——引导分批/简化写法
        throw new Error(`参数 ${name} 不是合法 JSON: ${v.slice(0, 120)}（长嵌套 JSON 易被截断，请分批提交（每批少量块）或改用简化写法：text 快捷参数 / table.rows 二维数组）`)
      }
    }
    return v
  }

  /** 工具工厂：统一 try/catch，失败返回可读文本。 */
  function tool(
    name: string,
    description: string,
    properties: Record<string, unknown>,
    required: string[],
    run: (args: Record<string, unknown>, ctx: ToolContext) => Promise<ToolResult>,
  ): Tool {
    return {
      name,
      description,
      parameters: schema(properties, required),
      async execute(args, ctx) {
        try {
          return await run(args, ctx)
        } catch (err) {
          return { output: `❌ ${(err as Error).message}` }
        }
      },
    }
  }

  /** 返回 JSON 数据（超长自动截断到会话文件）。 */
  async function jsonResult(ctx: ToolContext, data: unknown, label: string): Promise<ToolResult> {
    const text = `${label}: ${JSON.stringify(data)}`
    return truncate(text, "feishu_api", ctx)
  }

  /** 文档根块（page 块）id：未指定 block_id 时的默认父块。 */
  async function rootBlockId(ctx: ToolContext, documentId: string): Promise<string> {
    const data = (await docxCall(ctx, documentId, undefined, {}, () =>
      api(ctx, `/open-apis/docx/v1/documents/${documentId}/blocks`, { query: { page_size: 1 } }),
    )) as {
      items?: Array<{ block_id: string; block_type: number }>
    }
    const page = data.items?.[0]
    if (!page) throw new Error("无法定位文档根块（文档为空？）")
    return page.block_id
  }

  function num(v: unknown, dflt: number): number {
    const n = Number(v)
    return Number.isFinite(n) ? n : dflt
  }

  /** 自动翻页收集列表接口的全部 items（cap 兜底防失控）；truncated=达到 cap 时仍有更多数据。 */
  async function collectPages(ctx: ToolContext, path: string, pageSize: number, cap: number): Promise<{ items: Array<Record<string, unknown>>; truncated: boolean }> {
    const items: Array<Record<string, unknown>> = []
    let token = ""
    let truncated = false
    for (;;) {
      const query: Record<string, unknown> = { page_size: pageSize }
      if (token) query.page_token = token
      const data = (await api(ctx, path, { query })) as { items?: Array<Record<string, unknown>>; has_more?: boolean; page_token?: string }
      items.push(...(data.items ?? []))
      if (items.length >= cap) {
        truncated = data.has_more === true
        break
      }
      if (!data.has_more || !data.page_token || data.page_token === token) break
      token = data.page_token
    }
    return { items, truncated }
  }

  /**
   * docx 块操作包装：失败时本地探测诊断（区分文档不存在 / 块不存在 / 叶子块不支持子块），
   * 弥补飞书「invalid param」这类无信息量错误码。
   */
  async function docxCall(
    ctx: ToolContext,
    docId: string,
    blockId: string | undefined,
    opts: { checkParent?: boolean },
    fn: () => Promise<unknown>,
  ): Promise<unknown> {
    try {
      return await fn()
    } catch (err) {
      let diag = ""
      try {
        await api(ctx, `/open-apis/docx/v1/documents/${docId}`)
        if (blockId) {
          try {
            const b = unwrapBlockResponse(await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}`))
            const bt = Number(b?.block_type ?? 0)
            diag =
              opts.checkParent && LEAF_BLOCK_TYPES.has(bt)
                ? `本地诊断：block_id 有效，但块类型「${blockTypeName(bt)}」(${bt}) 不支持子块，请改用其父块（find_blocks 可定位）`
                : `本地诊断：block_id 有效（块类型「${blockTypeName(bt)}」${bt}）`
          } catch {
            diag = `本地诊断：block_id 在该文档中不存在或无访问权限——请用 find_blocks/get_doc_blocks/list_blocks 获取真实 block_id`
          }
        } else {
          diag = "本地诊断：document_id 有效"
        }
      } catch {
        diag = "本地诊断：document_id 可能无效或应用无访问权限（需文档所有者授权应用）"
      }
      throw new Error(`${(err as Error).message}；${diag}`)
    }
  }

  /** 读取某块子树文本（标题层级 + 表格行），长文档按小节读取用。 */
  async function readSubtreeText(ctx: ToolContext, docId: string, blockId: string): Promise<string> {
    const target = unwrapBlockResponse(await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}`)) ?? {}
    const desc = (await collectPages(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}/descendant`, 500, 10_000)).items
    const byId = new Map<string, Record<string, unknown>>([[String(target.block_id ?? blockId), target]])
    for (const b of desc) byId.set(String(b.block_id), b)
    const out: string[] = []
    const walk = (id: string): void => {
      const b = byId.get(id)
      if (!b) return
      const bt = Number(b.block_type ?? 0)
      if (bt === 1) {
        /* page 块自身无文本 */
      } else if (bt >= 3 && bt <= 11) {
        out.push(`${"#".repeat(bt - 2)} ${blockText(b)}`.trimEnd())
      } else if (bt === 31) {
        out.push(blockTableText(b, byId))
      } else {
        const t = blockText(b)
        if (t) out.push(t)
      }
      const kids = b.children
      if (Array.isArray(kids)) for (const c of kids) walk(String(c))
    }
    walk(String(target.block_id ?? blockId))
    return out.join("\n")
  }

  /* ================= 认证 ================= */

  const authStatus = tool("auth_status", "检查飞书应用凭证配置与 tenant_access_token 是否可用（不输出任何密钥明文）。", {}, [], async (_args, ctx) => {
    const { appId } = readConfig(ctx)
    await getTenantToken(ctx)
    return { output: `✓ 凭证已配置（app_id: ${appId.slice(0, 8)}...），tenant_access_token 获取成功，飞书 API 可用。` }
  })

  /* ================= 用户授权（user_access_token，OAuth code 流程） ================= */

  const authUserAuthorize = tool(
    "auth_user_authorize",
    "生成飞书用户授权链接（配置 user_access_token 第一步——以用户身份操作云文档，创建「用户所有权」的资源而非应用所有权）。默认自动回调：授权后浏览器自动跳回歌白（/api/v1/oauth/feishu/callback），**自动完成兑换并写回本会话，无需粘贴 code**。补充能力需对应 scope（如 sheets:spreadsheet/drive:drive/wiki:wiki/board:whiteboard）；自定义 redirect_uri 时需手动粘贴 code 走 auth_user_token。回调地址默认取 GEBAI_PUBLIC_URL（或本机 http://localhost:{GEBAI_PORT 或 3000}）拼 /api/v1/oauth/feishu/callback——**首次使用前需在开发者后台登记该回调地址**。返回授权链接（5 分钟有效、一次性）与操作说明。",
    {
      scopes: { type: "string", description: "空格分隔的授权 scope 列表（可选，缺省 docx:document offline_access auth:user.id:read）" },
      redirect_uri: { type: "string", description: "回调地址（可选，覆盖默认 GEBAI 回调；需已在开发者后台登记）" },
    },
    [],
    async (args, ctx) => {
      const { appId, appSecret } = readConfig(ctx)
      const rawScopes = String(args.scopes ?? DEFAULT_USER_SCOPES).trim()
      const scopes = [...new Set(rawScopes.split(/\s+/).filter((s) => s))].join(" ")
      if (!scopes || !/^[\w.:-]+(\s[\w.:-]+)*$/.test(scopes)) throw new Error(`scopes 格式非法: ${rawScopes}（空格分隔的 scope 列表，如 docx:document offline_access）`)
      // 默认回调地址：GEBAI_PUBLIC_URL（去尾斜杠）→ 本机 localhost:{GEBAI_PORT|3000}，拼内置回调端点
      const base = (ctx.env.GEBAI_PUBLIC_URL ?? "").replace(/\/+$/, "") || `http://localhost:${ctx.env.GEBAI_PORT ?? "3000"}`
      const redirectUri = args.redirect_uri !== undefined ? String(args.redirect_uri).trim() : `${base}/api/v1/oauth/feishu/callback`
      const state = crypto.randomUUID().replace(/-/g, "").slice(0, 16)
      registerPendingAuth({ state, sessionId: ctx.sessionId, user: ctx.user, scopes, redirectUri, appId, appSecret, createdAt: Date.now() })
      const url = new URL(AUTH_AUTHORIZE_URL)
      url.searchParams.set("client_id", appId)
      url.searchParams.set("response_type", "code")
      url.searchParams.set("redirect_uri", redirectUri)
      url.searchParams.set("scope", scopes)
      url.searchParams.set("prompt", "consent")
      url.searchParams.set("state", state)
      const autoFlow = args.redirect_uri === undefined
      const step2 = autoFlow
        ? "2. 授权完成后浏览器自动跳回歌白 并自动完成兑换，无需粘贴 code（首次使用前请在开发者后台「安全设置 → 重定向 URL」登记回调地址: " + redirectUri + "）"
        : `2. 授权完成后浏览器跳转到回调地址（需已在开发者后台登记: ${redirectUri}），复制地址栏中带 code= 的完整 URL（或仅 code）`
      const step3 = autoFlow ? "3. 回到会话说「已授权」即可继续操作（也可用 auth_user_status 确认状态）" : "3. 调用 auth_user_token 提交 code（或完整回调地址）完成配置"
      return {
        output: `✓ 已生成用户授权链接（code 5 分钟有效、一次性）:\n${url}\n\n操作步骤：\n1. 打开链接登录飞书并点击授权\n${step2}\n${step3}\n\n授权 scope: ${scopes}`,
      }
    },
  )

  const authUserToken = tool(
    "auth_user_token",
    "用授权码兑换 user_access_token 并保存到当前会话（手动粘贴流程，自动回调流程无需使用）。兑换成功后本会话的资源类操作（文档/表格/多维表格/知识库/云空间/画板）自动以该用户身份执行；access_token 过期自动用 refresh_token 刷新。令牌绝不输出明文。",
    {
      code: { type: "string", description: "授权码，或带 code= 的完整回调地址" },
      redirect_uri: { type: "string", description: "回调地址（可选，须与授权链接一致）" },
    },
    ["code"],
    async (args, ctx) => {
      const raw = String(args.code ?? "")
      const code = extractOAuthCode(raw)
      if (!code || !/^[A-Za-z0-9_-]+$/.test(code)) throw new Error("未提取到合法授权码：请粘贴授权后的 code（或完整回调地址），code 仅含字母数字与 -_")
      // state 校验：回调地址带 state 且与本次授权生成的不一致时拒绝（防跨会话/跨请求混淆）
      const stateMatch = raw.match(/(?:[?&]state=)([^&]+)/)
      if (stateMatch) {
        const pending = getSessionPendingAuth(ctx.sessionId, ctx.user)
        if (pending && pending.state !== decodeURIComponent(stateMatch[1])) {
          throw new Error("state 不匹配：该回调地址不是本会话最近一次授权的跳转结果，请重新执行 auth_user_authorize")
        }
      }
      const pending = getSessionPendingAuth(ctx.sessionId, ctx.user)
      const redirectUri = args.redirect_uri !== undefined ? String(args.redirect_uri).trim() : pending?.redirectUri ?? ""
      const { appId, appSecret } = readConfig(ctx)
      const json = await exchangeOAuthToken(deps.fetchFn, { clientId: appId, clientSecret: appSecret, grantType: "authorization_code", code, redirectUri: redirectUri || undefined })
      const entry = toUserTokenEntry(json)
      const user = await fetchFeishuUserInfo(deps.fetchFn, entry.accessToken)
      if (user) {
        entry.name = user.name
        entry.openId = user.openId
      }
      await saveUserToken(ctx, entry)
      if (pending) consumePendingAuth(pending.state)
      const who = entry.name || entry.openId || "（未获取到用户信息，可授权 auth:user.id:read/contact scope 后重试）"
      return {
        output: `✓ 已配置 user_access_token（绑定用户: ${who}，有效期 ${Number(json.expires_in ?? 7200)}s${entry.refreshToken ? "，已启用自动刷新" : ""}）\n授权 scope: ${entry.scopes.join(" / ") || "（未知）"}\n从此刻起，本会话的文档/表格/多维表格/知识库/云空间/画板操作自动以该用户身份执行（创建资源归用户所有）；令牌已存储于会话目录，不输出明文。`,
      }
    },
  )

  const authUserStatus = tool(
    "auth_user_status",
    "查看当前会话的 user_access_token 配置状态：是否已配置、绑定用户、access/refresh 有效期、已授权 scope；未配置时说明当前为应用身份并提示配置流程。过期令牌自动尝试刷新。",
    {},
    [],
    async (_args, ctx) => {
      const entry = await getValidUserToken(ctx)
      if (!entry) {
        return {
          output:
            "当前会话未配置 user_access_token：所有操作以应用身份（tenant_access_token）执行，创建的资源归应用所有、访问用户文档需文档所有者添加应用协作。\n如需以用户身份创建/操作用户文档：auth_user_authorize 生成授权链接 → 用户授权 → auth_user_token 提交 code 完成配置。",
        }
      }
      const lines = [
        `✓ user_access_token 已配置（绑定用户: ${entry.name ?? entry.openId ?? "未知"}`,
        `access 有效期至: ${new Date(entry.expireAt).toLocaleString()}${entry.expireAt <= Date.now() ? "（已过期，下次调用自动刷新）" : ""}`,
      ]
      if (entry.refreshToken) {
        lines.push(`refresh_token 有效期至: ${new Date(entry.refreshExpireAt ?? 0).toLocaleString()}（单次有效，刷新后自动轮换）`)
      } else {
        lines.push("未获取 refresh_token（授权时需含 offline_access scope）：access 过期后需重新授权")
      }
      lines.push(`已授权 scope: ${entry.scopes.join(" / ") || "（未知）"}`)
      lines.push("资源类操作（文档/表格/多维表格/知识库/云空间/画板）自动以该用户身份执行；如需更换用户/重置，用 auth_user_clear 清除后重新授权。")
      return { output: lines.join("\n") }
    },
  )

  const authUserClear = tool(
    "auth_user_clear",
    "清除当前会话已保存的 user_access_token（删除会话目录中的令牌文件）。清除后资源类操作回退为应用身份（tenant_access_token），如需用户身份需重新执行授权流程。",
    {},
    [],
    async (_args, ctx) => {
      await saveUserToken(ctx, null)
      const pending = getSessionPendingAuth(ctx.sessionId, ctx.user)
      if (pending) consumePendingAuth(pending.state)
      return { output: "✓ 已清除当前会话的 user_access_token：后续资源类操作回退为应用身份（tenant_access_token）。如需用户身份，重新执行 auth_user_authorize → 用户授权（自动回调或手动粘贴 code）。" }
    },
  )

  /* ================= 文档 docx v1 ================= */

  const createDoc = tool(
    "create_doc",
    "创建飞书在线文档（docx）。返回 document_id 与文档 URL。",
    { title: { type: "string", description: "文档标题" }, folder_token: { type: "string", description: "目标文件夹 token（可选，省略则应用云空间根目录）" } },
    ["title"],
    async (args, ctx) => {
      const data = (await api(ctx, "/open-apis/docx/v1/documents", {
        method: "POST",
        body: { title: String(args.title), folder_token: args.folder_token ? String(args.folder_token) : undefined },
      })) as { document?: { document_id: string; title: string; url?: string } }
      return jsonResult(ctx, data.document ?? data, "创建成功")
    },
  )

  const getDocMeta = tool(
    "get_doc_meta",
    "获取文档元信息（document_id、title、revision_id、url）。",
    { document_id: { type: "string" } },
    ["document_id"],
    async (args, ctx) => {
      const data = await api(ctx, `/open-apis/docx/v1/documents/${String(args.document_id)}`)
      return jsonResult(ctx, data, "文档信息")
    },
  )

  const getDocText = tool(
    "get_doc_text",
    "获取文档纯文本内容。",
    { document_id: { type: "string" }, block_id: { type: "string", description: "可选：起始块（如某标题块），只读取其子树（含标题层级与表格行，长文档按小节读取用）；缺省读整篇纯文本" } },
    ["document_id"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      if (args.block_id !== undefined) {
        const blockId = String(args.block_id)
        const text = (await docxCall(ctx, docId, blockId, {}, () => readSubtreeText(ctx, docId, blockId))) as string
        return truncate(text || "（该块无文本内容）", "feishu_doc_text", ctx)
      }
      const data = (await api(ctx, `/open-apis/docx/v1/documents/${docId}/raw_content`)) as { content?: string }
      return truncate(data.content ?? "", "feishu_doc_text", ctx)
    },
  )

  const getDocBlocks = tool(
    "get_doc_blocks",
    "获取文档全部块（含块类型/文本元素/子块 id 的富结构 JSON，每块附 type_name 类型标注）。块类型 43（mindnote 思维导图/画板）只返回 board.token 占位，其图形内容（UML 图等）用 get_board 工具读取。",
    {
      document_id: { type: "string" },
      page_token: { type: "string", description: "分页标记（可选）" },
      page_size: { type: "number", description: "每页块数，默认 100" },
      page_all: { type: "boolean", description: "自动翻页取全部块（默认 false，上限 2000）" },
    },
    ["document_id"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      if (args.page_all === true) {
        const { items, truncated } = await collectPages(ctx, `/open-apis/docx/v1/documents/${docId}/blocks`, num(args.page_size, 100), 2000)
        // 上限提示放在 JSON 之后另起一行（大 JSON 会被截断，尾部行保留提示）
        const payload = { items: items.map(decorateBlockType), total: items.length }
        const capNote = truncated ? "\n⚠️ 已达 2000 块读取上限，文档更长——建议按小节用 get_doc_text 传 block_id 读取，或 get_doc_blocks 传 page_token 继续翻页" : ""
        return truncate(`文档全部块: ${JSON.stringify(payload)}${capNote}`, "feishu_api", ctx)
      }
      const data = await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks`, {
        query: { page_token: args.page_token, page_size: args.page_size ? num(args.page_size, 100) : undefined },
      })
      return jsonResult(ctx, decorateBlocksPayload(data), "文档块")
    },
  )

  const listBlocks = tool(
    "list_blocks",
    "获取指定块下的子块列表（每块附 type_name 类型标注）。失败时自动附带本地诊断（块不存在/叶子块不支持子块等）。",
    {
      document_id: { type: "string" },
      block_id: { type: "string", description: "父块 id（缺省文档根块）" },
      page_token: { type: "string" },
      page_size: { type: "number" },
      page_all: { type: "boolean", description: "自动翻页取全部子块（默认 false）" },
    },
    ["document_id"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      const blockId = args.block_id ? String(args.block_id) : await rootBlockId(ctx, docId)
      const path = `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}/children`
      if (args.page_all === true) {
        const { items, truncated } = (await docxCall(ctx, docId, blockId, { checkParent: true }, () => collectPages(ctx, path, num(args.page_size, 100), 2000))) as {
          items: Array<Record<string, unknown>>
          truncated: boolean
        }
        // 上限提示放在 JSON 之后另起一行（大 JSON 会被截断，尾部行保留提示）
        const payload = { items: items.map(decorateBlockType), total: items.length }
        const capNote = truncated ? "\n⚠️ 已达 2000 块读取上限，子块更多——建议按小节用 get_doc_text 传 block_id 读取，或传 page_token 继续翻页" : ""
        return truncate(`块 ${blockId} 的全部子块: ${JSON.stringify(payload)}${capNote}`, "feishu_api", ctx)
      }
      const data = await docxCall(ctx, docId, blockId, { checkParent: true }, () =>
        api(ctx, path, { query: { page_token: args.page_token, page_size: args.page_size ? num(args.page_size, 100) : undefined } }),
      )
      return jsonResult(ctx, decorateBlocksPayload(data), `块 ${blockId} 的子块`)
    },
  )

  const findBlocks = tool(
    "find_blocks",
    "按文本关键词在文档中查找块，返回匹配块的 block_id/块类型(type_name)/文本/所在路径——按标题文本反查 block_id 的首选方式。",
    {
      document_id: { type: "string" },
      query: { type: "string", description: "搜索关键词（子串匹配，忽略大小写）" },
      block_type: { type: "string", description: "块类型过滤：数字或名称（heading/text/bullet/ordered/code/quote/todo/table 等）" },
      max_results: { type: "number", description: "最多返回条数（默认 20）" },
    },
    ["document_id", "query"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      const q = String(args.query ?? "").toLowerCase()
      if (!q) throw new Error("query 不能为空")
      const maxResults = Math.max(1, num(args.max_results, 20))
      let typeFilter: Set<number> | undefined
      if (args.block_type !== undefined) typeFilter = parseBlockTypeFilter(String(args.block_type))
      const items = (await collectPages(ctx, `/open-apis/docx/v1/documents/${docId}/blocks`, 500, 20_000)).items
      const byId = new Map(items.map((b) => [String(b.block_id), b]))
      let count = 0
      const matches: Array<Record<string, unknown>> = []
      for (const b of items) {
        if (typeFilter && !typeFilter.has(Number(b.block_type ?? 0))) continue
        const t = blockText(b)
        if (!t.toLowerCase().includes(q)) continue
        count++
        if (matches.length < maxResults) {
          matches.push({ block_id: b.block_id, block_type: b.block_type, type_name: blockTypeName(Number(b.block_type ?? 0)), text: t.slice(0, 60), path: blockPath(b, byId) })
        }
      }
      if (!count) return { output: `未找到包含「${args.query}」的块。可尝试：换关键词；get_doc_text 查看全文；get_doc_blocks/list_blocks 查看结构。` }
      const lines = matches.map((m, i) => `${i + 1}. ${m.block_id} | ${m.type_name}(${m.block_type}) | ${m.text} | 路径: ${m.path}`)
      const tail = count > matches.length ? `\n（共 ${count} 个匹配，仅显示前 ${matches.length} 个）` : ""
      return truncate(`找到 ${count} 个包含「${args.query}」的块：\n${lines.join("\n")}${tail}`, "feishu_find", ctx)
    },
  )

  const addBlocks = tool(
    "add_blocks",
    "在文档指定块下添加子块，单次最多 50 块（超出自动分批）。\n支持的块类型：\n- 文本类：**普通文本用 2 text（1 是 page 根块，不接受 text 内容）**；3~11 heading1~9、12 bullet、13 ordered、14 code、15 quote、17 todo（todo.style.done 标记完成）、22 divider（**divider 直接 divider:{}，不要传空 text**）。字段用类型对应驼峰名（text/heading1/bullet/ordered/code/quote/todo/divider），统一传 text 字段会自动映射；code 块 language 支持语言名（自动转枚举）。**16 equation 公式块不可经 API 创建（官方创建接口枚举不含 16，实测 99992402）——请改用普通文本块表示公式，或提示用户手动插入公式块**。\n- 表格 31：嵌套写法（table 带 children=[table_cell 块]）或简化写法 table.rows 二维数组（如 {\"block_type\":31,\"table\":{\"rows\":[[\"列A\",\"列B\"],[\"a1\",\"b1\"]]}}）。\n- 容器类（自动走创建嵌套块接口一次创建，追加到末尾、index 不生效）：19 callout 高亮块（**正文在 callout.elements（Text 结构），不是 children**；**颜色/emoji 字段放 callout.style 内**——background_color/border_color/text_color 数字枚举、emoji_id 字符串（如 pushpin/bulb），实测放 callout 顶层报 schema mismatch；text 快捷写法自动映射到 elements）；24 grid 分栏（grid.column_size 2~5 必填，children=[25 grid_column 块，每列一个]；**grid_column 不带 width_ratio（实测 9499 invalid parameter，列宽默认均分）**，调整列宽可 api_call 调 PATCH `.../blocks/{grid_id}` 传 `update_grid_column_width_ratio: {width_ratios: [全列宽度数组]}`——列内内容创建后经 update_block 填充（先 get_doc_blocks 查列内默认文本块 id），带 children 会报 field validation failed）。\n- **复杂嵌套 JSON 请分批提交（每批少量块）或优先简化写法（text 快捷参数 / table.rows）**——长 JSON 易被模型输出截断导致解析失败。\n- 引用型（需先有云空间资源 token 或外部地址）：35 embed（embed.url 必填）、37 file（file.token）、39 sheet（sheet.token）、43 mindnote（mindnote.token，思维导图/画板）、44 bitable（bitable.token，多维表格）、46 diagram（diagram.diagram_type）。\n- 图片 27 请用 insert_image 工具（三步流程，add_blocks 不支持）；32 table_cell 不可单独创建（须随 table）。",
    {
      document_id: { type: "string" },
      block_id: { type: "string", description: "父块 id（缺省文档根块，追加到末尾）" },
      blocks: { type: "array", items: { type: "object" }, description: "块数组（直接传 JSON 数组；兼容字符串形式。用数组直传可避免长 JSON 字符串被转义/截断）" },
      text: { type: "string", description: "要追加的纯文本（与 blocks 二选一）" },
      index: { type: "number", description: "插入位置（缺省末尾；含表格/嵌套/todo 时不生效）" },
    },
    ["document_id"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      const blockId = args.block_id ? String(args.block_id) : await rootBlockId(ctx, docId)
      let children: unknown
      if (args.blocks !== undefined) {
        children = jsonArg(args.blocks, "blocks")
        if (!Array.isArray(children) || children.length === 0) throw new Error("blocks 必须是至少一个块的 JSON 数组")
        // 字段自动映射：text → heading1/bullet/todo 等驼峰字段（实测统一 text 报 invalid param）
        children = (children as unknown[]).map((b) => normalizeBlockFields(b as Record<string, unknown>))
      } else if (args.text !== undefined) {
        children = [{ block_type: 2, text: { elements: textElements(String(args.text)) } }]
      } else {
        throw new Error("请提供 blocks 或 text 之一")
      }
      const index = args.index !== undefined ? num(args.index, 0) : undefined
      const childrenArr = children as unknown[]
      // 不可创建块类型前置拦截（实测：16 equation 不在 children/descendant 创建接口枚举内，报 99992402）
      for (const b of childrenArr) {
        const err = uncreatableBlockType(b)
        if (err) throw new Error(err)
      }
      // 嵌套结构（children 引用/table 块/自带 block_id）、todo 或容器块（callout/grid）：children 接口不支持，
      // 统一走创建嵌套块（descendant）接口——一次请求创建完整表格/嵌套结构
      // （实测修复：带内容表格逐格填充 N 次调用 + 限频 429 风险；B6：todo 经 children 接口报 99992402）
      const needsNested = childrenArr.some((b) => {
        if (!b || typeof b !== "object") return false
        const bo = b as Record<string, unknown>
        const t = Number(bo.block_type ?? 0)
        return t === BLOCK_TYPE.TABLE || t === BLOCK_TYPE.TODO || t === BLOCK_TYPE.CALLOUT || t === BLOCK_TYPE.GRID || t === BLOCK_TYPE.GRID_COLUMN || (Array.isArray(bo.children) && bo.children.length > 0) || bo.block_id !== undefined
      })
      if (needsNested) {
        const bb = new BlockBuilder(0)
        const groups: BlockGroup[] = []
        let fillNote = ""
        for (const b of childrenArr) {
          const bo = b as Record<string, unknown>
          // grid 结构前置校验（实测 1770041 open schema mismatch / 9499 invalid parameter）
          const gridErr = gridStructureError(bo)
          if (gridErr) throw new Error(gridErr)
          const before = bb.counter
          let rootId: string
          // 表格简化写法（table.rows 二维数组）→ 展开为 table+table_cell+text 嵌套块，一次创建
          if (Number(bo.block_type ?? 0) === BLOCK_TYPE.TABLE && (bo.table as Record<string, unknown> | undefined)?.rows !== undefined) {
            rootId = expandTableRows(bo, bb)
          } else {
            // grid 修复（实测）：grid_column 带 children 报 field validation failed、width_ratio 报 9499——
            // 创建时剥离列内容与列宽（只建 grid+grid_column 骨架，列宽默认均分、
            // 列内默认文本块由飞书自动生成），内容创建后经 update_block 填充
            const stripped = stripGridColumnContents(bo)
            if (stripped.fillColumns > 0) {
              fillNote = `；grid 列内内容已剥离，创建后用 update_block 填充（先 get_doc_blocks 查询列内默认文本块 id）`
            }
            rootId = buildGroup(stripped.block, bb)
          }
          groups.push({ rootId, blocks: bb.blocks.slice(before) })
        }
        // descendant 接口不支持 index（实测：index 语义对该接口不明确）：固定追加到末尾
        await insertGroups(ctx, docId, blockId, groups)
        const indexNote = index !== undefined ? "（index 参数对嵌套块接口不生效，已追加到末尾）" : ""
        return { output: `✓ 已添加 ${childrenArr.length} 个顶层块（含嵌套/表格/todo，走创建嵌套块接口，追加到末尾）${indexNote}${fillNote}` }
      }
      const results: string[] = []
      // children 接口单次最多 50 块，自动分批
      for (let i = 0; i < childrenArr.length; i += 50) {
        const body: Record<string, unknown> = { children: childrenArr.slice(i, i + 50) }
        if (i === 0 && index !== undefined) body.index = index
        const data = await docxCall(ctx, docId, blockId, { checkParent: true }, () =>
          api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}/children`, { method: "POST", body }),
        )
        results.push(JSON.stringify(data))
      }
      return { output: `✓ 已添加 ${childrenArr.length} 个块${results.length > 1 ? `（分 ${results.length} 批）` : ""}` }
    },
  )

  const updateBlock = tool(
    "update_block",
    "更新文档块内容。",
    {
      document_id: { type: "string" },
      block_id: { type: "string" },
      text: { type: "string", description: "替换后的文本（支持行内 Markdown 语法）" },
      insert_text: { type: "string", description: "要插入的文本" },
      insert_index: { type: "number", description: "插入位置（缺省 0=开头）" },
      style: { type: "object", description: "文本样式 {bold,italic,underline,strikethrough,inline_code}，需配合 text/insert_text" },
    },
    ["document_id", "block_id"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      const blockId = String(args.block_id)
      const style = args.style !== undefined ? (jsonArg(args.style, "style") as Record<string, unknown>) : undefined
      let patch: Record<string, unknown> = { block_id: blockId }
      let insertIndex = 0
      if (args.insert_text !== undefined) {
        insertIndex = args.insert_index !== undefined ? num(args.insert_index, 0) : 0
        patch.insert_text = { index: insertIndex, elements: textElements(String(args.insert_text), style) }
      } else if (args.text !== undefined) {
        patch.update_text_elements = { elements: textElements(String(args.text), style) }
      } else {
        throw new Error("请提供 text 或 insert_text")
      }
      try {
        const data = await docxCall(ctx, docId, blockId, {}, () =>
          api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}`, { method: "PATCH", body: patch }),
        )
        return jsonResult(ctx, data, "更新成功")
      } catch (err) {
        // insert_text 降级：飞书 insert_text 参数校验严格（实测 invalid param），
        // 读块原文 → 在 index 处拼接 → 整体替换（update_text_elements 已验证可用）；
        // 仅精确匹配参数校验类错误才降级（防误吞其他错误掩盖原始原因）
        const msg = (err as Error).message
        if (args.insert_text === undefined || !/(code=(99991400|10001)\b|invalid param)/i.test(msg)) throw err
        try {
          const block = unwrapBlockResponse(await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}`))
          if (!block) throw new Error("读取块原文失败：响应无 block 数据")
          const original = blockText(block)
          const chars = Array.from(original)
          const pos = Math.min(Math.max(insertIndex, 0), chars.length)
          const merged = chars.slice(0, pos).join("") + String(args.insert_text) + chars.slice(pos).join("")
          const data = await docxCall(ctx, docId, blockId, {}, () =>
            api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}`, {
              method: "PATCH",
              body: { block_id: blockId, update_text_elements: { elements: textElements(merged, style) } },
            }),
          )
          return jsonResult(ctx, data, "更新成功（insert_text 降级为整体替换）")
        } catch (err2) {
          throw new Error(`insert_text 失败（降级整体替换也失败）: ${(err2 as Error).message}\n原始错误: ${msg}`)
        }
      }
    },
  )

  const deleteBlocks = tool(
    "delete_blocks",
    "批量删除文档块（按子块下标区间 [start_index, end_index)，不含 end_index；只删一个时省略 end_index 或与 start_index 相同）。**注意：删除不可恢复**。",
    {
      document_id: { type: "string" },
      block_id: { type: "string", description: "父块 id（缺省文档根块）" },
      start_index: { type: "number" },
      end_index: { type: "number", description: "缺省或与 start_index 相同 = 只删一个" },
    },
    ["document_id", "start_index"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      const blockId = args.block_id ? String(args.block_id) : await rootBlockId(ctx, docId)
      const start = num(args.start_index, 0)
      // 半开区间 [start, end)：end 必须 > start；缺省/相同视为删单个；反向区间明确报错
      let end = args.end_index !== undefined ? num(args.end_index, start) : start
      if (args.end_index !== undefined && end < start) throw new Error(`end_index（${end}）不能小于 start_index（${start}）`)
      if (end <= start) end = start + 1
      const data = await docxCall(ctx, docId, blockId, {}, () =>
        api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}/children/batch_delete`, {
          method: "DELETE",
          body: { start_index: start, end_index: end },
        }),
      )
      return jsonResult(ctx, data, "删除成功")
    },
  )

  /** 分批将块组插入文档（创建嵌套块接口，单次 ≤1000 块；组不跨批切分，组内 children 引用批内自洽）。 */
  async function insertGroups(ctx: ToolContext, docId: string, parent: string, groups: BlockGroup[], index?: number): Promise<number> {
    const batches: BlockGroup[][] = []
    let cur: BlockGroup[] = []
    let cnt = 0
    for (const g of groups) {
      // 单组超上限（如超大表格）：接口单次限制 1000 块，明确报错提示拆分
      if (g.blocks.length > 1000) throw new Error(`单个块组 ${g.blocks.length} 块超过接口上限 1000（如超大表格），请拆分表格/减少行列后分批插入`)
      if (cur.length && cnt + g.blocks.length > 1000) {
        batches.push(cur)
        cur = []
        cnt = 0
      }
      cur.push(g)
      cnt += g.blocks.length
    }
    if (cur.length) batches.push(cur)
    for (let i = 0; i < batches.length; i++) {
      const body: Record<string, unknown> = {
        children_id: batches[i].map((g) => g.rootId),
        descendants: batches[i].flatMap((g) => g.blocks),
      }
      if (i === 0 && index !== undefined) body.index = index
      await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${parent}/descendant`, { method: "POST", body })
    }
    return groups.length
  }

  const importMarkdown = tool(
    "import_markdown",
    "将 Markdown 文本导入为飞书文档：不传 document_id 则新建文档（title 必填），否则追加到现有文档末尾。自动转换：多级标题（#~#########，1~9 级）/段落/有序无序列表（**缩进嵌套**，每 2 空格或 1 tab 一级）/任务列表（- [ ] / - [x]）/代码块（标注语言）/引用/GitHub 告示（`> [!NOTE]`/`[!TIP]`/`[!IMPORTANT]`/`[!WARNING]`/`[!CAUTION]` → 高亮块 callout，自动配色）/表格/分割线/行内加粗斜体粗斜体删除线行内代码链接。**生成整篇文档或大段内容时优先用本工具**（Markdown 一次成型，排版能力最全）。返回 document_id。",
    {
      content: { type: "string", description: "Markdown 文本" },
      document_id: { type: "string", description: "目标文档（缺省新建）" },
      title: { type: "string", description: "新建时的文档标题（document_id 缺省时必填）" },
      folder_token: { type: "string", description: "新建时的目标文件夹（可选）" },
      engine: { type: "string", description: "local（默认，本地转换）或 official（官方转换通道，支持更复杂的 Markdown）" },
    },
    ["content"],
    async (args, ctx) => {
      const content = String(args.content)
      let docId = args.document_id ? String(args.document_id) : ""
      if (!docId) {
        const created = (await api(ctx, "/open-apis/docx/v1/documents", {
          method: "POST",
          body: { title: String(args.title ?? "未命名文档"), folder_token: args.folder_token ? String(args.folder_token) : undefined },
        })) as { document?: { document_id: string } }
        docId = created.document?.document_id ?? ""
        if (!docId) throw new Error("创建文档失败：响应缺少 document_id")
      }
      const parent = await rootBlockId(ctx, docId)
      let count = 0
      let engineNote = ""
      if (args.engine === "official") {
        try {
          const conv = (await api(ctx, "/open-apis/docx/v1/documents/blocks/convert", {
            method: "POST",
            body: { content_type: "markdown", content },
          })) as { first_level_block_ids?: string[]; blocks?: Array<Record<string, unknown>> }
          const blocks = (conv.blocks ?? []).map(stripTableMergeInfo)
          const byParent = new Map<string, Array<Record<string, unknown>>>()
          for (const b of blocks) {
            const p = String(b.parent_id ?? "")
            if (p) byParent.set(p, [...(byParent.get(p) ?? []), b])
          }
          const groups: BlockGroup[] = []
          for (const rootId of conv.first_level_block_ids ?? []) {
            const root = blocks.find((b) => b.block_id === rootId)
            if (!root) continue
            const sub: Array<Record<string, unknown>> = []
            const stack = [...(byParent.get(rootId) ?? [])]
            while (stack.length) {
              const b = stack.pop()!
              sub.push(b)
              stack.push(...(byParent.get(String(b.block_id ?? "")) ?? []))
            }
            groups.push({ rootId, blocks: [root, ...sub] })
          }
          if (!groups.length) throw new Error("官方转换未返回任何块（Markdown 内容为空？）")
          count = await insertGroups(ctx, docId, parent, groups)
        } catch (err) {
          // B5：official 引擎对复杂组合（代码块+表格等）报 schema mismatch（1770041）等转换错误：
          // 自动回退本地转换保证导入可用（同一文档继续写入）
          if (!/1770041|99992402/.test((err as Error).message)) throw err
          const groups = markdownToBlocks(content)
          if (!groups.length) throw new Error("Markdown 内容为空，无可导入块")
          count = await insertGroups(ctx, docId, parent, groups)
          engineNote = "\n（official 引擎转换失败已自动回退本地转换；代码块与表格组合建议直接用 local 引擎）"
        }
      } else {
        const groups = markdownToBlocks(content)
        if (!groups.length) throw new Error("Markdown 内容为空，无可导入块")
        count = await insertGroups(ctx, docId, parent, groups)
      }
      return { output: `✓ 已导入 ${count} 个顶层块 → document_id: ${docId}\nURL: https://feishu.cn/docx/${docId}${engineNote}` }
    },
  )

  const exportDoc = tool(
    "export_doc",
    "导出云文档为本地文件格式。token 为文档级 token（**docx 传 document_id、sheet 传 spreadsheet_token、bitable 传 app_token——不要传数据表 table_id 作 token，实测报 1069914 file token invalid**）；file_extension 按类型支持：docx→docx/pdf、sheet→xlsx/csv、bitable→xlsx/csv。返回导出文件 file_token，可用 download_file 下载。",
    {
      token: { type: "string" },
      type: { type: "string", description: "docx/sheet/bitable，默认 docx" },
      file_extension: { type: "string", description: "docx/pdf/xlsx/csv，默认 docx" },
      sub_id: { type: "string", description: "bitable/sheet 导出 csv 时必填：bitable 传数据表 table_id、sheet 传工作表 sheet_id（导出 xlsx 不需要）" },
      file_name: { type: "string", description: "导出文件名（可选）" },
    },
    ["token", "file_extension"],
    async (args, ctx) => {
      const type = args.type ? String(args.type) : "docx"
      const fileExt = String(args.file_extension)
      const token = String(args.token)
      // 实测 1069914 file token invalid：导出 token 必须是文档级 token（bitable 为 app_token），
      // 数据表 table_id 是子表 ID，只能作为 sub_id 传参（官方接口：sub_id 仅当 sheet/bitable 导出 csv 时使用）
      const subId = args.sub_id !== undefined ? String(args.sub_id) : undefined
      if ((type === "bitable" || type === "sheet") && fileExt === "csv" && !subId) {
        return { output: `❌ ${type} 导出 csv 必须指定 sub_id（bitable 传数据表 table_id、sheet 传工作表 sheet_id——官方接口要求 sub_id 仅当导出 csv 时必填；xlsx 导出不需要）` }
      }
      const body: Record<string, unknown> = {
        token,
        type,
        file_extension: fileExt,
        file_name: args.file_name ? String(args.file_name) : undefined,
        sub_id: subId,
      }
      const created = (await api(ctx, "/open-apis/drive/v1/export_tasks", { method: "POST", body })) as { ticket?: string }
      if (!created.ticket) throw new Error("创建导出任务失败：响应缺少 ticket")
      // 轮询任务结果（最多 10 次 × 1s）：查询接口只携带 token query 参数
      // （实测多余 file_extension/type 报 field validation failed）
      for (let i = 0; i < 10; i++) {
        await new Promise((r) => setTimeout(r, 1000))
        const data = (await api(ctx, `/open-apis/drive/v1/export_tasks/${created.ticket}`, {
          query: { token },
        })) as { result?: { file_token?: string; file_name?: string; file_size?: number; size?: number; job_status?: number; job_error_msg?: string } }
        const result = data.result
        if (result?.file_token) {
          return {
            output: `✓ 导出完成: ${result.file_name ?? "文件"}（${result.file_size ?? result.size ?? "?"} 字节）\nfile_token: ${result.file_token}\n可用 download_file 工具下载。`,
          }
        }
        // 任务终态失败（job_status 3/107~123）提前返回原因，不再空等到超时
        if (result && result.job_status !== undefined && result.job_status !== 0 && result.job_status !== 1 && result.job_status !== 2) {
          return { output: `❌ 导出任务失败（job_status=${result.job_status}）: ${result.job_error_msg ?? "未知原因"}。` }
        }
      }
      return { output: `导出任务已创建（ticket: ${created.ticket}），但等待超时，请稍后重试。` }
    },
  )

  /* ================= 云空间 drive v1 ================= */

  const listFiles = tool(
    "list_files",
    "列出云空间文件夹中的文件清单（名称/token/类型/URL），文件类型 type 取值 docx/sheet/bitable/file/folder 等。",
    {
      folder_token: { type: "string", description: "文件夹 token（缺省根目录）" },
      page_size: { type: "number" },
      page_token: { type: "string" },
    },
    [],
    async (args, ctx) => {
      const data = await api(ctx, "/open-apis/drive/v1/files", {
        query: { folder_token: args.folder_token ? String(args.folder_token) : undefined, page_size: args.page_size ? num(args.page_size, 100) : undefined, page_token: args.page_token },
      })
      return jsonResult(ctx, data, "文件清单")
    },
  )

  const createFolder = tool(
    "create_folder",
    "在云空间创建文件夹。返回 folder token。",
    { name: { type: "string" }, folder_token: { type: "string", description: "父文件夹 token（可选，缺省根目录）" } },
    ["name"],
    async (args, ctx) => {
      const data = await api(ctx, "/open-apis/drive/v1/files/create_folder", {
        method: "POST",
        body: { name: String(args.name), folder_token: String(args.folder_token ?? "") },
      })
      return jsonResult(ctx, data, "创建成功")
    },
  )

  // 缺省 type 元信息查询的自动识别回退：file 补查无结果时再按 docx 补查（实测 docx 缺省识别报 970005）
  const retryMetaQuery = async (
    api: (ctx: ToolContext, path: string, opts?: ApiOptions) => Promise<unknown | Response>,
    token: string,
    ctx: ToolContext,
  ) => {
    const tryDocType = async (docType: string) =>
      api(ctx, "/open-apis/drive/v1/metas/batch_query", {
        method: "POST",
        body: { request_docs: [{ doc_token: token, doc_type: docType }] },
      })
    const fileRes = await tryDocType("file")
    const fileMetas = (fileRes as { metas?: unknown[] })?.metas ?? []
    if (fileMetas.length) return fileRes
    return tryDocType("docx")
  }

  const getFileMeta = tool(
    "get_file_meta",
    "获取云空间文件/文件夹元信息（名称、类型、URL 等；普通 file 类型无法自动识别时会自动回退按 file 查询）。内部走 metas/batch_query（B2：files/{token} 对 docx 返回 404）。",
    { file_token: { type: "string" }, type: { type: "string", description: "文件类型（可选，缺省自动识别；docx 建议显式传 docx——缺省识别不稳定可能报 970005）" } },
    ["file_token"],
    async (args, ctx) => {
      const token = String(args.file_token)
      const req: Record<string, unknown> = { doc_token: token }
      if (args.type) req.doc_type = String(args.type)
      try {
        const data = await api(ctx, "/open-apis/drive/v1/metas/batch_query", { method: "POST", body: { request_docs: [req] } })
        // 缺省 type 且未识别出结果：普通 file 类型需显式 doc_type=file，补一次查询
        const metas = (data as { metas?: unknown[] })?.metas ?? []
        if (!args.type && !metas.length) {
          return jsonResult(ctx, await retryMetaQuery(api, token, ctx), "文件信息")
        }
        return jsonResult(ctx, data, "文件信息")
      } catch (err) {
        // 缺省 type 查询失败（如 docx 无法自动识别）时按 file 类型补查
        if (args.type) throw err
        return jsonResult(ctx, await retryMetaQuery(api, token, ctx), "文件信息")
      }
    },
  )

  const uploadFile = tool(
    "upload_file",
    "上传文件到云空间（单文件 ≤ 20MB）。返回 file_token。",
    {
      file_name: { type: "string", description: "文件名（含扩展名）" },
      content: { type: "string", description: "文件内容（encoding=base64 时为 base64 文本，否则按 UTF-8 文本）" },
      folder_token: { type: "string", description: "目标文件夹 token（可选，缺省应用云空间根目录）" },
      encoding: { type: "string", description: "base64 或 text（默认 text）" },
    },
    ["file_name", "content"],
    async (args, ctx) => {
      const fileName = String(args.file_name)
      const raw = String(args.content)
      const bytes = args.encoding === "base64" ? Uint8Array.from(atob(raw), (c) => c.charCodeAt(0)) : new TextEncoder().encode(raw)
      const form = new FormData()
      form.append("file_name", fileName)
      form.append("parent_type", "explorer")
      // 缺省省略 parent_node（空串被飞书拒绝；描述称「缺省根目录」需先 root_folder_meta 取 token 传入）
      if (args.folder_token) form.append("parent_node", String(args.folder_token))
      form.append("size", String(bytes.length))
      form.append("file", new Blob([bytes], { type: "application/octet-stream" }), fileName)
      const data = await api(ctx, "/open-apis/drive/v1/files/upload_all", { method: "POST", form })
      return jsonResult(ctx, data, "上传成功")
    },
  )

  const insertImage = tool(
    "insert_image",
    "在文档中插入图片（飞书官方三步流程，实测验证）：1) 创建空 image 块（block_type=27，image:{}，创建时**不传 token**——传 token 报 1770001）；2) 上传图片素材到该块（POST drive/v1/medias/upload_all，multipart：parent_type=docx_image、parent_node=新建块 id；**云空间的 file_token 不能直接用于文档 image 块**，必须走 media 上传）；3) PATCH replace_image 设置素材 token（返回 width/height 自动识别）。",
    {
      document_id: { type: "string" },
      block_id: { type: "string", description: "父块 id（缺省文档根块，追加到末尾）" },
      image: { type: "string", description: "base64 文本（encoding=base64）或本地图片文件路径（**须传绝对路径**，相对路径相对会话目录会 ENOENT）" },
      file_name: { type: "string", description: "文件名（缺省 image.png）" },
      encoding: { type: "string", description: "base64 或 path（默认 path）" },
    },
    ["document_id", "image"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      const blockId = args.block_id ? String(args.block_id) : await rootBlockId(ctx, docId)
      const fileName = String(args.file_name ?? "image.png")
      // 先读文件/校验，后创建块：原顺序（先建空块）在路径错误/base64 非法/超限时会在文档中
      // 残留一个空 image 块（三步流程无回滚，脏数据需模型额外感知清理）
      let bytes: Uint8Array
      if (args.encoding === "base64") {
        try {
          bytes = Uint8Array.from(atob(String(args.image)), (c) => c.charCodeAt(0))
        } catch {
          throw new Error("base64 解码失败：image 不是合法 base64 文本")
        }
      } else {
        bytes = new Uint8Array(await Bun.file(ctx.resolvePath(String(args.image))).arrayBuffer())
      }
      if (!bytes.length) throw new Error("图片内容为空（文件不存在或 base64 无效？）")
      // 大小上限（飞书 media 上传限制 20MB）：显式校验做纵深防御（base64 双份拷贝内存峰值约 2.7×）
      if (bytes.length > 20 * 1024 * 1024) throw new Error(`图片超过 20MB 上限: ${(bytes.length / 1024 / 1024).toFixed(1)}MB`)
      // 步骤 1：创建空 image 块（image:{} 不传 token，否则 1770001 invalid param）
      const created = (await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}/children`, {
        method: "POST",
        body: { children: [{ block_type: BLOCK_TYPE.IMAGE, image: {} }] },
      })) as { children?: Array<{ block_id?: string }> }
      const imageBlockId = created.children?.[0]?.block_id
      if (!imageBlockId) throw new Error("创建 image 块失败：响应缺少 block_id")
      // 步骤 2：上传图片素材到该块（multipart；parent_type=docx_image 关联 image 块）
      const form = new FormData()
      form.append("file_name", fileName)
      form.append("parent_type", "docx_image")
      form.append("parent_node", imageBlockId)
      form.append("size", String(bytes.length))
      form.append("file", new Blob([bytes], { type: "application/octet-stream" }), fileName)
      const up = (await api(ctx, "/open-apis/drive/v1/medias/upload_all", { method: "POST", form })) as { file_token?: string }
      if (!up.file_token) throw new Error("图片素材上传失败：响应缺少 file_token")
      // 步骤 3：PATCH replace_image 设置素材（width/height 自动识别）
      const patched = (await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${imageBlockId}`, {
        method: "PATCH",
        body: { block_id: imageBlockId, replace_image: { token: up.file_token } },
      })) as { image?: { width?: number; height?: number } }
      const size = patched?.image?.width ? `（${patched.image.width}×${patched.image.height}）` : ""
      return { output: `✓ 已在文档插入图片${size}\nimage block_id: ${imageBlockId}\nmedia file_token: ${up.file_token}` }
    },
  )

  const downloadFile = tool(
    "download_file",
    "下载云空间文件到会话 tmp/ 目录。返回保存路径与大小（文本内容会附上前 300 字符预览）。",
    { file_token: { type: "string" }, save_path: { type: "string", description: "保存路径（可选，缺省为工作目录下的原文件名）" } },
    ["file_token"],
    async (args, ctx) => {
      const fileToken = String(args.file_token)
      // 先取元信息获得文件名（与 get_file_meta 一致走 batch_query；失败回退 token 不阻塞下载）
      let fileName = fileToken
      try {
        const meta = (await api(ctx, "/open-apis/drive/v1/metas/batch_query", { method: "POST", body: { request_docs: [{ doc_token: fileToken }] } })) as {
          metas?: Array<{ title?: string }>
        }
        if (meta.metas?.[0]?.title) fileName = meta.metas[0].title
      } catch {
        /* 元信息失败不阻塞下载 */
      }
      let res = (await api(ctx, `/open-apis/drive/v1/files/${fileToken}/download`, {
        raw: true,
        // 导出文件（export_tasks 产物）下载必须携带 Range 头，否则返回 403
        headers: { Range: "bytes=0-" },
      })) as Response
      // 导出产物是 media 类型 token：files 接口返回 403/404，回退 medias 下载接口（实测）
      if (!res.ok) {
        res = (await api(ctx, `/open-apis/drive/v1/medias/${fileToken}/download`, {
          raw: true,
          headers: { Range: "bytes=0-" },
        })) as Response
      }
      if (!res.ok) throw new Error(`下载失败: HTTP ${res.status} ${res.statusText}`)
      const buf = new Uint8Array(await res.arrayBuffer())
      const absPath = ctx.resolvePath(args.save_path ? String(args.save_path) : fileName)
      const { mkdir, writeFile } = await import("node:fs/promises")
      const { dirname } = await import("node:path")
      await mkdir(dirname(absPath), { recursive: true })
      await writeFile(absPath, buf)
      // 文本内容尝试解码预览
      let preview = ""
      const text = new TextDecoder().decode(buf.slice(0, 400))
      if (!text.includes("\uFFFD") && /[\x09\x0a\x0d\x20-\x7e\u4e00-\u9fff]/.test(text)) preview = `\n预览: ${text}`
      return { output: `✓ 已保存 ${buf.length} 字节 → ${absPath}${preview}`, filePath: absPath }
    },
  )

  const deleteFile = tool(
    "delete_file",
    "删除云空间文件/文件夹（删除不可恢复）。",
    { file_token: { type: "string" }, type: { type: "string", description: "docx/sheet/bitable/file/folder（必填）" } },
    ["file_token", "type"],
    async (args, ctx) => {
      const data = await api(ctx, `/open-apis/drive/v1/files/${String(args.file_token)}`, { method: "DELETE", query: { type: args.type } })
      return jsonResult(ctx, data, "删除成功")
    },
  )

  /* ================= 搜索 ================= */

  const search = tool(
    "search",
    "搜索云文档（文档/表格/多维表格/文件夹/知识库等）。注意：需应用开通「云文档搜索」权限，否则返回权限错误。",
    {
      query: { type: "string", description: "搜索关键词" },
      docs_types: { type: "string", description: "JSON 数组，如 [\"docx\",\"sheet\"]" },
      count: { type: "number", description: "条数（默认 10）" },
    },
    ["query"],
    async (args, ctx) => {
      const body: Record<string, unknown> = {
        search_key: String(args.query),
        count: args.count !== undefined ? num(args.count, 10) : 10,
        offset: 0,
      }
      if (args.docs_types !== undefined) body.docs_types = jsonArg(args.docs_types, "docs_types")
      const data = await api(ctx, "/open-apis/suite/docs-api/search/object", { method: "POST", body })
      return jsonResult(ctx, data, "搜索结果")
    },
  )

  /* ================= 电子表格 sheets v2/v3 ================= */

  const createSheet = tool(
    "create_sheet",
    "创建飞书电子表格。返回 spreadsheet_token。",
    { title: { type: "string" }, folder_token: { type: "string", description: "目标文件夹 token（可选）" } },
    ["title"],
    async (args, ctx) => {
      const data = await api(ctx, "/open-apis/sheets/v3/spreadsheets", {
        method: "POST",
        body: { title: String(args.title), folder_token: args.folder_token ? String(args.folder_token) : undefined },
      })
      return jsonResult(ctx, data, "创建成功")
    },
  )

  /** 工作表名称 → sheet_id 自动解析：range 前缀若匹配工作表标题（非 sheet_id 形式）则替换为 sheet_id（B6：读写须用 sheet_id，用名称报 90215）。 */
  async function resolveSheetRange(ctx: ToolContext, token: string, range: string): Promise<string> {
    const m = range.match(/^([^!]+)!(.+)$/)
    if (!m) return range
    const head = m[1]
    // 已是 sheet_id（oVs 开头长 token）：直接返回，不触发名称查询
    if (/^oVs[A-Za-z0-9_-]{6,}$/.test(head)) return range
    const data = (await api(ctx, `/open-apis/sheets/v3/spreadsheets/${token}/sheets/query`)) as { sheets?: Array<{ sheet_id?: string; title?: string }> }
    const sheets = data.sheets ?? []
    const hit = sheets.find((s) => s.title === head)
    if (hit?.sheet_id) return `${hit.sheet_id}!${m[2]}`
    // 数字下标引用（未命中名称时）：原样使用
    if (/^\d+$/.test(head)) return range
    // 前缀与任何工作表名/sheet_id 均不匹配（常见误因：用了表格标题而非工作表名）：直接报可用清单，替代接口侧 90215 盲错；
    // 仅在拿到非空清单时判定（清单为空=接口异常/降级，无从判定，保持旧行为交由飞书接口报错）
    if (sheets.length && !sheets.some((s) => s.sheet_id === head)) {
      const titles = sheets.map((s) => s.title).filter(Boolean).join("、")
      throw new Error(`range 前缀「${head}」不是本表格的工作表${titles ? `（可用工作表：${titles}）` : ""}——前缀须用工作表名或 get_sheet_meta 返回的 sheet_id，表格标题不可作前缀`)
    }
    return range
  }

  const getSheetMeta = tool(
    "get_sheet_meta",
    "获取电子表格信息（标题与完整工作表列表，含每个 sheet_id/title/index）。读数据前先调用本工具获取 sheet_id。",
    { spreadsheet_token: { type: "string" } },
    ["spreadsheet_token"],
    async (args, ctx) => {
      const token = String(args.spreadsheet_token)
      // B6：仅 /spreadsheets/{token} 缺工作表列表，补 sheets/query 合并返回
      const [meta, sheets] = await Promise.all([
        api(ctx, `/open-apis/sheets/v3/spreadsheets/${token}`),
        api(ctx, `/open-apis/sheets/v3/spreadsheets/${token}/sheets/query`),
      ])
      return jsonResult(ctx, { ...(meta as Record<string, unknown>), sheets: (sheets as { sheets?: unknown })?.sheets ?? [] }, "表格信息")
    },
  )

  const readSheet = tool(
    "read_sheet",
    "读取电子表格数据，值以字符串形式返回。",
    { spreadsheet_token: { type: "string" }, range: { type: "string", description: "如 Sheet1!A1:C10（可选；前缀支持工作表名称自动解析为 sheet_id；缺省返回工作表列表）" } },
    ["spreadsheet_token"],
    async (args, ctx) => {
      const token = String(args.spreadsheet_token)
      if (!args.range) {
        const data = await api(ctx, `/open-apis/sheets/v3/spreadsheets/${token}`)
        return jsonResult(ctx, data, "工作表列表（用 sheet_id 构造 range 读取数据，如 oVsAj!A1）")
      }
      const range = await resolveSheetRange(ctx, token, String(args.range))
      const data = await api(ctx, `/open-apis/sheets/v2/spreadsheets/${token}/values/${encodeURIComponent(range)}`, {
        query: { valueRenderOption: "ToString", dateTimeRenderOption: "FormattedString" },
      })
      return jsonResult(ctx, data, "单元格数据")
    },
  )

  const writeSheet = tool(
    "write_sheet",
    "写入电子表格（整体覆盖指定区域）。",
    { spreadsheet_token: { type: "string" }, range: { type: "string", description: "如 'Sheet1!A1:C3'（前缀支持工作表名称自动解析为 sheet_id）" }, values: { type: "string", description: "二维数组 JSON，如 [[\"a\",\"b\"],[\"c\",\"d\"]]" } },
    ["spreadsheet_token", "range", "values"],
    async (args, ctx) => {
      const values = jsonArg(args.values, "values")
      if (!Array.isArray(values)) throw new Error("values 必须是二维数组 JSON")
      const range = await resolveSheetRange(ctx, String(args.spreadsheet_token), String(args.range))
      const data = await api(ctx, `/open-apis/sheets/v2/spreadsheets/${String(args.spreadsheet_token)}/values`, {
        method: "PUT",
        body: { valueRange: { range, values } },
      })
      return jsonResult(ctx, data, "写入成功")
    },
  )

  const appendSheet = tool(
    "append_sheet",
    "向电子表格追加行。",
    { spreadsheet_token: { type: "string" }, range: { type: "string", description: "追加基准格如 'Sheet1!A1'（通常取首列首格；前缀支持工作表名称自动解析为 sheet_id；仅给起始格时自动扩展覆盖全部数据行）" }, values: { type: "string", description: "二维数组 JSON" } },
    ["spreadsheet_token", "range", "values"],
    async (args, ctx) => {
      const values = jsonArg(args.values, "values")
      if (!Array.isArray(values)) throw new Error("values 必须是二维数组 JSON")
      const rows = values.length
      const cols = Array.isArray(values[0]) ? (values[0] as unknown[]).length : 0
      // 自动扩展：range 只给起始格时扩展为覆盖全部数据行的区域（实测 wrong range）
      const range = expandAppendRange(await resolveSheetRange(ctx, String(args.spreadsheet_token), String(args.range)), rows, cols)
      const data = await api(ctx, `/open-apis/sheets/v2/spreadsheets/${String(args.spreadsheet_token)}/values_append`, {
        method: "POST",
        query: { insertDataOption: "INSERT_ROWS" },
        body: { valueRange: { range, values } },
      })
      return jsonResult(ctx, data, "追加成功")
    },
  )

  /* ================= 多维表格 bitable v1 ================= */

  const createBitable = tool(
    "create_bitable",
    "创建多维表格。返回 app_token 与默认数据表 id。",
    {
      name: { type: "string" },
      folder_token: { type: "string", description: "目标文件夹 token（可选）" },
      fields: { type: "string", description: "数据表字段定义 JSON 数组（可选，默认表创建后自动建字段，如 [{\"name\":\"名称\",\"type\":1},{\"name\":\"状态\",\"type\":3,\"property\":{\"options\":[{\"name\":\"进行中\"},{\"name\":\"已完成\"}]}}]；type 枚举：1 多行文本/2 数字/3 单选/4 多选/5 日期/7 复选框/11 人员/13 电话/15 超链接，单选多选需 property.options）" },
    },
    ["name"],
    async (args, ctx) => {
      const data = (await api(ctx, "/open-apis/bitable/v1/apps", {
        method: "POST",
        body: { name: String(args.name), folder_token: args.folder_token ? String(args.folder_token) : undefined },
      })) as { app?: { app_token?: string; default_table_id?: string } }
      const app = (data.app ?? data) as { app_token?: string; default_table_id?: string }
      const appToken = String(app.app_token ?? "")
      if (!appToken) throw new Error("创建多维表格失败：响应缺少 app_token")
      const tableId = String(app.default_table_id ?? "")
      let fieldsNote = ""
      if (args.fields !== undefined) {
        const fields = jsonArg(args.fields, "fields")
        if (!Array.isArray(fields)) throw new Error("fields 必须是字段定义 JSON 数组")
        if (!tableId) throw new Error("创建多维表格失败：响应缺少默认数据表 id（无法创建字段）")
        // 实测修复：create_bitable 后默认表只有基础字段，写入自定义字段名报 FieldNameNotFound——
        // 创建后按 fields 定义逐个建字段（单选/多选等带 property.options）
        const created: string[] = []
        for (const f of fields) {
          if (!f || typeof f !== "object") throw new Error(`字段定义无效（需为对象）: ${JSON.stringify(f)}`)
          const name = String((f as { name?: unknown }).name ?? "")
          const type = Number((f as { type?: unknown }).type ?? 0)
          if (!name || !type) throw new Error(`字段定义需含 name 与 type: ${JSON.stringify(f)}`)
          await api(ctx, `/open-apis/bitable/v1/apps/${appToken}/tables/${tableId}/fields`, {
            method: "POST",
            body: { field_name: name, type, property: (f as { property?: unknown }).property },
          })
          created.push(name)
        }
        fieldsNote = `\n已创建 ${created.length} 个字段: ${created.join(" / ")}`
      }
      return { output: `✓ 已创建多维表格: ${args.name}\napp_token: ${appToken}${tableId ? `\n默认数据表: ${tableId}` : ""}${fieldsNote}\n可用 add_bitable_records 写入记录。` }
    },
  )

  const listBitableTables = tool(
    "list_bitable_tables",
    "列出多维表格的数据表（table_id/name）。",
    { app_token: { type: "string" } },
    ["app_token"],
    async (args, ctx) => {
      const data = await api(ctx, `/open-apis/bitable/v1/apps/${String(args.app_token)}/tables`)
      return jsonResult(ctx, data, "数据表列表")
    },
  )

  const listBitableRecords = tool(
    "list_bitable_records",
    "列出多维表格记录。",
    {
      app_token: { type: "string" },
      table_id: { type: "string" },
      page_size: { type: "number", description: "分页大小（默认 100）" },
      page_token: { type: "string", description: "分页标记（可选）" },
      filter: { type: "string", description: "过滤条件 JSON（可选，如 {\"conjunction\":\"and\",\"conditions\":[{\"field_name\":\"状态\",\"operator\":\"is\",\"value\":[\"完成\"]}]}）" },
    },
    ["app_token", "table_id"],
    async (args, ctx) => {
      const base = `/open-apis/bitable/v1/apps/${String(args.app_token)}/tables/${String(args.table_id)}/records`
      // B4：filter 走 GET /records?filter= 报 InvalidFilter，须用 POST /records/search 带 body filter
      if (args.filter !== undefined) {
        const body: Record<string, unknown> = {
          filter: jsonArg(args.filter, "filter"),
          page_size: args.page_size ? num(args.page_size, 100) : undefined,
          page_token: args.page_token,
        }
        const data = await api(ctx, `${base}/search`, { method: "POST", body })
        return jsonResult(ctx, data, "记录列表")
      }
      const query: Record<string, unknown> = {
        page_size: args.page_size ? num(args.page_size, 100) : undefined,
        page_token: args.page_token,
      }
      const data = await api(ctx, base, { query })
      return jsonResult(ctx, data, "记录列表")
    },
  )

  const addBitableRecords = tool(
    "add_bitable_records",
    "新增多维表格记录（批量，单次最多 100 条）。",
    { app_token: { type: "string" }, table_id: { type: "string" }, records: { type: "string", description: "记录 JSON 数组，元素可直接是字段对象 {字段:值} 或 {fields:{字段:值}}（自动包装）" } },
    ["app_token", "table_id", "records"],
    async (args, ctx) => {
      const records = jsonArg(args.records, "records")
      if (!Array.isArray(records) || !records.length) throw new Error("records 必须是非空 JSON 数组")
      const body = { records: records.map((r) => (r && typeof r === "object" && "fields" in r ? r : { fields: r })) }
      const data = await api(ctx, `/open-apis/bitable/v1/apps/${String(args.app_token)}/tables/${String(args.table_id)}/records/batch_create`, {
        method: "POST",
        body,
      })
      return jsonResult(ctx, data, "新增成功")
    },
  )

  const updateBitableRecord = tool(
    "update_bitable_record",
    "更新多维表格单条记录。",
    {
      app_token: { type: "string" },
      table_id: { type: "string" },
      record_id: { type: "string" },
      fields: { type: "string", description: "字段对象 JSON {字段:新值}" },
    },
    ["app_token", "table_id", "record_id", "fields"],
    async (args, ctx) => {
      const fields = jsonArg(args.fields, "fields")
      if (!fields || typeof fields !== "object") throw new Error("fields 必须是对象 JSON")
      const data = await api(ctx, `/open-apis/bitable/v1/apps/${String(args.app_token)}/tables/${String(args.table_id)}/records/${String(args.record_id)}`, {
        method: "PUT",
        body: { fields },
      })
      return jsonResult(ctx, data, "更新成功")
    },
  )

  const deleteBitableRecords = tool(
    "delete_bitable_records",
    "删除多维表格记录（批量，不可恢复）。",
    { app_token: { type: "string" }, table_id: { type: "string" }, record_ids: { type: "string", description: "record_id 数组 JSON 或逗号分隔的列表" } },
    ["app_token", "table_id", "record_ids"],
    async (args, ctx) => {
      let ids: string[] = []
      if (args.record_ids !== undefined && typeof args.record_ids === "string" && !args.record_ids.trim().startsWith("[")) {
        ids = String(args.record_ids).split(",").map((s) => s.trim()).filter(Boolean)
      } else {
        const raw = jsonArg(args.record_ids, "record_ids")
        // 容错：元素可能是字符串或对象（{record_id}），统一提取为字符串
        ids = (Array.isArray(raw) ? raw : [raw])
          .map((id) => (id && typeof id === "object" ? String((id as { record_id?: unknown }).record_id ?? "") : String(id)))
          .filter(Boolean)
      }
      if (!ids.length) throw new Error("record_ids 不能为空")
      // B3/实测：DELETE /records 的 body 不生效；batch_delete 的 records 需为**字符串数组**
      // （对象数组 {record_id} 实测报错——用户用 api_call 直调字符串数组成功删除）
      // 单次上限 500 条，超出自动分批
      const batches: string[][] = []
      for (let i = 0; i < ids.length; i += 500) batches.push(ids.slice(i, i + 500))
      const results: string[] = []
      for (const batch of batches) {
        const data = await api(ctx, `/open-apis/bitable/v1/apps/${String(args.app_token)}/tables/${String(args.table_id)}/records/batch_delete`, {
          method: "POST",
          body: { records: batch },
        })
        results.push(JSON.stringify(data))
      }
      return jsonResult(ctx, results.length > 1 ? { batches: results.length, results } : results[0], "删除成功")
    },
  )

  /* ================= 知识库 wiki v2 ================= */

  const listWikiSpaces = tool(
    "list_wiki_spaces",
    "列出知识空间（space_id/name）。",
    { page_size: { type: "number" }, page_token: { type: "string" } },
    [],
    async (args, ctx) => {
      const data = await api(ctx, "/open-apis/wiki/v2/spaces", {
        query: { page_size: args.page_size ? num(args.page_size, 20) : undefined, page_token: args.page_token },
      })
      return jsonResult(ctx, data, "知识空间列表")
    },
  )

  const createWikiNode = tool(
    "create_wiki_node",
    "在知识空间创建节点（文档）。返回 node_token 与 obj_token（obj_token 即文档 document_id）。",
    {
      space_id: { type: "string" },
      title: { type: "string" },
      parent_node_token: { type: "string", description: "父节点 token（可选）" },
      markdown: { type: "string", description: "Markdown 正文（与 body 二选一）" },
      body: { type: "string", description: "块 JSON 数组（与 markdown 二选一）" },
    },
    ["space_id", "title"],
    async (args, ctx) => {
      const payload: Record<string, unknown> = { obj_type: "docx", title: String(args.title) }
      if (args.parent_node_token) payload.parent_node_token = String(args.parent_node_token)
      const data = (await api(ctx, `/open-apis/wiki/v2/spaces/${String(args.space_id)}/nodes`, { method: "POST", body: payload })) as {
        node?: { node_token?: string; obj_token?: string; title?: string }
      }
      const node = data.node ?? (data as Record<string, unknown>)
      const objToken = String(node.obj_token ?? "")
      const nodeToken = String(node.node_token ?? "")
      if (!objToken) throw new Error("创建节点失败：响应缺少 obj_token")
      let contentNote = ""
      if (args.markdown !== undefined) {
        const groups = markdownToBlocks(String(args.markdown))
        const parent = await rootBlockId(ctx, objToken)
        await insertGroups(ctx, objToken, parent, groups)
        contentNote = `，已写入 ${groups.length} 个块`
      } else if (args.body !== undefined) {
        const body = jsonArg(args.body, "body")
        if (!Array.isArray(body)) throw new Error("body 必须是块 JSON 数组")
        const parent = await rootBlockId(ctx, objToken)
        for (let i = 0; i < body.length; i += 50) {
          await api(ctx, `/open-apis/docx/v1/documents/${objToken}/blocks/${parent}/children`, { method: "POST", body: { children: body.slice(i, i + 50) } })
        }
        contentNote = `，已写入 ${body.length} 个块`
      }
      return { output: `✓ 已创建知识库节点: ${node.title ?? args.title}\nnode_token: ${nodeToken}\nobj_token: ${objToken}（即文档 document_id）${contentNote}` }
    },
  )

  const getWikiNode = tool(
    "get_wiki_node",
    "根据 token 查询知识库节点信息（node_token/obj_token/space_id/标题等）。",
    { token: { type: "string" }, obj_type: { type: "string", description: "docx/sheet/bitable/file/wiki（可选）" } },
    ["token"],
    async (args, ctx) => {
      const data = await api(ctx, "/open-apis/wiki/v2/spaces/get_node", { query: { token: args.token, obj_type: args.obj_type ?? "docx" } })
      return jsonResult(ctx, data, "节点信息")
    },
  )

  /* ================= 思维导图 / 画板 board v1 ================= */

  const getBoard = tool(
    "get_board",
    "读取思维导图/画板（board，含文档中 UML 等图形内容）。参数二选一：board_token 画板 token；或 document_id+block_id——mindnote 思维导图块（块类型 43，get_doc_blocks 输出 type_name=mindnote、含 board.token）自动提取画板 token。内部调用 /open-apis/board/v1/whiteboards/{token}/nodes 并结构化提取：优先返回 PlantUML 源码（syntax.code，语义完整可读），否则重建「形状文本 + 连接线关系」为流程描述（如 <步骤A> ->(是) <步骤B>），避免原始大 JSON 截断。",
    {
      board_token: { type: "string", description: "画板/思维导图 token（必填，或与 document_id+block_id 二选一）" },
      document_id: { type: "string", description: "含思维导图块的文档 id（与 block_id 配合，自动提取画板 token）" },
      block_id: { type: "string", description: "mindnote 思维导图块 id（type_name=mindnote，含 board.token）" },
    },
    [],
    async (args, ctx) => {
      let boardToken = args.board_token !== undefined ? String(args.board_token) : ""
      if (!boardToken) {
        if (args.document_id === undefined || args.block_id === undefined) {
          throw new Error("请提供 board_token，或同时提供 document_id 与 block_id（mindnote 思维导图块）")
        }
        const docId = String(args.document_id)
        const blockId = String(args.block_id)
        const block = unwrapBlockResponse(await docxCall(ctx, docId, blockId, {}, () =>
          api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}`),
        ))
        if (!block) throw new Error(`读取块 ${blockId} 失败：响应无 block 数据`)
        boardToken = extractBoardToken(block)
        if (!boardToken) {
          throw new Error(
            `块 ${blockId} 不含画板 token（块类型「${blockTypeName(Number(block.block_type ?? 0))}」${block.block_type ?? 0}）——请用 get_doc_blocks/find_blocks 查找 type_name=mindnote 的块`,
          )
        }
      }
      // 分页读取全部节点（上限 5000 防失控），结构化提取后再输出（避免原始大 JSON 爆 token/截断）
      const items: unknown[] = []
      let token = ""
      let truncated = false
      for (;;) {
        const query: Record<string, unknown> = { page_size: 100 }
        if (token) query.page_token = token
        const data = (await api(ctx, `/open-apis/board/v1/whiteboards/${boardToken}/nodes`, { query })) as {
          items?: unknown[]
          has_more?: boolean
          page_token?: string
        }
        items.push(...(data.items ?? []))
        if (items.length >= 5000) {
          truncated = data.has_more === true
          break
        }
        if (!data.has_more || !data.page_token || data.page_token === token) break
        token = data.page_token
      }
      if (!items.length) return { output: "✓ 画板为空（无节点内容）。" }
      const extracted = extractBoardContent(items)
      const tail = truncated ? "\n（画板节点超过 5000 个读取上限，仅输出前 5000 个）" : ""
      return truncate(extracted + tail, "feishu_board", ctx)
    },
  )

  /* ================= 权限 ================= */

  const addPermission = tool(
    "add_permission",
    "为云文档添加协作者（分享）。",
    {
      token: { type: "string" },
      type: { type: "string", description: "docx/sheet/bitable/file/folder/wiki" },
      member_type: { type: "string", description: "openid/unionid/userid/email/chat" },
      member_id: { type: "string", description: "成员 id（chat=群 chat_id）" },
      perm: { type: "string", description: "view/edit/full_access" },
    },
    ["token", "type", "member_type", "member_id", "perm"],
    async (args, ctx) => {
      const data = await api(ctx, `/open-apis/drive/v1/permissions/${String(args.token)}/members`, {
        method: "POST",
        query: { type: args.type },
        body: { member_type: String(args.member_type), member_id: String(args.member_id), perm: String(args.perm) },
      })
      return jsonResult(ctx, data, "授权成功")
    },
  )

  const setLinkShare = tool(
    "set_link_share",
    "设置云文档链接分享范围（PATCH permissions/{token}/public——**方法为 PATCH，PUT 实测 404**）。需应用开通云文档分享相关权限（drive:drive 等，否则返回权限错误）。",
    {
      token: { type: "string" },
      type: { type: "string", description: "docx/sheet/bitable/file/folder/wiki（必填，缺失 404）" },
      link_share_entity: { type: "string", description: "分享范围：tenant_readable=组织内可阅读（缺省）/tenant_editable/anyone_readable/anyone_editable/closed" },
      external_access_entity: { type: "string", description: "外部访问范围（可选）" },
      security_entity: { type: "string", description: "安全设置：anyone_can_view/anyone_can_edit/only_full_access（可选）" },
    },
    ["token", "type"],
    async (args, ctx) => {
      // 白名单校验：防拼写错误/幻觉值（尤其互联网公开级别，需审批兜底 + 明确枚举；实测修正：枚举为 tenant/anyone 系列，非 anyone_can_view）
      const share = String(args.link_share_entity ?? "tenant_readable")
      const SHARE_ENTITIES = ["tenant_readable", "tenant_editable", "anyone_readable", "anyone_editable", "closed"]
      if (!SHARE_ENTITIES.includes(share)) throw new Error(`link_share_entity 非法: ${share}（可选 ${SHARE_ENTITIES.join("/")}）`)
      const body: Record<string, unknown> = { link_share_entity: share }
      if (args.external_access_entity) body.external_access_entity = String(args.external_access_entity)
      if (args.security_entity) body.security_entity = String(args.security_entity)
      try {
        const data = await api(ctx, `/open-apis/drive/v1/permissions/${String(args.token)}/public`, {
          method: "PATCH",
          query: { type: String(args.type) },
          body,
        })
        return jsonResult(ctx, data, "分享设置成功")
      } catch (err) {
        // 实测 404：GET 权限正常但 PATCH /public 404——多为「设置链接分享」写权限 scope 未开通（飞书未授权接口返回 404 而非权限错误码）
        const msg = String((err as Error).message || err)
        if (msg.includes("404") || msg.includes("not found")) {
          return {
            output: `❌ ${msg}\n诊断：设置链接分享返回 404——①请确认应用已开通云文档分享权限（开发者后台 → 权限管理 → drive:drive 或 docs:permission.setting:write_only）；②确认 token 与 type 匹配（type=docx 用 document_id，type=sheet 用 spreadsheet_token）；③若为试用租户，部分接口可能不可用。GET 权限信息正常说明读权限与 token 均有效，问题聚焦在写权限。`,
          }
        }
        throw err
      }
    },
  )

  /* ================= 兜底 ================= */

  const apiCall = tool(
    "api_call",
    "直接调用任意飞书开放平台接口（兜底，覆盖未单独封装的新接口），自动携带 tenant_access_token。",
    {
      method: { type: "string", description: "GET/POST/PUT/PATCH/DELETE" },
      path: { type: "string", description: "以 /open-apis/ 开头的接口路径" },
      query: { type: "object", description: "查询参数对象（可选）" },
      body: { type: "object", description: "请求体 JSON（可选）" },
    },
    ["method", "path"],
    async (args, ctx) => {
      const path = String(args.path)
      if (!path.startsWith("/open-apis/")) throw new Error("path 必须以 /open-apis/ 开头")
      const method = String(args.method).toUpperCase()
      if (!["GET", "POST", "PUT", "PATCH", "DELETE"].includes(method)) throw new Error(`不支持的 method: ${method}`)
      const data = await api(ctx, path, {
        method,
        query: args.query !== undefined ? (jsonArg(args.query, "query") as Record<string, unknown>) : undefined,
        body: args.body !== undefined ? jsonArg(args.body, "body") : undefined,
      })
      return jsonResult(ctx, data, `调用成功 (${method} ${path})`)
    },
  )

  return {
    auth_status: authStatus,
    auth_user_authorize: authUserAuthorize,
    auth_user_token: authUserToken,
    auth_user_status: authUserStatus,
    auth_user_clear: authUserClear,
    create_doc: createDoc,
    get_doc_meta: getDocMeta,
    get_doc_text: getDocText,
    get_doc_blocks: getDocBlocks,
    list_blocks: listBlocks,
    find_blocks: findBlocks,
    add_blocks: addBlocks,
    update_block: updateBlock,
    delete_blocks: deleteBlocks,
    import_markdown: importMarkdown,
    export_doc: exportDoc,
    list_files: listFiles,
    create_folder: createFolder,
    get_file_meta: getFileMeta,
    upload_file: uploadFile,
    insert_image: insertImage,
    download_file: downloadFile,
    delete_file: deleteFile,
    search: search,
    create_sheet: createSheet,
    get_sheet_meta: getSheetMeta,
    read_sheet: readSheet,
    write_sheet: writeSheet,
    append_sheet: appendSheet,
    create_bitable: createBitable,
    list_bitable_tables: listBitableTables,
    list_bitable_records: listBitableRecords,
    add_bitable_records: addBitableRecords,
    update_bitable_record: updateBitableRecord,
    delete_bitable_records: deleteBitableRecords,
    list_wiki_spaces: listWikiSpaces,
    create_wiki_node: createWikiNode,
    get_wiki_node: getWikiNode,
    get_board: getBoard,
    add_permission: addPermission,
    set_link_share: setLinkShare,
    api_call: apiCall,
  }
}

/* ================= board 内容结构化提取（纯函数，可独立测试） ================= */

/** 深度遍历 JSON 结构（对象/数组），对每个对象字段回调。 */
function walkJson(data: unknown, visit: (key: string, value: unknown, parent: Record<string, unknown>) => void): void {
  if (Array.isArray(data)) {
    for (const item of data) walkJson(item, visit)
    return
  }
  if (data && typeof data === "object") {
    const obj = data as Record<string, unknown>
    for (const [k, v] of Object.entries(obj)) {
      visit(k, v, obj)
      walkJson(v, visit)
    }
  }
}

/** 从 mindnote 块对象中提取画板 token（支持 board.token 嵌套与 board_token/whiteboard_id 顶层字段）。 */
export function extractBoardToken(block: unknown): string {
  const hits: string[] = []
  walkJson(block, (key, value) => {
    if (key === "board" && value && typeof value === "object") {
      const t = (value as Record<string, unknown>).token
      if (typeof t === "string" && t) hits.push(t)
    } else if ((key === "board_token" || key === "whiteboard_id") && typeof value === "string" && value) {
      hits.push(value)
    }
  })
  return hits[0] ?? ""
}

/** 深度查找 PlantUML 源码：优先 syntax.code（UML 图形块的完整语义），其次任意像 PlantUML 的 code 字段。 */
export function findPlantUmlSource(data: unknown): string | undefined {
  const syntaxCodes: string[] = []
  const allCodes: string[] = []
  walkJson(data, (key, value) => {
    if (key !== "code" || typeof value !== "string" || !value.trim()) return
    allCodes.push(value)
  })
  walkJson(data, (key, value) => {
    if (key === "syntax" && value && typeof value === "object") {
      const c = (value as Record<string, unknown>).code
      if (typeof c === "string" && c.trim()) syntaxCodes.push(c)
    }
  })
  // 取像 PlantUML 的源码（@start 开头或含箭头语法），否则取第一个
  const looks = (c: string): boolean => /^\s*@start/.test(c) || /--[->]|->|\.\.|==>/.test(c)
  return [...syntaxCodes, ...allCodes].find(looks) ?? [...syntaxCodes, ...allCodes][0]
}

/** 从对象中提取形状/连接线文本（兼容 text/label/title 及 content.text/props.text 等常见嵌套）。 */
function objText(obj: Record<string, unknown>): string | undefined {
  const direct = (v: unknown): string | undefined => (typeof v === "string" && v.trim() ? v.trim() : undefined)
  const hit = direct(obj.text) ?? direct(obj.label) ?? direct(obj.title)
  if (hit !== undefined) return hit
  for (const key of ["content", "props"]) {
    const sub = obj[key]
    if (sub && typeof sub === "object") {
      const s = direct((sub as Record<string, unknown>).text)
      if (s !== undefined) return s
    }
  }
  return undefined
}

/** 引用取值：对象引用取 node_id/id，字符串直接用。 */
function refValue(obj: Record<string, unknown>, keys: string[]): string | undefined {
  for (const k of keys) {
    const v = obj[k]
    if (typeof v === "string" && v) return v
    if (v && typeof v === "object") {
      const sub = v as Record<string, unknown>
      const ref = sub.node_id ?? sub.id
      if (typeof ref === "string" && ref) return ref
    }
  }
  return undefined
}

export interface BoardShape {
  id: string
  text: string
}

export interface BoardEdge {
  from: string
  to: string
  label?: string
}

/** 收集画板形状节点（node_id/id + 文本）。 */
export function collectBoardShapes(data: unknown): BoardShape[] {
  const out = new Map<string, string>()
  walkJson(data, (key, value, parent) => {
    if (key !== "node_id" && key !== "id") return
    if (typeof value !== "string" || !value) return
    const text = objText(parent)
    if (text !== undefined) out.set(value, text)
  })
  return [...out.entries()].map(([id, text]) => ({ id, text }))
}

/** 收集画板连接线（from/to、source/target、start/end 等引用），label 取连接线对象自身文本。 */
export function collectBoardEdges(data: unknown): BoardEdge[] {
  const edges: BoardEdge[] = []
  walkJson(data, (key, value, parent) => {
    if (key !== "node_id" && key !== "id") return
    if (typeof value !== "string" || !value) return
    const from = refValue(parent, ["from", "source", "start", "start_node_id"])
    const to = refValue(parent, ["to", "target", "end", "end_node_id"])
    if (from && to && from !== to) edges.push({ from, to, label: objText(parent) })
  })
  return edges
}

/**
 * board nodes 响应 → 可读文本：
 * 1. 优先 PlantUML 源码（syntax.code）——UML 图形块的完整语义；
 * 2. 否则重建「形状文本 + 连接线关系」为流程描述（<A> ->(label) <B>）；
 * 3. 仅形状无连接线时输出节点文本列表；全无则回退原始 JSON 摘要。
 */
export function extractBoardContent(nodes: unknown[]): string {
  const plant = findPlantUmlSource(nodes)
  if (plant) return `[画板 PlantUML 源码]\n${plant}`
  const shapes = collectBoardShapes(nodes)
  const textById = new Map(shapes.map((s) => [s.id, s.text]))
  const edges = collectBoardEdges(nodes)
  if (edges.length) {
    const lines = edges.map((e) => `${textById.get(e.from) ?? e.from} ->(${e.label ?? ""}) ${textById.get(e.to) ?? e.to}`)
    const nodeList = shapes.length ? `\n节点: ${shapes.map((s) => `${s.id}="${s.text}"`).join("、")}` : ""
    return `[画板节点]${nodeList}\n[连接关系]\n${lines.join("\n")}`
  }
  if (shapes.length) return `[画板节点文本]\n${shapes.map((s) => s.text).join("\n")}`
  return `[画板节点原始结构]\n${JSON.stringify(nodes).slice(0, 2000)}`
}

/* ================= Markdown → docx 块转换（纯函数，可独立测试） ================= */

/** 行内 Markdown → text_run 元素数组（**加粗**、`代码`、[链接](url)、*斜体*、***粗斜体***、~~删除线~~）。 */
export function textElements(md: string, style?: Record<string, unknown>): Record<string, unknown>[] {
  const els: Record<string, unknown>[] = []
  const re = /(\*\*\*[^*\n]+\*\*\*|\*\*[^*\n]+\*\*|~~[^~\n]+~~|`[^`\n]+`|\[[^\]\n]+\]\([^)\s]+\)|\*[^*\n]+\*)/g
  let last = 0
  let m: RegExpExecArray | null
  const push = (content: string, st: Record<string, unknown> | undefined) => {
    if (!content) return
    const run: Record<string, unknown> = { content, ...(st && Object.keys(st).length ? { text_element_style: st } : {}) }
    els.push({ text_run: run })
  }
  while ((m = re.exec(md))) {
    push(md.slice(last, m.index), style)
    const tok = m[0]
    if (tok.startsWith("***") && tok.endsWith("***")) push(tok.slice(3, -3), { bold: true, italic: true, ...style })
    else if (tok.startsWith("**") && tok.endsWith("**")) push(tok.slice(2, -2), { bold: true, ...style })
    else if (tok.startsWith("~~") && tok.endsWith("~~")) push(tok.slice(2, -2), { strikethrough: true, ...style })
    else if (tok.startsWith("`") && tok.endsWith("`")) push(tok.slice(1, -1), { inline_code: true, ...style })
    else if (tok.startsWith("[")) {
      const link = tok.match(/^\[([^\]]+)\]\(([^)\s]+)\)$/)
      if (link) push(link[1], { link: { url: link[2] }, ...style })
      else push(tok, style)
    } else if (tok.startsWith("*") && tok.endsWith("*") && tok.length > 2) push(tok.slice(1, -1), { italic: true, ...style })
    else push(tok, style)
    last = re.lastIndex
  }
  push(md.slice(last), style)
  return els.length ? els : [{ text_run: { content: "" } }]
}

/** docx v1 块类型枚举（官方文档）：1=page 2=text 3~11=heading1~9 12=bullet 13=ordered 14=code 15=quote 16=equation 17=todo 19=callout 22=divider 24=grid 25=grid_column 27=image 31=table 32=table_cell 33=view 34=quote_container 35=embed 37=file 39=sheet 40=add_ons 43=mindnote 44=bitable 46=diagram */
export const BLOCK_TYPE = {
  TEXT: 2,
  HEADING1: 3,
  BULLET: 12,
  ORDERED: 13,
  CODE: 14,
  QUOTE: 15,
  EQUATION: 16,
  TODO: 17,
  DIVIDER: 22,
  IMAGE: 27,
  TABLE: 31,
  TABLE_CELL: 32,
  GRID: 24,
  GRID_COLUMN: 25,
  EMBED: 35,
  FILE: 37,
  SHEET: 39,
  CALLOUT: 19,
  MINDNOTE: 43,
  BITABLE: 44,
  DIAGRAM: 46,
} as const

/* ================= docx 块读取与诊断辅助（纯函数，可独立测试） ================= */

/** docx v1 块类型名称标注（官方公开类型；其余标记 未知(n)）。
 *  实测修正：图片块为 27（原 43 标注 image 有误——43 为 mindnote 思维笔记）；
 *  容器块实测修正：19=callout、24=grid、25=grid_column（原 40/33/34 标注有误——33=view、34=quote_container、40=add_ons）。 */
export const BLOCK_TYPE_NAME: Record<number, string> = {
  1: "page",
  2: "text",
  3: "heading1",
  4: "heading2",
  5: "heading3",
  6: "heading4",
  7: "heading5",
  8: "heading6",
  9: "heading7",
  10: "heading8",
  11: "heading9",
  12: "bullet",
  13: "ordered",
  14: "code",
  15: "quote",
  16: "equation",
  17: "todo",
  19: "callout",
  22: "divider",
  24: "grid",
  25: "grid_column",
  27: "image",
  31: "table",
  32: "table_cell",
  33: "view",
  34: "quote_container",
  35: "embed",
  37: "file",
  39: "sheet",
  40: "add_ons",
  41: "chat_card",
  43: "mindnote",
  44: "bitable",
  45: "iframe",
  46: "diagram",
  47: "isv",
}

export function blockTypeName(t: number): string {
  return BLOCK_TYPE_NAME[t] ?? `未知(${t})`
}

/** 单块读取响应解包：飞书 GET blocks/{id} 返回 data: { block: {...} }（api() 已解包 data 层）；
 *  兼容扁平结构（{block_id,...} 直接返回）。 */
export function unwrapBlockResponse(resp: unknown): Record<string, unknown> | undefined {
  if (!resp || typeof resp !== "object") return undefined
  const r = resp as Record<string, unknown>
  const inner = r.block
  if (inner && typeof inner === "object" && !Array.isArray(inner)) return inner as Record<string, unknown>
  return r
}

/** 明确不支持子块的叶子类型（children 类操作诊断用）。27=image 为叶子（实测补充）。 */
const LEAF_BLOCK_TYPES: ReadonlySet<number> = new Set([16, 22, 27, 35, 37, 39, 43])

/** 提取块文本元素数组（text/heading/bullet/ordered/quote/todo/code/equation 等含 elements 的字段）。 */
function blockElements(block: Record<string, unknown>): unknown[] | undefined {
  for (const [k, v] of Object.entries(block)) {
    if (k === "block_id" || k === "block_type" || k === "parent_id" || k === "children" || k === "table" || k === "table_cell" || k === "divider") continue
    if (v && typeof v === "object" && Array.isArray((v as Record<string, unknown>).elements)) return (v as Record<string, unknown>).elements as unknown[]
  }
  return undefined
}

/** 块的纯文本（拼接 text_run content；无文本返回空串）。 */
export function blockText(block: Record<string, unknown>): string {
  const els = blockElements(block) ?? []
  return els
    .map((e) => (e && typeof e === "object" && "text_run" in e ? String(((e as Record<string, unknown>).text_run as Record<string, unknown> | undefined)?.content ?? "") : ""))
    .join("")
}

/** 表格块 → 行文本（单元格内容按列分组）。 */
function blockTableText(table: Record<string, unknown>, byId: Map<string, Record<string, unknown>>): string {
  const prop = ((table.table ?? {}) as Record<string, unknown>).property as Record<string, unknown> | undefined
  const colSize = Number(prop?.column_size ?? 0)
  const kids = table.children
  const rows: string[] = []
  let row: string[] = []
  const seen = new Set<string>()
  for (const c of Array.isArray(kids) ? kids : []) {
    row.push(cellText(String(c), byId, seen))
    if (colSize > 0 && row.length >= colSize) {
      rows.push(`| ${row.join(" | ")} |`)
      row = []
    }
  }
  if (row.length) rows.push(`| ${row.join(" | ")} |`)
  return rows.length ? `[表格]\n${rows.join("\n")}` : "[表格]"
}

/** 单元格/块子树文本（含子块递归拼接）。 */
function cellText(id: string, byId: Map<string, Record<string, unknown>>, seen: Set<string>): string {
  const b = byId.get(id)
  if (!b || seen.has(id)) return ""
  seen.add(id)
  const kids = b.children
  let sub = ""
  if (Array.isArray(kids)) for (const c of kids) sub += cellText(String(c), byId, seen)
  return blockText(b) + sub
}

/** 块所在路径（根 → 自身，取每级文本前 30 字符）。 */
function blockPath(block: Record<string, unknown>, byId: Map<string, Record<string, unknown>>): string {
  const chain: string[] = []
  const seen = new Set<string>()
  let cur: Record<string, unknown> | undefined = block
  while (cur && !seen.has(String(cur.block_id))) {
    seen.add(String(cur.block_id))
    const bt = Number(cur.block_type ?? 0)
    const t = blockText(cur).slice(0, 30)
    chain.unshift(t || (bt === 1 ? "根" : blockTypeName(bt)))
    cur = byId.get(String(cur.parent_id ?? ""))
  }
  return chain.join(" / ") || "/"
}

/** 块类型过滤解析：数字或名称（heading 覆盖 heading1~9）。 */
function parseBlockTypeFilter(spec: string): Set<number> {
  const named: Record<string, number[]> = {
    page: [1],
    text: [2],
    heading: [3, 4, 5, 6, 7, 8, 9, 10, 11],
    heading1: [3],
    heading2: [4],
    heading3: [5],
    heading4: [6],
    heading5: [7],
    heading6: [8],
    heading7: [9],
    heading8: [10],
    heading9: [11],
    bullet: [12],
    ordered: [13],
    code: [14],
    quote: [15],
    equation: [16],
    todo: [17],
    divider: [22],
    table: [31],
    table_cell: [32],
    grid: [24],
    grid_column: [25],
    embed: [35],
    file: [37],
    sheet: [39],
    callout: [19],
    chat_card: [41],
    view: [33],
    mindnote: [43],
    bitable: [44],
    iframe: [45],
    image: [27],
  }
  const s = spec.trim().toLowerCase()
  const n = named[s]
  if (n) return new Set(n)
  const numeric = Number(s)
  if (Number.isFinite(numeric)) return new Set([numeric])
  throw new Error(`无法识别的块类型: ${spec}（支持数字或名称，如 heading/text/bullet/ordered/code/quote/todo/table）`)
}

/** 块列表项标注 type_name（不可变返回新对象）。 */
function decorateBlockType(item: Record<string, unknown>): Record<string, unknown> {
  return { ...item, type_name: blockTypeName(Number(item.block_type ?? 0)) }
}

/** 对列表响应整体标注 type_name。 */
function decorateBlocksPayload(data: unknown): unknown {
  const d = data as { items?: Array<Record<string, unknown>> } | null
  if (!d || !Array.isArray(d.items)) return data
  return { ...d, items: d.items.map(decorateBlockType) }
}

/** 块组：一个顶层块及其全部子树块（descendant 接口插入单位，块内已含 block_id 与 children 引用）。 */
export interface BlockGroup {
  rootId: string
  blocks: Record<string, unknown>[]
}

/** 官方 blocks/convert 返回的表格块中剥离只读字段 merge_info（官方要求，否则插入报错）。 */
export function stripTableMergeInfo(block: Record<string, unknown>): Record<string, unknown> {
  if (block.block_type === BLOCK_TYPE.TABLE && block.table && typeof block.table === "object") {
    const table = { ...(block.table as Record<string, unknown>) }
    const property = table.property && typeof table.property === "object" ? { ...(table.property as Record<string, unknown>) } : undefined
    if (property) {
      delete property.merge_info
      table.property = property
    }
    return { ...block, table }
  }
  return block
}

/** 块构建器：为每个块生成唯一 block_id（创建嵌套块接口要求，跨组全局唯一）。 */
class BlockBuilder {
  private n: number

  constructor(base = 0) {
    this.n = base
  }

  get counter(): number {
    return this.n
  }

  blocks: Record<string, unknown>[] = []

  add(block: Record<string, unknown>, children: string[] = [], prefix = "b"): string {
    const id = `${prefix}${++this.n}`
    this.blocks.push({ block_id: id, ...block, children })
    return id
  }
}

function headingBlock(level: number, content: string): Record<string, unknown> {
  const blockType = BLOCK_TYPE.HEADING1 + level - 1
  return { block_type: blockType, [`heading${level}`]: { elements: textElements(content) } }
}

function textBlock(content: string, blockType: number = BLOCK_TYPE.TEXT): Record<string, unknown> {
  const field: Record<number, string> = {
    [BLOCK_TYPE.TEXT]: "text",
    [BLOCK_TYPE.BULLET]: "bullet",
    [BLOCK_TYPE.ORDERED]: "ordered",
    [BLOCK_TYPE.QUOTE]: "quote",

  }
  return { block_type: blockType, [field[blockType] ?? "text"]: { elements: textElements(content) } }
}

/** 代码块语言名 → 飞书 code.style.language 数字枚举（B5：该字段是 int 枚举，传字符串报 99992402）。
 * 实测修正：26=JavaScript（用户以真实 API 验证；原 18=JavaScript 映射有误），
 * Markdown 枚举值未知，移除映射回退 PlainText(1)。 */
const CODE_LANG: Record<string, number> = {
  plaintext: 1, abap: 2, ada: 3, apache: 4, apex: 5, assemblylanguage: 6, bash: 7, csharp: 8, cpp: 9, css: 10, cobol: 11, commonlisp: 12, coq: 13, go: 14, haskell: 15, html: 16, java: 17, javascript: 26, json: 19, julia: 20, kotlin: 21, latex: 22, less: 23, lua: 24, makefile: 25, objectivec: 27, ocaml: 28, matlab: 29, openedgeabl: 30, perl: 31, php: 32, python: 34, protobuf: 35, r: 36, rust: 37, sas: 38, scala: 39, scheme: 40, scss: 41, shell: 42, sql: 43, svelte: 44, swift: 45, typescript: 46, visualbasic: 47, webassembly: 48, vue: 49, xlang: 50, yaml: 51,
  // 常见别名
  js: 26, ts: 46, py: 34, sh: 7, zsh: 7, golang: 14, "c++": 9, "c#": 8, "objective-c": 27, "obj-c": 27, kt: 21, yml: 51, dockerfile: 1, text: 1,
}

/** 语言名 → 枚举数字；未知语言回退 PlainText(1)。 */
export function codeLangEnum(lang: string | undefined): number {
  if (!lang) return 1
  return CODE_LANG[lang.trim().toLowerCase()] ?? 1
}

/** 列字母 → 数字（A=1，AA=27）。 */
export function colToNumber(col: string): number {
  let n = 0
  for (const ch of col.toUpperCase()) n = n * 26 + (ch.charCodeAt(0) - 64)
  return n
}

/** 数字 → 列字母（1=A，27=AA）。 */
export function numberToCol(n: number): string {
  let s = ""
  while (n > 0) {
    const m = (n - 1) % 26
    s = String.fromCharCode(65 + m) + s
    n = Math.floor((n - 1) / 26)
  }
  return s
}

/**
 * 追加 range 自动扩展（实测：values_append 的 range 只给起始格（如 Sheet1!A1）时，
 * 数据行数 > range 行数报 wrong range）：单格 → 覆盖 values 全尺寸的区域 `A1:{endCol}{endRow}`；
 * 已是区域（含 :）或非单格形式则原样返回。
 */
export function expandAppendRange(range: string, rows: number, cols: number): string {
  const m = range.match(/^(.+?)!([A-Za-z]+)(\d+)$/)
  if (!m || rows <= 0 || cols <= 0) return range
  const [, sheet, col, rowStr] = m
  const row = Number(rowStr)
  const endCol = numberToCol(colToNumber(col) + cols - 1)
  return `${sheet}!${col}${row}:${endCol}${row + rows - 1}`
}

/**
 * 块字段自动映射（实测：add_blocks 统一用 text 字段建 heading/bullet/todo 等报 invalid param）：
 * - 缺 block_type 时默认 text（2）
 * - 按 block_type 把通用 text 字段改写为对应驼峰字段（heading1..heading9/bullet/ordered/quote/todo/code）
 * - 字段值为字符串（如 {"block_type":2,"text":"hi"}）自动包装为 {elements:[{text_run:{content}}]}
 * - 有顶层 elements 无对应字段时自动包装（{"block_type":2,"elements":[...]} → text.elements）
 * - code 块的 language 字符串自动转数字枚举；分割线自动补 divider:{}
 * - todo 块显式 todo 对象（done/style）时：done 映射到 todo.style.done，text 合并进 todo.elements（实测 done 放 todo 顶层报 99992402）
 * - callout 块颜色/emoji 字段归一进 callout.style（实测放 callout 顶层报 schema mismatch）
 * 返回新对象，不修改入参。
 */
export function normalizeBlockFields(block: Record<string, unknown>): Record<string, unknown> {
  const b = { ...block }
  const type = b.block_type === undefined ? BLOCK_TYPE.TEXT : Number(b.block_type ?? 0)
  b.block_type = type
  // 字段值字符串 → 元素数组包装（飞书要求 {elements:[{text_run:{content}}]}，简化写法报 99992402）
  const wrapString = (field: string): void => {
    const v = b[field]
    if (typeof v === "string") b[field] = { elements: textElements(v) }
  }
  // 标题：3-11 → heading1..heading9
  if (type >= BLOCK_TYPE.HEADING1 && type <= BLOCK_TYPE.HEADING1 + 8) {
    const field = `heading${type - BLOCK_TYPE.HEADING1 + 1}`
    if (b[field] === undefined && b.text !== undefined) {
      b[field] = b.text
      delete b.text
    }
    wrapString(field) // 映射来源（text）可能是字符串，需再包装（与 fieldMap 分支一致）
    return b
  }
  // todo 块：显式 todo 对象（含 done/style）独立归一——
  // 实测：done 放 todo 顶层报 99992402（飞书结构为 todo:{elements:[...], style:{done:bool}}）；
  // text 快捷参数合并进 todo.elements；style 已含 done 时直接采用
  if (type === BLOCK_TYPE.TODO) {
    const todo = b.todo
    if (todo !== undefined && typeof todo === "object" && !Array.isArray(todo)) {
      const t = { ...(todo as Record<string, unknown>) }
      if (t.done !== undefined) {
        const style = t.style && typeof t.style === "object" && !Array.isArray(t.style) ? { ...(t.style as Record<string, unknown>) } : {}
        if (style.done === undefined) style.done = t.done
        t.style = style
        delete t.done
      }
      if (b.text !== undefined) {
        const els = Array.isArray(t.elements) ? [...(t.elements as unknown[])] : []
        els.push(...textElements(String(b.text)))
        t.elements = els
        delete b.text
      } else if (!Array.isArray(t.elements)) {
        t.elements = []
      }
      b.todo = t
    } else {
      // 简化写法：text → todo 字段（与 fieldMap 分支一致）
      if (b.todo === undefined && b.text !== undefined) {
        b.todo = b.text
        delete b.text
      }
      wrapString("todo")
      if (b.todo === undefined && b.elements !== undefined) {
        b.todo = { elements: b.elements }
        delete b.elements
      }
    }
    return b
  }
  // callout 高亮块（实测修正）：正文在 callout.elements（Text 结构），不在 children；
  // 颜色/emoji 字段必须放 callout.style 内（实测放 callout 顶层/块顶层报 schema mismatch）——
  // 兼容 callout.style 内 / callout 顶层 / 块顶层三种写法，统一归一进 callout.style
  if (type === BLOCK_TYPE.CALLOUT) {
    const callout = b.callout
    const styleKeys = ["background_color", "border_color", "text_color", "emoji_id"]
    if (callout !== undefined && typeof callout === "object" && !Array.isArray(callout)) {
      const c = { ...(callout as Record<string, unknown>) }
      if (b.text !== undefined) {
        const els = Array.isArray(c.elements) ? [...(c.elements as unknown[])] : []
        // text 兼容字符串与对象（elements 数组）：合并进 callout.elements
        if (typeof b.text === "string") els.push(...textElements(b.text))
        else if (b.text && typeof b.text === "object") {
          const t = b.text as Record<string, unknown>
          if (Array.isArray(t.elements)) els.push(...(t.elements as unknown[]))
        }
        c.elements = els
        delete b.text
      } else if (typeof c.elements === "string") {
        c.elements = textElements(String(c.elements))
      } else if (c.elements === undefined && b.elements !== undefined) {
        c.elements = b.elements
        delete b.elements
      }
      b.callout = c
    } else if (b.text !== undefined) {
      // 简化写法：text → callout 字段（与 fieldMap 分支一致：直接赋值 + 字符串包装）
      b.callout = b.text
      delete b.text
      wrapString("callout")
    }
    // 统一收口：颜色/emoji 字段归一进 callout.style（callout 顶层与块顶层两种来源都收敛）
    const finalCallout = b.callout
    if (finalCallout && typeof finalCallout === "object" && !Array.isArray(finalCallout)) {
      const c = { ...(finalCallout as Record<string, unknown>) }
      const st: Record<string, unknown> = c.style && typeof c.style === "object" && !Array.isArray(c.style) ? { ...(c.style as Record<string, unknown>) } : {}
      for (const k of styleKeys) {
        const v = c[k] !== undefined ? c[k] : b[k]
        if (v !== undefined && st[k] === undefined) st[k] = v
        delete c[k]
      }
      if (Object.keys(st).length) c.style = st
      b.callout = c
    }
    for (const k of styleKeys) delete b[k]
    // 实测 1770041 open schema mismatch：callout 正文在 elements，children 是错误用法（descendant 通道报错）——剥离
    delete b.children
    return b
  }
  const fieldMap: Record<number, string> = {
    [BLOCK_TYPE.BULLET]: "bullet",
    [BLOCK_TYPE.ORDERED]: "ordered",
    [BLOCK_TYPE.QUOTE]: "quote",
    [BLOCK_TYPE.CODE]: "code",
  }
  const field = fieldMap[type]
  if (field) {
    if (b[field] === undefined && b.text !== undefined) {
      b[field] = b.text
      delete b.text
    }
    wrapString(field) // 映射来源（text）可能是字符串，需再包装
    if (b[field] === undefined && b.elements !== undefined) {
      b[field] = { elements: b.elements }
      delete b.elements
    }
  } else if (type === BLOCK_TYPE.TEXT) {
    // text 类型：字段字符串包装 + 顶层 elements 包装
    wrapString("text")
    if (b.text === undefined && b.elements !== undefined) {
      b.text = { elements: b.elements }
      delete b.elements
    }
  }
  // 非文本容器类型（table/image/file/embed 等）不做 text/elements 兜底——
  // 实测修复：table 等块带顶层 elements/text 被强制转成 text 字段导致 invalid param
  // 分割线：块类型 22 需携带 divider:{} 字段（实测缺字段 invalid param）
  if (type === BLOCK_TYPE.DIVIDER && b.divider === undefined) {
    b.divider = {}
  }
  // code 块：language 字符串 → 数字枚举（深拷贝，避免污染调用方入参）；缺 style/language 补默认（实测缺字段 field validation failed）
  if (type === BLOCK_TYPE.CODE) {
    const code = b.code as Record<string, unknown> | undefined
    if (code && typeof code === "object" && !Array.isArray(code)) {
      const style = code.style && typeof code.style === "object" && !Array.isArray(code.style) ? { ...(code.style as Record<string, unknown>) } : {}
      style.language = typeof style.language === "string" ? codeLangEnum(String(style.language)) : (style.language ?? 1) // 缺省 PlainText(1)
      b.code = { ...code, style }
    }
  }
  return b
}

function codeBlock(lang: string | undefined, content: string): Record<string, unknown> {
  return {
    block_type: BLOCK_TYPE.CODE,
    code: { style: { language: codeLangEnum(lang) }, elements: [{ text_run: { content } }] },
  }
}

function dividerBlock(): Record<string, unknown> {
  return { block_type: BLOCK_TYPE.DIVIDER, divider: {} }
}

/** GitHub 告示语法（`> [!NOTE]` 等）→ callout 高亮块样式映射：背景用浅色系枚举、边框同色系（官方 CalloutBackgroundColor/CalloutBorderColor）。 */
const ALERT_CALLOUT_STYLE: Record<string, { background_color: number; border_color: number; emoji_id: string }> = {
  NOTE: { background_color: 5, border_color: 5, emoji_id: "bulb" },
  TIP: { background_color: 4, border_color: 4, emoji_id: "bulb" },
  IMPORTANT: { background_color: 6, border_color: 6, emoji_id: "pushpin" },
  WARNING: { background_color: 3, border_color: 3, emoji_id: "pushpin" },
  CAUTION: { background_color: 1, border_color: 1, emoji_id: "pushpin" },
}

/** 告示块（callout）：标题粗体置首 + 正文（行内样式），无标题/正文时保留空元素占位。 */
function calloutBlock(kind: string, title: string, body: string): Record<string, unknown> {
  const els: Record<string, unknown>[] = []
  if (title) els.push({ text_run: { content: title, text_element_style: { bold: true } } })
  els.push(...textElements(body))
  return { block_type: BLOCK_TYPE.CALLOUT, callout: { style: ALERT_CALLOUT_STYLE[kind], elements: els } }
}

/** Markdown 列表行解析（含缩进）：返回层级（缩进每 2 空格/1 tab 一级）、类型（bullet/ordered/todo）与文本。 */
interface ListLineInfo {
  depth: number
  kind: "bullet" | "ordered" | "todo"
  done: boolean
  text: string
}

function parseListLine(line: string): ListLineInfo | undefined {
  const m = /^(\s*)(?:[-*+]\s+(?:\[([ xX])\]\s+)?|(\d+)[.)]\s+)(.*)$/.exec(line)
  if (!m) return undefined
  const indent = m[1].replace(/\t/g, "  ")
  return {
    depth: Math.floor(indent.length / 2),
    kind: m[3] !== undefined ? "ordered" : m[2] !== undefined ? "todo" : "bullet",
    done: (m[2] ?? "").toLowerCase() === "x",
    text: m[4],
  }
}

/** 列表树节点（嵌套列表构建中间结构）。 */
interface ListNode {
  kind: "bullet" | "ordered" | "todo"
  done: boolean
  text: string
  children: ListNode[]
}

function listBlockOf(node: ListNode): Record<string, unknown> {
  if (node.kind === "todo") {
    return { block_type: BLOCK_TYPE.TODO, todo: { style: { done: node.done }, elements: textElements(node.text) } }
  }
  return textBlock(node.text, node.kind === "ordered" ? BLOCK_TYPE.ORDERED : BLOCK_TYPE.BULLET)
}

/** 递归发射列表树：子项作为父块 children 引用（descendant 接口一次创建嵌套列表）。 */
function emitListItem(node: ListNode, bb: BlockBuilder): string {
  const childIds = node.children.map((c) => emitListItem(c, bb))
  return bb.add(listBlockOf(node), childIds)
}

/** 表格 → 块组：table 块 + table_cell 块 + 单元格内文本块（官方推荐结构，单元格至少含一个空文本块）。 */
function tableGroup(rows: string[][], base: number): BlockGroup {
  const bb = new BlockBuilder(base)
  const columnSize = Math.max(...rows.map((r) => r.length), 1)
  const cellIds: string[][] = []
  for (const row of rows) {
    const rowCells: string[] = []
    for (let c = 0; c < columnSize; c++) {
      const content = row[c] ?? ""
      const textId = bb.add(textBlock(content))
      const cellId = bb.add({ block_type: BLOCK_TYPE.TABLE_CELL, table_cell: {} }, [textId], "cell")
      rowCells.push(cellId)
    }
    cellIds.push(rowCells)
  }
  const tableId = bb.add(
    {
      block_type: BLOCK_TYPE.TABLE,
      table: { property: { row_size: rows.length, column_size: columnSize, column_width: Array.from({ length: columnSize }, () => 100) } },
    },
    cellIds.flat(),
    "tbl",
  )
  return { rootId: tableId, blocks: bb.blocks }
}

function leafGroup(block: Record<string, unknown>, base: number): BlockGroup {
  const bb = new BlockBuilder(base)
  bb.add(block)
  return { rootId: bb.blocks[0].block_id as string, blocks: bb.blocks }
}

/**
 * table 块简化写法展开：`{"block_type":31,"table":{"rows":[["a","b"],["c","d"]]}}` →
 * table + table_cell + text 嵌套块（一次性生成完整表格结构，供 descendant 接口一次创建，避免逐格 N 次调用）。
 * 复用调用方 BlockBuilder 保证跨顶层块 id 全局唯一。
 */
function expandTableRows(block: Record<string, unknown>, bb: BlockBuilder): string {
  const table = block.table as Record<string, unknown> | undefined
  const rows = (table?.rows ?? []) as unknown[][]
  if (!Array.isArray(rows) || rows.length === 0) throw new Error("table.rows 必须是非空二维数组（每行为单元格字符串数组）")
  const columnSize = Math.max(...rows.map((r) => (Array.isArray(r) ? r.length : 0)), 1)
  const cellIds: string[][] = []
  for (const row of rows) {
    if (!Array.isArray(row)) throw new Error("table.rows 每行必须是单元格字符串数组")
    const rowCells: string[] = []
    for (let c = 0; c < columnSize; c++) {
      const textId = bb.add(textBlock(String(row[c] ?? "")))
      rowCells.push(bb.add({ block_type: BLOCK_TYPE.TABLE_CELL, table_cell: {} }, [textId], "cell"))
    }
    cellIds.push(rowCells)
  }
  return bb.add(
    {
      block_type: BLOCK_TYPE.TABLE,
      table: { property: { row_size: rows.length, column_size: columnSize, column_width: Array.from({ length: columnSize }, () => 100) } },
    },
    cellIds.flat(),
    "tbl",
  )
}

/**
 * 递归构建块组：块含 children（块对象数组）时逐层补 block_id 并把 children 改写为 id 引用
 * （descendant 接口要求）；忽略调用方传入的 block_id，强制生成全局唯一 id（防重复引用冲突）。
 * 返回该顶层块的 rootId（块本体已 push 进 bb.blocks）。
 */
/** grid 骨架化（实测修复）：grid_column 带 children 报 field validation failed、
 *  传 width_ratio 报 9499 invalid parameter——创建时剥离列内容与列宽（只建 grid+grid_column 骨架，
 *  列宽默认均分、列内默认文本块由飞书自动生成），内容创建后经 update_block 填充。
 *  返回剥离了内容的列数。 */
export function stripGridColumnContents(block: Record<string, unknown>): { block: Record<string, unknown>; fillColumns: number } {
  const b = { ...block }
  let fillColumns = 0
  if (Number(b.block_type ?? 0) === BLOCK_TYPE.GRID && Array.isArray(b.children)) {
    b.children = b.children.map((k) => {
      const col = { ...(k as Record<string, unknown>) }
      if (Number(col.block_type ?? 0) === BLOCK_TYPE.GRID_COLUMN) {
        // 实测 9499 invalid parameter：创建时不传 width_ratio（列宽默认均分）；
        // grid_column 字段保留为对象（无其他字段时为 {}，符合官方创建 schema）
        const gc = col.grid_column && typeof col.grid_column === "object" && !Array.isArray(col.grid_column) ? { ...(col.grid_column as Record<string, unknown>) } : {}
        delete gc.width_ratio
        col.grid_column = gc
        if (Array.isArray(col.children) && col.children.length > 0) {
          delete col.children
          fillColumns++
        }
      }
      return col
    })
  }
  return { block: b, fillColumns }
}

/** grid 结构校验（实测 1770041 open schema mismatch）：column_size 必须 2~5 且与 grid_column 子块数一致。 */
export function gridStructureError(block: Record<string, unknown>): string | null {
  if (Number(block.block_type ?? 0) !== BLOCK_TYPE.GRID) return null
  const columnSize = Number((block.grid as Record<string, unknown> | undefined)?.column_size ?? 0)
  if (!columnSize || columnSize < 2 || columnSize > 5) {
    return `grid.column_size 必须是 2~5 的数字（收到 ${columnSize || "缺失"}）——分栏最少 2 列最多 5 列`
  }
  const cols = (Array.isArray(block.children) ? block.children : []).filter(
    (k) => k && typeof k === "object" && Number((k as Record<string, unknown>).block_type ?? 0) === BLOCK_TYPE.GRID_COLUMN,
  )
  if (cols.length > 0 && cols.length !== columnSize) {
    return `grid.column_size（${columnSize}）与 grid_column 子块数（${cols.length}）不一致——每列必须有一个 grid_column 块（实测不一致报 1770041 open schema mismatch）`
  }
  return null
}

/** 创建接口不支持的块类型检查（含嵌套子树；实测：16 equation 不在创建枚举内，报 99992402 field validation failed）。 */
function uncreatableBlockType(block: unknown): string | null {
  if (!block || typeof block !== "object") return null
  const o = block as Record<string, unknown>
  if (Number(o.block_type ?? 0) === BLOCK_TYPE.EQUATION) {
    return "equation（公式块 16）不可通过 API 创建（官方创建接口枚举不含 16，实测 99992402 field validation failed）——请改用普通文本块表示公式，或提示用户手动插入公式块"
  }
  const kids = o.children
  if (Array.isArray(kids)) {
    for (const k of kids) {
      const err = uncreatableBlockType(k)
      if (err) return err
    }
  }
  return null
}

function buildGroup(block: Record<string, unknown>, bb: BlockBuilder): string {
  const b = { ...block }
  delete b.block_id
  const kids = b.children
  if (Array.isArray(kids) && kids.length > 0) {
    const childIds: string[] = []
    for (const k of kids) {
      // 仅接受块对象：字符串 id 引用（如 find_blocks 复制的旧 id）在本批中必然悬空，明确报错提示
      if (k && typeof k === "object") childIds.push(buildGroup(normalizeBlockFields(k as Record<string, unknown>), bb))
      else throw new Error("children 元素必须是块对象（嵌套结构请直接写块 JSON，不支持字符串 id 引用）")
    }
    delete b.children
    return bb.add(b, childIds)
  }
  return bb.add(b)
}

function isTableRow(line: string): boolean {
  return /^\|.*\|$/.test(line) || line.includes("|")
}

function parseTableRow(line: string): string[] {
  const t = line.trim().replace(/^\|/, "").replace(/\|$/, "")
  return t.split("|").map((c) => c.trim())
}

function isTableSeparator(line: string): boolean {
  const t = line.trim().replace(/^\|/, "").replace(/\|$/, "")
  return /^[\s:|-]+$/.test(t) && t.includes("-")
}

const LIST_RE = /^\s*(?:[-*+]\s+|(\d+)[.)]\s+)/
const TODO_RE = /^\s*[-*+]\s+\[([ xX])\]\s+(.*)/
const HEADING_RE = /^(#{1,9})\s+(.*)/
const DIVIDER_RE = /^(-{3,}|\*{3,}|_{3,})$/
const FENCE_RE = /^```(\w*)/
const ALERT_RE = /^\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*(.*)$/i

/**
 * 将 Markdown 文本转换为「创建嵌套块」接口所需的块组数组。
 * 支持：标题（1~9 级）/段落/有序无序列表（缩进嵌套，2 空格或 1 tab 一级）/任务列表/代码块/引用/
 * GitHub 告示（`> [!NOTE]` 等 → callout 高亮块）/表格/分割线/行内加粗粗斜体斜体删除线代码链接。
 * 每个块组自带 block_id 与 children 引用，可直接分批插入（单批 ≤1000 块）。
 */
export function markdownToBlocks(md: string): BlockGroup[] {
  const lines = md.replace(/\r\n/g, "\n").split("\n")
  const groups: BlockGroup[] = []
  let i = 0
  let idBase = 0
  while (i < lines.length) {
    const line = lines[i]
    const trimmed = line.trim()

    const fence = trimmed.match(FENCE_RE)
    if (fence) {
      const lang = fence[1] || undefined
      const code: string[] = []
      i++
      while (i < lines.length && !lines[i].trim().startsWith("```")) {
        code.push(lines[i])
        i++
      }
      i++ // 跳过结束 fence
      groups.push(leafGroup(codeBlock(lang, code.join("\n")), idBase))
      idBase += 1
      continue
    }

    const h = trimmed.match(HEADING_RE)
    if (h) {
      groups.push(leafGroup(headingBlock(h[1].length, h[2]), idBase))
      idBase += 1
      i++
      continue
    }

    if (DIVIDER_RE.test(trimmed)) {
      groups.push(leafGroup(dividerBlock(), idBase))
      idBase += 1
      i++
      continue
    }

    const listStart = parseListLine(line)
    if (listStart) {
      // 连续列表行 → 嵌套树（按缩进层级，跳级缩进归一为 +1 级，最深 9 级）
      const roots: ListNode[] = []
      const stack: Array<{ raw: number; clamped: number; node: ListNode }> = []
      while (i < lines.length) {
        const cur = parseListLine(lines[i])
        if (!cur) break
        const node: ListNode = { kind: cur.kind, done: cur.done, text: cur.text, children: [] }
        while (stack.length && stack[stack.length - 1].raw >= cur.depth) stack.pop()
        const clamped = Math.min(stack.length ? stack[stack.length - 1].clamped + 1 : 0, cur.depth, 9)
        ;(stack.length ? stack[stack.length - 1].node.children : roots).push(node)
        stack.push({ raw: cur.depth, clamped, node })
        i++
      }
      const bb = new BlockBuilder(idBase)
      for (const root of roots) {
        const start = bb.blocks.length
        const rootId = emitListItem(root, bb)
        groups.push({ rootId, blocks: bb.blocks.slice(start) })
      }
      idBase = bb.counter
      continue
    }

    if (trimmed.startsWith(">")) {
      const quote: string[] = []
      while (i < lines.length && lines[i].trim().startsWith(">")) {
        quote.push(lines[i].trim().replace(/^>\s?/, ""))
        i++
      }
      // GitHub 告示语法（首行 [!NOTE]/[!TIP]/[!IMPORTANT]/[!WARNING]/[!CAUTION]）→ callout 高亮块
      const alert = ALERT_RE.exec(quote[0] ?? "")
      if (alert) {
        groups.push(leafGroup(calloutBlock(alert[1].toUpperCase(), alert[2].trim(), quote.slice(1).join("\n")), idBase))
      } else {
        groups.push(leafGroup(textBlock(quote.join("\n"), BLOCK_TYPE.QUOTE), idBase))
      }
      idBase += 1
      continue
    }

    if (isTableRow(trimmed) && i + 1 < lines.length && isTableSeparator(lines[i + 1])) {
      const rows: string[][] = [parseTableRow(trimmed)]
      i++
      while (i < lines.length && isTableSeparator(lines[i])) i++
      while (i < lines.length && isTableRow(lines[i].trim()) && !isTableSeparator(lines[i].trim())) {
        rows.push(parseTableRow(lines[i].trim()))
        i++
      }
      const g = tableGroup(rows, idBase)
      groups.push(g)
      idBase += g.blocks.length
      continue
    }

    if (!trimmed) {
      i++
      continue
    }

    // 段落：合并连续非特殊行
    const para: string[] = [trimmed]
    i++
    while (
      i < lines.length &&
      lines[i].trim() &&
      !HEADING_RE.test(lines[i].trim()) &&
      !FENCE_RE.test(lines[i].trim()) &&
      !lines[i].trim().startsWith(">") &&
      !TODO_RE.test(lines[i].trim()) &&
      !LIST_RE.test(lines[i].trim()) &&
      !DIVIDER_RE.test(lines[i].trim()) &&
      !(isTableRow(lines[i].trim()) && i + 1 < lines.length && isTableSeparator(lines[i + 1]))
    ) {
      para.push(lines[i].trim())
      i++
    }
    groups.push(leafGroup(textBlock(para.join("\n")), idBase))
    idBase += 1
  }
  return groups
}
