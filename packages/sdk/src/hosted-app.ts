/**
 * 托管应用的**作者契约**（SDK 导出面 `@gebai/sdk` 的 `hosted-app.ts`）。
 *
 * ## 为什么要放进 SDK 而不是留在 server 内部
 *
 * 写应用的人（人或智能体）应当只依赖一份稳定契约，而不是 server 的内部路径——否则
 * `apps/foo.ts` 里出现 `import type { InlineApp } from "../../packages/server/src/core/apps/inline"`
 * 这种引进内核的写法：一是二进制形态无源码树时类型也不存在，二是内核与扩展的边界当场消失。
 * 这与子 Agent 的纪律完全同源（AGENTS.md：契约类型一律来自 `@gebai/sdk`）。
 *
 * ## 契约只描述「应用是什么样子」，不描述「宿主怎么实现」
 *
 * - `InlineApp`：源码内联应用的定义形态（文件内容 + 可选后端函数）；
 * - `AppBackendHandler` / `AppBackendContext`：后端处理函数的入参（只读能力面）；
 * - `AppManifest`：已归一的清单形态（REST 与索引页对外展示的就是它）。
 *
 * server 侧的 `core/apps/*` 直接复用本文件的类型（单一真相：作者看到的签名就是宿主实际调用的签名）。
 */

/** 应用 id 形态：既是 URL 段也是磁盘目录名（白名单防路径穿越与大小写歧义）。 */
export const HOSTED_APP_ID_RE = /^[a-z0-9][a-z0-9_-]{0,63}$/

/** 默认入口文件名（清单未声明 entry 时）。 */
export const HOSTED_APP_DEFAULT_ENTRY = "index.html"

/** 应用清单（宿主归一后的形态：所有字段都有确定值）。 */
export interface AppManifest {
  id: string
  title: string
  description: string
  version: string
  /** 入口文件（相对应用根，POSIX 分隔）。 */
  entry: string
  /** 前端路由回退：无扩展名的未知路径回落到 entry（单页应用必需）。 */
  spa: boolean
  /** 后端模块相对路径（空串 = 无后端）。 */
  backend: string
  /** 是否在索引页/REST 列表展示（false = 仍可直接访问）。 */
  listed: boolean
  /** 传给后端的只读环境变量（清单声明）。 */
  env: Record<string, string>
}

/** 后端处理的返回形态：`Response` 原样发出；其余可 JSON 化值按 JSON 响应。 */
export type AppBackendResult = Response | unknown

/** 后端处理函数拿到的只读上下文。 */
export interface AppBackendContext {
  /** 应用 id。 */
  appId: string
  /** HTTP 方法（大写）。 */
  method: string
  /** 应用内路径（相对应用根、已归一、无前导斜杠；路由表键按它匹配）。 */
  path: string
  /** 查询参数（同名多值取最后一个）。 */
  query: Record<string, string>
  /** 请求头读取（大小写不敏感）。 */
  header: (name: string) => string | undefined
  /** 请求体文本（按需读一次，内部缓存）。 */
  text: () => Promise<string>
  /** 请求体 JSON（空体返回 null；解析失败抛错 → 宿主转 400）。 */
  json: () => Promise<unknown>
  /** 调用方身份（最小面；未登录/公开端点时 id 为空串）。 */
  user: { id: string; role: string }
  /** 清单里声明的 env（只读副本）。 */
  env: Record<string, string>
  /** 响应助手（需要定制状态码/头时的出口）。 */
  respond: (data: unknown, init?: { status?: number; headers?: Record<string, string> }) => Response
}

/** 后端处理函数签名（三种导出形态统一收敛到它）。 */
export type AppBackendHandler = (ctx: AppBackendContext) => AppBackendResult | Promise<AppBackendResult>

/** 内联应用的文件内容：字符串按 UTF-8；字节数组原样。 */
export type InlineAppFile = string | Uint8Array

/**
 * 源码内联应用定义（`apps/*.ts` 的 `export const apps = [...]` 元素）。
 *
 * 三种导出形态（`export const apps` / `export default` / 单个对象）宿主都认；
 * `backend` 是**函数**——同进程直接调用，不需要 `server.js`、不需要动态 import。
 */
export interface InlineApp {
  id: string
  title?: string
  description?: string
  version?: string
  /** 入口（缺省 `index.html`）。 */
  entry?: string
  /** SPA 回退（缺省 true）。 */
  spa?: boolean
  /** 是否在索引页列出（缺省 true）。 */
  listed?: boolean
  /** 传给后端的只读 env。 */
  env?: Record<string, string>
  /** 文件表：相对路径（POSIX）→ 内容。 */
  files: Record<string, InlineAppFile>
  /** 后端处理函数（可选）。 */
  backend?: AppBackendHandler
}
