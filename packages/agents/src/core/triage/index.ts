/**
 * core/triage 入口：管线实现（pipeline）+ 契约（types）一并导出，
 * 供子Agent 工具（会话内）与 server REST 接口共用同一份编排逻辑。
 */
export * from "./types"
export * from "./pipeline"
