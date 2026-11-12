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

import { OFFLINE_ACCESS_SCOPE, ERR_OFFLINE_ACCESS_NOT_ENABLED, PERSONA_SCOPES } from "./oauth"
import type { PersonaOAuth, AuthorizeTicket } from "./oauth"
import { probeOfflineAccess } from "./offline-access"
import type { OfflineAccessProbeResult } from "./offline-access"
import type { PersonaTokenStore, PersonaUserToken } from "./user-token"

/** 从空格分隔的 scope 串里剔掉一个（不区分位置、不重复）。 */
function stripScope(scopes: string | undefined, drop: string): string | undefined {
  if (!scopes) return scopes
  const keep = scopes.split(/\s+/).filter((s) => s && s !== drop)
  return keep.length ? keep.join(" ") : undefined
}

/**
 * 这个错误是不是“应用未开通 offline_access”。
 *
 * 认两种形态：
 * - 回调地址里带的 `error=20027`（授权页跳转前就拒了）
 * - 兑换失败的 `兑换失败：code=20027 …`
 *
 * **只认 20027**，不靠“文案里出现 offline_access”推断：飞书报权限不足可能有很多原因
 * （管理员限制、用户没勾选某个 scope、应用权限配置改过），那些都不该被当成
 * “应用没开长期权限”而去降级——降了没用，还把真正的错误掩盖了；
 * 更坏的副作用是把用户送去一次莫名的新授权。
 */
export function isOfflineAccessRejection(err: unknown): boolean {
  const msg = String((err as Error)?.message ?? err ?? "")
  return msg.includes(String(ERR_OFFLINE_ACCESS_NOT_ENABLED))
}

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
  /** 覆盖 scopes（缺省 PERSONA_SCOPES）。
   *
   * 与 appId/appSecret 一样取**函数**而非值：它从渠道实例配置里读，
   * 而渠道在凭据对象构造之后才展开——取值快照会永远停在空的初值上
   * （现象：配了 offline_access 但授权链接里没有它，且零报错）。
   */
  scopes?: () => string
  /**
   * 探测「该应用开没开通 offline_access」（缺省走 `probeOfflineAccess` 真读权限清单）。
   *
   * 为什么必须有这一步，而不是“先要长期、被拒再降级”：飞书在应用没开通时
   * **直接在 accounts.feishu.cn 上渲染 20027 错误页**，不回跳 `redirect_uri`——
   * 客户端根本没有“被拒”这个事件可接，用户看到的是死页（点刷新也无效）。
   * 详见 `offline-access.ts` 的文件头。
   *
   * 测试注入用：它决定链接带不带 `offline_access`。
   */
  probeOfflineAccess?: (appId: string, appSecret: string) => Promise<OfflineAccessProbeResult>
  log?: (level: "info" | "warn", msg: string) => void
  /** 当前时刻（测试注入；缺省 `Date.now`）。授权链接的保质期判定靠它。 */
  now?: () => number
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
/** 前置提醒窗口：remainingMs 低于它即标 `expiring`（仍可用但即将失效）。 */
const EXPIRING_WINDOW_MS = 10 * 60_000
/** 探测结论的保质期：“支持长期”这类稳定属性判得久一点，少打无谓的接口。 */
const SUPPORTED_TTL_MS = 30 * 60_000
/** “不支持/读不到”判得短一点：管理员刚开通权限后要能自动跟上，不用重启。 */
const UNSUPPORTED_TTL_MS = 5 * 60_000

export class PersonaCredentials {
  private lastTicket: AuthorizeTicket | null = null
  /**
   * 本次进程内探测到的长期授权支持（按 appId 缓存，**带保质期**）。
   *
   * 为何要保质期：它缓存的是**应用属性**（后台开没开通），而属性会变——
   * 管理员刚开了「持续访问已授权的数据」、刚撤销了它，都得能自己发现。
   * 永久记住的后果是：开通后永远只能拿 2 小时短期令牌，且没人知道为什么。
   */
  private offlineSupport = new Map<string, { result: OfflineAccessProbeResult; at: number }>()
  /** 进行中的探测（并发调用共享同一次网络请求）。 */
  private probes = new Map<string, Promise<OfflineAccessProbeResult>>()

