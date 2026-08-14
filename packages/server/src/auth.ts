import { randomBytes, scryptSync, timingSafeEqual, createHmac, createHash } from "node:crypto"
import { mkdir, readFile, writeFile } from "node:fs/promises"
import { join } from "node:path"
import type { UserInfo, UserPatch } from "@gebai/sdk"

const TOKEN_TTL = 7 * 24 * 3600 * 1000
/** 登录失败锁定：连续失败次数与锁定时长（防在线爆破）。 */
const AUTH_MAX_FAILS = 5
const AUTH_LOCK_MS = 60 * 1000
/** 登录失败记录条目上限（防匿名 Basic/登录请求以随机用户名无界填充内存）。 */
const AUTH_MAX_FAIL_ENTRIES = 10000

export interface AuthUser extends UserInfo {
  salt: string
  hash: string
}

export class AuthService {
  private registryPath: string
  private tokenSecret = randomBytes(32).toString("hex")
  private tokens = new Map<string, { userId: string; exp: number }>()
  private loginFails = new Map<string, { count: number; until: number }>()

  constructor(
    private home: string,
    private mode: "local" | "server",
  ) {
    this.registryPath = join(home, "users", "registry.json")
  }

  /** 本地模式默认用户：admin（特权用户，无登录、不受任何权限限制）。 */
  defaultUser(): AuthUser {
    return { id: "admin", username: "admin", role: "admin", disabled: false, createdAt: Date.now(), salt: "", hash: "" }
  }

  private async readRegistry(): Promise<Record<string, AuthUser>> {
    if (this.mode === "local") return {}
    try {
      return JSON.parse(await readFile(this.registryPath, "utf8")) as Record<string, AuthUser>
    } catch {
      return {}
    }
  }

  private async writeRegistry(reg: Record<string, AuthUser>): Promise<void> {
    await mkdir(join(this.home, "users"), { recursive: true })
    await writeFile(this.registryPath, JSON.stringify(reg, null, 2))
  }

  private hashPassword(password: string, salt: string): string {
    return scryptSync(password, salt, 64).toString("hex")
  }

  private verifyPassword(password: string, salt: string, expected: string): boolean {
    const actual = Buffer.from(this.hashPassword(password, salt), "hex")
    const exp = Buffer.from(expected, "hex")
    if (actual.length !== exp.length) return false
    return timingSafeEqual(actual, exp)
  }

  private sign(userId: string): string {
    const payload = `${userId}.${Date.now() + TOKEN_TTL}`
    const sig = createHmac("sha256", this.tokenSecret).update(payload).digest("hex")
    const token = `${Buffer.from(payload).toString("base64url")}.${sig}`
    this.tokens.set(token, { userId, exp: Date.now() + TOKEN_TTL })
    return token
  }

  /**
   * 凭据直验（HTTP Basic 认证用）：与 login 相同的密码校验与登录限流，但不签发令牌（单次请求场景，避免令牌堆积）。
   * 失败/禁用/锁定期一律返回 null（统一 401，不泄露原因）。
   */
  async verifyCredentials(username: string, password: string): Promise<AuthUser | null> {
    const fail = this.loginFails.get(username)
    if (fail && fail.until > Date.now()) return null // 锁定期内直接拒绝（不泄露原因）
    const reg = await this.readRegistry()
    const user = reg[username]
    if (!user) {
      // 恒时：用户名不存在也执行一次等成本 scrypt（防用户名枚举时序侧信道——存在用户才会走慢路径）
      this.hashPassword(password, randomBytes(16).toString("hex"))
      this.recordLoginFail(username)
      return null
    }
    if (user.disabled || !this.verifyPassword(password, user.salt, user.hash)) {
      this.recordLoginFail(username)
      return null
    }
    this.loginFails.delete(username)
    return user
  }

