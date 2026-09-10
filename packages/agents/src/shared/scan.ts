/**
 * 子Agent 目录扫描的排除清单（dev 发现与 bundle 构建共享的唯一事实源）：
 * - NON_AGENT_DIRS：agents 包内基建目录（非子Agent 定义）
 * - NON_AGENT_FILES：agents 包 src/ 顶层的非子Agent 文件（包入口/类型声明）
 * 新增基建目录/入口文件时改这里，dev 扫描（subagents.ts）与构建（build-subagents.ts）同步生效。
 */
export const NON_AGENT_DIRS: ReadonlySet<string> = new Set(["analyzer", "browser", "cv", "shared", "widgets-store"])
export const NON_AGENT_FILES: ReadonlySet<string> = new Set(["index.ts", "types-md.d.ts"])
