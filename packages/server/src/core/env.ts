import { readFile } from "node:fs/promises"
import { join } from "node:path"
import type { SessionStore } from "./store"
import type { EnvVarSource } from "@gebai/sdk"

const SENSITIVE_RE =
  /(_KEY|_TOKEN|_SECRET|PASSWORD|_PASSWD|_PAT|_CREDENTIAL|_CREDENTIALS|_AUTH|_SECRETS|CONNECTION_STRING|DATABASE_URL|PRIVATE_KEY|CLIENT_SECRET|APP_SECRET)$/i

export function isSensitive(name: string): boolean {
  return SENSITIVE_RE.test(name)
}

export function mask(value: string): string {
  if (!value) return ""
  if (value.length <= 6) return "***"
  return `${value.slice(0, 2)}****${value.slice(-2)}`
}

/** 环境变量记录脱敏（写入/返回路径共用）：敏感键值替换为掩码，其余原样。 */
export function maskEnv(env: Record<string, string>): Record<string, string> {
  const out: Record<string, string> = {}
  for (const [k, v] of Object.entries(env)) out[k] = isSensitive(k) ? mask(v) : v
  return out
}

/** 环境变量批量写入校验：必须为对象、值仅允许 string|null、名称仅标识符形式且拒绝 __proto__；返回错误消息或 null。 */
export function validateEnvVars(vars: unknown): string | null {
  if (typeof vars !== "object" || vars === null || Array.isArray(vars)) return "请求体必须是对象"
  for (const [k, v] of Object.entries(vars)) {
    if (k === "__proto__" || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(k) || (v !== null && typeof v !== "string")) {
      return `环境变量名非法: ${k}`
    }
  }
  return null
}

/**
 * 多用户模式下非管理员禁止设置审批跳过键（GEBAI_APPROVAL_SKIP）：
 * 该键会让 sh/py/cron 等敏感工具免审批执行，普通用户可借此绕过审批边界直接拿服务端执行权限。
 * 返回错误消息或 null（auth=none 默认用户即 admin，不拦截）。
 */
export function rejectApprovalSkip(authMode: string, role: string, vars: unknown): string | null {
  if (authMode === "server" && role !== "admin" && typeof vars === "object" && vars !== null && "GEBAI_APPROVAL_SKIP" in vars) {
    return "GEBAI_APPROVAL_SKIP 仅管理员可设置"
  }
  return null
}

/**
 * 浏览器本地 env 注入过滤（宽容模式，prompt 临时注入通道专用）：
 * 丢弃不支持/非法的变量、保留其余继续执行——前端 localStorage 可能残留旧版目录外/越权键
 * （如服务模式非管理员保存过的 GEBAI_APPROVAL_SKIP、ask_env 存过的目录外变量），
 * 拒绝整个任务会阻断正常使用；显式管理通道（session.env.set 等）仍走严格校验（validateEnvVars/rejectApprovalSkip）。
 * 过滤规则：非 string 值/null、非法标识符名（含 __proto__）、服务模式非管理员的 GEBAI_APPROVAL_SKIP。
 */
export function filterEnvInjection(env: Record<string, string | null>, authMode: string, role: string): Record<string, string> {
  const out: Record<string, string> = {}
  for (const [k, v] of Object.entries(env)) {
    if (v === null || typeof v !== "string") continue
    if (k === "__proto__" || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(k)) continue
    if (k === "GEBAI_APPROVAL_SKIP" && authMode === "server" && role !== "admin") continue
    out[k] = v
  }
  return out
}

export class EnvManager {
  constructor(
    private home: string,
    private store: SessionStore,
  ) {}

  private async readJson(p: string): Promise<Record<string, string>> {
    try {
      return JSON.parse(await readFile(p, "utf8")) as Record<string, string>
    } catch {
      return {}
    }
  }

  async getUserEnv(user: string): Promise<Record<string, string>> {
    return this.readJson(join(this.home, "users", user, "env.json"))
  }

  async resolve(sessionId: string, user: string): Promise<Record<string, string>> {
    const global: Record<string, string> = {}
    for (const [k, v] of Object.entries(process.env)) if (v !== undefined) global[k] = v
    const userEnv = await this.getUserEnv(user)
    const sessionEnv = await this.store.getEnv(sessionId, user)
    return { ...global, ...userEnv, ...sessionEnv }
  }

  async describe(sessionId: string, user: string): Promise<EnvVarSource[]> {
    // 一次性读取三层来源（避免循环内重复读盘）：进程环境（内存）、用户 env.json、会话 env.json
    const globalEnv = process.env
    const userEnv = await this.getUserEnv(user)
    const sessionEnv = await this.store.getEnv(sessionId, user)
    const names = new Set([...Object.keys(globalEnv), ...Object.keys(userEnv), ...Object.keys(sessionEnv)])
    const out: EnvVarSource[] = []
    for (const name of names) {
      const sensitive = isSensitive(name)
      if (sessionEnv[name] !== undefined) {
        out.push({ name, value: sensitive ? mask(sessionEnv[name]) : sessionEnv[name], source: "session", sensitive })
      } else if (userEnv[name] !== undefined) {
        out.push({ name, value: sensitive ? mask(userEnv[name]) : userEnv[name], source: "user", sensitive })
      } else if (globalEnv[name] !== undefined) {
        out.push({ name, value: sensitive ? mask(globalEnv[name]) : globalEnv[name], source: "global", sensitive })
      }
    }
    return out.sort((a, b) => a.name.localeCompare(b.name))
  }
}