  constructor(private readonly opts: PersonaCredentialsOptions) {}

  /**
   * 当前状态（**会顺带尝试刷新**，因为「临期但可刷」对用户来说就是可用）。
   *
   * 为什么不只返回快照：用户看到「需要授权」却被实际能跑的 token 挡着，
   * 会白点一次授权。宁可多花一次刷新，也不要造成误提示。
   */
  async status(): Promise<PersonaCredentialStatus> {
    let resolved: Awaited<ReturnType<PersonaTokenStore["resolve"]>>
    try {
      resolved = await this.opts.store.resolve()
    } catch (err) {
      // 读盘/解析意外抛错（不是「没有令牌」那种正常态）：如实标 error，
      // 而不是当成 missing 给一个授权链接把人引去白点一次授权。
      return {
        ready: false,
        state: "error",
        message: `凭据读取异常：${String((err as Error)?.message || err)}`,
        needsAuth: true,
        ...(this.ensureTicket() ? { authorizeUrl: this.ensureTicket()!.url } : {}),
      }
    }
    if (resolved.status === "ok") {
      const remaining = resolved.remainingMs
      // 无 refresh_token 时令牌不可续：到期即需人工重新授权。remaining 进入前置
      // 窗口就标 expiring（提前叫人），而不是等它过期变 expired——那时人格已失声。
      const renewable = !!resolved.token.refreshToken
      if (!renewable && remaining < EXPIRING_WINDOW_MS) {
        return {
          ready: true,
          state: "expiring",
          message: `令牌即将过期（剩 ${Math.max(0, Math.round(remaining / 60_000))} 分钟，无 refresh_token 不可自动续期）——请提前重新授权`,
          ...(resolved.token.name ? { authorizedUser: resolved.token.name } : {}),
          expireAt: resolved.token.expireAt,
          remainingMs: remaining,
          scopes: resolved.token.scopes,
          needsAuth: true,
        }
      }
      return {
        ready: true,
        state: "ok",
        message: resolved.token.name ? `已授权（${resolved.token.name}）` : "已授权",
        ...(resolved.token.name ? { authorizedUser: resolved.token.name } : {}),
        expireAt: resolved.token.expireAt,
        remainingMs: remaining,
        scopes: resolved.token.scopes,
        needsAuth: false,
      }
    }
    // 缺或过期：给一个授权链接（同一时刻只保留最近一次，避免链接泛滥）
    //
    // 出链接前先等一小会儿探测结果：链接带不带 offline_access 取决于它，
    // 而这张卡会直接推给用户——发一个进去就是 20027 死页的链接等于白推。
    // 超时（3s）则先按短期发，探测继续在后台跑（结果入缓存，下次取链接就是对的）。
    const appId = this.opts.appId()
    const appSecret = this.opts.appSecret()
    if (appId && appSecret && !this.knownSupport(appId)) {
      await this.probeWithin(appId, appSecret, 3000)
    }
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
    // 已探测到支持长期 → 直接给长期链接；未知/不支持 → 先给**不带** offline_access 的
    // （宁可用短期，也不发一个必然撞死页的链接），探测完确认支持后会换成长期票。
    const known = this.knownSupport(appId)
    const ticket = this.mintTicket(appId, appSecret, known?.support !== "supported")
    this.lastTicket = ticket
    this.opts.log?.("info", `[persona:credentials] 已生成授权链接（${PENDING_HINT}）`)
    if (known === undefined) void this.resolveOfflineSupport(appId, appSecret)
    return ticket
  }

  /** 未过期的探测结果；没有/过期返回 undefined（调用方自行重探）。 */
  private knownSupport(appId: string): OfflineAccessProbeResult | undefined {
    const entry = this.offlineSupport.get(appId)
    if (!entry) return undefined
    const ttl = entry.result.support === "supported" ? SUPPORTED_TTL_MS : UNSUPPORTED_TTL_MS
    if (this.now() - entry.at >= ttl) {
      this.offlineSupport.delete(appId)
      return undefined
    }
    return entry.result
  }

