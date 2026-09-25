/**
 * 文件工作台 · 终端配色（纯函数，带单测）。
 *
 * 终端 16 色是**一张独立于界面主题的调色板**（VSCode 里它来自主题的 `terminal.ansi*` 色），
 * 界面主题只提供背景 / 前景 / 选区三色。两者混着算就会出「亮红与红取值相同」这种在 ANSI
 * 语义上说不通的表——`\e[31m` 与 `\e[91m` 本就是两档亮度，重复取值等于把 8 个颜色位浪费掉，
 * 也让彩色输出的语义（暗色=普通、亮色=强调）在界面上消失。
 * 故这里内置 VSCode 默认的暗 / 亮两套 ANSI 调色板，按**背景亮度**择一，界面只交背景与前景。
 *
 * 另附 VSCode 默认的**最小对比度**（4.5）：ANSI 前景与背景对比不足时由 xterm 向可读方向调整，
 * 保证「主题换了、终端文字仍然读得清」。
 */

/** VSCode 默认暗色终端调色板（Dark Modern 的 `terminal.ansi*`）。 */
const DARK_ANSI: Record<string, string> = {
  black: "#000000",
  red: "#cd3131",
  green: "#0dbc79",
  yellow: "#e5e510",
  blue: "#2472c8",
  magenta: "#bc3fbc",
  cyan: "#11a8cd",
  white: "#e5e5e5",
  brightBlack: "#666666",
  brightRed: "#f14c4c",
  brightGreen: "#23d18b",
  brightYellow: "#f5f543",
  brightBlue: "#3b8eea",
  brightMagenta: "#d670d6",
  brightCyan: "#29b8db",
  brightWhite: "#ffffff",
}

/** VSCode 默认亮色终端调色板（Light Modern 的 `terminal.ansi*`）。 */
const LIGHT_ANSI: Record<string, string> = {
  black: "#000000",
  red: "#cd3131",
  green: "#00bc00",
  yellow: "#949800",
  blue: "#0451a5",
  magenta: "#bc05bc",
  cyan: "#0598bc",
  white: "#555555",
  brightBlack: "#666666",
  brightRed: "#cd3131",
  brightGreen: "#14ce14",
  brightYellow: "#b5ba00",
  brightBlue: "#0451a5",
  brightMagenta: "#bc05bc",
  brightCyan: "#0598bc",
  brightWhite: "#a5a5a5",
}

/** 最小对比度：VSCode 的 `terminal.integrated.minimumContrastRatio` 默认值。 */
export const MIN_CONTRAST_RATIO = 4.5

/** 搜索命中的底色（随明暗切换）——对齐 VSCode 的 `terminal.findMatch*` 默认色。 */
const MATCH = {
  dark: { match: "#515c6a", active: "#f5f543" },
  light: { match: "#a8ac94", active: "#f5f543" },
}

export interface TerminalThemeInput {
  background: string
  foreground: string
  /** 选区底色（来自界面主题，缺省用半透明蓝）。 */
  selectionBackground?: string
}

/** 解析 `#rgb` / `#rrggbb` / `rgb()/rgba()` 三种常见写法；解析不出返回 null。 */
export function parseColor(color: string): [number, number, number] | null {
  const s = (color || "").trim().toLowerCase()
  if (!s) return null
  const hex = /^#([0-9a-f]{3}|[0-9a-f]{6})$/.exec(s)
  if (hex) {
    const h = hex[1]!
    const wide = h.length === 3 ? h.split("").map((c) => c + c).join("") : h
    return [parseInt(wide.slice(0, 2), 16), parseInt(wide.slice(2, 4), 16), parseInt(wide.slice(4, 6), 16)]
  }
  const rgb = /^rgba?\(\s*([\d.]+)[,\s]+([\d.]+)[,\s]+([\d.]+)/.exec(s)
  if (rgb) {
    const clamp = (n: string) => Math.max(0, Math.min(255, Math.round(Number(n))))
    const out: [number, number, number] = [clamp(rgb[1]!), clamp(rgb[2]!), clamp(rgb[3]!)]
    return out.every((n) => Number.isFinite(n)) ? out : null
  }
  return null
}

/**
 * 判定颜色是否偏暗（决定用暗色还是亮色 ANSI 调色板）。
 * 用 WCAG 相对亮度：0.5 是「中灰」这一档的分界，解析不出时按暗色处理（终端默认是暗底）。
 */
export function isDarkColor(color: string): boolean {
  const rgb = parseColor(color)
  if (!rgb) return true
  const [r, g, b] = rgb.map((v) => {
    const c = v / 255
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4
  }) as [number, number, number]
  return 0.2126 * r + 0.7152 * g + 0.0722 * b < 0.5
}

/** 组装 xterm 主题（背景 / 前景 / 光标 / 选区 + 16 色 ANSI 调色板）。 */
export function terminalTheme(input: TerminalThemeInput): Record<string, string> {
  const background = input.background || "#181818"
  const foreground = input.foreground || "#d4d4d4"
  const dark = isDarkColor(background)
  const ansi = dark ? DARK_ANSI : LIGHT_ANSI
  return {
    background,
    foreground,
    cursor: foreground,
    cursorAccent: background,
    selectionBackground: input.selectionBackground || (dark ? "rgba(120, 160, 255, 0.35)" : "rgba(60, 110, 220, 0.25)"),
    ...ansi,
  }
}

/** 搜索高亮的 decoration 取值（按明暗择色）。 */
export function searchMatchColors(background: string): { matchBackground: string; activeMatchBackground: string; activeMatchBorder: string } {
  const c = isDarkColor(background) ? MATCH.dark : MATCH.light
  return { matchBackground: c.match, activeMatchBackground: c.active, activeMatchBorder: c.active }
}
