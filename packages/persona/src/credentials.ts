/**
 * 拟人模式的飞书凭据编排：把「令牌存储」与「授权流程」合成一个可查询的状态机。
 *
 * ## 为什么要有这一层
 *
 * 上层（启动装配、REST 状态接口、前端横幅、飞书渠道）各自需要的东西不同：
 *
 * | 谁 | 要什么 |
 * |---|---|
 * | 启动装配 | 「现在能不能跑？不能的话给用户一个授权链接」 |
 * | REST 状态接口 | 「当前状态、授权人是谁、什么时候过期」 |
 * | 前端横幅 | 「要不要提示用户？点一下跳到哪」 |
 * | 飞书渠道发消息 | 「给我一个能用的 token」 |
 *
 * 若让它们各自去拼 `PersonaTokenStore` + `PersonaOAuth`，就会散出多份
 * 「什么算需要授权」「链接怎么生成」的判断——**规则分叉是这里最该避免的**。
 *
 * ## 状态机
 *
 * ```
 *   missing ──startAuthorize()──▶ pending ──回调/手动粘贴──▶ ok
 *      ▲                            │                        │
 *      └──────── 兑换失败 ◀──────────┘        过期+刷新失败 ───┘
 * ```
 *
 * `ready` 为 false 时**不是错误**——首次部署天然如此。上层据此提示用户，而不是抛错。
 */

import type { PersonaOAuth, AuthorizeTicket } from "./oauth"
import type { PersonaTokenStore, PersonaUserToken } from "./user-token"

/** 对外状态（REST/前端消费）。**不含任何令牌明文**。 */
export interface PersonaCredentialStatus {
  /** 是否可正常对外动作。 */
  ready: boolean
  /**
   * 状态标签。
   *
   * - `ok`：可用
   * - `missing`：从未授权
   * - `expired`：已过期（**人格此刻已不能以用户名义发言**）
   * - `error`：读盘/解析出错
   * - `expiring`：仍可用但**即将失效**（仍在前置提醒窗口内）——
   *   没有 `offline_access` 时令牌不可续，必须提前叫人，不能等过期才叫。
   */
  state: "ok" | "missing" | "expired" | "error" | "expiring"
  /** 人类可读说明（直接展示给用户）。 */
  message: string
  /** 授权人（`ok` 时有值）。 */
  authorizedUser?: string
  /** 令牌到期时间（ms）。 */
  expireAt?: number
  /** 剩余有效期（ms，`ok` 时有值）。 */
  remainingMs?: number
  /** 已授权的 scopes。 */
  scopes?: string[]
  /** 是否需要用户授权（前端据此显示横幅）。 */
  needsAuth: boolean
  /** 最近一次授权链接（可能需要用户再点一次）。 */
  authorizeUrl?: string
}

export interface PersonaCredentialsOptions {
  store: PersonaTokenStore
  oauth: PersonaOAuth
  /** 回调地址（须在飞书后台登记）。 */
  redirectUri: () => string
  /** 视频应用凭据（生成授权链接与刷新都需要）。 */
  appId: () => string | undefined
  appSecret: () => string | undefined
  /** 覆盖 scopes（缺省 PERSONA_SCOPES）。 */
  scopes?: string
  log?: (level: "info" | "warn", msg: string) => void
  /**
   * 凭据状态变化时回调（授权完成 / 撤销 / 失效）。
   *
   * 用途：让外面的提醒（相期授权卡）跟着状态走——授权完成后把那张卡撤掉，
   * 而不是留着一张“请去授权”的卡叫用户以为还没好。
   */
  onChange?: (status: PersonaCredentialStatus) => void
}

/**
 * 凭据编排：状态查询 + 发起授权 + 兑换。
 *
 * 刻意不自己持有令牌：读写落在 `PersonaTokenStore`，这样「进程级唯一凭据」
 * 这条不变式只由一处保证（多处持有必然出现「刷新了但另一份还是旧的」）。
 */
export class PersonaCredentials {
  private lastTicket: AuthorizeTicket | null = null

  constructor(private readonly opts: PersonaCredentialsOptions) {}

