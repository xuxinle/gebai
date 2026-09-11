/** 文件工作台路由共用件：请求级 RootContext 装配、错误映射、根/路径参数解析。
 *  root 解析与边界校验的语义全在 core/fs/roots.ts，本文件只做「HTTP 语义 ↔ 领域语义」的桥接。 */
import type { Context } from "hono"
import type { AppDeps } from "../app"
import type { AuthUser } from "../auth"
import { FsError, parseExtraRoots, localExtraRoots, type RootContext, type FileRoot } from "../core/fs/roots"
import { GitError } from "../core/git/service"

/** 文件工作台是否可用（GEBAI_FS_ENABLED）。 */
export function fsEnabled(d: AppDeps): boolean {
  return d.config.fsEnabled !== false
}

/** 请求携带的环境变量（浏览器本地 / 会话内存态）：JSON 字符串或对象，非法忽略。 */
export function parseEnvInput(raw: unknown): Record<string, string> {
  if (!raw) return {}
  let obj: unknown = raw
  if (typeof raw === "string") {
    const s = raw.trim()
    if (!s) return {}
    if (s.startsWith("{")) {
      try {
        obj = JSON.parse(s)
      } catch {
        return {}
      }
    } else return {}
  }
  if (!obj || typeof obj !== "object") return {}
  const out: Record<string, string> = {}
  for (const [k, v] of Object.entries(obj as Record<string, unknown>)) {
    if (typeof v === "string") out[k] = v
  }
  return out
}

/** 根清单缓存（2s）：避免前端每次刷新都全量扫会话目录。 */
const catalogCache = new Map<string, { ts: number; roots: FileRoot[] }>()

/**
 * 装配 RootContext：
 * - 项目注册表经 engine.workbenchProjects 解析（与会话 prompt 的 project 参数同源）；
 * - env 来源 = 会话内存态（?session=）∪ 请求携带（?env= / body.env）——浏览器本地 env 优先级更高；
 * - 沙箱（服务模式非豁免用户）下绝对路径根一律不可用，额外根限 GEBAI_FS_ROOTS 配置。
 */
export async function buildRootContext(
  d: AppDeps,
  user: AuthUser,
  opts: { sessionId?: string; envInput?: unknown; withSessions?: boolean } = {},
): Promise<RootContext> {
  const sandboxed = d.sandbox.enforcedFor(user.id)
  const sessionEnv = opts.sessionId ? await d.store.getEnv(opts.sessionId, user.id).catch(() => ({})) : {}
  const env = { ...sessionEnv, ...parseEnvInput(opts.envInput) }
  const { projects, binds } = d.engine.workbenchProjects(user.id, env)
  const extraRoots: FileRoot[] = [...parseExtraRoots(d.config.fsRoots, sandboxed)]
  if (!sandboxed) extraRoots.push(...localExtraRoots())
  const ctx: RootContext = {
    home: d.config.gebaiHome,
    user: user.id,
    sandboxed,
    writable: d.config.fsWrite !== false,
    projects,
    binds,
    extraRoots,
  }
  if (opts.withSessions) {
    const key = user.id
    const hit = catalogCache.get(key)
    if (hit && Date.now() - hit.ts < 2000) {
      ctx.sessions = hit.roots.map((r) => ({ id: r.sessionId ?? "", name: r.name })).filter((s) => s.id)
    } else {
      const sessions = await d.store.listSessionInfos(user.id).catch(() => [])
      const sorted = sessions.sort((a, b) => (b.updatedAt ?? 0) - (a.updatedAt ?? 0)).slice(0, 20)
      ctx.sessions = sorted.map((s) => ({ id: s.id, name: s.name || s.id.slice(0, 8) }))
      catalogCache.set(key, { ts: Date.now(), roots: ctx.sessions.map((s) => ({ id: `sess:${s.id}`, kind: "sess" as const, name: s.name, path: "", writable: ctx.writable, sessionId: s.id })) })
    }
  }
  return ctx
}

/** 统一错误响应（FsError / GitError → 状态码 + 可读消息；其余 500）。 */
export function errorResponse(c: Context, err: unknown): Response {
  if (err instanceof FsError) return c.json({ error: err.message, code: err.status }, err.status as 400)
  if (err instanceof GitError) return c.json({ error: err.message, code: err.status }, err.status as 400)
  const msg = err instanceof Error ? err.message : String(err)
  // 沙箱/路径类错误的原生消息（resolveInSandbox 抛的 Error）统一 403，避免 500 噪声
  if (/path traversal|outside sandbox|symlink outside|absolute path not allowed|路径穿越|路径越界/.test(msg)) {
    return c.json({ error: msg, code: 403 }, 403)
  }
  return c.json({ error: msg, code: 500 }, 500)
}

/** 请求参数读取（query 优先，回退 body）。 */
export function pickParam(c: Context, key: string, body?: Record<string, unknown>): string {
  const q = c.req.query(key)
  if (q !== undefined && q !== "") return q
  const b = body?.[key]
  return b === undefined || b === null ? "" : String(b)
}

/** 布尔参数（query `1`/`true`）。 */
export function pickBool(c: Context, key: string, body?: Record<string, unknown>): boolean {
  const q = c.req.query(key)
  if (q !== undefined && q !== "") return q === "1" || q === "true"
  const b = body?.[key]
  return b === true || b === 1 || b === "1" || b === "true"
}

/** 文件工作台未启用时的短路响应（统一 404 语义，不泄露能力存在性）。 */
export function requireFsEnabled(c: Context, d: AppDeps): Response | null {
  if (fsEnabled(d)) return null
  return c.json({ error: "文件工作台未启用（GEBAI_FS_ENABLED=false）" }, 404)
}

/** Git 服务未注入时的短路响应。 */
export function requireGit(c: Context, d: AppDeps): Response | null {
  if (d.git) return null
  return c.json({ error: "Git 能力未启用" }, 503)
}
