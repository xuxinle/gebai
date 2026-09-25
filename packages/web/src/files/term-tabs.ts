/**
 * 文件工作台 · 终端标签（纯函数，带单测）。
 *
 * 标签名与「关掉它会不会杀掉正在跑的东西」是两件小事，但都属于**看着不起眼、错了很难受**的那类：
 * 名字不跟随 shell（VSCode 用 OSC 标题/进程名）时，三个 Bash 标签长得一模一样，关哪个全凭猜；
 * 关闭不做任何判断时，一条 `npm run build` 会随着误点的 × 无声消失。
 */

/** 忙闲判定窗口：这段时间内有输出即认为「有东西在跑」（见 `shouldConfirmClose`）。 */
export const BUSY_WINDOW_MS = 1500

export interface TermTabMeta {
  /** shell 展示名（Bash / PowerShell…），永不为空——它是最后的兜底。 */
  shellName: string
  /** shell 经 OSC 0/2 上报的标题（可为空）。 */
  oscTitle?: string
  /** 用户重命名（非空即最高优先）。 */
  custom?: string
}

/**
 * 标题清洗：OSC 标题里可能带控制字符与超长串（某些程序把整个命令行当标题），
 * 直接进标签会撑破标签栏或带进不可见字符。
 */
export function cleanTitle(raw: string | undefined): string {
  if (!raw) return ""
  // eslint-disable-next-line no-control-regex
  const cleaned = raw.replace(/[\u0000-\u001f\u007f]/g, "").trim()
  return cleaned.slice(0, 60)
}

/** 标签名：用户重命名 > shell 标题 > shell 名。 */
export function resolveTabLabel(meta: TermTabMeta): string {
  const custom = cleanTitle(meta.custom)
  if (custom) return custom
  const osc = cleanTitle(meta.oscTitle)
  if (osc) return osc
  return meta.shellName || "终端"
}

/** 标签 tooltip：名字之外补齐「哪个 shell、在哪、是否还在跑」这三件事。 */
export function tabTooltip(opts: { label: string; shellName: string; cwd: string; alive: boolean; exitCode?: number | null }): string {
  const where = opts.cwd || "会话根目录"
  const state = opts.alive ? "" : opts.exitCode === undefined || opts.exitCode === null || opts.exitCode === 0 ? " · 已退出" : ` · 已退出（退出码 ${opts.exitCode}）`
  return `${opts.label}（${opts.shellName}）· ${where}${state}`
}

/**
 * 关标签是否先确认：进程还活着、且最近仍有输出（近似「命令正在跑」）。
 *
 * 为什么用近似：精确判定要看 PTY 里的**前台进程组**，那需要驱动侧上报（见 DESIGN 已知边界）。
 * 近似只在一侧保守——正在跑的命令总会有输出（进度、日志），静默的长时间命令（`sleep`）会漏，
 * 但那也不会「看起来像空闲」，最多是少一次确认；反过来「刚敲完命令就关」不会误弹，避免噪声。
 */
export function shouldConfirmClose(opts: { alive: boolean; lastOutputAt: number; now: number }): boolean {
  if (!opts.alive) return false
  if (!opts.lastOutputAt) return false
  return opts.now - opts.lastOutputAt <= BUSY_WINDOW_MS
}