  /**
   * 该应用是否支持长期授权（命中缓存则直接返回）。
   *
   * 探测失败/读不到清单 → `unknown`，按**不支持**处理（发短期链接）：
   * 拿得到 2 小时令牌，总好过一个必然打不开的授权页。
   */
  async resolveOfflineSupport(appId: string, appSecret: string): Promise<OfflineAccessProbeResult> {
    const cached = this.knownSupport(appId)
    if (cached) return cached
    const running = this.probes.get(appId)
    if (running) return running
    const probe =
      this.opts.probeOfflineAccess ?? ((id: string, secret: string) => probeOfflineAccess({ appId: id, appSecret: secret }))
    const task = probe(appId, appSecret)
      .catch((err): OfflineAccessProbeResult => ({ support: "unknown", reason: String((err as Error)?.message || err) }))
      .then((result) => {
        this.offlineSupport.set(appId, { result, at: this.now() })
        this.probes.delete(appId)
        if (result.support === "supported") {
          this.opts.log?.("info", `[persona:credentials] 应用已开通长期授权（offline_access），授权链接申请长期令牌`)
        } else {
          this.opts.log?.(
            "warn",
            `[persona:credentials] 按**短期授权**处理（${
              result.support === "unsupported" ? "应用权限清单里没有 offline_access" : "无法确认应用是否开通"
            }）：${result.reason ?? ""}\n` +
              `  短期令牌最长 2 小时，到期前会弹卡提醒重新授权。想免重复授权：飞书开放平台 → 该应用 → 权限管理 → 开通「持续访问已授权的数据」并发布版本。`,
          )
        }
        // 探测前已经发出的短期票据：确认支持长期后换成长期票
        if (result.support === "supported") this.promoteTicket(appId, appSecret)
        return result
      })
    this.probes.set(appId, task)
    return task
  }

  /**
   * 等探测结果最多 `ms` 毫秒（超时就先按“不确定”走，探测继续在后台跑）。
   *
   * 为何不等到底：探测是一次外部 HTTP（最坏 8 秒超时），而它在**状态查询**路径上——
   * 让面板/启动流程为一个“能不能长期授权”的优化等 8 秒是不划算的。
   * 超时后发出去的短期链接仍然能用，下一次取链接时探测结果已在缓存里。
   */
  private async probeWithin(appId: string, appSecret: string, ms: number): Promise<OfflineAccessProbeResult | undefined> {
    const cached = this.knownSupport(appId)
    if (cached) return cached
    if (!this.probes.has(appId)) void this.resolveOfflineSupport(appId, appSecret)
    const timeout = new Promise<undefined>((resolve) => {
      const t = setTimeout(() => resolve(undefined), ms)
      // 定时器不持有事件循环（否则进程退出要被它拖住）
      ;(t as { unref?: () => void }).unref?.()
    })
    return Promise.race([this.resolveOfflineSupport(appId, appSecret), timeout])
  }

  /**
   * 把当前短期票据换成**带** offline_access 的版本。
   *
   * 只在“手上这张是短期票”时换——已是长期票或没有票据就什么都不做。
   */
  private promoteTicket(appId: string, appSecret: string): void {
    if (this.lastTicket !== null && !this.lastTicket.url.includes(`client_id=${appId}`)) return
    if (this.lastTicket !== null && this.lastTicket.scopes.includes(OFFLINE_ACCESS_SCOPE)) return
    try {
      this.lastTicket = this.mintTicket(appId, appSecret, false)
    } catch {
      // 换票失败无关紧要：短期链接本身可用
    }
  }

