/**
 * 浏览器本地环境变量（localStorage）：对本浏览器所有会话可用，
 * 发送消息时随 prompt 请求临时注入服务端（仅本次任务生效，不持久化到服务端）。
 */
const LOCAL_ENV_KEY = "gebai.ui.env"

export function loadLocalEnv(): Record<string, string> {
  try {
    const raw = localStorage.getItem(LOCAL_ENV_KEY)
    if (!raw) return {}
    const parsed = JSON.parse(raw) as Record<string, string>
    return typeof parsed === "object" && parsed !== null && !Array.isArray(parsed) ? parsed : {}
  } catch {
    return {}
  }
}

export function saveLocalEnv(vars: Record<string, string>): void {
  try {
    localStorage.setItem(LOCAL_ENV_KEY, JSON.stringify(vars))
  } catch {
    /* 存储不可用（隐私模式/配额满）时静默忽略，与主题/审批跳过等模块一致 */
  }
}

/** 环境变量目录分组（与 SDK getEnvCatalog 返回结构一致）。 */
export interface EnvCatalogGroup {
  group: string
  label: string
  vars: Array<{ name: string; description: string }>
}

/**
 * 过滤到目录白名单：只保留目录内变量（丢弃自定义/目录外变量）。
 * 前端保存与注入前均经此过滤——环境变量配置只支持目录项，不可自定义变量名。
 */
export function filterEnvToCatalog(env: Record<string, string>, groups: EnvCatalogGroup[]): Record<string, string> {
  const allowed = new Set<string>()
  for (const g of groups) {
    for (const v of g.vars) allowed.add(v.name)
  }
  const out: Record<string, string> = {}
  for (const [k, v] of Object.entries(env)) {
    if (allowed.has(k)) out[k] = v
  }
  return out
}
