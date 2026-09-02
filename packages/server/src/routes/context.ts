/** 路由域文件共用上下文（routes/ 目录内部契约）：每个域文件导出 `register{Domain}Routes(rc)`，
 *  由 app.ts 统一装配——处理器体从原单文件 app.ts 原样拆分，行为不变。
 *  身份/权限助手由 app.ts 的 auth 中间件与 userOf/requireAdmin 提供（auth 域注释见 app.ts）。 */
import type { Context } from "hono"
import type { Hono } from "hono"
import type { AuthUser } from "../auth"
import type { AppDeps, AppEnv } from "../app"

export interface RouteCtx {
  app: Hono<AppEnv>
  d: AppDeps
  /** 当前请求用户（auth 中间件已解析放入 c.var；未认证为 SERVICE_USER 占位）。 */
  userOf: (c: Context) => AuthUser
  /** 多用户模式管理员校验：非管理员返回 403 Response（auth=none 不拦截）。 */
  requireAdmin: (c: Context) => Response | null
}
