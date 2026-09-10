/**
 * @gebai/sdk node 子入口（`@gebai/sdk/node`）：node 工具模块聚合——agent-utils（截断/路径工具）、
 * artifacts（内容块产物）、projects（预置项目解析）、walk（目录遍历）、paths（分片/沙箱路径）。
 *
 * 这些模块 import node:path/node:crypto/node:fs，浏览器构建不可解析，故独立于主入口
 * （DESIGN「SDK 双入口」）：web/客户端 SDK 只 import `@gebai/sdk`，server/agents 的 node
 * 侧值导入改走 `@gebai/sdk/node`。
 */
export * from "./agent-utils"
export * from "./artifacts"
export * from "./projects"
export * from "./walk"
export * from "./paths"
