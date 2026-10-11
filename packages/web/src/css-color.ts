/**
 * CSS 颜色解析（纯函数）：把 CSS 变量计算值解析为不透明 hex。
 * - `#rrggbb` 原样返回
 * - `rgb()`/`rgba()` 与合成底（页面背景色）叠加（半透明叠在背景上）得到不透明等效色——
 *   否则半透明变量（如 `--bg-hover: rgba(0,0,0,0.05)`）会退化回硬编码默认值，与真实呈现色不符
 * - 其余值返回 fallback
 * 用途：文件工作台 Monaco 主题映射
 * （`files/editor.ts` 逐层合成：页面底 → `--bg` 面板底 → `--bg-inset` 编辑器底，中间层是 hex，
 * 所以合成底两种写法（`rgb()/rgba()` 与 `#rrggbb`）都要认）。
 */
export function cssVarToHex(raw: string, bodyBgRaw: string, fallback: string): string {
  if (/^#[0-9a-fA-F]{6}$/.test(raw)) return raw
  const m = raw.match(/^rgba?\(\s*([\d.]+)[,\s]+([\d.]+)[,\s]+([\d.]+)(?:[,\s/]+([\d.]+%?))?\s*\)$/)
  if (!m) return fallback
  const r = +m[1]
  const g = +m[2]
  const b = +m[3]
  const a = m[4] ? parseFloat(m[4]) / (m[4].endsWith("%") ? 100 : 1) : 1
  const [br, bg, bb] = parseBackdrop(bodyBgRaw) ?? [13, 17, 23] // 兜底近黑
  const mix = (c: number, bc: number) => Math.max(0, Math.min(255, Math.round(c * a + bc * (1 - a))))
  return `#${[mix(r, br), mix(g, bg), mix(b, bb)].map((x) => x.toString(16).padStart(2, "0")).join("")}`
}

/** 解析合成底颜色：`rgb()/rgba()`（计算样式值）或 `#rrggbb`/`#rgb`（上一层合成结果）；拿不到返回 null。 */
function parseBackdrop(raw: string): [number, number, number] | null {
  const m = raw.match(/^rgba?\(\s*([\d.]+)[,\s]+([\d.]+)[,\s]+([\d.]+)/)
  if (m) return [+m[1], +m[2], +m[3]]
  const h = raw.trim().replace(/^#/, "")
  const full = h.length === 3 ? h.replace(/./g, (c) => c + c) : h
  if (!/^[0-9a-fA-F]{6}$/.test(full)) return null
  return [parseInt(full.slice(0, 2), 16), parseInt(full.slice(2, 4), 16), parseInt(full.slice(4, 6), 16)]
}
