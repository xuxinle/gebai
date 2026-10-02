/**
 * 凭证来源扩展点（服务端鉴权链路的可插拔入口）。
 *
 * 为什么要有这一层：`resolveUser` 原本写死 Bearer / Basic 两种凭证载体，部署方无法改变
 * 「歌白如何从请求里认出用户」——而实际部署常常需要：企业网关注入的身份头（`X-User`）、
 * 需要浏览器自动携带的场景（`<img>`/`<video>`/`<a download>` 无法插 `Authorization` 头，
 * 只能用 cookie）、或与既有 SSO 体系对接的自定义头。同一个 IP 上并排多套歌白时，
 * cookie 按 host 共享还会互相覆盖，凭证载体必须可换。
 *
 * 两条提供途径（可同时使用，按声明顺序依次尝试）：
 * 1. **数据级**：环境变量 `GEBAI_CREDENTIAL_SOURCES`，如
 *    `bearer,basic,cookie:gebai.auth.token,header:X-Gebai-Token`——二进制/镜像部署直接用，
 *    无需重编。名称即语义：`bearer`/`basic` 为内置解析，`cookie:<名>`/`header:<名>` 为取值载体。
 * 2. **代码级**：仓库根 `custom/auth/` 下的自定义来源（自动扫描，见 custom/README.md），
 *    适合源码部署与复杂逻辑（签名校验、多字段组合、外部系统查询）。
 *
 * **安全边界（框架不因扩展点放宽）**：
 * - 本层只负责「从请求里取出身份」，**校验一律下沉**：令牌型取值交 `AuthService.authorize()`
 *   做 HMAC 验签 + TTL + `disabled`；用户名型取值必须能在注册表里命中**已启用**用户
 *   （`AuthService.userByName`），不得凭空构造身份。
 * - **安全方法限定（默认）**：cookie 是浏览器自动携带的凭证，若写型端点也认 cookie，
 *   任何第三方页面都能让已登录浏览器代发 POST/DELETE/PATCH（CSRF）。因此凡是
 *   `carrier: "cookie"` 的来源默认只对 GET/HEAD 生效（`safeMethodsOnly: true`），需要放开
 *   的场景由部署方在自己的来源里显式声明——取图/下载/iframe 预览全是 GET，不受影响。
 * - 来源返回 null 即视为该来源不适用，继续尝试下一个；全部失败才落到 401。
 */
import type { AuthService, AuthUser } from "./auth"

/** 取值载体：从哪里读凭证字符串。 */
export type CredentialCarrier = "header" | "cookie" | "bearer" | "basic"

/**
 * 单个凭证来源。`resolve` 返回 `null` 表示「本来源不适用/未命中」，框架继续尝试下一个来源。
 * 实现应避免抛异常（抛错会被框架当作未命中并记录），而**不得**在此层做授权判定。
 */
export interface CredentialSource {
  /** 来源名（诊断与日志用；数据级来源为配置项原文，代码级来源建议用文件名）。 */
  readonly name: string
  /**
   * 是否仅对安全方法（GET/HEAD）生效。cookie 载体默认 true（防 CSRF）；部署方显式设 false
   * 即声明「我知道写端点也认这个凭证」——框架尊重该声明，但要求来源自身能防跨站（如同源校验）。
   */
  readonly safeMethodsOnly?: boolean
  resolve(ctx: CredentialContext): Promise<AuthUser | null> | AuthUser | null
}

/** 传给来源的请求上下文（只读快照，避免来源直接依赖 Hono 类型）。 */
export interface CredentialContext {
  method: string
  /** 请求头读取（大小写不敏感，由调用方保证）。 */
  header: (name: string) => string | undefined
  /** 令牌校验（验签 + TTL + disabled）——令牌型来源必须经此，不得自行比对令牌表。 */
  authorize: (token: string) => Promise<AuthUser | null>
  /** 账号密码校验（HTTP Basic 语义，含登录限流）——密码型来源必须经此。 */
  verifyCredentials: (username: string, password: string) => Promise<AuthUser | null>
  /** 按用户名取**已启用**用户（用户名型来源的映射入口，不绕过 disabled/pending）。 */
  userByName: (username: string) => Promise<AuthUser | null>
}

/** 安全方法判定（RFC 9110：GET/HEAD/OPTIONS/TRACE 为安全方法；本框架取 GET/HEAD）。 */
export function isSafeMethod(method: string): boolean {
  const m = method.toUpperCase()
  return m === "GET" || m === "HEAD"
}

/** Cookie 请求头解析：`k=v; k2=v2`，值按 URL 解码（失败保原值）。 */
export function parseCookieHeader(header: string | undefined, name: string): string | undefined {
  if (!header) return undefined
  for (const part of header.split(";")) {
    const i = part.indexOf("=")
    if (i <= 0) continue
    if (part.slice(0, i).trim() !== name) continue
    const raw = part.slice(i + 1).trim()
    try {
      return decodeURIComponent(raw)
    } catch {
      return raw
    }
  }
  return undefined
}

/** 内置：`Authorization: Bearer <token>`。 */
export function bearerSource(): CredentialSource {
  return {
    name: "bearer",
    resolve: (c) => {
      const auth = c.header("authorization")
      if (!auth || !/^bearer /i.test(auth)) return null
      return c.authorize(auth.slice(7).trim())
    },
  }
}

/**
 * 内置：`Authorization: Basic base64(username:password)`（RFC 7617）——等价隐式登录，
 * 复用密码校验与登录限流；base64 非加密，须经 HTTPS 传输。
 */
