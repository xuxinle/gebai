/** 脚本执行选项（自 core/tools.ts 抽取；sh/py/js 脚本工具共用）。 */

/** 脚本执行超时参数（秒）：默认 300（5 分钟，与引擎脚本超时一致），上限 540（即引擎 9 分钟工具兜底值，脚本级超时不会晚于引擎兜底触发）。 */
const SCRIPT_TIMEOUT_DEFAULT_S = 300
const SCRIPT_TIMEOUT_MAX_S = 540

/** 脚本超时参数解析（秒 → 毫秒）：非正数/非法回退默认值，超上限截断。（py/js 单次执行超时——超时杀进程，导出） */
export function scriptTimeoutMs(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n) || n <= 0) return SCRIPT_TIMEOUT_DEFAULT_S * 1000
  return Math.min(n, SCRIPT_TIMEOUT_MAX_S) * 1000
}

/** sh 同步等待窗口（秒）：默认 60、上限 120——窗口只决定「前台等多久」，到期命令转后台继续跑而非被杀
 *  （见 DESIGN「sh 执行」），取值与 py/js 的单次执行超时（杀进程上限）刻意分开。 */
const SH_WAIT_DEFAULT_S = 60
const SH_WAIT_MAX_S = 120

/** sh 同步等待窗口解析（秒 → 毫秒）：非正数/非法回退默认值，超上限截断；
 *  同步回退路径（安全模式 / 无后台任务服务 / 后台并发已满）下同值用作该次执行的杀进程上限。 */
export function shWaitMs(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n) || n <= 0) return SH_WAIT_DEFAULT_S * 1000
  return Math.min(n, SH_WAIT_MAX_S) * 1000
}