  /**
   * 当前状态（**会顺带尝试刷新**，因为「临期但可刷」对用户来说就是可用）。
   *
   * 为什么不只返回快照：用户看到「需要授权」却被实际能跑的 token 挡着，
   * 会白点一次授权。宁可多花一次刷新，也不要造成误提示。
   */
  async status(): Promise<PersonaCredentialStatus> {
    const resolved = await this.opts.store.resolve()
    if (resolved.status === "ok") {
      return {
        ready: true,
        state: "ok",
        message: resolved.token.name ? `已授权（${resolved.token.name}）` : "已授权",
        ...(resolved.token.name ? { authorizedUser: resolved.token.name } : {}),
        expireAt: resolved.token.expireAt,
        remainingMs: resolved.remainingMs,
        scopes: resolved.token.scopes,
        needsAuth: false,
      }
    }
    // 缺或过期：给一个授权链接（同一时刻只保留最近一次，避免链接泛滥）
    const ticket = this.ensureTicket()
    return {
      ready: false,
      state: resolved.status === "expired" ? "expired" : "missing",
      message: resolved.reason,
      needsAuth: true,
      ...(ticket ? { authorizeUrl: ticket.url } : {}),
    }
  }

  /**
   * 发起授权：生成新链接（覆盖上一次）。
   *
   * 每次调用都生成新 state：旧链接自然失效（10 分钟 TTL），避免「用户点了旧链接
   * 授权成功但 state 已被淘汰」的困惑。
   */
  startAuthorize(): AuthorizeTicket {
    const appId = this.opts.appId()
    const appSecret = this.opts.appSecret()
    if (!appId || !appSecret) {
      throw new Error("未配置飞书应用凭据（AppID/AppSecret）：无法发起授权")
    }
    const ticket = this.opts.oauth.startAuthorize({
      appId,
      appSecret,
      redirectUri: this.opts.redirectUri(),
      ...(this.opts.scopes ? { scopes: this.opts.scopes } : {}),
    })
    this.lastTicket = ticket
    this.opts.log?.("info", `[persona:credentials] 已生成授权链接（${PENDING_HINT}）`)
    return ticket
  }

  /**
   * 用授权码兑换并落盘。
   *
   * 成功后清掉 `lastTicket`（旧的授权链接不应再被展示）。
   */
  async complete(rawCode: string, state?: string): Promise<PersonaCredentialStatus> {
    const entry = await this.opts.oauth.exchange({ code: rawCode, ...(state ? { state } : {}) })
    await this.opts.store.set(entry)
    this.lastTicket = null
    this.opts.log?.("info", `[persona:credentials] 授权完成${entry.name ? `（${entry.name}）` : ""}`)
    const status = await this.status()
    // 授权完成 → 通知外层（相期会把那张“去授权”卡撤掉）
    this.opts.onChange?.(status)
    return status
  }

  /** 取一个可用令牌（渠道发消息用）；不可用返回 null + 原因。 */
  async token(): Promise<{ token: PersonaUserToken } | { reason: string }> {
    const resolved = await this.opts.store.resolve()
    if (resolved.status === "ok") return { token: resolved.token }
    return { reason: resolved.reason }
  }

  /** 撤销授权（清除落盘令牌）。 */
  async revoke(): Promise<void> {
    await this.opts.store.clear()
    this.lastTicket = null
    this.opts.log?.("info", "[persona:credentials] 已撤销飞书授权")
    this.opts.onChange?.(await this.status())
  }

  /** 最近一次生成的授权链接（未发起过为 null）。 */
  authorizeUrl(): string | null {
    return this.lastTicket?.url ?? null
  }

  /**
   * 惰性生成授权链接（状态查询用）。
   *
   * 没有配置应用凭据时返回 null——此时前端显示「需要配置应用凭据」而不是
   * 给一个必然报错的授权链接。
   */
  private ensureTicket(): AuthorizeTicket | null {
    if (this.lastTicket) return this.lastTicket
    if (!this.opts.appId() || !this.opts.appSecret()) return null
    try {
      return this.startAuthorize()
    } catch {
      return null
    }
  }
}

const PENDING_HINT = "10 分钟内有效"
