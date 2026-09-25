import type {  Tool, ToolContext, ToolResult, DiagramFormat  } from "@gebai/sdk"
import { statSync, readFileSync } from "node:fs"
import { truncate } from "@gebai/sdk/node"
import type { ToolSchema } from "@gebai/sdk"
import { xmlToBlocks } from "./docx_xml"
import { readStyleDoc, styleDocList } from "./styles"
import { feishuFetch } from "../../core/shared/tls"
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

/** 服务端图表渲染能力（engine 视 ToolContext 扩展注入；受限环境可能不提供——届时图表围栏降级为代码块）。 */
type DiagramRendererCtx = {
  renderDiagram?: (code: string, opts?: { format?: DiagramFormat; background?: string; maxWidth?: number; maxHeight?: number }) => Promise<Uint8Array>
}

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

/** 目标文件夹配置变量名（子Agent 前缀优先，兼容全局命名）：创建的资源落在该文件夹下，用户在飞书里自动拥有权限。 */
const FOLDER_URL_VARS = ["FEISHU_DOCS_FOLDER_URL", "GEBAI_FEISHU_FOLDER_URL"]

/** 文件夹方案引导：用户在飞书侧一次性动作，此后创建的资源无需逐个分享/转移权限。 */
const FOLDER_ADVICE =
  "推荐「文件夹方案」：在你的飞书云空间新建一个文件夹 → 把本应用添加为协作者（可编辑）→ 把该文件夹 URL 配置到环境变量 FEISHU_DOCS_FOLDER_URL——此后机器人创建的资源都落在该文件夹下，你自动拥有全部权限"

