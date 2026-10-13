import { describe, expect, test } from "bun:test"
import {
  buildPalette,
  contrastRatio,
  echartsPalette,
  injectEchartsDark,
  luminance,
  mermaidThemeVariables,
  mix,
  normalizeColor,
  plantumlSkinparams,
  remapD2Svg,
  shade,
} from "./diagram-theme"

/** 暗色主题样例（默认 acrylic 暗档：真实变量值）。 */
const DARK_SRC = {
  bg: "#0c0c0e",
  text: "#f2f2f5",
  textMuted: "#8f8f99",
  accent: "#8b9aff",
  accent2: null,
  borderStrong: "rgba(255, 255, 255, .22)",
  bgElev: "rgba(18, 18, 23, .82)",
  onAccent: "#ffffff",
}

/** 亮色主题样例（ink 水墨：真实变量值）。 */
const LIGHT_SRC = {
  bg: "#f5f0e6",
  text: "#2a2620",
  textMuted: "#6e675a",
  accent: "#b03a2e",
  accent2: "#3f5c6e",
  borderStrong: "#c8bda3",
  bgElev: "#faf7ef",
  onAccent: "#fdf9f0",
}

describe("diagram-theme 色彩基元", () => {
  test("normalizeColor：#RGB/#RRGGBB/rgba()/半透明合成", () => {
    expect(normalizeColor("#FFF")).toBe("#ffffff")
    expect(normalizeColor("#f2f2f5")).toBe("#f2f2f5")
    // rgba 半透明按黑底合成：0.5 白 → ~#808080
    expect(normalizeColor("rgba(255,255,255,0.5)")).toBe("#808080")
    expect(normalizeColor("rgba(255, 255, 255, .22)")).toBe("#383838")
    expect(normalizeColor("not-a-color")).toBeNull()
    expect(normalizeColor("")).toBeNull()
  })

  test("luminance/contrastRatio：白底黑字 ≥ 15、同色 = 1", () => {
    expect(luminance("#ffffff")).toBeGreaterThan(0.999)
    expect(luminance("#000000")).toBeLessThan(0.001)
    expect(contrastRatio("#000000", "#ffffff")).toBeGreaterThan(15)
    expect(contrastRatio("#808080", "#808080")).toBe(1)
  })

  test("shade/mix：方向与端点", () => {
    expect(shade("#808080", 0.5)).toBe("#c0c0c0")
    expect(shade("#808080", -0.5)).toBe("#404040")
    expect(mix("#000000", "#ffffff", 0)).toBe("#000000")
    expect(mix("#000000", "#ffffff", 1)).toBe("#ffffff")
    expect(mix("#000000", "#ffffff", 0.5)).toBe("#808080")
  })
})

describe("buildPalette：暗色构建注入、亮色也构建（判定由 bg 亮度承载）", () => {
  test("暗色源：色板各项来自变量，rgba 边框归一为合成色", () => {
    const p = buildPalette(DARK_SRC)
    expect(p).not.toBeNull()
    expect(p!.bg).toBe("#0c0c0e")
    expect(p!.text).toBe("#f2f2f5")
    expect(p!.accent).toBe("#8b9aff")
    // borderStrong 为 rgba → 黑底合成灰
    expect(p!.border).toBe("#383838")
    // accent2 缺失降级：主色提亮（不同于 accent）
    expect(p!.accent2).not.toBe(p!.accent)
    // 缓存键稳定：同输入同键
    expect(p!.key).toBe(buildPalette(DARK_SRC)!.key)
  })

  test("亮色源：返回 null（引擎默认亮色，与历史行为一致）", () => {
    expect(buildPalette(LIGHT_SRC)).toBeNull()
  })

  test("全空源：默认暗色（工程上不出现——CSS 变量恒有值，防御性兜底）", () => {
    const p = buildPalette({})
    expect(p!.bg).toBe("#0c0c0e")
    expect(luminance(p!.bg)).toBeLessThan(0.35)
  })

  test("暗色色板可读性门槛：文字对画布/节点底 WCAG ≥ 4.5", () => {
    const p = buildPalette(DARK_SRC)!
    expect(contrastRatio(p.text, p.bg)).toBeGreaterThanOrEqual(4.5)
    expect(contrastRatio(p.text, p.node)).toBeGreaterThanOrEqual(4.5)
    expect(contrastRatio(p.text, p.nodeAlt)).toBeGreaterThanOrEqual(4.5)
    expect(contrastRatio(p.textMuted, p.bg)).toBeGreaterThanOrEqual(2.5)
  })
})

describe("Mermaid 注入", () => {
  test("核心变量族全覆盖（主色/文字/连线/时序/甘特/饼图）", () => {
    const p = buildPalette(DARK_SRC)!
    const v = mermaidThemeVariables(p)
    expect(v.background).toBe(p.bg)
    expect(v.primaryColor).toBe(p.node)
    expect(v.primaryTextColor).toBe(p.text)
    expect(v.lineColor).toBe(p.accent)
    // 时序图
    expect(v.actorBkg).toBe(p.node)
    expect(v.signalColor).toBe(p.text)
    // 甘特图（titleColor 是段标题的实际变量名，非直觉命名）
    expect(v.titleColor).toBe(p.text)
    expect(v.taskBkgColor).toBe(p.node)
    expect(v.gridColor).toBe(p.border)
    // 饼图 12 扇区
    expect(v.pie1).toBe(p.accent)
    expect(v.pie12).toBeTruthy()
  })
})

