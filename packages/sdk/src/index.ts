/**
 * @gebai/sdk 主入口：**浏览器安全集**（types/task-types/agent-contract 契约与类型 + GebaiClient 客户端）。
 *
 * 入口拆分（DESIGN「SDK 双入口」）：node 内建依赖的工具模块（agent-utils/artifacts/projects/walk/paths，
 * import node:path/node:crypto 等）独立走 `@gebai/sdk/node` 子路径导出——本入口零 node 内建，
 * web 构建（vite treeshake:false）从根上不再解析 node 模块（"join" is not exported by
 * "__vite-browser-external" 崩溃根因）。新增浏览器安全模块挂这里，含 node 内建的挂 node.ts。
 */
export * from "./types"
export * from "./task-types"
export * from "./agent-contract"
export * from "./symbol-grammar"
export * from "./file-language"
export { GebaiClient } from "./client"
export type { GebaiClientOptions } from "./client"
export { appBase, appPath, appWsUrl, resolveAppBase } from "./app-base"
export { AUTH_TOKEN_KEY, readToken, writeToken, clearTokenState, requestHeaders } from "./auth-contract"
export type { AuthOverrides } from "./auth-contract"
