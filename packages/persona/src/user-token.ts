/**
 * 拟人模式的**进程级**飞书用户令牌：长期有效、自动刷新、落盘可恢复。
 *
 * ## 为什么不能用会话级令牌（`feishu_docs` 那套）
 *
 * | | 会话级（`feishu_docs`） | 进程级（本模块） |
 * |---|---|---|
 * | 归属 | 某个对话（`sessionId`） | 整个人格运行时 |
 * | 生命周期 | 跟随会话 | 跨会话、跨任务、跨重启 |
 * | 谁授权 | 用户在对话里主动发起 | 拟人模式启动时自动检查，缺了就提示用户 |
 * | 缺了会怎样 | 该次文档操作失败 | **所有对外动作都做不了**（人格无法回消息） |
 *
 * 人格是常驻的（「小霜一直在线答支持群」），它的凭据不能绑在某次对话上——
 * 否则「用户开了个新对话，人格就失忆了」。
 *
 * ## 为什么必须自动刷新
 *
 * 飞书 user_access_token 有效期 2 小时，refresh_token 通常 30 天。人格是常驻的，
 * 若只在启动时检查一次，跑过 2 小时就会全部失败。所以：
 *
 * - 取用时**先看是否临近过期**（提前 5 分钟），临期即刷
 * - 刷新失败（refresh_token 也过期）→ 如实上报 `needsAuth`，由上层提示用户重新授权
 * - 刷新成功**立即落盘**（否则进程重启后手上还是旧 token，白刷一次）
 *
 * ## 落盘位置与权限
 *
 * `{GEBAI_HOME}/persona/feishu-user-token.json`（0600）。
 * 放 `persona/` 下与人格的其他状态（`drivers.json`）同级，迁移时一起带走。
 */

import { mkdir, readFile, rename, writeFile } from "node:fs/promises"
import { join } from "node:path"

/** 与 `@gebai/agents` 的 `UserTokenEntry` 同形（结构化兼容，无需适配器）。 */
export interface PersonaUserToken {
  accessToken: string
  expireAt: number
  refreshToken?: string
  refreshExpireAt?: number
  scopes: string[]
  name?: string
  openId?: string
  /** 绑定的飞书应用（换应用后旧 token 无效，据 this 判断是否还能用）。 */
  appId?: string
  /** 最近一次写入时间（诊断用）。 */
  updatedAt?: number
}

/** 令牌状态：上层据此决定「能不能跑」与「要不要提示用户授权」。 */
export type TokenState =
  | { status: "ok"; token: PersonaUserToken; /** 剩余有效毫秒 */ remainingMs: number }
  | { status: "expired"; reason: string; token?: PersonaUserToken }
  | { status: "missing"; reason: string }

export interface PersonaTokenStoreOptions {
  home: string
  /** 当前应用 AppID（用于判断落盘 token 是否属于当前应用；换应用即失效）。 */
  appId?: () => string | undefined
  /** 当前应用 AppSecret（刷新令牌需要；缺则刷新必然失败，会在状态里如实报）。 */
  appSecret?: () => string | undefined
  /** OAuth 兑换/刷新端点（accounts.feishu.cn，与 open.feishu.cn 不同域）。 */
  tokenUrl?: string
  fetchImpl?: typeof fetch
  now?: () => number
  log?: (level: "info" | "warn", msg: string) => void
}

/** 提前量：临期 5 分钟就刷（避免"恰好卡在过期瞬间"）。 */
const REFRESH_SKEW_MS = 5 * 60_000

const TOKEN_FILE = "feishu-user-token.json"

/**
 * 进程级用户令牌管理器。
 *
 * 并发安全：`resolve()` 可能被多个 driver 同时调用（多个分身一起被唤醒）。
 * 用单飞（single-flight）保证同一时刻只有一次刷新请求——否则 N 个并发会
 * 拿同一个 refresh_token 去换，飞书侧通常会作废前面的，导致互相踩。
 */
export class PersonaTokenStore {
  private readonly path: string
  private readonly fetchImpl: typeof fetch
  private readonly now: () => number
  private cache: PersonaUserToken | null = null
  private loaded = false
  private inflight: Promise<TokenState> | null = null
  /** 最近一次失败原因（诊断/提示用）。 */
  private lastError: string | null = null
  /** 上次是否已提示过需要授权（避免每次取用都重复告警）。 */
  private warnedNeedsAuth = false

