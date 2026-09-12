/**
 * 最小日志器（`GEBAI_LOG_LEVEL` 的实际消费点）：按级别过滤输出，不引入依赖、不做结构化日志。
 *
 * 设计取舍：
 * - **模块级单例级别**：`loadConfig` 之后由 boot 装配处 `setLogLevel(config.logLevel)` 一次设定；
 *   各模块直接 `import { log } from "@gebai/sdk/node"` 即可，无需把级别逐层注入（core/ 内部模块
 *   多数拿不到 config）。
 * - **只做级别过滤**：输出去向仍是 `console.log/info/debug → stdout`、`console.warn/error → stderr`，
 *   保持既有排障习惯（`server.log.*` 重定向、`[gebai] listening` 等宿主解析均不受影响）。
 * - **协议性输出不走本器**：宿主/工具据以解析的行（如启动就绪行 `[gebai] listening on http://…`、
 *   拉起器写日志、`dev-reload` 子进程 stderr 转发）直接 `console.*`——它们不是「可调日志级别」的东西，
 *   被级别静默会让外部依赖方失效（桌面启动器靠该行取端口）。
 *
 * 级别：`debug` < `info`（默认）< `warn` < `error`。
 */

export type LogLevel = "debug" | "info" | "warn" | "error"

const ORDER: Record<LogLevel, number> = { debug: 0, info: 1, warn: 2, error: 3 }

let current: LogLevel = "info"

/** 设定全局日志级别（非法/缺省值忽略，保持当前）。返回生效级别。 */
export function setLogLevel(level: string | undefined | null): LogLevel {
  const v = String(level ?? "").trim().toLowerCase()
  if (v === "debug" || v === "info" || v === "warn" || v === "error") current = v
  return current
}

/** 当前生效级别（测试与诊断用）。 */
export function getLogLevel(): LogLevel {
  return current
}

/** 该级别在当前设置下是否会被输出（避免昂贵的日志参数构造）。 */
export function logEnabled(level: LogLevel): boolean {
  return ORDER[level] >= ORDER[current]
}

function emit(level: LogLevel, args: unknown[]): void {
  if (!logEnabled(level)) return
  if (level === "error") console.error(...args)
  else if (level === "warn") console.warn(...args)
  else console.log(...args)
}

/** 级别化日志（`log.info(...)` / `log.warn(...)` …）；调用点自带 `[tag]` 前缀，本器不加。 */
export const log = {
  debug: (...args: unknown[]): void => emit("debug", args),
  info: (...args: unknown[]): void => emit("info", args),
  warn: (...args: unknown[]): void => emit("warn", args),
  error: (...args: unknown[]): void => emit("error", args),
}
