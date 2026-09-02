/** 脚本执行选项（自 core/tools.ts 抽取；sh/py/js 脚本工具共用）。 */

/** 脚本执行超时参数（秒）：默认 300（5 分钟，与引擎脚本超时一致），上限 540（即引擎 9 分钟工具兜底值，脚本级超时不会晚于引擎兜底触发）。 */
const SCRIPT_TIMEOUT_DEFAULT_S = 300
const SCRIPT_TIMEOUT_MAX_S = 540

/** 脚本超时参数解析（秒 → 毫秒）：非正数/非法回退默认值，超上限截断。（js 脚本工具复用，导出） */
export function scriptTimeoutMs(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n) || n <= 0) return SCRIPT_TIMEOUT_DEFAULT_S * 1000
  return Math.min(n, SCRIPT_TIMEOUT_MAX_S) * 1000
}
