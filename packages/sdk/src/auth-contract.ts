/**
 * 凭证契约（Credential Contract）：**前端一切「带身份去请求」的单一出口**。
 *
 * 为什么要有这一层：令牌是「怎么存、怎么带」原本散落各处——主界面 `auth.ts` 写 localStorage、
 * 文件工作台 `terminal.ts`/`lsp.ts`/`ws-client.ts` 各自读 localStorage 拼 `Authorization` 头、
 * `FsApi` 则完全不带头（服务模式下只能靠浏览器 cookie 兜底）。部署方（业务系统 / 网关 / 免登脚本）
 * 无法在框架之外改变这套行为，而实际部署常常需要——例如同一个 IP 上并排多套歌白时，
 * cookie 按 host 共享会互相覆盖，凭证必须改用「按实例区分」的载体。
 *
 * 契约把「读令牌 / 写令牌 / 清令牌 / 附请求头」收敛为四个动作，默认实现就是歌白既有行为
 * （localStorage `gebai.auth.token` + `Authorization: Bearer`），行为与从前完全一致。
 * 部署方在 `custom/web/init.js` 里给 `window.__GEBAI_AUTH__` 赋值即可整体替换：
 *
 * ```js
 * window.__GEBAI_AUTH__ = {
 *   // 令牌放哪（默认 localStorage）；返回 null 表示未登录
 *   readToken: () => sessionStorage.getItem("myapp.gebaiToken"),
 *   writeToken: (t) => sessionStorage.setItem("myapp.gebaiToken", t),
 *   clearToken: () => sessionStorage.removeItem("myapp.gebaiToken"),
 *   // 附加请求头（默认 { Authorization: `Bearer ${token}` }）；资源类请求（<img> 等）也要能带上时，
 *   // 在这里顺手写 cookie（框架不写 cookie——那是部署方对「同 host 多实例」的取舍）
 *   requestHeaders: (token) => ({ Authorization: `Bearer ${token}` }),
 * }
 * ```
 *
 * 覆盖点缺省逐项回落到默认实现（只实现 `readToken` 也能工作），实现抛错按「未提供」处理——
 * 部署方的脚本问题不使整个界面失效。
 *
 * **边界**：契约只决定「凭证如何携带」；身份如何校验由服务端 `CredentialSource` 负责，
 * 返回的身份仍须经 AuthService 校验（验签 / TTL / disabled），写操作仍不得仅凭可被浏览器
 * 自动携带的凭证（防 CSRF）。前端契约无法、也不应扩大权限面。
 */
import { appBase } from "./app-base"

/** 登录态默认存储键（与 packages/web/src/auth.ts 的 AUTH_TOKEN_KEY 同名同值）。 */
export const AUTH_TOKEN_KEY = "gebai.auth.token"

/** 部署方可覆盖的凭证动作（均可选，逐项回落默认实现）。 */
export interface AuthOverrides {
  /** 读取当前令牌；无登录态返回 null/undefined。 */
  readToken?: () => string | null | undefined
  /** 持久化令牌（登录 / 外部兑换成功后调用）。 */
  writeToken?: (token: string) => void
  /** 清除登录态（登出调用）。 */
  clearToken?: () => void
  /**
   * 令牌 → 附加请求头（每次请求时求值，令牌为空时也会调用——实现可据此挂非令牌类头）。
   * 缺省返回 `Authorization: Bearer <token>`（token 为空时不附加）。
   */
  requestHeaders?: (token: string | null) => Record<string, string>
}

/** 默认令牌存储：localStorage（隐私模式 / 配额满时静默降级为无持久化）。 */
function defaultReadToken(): string | null {
  try {
    return globalThis.localStorage?.getItem(AUTH_TOKEN_KEY) ?? null
  } catch {
    return null
  }
}

function defaultWriteToken(token: string): void {
  try {
    globalThis.localStorage?.setItem(AUTH_TOKEN_KEY, token)
  } catch {
    /* 忽略：存储不可用时令牌仅存活于内存（client.token） */
  }
}

function defaultClearToken(): void {
  try {
    globalThis.localStorage?.removeItem(AUTH_TOKEN_KEY)
  } catch {
    /* 忽略 */
  }
}

function defaultRequestHeaders(token: string | null): Record<string, string> {
  return token ? { Authorization: `Bearer ${token}` } : {}
}

/** 覆盖点挂在 window 上：二开脚本先于应用初始化执行，赋值即生效（无注册时序要求）。 */
const OVERRIDE_KEY = "__GEBAI_AUTH__"

/** 读取当前生效的覆盖点（SSR / 无 window 环境为空）。实现抛错按未提供处理。 */
function overrides(): AuthOverrides {
  try {
    const o = (globalThis as Record<string, unknown>)[OVERRIDE_KEY]
    return o && typeof o === "object" ? (o as AuthOverrides) : {}
  } catch {
    return {}
  }
}

/** 安全调用覆盖点：抛错即回落默认行为（部署方脚本问题不使界面失效）。 */
function invoke<T>(fn: (() => T) | undefined, fallback: () => T): T {
  if (typeof fn !== "function") return fallback()
  try {
    return fn()
  } catch {
    return fallback()
  }
}

/** 读取登录令牌（覆盖点优先，失败回落 localStorage）。 */
export function readToken(): string | null {
  return invoke(overrides().readToken, defaultReadToken) ?? null
}

/** 持久化登录令牌（覆盖点优先）。 */
export function writeToken(token: string): void {
  const fn = overrides().writeToken
  if (typeof fn === "function") {
    try {
      fn(token)
      return
    } catch {
      /* 回落默认实现 */
    }
  }
  defaultWriteToken(token)
}

/** 清除登录令牌（覆盖点优先）。 */
export function clearTokenState(): void {
  const fn = overrides().clearToken
  if (typeof fn === "function") {
    try {
      fn()
      return
    } catch {
      /* 回落默认实现 */
    }
  }
  defaultClearToken()
}

/**
 * 令牌 → 附加请求头（覆盖点优先）。
 * `token` 显式传入而非内部读取：调用方通常已持有当前令牌（如 `client.getToken()`），
 * 避免「读一次令牌、发请求时又读一次」在切换登录态的竞态下取到不同值。
 */
export function requestHeaders(token: string | null): Record<string, string> {
  const fn = overrides().requestHeaders
  const got = typeof fn === "function" ? (() => {
    try {
      return fn(token)
    } catch {
      return undefined
    }
  })() : undefined
  if (got && typeof got === "object") return got
  return defaultRequestHeaders(token)
}

/** 应用内路径（凭证契约与请求地址共用同一基准；转发便于部署方在覆盖点里拼地址）。 */
export { appBase }