/** 解析目标文件夹配置：接受文件夹 URL（路径含 /folder/{token}，查询串/锚点忽略）或裸 folder token；无法识别返回 null。 */
export function parseFolderToken(raw: string): string | null {
  const v = raw.trim()
  if (!v) return null
  const m = v.match(/^(?:https?:\/\/[^/]+)?\/(?:drive\/)?folder\/([A-Za-z0-9_-]+)/)
  if (m) return m[1]
  if (/^https?:\/\//i.test(v)) return null
  return /^[A-Za-z0-9_-]+$/.test(v) ? v : null
}

/** 权限受限判定：飞书权限错误码区间 / HTTP 403・404 / 无权限类文案（命中时附替代路径引导）。 */
function isPermissionBlocked(err: unknown): boolean {
  const msg = String((err as Error)?.message ?? err)
  const code = (err as { code?: number })?.code
  if (typeof code === "number" && code >= PERMISSION_CODE_RANGE[0] && code <= PERMISSION_CODE_RANGE[1]) return true
  return /\b(403|404)\b|9999166\d|9999167\d|1061002|1061004|forbidden|no permission/i.test(msg)
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
      throw new Error(`缺少飞书应用凭证：请配置环境变量 FEISHU_DOCS_APP_ID 与 FEISHU_DOCS_APP_SECRET（或全局兼容的 GEBAI_FEISHU_APP_ID / GEBAI_FEISHU_APP_SECRET）；可调用 ask 工具（name=FEISHU_DOCS_APP_ID，secret=true）请求用户直接填写`)
    }
    return { appId, appSecret }
  }

  /** 会话配置的目标文件夹 token（未配置返回 null；配置值既非 URL 也非 token 时报错，不静默忽略）。 */
  function readFolderToken(ctx: ToolContext): string | null {
    const raw = FOLDER_URL_VARS.map((k) => ctx.env[k]).find((v) => v && v.trim())
    if (!raw) return null
    const token = parseFolderToken(raw)
    if (!token) {
      throw new Error(`目标文件夹配置无效（${FOLDER_URL_VARS[0]}）: ${raw.slice(0, 120)}——请填飞书文件夹 URL（如 https://xxx.feishu.cn/drive/folder/fldcnXXXX）或直接填 folder token`)
    }
    return token
  }

  /** 创建类接口的目标文件夹：显式传参优先，其次会话配置的目标文件夹，都无则省略（落应用云空间根目录）。 */
  function targetFolder(ctx: ToolContext, explicit?: unknown): string | undefined {
    const arg = explicit === undefined || explicit === null ? "" : String(explicit).trim()
    if (arg) return arg
    return readFolderToken(ctx) ?? undefined
  }

  /** 创建结果落位说明：配置了目标文件夹 = 落在用户文件夹下用户直接可用；未配置 = 资源归应用所有并附配置引导。 */
  function folderNote(ctx: ToolContext, explicit?: unknown): string {
    const arg = explicit === undefined || explicit === null ? "" : String(explicit).trim()
    if (arg) return ""
    const token = readFolderToken(ctx)
    if (token) return `\n落位：已创建到配置的目标文件夹（folder_token=${token}），你本人在飞书可直接使用`
    return `\n落位：未配置目标文件夹（FEISHU_DOCS_FOLDER_URL），资源创建在应用云空间（归应用所有、你本人需被单独分享才能使用）。${FOLDER_ADVICE}`
  }

  /** 输出尾注追加（ToolResult 文本后接说明行；note 为空时原样返回）。 */
  function appendNote(res: ToolResult, note: string): ToolResult {
    return note ? { ...res, output: `${res.output}${note}` } : res
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

  /** 读取图源字节：本地路径（经会话目录解析）或 http(s) URL 下载；统一做空值与 20MB 上限校验（飞书 media 限制）。 */
  async function readImageBytes(ctx: ToolContext, src: string): Promise<{ bytes: Uint8Array; fileName: string }> {
    let bytes: Uint8Array
    let fileName: string
    if (/^https?:\/\//i.test(src)) {
      let res: Response
      try {
        res = await deps.fetchFn(src, { signal: AbortSignal.timeout(API_TIMEOUT_MS) })
      } catch (err) {
        throw new Error(`图片下载失败（网络不可达？）: ${src}（${(err as Error).message}）`)
      }
      if (!res.ok) throw new Error(`图片下载失败 HTTP ${res.status}: ${src}`)
      bytes = new Uint8Array(await res.arrayBuffer())
      const path = new URL(src).pathname
      fileName = decodeURIComponent(path.split("/").pop() || "image.png")
    } else {
      const path = ctx.resolvePath(src)
      bytes = new Uint8Array(await Bun.file(path).arrayBuffer())
      fileName = path.split(/[\\/]/).pop() || "image.png"
    }
    if (!bytes.length) throw new Error(`图片内容为空（文件不存在或下载失败？）: ${src}`)
    if (bytes.length > 20 * 1024 * 1024) throw new Error(`图片超过 20MB 上限: ${(bytes.length / 1024 / 1024).toFixed(1)}MB（${src}）`)
    return { bytes, fileName }
  }

  /** 上传图片素材并绑定到已有 image 块：multipart 上传（parent_type=docx_image + parent_node=块 id，
   *  云空间 file_token 不能直接用于文档 image 块）→ PATCH replace_image（width/height 由服务端识别）。 */
  async function fillImageBlock(ctx: ToolContext, docId: string, imageBlockId: string, bytes: Uint8Array, fileName: string): Promise<{ token: string; width?: number; height?: number }> {
    const form = new FormData()
    form.append("file_name", fileName)
    form.append("parent_type", "docx_image")
    form.append("parent_node", imageBlockId)
    form.append("size", String(bytes.length))
    form.append("file", new Blob([bytes], { type: "application/octet-stream" }), fileName)
    const up = (await api(ctx, "/open-apis/drive/v1/medias/upload_all", { method: "POST", form })) as { file_token?: string }
    if (!up.file_token) throw new Error("图片素材上传失败：响应缺少 file_token")
    const patched = (await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${imageBlockId}`, {
      method: "PATCH",
      body: { block_id: imageBlockId, replace_image: { token: up.file_token } },
    })) as { image?: { width?: number; height?: number } }
    return { token: up.file_token, width: patched?.image?.width, height: patched?.image?.height }
  }

  /** 剥离块上的本地元数据（`_` 前缀字段，如 Markdown 图片占位块的图源），不随请求发往飞书。 */
  function stripLocalMeta(blocks: Record<string, unknown>[]): Record<string, unknown>[] {
    return blocks.map((b) => {
      const local = Object.keys(b).filter((k) => k.startsWith("_"))
      if (!local.length) return b
      const c = { ...b }
      for (const k of local) delete c[k]
      return c
    })
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
    "创建飞书在线文档（docx）。返回 document_id 与文档 URL；缺省落在环境变量 FEISHU_DOCS_FOLDER_URL 配置的目标文件夹下（用户直接可用），未配置则应用云空间根目录（归应用所有）。",
    { title: { type: "string", description: "文档标题" }, folder_token: { type: "string", description: "目标文件夹 token（可选；缺省用 FEISHU_DOCS_FOLDER_URL 配置的文件夹，未配置则应用云空间根目录）" } },
    ["title"],
    async (args, ctx) => {
      const data = (await api(ctx, "/open-apis/docx/v1/documents", {
        method: "POST",
        body: { title: String(args.title), folder_token: targetFolder(ctx, args.folder_token) },
      })) as { document?: { document_id: string; title: string; url?: string } }
      return appendNote(await jsonResult(ctx, data.document ?? data, "创建成功"), folderNote(ctx, args.folder_token))
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
    "获取文档块结构。三种形态任选：**`outline=true` 只返回大纲**（标题层级/文本/block_id/每节顶层块数，大文档先看目录）；**`detail=compact` 返回紧凑块视图**（一行一块 `{缩进}{类型} [block_id] {摘要}`，表格不展开单元格——轻量读全文且每行直接带 id 可去改，适合「看完就改」）；缺省 `detail=full` 返回完整块 JSON（含样式/子块/原始字段，每块附 type_name 与块类型 43 画板占位）。块类型 43（mindnote 思维导图/画板）只返回 board.token 占位，其图形内容（UML 图等）用 get_board 工具读取。",
    {
      document_id: { type: "string" },
      outline: { type: "boolean", description: "只返回文档大纲（标题清单 + block_id + 每节顶层块数），用于先定位再按节读取（默认 false 返回全部块）" },
      detail: { type: "string", description: "full（默认，完整块 JSON）或 compact（一行一块 + block_id + 缩进层级，表格不展开；也可用 max_lines/text_limit 调整）" },
      max_lines: { type: "number", description: "detail=compact 时最多输出行数（默认 600，超出截断并提示）" },
      text_limit: { type: "number", description: "detail=compact 时每块文本摘要长度上限（默认 80 字符）" },
      page_token: { type: "string", description: "分页标记（可选）" },
      page_size: { type: "number", description: "每页块数，默认 100" },
      page_all: { type: "boolean", description: "自动翻页取全部块（默认 false，上限 2000）" },
    },
    ["document_id"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      if (args.detail === "compact") {
        const { items, truncated } = await collectPages(ctx, `/open-apis/docx/v1/documents/${docId}/blocks`, 500, 2000)
        if (!items.length) return { output: "文档为空（没有任何块）" }
        const view = compactBlocks(items, { textLimit: args.text_limit !== undefined ? num(args.text_limit, 80) : undefined, maxLines: args.max_lines !== undefined ? num(args.max_lines, 600) : undefined })
        const head = `紧凑块视图（${view.total} 个块${truncated ? "，⚠️ 已达 2000 块读取上限" : ""}，格式：类型 [block_id] 摘要）：`
        const tail = view.truncated ? `\n⚠️ 输出已达 max_lines 上限；用 max_lines 调大，或先 outline=true 定位再用 get_doc_text 读某节` : ""
        return truncate(`${head}\n${view.lines.join("\n")}${tail}`, "feishu_blocks_compact", ctx)
      }
      if (args.outline === true) {
        const { items, truncated } = await collectPages(ctx, `/open-apis/docx/v1/documents/${docId}/blocks`, 500, 2000)
        const { entries } = docOutline(items)
        if (!entries.length) return { output: `文档没有标题（共 ${items.length} 个块）——建议先按读者任务分节加标题，再用本模式定位` }
        const lines = entries.map((e) => `${"  ".repeat(Math.max(0, e.level - 1))}- h${e.level} ${e.text || "(无标题文本)"}  [${e.blockId}] 节内块 ${e.sectionBlocks}`)
        const capNote = truncated ? "；⚠️ 已达 2000 块读取上限，大纲可能不完整" : ""
        return { output: `文档大纲（${entries.length} 个标题${capNote}）：\n${lines.join("\n")}\n\n下一步：用 get_doc_text 传某个标题的 block_id 读整节，或用 find_blocks 找关键词定位。` }
      }
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
    "按文本关键词在文档中查找块，返回匹配块的 block_id/块类型(type_name)/文本/所在路径——按标题文本反查 block_id 的首选方式。传 `context_before`/`context_after` 时在命中块下展开同父相邻块（`▶` 标命中行、`·` 标上下文行），看清上下文再决定怎么改。",
    {
      document_id: { type: "string" },
      query: { type: "string", description: "搜索关键词（子串匹配，忽略大小写）" },
      block_type: { type: "string", description: "块类型过滤：数字或名称（heading/text/bullet/ordered/code/quote/todo/table 等）" },
      max_results: { type: "number", description: "最多返回条数（默认 20）" },
      context_before: { type: "number", description: "命中块前显示几个相邻块（同父兄弟，默认 0 = 只列命中块）" },
      context_after: { type: "number", description: "命中块后显示几个相邻块（同父兄弟，默认 0）" },
    },
    ["document_id", "query"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      const q = String(args.query ?? "").toLowerCase()
      if (!q) throw new Error("query 不能为空")
      const maxResults = Math.max(1, num(args.max_results, 20))
      const ctxBefore = Math.max(0, Math.min(20, num(args.context_before, 0)))
      const ctxAfter = Math.max(0, Math.min(20, num(args.context_after, 0)))
      let typeFilter: Set<number> | undefined
      if (args.block_type !== undefined) typeFilter = parseBlockTypeFilter(String(args.block_type))
      const items = (await collectPages(ctx, `/open-apis/docx/v1/documents/${docId}/blocks`, 500, 20_000)).items
      const byId = new Map(items.map((b) => [String(b.block_id), b]))
      // 同父兄弟序列（上下文展开用）：有父块用父的 children 顺序，顶层块用接口返回顺序
      const siblingsOf = (b: Record<string, unknown>): string[] => {
        const parent = b.parent_id ? byId.get(String(b.parent_id)) : undefined
        if (parent && Array.isArray(parent.children)) return (parent.children as unknown[]).map((c) => String(c))
        return items.map((x) => String(x.block_id ?? ""))
      }
      /** 单块一行（带类型/文本/可选 id）；mark 为行首标记（▶ 命中 / · 上下文）。
       *  容器块（无自身文本）用紧凑摘要补充（如 callout 的配色、表格的行列数）。 */
      const row = (b: Record<string, unknown>, mark?: string): string => {
        const type = Number(b.block_type ?? 0)
        const own = blockText(b).replace(/\s+/g, " ").trim().slice(0, 60)
        const isTextual = type === 2 || type === 14 || (type >= 3 && type <= 11)
        const summary = own || (isTextual ? "" : compactSummary(b, 60))
        return `${mark ? `${mark} ` : ""}[${blockTypeName(type)}] ${String(b.block_id ?? "")}${summary ? ` ${summary}` : ""}`
      }
      let count = 0
      const chunks: string[] = []
      for (const b of items) {
        if (typeFilter && !typeFilter.has(Number(b.block_type ?? 0))) continue
        const t = blockText(b)
        if (!t.toLowerCase().includes(q)) continue
        count++
        if (chunks.length >= maxResults) continue
        const head = `${chunks.length + 1}. ${b.block_id} | ${blockTypeName(Number(b.block_type ?? 0))}(${b.block_type}) | ${t.slice(0, 60)} | 路径: ${blockPath(b, byId)}`
        if (!ctxBefore && !ctxAfter) {
          chunks.push(head)
          continue
        }
        const ids = siblingsOf(b)
        const idx = ids.indexOf(String(b.block_id ?? ""))
        const before = idx > 0 ? ids.slice(Math.max(0, idx - ctxBefore), idx) : []
        const after = idx >= 0 ? ids.slice(idx + 1, idx + 1 + ctxAfter) : []
        const body = [
          ...before.map((id) => byId.get(id)).filter(Boolean).map((x) => `   ${row(x as Record<string, unknown>, "·")}`),
          `   ${row(b, "▶")}`,
          ...after.map((id) => byId.get(id)).filter(Boolean).map((x) => `   ${row(x as Record<string, unknown>, "·")}`),
        ]
        chunks.push(`${head}\n${body.join("\n")}`)
      }
      if (!count) return { output: `未找到包含「${args.query}」的块。可尝试：换关键词；get_doc_text 查看全文；get_doc_blocks detail=compact 查看紧凑结构。` }
      const tail = count > chunks.length ? `\n（共 ${count} 个匹配，仅显示前 ${chunks.length} 个）` : ""
      const hint = ctxBefore || ctxAfter ? "" : "\n提示：加 context_before/context_after 可在命中处展开相邻块看上下文。"
      return truncate(`找到 ${count} 个包含「${args.query}」的块：\n${chunks.join("\n")}${tail}${hint}`, "feishu_find", ctx)
    },
  )

  const addBlocks = tool(
    "add_blocks",
    "在文档指定块下添加子块，单次最多 50 块（超出自动分批）。\n支持的块类型：\n- 文本类：**普通文本用 2 text（1 是 page 根块，不接受 text 内容）**；3~11 heading1~9、12 bullet、13 ordered、14 code、15 quote、17 todo（todo.style.done 标记完成）、22 divider（**divider 直接 divider:{}，不要传空 text**）。字段用类型对应驼峰名（text/heading1/bullet/ordered/code/quote/todo/divider），统一传 text 字段会自动映射；code 块 language 支持语言名（**按飞书官方枚举表转数字**，未知回退 PlainText）、默认 `wrap=true` 自动换行（可传 `code.style.wrap=false` 关闭）。**16 equation 公式块不可经 API 创建（官方创建接口枚举不含 16，实测 99992402）——请改用普通文本块表示公式，或提示用户手动插入公式块**。\n- 表格 31：嵌套写法（table 带 children=[table_cell 块]）或简化写法 table.rows 二维数组（如 {\"block_type\":31,\"table\":{\"rows\":[[\"列A\",\"列B\"],[\"a1\",\"b1\"]]}}）。\n- 容器类（自动走创建嵌套块接口一次创建，追加到末尾、index 不生效）：19 callout 高亮块（**正文放 children 子块**——`callout.elements` 会被服务端忽略；**颜色/emoji 为 callout 顶层字段**（background_color/border_color/text_color 数字枚举、emoji_id）——`callout.style` 包裹会被忽略并回落默认配色；**必须至少一个子块**（空内容补空 text 子块，否则报 1770041）；text 快捷写法自动生成子块）；24 grid 分栏（grid.column_size 2~5 必填且等于 grid_column 子块数；**每个 grid_column 必须带** `grid_column:{width_ratio:<整数>}` **且至少一个子块**——实测 width_ratio 只接受整数（传 0.5 报 9499）、缺该字段或空列报 1770041；工具会自动把小数权重按比例换算为整数并补空列；列内可直接放段落/列表/待办等块，随分栏一次创建；列宽可再经 api_call 调 PATCH `.../blocks/{grid_id}` 传 `update_grid_column_width_ratio: {width_ratios: [整数权重数组]}` 调整）。\n- **表格列宽自动按内容自适应**（汉字计双宽、总宽 730px、单列下限 100px）：可用 table.column_width 显式指定每列 px（长度须等于列数）或 table.total_width 改目标总宽，table.header_row 设首行标题行；改**已有表格**的列宽/标题行用 set_table_width。\n- **复杂嵌套 JSON 请分批提交（每批少量块）或优先简化写法（text 快捷参数 / table.rows）**——长 JSON 易被模型输出截断导致解析失败。\n- 引用型（需先有云空间资源 token 或外部地址）：35 embed（embed.url 必填）、37 file（file.token）、39 sheet（sheet.token）、43 mindnote（mindnote.token，思维导图/画板）、44 bitable（bitable.token，多维表格）、46 diagram（diagram.diagram_type）。\n- 图片 27 请用 insert_image 工具（三步流程，add_blocks 不支持）；32 table_cell 不可单独创建（须随 table）。",
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
            // grid 归一（实测）：每列补 width_ratio 与空子块——缺一即报 1770041
            const prepared = prepareGridColumns(bo)
            if (prepared.filledColumns > 0) {
              fillNote = `；分栏空列已补空段落（平台要求每列至少一个子块）`
            }
            rootId = buildGroup(prepared.block, bb)
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
    "更新文档块内容：文本（text 整体替换 / insert_text 插入）或表格属性（table_property 直通 update_table_property，如 {column_index, column_width} 逐列改宽、{header_row} / {header_column}、{insert_table_row} / {insert_table_column} / {delete_table_rows} 增删行列；批量改宽用 set_table_width）。",
    {
      document_id: { type: "string" },
      block_id: { type: "string" },
      text: { type: "string", description: "替换后的文本（支持行内 Markdown 语法）" },
      insert_text: { type: "string", description: "要插入的文本" },
      insert_index: { type: "number", description: "插入位置（缺省 0=开头）" },
      style: { type: "object", description: "文本样式 {bold,italic,underline,strikethrough,inline_code}，需配合 text/insert_text" },
      table_property: { type: "object", description: "表格属性（仅表格块 31）——原样作为 update_table_property 提交，如 {column_index:0,column_width:320} 改第 0 列宽、{header_row:true} 首行标题行、{insert_table_row:{row_index:-1}} 末尾插行" },
    },
    ["document_id", "block_id"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      const blockId = String(args.block_id)
      const style = args.style !== undefined ? (jsonArg(args.style, "style") as Record<string, unknown>) : undefined
      let patch: Record<string, unknown> = { block_id: blockId }
      let insertIndex = 0
      if (args.table_property !== undefined) {
        patch.update_table_property = jsonArg(args.table_property, "table_property") as Record<string, unknown>
      } else if (args.insert_text !== undefined) {
        insertIndex = args.insert_index !== undefined ? num(args.insert_index, 0) : 0
        patch.insert_text = { index: insertIndex, elements: textElements(String(args.insert_text), style) }
      } else if (args.text !== undefined) {
        patch.update_text_elements = { elements: textElements(String(args.text), style) }
      } else {
        throw new Error("请提供 text / insert_text / table_property 之一")
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

  const replaceText = tool(
    "replace_text",
    "跨块查找替换文本（不改变块结构与行内样式）：先 `dry_run=true` 看命中清单（块 id/类型/次数/预览），确认后正式替换（一次 ``batch_update`` 批量提交，比逐块 update_block 快）。适用场景：同一个术语/名称/口径在全文多处要统一改。**跨样式片段**（命中跨越加粗/链接等样式边界）无法逐段替换，会在报告里单列并给出块 id（改用 update_block 整块重写）。",
    {
      document_id: { type: "string" },
      pattern: { type: "string", description: "要查找的文本（默认按字面匹配；regex=true 时按正则）" },
      replacement: { type: "string", description: "替换为的文本（空字符串 = 删除命中内容）" },
      regex: { type: "boolean", description: "pattern 按正则解释（默认 false 字面匹配）" },
      block_ids: { type: "array", items: { type: "string" }, description: "只在这些块内替换（缺省全文）" },
      limit: { type: "number", description: "最多更新多少个块（防止一次改太多，缺省不限）" },
      dry_run: { type: "boolean", description: "只预览命中不写入（默认 false）" },
    },
    ["document_id", "pattern", "replacement"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      const { items } = await collectPages(ctx, `/open-apis/docx/v1/documents/${docId}/blocks`, 500, 2000)
      const plan = replaceInBlocks(items, {
        pattern: String(args.pattern),
        replacement: String(args.replacement),
        regex: args.regex === true,
        blockIds: Array.isArray(args.block_ids) ? (args.block_ids as unknown[]).map((x) => String(x)) : undefined,
        limit: args.limit !== undefined ? num(args.limit, 0) : undefined,
      })
      const hitLines = plan.hits.map((h) => `- [${h.typeName}] ${h.blockId}：${h.count} 处 → ${h.preview}`)
      const crossLines = plan.crossRun.map((h) => `- [${h.typeName}] ${h.blockId}：命中跨样式片段（${h.count} 处），逐段替换无法完成 → 用 update_block 整块重写；片段：${h.preview}`)
      if (args.dry_run === true) {
        if (!plan.total) return { output: `未命中：全文没有与「${String(args.pattern)}」匹配的文本（统计 ${items.length} 个块；可检查大小写/全角半角，或改用 regex=true）` }
        const parts = [`预演（dry_run，未写入）：共命中 ${plan.total} 处，涉及 ${plan.hits.length + plan.crossRun.length} 个块`]
        if (hitLines.length) parts.push(`可替换 ${plan.hits.length} 个块：\n${hitLines.join("\n")}`)
        if (crossLines.length) parts.push(`需手工处理 ${plan.crossRun.length} 个块：\n${crossLines.join("\n")}`)
        if (plan.skipped) parts.push(`因 limit 跳过 ${plan.skipped} 个块`)
        parts.push("确认无误后用 dry_run=false 正式替换。")
        return { output: parts.join("\n") }
      }
      if (!plan.updates.length) return { output: `未执行替换：可替换命中 0 处${plan.crossRun.length ? `（有 ${plan.crossRun.length} 个块命中跨样式片段，需用 update_block 手工改）` : ""}` }
      let ok = 0
      const failures: string[] = []
      for (let i = 0; i < plan.updates.length; i += 20) {
        const batch = plan.updates.slice(i, i + 20)
        try {
          await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/batch_update`, {
            method: "PATCH",
            body: { requests: batch.map((u) => ({ block_id: u.blockId, update_text_elements: { elements: u.elements } })) },
          })
          ok += batch.length
        } catch (err) {
          // 整批失败时逐个重试（定位到具体块，并把失败块如实报出，不整批丢弃）
          for (const u of batch) {
            try {
              await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${u.blockId}`, {
                method: "PATCH",
                body: { block_id: u.blockId, update_text_elements: { elements: u.elements } },
              })
              ok++
            } catch (err2) {
              failures.push(`${u.blockId}（${(err2 as Error).message.slice(0, 120)}）`)
            }
          }
        }
        if (i + 20 < plan.updates.length) await new Promise((r) => setTimeout(r, 350))
      }
      const parts = [`✓ 已替换 ${ok}/${plan.updates.length} 个块（共 ${plan.total} 处命中）`]
      if (failures.length) parts.push(`未成功 ${failures.length} 个：${failures.join("；")}`)
      if (plan.crossRun.length) parts.push(`跨样式片段未处理 ${plan.crossRun.length} 个块：${plan.crossRun.map((h) => h.blockId).join("、")}（用 update_block 整块重写）`)
      if (plan.skipped) parts.push(`因 limit 跳过 ${plan.skipped} 个块`)
      parts.push("建议：重新跑 lint_doc 确认无回归。")
      return { output: parts.join("\n") }
    },
  )

  const setTableWidth = tool(
    "set_table_width",
    "重设文档中表格的列宽——修复接口默认列宽（每列 100px）导致的窄列长条。columns 缺省时按单元格内容自适应分配（汉字计双宽、总宽 730px、单列下限 100px，与 Markdown 导入表格同一算法）；total_width 指定自适应目标总宽；columns 显式指定每列宽度（px，长度须等于列数）。接口一次只能改一列，工具内部逐列串行提交（遵守文档编辑 3 次/秒限频）。",
    {
      document_id: { type: "string" },
      block_id: { type: "string", description: "表格块的 block_id（可用 find_blocks 按 type=table 反查，或从 get_doc_blocks 的 type_name=table 块取）" },
      columns: { type: "array", items: { type: "number" }, description: "显式列宽数组（px，每列不低于 50），长度须等于列数；缺省按内容自适应" },
      total_width: { type: "number", description: "自适应时的目标总宽 px（缺省 730 = 文档正文宽度）" },
      header_row: { type: "boolean", description: "首行设为标题行（加粗 + 底色）" },
      header_column: { type: "boolean", description: "首列设为标题列" },
    },
    ["document_id", "block_id"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      const blockId = String(args.block_id)
      const { items } = await collectPages(ctx, `/open-apis/docx/v1/documents/${docId}/blocks`, 500, 2000)
      const byId = new Map(items.map((b) => [String(b.block_id), b]))
      const table = byId.get(blockId)
      if (!table) throw new Error(`文档 ${docId} 中不存在块 ${blockId}（先用 get_doc_blocks / find_blocks 确认表格块的 block_id）`)
      const type = Number(table.block_type ?? 0)
      if (type !== BLOCK_TYPE.TABLE) throw new Error(`块 ${blockId} 不是表格块（实际类型 ${blockTypeName(type)}）——set_table_width 只作用于 block_type=31 的表格`)
      const prop = ((table.table ?? {}) as Record<string, unknown>).property as Record<string, unknown> | undefined
      const columnSize = Math.max(Number(prop?.column_size ?? 0), 1)
      let widths: number[]
      if (args.columns !== undefined) {
        const raw = jsonArg(args.columns, "columns")
        if (!Array.isArray(raw)) throw new Error("columns 必须是数字数组（每列宽度 px）")
        if (raw.length !== columnSize) throw new Error(`columns 长度（${raw.length}）必须与表格列数（${columnSize}）一致`)
        widths = raw.map((v) => Math.round(Number(v)))
        if (widths.some((w) => !Number.isFinite(w) || w < 50)) throw new Error("columns 每列宽度必须是不小于 50 的数字（平台下限 50px）")
      } else {
        widths = tableColumnWidths(tableCellRows(table, byId), args.total_width !== undefined ? Number(args.total_width) : TABLE_PAGE_WIDTH)
      }
      for (let i = 0; i < widths.length; i++) {
        await docxCall(ctx, docId, blockId, {}, () =>
          api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}`, {
            method: "PATCH",
            body: { update_table_property: { column_index: i, column_width: widths[i] } },
          }),
        )
        // 文档编辑限频 3 次/秒：逐列串行并留间隔，避免 429
        if (i < widths.length - 1) await new Promise((r) => setTimeout(r, 350))
      }
      if (args.header_row !== undefined || args.header_column !== undefined) {
        const propBody: Record<string, unknown> = {}
        if (args.header_row !== undefined) propBody.header_row = Boolean(args.header_row)
        if (args.header_column !== undefined) propBody.header_column = Boolean(args.header_column)
        await docxCall(ctx, docId, blockId, {}, () =>
          api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}`, { method: "PATCH", body: { update_table_property: propBody } }),
        )
      }
      const notes = [
        args.header_row !== undefined ? `首行标题行=${Boolean(args.header_row)}` : "",
        args.header_column !== undefined ? `首列标题列=${Boolean(args.header_column)}` : "",
      ].filter(Boolean)
      return { output: `✓ 表格 ${blockId} 列宽已设为 [${widths.join(", ")}]（${columnSize} 列，总宽 ${widths.reduce((a, b) => a + b, 0)}px）${notes.length ? `；${notes.join("、")}` : ""}` }
    },
  )

  /** 分批将块组插入文档（创建嵌套块接口，单次 ≤1000 块；组不跨批切分，组内 children 引用批内自洽）。
   *  返回插入数量与该批「临时 id → 真实 block_id」映射（图片占位块插入后回填素材用）。 */
  async function insertGroups(ctx: ToolContext, docId: string, parent: string, groups: BlockGroup[], index?: number): Promise<{ count: number; relations: Map<string, string> }> {
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
    const relations = new Map<string, string>()
    for (let i = 0; i < batches.length; i++) {
      const body: Record<string, unknown> = {
        children_id: batches[i].map((g) => g.rootId),
        descendants: batches[i].flatMap((g) => stripLocalMeta(g.blocks)),
      }
      if (i === 0 && index !== undefined) body.index = index
      const res = (await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${parent}/descendant`, { method: "POST", body })) as
        | { block_id_relations?: Array<{ block_id?: string; temporary_block_id?: string }> }
        | undefined
      for (const rel of res?.block_id_relations ?? []) {
        if (rel.temporary_block_id && rel.block_id) relations.set(rel.temporary_block_id, rel.block_id)
      }
    }
    return { count: groups.length, relations }
  }

  /** 图表占位块渲染 + 图片素材回填的共享管线（import_markdown / import_xml 共用）：
   *  渲染图表占位块（ctx.renderDiagram；渲染器缺失或失败时就地降级为同语言代码块，源码不丢）
   *  → 插入块组 → 按 block_id 关系回填图片素材（单条失败只记录，不中断整篇导入）。 */
  async function insertWithMedia(
    ctx: ToolContext,
    docId: string,
    parent: string,
    groups: BlockGroup[],
    opts: { keepDiagramSource?: boolean } = {},
  ): Promise<{ count: number; images: Array<{ src: string; ok: boolean; note: string }>; diagrams: Array<{ format: string; ok: boolean; note: string }> }> {
    const images: Array<{ src: string; ok: boolean; note: string }> = []
    const diagrams: Array<{ format: string; ok: boolean; note: string }> = []
    const renderer = (ctx as ToolContext & DiagramRendererCtx).renderDiagram
    const prepared: BlockGroup[] = []
    let srcSeq = 0
    for (const g of groups) {
      prepared.push(g)
      for (const block of g.blocks) {
        const format = block._diagram_format
        if (typeof format !== "string") continue
        const code = String(block._diagram_code ?? "")
        const degrade = (reason: string): void => {
          // 渲染错误可能多行（如 Mermaid 的 parse error 示意图）：折成单行保持输出紧凑
          diagrams.push({ format, ok: false, note: reason.replace(/\s+/g, " ").trim() })
          delete block._diagram_format
          delete block._diagram_code
          block.block_type = BLOCK_TYPE.CODE
          block.code = { style: { language: codeLangEnum(format), wrap: true }, elements: [{ text_run: { content: code } }] }
        }
        if (!renderer) {
          degrade("当前环境未提供服务端图表渲染，已保留为代码块")
          continue
        }
        try {
          const png = await renderer(code, { format: format as DiagramFormat })
          block._image_bytes = png
          block._image_name = `${format}.png`
          diagrams.push({ format, ok: true, note: `${(png.length / 1024).toFixed(0)}KB` })
          // keep：图下方再留一份源码代码块（独立块组，插在图之后）——id 用独立前缀避开占位块数字 id
          if (opts.keepDiagramSource) {
            const id = `diagsrc${++srcSeq}`
            prepared.push({ rootId: id, blocks: [{ block_id: id, ...codeBlock(format, code), children: [] }] })
          }
        } catch (err) {
          degrade((err as Error).message.slice(0, 200))
        }
      }
    }
    // 内联 SVG（<whiteboard type="svg">…</whiteboard>）：转字节后走图片回填管线（与本地图片同一通道）
    for (const g of prepared) {
      for (const block of g.blocks) {
        const svg = block._svg_inline
        if (typeof svg !== "string") continue
        delete block._svg_inline
        block._image_bytes = new TextEncoder().encode(svg)
        if (typeof block._image_name !== "string") block._image_name = "diagram.svg"
      }
    }
    const res = await insertGroups(ctx, docId, parent, prepared)
    for (const block of prepared.flatMap((g) => g.blocks)) {
      const bytes = block._image_bytes instanceof Uint8Array ? block._image_bytes : undefined
      const src = typeof block._image_src === "string" ? block._image_src : undefined
      if (!bytes && !src) continue
      const label = src ?? (typeof block._image_name === "string" ? block._image_name : "图表")
      const realId = res.relations.get(String(block.block_id))
      if (!realId) {
        images.push({ src: label, ok: false, note: "未拿到块 id" })
        continue
      }
      try {
        if (bytes) {
          const done = await fillImageBlock(ctx, docId, realId, bytes, typeof block._image_name === "string" ? block._image_name : "diagram.png")
          images.push({ src: label, ok: true, note: done.width ? `${done.width}×${done.height}` : "已上传" })
        } else {
          const { bytes: imgBytes, fileName } = await readImageBytes(ctx, src!)
          const done = await fillImageBlock(ctx, docId, realId, imgBytes, fileName)
          images.push({ src: label, ok: true, note: done.width ? `${done.width}×${done.height}` : "已上传" })
        }
      } catch (err) {
        images.push({ src: label, ok: false, note: (err as Error).message })
      }
    }
    return { count: res.count, images, diagrams }
  }

  /** 写入前媒体预检（dry_run 用）：图片路径/大小/可达性、内联 SVG 合法性、图表试渲染。
   *  只返回问题清单，不发写入请求——把错在产生空文档之前就暴露出来。 */
  async function precheckMedia(
    ctx: ToolContext,
    blocks: Array<Record<string, unknown>>,
    renderer: ((code: string, opts?: { format?: DiagramFormat }) => Promise<Uint8Array>) | undefined,
    keepSource: boolean,
  ): Promise<string[]> {
    const problems: string[] = []
    const images: string[] = []
    const diagrams: Array<{ format: string; code: string }> = []
    let svgCount = 0
    const walk = (list: Array<Record<string, unknown>>): void => {
      for (const b of list) {
        const src = b._image_src
        if (typeof src === "string") images.push(src)
        if (typeof b._svg_inline === "string") svgCount++
        if (typeof b._diagram_format === "string") diagrams.push({ format: b._diagram_format, code: String(b._diagram_code ?? "") })
        const kids = b.children
        if (Array.isArray(kids)) walk(kids as Array<Record<string, unknown>>)
      }
    }
    walk(blocks)
    const maxBytes = 20 * 1024 * 1024
    for (const src of images) {
      if (/^https?:\/\//i.test(src)) {
        try {
          const res = await deps.fetchFn(src, { signal: AbortSignal.timeout(8000) })
          if (!res.ok) problems.push(`网络图片不可访问（HTTP ${res.status}）：${src}`)
          else {
            const len = Number(res.headers.get("content-length") ?? 0)
            if (len > maxBytes) problems.push(`网络图片超过 20MB 上限：${src}`)
          }
        } catch (err) {
          problems.push(`网络图片下载失败（网络不可达或超时）：${src}（${(err as Error).message.slice(0, 80)}）`)
        }
        continue
      }
      try {
        const st = statSync(ctx.resolvePath(src))
        if (!st.isFile()) problems.push(`图片路径不是文件：${src}`)
        else if (st.size === 0) problems.push(`图片文件为空：${src}`)
        else if (st.size > maxBytes) problems.push(`图片超过 20MB 上限：${src}（${(st.size / 1024 / 1024).toFixed(1)}MB）`)
      } catch {
        problems.push(`图片文件不存在（相对当前工作目录解析）：${src}`)
      }
    }
    if (svgCount > 0) problems.push(`内含 ${svgCount} 个内联 SVG：将作为图片插入（保真显示，不可编辑为画板）——确认这是预期效果`)
    for (const { format, code } of diagrams) {
      if (!renderer) {
        problems.push(`当前环境未提供图表渲染（${format}）——正式导入会降级为代码块`)
        continue
      }
      try {
        const png = await renderer(code, { format: format as DiagramFormat })
        problems.push(`[信息] ${format} 图表可渲染（${(png.length / 1024).toFixed(0)}KB）${keepSource ? "；diagram_source=keep 会在图下附源码" : ""}`)
      } catch (err) {
        problems.push(`${format} 图表渲染失败（会降级为代码块）：${(err as Error).message.replace(/\s+/g, " ").slice(0, 160)}`)
      }
    }
    return problems
  }

  const importMarkdown = tool(
    "import_markdown",
    "将 Markdown 文本导入为飞书文档：不传 document_id 则新建文档（title 必填，**首行 H1 与 title 重复时自动去重**），否则追加到现有文档末尾。自动转换：多级标题（#~#########，1~9 级）/段落（**行首两个全角空格 = 首行缩进**）/有序无序列表（**缩进嵌套**，每 2 空格或 1 tab 一级；**有序列表保留起始编号**）/任务列表（- [ ] / - [x]）/代码块（按官方枚举表标注语言、默认自动换行）/**图表围栏（```mermaid / ```plantuml / ```d2 / ```echarts 代码块）→ 服务端本地渲染为 PNG 图片插入文档（无需联网；渲染不可用或失败时保留为代码块；diagram_source=keep 可在图下再留源码）**/引用（**引用内代码围栏转行内代码样式**——quote 块平台不支持子块）/GitHub 告示（`> [!NOTE]`/`[!TIP]`/`[!IMPORTANT]`/`[!WARNING]`/`[!CAUTION]` → 高亮块 callout，自动配色）/表格（列宽自适应、首行标题行；**单元格内 `\|` 为字面竖线、反引号内 `|` 不切列，`<br>` 单元格内换行、连续两个 `<br>` 转多段落**）/**图片（独立成行的 `![说明](本地路径或URL)` → 上传素材插入，单张 ≤20MB）**/行内加粗斜体粗斜体删除线行内代码链接。**生成整篇文档或大段内容时优先用本工具**（Markdown 一次成型，排版能力最全）。返回 document_id。",
    {
      content: { type: "string", description: "Markdown 文本" },
      document_id: { type: "string", description: "目标文档（缺省新建）" },
      title: { type: "string", description: "新建时的文档标题（document_id 缺省时必填）" },
      folder_token: { type: "string", description: "新建时的目标文件夹 token（可选；缺省用 FEISHU_DOCS_FOLDER_URL 配置的文件夹，未配置则应用云空间根目录）" },
      engine: { type: "string", description: "local（默认，本地转换）或 official（官方转换通道，支持更复杂的 Markdown）" },
      diagram_source: { type: "string", description: "图表源码保留策略：hide（默认，只插入渲染图）或 keep（图下方再留一份源码代码块，便于后续修改）" },
    },
    ["content"],
    async (args, ctx) => {
      const rawContent = String(args.content)
      let docId = args.document_id ? String(args.document_id) : ""
      // 新建文档时去掉与文档标题重复的首行 H1（飞书文档已有 title 字段，保留会重复标题层级）
      const content = docId ? rawContent : dropDuplicateTitleHeading(rawContent, String(args.title ?? "未命名文档"))
      const createdDoc = !docId
      if (!docId) {
        const created = (await api(ctx, "/open-apis/docx/v1/documents", {
          method: "POST",
          body: { title: String(args.title ?? "未命名文档"), folder_token: targetFolder(ctx, args.folder_token) },
        })) as { document?: { document_id: string } }
        docId = created.document?.document_id ?? ""
        if (!docId) throw new Error("创建文档失败：响应缺少 document_id")
      }
      const parent = await rootBlockId(ctx, docId)
      let count = 0
      let engineNote = ""
      const images: Array<{ src: string; ok: boolean; note: string }> = []
      const diagrams: Array<{ format: string; ok: boolean; note: string }> = []
      /** 导入管线：图表渲染 + 图片回填（与 import_xml 共用 insertWithMedia） */
      const runImport = async (groups: BlockGroup[]): Promise<void> => {
        const res = await insertWithMedia(ctx, docId, parent, groups, { keepDiagramSource: args.diagram_source === "keep" })
        count = res.count
        images.push(...res.images)
        diagrams.push(...res.diagrams)
      }
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
          await runImport(groups)
        } catch (err) {
          // B5：official 引擎对复杂组合（代码块+表格等）报 schema mismatch（1770041）等转换错误：
          // 自动回退本地转换保证导入可用（同一文档继续写入）
          if (!/1770041|99992402/.test((err as Error).message)) throw err
          const groups = markdownToBlocks(content)
          if (!groups.length) throw new Error("Markdown 内容为空，无可导入块")
          await runImport(groups)
          engineNote = "\n（official 引擎转换失败已自动回退本地转换；代码块与表格组合建议直接用 local 引擎）"
        }
      } else {
        const groups = markdownToBlocks(content)
        if (!groups.length) throw new Error("Markdown 内容为空，无可导入块")
        await runImport(groups)
      }
      const failed = images.filter((x) => !x.ok)
      const imageNote = images.length
        ? `\n图片: ${images.length - failed.length}/${images.length} 已插入${failed.length ? `\n未插入: ${failed.map((x) => `${x.src}（${x.note}）`).join("；")}` : ""}`
        : ""
      const missed = diagrams.filter((x) => !x.ok)
      const diagramNote = diagrams.length
        ? `\n图表: ${diagrams.length - missed.length}/${diagrams.length} 已渲染为图片${missed.length ? `\n未渲染（已保留为代码块）: ${missed.map((x) => `${x.format}（${x.note}）`).join("；")}` : ""}`
        : ""
      const folderDash = createdDoc ? folderNote(ctx, args.folder_token) : ""
      return { output: `✓ 已导入 ${count} 个顶层块 → document_id: ${docId}\nURL: https://feishu.cn/docx/${docId}${diagramNote}${imageNote}${engineNote}${folderDash}` }
    },
  )

  const importXml = tool(
    "import_xml",
    "将 XML 排版语法（类 HTML）一次导入为飞书文档：不传 document_id 则新建（标题取参数或 XML 内 `<title>`）。**整篇创作首选**——相比 import_markdown 多出：标题自动编号（`seq=\"auto\"` → 1 / 1.1 / 1.1.1）、分栏 `<grid>`、高亮块配色 `<callout>`、表格列宽 `<colgroup>`、图片/代码题注、`<whiteboard type=\"mermaid\">` 图示（也支持 `path=\"@./a.mmd\"` 引用本地源码与 `type=\"svg\"` 直传）、`<cite type=\"user\" user-id=\"ou_…\"/>` @人。动笔前先 style_guide 读 style 与对应体裁契约；标签清单见 style_guide(name=\"xml\")。**结构较大或含图片/图示时先传 `dry_run=true` 预检**（出块画像与图片/图表检查，零写入，不创建空文档）；导入后跑 lint_doc 体检并精修。返回 document_id 与降级说明。",
    {
      content: { type: "string", description: "XML 排版内容（`<title>` / `<h1 seq=\"auto\">` / `<p>` / `<ul>` / `<table>` / `<callout>` / `<grid>` / `<img>` / `<whiteboard>` / `<cite>` 等）" },
      document_id: { type: "string", description: "目标文档（缺省新建；传则追加到文末）" },
      title: { type: "string", description: "新建时的文档标题（缺省取 XML 内 <title>，再退到「未命名文档」）" },
      folder_token: { type: "string", description: "新建时的目标文件夹 token（可选；缺省用 FEISHU_DOCS_FOLDER_URL）" },
      diagram_source: { type: "string", description: "图表源码保留策略：hide（默认）或 keep（图下方再留源码代码块）" },
      dry_run: { type: "boolean", description: "只做预检不写入（默认 false）：解析 XML + 出块画像（顶层块数/字数/类型分布）+ 检查图片路径与大小、试渲染图表，返回问题清单——用于写入前把错就地改掉" },
    },
    ["content"],
    async (args, ctx) => {
      // path= 引用的本地文件（图表源码 / SVG）在解析阶段读入；读取失败只记说明，不中断
      const readLocal = (p: string): string | undefined => {
        try {
          return readFileSync(ctx.resolvePath(p), "utf8")
        } catch {
          return undefined
        }
      }
      const parsed = xmlToBlocks(String(args.content), { title: args.title !== undefined ? String(args.title) : undefined, readFile: readLocal })
      if (args.dry_run === true) {
        const profile = xmlProfile(parsed.blocks)
        const problems: string[] = []
        const typeLines = Object.entries(profile.counts)
          .sort((a, b) => b[1] - a[1])
          .map(([k, v]) => `${k} ${v}`)
          .join(" / ")
        // 图片与本地图表文件预检
        for (const issue of await precheckMedia(ctx, parsed.blocks, (ctx as ToolContext & DiagramRendererCtx).renderDiagram, args.diagram_source === "keep")) problems.push(issue)
        const head = `预检通过项：顶层块 ${profile.topLevel}、总块 ${profile.total}、字数约 ${profile.chars}（${typeLines}）${parsed.title ? `、标题「${parsed.title}」` : "、⚠️ 未提供标题（新建时需传 title 参数）"}`
        const notes = parsed.notes.length ? `\n排版说明:\n- ${parsed.notes.join("\n- ")}` : ""
        const body = problems.length ? `\n待处理 ${problems.length} 项:\n- ${problems.join("\n- ")}` : "\n未发现阻断问题，可直接正式导入（dry_run 置为 false）。"
        return { output: `预检（dry_run，未写入任何内容）：\n${head}${body}${notes}\n建议：修完问题后正式导入（dry_run=false），再跑 lint_doc 做排版体检。` }
      }
      let docId = args.document_id ? String(args.document_id) : ""
      const createdDoc = !docId
      const title = String(args.title ?? parsed.title ?? "未命名文档")
      if (!docId) {
        const created = (await api(ctx, "/open-apis/docx/v1/documents", {
          method: "POST",
          body: { title, folder_token: targetFolder(ctx, args.folder_token) },
        })) as { document?: { document_id: string } }
        docId = created.document?.document_id ?? ""
        if (!docId) throw new Error("创建文档失败：响应缺少 document_id")
      }
      const parent = await rootBlockId(ctx, docId)
      // 块描述 → 块组：走与 add_blocks 同一套归一（callout 配色与子块、code 语言、todo done、表格列宽）+ grid 列补全
      const bb = new BlockBuilder(0)
      const groups: BlockGroup[] = []
      for (const desc of parsed.blocks) {
        const gridErr = gridStructureError(desc)
        if (gridErr) throw new Error(gridErr)
        const before = bb.counter
        const normalized = prepareGridColumns(normalizeBlockFields(desc)).block
        const rootId =
          Number(normalized.block_type ?? 0) === BLOCK_TYPE.TABLE && (normalized.table as Record<string, unknown> | undefined)?.rows !== undefined
            ? expandTableRows(normalized, bb)
            : buildGroup(normalized, bb)
        groups.push({ rootId, blocks: bb.blocks.slice(before) })
      }
      const res = await insertWithMedia(ctx, docId, parent, groups, { keepDiagramSource: args.diagram_source === "keep" })
      const missedDiagrams = res.diagrams.filter((x) => !x.ok)
      const diagramNote = res.diagrams.length
        ? `\n图表: ${res.diagrams.length - missedDiagrams.length}/${res.diagrams.length} 已渲染为图片${missedDiagrams.length ? `（未渲染已保留为代码块: ${missedDiagrams.map((x) => x.format).join("、")}）` : ""}`
        : ""
      const failedImages = res.images.filter((x) => !x.ok)
      const imageNote = res.images.length
        ? `\n图片: ${res.images.length - failedImages.length}/${res.images.length} 已插入${failedImages.length ? `（未插入: ${failedImages.map((x) => `${x.src}（${x.note}）`).join("；")}）` : ""}`
        : ""
      const notes = parsed.notes.length ? `\n排版说明:\n- ${parsed.notes.join("\n- ")}` : ""
      const folderDash = createdDoc ? folderNote(ctx, args.folder_token) : ""
      return {
        output: `✓ 已导入 ${res.count} 个顶层块 → document_id: ${docId}\nURL: https://feishu.cn/docx/${docId}${diagramNote}${imageNote}${notes}${folderDash}\n建议：接着跑 lint_doc 体检，按报告精修（表格列宽/层级/长段落）。`,
      }
    },
  )

  const lintDoc = tool(
    "lint_doc",
    "排版体检：读取文档块结构，按排版规范逐条检查（标题层级跳级/连续标题/空标题、超长段落、表格列宽过窄与缺表头、代码块缺语言、列表嵌套过深、分割线过密、高亮块滥用、纯文字墙、空段落）并返回问题清单（block_id + 问题 + 修复建议）。生成或改完文档后跑一遍，按报告做最小范围精修（表格用 set_table_width、文本用 update_block、长段拆用 add_blocks/delete_blocks）。",
    {
      document_id: { type: "string" },
      limit: { type: "number", description: "最多返回的问题条数（默认 30）" },
    },
    ["document_id"],
    async (args, ctx) => {
      const docId = String(args.document_id)
      const { items, truncated } = await collectPages(ctx, `/open-apis/docx/v1/documents/${docId}/blocks`, 500, 2000)
      const { issues, stats } = lintBlocks(items)
      const limit = args.limit !== undefined ? Math.max(1, Math.min(200, Number(args.limit))) : 30
      const warn = issues.filter((i) => i.level === "warn").length
      const info = issues.length - warn
      const typeSummary = Object.entries(stats.counts)
        .sort((a, b) => b[1] - a[1])
        .map(([k, v]) => `${k} ${v}`)
        .join(" / ")
      const shown = issues.slice(0, limit)
      const lines = shown.map((i) => `- [${i.level}] ${i.rule}（${i.typeName} ${i.blockId}）：${i.detail} → ${i.fix}`)
      const more = issues.length > shown.length ? `\n（另有 ${issues.length - shown.length} 条未展示，可传 limit 查看）` : ""
      const head = `排版体检：${stats.blocks} 个块（${typeSummary}），发现 ${issues.length} 个问题（warn ${warn} / info ${info}）${truncated ? "；块数达到读取上限 2000，报告只覆盖已读取部分" : ""}`
      if (!issues.length) return { output: `${head}\n未发现排版问题：层级、表格、代码与组件使用均符合规范。` }
      return { output: `${head}\n${lines.join("\n")}${more}\n建议：先修 warn 项（表格列宽 / 标题层级），再按需处理 info；每轮修改后重新体检，不沿用旧的 block_id。` }
    },
  )

  const styleGuide = tool(
    "style_guide",
    "读取飞书文档排版规范（SKILL）：不传 name 列出可用文档；传 name 返回全文——`style`（排版总纲：判定原则、视觉策略档位、组件选择表、颜色规范、硬性排版规格、两阶段工作流、交付自检）、`xml`（XML 排版语法与不支持项），以及体裁契约：weekly-report / technical-doc / prd / proposal / retrospective / research-report / data-report / meeting-minutes / sop-tutorial / formal-doc。**整篇创作前必读**：先 style，再对应体裁；写 XML 时补 xml。",
    { name: { type: "string", description: "文档名（缺省列出全部可用文档）" } },
    [],
    async (args) => {
      const name = args.name !== undefined ? String(args.name).trim() : ""
      if (!name) {
        const list = styleDocList().map((d) => `- ${d.name}：${d.summary}`).join("\n")
        return { output: `可用排版规范文档（用 style_guide name=<名> 读取全文）：\n${list}` }
      }
      const doc = readStyleDoc(name)
      if (!doc) return { output: `❌ 未知规范文档 "${name}"；可用：${styleDocList().map((d) => d.name).join("、")}` }
      return { output: doc.content }
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
    { name: { type: "string" }, folder_token: { type: "string", description: "父文件夹 token（可选；缺省用 FEISHU_DOCS_FOLDER_URL 配置的文件夹，未配置则根目录）" } },
    ["name"],
    async (args, ctx) => {
      const data = await api(ctx, "/open-apis/drive/v1/files/create_folder", {
        method: "POST",
        body: { name: String(args.name), folder_token: targetFolder(ctx, args.folder_token) ?? "" },
      })
      return appendNote(await jsonResult(ctx, data, "创建成功"), folderNote(ctx, args.folder_token))
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
      folder_token: { type: "string", description: "目标文件夹 token（可选；缺省用 FEISHU_DOCS_FOLDER_URL 配置的文件夹，未配置则应用云空间根目录）" },
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
      const folderToken = targetFolder(ctx, args.folder_token)
      if (folderToken) form.append("parent_node", folderToken)
      form.append("size", String(bytes.length))
      form.append("file", new Blob([bytes], { type: "application/octet-stream" }), fileName)
      const data = await api(ctx, "/open-apis/drive/v1/files/upload_all", { method: "POST", form })
      return appendNote(await jsonResult(ctx, data, "上传成功"), folderNote(ctx, args.folder_token))
    },
  )

  const insertImage = tool(
    "insert_image",
    "在文档中插入图片（飞书官方三步流程，实测验证）：1) 创建空 image 块（block_type=27，image:{}，创建时**不传 token**——传 token 报 1770001）；2) 上传图片素材到该块（POST drive/v1/medias/upload_all，multipart：parent_type=docx_image、parent_node=新建块 id；**云空间的 file_token 不能直接用于文档 image 块**，必须走 media 上传）；3) PATCH replace_image 设置素材 token（返回 width/height 自动识别）。",
    {
      document_id: { type: "string" },
      block_id: { type: "string", description: "父块 id（缺省文档根块，追加到末尾）" },
      image: { type: "string", description: "base64 文本（encoding=base64）、本地图片文件路径（**须传绝对路径**，相对路径相对会话目录会 ENOENT）或 http(s) 图片地址" },
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
        if (!bytes.length) throw new Error("图片内容为空（base64 无效？）")
        // 大小上限（飞书 media 上传限制 20MB）：显式校验做纵深防御（base64 双份拷贝内存峰值约 2.7×）
        if (bytes.length > 20 * 1024 * 1024) throw new Error(`图片超过 20MB 上限: ${(bytes.length / 1024 / 1024).toFixed(1)}MB`)
      } else {
        bytes = (await readImageBytes(ctx, String(args.image))).bytes
      }
      // 步骤 1：创建空 image 块（image:{} 不传 token，否则 1770001 invalid param）
      const created = (await api(ctx, `/open-apis/docx/v1/documents/${docId}/blocks/${blockId}/children`, {
        method: "POST",
        body: { children: [{ block_type: BLOCK_TYPE.IMAGE, image: {} }] },
      })) as { children?: Array<{ block_id?: string }> }
      const imageBlockId = created.children?.[0]?.block_id
      if (!imageBlockId) throw new Error("创建 image 块失败：响应缺少 block_id")
      // 步骤 2/3：上传素材并 replace_image（与 Markdown 导入图片共用实现）
      const filled = await fillImageBlock(ctx, docId, imageBlockId, bytes, fileName)
      const size = filled.width ? `（${filled.width}×${filled.height}）` : ""
      return { output: `✓ 已在文档插入图片${size}\nimage block_id: ${imageBlockId}\nmedia file_token: ${filled.token}` }
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
    { title: { type: "string" }, folder_token: { type: "string", description: "目标文件夹 token（可选；缺省用 FEISHU_DOCS_FOLDER_URL 配置的文件夹，未配置则应用云空间根目录）" } },
    ["title"],
    async (args, ctx) => {
      const data = await api(ctx, "/open-apis/sheets/v3/spreadsheets", {
        method: "POST",
        body: { title: String(args.title), folder_token: targetFolder(ctx, args.folder_token) },
      })
      return appendNote(await jsonResult(ctx, data, "创建成功"), folderNote(ctx, args.folder_token))
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
      folder_token: { type: "string", description: "目标文件夹 token（可选；缺省用 FEISHU_DOCS_FOLDER_URL 配置的文件夹，未配置则应用云空间根目录）" },
      fields: { type: "string", description: "数据表字段定义 JSON 数组（可选，默认表创建后自动建字段，如 [{\"name\":\"名称\",\"type\":1},{\"name\":\"状态\",\"type\":3,\"property\":{\"options\":[{\"name\":\"进行中\"},{\"name\":\"已完成\"}]}}]；type 枚举：1 多行文本/2 数字/3 单选/4 多选/5 日期/7 复选框/11 人员/13 电话/15 超链接，单选多选需 property.options）" },
    },
    ["name"],
    async (args, ctx) => {
      const data = (await api(ctx, "/open-apis/bitable/v1/apps", {
        method: "POST",
        body: { name: String(args.name), folder_token: targetFolder(ctx, args.folder_token) },
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
      const folderDash = folderNote(ctx, args.folder_token)
      return { output: `✓ 已创建多维表格: ${args.name}\napp_token: ${appToken}${tableId ? `\n默认数据表: ${tableId}` : ""}${fieldsNote}\n可用 add_bitable_records 写入记录。${folderDash}` }
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
      try {
        const data = await api(ctx, `/open-apis/drive/v1/permissions/${String(args.token)}/members`, {
          method: "POST",
          query: { type: args.type },
          body: { member_type: String(args.member_type), member_id: String(args.member_id), perm: String(args.perm) },
        })
        return jsonResult(ctx, data, "授权成功")
      } catch (err) {
        // 应用对资源无分享权（受限）时给可落地替代路径，替代模型反复换参数重试
        throw new Error(`${(err as Error).message}${isPermissionBlocked(err) ? `\n${FOLDER_ADVICE}` : ""}`)
      }
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
            output: `❌ ${msg}\n诊断：设置链接分享返回 404——①请确认应用已开通云文档分享权限（开发者后台 → 权限管理 → drive:drive 或 docs:permission.setting:write_only）；②确认 token 与 type 匹配（type=docx 用 document_id，type=sheet 用 spreadsheet_token）；③若为试用租户，部分接口可能不可用。GET 权限信息正常说明读权限与 token 均有效，问题聚焦在写权限。\n${FOLDER_ADVICE}`,
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
    replace_text: replaceText,
    set_table_width: setTableWidth,
    delete_blocks: deleteBlocks,
    import_markdown: importMarkdown,
    import_xml: importXml,
    lint_doc: lintDoc,
    style_guide: styleGuide,
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
/** 块内文本：text_run 取内容，mention_user / mention_doc 取可读占位（@用户 / @文档）——
 *  纯文本视图里 @人 不能凭空消失，否则查找、紧凑视图、体检文本都看不到它。 */
export function blockText(block: Record<string, unknown>): string {
  const els = blockElements(block) ?? []
  return els
    .map((e) => {
      if (!e || typeof e !== "object") return ""
      const o = e as Record<string, unknown>
      if ("text_run" in o) return String((o.text_run as Record<string, unknown> | undefined)?.content ?? "")
      if (o.mention_user) return "@用户"
      if (o.mention_doc) return "@文档"
      if (o.equation) return "[公式]"
      if (o.reminder) return "[提醒]"
      return ""
    })
    .join("")
}

/** 表格块 → 单元格文本二维数组（单元格按 column_size 分行）。 */
function tableCellRows(table: Record<string, unknown>, byId: Map<string, Record<string, unknown>>): string[][] {
  const prop = ((table.table ?? {}) as Record<string, unknown>).property as Record<string, unknown> | undefined
  const colSize = Number(prop?.column_size ?? 0)
  const seen = new Set<string>()
  const rows: string[][] = []
  let row: string[] = []
  for (const c of Array.isArray(table.children) ? table.children : []) {
    row.push(cellText(String(c), byId, seen))
    if (colSize > 0 && row.length >= colSize) {
      rows.push(row)
      row = []
    }
  }
  if (row.length) rows.push(row)
  return rows
}

/** 表格块 → 行文本（单元格内容按列分组）。 */
function blockTableText(table: Record<string, unknown>, byId: Map<string, Record<string, unknown>>): string {
  const rows = tableCellRows(table, byId)
  return rows.length ? `[表格]\n${rows.map((r) => `| ${r.join(" | ")} |`).join("\n")}` : "[表格]"
}

/** 单元格/块子树文本（子块递归收集，层级之间以换行分隔——单元格多段落与嵌套列表均可读）。 */
function cellText(id: string, byId: Map<string, Record<string, unknown>>, seen: Set<string>): string {
  const b = byId.get(id)
  if (!b || seen.has(id)) return ""
  seen.add(id)
  const kids = b.children
  const parts = [blockText(b)]
  if (Array.isArray(kids)) for (const c of kids) parts.push(cellText(String(c), byId, seen))
  return parts.filter((p) => p !== "").join("\n")
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

function textBlock(content: string, blockType: number = BLOCK_TYPE.TEXT, style?: Record<string, unknown>): Record<string, unknown> {
  const field: Record<number, string> = {
    [BLOCK_TYPE.TEXT]: "text",
    [BLOCK_TYPE.BULLET]: "bullet",
    [BLOCK_TYPE.ORDERED]: "ordered",
    [BLOCK_TYPE.QUOTE]: "quote",

  }
  return { block_type: blockType, [field[blockType] ?? "text"]: { ...(style ? { style } : {}), elements: textElements(content) } }
}

/** 飞书 code.style.language 数字枚举名表（下标 + 1 = 枚举值，共 75 项，与官方枚举表一致）。
 * 该字段是 int 枚举，传字符串报 99992402：语言标识统一按本表归一化后查表。 */
const CODE_LANG_NAMES = [
  "PlainText", "ABAP", "Ada", "Apache", "Apex", "Assembly Language", "Bash", "CSharp", "C++", "C",
  "COBOL", "CSS", "CoffeeScript", "D", "Dart", "Delphi", "Django", "Dockerfile", "Erlang", "Fortran",
  "FoxPro", "Go", "Groovy", "HTML", "HTMLBars", "HTTP", "Haskell", "JSON", "Java", "JavaScript",
  "Julia", "Kotlin", "LateX", "Lisp", "Logo", "Lua", "MATLAB", "Makefile", "Markdown", "Nginx",
  "Objective-C", "OpenEdgeABL", "PHP", "Perl", "PostScript", "Power Shell", "Prolog", "ProtoBuf", "Python", "R",
  "RPG", "Ruby", "Rust", "SAS", "SCSS", "SQL", "Scala", "Scheme", "Scratch", "Shell",
  "Swift", "Thrift", "TypeScript", "VBScript", "Visual Basic", "XML", "YAML", "CMake", "Diff", "Gherkin",
  "GraphQL", "OpenGL Shading Language", "Properties", "Solidity", "TOML",
]

/** 语言标识归一化（小写、去空格/连字符/下划线/点）："Power Shell"/"power-shell" → "powershell"。 */
function langKey(s: string): string {
  return s.trim().toLowerCase().replace(/[\s._-]/g, "")
}

/** Markdown 围栏常用别名（枚举名表未覆盖的写法）→ 枚举值。 */
const CODE_LANG_ALIASES: Record<string, number> = {
  js: 30, jsx: 30, mjs: 30, cjs: 30,
  ts: 63, tsx: 63,
  py: 49, py3: 49, python3: 49,
  rb: 52, rs: 53, jl: 31, pl: 44, kt: 32,
  sh: 7, zsh: 7, console: 7,
  yml: 67, md: 39, tex: 33, docker: 18, make: 38,
  cs: 8, "c#": 8, cpp: 9, cplusplus: 9, objc: 41, ps1: 46, pwsh: 46,
  proto: 48, golang: 22, gql: 71, sol: 74, patch: 69, json5: 28,
  h: 9, hh: 9, hpp: 9, cc: 9, cxx: 9,
  ini: 73, conf: 73, cfg: 73, less: 12, sass: 55, vue: 24,
  txt: 1, text: 1, plain: 1, log: 1,
  vb: 65, vbnet: 65, vbs: 64, asm: 6,
}

/** 语言名 → 枚举数字：先按枚举名归一化查表，再按别名覆盖；未知语言回退 PlainText(1)。 */
const CODE_LANG: Record<string, number> = (() => {
  const m: Record<string, number> = {}
  CODE_LANG_NAMES.forEach((name, i) => {
    m[langKey(name)] = i + 1
  })
  for (const [k, v] of Object.entries(CODE_LANG_ALIASES)) m[langKey(k)] = v
  return m
})()

/** 官方枚举总项数（测试断言用）。 */
export const CODE_LANG_COUNT = CODE_LANG_NAMES.length

/** 语言名 → 枚举数字；未知语言回退 PlainText(1)。 */
export function codeLangEnum(lang: string | undefined): number {
  if (!lang) return 1
  return CODE_LANG[langKey(lang)] ?? 1
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
 * - callout 块：颜色/emoji 归一为 callout 顶层字段、正文转为 children 子块（实测：callout.style 包裹与 callout.elements 均被服务端忽略）
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
  // callout 高亮块（实测）：正文在 **children 子块**（callout.elements 被服务端忽略）；颜色/emoji 字段为
  // **callout 顶层字段**（background_color/border_color/text_color 数字枚举、emoji_id 字符串；callout.style
  // 包裹会被忽略并回落默认）；**必须有至少一个子块**（缺子块报 1770041 open schema mismatch，空内容补空 text 子块）。
  // 兼容 callout.style / callout 顶层 / 块顶层三种写法，统一归一为 callout 顶层字段。
  if (type === BLOCK_TYPE.CALLOUT) {
    const callout = b.callout
    const styleKeys = ["background_color", "border_color", "text_color", "emoji_id"]
    if (callout !== undefined && typeof callout === "object" && !Array.isArray(callout)) {
      const c = { ...(callout as Record<string, unknown>) }
      // 正文兼容：text 快捷写法 → callout 正文段落；已有 callout.elements（旧写法）→ 转为子块内容
      let bodyEls: unknown[] | undefined
      if (b.text !== undefined) {
        if (typeof b.text === "string") bodyEls = textElements(b.text)
        else if (b.text && typeof b.text === "object") {
          const t = b.text as Record<string, unknown>
          if (Array.isArray(t.elements)) bodyEls = t.elements as unknown[]
        }
        delete b.text
      } else if (typeof c.elements === "string") {
        bodyEls = textElements(String(c.elements))
      } else if (Array.isArray(c.elements)) {
        bodyEls = c.elements as unknown[]
      } else if (Array.isArray(b.elements)) {
        bodyEls = b.elements as unknown[]
      }
      delete c.elements
      delete b.elements
      const kids = Array.isArray(b.children) ? (b.children as unknown[]) : []
      // 子块：已有块对象子块保留（递归由 buildGroup 处理）；否则用正文段落生成子块（至少一个）
      if (kids.length === 0) {
        b.children = [{ block_type: BLOCK_TYPE.TEXT, text: { elements: bodyEls && bodyEls.length ? bodyEls : [{ text_run: { content: "" } }] } }]
      } else if (bodyEls && bodyEls.length) {
        // 已有子块且另有正文：追加一个正文子块，不丢内容
        kids.push({ block_type: BLOCK_TYPE.TEXT, text: { elements: bodyEls } })
        b.children = kids
      }
      b.callout = c
    } else {
      // 简化写法：text → callout 正文子块（callout 字段保留为对象）
      const bodyEls = typeof b.text === "string" ? textElements(b.text) : b.callout !== undefined && b.callout !== null && typeof b.callout === "object" && Array.isArray((b.callout as Record<string, unknown>).elements) ? ((b.callout as Record<string, unknown>).elements as unknown[]) : undefined
      b.callout = typeof b.callout === "object" && b.callout !== null && !Array.isArray(b.callout) ? { ...(b.callout as Record<string, unknown>) } : {}
      delete (b.callout as Record<string, unknown>).elements
      if (b.text !== undefined) delete b.text
      if (!Array.isArray(b.children) || (b.children as unknown[]).length === 0) {
        b.children = [{ block_type: BLOCK_TYPE.TEXT, text: { elements: bodyEls && bodyEls.length ? bodyEls : [{ text_run: { content: "" } }] } }]
      }
    }
    // 统一收口：颜色/emoji 归一为 callout 顶层字段（callout.style 包裹会被忽略）
    const finalCallout = b.callout
    if (finalCallout && typeof finalCallout === "object" && !Array.isArray(finalCallout)) {
      const c = { ...(finalCallout as Record<string, unknown>) }
      const st: Record<string, unknown> = c.style && typeof c.style === "object" && !Array.isArray(c.style) ? (c.style as Record<string, unknown>) : {}
      delete c.style
      for (const k of styleKeys) {
        const v = c[k] !== undefined ? c[k] : st[k] !== undefined ? st[k] : b[k]
        if (v !== undefined) c[k] = v
        delete b[k]
      }
      b.callout = c
    }
    for (const k of styleKeys) delete b[k]
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
    // wrap=true：长行自动换行，避免代码块横向溢出（官方默认为 false）
    code: { style: { language: codeLangEnum(lang), wrap: true }, elements: [{ text_run: { content } }] },
  }
}

/** 图表围栏语言 → 图表格式（import_markdown 把围栏渲染成 PNG 图片插入文档）。 */
const DIAGRAM_FENCE_LANGS: Record<string, DiagramFormat> = {
  mermaid: "mermaid",
  mmd: "mermaid",
  plantuml: "plantuml",
  puml: "plantuml",
  d2: "d2",
  echarts: "echarts",
}

/** 图表占位块：Markdown 图表围栏先落为 image 占位块，导入时渲染 PNG 再上传素材回填；
 *  渲染不可用/失败时降级为同语言代码块（源码保留）。`_` 前缀为本地字段，发送飞书前由 stripLocalMeta 剥离。 */
function diagramPlaceholder(format: DiagramFormat, code: string): Record<string, unknown> {
  return { block_type: BLOCK_TYPE.IMAGE, image: {}, _diagram_format: format, _diagram_code: code }
}

function dividerBlock(): Record<string, unknown> {
  return { block_type: BLOCK_TYPE.DIVIDER, divider: {} }
}

/** 引用块（含引用内代码围栏）。实测：quote 块不支持子块（带 children 报 1770041 open schema mismatch），
 *  故引用内的列表/代码块只能以文本呈现——代码行改用行内代码样式，在平台限制下保留视觉区分。 */
function quoteBlockFromLines(lines: string[]): Record<string, unknown> {
  const els: Record<string, unknown>[] = []
  let normal: string[] = []
  let code: string[] = []
  let inCode = false
  const sep = (): void => {
    if (els.length) els.push({ text_run: { content: "\n" } })
  }
  const flushNormal = (): void => {
    if (!normal.length) return
    sep()
    els.push(...textElements(normal.join("\n")))
    normal = []
  }
  const flushCode = (): void => {
    if (!code.length) return
    sep()
    els.push({ text_run: { content: code.join("\n"), text_element_style: { inline_code: true } } })
    code = []
  }
  for (const raw of lines) {
    if (FENCE_RE.test(raw.trim())) {
      inCode ? flushCode() : flushNormal()
      inCode = !inCode
      continue
    }
    ;(inCode ? code : normal).push(raw)
  }
  inCode ? flushCode() : flushNormal()
  if (!els.length) els.push({ text_run: { content: "" } })
  return { block_type: BLOCK_TYPE.QUOTE, quote: { elements: els } }
}

/** GitHub 告示语法（`> [!NOTE]` 等）→ callout 高亮块样式映射：背景用浅色系枚举、边框同色系（官方 CalloutBackgroundColor/CalloutBorderColor）。 */
const ALERT_CALLOUT_STYLE: Record<string, { background_color: number; border_color: number; emoji_id: string }> = {
  NOTE: { background_color: 5, border_color: 5, emoji_id: "bulb" },
  TIP: { background_color: 4, border_color: 4, emoji_id: "bulb" },
  IMPORTANT: { background_color: 6, border_color: 6, emoji_id: "pushpin" },
  WARNING: { background_color: 3, border_color: 3, emoji_id: "pushpin" },
  CAUTION: { background_color: 1, border_color: 1, emoji_id: "pushpin" },
}

/** 告示块（callout）：颜色/emoji 为 callout 顶层字段；正文放 children 子块（服务端忽略 callout.elements）；
 *  至少一个子块（descendant 接口对 Callout 强制要求，缺则 1770041 open schema mismatch）。
 *  标题粗体置首段落，正文按空行拆多段（每段一个 text 子块）。 */
function calloutGroup(kind: string, title: string, body: string, base: number): BlockGroup {
  const bb = new BlockBuilder(base)
  const paras = body.split(/\n{2,}/).map((p) => p.trim()).filter((p) => p !== "")
  const first: Record<string, unknown>[] = []
  if (title) first.push({ text_run: { content: title, text_element_style: { bold: true } } })
  if (paras.length) {
    if (first.length) first.push({ text_run: { content: "\n" } })
    first.push(...textElements(paras[0]))
  }
  const childIds = [bb.add({ block_type: BLOCK_TYPE.TEXT, text: { elements: first.length ? first : [{ text_run: { content: "" } }] } })]
  for (const p of paras.slice(1)) childIds.push(bb.add({ block_type: BLOCK_TYPE.TEXT, text: { elements: textElements(p) } }))
  const calloutId = bb.add({ block_type: BLOCK_TYPE.CALLOUT, callout: { ...ALERT_CALLOUT_STYLE[kind] } }, childIds, "ca")
  return { rootId: calloutId, blocks: bb.blocks }
}

/** Markdown 列表行解析（含缩进）：返回层级（缩进每 2 空格/1 tab 一级）、类型（bullet/ordered/todo）与文本。 */
interface ListLineInfo {
  depth: number
  kind: "bullet" | "ordered" | "todo"
  done: boolean
  /** 有序列表项在 Markdown 中书写的序号（段内首项决定起始编号）。 */
  sequence?: number
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
    sequence: m[3] !== undefined ? Number(m[3]) : undefined,
    text: m[4],
  }
}

/** 列表树节点（嵌套列表构建中间结构）。 */
interface ListNode {
  kind: "bullet" | "ordered" | "todo"
  done: boolean
  sequence?: number
  text: string
  children: ListNode[]
}

/** 列表项 → 块：有序项写 `ordered.style.sequence`（飞书按块存储编号；CommonMark 语法只有段首项的数字决定起始编号，
 *  故段内逐项下发算好的编号，保证起始号非 1 或全写 `1.` 时都是连续编号）。 */
function listBlockOf(node: ListNode, seq?: number): Record<string, unknown> {
  if (node.kind === "todo") {
    return { block_type: BLOCK_TYPE.TODO, todo: { style: { done: node.done }, elements: textElements(node.text) } }
  }
  if (node.kind === "ordered") {
    return { block_type: BLOCK_TYPE.ORDERED, ordered: { style: { sequence: String(seq ?? node.sequence ?? 1) }, elements: textElements(node.text) } }
  }
  return textBlock(node.text, BLOCK_TYPE.BULLET)
}

/** 发射一层列表项（按同 kind 连续段分组计数），返回各块 id。 */
function emitListLevel(nodes: ListNode[], bb: BlockBuilder): string[] {
  const ids: string[] = []
  let orderedStart: number | undefined
  let orderedCount = 0
  for (const n of nodes) {
    if (n.kind === "ordered") {
      if (orderedCount === 0) orderedStart = n.sequence ?? 1
      ids.push(emitListItem(n, bb, (orderedStart ?? 1) + orderedCount))
      orderedCount++
    } else {
      orderedCount = 0
      ids.push(emitListItem(n, bb))
    }
  }
  return ids
}

/** 递归发射列表树：子项作为父块 children 引用（descendant 接口一次创建嵌套列表）。 */
function emitListItem(node: ListNode, bb: BlockBuilder, seq?: number): string {
  const childIds = emitListLevel(node.children, bb)
  return bb.add(listBlockOf(node, seq), childIds)
}

/* ================= 表格列宽自适应 ================= */

/** 文档正文可用宽度（px，实测：官方 Markdown 转换通道生成的表格总宽恒为 730——1 列 730、2 列 365×2、6 列 122×6）。 */
export const TABLE_PAGE_WIDTH = 730

/** 单列最小宽度（px，实测：官方转换通道在列数过多、页面宽度均分不足 100 时回落为每列 100；平台校验下限为 50）。 */
export const TABLE_MIN_COLUMN_WIDTH = 100

/** 是否为宽字符（CJK/韩文/全角/emoji——表格列宽按显示宽度分配，汉字占两格）。 */
function isWideCodePoint(cp: number): boolean {
  return (
    (cp >= 0x1100 && cp <= 0x115f) ||
    (cp >= 0x2e80 && cp <= 0xa4cf) ||
    (cp >= 0xac00 && cp <= 0xd7a3) ||
    (cp >= 0xf900 && cp <= 0xfaff) ||
    (cp >= 0xfe30 && cp <= 0xfe6f) ||
    (cp >= 0xff00 && cp <= 0xff60) ||
    (cp >= 0xffe0 && cp <= 0xffe6) ||
    (cp >= 0x1f300 && cp <= 0x1faff) ||
    (cp >= 0x20000 && cp <= 0x3fffd)
  )
}

/** 文本显示宽度（CJK/全角字符计 2，其余计 1）。 */
export function displayWidth(text: string): number {
  let w = 0
  for (const ch of text) w += isWideCodePoint(ch.codePointAt(0) ?? 0) ? 2 : 1
  return w
}

/**
 * 表格列宽自适应：各列取内容显示宽度最大值作为权重，把目标总宽按权重分配下去。
 * 单列不低于 TABLE_MIN_COLUMN_WIDTH；列数过多（最小宽度之和已超目标宽度）时全部取最小宽度。
 * 目标总宽缺省 TABLE_PAGE_WIDTH（文档正文宽度）——飞书不传 `column_width` 时每列固定 100px，
 * 宽内容会被挤成长条，故始终显式下发算好的列宽。
 */
export function tableColumnWidths(rows: string[][], totalWidth: number = TABLE_PAGE_WIDTH): number[] {
  const columnSize = Math.max(...rows.map((r) => r.length), 1)
  const target = Number.isFinite(totalWidth) && totalWidth > 0 ? totalWidth : TABLE_PAGE_WIDTH
  const min = TABLE_MIN_COLUMN_WIDTH
  if (min * columnSize >= target) return new Array(columnSize).fill(min)
  const weights = new Array(columnSize).fill(0)
  for (const row of rows) {
    for (let c = 0; c < columnSize; c++) {
      // 单元格可能含多段落/软换行：按最长行计宽（换行不计入显示宽度）
      const lines = String(row[c] ?? "").split("\n")
      const w = Math.max(...lines.map((l) => displayWidth(l)), 0)
      weights[c] = Math.max(weights[c], w)
    }
  }
  const sum = weights.reduce((a, b) => a + b, 0)
  const rest = target - min * columnSize
  const widths = weights.map((w) => Math.round(min + (sum > 0 ? (rest * w) / sum : rest / columnSize)))
  // 取整余量并入首列，保证总宽与目标宽度一致
  widths[0] += target - widths.reduce((a, b) => a + b, 0)
  return widths
}

/**
 * 表格属性（`column_width` + `header_row`）：`table.column_width`（或官方 `table.property.column_width`）
 * 显式给出时原样使用（长度须等于列数），否则按内容自适应；`table.total_width` 可指定自适应目标总宽。
 */
export function tablePropertyOf(table: Record<string, unknown> | undefined, rows: string[][], columnSize: number): Record<string, unknown> {
  const prop = (table?.property ?? {}) as Record<string, unknown>
  const explicit = table?.column_width ?? prop.column_width
  const headerRow = table?.header_row ?? prop.header_row
  let columnWidth: number[]
  if (explicit !== undefined) {
    if (!Array.isArray(explicit)) throw new Error("table.column_width 必须是数字数组（每列宽度 px）")
    columnWidth = explicit.map((v) => Math.round(Number(v)))
    if (columnWidth.length !== columnSize) throw new Error(`table.column_width 长度（${columnWidth.length}）必须与列数（${columnSize}）一致`)
    if (columnWidth.some((w) => !Number.isFinite(w) || w < 50)) throw new Error("table.column_width 每列宽度必须是不小于 50 的数字（px，平台下限）")
  } else {
    columnWidth = tableColumnWidths(rows, Number(table?.total_width ?? prop.total_width ?? TABLE_PAGE_WIDTH))
  }
  return { column_width: columnWidth, ...(headerRow !== undefined ? { header_row: Boolean(headerRow) } : {}) }
}

/** 单元格内容 → 子块：段落以空行（\n\n）分隔，每段落一个 text 子块（单元格至少一个子块，空内容补空块）。 */
function cellChildBlocks(content: string, bb: BlockBuilder): string[] {
  const paras = content.split(/\n{2,}/).map((p) => p.trim())
  const list = paras.length && paras.some((p) => p !== "") ? paras.filter((p) => p !== "") : [""]
  return list.map((p) => bb.add(textBlock(p)))
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
      const cellId = bb.add({ block_type: BLOCK_TYPE.TABLE_CELL, table_cell: {} }, cellChildBlocks(content, bb), "cell")
      rowCells.push(cellId)
    }
    cellIds.push(rowCells)
  }
  const tableId = bb.add(
    {
      block_type: BLOCK_TYPE.TABLE,
      // Markdown 表格语法保证首行为表头，默认设为标题行（加粗 + 底色）
      table: { property: { row_size: rows.length, column_size: columnSize, ...tablePropertyOf({ header_row: true }, rows, columnSize) } },
    },
    cellIds.flat(),
    "tbl",
  )
  return { rootId: tableId, blocks: bb.blocks }
}

/** 图片占位块：创建接口不接受 image.token（报 1770001），故先插占位块，插入后据 block_id_relations
 *  找到真实块 id 再上传素材与 replace_image。`_image_src` 为本地字段（发送前由 stripLocalMeta 剥离）。 */
function imagePlaceholder(src: string): Record<string, unknown> {
  return { block_type: BLOCK_TYPE.IMAGE, image: {}, _image_src: src }
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
      const cellId = bb.add({ block_type: BLOCK_TYPE.TABLE_CELL, table_cell: {} }, cellChildBlocks(String(row[c] ?? ""), bb), "cell")
      rowCells.push(cellId)
    }
    cellIds.push(rowCells)
  }
  const cells = rows.map((r) => (Array.isArray(r) ? r.map((v) => String(v ?? "")) : []))
  return bb.add(
    {
      block_type: BLOCK_TYPE.TABLE,
      table: { property: { row_size: rows.length, column_size: columnSize, ...tablePropertyOf(table, cells, columnSize) } },
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
/**
 * 分栏列宽比例归一为**整数权重**（实测修复：接口 width_ratio 只接受整数，传 0.5 报
 * 9499 Invalid parameter value；XML 写法按官方样式用 0~1 小数，这里换算成同比例整数）。
 * 规则：全为整数则原样；含小数则整体 ×100 取整，再按最大公约数约分（0.5/0.5 → 1/1、0.6/0.4 → 3/2、0.25/0.75 → 1/3）。
 */
export function widthRatioWeights(ratios: number[]): number[] {
  const allInt = ratios.every((r) => Number.isInteger(r) && r > 0)
  if (allInt) return ratios.map((r) => Math.round(r))
  const scaled = ratios.map((r) => Math.max(1, Math.round((Number.isFinite(r) && r > 0 ? r : 1) * 100)))
  const gcd = (a: number, b: number): number => (b === 0 ? a : gcd(b, a % b))
  const g = scaled.reduce((a, b) => gcd(a, b))
  return g > 1 ? scaled.map((v) => v / g) : scaled
}

/**
 * grid 分栏创建前归一（实测修复）：descendant 接口要求**每个 grid_column 带整数 width_ratio 且至少一个子块**
 * ——缺 width_ratio（或传小数权重）与空列均被拒（1770041 open schema mismatch / 9499 invalid parameter）。
 * 归一：width_ratio 统一为整数权重（小数按比例换算）；空列补一个空文本子块。返回补全子块的列数。
 */
export function prepareGridColumns(block: Record<string, unknown>): { block: Record<string, unknown>; filledColumns: number } {
  const b = { ...block }
  let filledColumns = 0
  if (Number(b.block_type ?? 0) === BLOCK_TYPE.GRID && Array.isArray(b.children)) {
    const cols = b.children as Array<Record<string, unknown>>
    const ratios = cols.map((k) => {
      const gc = (k as Record<string, unknown>).grid_column
      const raw = gc && typeof gc === "object" && !Array.isArray(gc) ? Number((gc as Record<string, unknown>).width_ratio) : NaN
      return Number.isFinite(raw) && raw > 0 ? raw : 1
    })
    const weights = widthRatioWeights(ratios)
    b.children = cols.map((k, i) => {
      const col = { ...k }
      if (Number(col.block_type ?? 0) !== BLOCK_TYPE.GRID_COLUMN) return col
      const gc = col.grid_column && typeof col.grid_column === "object" && !Array.isArray(col.grid_column) ? { ...(col.grid_column as Record<string, unknown>) } : {}
      gc.width_ratio = weights[i] ?? 1
      col.grid_column = gc
      const kids = Array.isArray(col.children) ? col.children : []
      if (!kids.length) {
        col.children = [{ block_type: BLOCK_TYPE.TEXT, text: { elements: [{ text_run: { content: "" } }] } }]
        filledColumns++
      }
      return col
    })
  }
  return { block: b, filledColumns }
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

/** 单元格文本归一：`<br>`/`<br/>`/`<br />` 转软换行；连续两个 `<br>`（或空行）作为段落分隔（\n\n）
 *  ——单元格支持多个子块（实测多段落可行），故段落分隔可落为独立 text 子块。 */
function normalizeCellText(raw: string): string {
  const withBreaks = raw.trim().replace(/<br\s*\/?>/gi, "\n")
  return withBreaks.replace(/\n{2,}/g, "\n\n")
}

/** Markdown 表格行 → 单元格数组：`\|` 为字面竖线、反引号（行内代码）内的 `|` 不切列，其余按 `|` 切分。
 *  官方 convert 通道对上述两种情况会错切列，本实现按 Markdown 语义处理。 */
function parseTableRow(line: string): string[] {
  const t = line.trim().replace(/^\|/, "").replace(/\|$/, "")
  const cells: string[] = []
  let cur = ""
  let inCode = false
  for (let i = 0; i < t.length; i++) {
    const ch = t[i]
    if (ch === "`") {
      inCode = !inCode
      cur += ch
      continue
    }
    if (!inCode && ch === "\\" && t[i + 1] === "|") {
      cur += "|"
      i++
      continue
    }
    if (!inCode && ch === "|") {
      cells.push(normalizeCellText(cur))
      cur = ""
      continue
    }
    cur += ch
  }
  cells.push(normalizeCellText(cur))
  return cells
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

/** 新建文档时首行 H1 与文档标题重复时去掉该行（避免标题层级重复：飞书文档已有 title 字段）。 */
export function dropDuplicateTitleHeading(md: string, title: string): string {
  const lines = md.replace(/\r\n/g, "\n").split("\n")
  const m = /^#\s+(.*)$/.exec((lines[0] ?? "").trim())
  if (!m || m[1].trim() !== title.trim()) return md
  let i = 1
  while (i < lines.length && !lines[i].trim()) i++
  return lines.slice(i).join("\n")
}

/**
 * 将 Markdown 文本转换为「创建嵌套块」接口所需的块组数组。
 * 支持：标题（1~9 级）/段落（行首两个全角空格=首行缩进）/有序无序列表（缩进嵌套，2 空格或 1 tab 一级）/任务列表/代码块/引用（含引用内代码围栏）/
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
      const body = code.join("\n")
      // 图表围栏（```mermaid / ```plantuml / ```d2 / ```echarts）→ 图片占位块，导入时服务端渲染为 PNG
      const format = lang ? DIAGRAM_FENCE_LANGS[lang.toLowerCase()] : undefined
      groups.push(leafGroup(format ? diagramPlaceholder(format, body) : codeBlock(lang, body), idBase))
      idBase += 1
      continue
    }

    // 独立成行的图片 → image 占位块（导入时上传素材回填；行内图片仍按普通文本处理）
    const img = /^!\[([^\]]*)\]\((\S+?)\)$/.exec(trimmed)
    if (img) {
      groups.push(leafGroup(imagePlaceholder(img[2]), idBase))
      idBase += 1
      i++
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
        const node: ListNode = { kind: cur.kind, done: cur.done, sequence: cur.sequence, text: cur.text, children: [] }
        while (stack.length && stack[stack.length - 1].raw >= cur.depth) stack.pop()
        const clamped = Math.min(stack.length ? stack[stack.length - 1].clamped + 1 : 0, cur.depth, 9)
        ;(stack.length ? stack[stack.length - 1].node.children : roots).push(node)
        stack.push({ raw: cur.depth, clamped, node })
        i++
      }
      const bb = new BlockBuilder(idBase)
      let orderedStart: number | undefined
      let orderedCount = 0
      for (const root of roots) {
        const start = bb.blocks.length
        let seq: number | undefined
        if (root.kind === "ordered") {
          if (orderedCount === 0) orderedStart = root.sequence ?? 1
          seq = (orderedStart ?? 1) + orderedCount
          orderedCount++
        } else {
          orderedCount = 0
        }
        const rootId = emitListItem(root, bb, seq)
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
        const g = calloutGroup(alert[1].toUpperCase(), alert[2].trim(), quote.slice(1).join("\n"), idBase)
        groups.push(g)
        idBase += g.blocks.length
        continue
      }
      groups.push(leafGroup(quoteBlockFromLines(quote), idBase))
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

    // 段落：合并连续非特殊行（行首两个全角空格 = 首行缩进一级；trim 会吞掉全角空格，故从原始行判定）
    const indented = /^\u3000\u3000/.test(line) || trimmed.startsWith("&emsp;&emsp;")
    const para: string[] = [trimmed.replace(/^&emsp;&emsp;/, "")]
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
    groups.push(leafGroup(textBlock(para.join("\n"), BLOCK_TYPE.TEXT, indented ? { indentation_level: "OneLevelIndent" } : undefined), idBase))
    idBase += 1
  }
  return groups
}

/** 排版体检问题项。 */
export interface LintIssue {
  level: "warn" | "info"
  rule: string
  blockId: string
  typeName: string
  detail: string
  fix: string
}

/** 正文块可读文本（标题/段落/列表/待办/引用/代码；容器块不含）。 */
function lintText(block: Record<string, unknown>): string {
  return (blockElements(block) ?? [])
    .map((e) => String((e as { text_run?: { content?: unknown } }).text_run?.content ?? ""))
    .join("")
}

/**
 * 排版体检（纯函数，供 lint_doc 工具与单测使用）：按文档流顺序检查结构、层级、表格、代码与组件使用。
 * 报告块 id 与修复动作，使模型能直接做最小范围精修。
 */
export function lintBlocks(items: Array<Record<string, unknown>>): { issues: LintIssue[]; stats: { blocks: number; counts: Record<string, number> } } {
  const counts: Record<string, number> = {}
  for (const b of items) {
    const t = blockTypeName(Number(b.block_type ?? 0))
    counts[t] = (counts[t] ?? 0) + 1
  }
  // 按文档流顺序展开（自 page 根块深度优先；无 page 块时按接口返回顺序）
  const byId = new Map(items.map((b) => [String(b.block_id ?? ""), b]))
  const order: Array<{ b: Record<string, unknown>; listDepth: number }> = []
  const seen = new Set<string>()
  const walk = (id: string, listDepth: number, depth: number): void => {
    const b = byId.get(id)
    if (!b || seen.has(id) || depth > 20) return
    seen.add(id)
    const type = Number(b.block_type ?? 0)
    const isList = type === BLOCK_TYPE.BULLET || type === BLOCK_TYPE.ORDERED || type === BLOCK_TYPE.TODO
    order.push({ b, listDepth })
    for (const c of (Array.isArray(b.children) ? b.children : []) as unknown[]) walk(String(c), isList ? listDepth + 1 : listDepth, depth + 1)
  }
  const page = items.find((b) => Number(b.block_type ?? 0) === 1)
  if (page) walk(String(page.block_id ?? ""), 0, 0)
  else for (const b of items) walk(String(b.block_id ?? ""), 0, 0)

  const issues: LintIssue[] = []
  const push = (level: "warn" | "info", rule: string, b: Record<string, unknown>, detail: string, fix: string): void => {
    issues.push({ level, rule, blockId: String(b.block_id ?? ""), typeName: blockTypeName(Number(b.block_type ?? 0)), detail, fix })
  }
  const isHeading = (t: number): boolean => t >= BLOCK_TYPE.HEADING1 && t <= BLOCK_TYPE.HEADING1 + 8
  let lastHeadingLevel = 0
  let headingCount = 0
  let dividerCount = 0
  let calloutCount = 0
  let textCount = 0
  let richCount = 0
  let prevWasHeading = false

  for (const { b, listDepth } of order) {
    const type = Number(b.block_type ?? 0)
    if (isHeading(type)) {
      headingCount++
      const level = type - BLOCK_TYPE.HEADING1 + 1
      const text = lintText(b).trim()
      if (!text) push("warn", "空标题", b, "标题没有文字", "update_block 补标题文本，或 delete_blocks 删掉空标题")
      if (lastHeadingLevel && level > lastHeadingLevel + 1) {
        push("warn", "标题层级跳级", b, `h${lastHeadingLevel} 之后直接出现 h${level}`, "把该标题降为 h" + (lastHeadingLevel + 1) + "（update_block 无法改块类型：用 add_blocks 重建该标题并删除原块）")
      }
      if (prevWasHeading) push("warn", "连续标题", b, "上一个块也是标题，本节没有正文", "补正文，或合并相邻小节")
      if (level > 4) push("info", "标题层级过深", b, `h${level} 已超过 4 级`, "降级为列表或拆分文档")
      lastHeadingLevel = level
      prevWasHeading = true
      continue
    }
    prevWasHeading = false
    if (type === BLOCK_TYPE.TEXT) {
      const text = lintText(b)
      if (!text.trim()) push("warn", "空段落", b, "空文本块残留", "delete_blocks 删除该块")
      else {
        textCount++
        const lines = text.split("\n").length
        if (text.length > 300 || lines > 6) push("info", "段落过长", b, `${text.length} 字 / ${lines} 行（建议 ≤300 字、≤6 行）`, "拆成多段或用列表/表格承载要点")
      }
      continue
    }
    if (type === BLOCK_TYPE.CODE) {
      const codeStyle = (((b.code as Record<string, unknown> | undefined)?.style ?? {}) as { language?: unknown })
      const lang = Number(codeStyle.language ?? 1)
      if (!lang || lang === 1) push("info", "代码块缺语言", b, "未标注代码语言（PlainText）", "重新导入该块时补 lang（add_blocks 传 code.style.language）")
      richCount++
      continue
    }
    if (type === BLOCK_TYPE.TABLE) {
      const prop = ((b.table as Record<string, unknown> | undefined)?.property ?? {}) as Record<string, unknown>
      const widths = Array.isArray(prop.column_width) ? (prop.column_width as unknown[]).map((w) => Number(w)) : []
      const min = widths.length ? Math.min(...widths) : 0
      const cols = Number(prop.column_size ?? widths.length ?? 0)
      if (widths.length && min < TABLE_MIN_COLUMN_WIDTH) {
        push("warn", "表格列宽过窄", b, `${cols} 列，最小列宽 ${min}px（建议 ≥${TABLE_MIN_COLUMN_WIDTH}px）`, "set_table_width 按内容自适应重排")
      }
      if (prop.header_row !== true) push("info", "表格缺表头", b, "首行未设为标题行", "set_table_width 传 header_row=true")
      if (cols > 6) push("info", "表格列过多", b, `${cols} 列`, "拆成多张表，或改用分栏/列表承载")
      richCount++
      continue
    }
    if (type === BLOCK_TYPE.DIVIDER) {
      dividerCount++
      continue
    }
    if (type === BLOCK_TYPE.CALLOUT) {
      calloutCount++
      richCount++
      continue
    }
    if (type === BLOCK_TYPE.IMAGE || type === BLOCK_TYPE.GRID || type === BLOCK_TYPE.DIAGRAM || type === BLOCK_TYPE.SHEET || type === BLOCK_TYPE.BITABLE) {
      richCount++
      continue
    }
    if (listDepth > 3) push("info", "列表嵌套过深", b, `嵌套 ${listDepth} 层`, "压平为 2 层以内，深层内容改用表格或独立小节")
  }

  if (dividerCount > 5) push("info", "分割线过多", { block_id: "-" }, `全文 ${dividerCount} 条分割线`, "只在大的章节切换处保留分割线")
  if (calloutCount > 10 || (calloutCount > 0 && calloutCount > textCount * 0.5)) {
    push("info", "高亮块过多", { block_id: "-" }, `高亮块 ${calloutCount} 个 vs 正文段 ${textCount} 个`, "只保留真正需要提醒的高亮块（克制原则）")
  }
  if (!headingCount && items.length > 10) push("info", "缺少标题结构", { block_id: "-" }, "整篇没有标题", "按读者任务分节，用 h1/h2 建立层级")
  if (textCount > 20 && richCount === 0) push("info", "纯文字墙", { block_id: "-" }, `${textCount} 个段落但无表格/图示/高亮块`, "把并列信息改为表格、流程改为图示、关键提醒改高亮块")
  return { issues, stats: { blocks: items.length, counts } }
}

/** XML 预检画像（纯函数）：顶层块数、总块数（含子树）、字数（文本元素之和）、块类型分布。 */
export function xmlProfile(blocks: Array<Record<string, unknown>>): { topLevel: number; total: number; chars: number; counts: Record<string, number> } {
  const counts: Record<string, number> = {}
  let total = 0
  let chars = 0
  const walk = (list: Array<Record<string, unknown>>): void => {
    for (const b of list) {
      total++
      const name = blockTypeName(Number(b.block_type ?? 0))
      counts[name] = (counts[name] ?? 0) + 1
      for (const e of (blockElements(b) ?? []) as Array<{ text_run?: { content?: unknown } }>) chars += String(e.text_run?.content ?? "").length
      const kids = b.children
      if (Array.isArray(kids)) walk(kids as Array<Record<string, unknown>>)
    }
  }
  walk(blocks)
  return { topLevel: blocks.length, total, chars, counts }
}

/** 紧凑块视图：一行一块（带 block_id 与层级缩进），表格不展开单元格。 */
export interface CompactView {
  lines: string[]
  /** 实际输出的块行数。 */
  total: number
  truncated: boolean
}

/** 单块摘要（紧凑视图用）：按块类型取最有信息量的一行描述。 */
function compactSummary(b: Record<string, unknown>, textLimit: number): string {
  const type = Number(b.block_type ?? 0)
  const clip = (s: string): string => {
    const one = s.replace(/\s+/g, " ").trim()
    return one.length > textLimit ? `${one.slice(0, textLimit)}…` : one
  }
  if (type === BLOCK_TYPE.TABLE) {
    const prop = ((b.table as Record<string, unknown> | undefined)?.property ?? {}) as Record<string, unknown>
    return `table ${Number(prop.row_size ?? 0)}×${Number(prop.column_size ?? 0)}${prop.header_row ? " 表头行" : ""}`
  }
  if (type === BLOCK_TYPE.IMAGE) {
    const img = (b.image ?? {}) as Record<string, unknown>
    const size = img.width && img.height ? ` ${img.width}×${img.height}` : ""
    return `image${size}${img.token ? "" : "（空槽）"}`
  }
  if (type === BLOCK_TYPE.CALLOUT) {
    const c = (b.callout ?? {}) as Record<string, unknown>
    const bits = [c.emoji_id, c.background_color !== undefined ? `bg=${c.background_color}` : ""].filter(Boolean)
    return `callout${bits.length ? ` ${bits.join(" ")}` : ""}`
  }
  if (type === BLOCK_TYPE.GRID) return `grid ${Number((b.grid as Record<string, unknown> | undefined)?.column_size ?? 0)} 列`
  if (type === BLOCK_TYPE.GRID_COLUMN) return `column ratio=${(b.grid_column as Record<string, unknown> | undefined)?.width_ratio ?? 1}`
  if (type === BLOCK_TYPE.MINDNOTE) {
    const token = (b.board as Record<string, unknown> | undefined)?.token
    return `mindnote${token ? ` board=${token}` : ""}`
  }
  if (type === BLOCK_TYPE.SHEET || type === BLOCK_TYPE.BITABLE || type === BLOCK_TYPE.EMBED || type === BLOCK_TYPE.FILE) {
    const field = type === BLOCK_TYPE.SHEET ? "sheet" : type === BLOCK_TYPE.BITABLE ? "bitable" : type === BLOCK_TYPE.EMBED ? "embed" : "file"
    const o = (b[field] ?? {}) as Record<string, unknown>
    const token = o.token ?? (o.embed as Record<string, unknown> | undefined)?.url
    return `${blockTypeName(type)}${token ? ` token=${String(token).slice(0, 24)}` : ""}`
  }
  if (type === BLOCK_TYPE.DIVIDER) return "divider"
  if (type === BLOCK_TYPE.CODE) {
    const style = ((b.code as Record<string, unknown> | undefined)?.style ?? {}) as Record<string, unknown>
    const lang = Number(style.language ?? 1)
    return `code lang=${lang}${lang === 1 ? "(PlainText)" : ""} ${clip(blockText(b).split("\n")[0] ?? "")}`.trimEnd()
  }
  return clip(blockText(b))
}

/**
 * 紧凑块视图（`get_doc_blocks detail=compact`）：按文档流 DFS，每行 `{缩进}{type} [{block_id}] {摘要}`。
 * 表格不展开单元格（只给行列数），其余容器展开子块——大文档也能整篇读完并直接拿 id 去改；
 * 需要样式/原始字段时用 `detail=full`。
 */
export function compactBlocks(items: Array<Record<string, unknown>>, opts: { textLimit?: number; maxLines?: number } = {}): CompactView {
  const textLimit = opts.textLimit !== undefined && opts.textLimit > 0 ? Math.floor(opts.textLimit) : 80
  const maxLines = opts.maxLines !== undefined && opts.maxLines > 0 ? Math.floor(opts.maxLines) : 600
  const byId = new Map(items.map((b) => [String(b.block_id ?? ""), b]))
  const lines: string[] = []
  let total = 0
  let truncated = false
  const visited = new Set<string>()
  const walk = (id: string, depth: number): void => {
    if (visited.has(id)) return
    visited.add(id)
    const b = byId.get(id)
    if (!b) return
    const type = Number(b.block_type ?? 0)
    // page 根块自身不占行（标题在文档元信息里），直接展开子块
    if (type !== 1) {
      total++
      if (lines.length < maxLines) {
        const indent = "  ".repeat(Math.min(depth, 12))
        const name = blockTypeName(type)
        const summary = compactSummary(b, textLimit)
        lines.push(`${indent}- ${name} [${id}]${summary ? ` ${summary}` : ""}`)
      } else {
        truncated = true
      }
    }
    // 表格不展开（单元格内部文本会让紧凑视图退化）；其余容器展开
    if (type === BLOCK_TYPE.TABLE) return
    const kids = Array.isArray(b.children) ? (b.children as unknown[]) : []
    for (const c of kids) walk(String(c), type === 1 ? depth : depth + 1)
  }
  const page = items.find((b) => Number(b.block_type ?? 0) === 1)
  if (page) walk(String(page.block_id ?? ""), 0)
  else for (const b of items) walk(String(b.block_id ?? ""), 0)
  return { lines, total, truncated }
}

/** 文档大纲条目（outline 模式用）。 */
export interface OutlineEntry {
  level: number
  /** 标题文本（含自动编号前缀，若有）。 */
  text: string
  blockId: string
  /** 该节内的顶层块数（到下一个同级或更高级标题为止；0 = 空节）。 */
  sectionBlocks: number
}

/**
 * 文档大纲（纯函数）：按文档流顺序抽取标题并统计每节顶层块数。
 * 供 get_doc_blocks outline=true 使用——大文档先看目录再按节读取，避免整篇拉取。
 */
export function docOutline(items: Array<Record<string, unknown>>): { entries: OutlineEntry[]; hasPage: boolean } {
  const byId = new Map(items.map((b) => [String(b.block_id ?? ""), b]))
  const page = items.find((b) => Number(b.block_type ?? 0) === 1)
  const topLevel = (page
    ? ((Array.isArray(page.children) ? page.children : []) as unknown[]).map((c) => byId.get(String(c))).filter(Boolean)
    : items.filter((b) => !b.parent_id)) as Array<Record<string, unknown>>
  const isHeading = (t: number): boolean => t >= BLOCK_TYPE.HEADING1 && t <= BLOCK_TYPE.HEADING1 + 8
  const entries: OutlineEntry[] = []
  let current: OutlineEntry | null = null
  for (const b of topLevel) {
    const type = Number(b.block_type ?? 0)
    if (isHeading(type)) {
      current = { level: type - BLOCK_TYPE.HEADING1 + 1, text: lintText(b).trim(), blockId: String(b.block_id ?? ""), sectionBlocks: 0 }
      entries.push(current)
      continue
    }
    if (current) current.sectionBlocks++
  }
  return { entries, hasPage: !!page }
}

/** 替换命中详情。 */
export interface ReplaceHit {
  blockId: string
  typeName: string
  count: number
  /** 替换后的片段预览（截断）。 */
  preview: string
}

export interface ReplacePlan {
  /** 待提交的块更新（block_id + 新文本元素）。 */
  updates: Array<{ blockId: string; elements: Record<string, unknown>[] }>
  hits: ReplaceHit[]
  /** 命中跨越多个文本片段（跨样式/跨 run），逐段替换无法完成——需用 update_block 手工改。 */
  crossRun: ReplaceHit[]
  /** 全部命中次数（含 crossRun 中的）。 */
  total: number
  /** 因 limit 未纳入更新的块数。 */
  skipped: number
}

function escapeRegExp(s: string): string {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
}

/**
 * 跨块文本替换（纯函数）：逐 text_run 替换以**保留行内样式与链接**。
 * 飞书把一段文本切成多个 run（样式边界、编辑历史），若命中的字符串跨 run（如加粗尾部 + 普通开头），
 * 逐段替换无法命中——单独归入 crossRun 提示用 update_block 手工处理，不静默丢失。
 */
export function replaceInBlocks(
  items: Array<Record<string, unknown>>,
  opts: { pattern: string; replacement: string; regex?: boolean; blockIds?: string[]; limit?: number },
): ReplacePlan {
  const source = opts.regex ? opts.pattern : escapeRegExp(opts.pattern)
  try {
    new RegExp(source, "g")
  } catch (err) {
    throw new Error(`查找模式不合法：${(err as Error).message}${opts.regex ? "" : "（pattern 默认按字面文本处理）"}`)
  }
  const allowed = opts.blockIds && opts.blockIds.length ? new Set(opts.blockIds) : null
  const limit = opts.limit !== undefined && opts.limit > 0 ? Math.floor(opts.limit) : Infinity
  const updates: ReplacePlan["updates"] = []
  const hits: ReplaceHit[] = []
  const crossRun: ReplaceHit[] = []
  let total = 0
  let skipped = 0
  for (const b of items) {
    const blockId = String(b.block_id ?? "")
    if (allowed && !allowed.has(blockId)) continue
    const elements = (blockElements(b) ?? []) as Record<string, unknown>[]
    if (!elements.length) continue
    const runText = (e: Record<string, unknown>): string => String((e.text_run as { content?: unknown } | undefined)?.content ?? "")
    const joined = elements.map(runText).join("")
    const count = [...joined.matchAll(new RegExp(source, "g"))].length
    if (!count) continue
    total += count
    let runCount = 0
    const next = elements.map((e) => {
      const tr = e.text_run as { content?: string } | undefined
      if (!tr || typeof tr.content !== "string" || !tr.content) return e
      const replaced = tr.content.replace(new RegExp(source, "g"), opts.replacement)
      if (replaced === tr.content) return e
      runCount += [...tr.content.matchAll(new RegExp(source, "g"))].length
      return { ...e, text_run: { ...tr, content: replaced } }
    })
    const typeName = blockTypeName(Number(b.block_type ?? 0))
    if (runCount === 0) {
      crossRun.push({ blockId, typeName, count, preview: joined.slice(0, 60) })
      continue
    }
    if (updates.length >= limit) {
      skipped++
      continue
    }
    updates.push({ blockId, elements: next })
    hits.push({ blockId, typeName, count: runCount, preview: joined.replace(new RegExp(source, "g"), opts.replacement).slice(0, 60) })
  }
  return { updates, hits, crossRun, total, skipped }
}