  /** 登录失败计数（含锁定期过期条目清理与容量上限，防随机用户名无界填充）。 */
  private recordLoginFail(username: string): void {
    const f = this.loginFails.get(username)
    // 仅清理「已进入锁定期且已过期」的条目（until=0 表示未锁定，不能删——否则计数永远到不了锁定阈值）
    if (f && f.until > 0 && f.until <= Date.now()) this.loginFails.delete(username)
    const cur = this.loginFails.get(username) || { count: 0, until: 0 }
    cur.count++
    if (cur.count >= AUTH_MAX_FAILS) {
      cur.until = Date.now() + AUTH_LOCK_MS
      cur.count = 0
    }
    // 容量上限：超限时先清已过期条目；仍满则按插入序淘汰最旧条目（Map 迭代序=插入序），
    // 保证任何用户名（含真实目标账号）的失败都能计数——被攻击账号不可能因 Map 满而失去锁定保护
    if (this.loginFails.size >= AUTH_MAX_FAIL_ENTRIES) {
      for (const [k, v] of this.loginFails) {
        if (v.until > 0 && v.until <= Date.now()) this.loginFails.delete(k)
      }
      if (this.loginFails.size >= AUTH_MAX_FAIL_ENTRIES) {
        const oldest = this.loginFails.keys().next().value
        if (oldest !== undefined) this.loginFails.delete(oldest)
      }
    }
    this.loginFails.set(username, cur)
  }

  async login(username: string, password: string): Promise<string | null> {
    const user = await this.verifyCredentials(username, password)
    return user ? this.sign(user.id) : null
  }

  /**
   * 开放注册（服务模式）：注册用户**恒为普通角色**（admin 唯一入口是启动参数 GEBAI_ADMIN_PASSWORD_HASH，
   * 不可经注册创建/提权）。
   * - open 模式（默认）：注册即登录（返回令牌，enabled）
   * - approval 模式：注册用户置 disabled+pending（待 admin 审批，不可登录，不签发令牌）
   * 用户名限 ≤64 字符且禁路径分隔符（防用户目录越界）。
   */
  async register(username: string, password: string, mode: "open" | "approval" = "open"): Promise<{ user: AuthUser; token: string | null; pending: boolean }> {
    const name = username.trim()
    if (!name || name.length > 64 || /[\/\\]/.test(name)) throw new Error("用户名非法（≤64 字符，不含路径分隔符）")
    if (!password) throw new Error("密码不能为空")
    const pending = mode === "approval"
    const user = await this.createUser(name, password, "user", { disabled: pending, pending })
    return { user, token: pending ? null : this.sign(user.id), pending }
  }

  /**
   * 外部身份兑换：验证通过的外部用户名 → GEBAI 用户令牌。
   * autocreate=true 时不存在的外部用户自动创建（普通角色、随机密码不可密码登录）；false 时仅允许已存在用户。
   * 失败/禁用/白名单外一律返回 null（统一 401，不泄露原因）；连续失败触发与登录一致的锁定（防回调模式被无限触发）。
   */
  async exchangeExternal(username: string, provider: { verify(c: { username: string; credential: string }): Promise<string | null> }, credential: string, autocreate: boolean): Promise<string | null> {
    const fail = this.loginFails.get(username)
    if (fail && fail.until > Date.now()) return null // 锁定期内直接拒绝（不泄露原因）
    const name = (await provider.verify({ username, credential }))?.trim()
    if (!name || name.length > 64 || /[\/\\]/.test(name)) {
      this.recordLoginFail(username)
      return null
    }
    this.loginFails.delete(username)
    const reg = await this.readRegistry()
    let user = reg[name]
    if (!user && autocreate) {
      try {
        user = await this.createUser(name, randomBytes(32).toString("hex"), "user")
      } catch {
        user = (await this.readRegistry())[name] // 并发创建竞态：已存在则复用
      }
    }
    if (!user || user.disabled) return null
    return this.sign(user.id)
  }

  async logout(token: string): Promise<void> {
    this.tokens.delete(token)
  }

