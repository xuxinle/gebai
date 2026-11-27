/**
 * 托管应用的后端模块（应用目录里的 `server.js` 一类）：加载、缓存、调用与失败隔离。
 *
 * ## 形态（三种导出，任选其一）
 *
 * ```js
 * // ① 单函数（最自由）：拿到请求上下文，返回 Response 或任意可 JSON 化的值
 * export default async function (ctx) {
 *   const n = await ctx.json()
 *   return { ok: true, echo: n }
 * }
 *
 * // ② 命名 fetch（与 ① 等价，语义更明确）
 * export async function fetch(ctx) { ... }
 *
 * // ③ 路由表（最省事）：键是「方法 + 应用内路径」，值为处理函数
 * export const routes = {
 *   "GET /health": () => ({ ok: true }),
 *   "POST /echo": async (ctx) => ctx.json(),
 * }
 * ```
 *
 * `ctx` 是**只读能力面**（`AppBackendContext`）：请求信息 + 响应助手 + 清单 env + 最小身份。
 * 它**不含**引擎、工具注册表、会话存储——后端模块是部署方自己的代码（与 `custom/` 同级的二开面），
 * 能力边界靠「不给」而不是靠「拦住」：给引擎等于把智能体的手交给一个 HTTP 处理器。
 *
 * ## 权力内守（为什么默认关着）
 *
 * 后端模块在本进程内执行，拥有 node/Bun 全部能力——它等价于部署方在服务器上写代码。
 * 因此：
 * - **服务模式（多用户部署）默认不加载**（`GEBAI_APPS_BACKEND=auto` 时判定为 off）：
 *   多用户共享的进程里跑第三方应用后端，等于让一个应用的用户影响全部用户；
 * - **安全模式（`GEBAI_SAFE_MODE=true`）强制不加载**（安全模式的语义就是「不执行部署方代码」）；
 * - 需要时显式 `GEBAI_APPS_BACKEND=on`（部署方整体担责，与 `GEBAI_FEISHU_BOT_AUTO_APPROVE` 同类）。
 *
 * ## 热重载
 *
 * 按 `mtime` 作 import 版本号：改文件→下一次请求即用新代码（旧模块实例被 Bun 缓存但不再被选中）。
 * 与子 Agent 的「仅入口热重载」同理，是有意为之的取舍：应用后端通常单文件。
 */
import { statSync } from "node:fs"
import { createRequire } from "node:module"
import { pathToFileURL } from "node:url"
import { join } from "node:path"
import type { AppBackendContext, AppBackendHandler, AppBackendResult } from "@gebai/sdk"
import type { AppEntry } from "./sources"

// 后端契约（ctx/返回值/处理函数签名）的**单一真相在 `@gebai/sdk` 的 `hosted-app.ts`**：
// 写应用的人看到的就是这里实际调用的。本模块只做加载与调用。
export type { AppBackendContext, AppBackendHandler, AppBackendResult }

/**
 * 后端模块热重载的关键细节：**清模块缓存**。
 *
 * 实测（Bun 1.4）：只给 import URL 加 `?v=<mtime>` **不够**——Bun 的 ESM 缓存归一化到文件路径，
 * 同一文件的不同 query 仍返回旧实例（改完 server.js 后接口持续返回旧结果，看上去像“热重载没实现”）。
 * 而 `createRequire(...).cache` 与 ESM 缓存是同一张表：删掉该路径的条目后，下一次 import 真的重读文件。
 * 所以做法是「删缓存 + query 版本号」两道一起上（后者保证与外部持有的旧 URL 不串）。
 */
function purgeModuleCache(abs: string): void {
  try {
    const req = createRequire(join(abs, "..", "__gebai_apps__.js"))
    delete req.cache[abs]
    delete req.cache[pathToFileURL(abs).href]
  } catch {
    /* 清不掉不阻断：只是这一次可能拿到旧版本，下一次 mtime 再变时还会重试 */
  }
}

/** 已加载的后端模块（按 mtime 失效）。 */
interface LoadedBackend {
  mtime: number
  handler: AppBackendHandler
}

/** 后端调用默认超时（毫秒）：处理器挂死不该把服务端连接一直占着。可用清单 env `GEBAI_APP_TIMEOUT_MS` 覆盖。 */
const DEFAULT_TIMEOUT_MS = 15_000