describe("PlantUML 注入", () => {
  test("skinparam 集：通用 + 分图型，均带色板色", () => {
    const p = buildPalette(DARK_SRC)!
    const lines = plantumlSkinparams(p)
    const all = lines.join("\n")
    expect(all).toContain("skinparam backgroundColor " + p.bg)
    expect(all).toContain("skinparam defaultFontColor " + p.text)
    expect(all).toContain("skinparam ArrowColor " + p.accent)
    expect(all).toContain("skinparam ClassBackgroundColor " + p.node)
    expect(all).toContain("skinparam ParticipantBackgroundColor " + p.node)
    expect(all).toContain("skinparam ActivityBackgroundColor " + p.node)
    expect(all).toContain("skinparam StateBackgroundColor " + p.node)
    // 每行都是合法 skinparam 指令（无空色值）
    for (const line of lines) expect(line).toMatch(/^skinparam \S+ (#[0-9a-f]{6}|\S+)$/)
  })
})

describe("D2 重映射", () => {
  test("亮色默认色表 → 色板色；未知色原样保留", () => {
    const p = buildPalette(DARK_SRC)!
    const svg = '<svg><rect fill="#FFFFFF"/><path stroke="#0D32B2"/><text fill="#0A0F25"/><rect fill="#123456"/></svg>'
    const out = remapD2Svg(svg, p)
    expect(out).toContain(`fill="${p.bg}"`)
    expect(out).toContain(`stroke="${p.accent}"`)
    expect(out).toContain(`fill="${p.text}"`)
    expect(out).toContain('fill="#123456"') // 用户自定义色不动
  })

  test("CSS 类声明同步重映射（D2 色板定义在 <style> 的 fill-N 系规则里，CSS 压过属性）", () => {
    const p = buildPalette(DARK_SRC)!
    const svg = '<svg><style>.d2-x .fill-N7{fill:#FFFFFF;} .d2-x .fill-B1{fill:#0D32B2;} .d2-x .fill-N1{fill:#0A0F25;}</style><rect class="fill-N7" fill="#FFFFFF"/></svg>'
    const out = remapD2Svg(svg, p)
    expect(out).toContain(`.fill-N7{fill:${p.bg};}`)
    expect(out).toContain(`.fill-B1{fill:${p.accent};}`)
    expect(out).toContain(`.fill-N1{fill:${p.text};}`)
    // 属性同步替换
    expect(out).toContain(`fill="${p.bg}"`)
    // 无关 CSS 声明不动（stroke-linejoin 等）
    expect(out).not.toContain("stroke-linejoin")
  })

  test("大小写不敏感（D2 输出大写十六进制）", () => {
    const p = buildPalette(DARK_SRC)!
    const out = remapD2Svg('<rect fill="#ffffff"/>', p)
    expect(out).toContain(`fill="${p.bg}"`)
  })
})

describe("ECharts 注入", () => {
  test("暗色：backgroundColor/textStyle/色板 + 轴系默认色；用户显式字段不被覆盖", () => {
    const p = buildPalette(DARK_SRC)!
    const option = {
      xAxis: { type: "category", data: ["甲"], axisLabel: { color: "#ff0000" } },
      yAxis: {},
      legend: { data: ["a"] },
      series: [{ type: "bar", data: [1] }],
    }
    const out = injectEchartsDark(option, p) as Record<string, any>
    expect(out.backgroundColor).toBe(p.bg)
    expect(out.textStyle).toEqual({ color: p.text })
    expect(out.color).toEqual(echartsPalette(p))
    const x = Array.isArray(out.xAxis) ? out.xAxis[0] : out.xAxis
    expect(x.axisLabel.color).toBe("#ff0000") // 用户显式保留
    expect(x.axisLine.lineStyle.color).toBe(p.border) // 注入默认
    const y = Array.isArray(out.yAxis) ? out.yAxis[0] : out.yAxis
    expect(y.axisLabel.color).toBe(p.textMuted)
    expect(out.legend.textColor).toBe(p.text)
    // 原对象不被修改（浅拷贝）
    expect((option as any).backgroundColor).toBeUndefined()
  })

  test("数组轴与对象轴两种形态等价注入", () => {
    const p = buildPalette(DARK_SRC)!
    const a = injectEchartsDark({ xAxis: [{}, {}] }, p) as any
    expect(a.xAxis).toHaveLength(2)
    expect(a.xAxis[1].splitLine.lineStyle.color).toBeTruthy()
    const b = injectEchartsDark({ xAxis: { type: "value" } }, p) as any
    expect(b.xAxis.axisLabel.color).toBe(p.textMuted)
  })

  test("系列色板：8 色、无重复、首色为主色", () => {
    const p = buildPalette(DARK_SRC)!
    const c = echartsPalette(p)
    expect(c).toHaveLength(8)
    expect(new Set(c).size).toBe(8)
    expect(c[0]).toBe(p.accent)
  })
})