  /**
   * 飞书仍因为 offline_access 拒了（20027）→ 改用**不带它**的链接重发。
   *
   * 主路径是**事前探测**（见 `resolveOfflineSupport`）：探测不到时本来就不带。
   * 这里是探测误判（如清单读到了旧快照、后台刚改）与旧链接遗留的兜底：
   * 把“本次进程内这个应用支持长期”改判为不支持，并发一张当下可用的短期链接。
   *
   * 只改本次进程内的判断、不落盘：后台随时可能开通，重启后重新探测。
   */
  downgradeToShortLived(): AuthorizeTicket {
    const appId = this.opts.appId()
    const appSecret = this.opts.appSecret()
    if (!appId || !appSecret) {
      throw new Error("未配置飞书应用凭据（AppID/AppSecret）：无法重新发起授权")
    }
    this.offlineSupport.set(appId, { result: { support: "unsupported", reason: "授权链上被飞书拒（20027）" }, at: this.now() })
    const ticket = this.mintTicket(appId, appSecret, true)
    this.lastTicket = ticket
    this.opts.log?.(
      "warn",
      `[persona:credentials] 飞书拒绝了长期授权（20027），已改为**短期授权**链接；` +
        `飞书令牌最长 2 小时，到期会提醒你重新授权。想免重复授权：飞书开放平台 → 权限管理 → 开通「持续访问已授权的数据」。`,
    )
    return ticket
  }

  /** 生成一张授权票据（`withoutOfflineAccess` 时从 scopes 里剔掉它）。 */
  private mintTicket(appId: string, appSecret: string, withoutOfflineAccess: boolean): AuthorizeTicket {
    // scopes 延迟求值：它读的是渠道实例配置（渠道在凭据对象之后才展开）
    // 未注入时用基础集：否则“剔掉 offline_access”会落空（undefined 剔不了任何东西），
    // 而 OAuth 层会在缺 scopes 时回落成**带 offline_access 的** PERSONA_SCOPES。
    const declared = this.opts.scopes?.() ?? PERSONA_SCOPES
    const scopes = withoutOfflineAccess ? stripScope(declared, OFFLINE_ACCESS_SCOPE) : declared
    return this.opts.oauth.startAuthorize({
      appId,
      appSecret,
      redirectUri: this.opts.redirectUri(),
      ...(scopes ? { scopes } : {}),
    })
  }