  async authorize(token: string): Promise<AuthUser | null> {
    const t = this.tokens.get(token)
    if (!t) return null
    if (t.exp < Date.now()) {
      this.tokens.delete(token)
      return null
    }
    const reg = await this.readRegistry()
    for (const u of Object.values(reg)) {
      if (u.id === t.userId && !u.disabled) return u
    }
    return null
  }

  /**
   * 服务模式 admin 用户密码哈希引导（GEBAI_ADMIN_PASSWORD_HASH，格式 `salt:hash` 均 hex）：
   * - 设置：admin 用户不存在则创建（role=admin），存在则覆盖哈希并启用（disabled=false）——启动参数是权威配置，每次启动重置
   * - 未设置：注册表中已存在的 admin 用户标记为禁用（disabled=true），不创建新用户
   * 非法格式（salt 非 16 字节 hex / hash 非 64 字节 hex）抛错，启动即失败。
   */
  async applyAdminHash(hash: string | undefined): Promise<void> {
    const reg = await this.readRegistry()
    const admin = reg["admin"]
    if (!hash) {
      if (admin && !admin.disabled) {
        admin.disabled = true
        await this.writeRegistry(reg)
      }
      return
    }
    if (!/^[0-9a-f]{32}:[0-9a-f]{128}$/.test(hash)) {
      throw new Error("GEBAI_ADMIN_PASSWORD_HASH 格式非法：应为 `salt:hash`（均为 hex，salt 16 字节、hash 64 字节，与注册表 scrypt 哈希一致）；可用 `bun run --cwd packages/server hash-password` 生成")
    }
    const [salt, h] = hash.split(":")
    if (admin) {
      admin.salt = salt
      admin.hash = h
      admin.role = "admin"
      admin.disabled = false
    } else {
      reg["admin"] = {
        id: createHash("sha256").update(`admin${salt}`).digest("hex").slice(0, 16),
        username: "admin",
        role: "admin",
        disabled: false,
        createdAt: Date.now(),
        salt,
        hash: h,
      }
    }
    await this.writeRegistry(reg)
  }

  async listUsers(): Promise<AuthUser[]> {
    const reg = await this.readRegistry()
    return Object.values(reg).sort((a, b) => a.createdAt - b.createdAt)
  }

  async createUser(username: string, password: string, role: "user" | "admin" = "user", init?: { disabled?: boolean; pending?: boolean }): Promise<AuthUser> {
    const reg = await this.readRegistry()
    if (reg[username]) throw new Error(`user exists: ${username}`)
    const salt = randomBytes(16).toString("hex")
    const user: AuthUser = {
      id: createHash("sha256").update(username + salt).digest("hex").slice(0, 16),
      username,
      role,
      disabled: init?.disabled ?? false,
      pending: init?.pending,
      createdAt: Date.now(),
      salt,
      hash: this.hashPassword(password, salt),
    }
    reg[username] = user
    await this.writeRegistry(reg)
    return user
  }

  async updateUser(id: string, patch: UserPatch): Promise<AuthUser> {
    const reg = await this.readRegistry()
    const user = Object.values(reg).find((u) => u.id === id)
    if (!user) throw new Error(`user not found: ${id}`)
    if (patch.password) {
      user.salt = randomBytes(16).toString("hex")
      user.hash = this.hashPassword(patch.password, user.salt)
    }
    if (patch.role) user.role = patch.role
    if (patch.disabled !== undefined) user.disabled = patch.disabled
    if (patch.pending !== undefined) user.pending = patch.pending
    reg[user.username] = user
    await this.writeRegistry(reg)
    return user
  }

  async deleteUser(id: string): Promise<void> {
    const reg = await this.readRegistry()
    for (const [name, u] of Object.entries(reg)) {
      if (u.id === id) {
        delete reg[name]
        await this.writeRegistry(reg)
        return
      }
    }
    throw new Error(`user not found: ${id}`)
  }

  strip(user: AuthUser): UserInfo {
    return { id: user.id, username: user.username, role: user.role, disabled: user.disabled, pending: user.pending, createdAt: user.createdAt }
  }
}
