import { readdir, unlink } from "node:fs/promises"
import { join } from "node:path"
import type { SessionStore } from "./store"
import type { EnvVarSource } from "@gebai/sdk"

/** 敏感判定按「完整结尾单词」匹配（`(^|_)WORD$`），同时覆盖后缀与前缀/裸名形态：
 *  如 GEBAI_ADMIN_PASSWORD_HASH（_HASH 结尾，管理员口令哈希）、AWS_ACCESS_KEY_ID（_KEY_ID）、
 *  TOKEN/SECRET/PASSWORD 等裸名与 SECRET_* 前缀式命名均不再漏判。 */
const SENSITIVE_RE =
  /(^|_)(PASSWORD|PASSWD|PASSPHRASE|SECRET|SECRETS|TOKEN|TOKENS|KEY|KEYS|KEY_ID|CREDENTIAL|CREDENTIALS|CRED|AUTH|PAT|HASH|APIKEY|DSN|CONNECTION_STRING|DATABASE_URL)$/i

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
 * 浏览器本地 env 注入过滤（宽容模式，prompt 临时注入通道专用）：
 * 丢弃不支持/非法的变量、保留其余继续执行——前端 localStorage 可能残留旧版目录外键
 * （如 ask 填值分支存过的目录外变量），拒绝整个任务会阻断正常使用；
 * 显式管理通道（session.env.set 等）走严格校验（validateEnvVars）。
 * 过滤规则：非 string 值/null、非法标识符名（含 __proto__）。
 */
export function filterEnvInjection(env: Record<string, string | null>): Record<string, string> {
  const out: Record<string, string> = {}
  for (const [k, v] of Object.entries(env)) {
    if (v === null || typeof v !== "string") continue
    if (k === "__proto__" || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(k)) continue
    out[k] = v
  }
  return out
}

/** 一次性清理历史用户级 env.json（users/{user}/env.json，旧版三层结构遗留）：用户环境变量现仅存
 *  浏览器本地（服务端零留存），启动时移除存量文件；失败静默（不存在/权限异常不阻塞启动）。 */
export async function cleanupLegacyUserEnv(home: string): Promise<void> {
  let users
  try {
    users = await readdir(join(home, "users"), { withFileTypes: true })
  } catch {
    return
  }
  for (const u of users) {
    if (!u.isDirectory()) continue
    try {
      await unlink(join(home, "users", u.name, "env.json"))
    } catch {
      /* 不存在（常态） */
    }
  }
}

export class EnvManager {
  constructor(private store: SessionStore) {}

  /** 任务 env 解析：进程全局 env（.env/系统注入）+ 会话内存态 env（运行中经 env 接口/飞书命令设置，不落盘）。
   *  用户环境变量只存浏览器本地（localStorage），随 prompt 注入为任务级 envOverride（覆盖以上两层）。 */
  async resolve(sessionId: string, user: string): Promise<Record<string, string>> {
    const global: Record<string, string> = {}
    for (const [k, v] of Object.entries(process.env)) if (v !== undefined) global[k] = v
    const sessionEnv = await this.store.getEnv(sessionId, user)
    return { ...global, ...sessionEnv }
  }

  async describe(sessionId: string, user: string): Promise<EnvVarSource[]> {
    // 来源两层：进程环境（global，含 .env）+ 会话内存态（session，本进程生命周期内设置，重启即空）
    const globalEnv = process.env
    const sessionEnv = await this.store.getEnv(sessionId, user)
    const names = new Set([...Object.keys(globalEnv), ...Object.keys(sessionEnv)])
    const out: EnvVarSource[] = []
    for (const name of names) {
      const sensitive = isSensitive(name)
      if (sessionEnv[name] !== undefined) {
        out.push({ name, value: sensitive ? mask(sessionEnv[name]) : sessionEnv[name], source: "session", sensitive })
      } else if (globalEnv[name] !== undefined) {
        out.push({ name, value: sensitive ? mask(globalEnv[name]) : globalEnv[name], source: "global", sensitive })
      }
    }
    return out.sort((a, b) => a.name.localeCompare(b.name))
  }
}
