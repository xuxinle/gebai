/**
 * CSS 颜色解析（纯函数）：把 CSS 变量计算值解析为不透明 hex。
 * - `#rrggbb` 原样返回
 * - `rgb()`/`rgba()` 与页面背景色合成（半透明叠在背景上）得到不透明等效色——
 *   否则亮/暗主题的半透明变量（如 `--bg-hover: rgba(0,0,0,0.05)`）会退化回暗色默认值，
 *   亮色主题下图表变深色不可读
 * - 其余值返回 fallback
 * 用途：PlantUML 图表主题化（skinparam / SVG fill 需要纯色）。
 */
export function cssVarToHex(raw: string, bodyBgRaw: string, fallback: string): string {
  if (/^#[0-9a-fA-F]{6}$/.test(raw)) return raw
  const m = raw.match(/^rgba?\(\s*([\d.]+)[,\s]+([\d.]+)[,\s]+([\d.]+)(?:[,\s/]+([\d.]+%?))?\s*\)$/)
  if (!m) return fallback
  const r = +m[1]
  const g = +m[2]
  const b = +m[3]
  const a = m[4] ? parseFloat(m[4]) / (m[4].endsWith("%") ? 100 : 1) : 1
  const bm = bodyBgRaw.match(/^rgba?\(\s*([\d.]+)[,\s]+([\d.]+)[,\s]+([\d.]+)/)
  const [br, bg, bb] = bm ? [+bm[1], +bm[2], +bm[3]] : [13, 17, 23] // 兜底近黑
  const mix = (c: number, bc: number) => Math.max(0, Math.min(255, Math.round(c * a + bc * (1 - a))))
  return `#${[mix(r, br), mix(g, bg), mix(b, bb)].map((x) => x.toString(16).padStart(2, "0")).join("")}`
}