/** 加载失败/调用失败的统一错误（路由层据此回 500 并附原因）。 */
export class AppBackendError extends Error {
  constructor(message: string, readonly stage: "load" | "run" | "timeout") {
    super(message)
  }
}

/** 路由表匹配：返回命中处理函数（未命中返回 null）。“未命中”与“命中但自己返回 null”是两件事，
 *  所以这里只找不叫，由调用方决定未命中时落到 fallback。 */
function matchRoute(routes: Record<string, unknown>, ctx: AppBackendContext): AppBackendHandler | null {
  const p = `/${ctx.path}`
  // 精确键优先（同时容忍末尾斜杠与“任意方法”写法）
  for (const key of [`${ctx.method} ${p}`, `${ctx.method} ${p}/`, `* ${p}`, "ANY " + p] as const) {
    const fn = routes[key]
    if (typeof fn === "function") return fn as AppBackendHandler
  }
  // 前缀通配（`"GET /api/*"`）
  for (const [key, fn] of Object.entries(routes)) {
    if (typeof fn !== "function") continue
    const sp = key.indexOf(" ")
    if (sp < 0) continue
    const method = key.slice(0, sp).toUpperCase()
    const path = key.slice(sp + 1)
    if (method !== "*" && method !== "ANY" && method !== ctx.method) continue
    if (path.endsWith("/*") && p.startsWith(path.slice(0, -1))) return fn as AppBackendHandler
  }
  return null
}

/**
 * 把模块导出的各种形态收敛成单一 handler。
 *
 * 优先级与叠加语义：**路由表 > fetch > default，且 default 兼任「路由表未命中」的兜底**——
 * 这条规则让「用路由表写常用的几个端点 + 用 default 写统一 404/统一鉴权」这个自然写法真的生效。
 * 若把 default 当成独立形态优先于 routes，示例里那种“routes + default 兜底”的模块就会
 * 静默变成“只有 default 生效”（实测过：页面一切正常但接口全 404）。
 */
function toHandler(mod: unknown, where: string): AppBackendHandler {
  const m = (mod ?? {}) as Record<string, unknown>
  const dflt = m.default
  const fallbackFn: AppBackendHandler | null =
    typeof dflt === "function"
      ? (dflt as AppBackendHandler)
      : typeof m.fetch === "function"
        ? (m.fetch as AppBackendHandler)
        : dflt && typeof dflt === "object"
          ? toHandlerIfPossible(dflt)
          : null
  const routes = m.routes && typeof m.routes === "object" ? (m.routes as Record<string, unknown>) : null

  if (routes) {
    return (ctx) => {
      const hit = matchRoute(routes, ctx)
      if (hit) return hit(ctx)
      if (fallbackFn) return fallbackFn(ctx)
      return new Response(JSON.stringify({ error: `应用未定义路由：${ctx.method} /${ctx.path}` }), {
        status: 404,
        headers: { "Content-Type": "application/json; charset=utf-8" },
      })
    }
  }
  if (fallbackFn) return fallbackFn
  throw new AppBackendError(
    `${where} 未导出可用的后端形态：需要 default 函数 / fetch 函数 / routes 对象（见 DESIGN「托管应用」）`,
    "load",
  )
}

/** 对象形态的默认导出是否像模块（含 default/fetch/routes）——CJS 互操作处理。 */
function toHandlerIfPossible(obj: unknown): AppBackendHandler | null {
  const o = obj as Record<string, unknown>
  if (typeof o.fetch === "function") return o.fetch as AppBackendHandler
  if (o.routes && typeof o.routes === "object") return toHandler({ ...o }, "default 对象")
  if (typeof o.default === "function") return o.default as AppBackendHandler
  return null
}

/** 后端加载器（进程级单例语义：按应用 id + 文件 mtime 缓存 handler）。 */
export class AppBackendLoader {
  private readonly loaded = new Map<string, LoadedBackend>()

  constructor(private readonly opts: { enabled: boolean; reason?: string } = { enabled: false }) {}

  /** 是否允许加载（配置判定结果 + 原因，REST/索引页如实展示）。 */
  get enabled(): boolean {
    return this.opts.enabled
  }

  get disabledReason(): string {
    return this.opts.reason ?? "托管应用后端未启用（GEBAI_APPS_BACKEND）"
  }

