import { homedir } from "node:os"
import { join } from "node:path"
import { existsSync, readFileSync } from "node:fs"

export type AuthMode = "local" | "server"
export type SandboxMode = "auto" | "on" | "off"

/**
 * In script-debug mode, Bun auto-loads `.env` from the process cwd, which depends
 * on how the server was launched. Load the repo-root `.env` explicitly so LLM /
 * server config works regardless of cwd. Binary mode (incl. both desktop hosts)
 * loads `{GEBAI_HOME}/.env` instead, so deployed binaries stay configurable via
 * a file next to user data. Values already present in the real env always win.
 */
function loadDotEnv() {
  const base = isBinaryMode()
    ? resolveGebaiHome()
    : join(import.meta.dirname, "..", "..", "..", "..")
  const p = join(base, ".env")
  try {
    const raw = readFileSync(p, "utf8")
    for (const line of raw.split("\n")) {
      const m = line.match(/^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$/)
      if (m && process.env[m[1]] === undefined) {
        process.env[m[1]] = m[2].replace(/^["']|["']$/g, "")
      }
    }
  } catch {
    /* no .env; fine */
  }
}

export interface ServerConfig {
  host: string
  port: number
  /** 运行形态：`local`（本地模式，默认，直接使用 admin 用户免登录）/ `server`（服务模式，账号密码登录）。 */
  auth: AuthMode
  corsOrigins: string[]
  basePath: string
  trustProxy: boolean
  sandbox: SandboxMode
  preloadSubAgents: string[]
  uiStyle: string
  logLevel: "debug" | "info" | "warn" | "error"
  toolEnable?: string[]
  toolDisable?: string[]
  selfModify: boolean
  gebaiHome: string
  binaryMode: boolean
  webDist: string
  /** 服务模式 admin 用户密码哈希（GEBAI_ADMIN_PASSWORD_HASH，格式 `salt:hash` 均为 hex，与注册表 scrypt 哈希一致）；
   *  设置则启用 admin（覆盖其哈希），不设置则禁用 admin 用户。 */
  adminPasswordHash?: string
  /** 注册审批模式（GEBAI_SIGNUP_MODE）：open=注册即用（默认）/ approval=注册待 admin 审批（disabled+pending，批准后启用）。 */
  signupMode: "open" | "approval"
  /** 关闭数据生命周期 GC 清理任务（默认开启）。 */
  gcDisabled: boolean
  /** 是否启用定时任务能力（GEBAI_CRON_ENABLED，默认 true：注册 cron 子Agent（cron_* 工具）并启动调度器；
   *  显式 false 时完全不可见）。 */
  cronEnabled: boolean
  /** 外部身份扩展点：HMAC 共享密钥（GEBAI_EXTERNAL_AUTH_SECRET，与 GEBAI_EXTERNAL_AUTH_URL 互斥）。 */
  externalAuthSecret?: string
  /** 外部身份扩展点：HTTP 回调验证 URL（GEBAI_EXTERNAL_AUTH_URL，与 GEBAI_EXTERNAL_AUTH_SECRET 互斥）。 */
  externalAuthUrl?: string
  /** 外部身份扩展点：外部用户名不存在时自动创建 GEBAI 用户（默认 true；false 时仅允许已存在用户）。 */
  externalAuthAutocreate: boolean
  /** 外部身份扩展点：Web UI 同源直读宿主 localStorage 的凭证 key（可选，不设则仅支持 URL 参数注入）。 */
  externalAuthStorageKey?: string
  /** 开发模式热刷新（bun run dev --reload 或 GEBAI_DEV_RELOAD=1）：Web 源码变更自动重建并广播页面刷新。 */
  devReload: boolean
  /** 飞书机器人对话桥接（GEBAI_FEISHU_BOT_ENABLED=true 时启用长连接事件订阅）。 */
  feishuBotEnabled: boolean
  /** 飞书应用 App ID（GEBAI_FEISHU_APP_ID；feishu_docs 子 Agent 与机器人桥接共用）。 */
  feishuAppId?: string
  /** 飞书应用 App Secret（GEBAI_FEISHU_APP_SECRET）。 */
  feishuAppSecret?: string
  /** 安全模式（GEBAI_SAFE_MODE=true 启动时加载，不可在会话/任务级修改）：有风险的工具（命令执行/写删文件等）
   *  被阻止执行，直接返回限制信息给模型。 */
  safeMode: boolean
}

function env(name: string, fallback = ""): string {
  return process.env[name] ?? fallback
}

function bool(name: string, fallback = false): boolean {
  const v = process.env[name]
  if (v === undefined) return fallback
  return v === "true" || v === "1"
}

export function isBinaryMode(): boolean {
  return !existsSync(join(import.meta.dirname, "..", "..", "package.json"))
}

export function resolveGebaiHome(): string {
  if (process.env.GEBAI_HOME) return process.env.GEBAI_HOME
  if (isBinaryMode()) return join(homedir(), ".gebai")
  return join(import.meta.dirname, "..", "..", "..", "..")
}

function splitList(v: string | undefined): string[] {
  if (!v) return []
  return v
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean)
}

/**
 * 运行形态解析（默认本地模式）：
 * 1. CLI 参数 `--server` 开启服务模式（最高优先，便于二进制直接传参）
 * 2. `GEBAI_MODE=server|local` 环境变量
 * 3. 兼容旧 `GEBAI_AUTH`：`none` → local、`multi` → server
 */
function resolveAuthMode(): AuthMode {
  if (process.argv.includes("--server")) return "server"
  const mode = process.env.GEBAI_MODE || process.env.GEBAI_AUTH || "local"
  if (mode === "server" || mode === "multi") return "server"
  return "local"
}

export function loadConfig(overrides: Partial<ServerConfig> = {}): ServerConfig {
  loadDotEnv()
  const defaultWebDist = join(import.meta.dirname, "..", "..", "..", "web", "dist")
  const config: ServerConfig = {
    host: env("GEBAI_HOST", "127.0.0.1"),
    port: Number(env("GEBAI_PORT", "3000")),
    auth: resolveAuthMode(),
    corsOrigins: splitList(env("GEBAI_CORS_ORIGINS", "*")),
    basePath: env("GEBAI_BASE_PATH", "/").replace(/\/+$/, "") || "/",
    trustProxy: bool("GEBAI_TRUST_PROXY", false),
    sandbox: env("GEBAI_SANDBOX", "auto") as SandboxMode,
    preloadSubAgents: splitList(env("GEBAI_PRELOAD_SUB_AGENTS")),
    uiStyle: env("GEBAI_UI_STYLE", "acrylic"),
    logLevel: env("GEBAI_LOG_LEVEL", "info") as ServerConfig["logLevel"],
    toolEnable: splitList(env("GEBAI_TOOL_ENABLE")),
    toolDisable: splitList(env("GEBAI_TOOL_DISABLE")),
    selfModify: bool("GEBAI_SELF_MODIFY", false),
    gebaiHome: resolveGebaiHome(),
    binaryMode: isBinaryMode(),
    webDist: defaultWebDist,
    adminPasswordHash: env("GEBAI_ADMIN_PASSWORD_HASH") || undefined,
    signupMode: env("GEBAI_SIGNUP_MODE") === "approval" ? "approval" : "open",
    gcDisabled: bool("GEBAI_GC_DISABLED", false),
    cronEnabled: bool("GEBAI_CRON_ENABLED", true),
    externalAuthSecret: env("GEBAI_EXTERNAL_AUTH_SECRET") || undefined,
    externalAuthUrl: env("GEBAI_EXTERNAL_AUTH_URL") || undefined,
    externalAuthAutocreate: bool("GEBAI_EXTERNAL_AUTH_AUTOCREATE", true),
    externalAuthStorageKey: env("GEBAI_EXTERNAL_AUTH_STORAGE_KEY") || undefined,
    // 开发模式热刷新：bun run dev --reload 或 GEBAI_DEV_RELOAD=1
    devReload: process.argv.includes("--reload") || env("GEBAI_DEV_RELOAD") === "1",
    // 飞书机器人对话桥接（长连接模式）：需同时配置 GEBAI_FEISHU_APP_ID / GEBAI_FEISHU_APP_SECRET
    feishuBotEnabled: bool("GEBAI_FEISHU_BOT_ENABLED", false),
    feishuAppId: env("GEBAI_FEISHU_APP_ID") || undefined,
    feishuAppSecret: env("GEBAI_FEISHU_APP_SECRET") || undefined,
    // 安全模式：仅启动时从环境变量加载，不进入会话 env（不可被 ask 填值分支/前端 envOverride 修改）
    safeMode: bool("GEBAI_SAFE_MODE", false),
  }
  return { ...config, ...overrides }
}