  constructor(private readonly opts: PersonaTokenStoreOptions) {
    this.path = join(opts.home, "persona", TOKEN_FILE)
    this.fetchImpl = opts.fetchImpl ?? fetch
    this.now = opts.now ?? Date.now
  }

  /** 令牌文件路径（诊断用）。 */
  filePath(): string {
    return this.path
  }

  /** 最近一次失败原因（无失败为 null）。 */
  error(): string | null {
    return this.lastError
  }

  /** 读盘（幂等；首次调用装载）。 */
  private async load(): Promise<PersonaUserToken | null> {
    if (this.loaded) return this.cache
    this.loaded = true
    try {
      const text = await readFile(this.path, "utf8")
      this.cache = JSON.parse(text) as PersonaUserToken
    } catch {
      this.cache = null // 不存在/坏文件一律当没有（不阻断启动，交给授权流程重建）
    }
    return this.cache
  }

  /**
   * 落盘。
   *
   * 原子写（tmp + rename）：token 是**唯一凭据**，写一半断电会让文件损坏，
   * 下次启动就得让用户重新授权——这是能避免的打扰。
   */
  private async save(entry: PersonaUserToken | null): Promise<void> {
    this.cache = entry
    try {
      await mkdir(join(this.opts.home, "persona"), { recursive: true })
      if (entry === null) {
        // 清空：写一个空对象而不是删文件（保留 0600 权限语义，且便于诊断"曾经有过"）
        await writeFile(this.path, "{}", { encoding: "utf8", mode: 0o600 })
        return
      }
      const body = JSON.stringify({ ...entry, updatedAt: this.now() }, null, 2)
      const tmp = `${this.path}.tmp`
      await writeFile(tmp, body, { encoding: "utf8", mode: 0o600 })
      await rename(tmp, this.path).catch(async () => {
        await writeFile(this.path, body, { encoding: "utf8", mode: 0o600 })
      })
    } catch (err) {
      // 落盘失败只记日志：内存里仍有可用 token，不该让本次运行失败
      this.opts.log?.("warn", `[persona:token] 写入失败（本次运行不受影响）: ${String((err as Error)?.message || err)}`)
    }
  }

  /** 写入一份新令牌（OAuth 回调兑换成功后调用）。 */
  async set(entry: PersonaUserToken): Promise<void> {
    this.lastError = null
    this.warnedNeedsAuth = false
    // 保留调用方给的 appId（兑换时它就是 pending 里的应用）；没给才用当前配置。
    // 不能用当前配置无条件覆盖：那会让「旧应用的 token 在换应用后看起来仍属于新应用」，
    // 换应用检查就永远不生效。
    const appId = entry.appId ?? this.opts.appId?.()
    await this.save({ ...entry, ...(appId ? { appId } : {}) })
    this.opts.log?.("info", `[persona:token] 已保存用户令牌${entry.name ? `（${entry.name}）` : ""}`)
  }

  /** 清除（用户取消授权/换应用时）。 */
  async clear(): Promise<void> {
    this.lastError = null
    this.warnedNeedsAuth = false
    await this.save(null)
  }

  /**
   * 取一个**可用**的令牌（必要时自动刷新）。
   *
   * 返回 `missing`/`expired` 时上层应提示用户授权——**不抛错**：
   * 「没有凭据」是正常状态（首次部署），不是异常。
   */
  async resolve(): Promise<TokenState> {
    if (this.inflight) return this.inflight // 单飞：并发只刷一次
    this.inflight = this.resolveInner().finally(() => {
      this.inflight = null
    })
    return this.inflight
  }

