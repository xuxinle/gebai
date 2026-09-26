/**
 * core/triage 入口：参数契约（params）+ 管线实现（pipeline）+ 契约（types）一并导出，
 * 供子Agent 工具（会话内）与 server REST 接口共用同一份编排与同一套参数。
 */
export * from "./types"
export * from "./pipeline"
export * from "./params"