  /**
   * 用授权码兑换并落盘。
   *
   * 成功后清掉 `lastTicket`（旧的授权链接不应再被展示）。
   */
  async complete(rawCode: string, state?: string): Promise<PersonaCredentialStatus> {
    let entry: PersonaUserToken
    try {
      entry = await this.opts.oauth.exchange({ code: rawCode, ...(state ? { state } : {}) })
    } catch (err) {
      // 兑换阶段才报 20027（少数情况下授权页放行了、换令牌时才拒）：
      // 同样退一步——改发不带 offline_access 的链接，让用户重新点一下。
      if (isOfflineAccessRejection(err)) {
        this.downgradeToShortLived()
        throw new Error(
          "该飞书应用未开通长期授权（offline_access 报 20027）——已改为**短期授权**，请重新点一次授权链接。\n" +
            "想免重复授权：飞书开放平台 → 该应用 → 权限管理 → 开通「持续访问已授权的数据」。",
        )
      }
      throw err
    }
    await this.opts.store.set(entry)
    this.lastTicket = null
    // 以**实际拿到的** refresh_token 为准判定长期/短期，不凭“我请求过”推断：
    // 飞书可以不理 offline_access（应用没开通、或管理员拒了），那样就是短期。
    const longLived = !!entry.refreshToken
    this.opts.log?.(
      "info",
      `[persona:credentials] 授权完成${entry.name ? `（${entry.name}）` : ""}：` +
        (longLived ? "长期（可自动续期）" : "短期（无 refresh_token，到期需重新授权）"),
    )
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

  /**
   * 用户本人的飞书 open_id（拿不到返回 undefined）。
   *
   * 用途：判定「这条消息是不是用户自己发的」（见 `speaker-filter`）。
   * 为何从令牌写体取而不是现查接口：`open_id` 在授权兑换时已顺手存下，
   * 多打一次 `user_info` 只为拿它没意义。
   *
   * 令牌过期也照返回：“用户是谁”不会因令牌过期而变，而判定身份恰恰在
   * 凭据不可用时也需要（比如决定要不要提醒授权）。
   */
  async ownerOpenId(): Promise<string | undefined> {
    try {
      const snapshot = await this.opts.store.snapshot()
      return snapshot.token?.openId?.trim() || undefined
    } catch {
      return undefined
    }
  }

  /** 撤销授权（清除落盘令牌）。 */
  async revoke(): Promise<void> {
    await this.opts.store.clear()
    this.lastTicket = null
    this.opts.log?.("info", "[persona:credentials] 已撤销飞书授权")
    this.opts.onChange?.(await this.status())
  }

  /**
   * 最近一次生成的授权链接（**未过期**；已过期返回 null）。
   *
   * 为何要看保质期：链接里的 `state` 在 OAuth 侧只有 10 分钟（`PENDING_TTL_MS`，
   * 过时即从 pending 里 GC 掉）。而上层（授权卡）会在过期后**重推**卡片——
   * 若这里无脑返回 `lastTicket`，重推出去的是一张“10 分钟前就没用了”的链接：
   * 用户点开、走完飞书授权页，回来报到“授权会话已过期”。
   *
   * 实测路径：相期未运行 → 启动时推卡失败/无人看见 → 5 分钟定时器发现卡过期 →
   * 重推 → 用户点了一个死链接。现象是“一直授权不成功”，但服务端日志全绿。
   */
  authorizeUrl(): string | null {
    if (this.lastTicket === null) return null
    // 应用变了 → 旧票据失效（同 ensureTicket 的理由：client_id/scopes 都是旧应用的）
    const appId = this.opts.appId()
    if (appId === undefined || !this.lastTicket.url.includes(`client_id=${appId}`)) {
      this.lastTicket = null
      return null
    }
    if (this.ticketExpired(this.lastTicket)) {
      this.lastTicket = null
      return null
    }
    return this.lastTicket.url
  }

  /** 链接是否已过期（`issuedAt` 缺省时视为不过期：兼容旧票据形态）。 */
  private ticketExpired(ticket: AuthorizeTicket): boolean {
    const issuedAt = (ticket as { issuedAtMs?: number }).issuedAtMs
    if (issuedAt === undefined) return false
    return this.now() - issuedAt >= ticket.expiresInMs
  }

  /**
   * 惰性生成授权链接（状态查询用）。
   *
   * 没有配置应用凭据时返回 null——此时前端显示「需要配置应用凭据」而不是
   * 给一个必然报错的授权链接。
   *
   * **过期即重新生成**（而不是继续用旧的）：上层拿到 null 就会以“没有链接”处理，
   * 而拿到一个死链接会把它展示给用户并让人白跑一趟授权页。
   */
  private ensureTicket(): AuthorizeTicket | null {
    const appId = this.opts.appId()
    const appSecret = this.opts.appSecret()
    if (this.lastTicket !== null) {
      // 应用换了就不能再用旧票据：票据里的 client_id / scopes 都是旧应用的，
      // 拿它去授权会得到一个属于另一个应用的令牌（或者 20027）。
      const sameApp = appId !== undefined && this.lastTicket.url.includes(`client_id=${appId}`)
      if (sameApp && !this.ticketExpired(this.lastTicket)) return this.lastTicket
      this.lastTicket = null
    }
    if (!appId || !appSecret) return null
    try {
      // 已探测过且确认支持长期 → 直接出长期链接；否则先出短期，探测在后台补上
      const ticket = this.mintTicket(appId, appSecret, this.knownSupport(appId)?.support !== "supported")
      this.lastTicket = ticket
      if (!this.knownSupport(appId)) void this.resolveOfflineSupport(appId, appSecret)
      return ticket
    } catch {
      return null
    }
  }

  /** 当前时刻（可用测试注入）。 */
  private now(): number {
    return this.opts.now?.() ?? Date.now()
  }
}

const PENDING_HINT = "10 分钟内有效"