  private async resolveInner(): Promise<TokenState> {
    const entry = await this.load()
    if (!entry || !entry.accessToken) {
      return this.needsAuth("尚未完成飞书用户授权")
    }

    // 换应用后旧 token 无效：直接要求重新授权（否则会拿着别的应用的 token 去调，报错难懂）
    const wantApp = this.opts.appId?.()
    if (wantApp && entry.appId && entry.appId !== wantApp) {
      return this.needsAuth(`令牌属于应用 ${entry.appId.slice(0, 8)}…，与当前应用不一致（换了应用需重新授权）`)
    }

    const remaining = entry.expireAt - this.now()
    if (remaining > REFRESH_SKEW_MS) {
      this.lastError = null
      this.warnedNeedsAuth = false
      return { status: "ok", token: entry, remainingMs: remaining }
    }

    // 临期/已过期：尝试刷新
    if (!entry.refreshToken) {
      return this.needsAuth("access_token 已过期且没有 refresh_token", entry)
    }
    if (entry.refreshExpireAt !== undefined && entry.refreshExpireAt <= this.now()) {
      return this.needsAuth("refresh_token 也已过期，需重新授权", entry)
    }

    try {
      const next = await this.refresh(entry)
      await this.save(next) // 立即落盘：否则重启后又是旧 token
      this.opts.log?.("info", "[persona:token] 用户令牌已自动刷新")
      return { status: "ok", token: next, remainingMs: next.expireAt - this.now() }
    } catch (err) {
      const reason = `刷新令牌失败：${String((err as Error)?.message || err)}`
      return this.needsAuth(reason, entry)
    }
  }

  /**
   * 标记「需要授权」并按需记一次日志。
   *
   * 为何只记一次：令牌缺失时每个事件都会调 `resolve()`，若每次都 warn，
   * 日志会被同一句话刷屏（反而把真正的问题埋掉）。
   */
  private needsAuth(reason: string, token?: PersonaUserToken): TokenState {
    this.lastError = reason
    if (!this.warnedNeedsAuth) {
      this.warnedNeedsAuth = true
      this.opts.log?.("warn", `[persona:token] 需要飞书授权：${reason}`)
    }
    return token ? { status: "expired", reason, token } : { status: "missing", reason }
  }

  /**
   * 用 refresh_token 换新令牌。
   *
   * 保留原 name/openId/appId：刷新响应通常不回这两个，丢掉会让诊断信息消失。
   */
  private async refresh(entry: PersonaUserToken): Promise<PersonaUserToken> {
    const appId = this.opts.appId?.()
    if (!appId) throw new Error("未配置应用 AppID，无法刷新")
    const appSecret = this.opts.appSecret?.()
    if (!appSecret) throw new Error("未配置应用 AppSecret，无法刷新")

    const res = await this.fetchImpl(this.opts.tokenUrl ?? "https://accounts.feishu.cn/oauth/v3/token", {
      method: "POST",
      headers: { "Content-Type": "application/json; charset=utf-8" },
      body: JSON.stringify({
        grant_type: "refresh_token",
        client_id: appId,
        client_secret: appSecret,
        refresh_token: entry.refreshToken,
      }),
    })
    const body = (await res.json().catch(() => ({}))) as Record<string, unknown>
    const code = Number(body.code ?? 0)
    if (!res.ok || (code && code !== 0)) {
      throw new Error(`code=${code} ${String(body.error_description ?? body.msg ?? body.error ?? "")}`.trim())
    }
    const accessToken = String(body.access_token ?? "")
    if (!accessToken) throw new Error("刷新响应缺少 access_token")

    const now = this.now()
    const expiresIn = Number(body.expires_in ?? 7200)
    const refreshExpiresIn = Number(body.refresh_token_expires_in ?? 0)
    return {
      accessToken,
      expireAt: now + expiresIn * 1000,
      ...(body.refresh_token ? { refreshToken: String(body.refresh_token) } : entry.refreshToken ? { refreshToken: entry.refreshToken } : {}),
      ...(refreshExpiresIn > 0
        ? { refreshExpireAt: now + refreshExpiresIn * 1000 }
        : entry.refreshExpireAt !== undefined
          ? { refreshExpireAt: entry.refreshExpireAt }
          : {}),
      scopes: Array.isArray(body.scope) ? (body.scope as string[]) : entry.scopes,
      ...(entry.name ? { name: entry.name } : {}),
      ...(entry.openId ? { openId: entry.openId } : {}),
      ...(entry.appId ?? appId ? { appId: entry.appId ?? appId } : {}),
    }
  }

  /** 只读快照（状态接口用；不触发刷新）。 */
  async snapshot(): Promise<{ token: PersonaUserToken | null; needsAuth: boolean; reason: string | null; filePath: string }> {
    const entry = await this.load()
    const usable = !!entry?.accessToken && entry.expireAt > this.now()
    return {
      token: entry,
      needsAuth: !usable,
      reason: usable ? null : (this.lastError ?? "尚未完成飞书用户授权"),
      filePath: this.path,
    }
  }
}