  /** 应用能否真的跑后端：开关打开 + 清单声明 + 有内联函数或有可落地的模块目录。 */
  canRun(entry: AppEntry): boolean {
    return this.opts.enabled && entry.hasBackend
  }

  /**
   * 取后端 handler。三种来源依次：
   * - **源码内联函数**（entry.inlineBackend）：直接用，不需要文件、不需要 import；
   * - **磁盘层模块**：按 mtime 热重载 import；
   * - **内嵌层模块**（二进制形态）：文件先从产物落地到缓存目录，再同上 import；
   * - 都不行：抛 AppBackendError（stage=load）——由路由层转 503/500 并附原因，
   *   **不静默降级到静态**（那会让「后端没生效」看起来像「接口 404」）。
   */
  async handler(entry: AppEntry): Promise<AppBackendHandler> {
    if (!this.opts.enabled) throw new AppBackendError(this.disabledReason, "load")
    if (entry.inlineBackend) return entry.inlineBackend
    if (!entry.manifest.backend) throw new AppBackendError(`应用「${entry.manifest.id}」未声明后端模块`, "load")
    const dir = await entry.backendDir()
    if (!dir) throw new AppBackendError(`应用「${entry.manifest.id}」的后端模块无处落地（既无磁盘目录，也不在内嵌产物里）`, "load")
    const abs = join(dir, entry.manifest.backend)
    let mtime: number
    try {
      mtime = statSync(abs).mtimeMs
    } catch {
      throw new AppBackendError(`后端模块不存在：${entry.manifest.backend}`, "load")
    }
    const cacheKey = `${entry.manifest.id}:${abs}`
    const hit = this.loaded.get(cacheKey)
    if (hit && hit.mtime === mtime) return hit.handler
    let mod: unknown
    try {
      // 删模块缓存 + query 版本号（Bun 的 ESM 缓存按文件路径归一化，单靠 query 不生效——见 purgeModuleCache）
      purgeModuleCache(abs)
      mod = await import(`${pathToFileURL(abs).href}?v=${Math.round(mtime)}`)
    } catch (err) {
      throw new AppBackendError(`后端模块加载失败：${String((err as Error)?.message || err)}`, "load")
    }
    const handler = toHandler(mod, entry.manifest.backend)
    this.loaded.set(cacheKey, { mtime, handler })
    return handler
  }

  /** 调用（含超时保护）。返回 Response 由调用方直接发出。 */
  async invoke(entry: AppEntry, ctx: AppBackendContext): Promise<Response | unknown> {
    const handler = await this.handler(entry)
    const timeoutMs = Number(entry.manifest.env.GEBAI_APP_TIMEOUT_MS) > 0 ? Number(entry.manifest.env.GEBAI_APP_TIMEOUT_MS) : DEFAULT_TIMEOUT_MS
    let timer: ReturnType<typeof setTimeout> | undefined
    try {
      const result = await Promise.race([
        Promise.resolve(handler(ctx)),
        new Promise<never>((_, reject) => {
          timer = setTimeout(() => reject(new AppBackendError(`后端处理超时（${timeoutMs}ms）`, "timeout")), timeoutMs)
        }),
      ])
      return result
    } catch (err) {
      if (err instanceof AppBackendError) throw err
      throw new AppBackendError(`后端处理异常：${String((err as Error)?.message || err)}`, "run")
    } finally {
      if (timer) clearTimeout(timer)
    }
  }
}

/**
 * 测试与内联应用共用的 ctx 构造（仅测试使用；生产路径的 ctx 由 routes/apps.ts 据真实请求构造）。
 * 导出而非内联在测试里，是因为「ctx 字段变了但测试没跟上」会让新建 态的断言悄悄失效。
 */
export function testContext(appId: string, method: string, path: string, extra: Partial<AppBackendContext> = {}): AppBackendContext {
  return {
    appId,
    method,
    path,
    query: {},
    header: () => undefined,
    text: async () => "",
    json: async () => null,
    user: { id: "admin", role: "admin" },
    env: {},
    respond: (data, init) => new Response(JSON.stringify(data), { status: init?.status ?? 200, headers: { "Content-Type": "application/json; charset=utf-8", ...(init?.headers ?? {}) } }),
    ...extra,
  }
}