export function basicSource(): CredentialSource {
  return {
    name: "basic",
    resolve: (c) => {
      const auth = c.header("authorization")
      if (!auth || !/^basic /i.test(auth)) return null
      const decoded = Buffer.from(auth.slice(6).trim(), "base64").toString("utf8")
      const idx = decoded.indexOf(":")
      if (idx <= 0) return null
      return c.verifyCredentials(decoded.slice(0, idx), decoded.slice(idx + 1))
    },
  }
}

/**
 * 从指定请求头读令牌并校验。`header:<名>` 的载体。
 * 默认仅安全方法（与 cookie 同口径的保守默认，防自定义头被跨站表单意外带上）；
 * 需要写端点也认时在该来源声明 `safeMethodsOnly: false`。
 */
export function headerTokenSource(headerName: string, safeMethodsOnly = true): CredentialSource {
  return {
    name: `header:${headerName}`,
    safeMethodsOnly,
    resolve: (c) => {
      const tok = c.header(headerName.toLowerCase())
      if (!tok) return null
      return c.authorize(tok.trim())
    },
  }
}

/** 从指定 cookie 读令牌并校验。`cookie:<名>` 的载体，默认仅安全方法（防 CSRF）。 */
export function cookieTokenSource(cookieName: string, safeMethodsOnly = true): CredentialSource {
  return {
    name: `cookie:${cookieName}`,
    safeMethodsOnly,
    resolve: (c) => {
      const tok = parseCookieHeader(c.header("cookie"), cookieName)
      if (!tok) return null
      return c.authorize(tok)
    },
  }
}

/**
 * 数据级来源解析：`GEBAI_CREDENTIAL_SOURCES` 的逗号分隔清单 → 来源实例。
 * 名称语法：`bearer` / `basic` / `header:<名>` / `cookie:<名>`。
 * 非法项一律抛错（启动期暴露配置错误，不静默降级为「少了一道鉴权」或「少了一条通道」）。
 */
export function parseCredentialSources(spec: string | undefined): CredentialSource[] {
  const out: CredentialSource[] = []
  const items = (spec ?? "")
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean)
  for (const item of items) {
    const lower = item.toLowerCase()
    if (lower === "bearer") {
      out.push(bearerSource())
      continue
    }
    if (lower === "basic") {
      out.push(basicSource())
      continue
    }
    const idx = item.indexOf(":")
    if (idx < 0) {
      throw new Error(`GEBAI_CREDENTIAL_SOURCES 项「${item}」无法识别（支持 bearer / basic / cookie:<名> / header:<名>）`)
    }
    const kind = item.slice(0, idx).trim().toLowerCase()
    const arg = item.slice(idx + 1).trim()
    if (kind !== "cookie" && kind !== "header") {
      throw new Error(`GEBAI_CREDENTIAL_SOURCES 项「${item}」无法识别（支持 bearer / basic / cookie:<名> / header:<名>）`)
    }
    if (!arg) throw new Error(`GEBAI_CREDENTIAL_SOURCES 项「${item}」缺名称（形如 cookie:gebai.auth.token）`)
    if (kind === "cookie") {
      out.push(cookieTokenSource(arg))
      continue
    }
    out.push(headerTokenSource(arg))
  }
  return out
}

/**
 * 缺省来源链：Bearer → Basic → cookie `gebai.auth.token`。
 *
 * 为什么缺省就带 cookie 通道：页面里的图片/视频/iframe/下载是**浏览器原生请求**，
 * 前端脚本没有插入 `Authorization` 头的机会（见 packages/sdk auth-contract 与
 * packages/web/src/auth.ts 的登录态同步）。不带这条通道时，服务模式下消息流里的图片一律 401。
 * cookie 通道**默认仅 GET/HEAD**，写端点仍只认 Bearer/Basic，权限面不变。
 * 需要收紧或改用其他载体时用 `GEBAI_CREDENTIAL_SOURCES` 显式覆盖（如只写 `bearer,basic`）。
 */
export function defaultCredentialSources(): CredentialSource[] {
  return [bearerSource(), basicSource(), cookieTokenSource("gebai.auth.token")]
}

/**
 * 依次尝试各来源，返回首个命中的身份。
 * 来源抛错按未命中处理（单个扩展点故障不使整站不可登录）并回调 `onError` 供启动/日志层记录。
 */
export async function resolveCredential(
  sources: readonly CredentialSource[],
  ctx: CredentialContext,
  onError?: (source: string, err: unknown) => void,
): Promise<AuthUser | null> {
  const safe = isSafeMethod(ctx.method)
  for (const src of sources) {
    if (src.safeMethodsOnly && !safe) continue
    try {
      const user = await src.resolve(ctx)
      if (user) return user
    } catch (err) {
      onError?.(src.name, err)
    }
  }
  return null
}

/** 供组合根与测试构造上下文的便捷工厂（从 AuthService 派生校验能力）。 */
export function credentialContext(opts: {
  method: string
  header: (name: string) => string | undefined
  auth: Pick<AuthService, "authorize" | "verifyCredentials" | "userByName">
}): CredentialContext {
  return {
    method: opts.method,
    header: opts.header,
    authorize: (t) => opts.auth.authorize(t),
    verifyCredentials: (u, p) => opts.auth.verifyCredentials(u, p),
    userByName: (u) => opts.auth.userByName(u),
  }
}
