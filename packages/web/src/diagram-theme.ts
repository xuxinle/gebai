/**
 * 图表主题色板：把 UI 主题 CSS 变量转成四图表引擎（PlantUML/Mermaid/D2/ECharts）的暗色注入参数。
 *
 * 与 diagram.ts 的分工：本模块**刻意不依赖任何引擎加载逻辑**（纯函数 + CSS 变量读取），
 * 聊天页（diagram.ts）与文件工作台（files/viewers.ts）共用同一份注入规则——
 * 两处入口的图表必须同色，否则同一文件在两页看出两套配色。
 *
 * 明暗判定：按 --bg 实际合成色亮度判定（主题自身声明不可靠，且亮暗是观感问题不是配置问题）。
 * 亮色主题（水墨/亚克力亮色档）返回 null 注入——完全保持引擎默认亮色渲染，与历史行为一致。
 * 暗色主题返回注入参数：文字/连线/节点底色与 UI 主题对齐（--text/--accent/--accent-2…），
 * 用户源码内的显式颜色（classDef/skinparam/itemStyle）优先级高于注入，一律不被覆盖。
 */

/** 图表引擎主题色板（全部为 #RRGGBB，可直接写入 skinparam/themeVariables/attribute）。 */
export type DiagramPalette = {
  /** 稳定缓存键：同主题下恒定、跨主题必不同（参与渲染缓存分键）。 */
  key: string
  /** 画布底色（图表整体背景）。 */
  bg: string
  /** 正文文字。 */
  text: string
  /** 次级文字（轴标签/次要说明）。 */
  textMuted: string
  /** 主色（连线/节点边框/主系列）。 */
  accent: string
  /** 辅色（第二系列/激活态边框）。 */
  accent2: string
  /** 节点底色（比画布底略浅一档，卡片感）。 */
  node: string
  /** 节点底色·次档（分组容器/备注/激活块）。 */
  nodeAlt: string
  /** 弱边框（生命线/分割线/网格线）。 */
  border: string
  /** 主色上的文字（on-accent，深底主题通常为亮色）。 */
  onAccent: string
}

/** 从根元素读 CSS 变量并解析为十六进制色；变量缺失/无法解析时返回 null。 */
function readVar(cs: CSSStyleDeclaration, name: string): string | null {
  const raw = cs.getPropertyValue(name).trim()
  if (!raw) return null
  return normalizeColor(raw)
}

/** 颜色归一：#RGB/#RGBA/#RRGGBB/#RRGGBBAA/rgba()/命名色 → #RRGGBB（透明通道参与合成时按黑底）。 */
export function normalizeColor(input: string): string | null {
  const s = input.trim().toLowerCase()
  // #RGB / #RGBA
  let m = s.match(/^#([0-9a-f]{3,4})$/)
  if (m) {
    const h = m[1]
    const r = h[0] + h[0], g = h[1] + h[1], b = h[2] + h[2]
    return `#${r}${g}${b}`
  }
  // #RRGGBB / #RRGGBBAA
  m = s.match(/^#([0-9a-f]{6})([0-9a-f]{2})?$/)
  if (m) return `#${m[1]}`
  // rgb()/rgba()
  m = s.match(/^rgba?\(\s*([\d.]+)[\s,]+([\d.]+)[\s,]+([\d.]+)(?:[\s,/]+([\d.%]+))?\s*\)$/)
  if (m) {
    const a = m[4] === undefined ? 1 : m[4].endsWith("%") ? parseFloat(m[4]) / 100 : parseFloat(m[4])
    const base = [m[1], m[2], m[3]].map((v) => Math.max(0, Math.min(255, Math.round(parseFloat(v)))))
    // 半透明按黑底合成（主题变量在暗色下叠黑背景，观感以合成为准）
    const out = base.map((v) => Math.round(v * a))
    return `#${out.map((v) => v.toString(16).padStart(2, "0")).join("")}`
  }
  return null
}

/** 相对亮度（WCAG）：0（黑）~1（白）。 */
export function luminance(hex: string): number {
  const n = parseInt(hex.slice(1), 16)
  const r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255
  const lin = [r, g, b].map((v) => {
    const c = v / 255
    return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4
  })
  return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]
}

/** WCAG 对比度（1~21），量化「暗底上文字是否可读」的标准尺子（正文 ≥4.5、大字/图形 ≥3）。 */
export function contrastRatio(a: string, b: string): number {
  const la = luminance(a), lb = luminance(b)
  const [hi, lo] = la >= lb ? [la, lb] : [lb, la]
  return (hi + 0.05) / (lo + 0.05)
}

/** 颜色变亮/变暗：amount ∈ [-1,1]，负数压暗（向黑靠近）、正数提亮。 */
export function shade(hex: string, amount: number): string {
  const n = parseInt(hex.slice(1), 16)
  let r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255
  if (amount >= 0) {
    r = Math.round(r + (255 - r) * amount)
    g = Math.round(g + (255 - g) * amount)
    b = Math.round(b + (255 - b) * amount)
  } else {
    r = Math.round(r * (1 + amount))
    g = Math.round(g * (1 + amount))
    b = Math.round(b * (1 + amount))
  }
  return `#${[r, g, b].map((v) => Math.max(0, Math.min(255, v)).toString(16).padStart(2, "0")).join("")}`
}

/** 混合两色：ratio ∈ [0,1]，0=a、1=b。 */
export function mix(a: string, b: string, ratio: number): string {
  const pa = parseInt(a.slice(1), 16), pb = parseInt(b.slice(1), 16)
  const out = [0, 1, 2].map((i) => {
    const va = (pa >> (16 - i * 8)) & 255, vb = (pb >> (16 - i * 8)) & 255
    return Math.round(va + (vb - va) * ratio)
  })
  return `#${out.map((v) => v.toString(16).padStart(2, "0")).join("")}`
}

/** CSS 变量名集合：色板的所有来源变量（缺失时逐项降级，见 buildPalette）。 */
const VAR = {
  bg: "--bg",
  text: "--text",
  textMuted: "--text-muted",
  accent: "--accent",
  accent2: "--accent-2",
  borderStrong: "--border-strong",
  bgElev: "--bg-elev",
  onAccent: "--on-accent",
} as const

/** 根元素上读取的变量原始值（buildPalette 入参，便于测试注入）。 */
export type PaletteSource = Partial<Record<keyof typeof VAR, string | null>>

/** 读当前主题变量（getComputedStyle 快照；主题切换后重读即得新色板）。 */
export function readPaletteSource(): PaletteSource {
  if (typeof document === "undefined") return {}
  const cs = getComputedStyle(document.documentElement)
  const out: PaletteSource = {}
  for (const k of Object.keys(VAR) as Array<keyof typeof VAR>) out[k] = readVar(cs, VAR[k])
  return out
}

/** 由变量源构建色板：亮色主题（--bg 亮度 ≥ 0.35）返回 null——引擎默认亮色已是最佳亮色渲染，
 *  不注入（与历史行为一致）；暗色主题构建注入色板，变量缺失时按依赖链降级（--accent-2 缺失用
 *  --accent 提亮替代等）；全部色值经 normalizeColor 归一为 #RRGGBB（rgba/半透明不可直接写入
 *  skinparam/themeVariables）。 */
export function buildPalette(src: PaletteSource): DiagramPalette | null {
  const norm = (v: string | null | undefined, fallback: string): string => normalizeColor(v ?? "") ?? fallback
  const bgProbe = norm(src.bg, "#0c0c0e")
  if (luminance(bgProbe) >= 0.35) return null // 亮色主题：引擎默认亮色
  const bg = bgProbe
  const text = norm(src.text, "#f2f2f5")
  const accent = norm(src.accent, "#8b9aff")
  const textMuted = norm(src.textMuted, mix(text, bg, 0.45))
  const accent2 = src.accent2 && src.accent2 !== src.accent ? norm(src.accent2, shade(accent, 0.18)) : shade(accent, 0.18)
  const border = norm(src.borderStrong, mix(text, bg, 0.28))
  const onAccent = norm(src.onAccent, "#ffffff")
  const node = shade(bg, 0.12)
  const nodeAlt = shade(bg, 0.2)
  return {
    key: [bg, text, accent, accent2].join("|"),
    bg, text, textMuted, accent, accent2, node, nodeAlt, border, onAccent,
  }
}

/** 当前主题色板（null=亮色主题，保持引擎默认渲染）。 */
export function currentPalette(): DiagramPalette | null {
  return buildPalette(readPaletteSource())
}

/* ---------- Mermaid 注入 ---------- */

/** Mermaid themeVariables（暗色）：主色系 + 时序图/甘特图/饼图分图型变量，全部实测生效。 */
export function mermaidThemeVariables(p: DiagramPalette): Record<string, string> {
  const line = p.accent
  return {
    // 主变量族（flowchart/state/class/er 等通用）
    background: p.bg,
    primaryColor: p.node,
    primaryTextColor: p.text,
    primaryBorderColor: p.accent,
    lineColor: line,
    textColor: p.text,
    fontSize: "14px",
    // 二级/三级节点底
    secondaryColor: p.nodeAlt,
    tertiaryColor: p.node,
    mainBkg: p.node,
    nodeBorder: p.accent,
    // cluster/subgraph
    clusterBkg: p.nodeAlt,
    clusterBorder: p.border,
    titleColor: p.text,
    edgeLabelBackground: p.nodeAlt,
    // 时序图
    actorBkg: p.node,
    actorBorder: p.accent,
    actorTextColor: p.text,
    actorLineColor: p.border,
    signalColor: p.text,
    signalTextColor: p.text,
    labelBoxBkgColor: p.nodeAlt,
    labelBoxBorderColor: p.accent,
    labelTextColor: p.text,
    loopTextColor: p.text,
    noteBkgColor: p.nodeAlt,
    noteBorderColor: p.accent,
    noteTextColor: p.text,
    activationBkgColor: p.nodeAlt,
    activationBorderColor: p.accent,
    sequenceNumberColor: p.onAccent,
    // 甘特图
    altSectionBkgColor: p.nodeAlt,
    sectionBkgColor: p.node,
    sectionBkgColor2: p.nodeAlt,
    altBackground: p.node,
    taskBkgColor: p.node,
    taskBorderColor: p.accent,
    taskTextColor: p.text,
    taskTextOutsideColor: p.text,
    taskTextDarkColor: p.text,
    taskTextClickableColor: p.accent2,
    gridColor: p.border,
    todayLineColor: p.accent2,
    critBkgColor: p.accent2,
    critBorderColor: p.accent2,
    critColor: p.accent2,
    activeTaskBkgColor: p.nodeAlt,
    activeTaskBorderColor: p.accent,
    doneTaskBkgColor: p.node,
    doneTaskBorderColor: p.border,
    vertLineColor: p.border,
    // 饼图扇区（12 色环：主色起手 + 色相渐进，避免亮色主题遗留的糖果色）
    pie1: p.accent,
    pie2: p.accent2,
    pie3: shade(p.accent, 0.3),
    pie4: mix(p.accent2, "#ffffff", 0.3),
    pie5: shade(p.accent, 0.45),
    pie6: mix(p.accent, p.text, 0.4),
    pie7: mix(p.accent2, p.text, 0.4),
    pie8: shade(p.accent2, 0.3),
    pie9: mix(p.accent, p.text, 0.6),
    pie10: mix(p.accent2, p.text, 0.6),
    pie11: shade(p.accent, 0.6),
    pie12: mix(p.accent2, p.text, 0.75),
    // 状态图
    labelBackground: p.nodeAlt,
    compositeTitleBackground: p.nodeAlt,
    compositeBackground: p.nodeAlt,
    compositeBorderBackground: p.accent,
    // class 图
    classText: p.text,
    // er 图
    attributeBackgroundColorOdd: p.node,
    attributeBackgroundColorEven: p.nodeAlt,
  }
}

/* ---------- PlantUML 注入（skinparam 逐项；!theme 在 TeaVM 版被静默忽略，不可用） ---------- */

/** PlantUML skinparam 行（暗色）：跨图型通用集 + 分图型全覆盖（时序/类/状态/活动/用例/组件部署/
 *  包围容器/思维导图/WBS），全部实测生效（逐一渲染断言用色，含用户示例用例图）。 */
export function plantumlSkinparams(p: DiagramPalette): string[] {
  return [
    "skinparam backgroundColor " + p.bg,
    "skinparam defaultFontColor " + p.text,
    "skinparam ArrowColor " + p.accent,
    "skinparam ArrowFontColor " + p.text,
    "skinparam BorderColor " + p.accent,
    // 时序图
    "skinparam ParticipantBackgroundColor " + p.node,
    "skinparam ParticipantBorderColor " + p.accent,
    "skinparam ParticipantFontColor " + p.text,
    "skinparam LifeLineBackgroundColor " + p.node,
    "skinparam LifeLineBorderColor " + p.border,
    "skinparam ActorBackgroundColor " + p.node,
    "skinparam ActorBorderColor " + p.accent,
    "skinparam ActorFontColor " + p.text,
    "skinparam SequenceGroupBorderColor " + p.accent,
    "skinparam SequenceGroupFontColor " + p.text,
    "skinparam SequenceGroupHeaderFontColor " + p.text,
    "skinparam NoteBackgroundColor " + p.nodeAlt,
    "skinparam NoteBorderColor " + p.border,
    "skinparam NoteFontColor " + p.text,
    "skinparam DividerBackgroundColor " + p.nodeAlt,
    "skinparam DividerFontColor " + p.text,
    // 类图/对象
    "skinparam ClassBackgroundColor " + p.node,
    "skinparam ClassBorderColor " + p.accent,
    "skinparam ClassFontColor " + p.text,
    "skinparam ClassStereotypeFontColor " + p.textMuted,
    "skinparam ClassAttributeFontColor " + p.text,
    "skinparam ObjectBackgroundColor " + p.node,
    "skinparam ObjectBorderColor " + p.accent,
    "skinparam ObjectFontColor " + p.text,
    "skinparam ObjectAttributeFontColor " + p.text,
    // 状态图
    "skinparam StateBackgroundColor " + p.node,
    "skinparam StateBorderColor " + p.accent,
    "skinparam StateFontColor " + p.text,
    "skinparam StateAttributeFontColor " + p.text,
    // 活动图
    "skinparam ActivityBackgroundColor " + p.node,
    "skinparam ActivityBorderColor " + p.accent,
    "skinparam ActivityFontColor " + p.text,
    "skinparam ActivityDiamondBackgroundColor " + p.nodeAlt,
    "skinparam ActivityDiamondBorderColor " + p.accent,
    "skinparam ActivityDiamondFontColor " + p.text,
    "skinparam ActivityStartColor " + p.accent,
    "skinparam ActivityEndColor " + p.accent,
    // 用例图
    "skinparam UsecaseBackgroundColor " + p.node,
    "skinparam UsecaseBorderColor " + p.accent,
    "skinparam UsecaseFontColor " + p.text,
    // 组件/部署
    "skinparam ComponentBackgroundColor " + p.node,
    "skinparam ComponentBorderColor " + p.accent,
    "skinparam ComponentFontColor " + p.text,
    "skinparam ComponentStyle rectangle",
    "skinparam AgentBackgroundColor " + p.node,
    "skinparam AgentBorderColor " + p.accent,
    "skinparam AgentFontColor " + p.text,
    "skinparam DatabaseBackgroundColor " + p.node,
    "skinparam DatabaseBorderColor " + p.accent,
    "skinparam DatabaseFontColor " + p.text,
    "skinparam QueueBackgroundColor " + p.node,
    "skinparam QueueBorderColor " + p.accent,
    "skinparam QueueFontColor " + p.text,
    "skinparam ArtifactBackgroundColor " + p.node,
    "skinparam ArtifactBorderColor " + p.accent,
    "skinparam ArtifactFontColor " + p.text,
    "skinparam StorageBackgroundColor " + p.node,
    "skinparam StorageBorderColor " + p.accent,
    "skinparam StorageFontColor " + p.text,
    "skinparam CardBackgroundColor " + p.node,
    "skinparam CardBorderColor " + p.accent,
    "skinparam CardFontColor " + p.text,
    "skinparam FileBackgroundColor " + p.node,
    "skinparam FileBorderColor " + p.accent,
    "skinparam FileFontColor " + p.text,
    "skinparam InterfaceBackgroundColor " + p.node,
    "skinparam InterfaceBorderColor " + p.accent,
    "skinparam CollectionsBackgroundColor " + p.node,
    "skinparam CollectionsBorderColor " + p.accent,
    // 包围容器（rectangle/package/folder/frame/cloud——用例图的系统边界、组件图的分组框）
    "skinparam RectangleBackgroundColor " + p.nodeAlt,
    "skinparam RectangleBorderColor " + p.border,
    "skinparam RectangleFontColor " + p.text,
    "skinparam RectangleStereotypeFontColor " + p.textMuted,
    "skinparam PackageBackgroundColor " + p.nodeAlt,
    "skinparam PackageBorderColor " + p.border,
    "skinparam PackageFontColor " + p.text,
    "skinparam FolderBackgroundColor " + p.nodeAlt,
    "skinparam FolderBorderColor " + p.border,
    "skinparam FolderFontColor " + p.text,
    "skinparam FrameBackgroundColor " + p.nodeAlt,
    "skinparam FrameBorderColor " + p.border,
    "skinparam FrameFontColor " + p.text,
    "skinparam CloudBackgroundColor " + p.nodeAlt,
    "skinparam CloudBorderColor " + p.border,
    "skinparam CloudFontColor " + p.text,
    // 思维导图/WBS（Node* 系实测对 mindmap/wbs 节点生效）
    "skinparam NodeBackgroundColor " + p.node,
    "skinparam NodeBorderColor " + p.accent,
    "skinparam NodeFontColor " + p.text,
    // 通用兜底
    "skinparam TitleFontColor " + p.text,
    "skinparam CaptionFontColor " + p.textMuted,
    "skinparam LegendBackgroundColor " + p.nodeAlt,
    "skinparam LegendBorderColor " + p.border,
    "skinparam LegendFontColor " + p.text,
    "skinparam HeaderFooterFontColor " + p.textMuted,
  ]
}

/* ---------- D2 注入（SVG 后处理颜色重映射） ---------- */

/**
 * D2 浏览器版仅内置 8 个亮色装饰主题、无暗色（themeID 9+ 报 theme not found、vars dark-id 无效，实测）。
 * 暗色方案：渲染后按实测色表重映射（引擎输出为封闭色集，映射表稳定）：
 *   背景 #FFFFFF→画布底；节点底 #F7F8FE/#E3E9FD/#EDF0FD→节点底两档；深字 #0A0F25→正文；
 *   主蓝 #0D32B2/#4A6FF3→主色；灰字 #676C7E→次级文字。
 * 未在表内的颜色原样保留（用户 style 指定的颜色不受影响）。
 */
const D2_REMAP_TABLE: Array<{ from: string[]; to: keyof DiagramPalette }> = [
  { from: ["#FFFFFF"], to: "bg" },
  { from: ["#F7F8FE", "#E3E9FD", "#EDF0FD", "#F2EDFF", "#E5F3FF", "#E7EAFF", "#E5EDFF", "#F4F0FF"], to: "node" },
  { from: ["#0A0F25"], to: "text" },
  { from: ["#0D32B2", "#4A6FF3", "#000E3D", "#170034", "#010E31"], to: "accent" },
  { from: ["#676C7E"], to: "textMuted" },
]

/** D2 SVG 暗色重映射：fill/stroke 属性与 <style> 内 CSS 声明同步替换（D2 色板定义在 fill-N 系
 *  类规则里，CSS 优先级高于 presentation 属性——只换属性会被原 CSS 白底压回，实测疑难坑）。 */
export function remapD2Svg(svg: string, p: DiagramPalette): string {
  const map = new Map<string, string>()
  for (const entry of D2_REMAP_TABLE) for (const f of entry.from) map.set(f, p[entry.to])
  const remapHex = (whole: string, attr: string, hex: string): string => {
    const to = map.get(hex.toUpperCase())
    return to ? `${attr}="${to}"` : whole
  }
  const remapCss = (whole: string, prop: string, hex: string): string => {
    const to = map.get(hex.toUpperCase())
    return to ? `${prop}:${to}` : whole
  }
  return svg
    .replace(/(fill|stroke)="(#[0-9A-Fa-f]{3,8})"/gi, remapHex)
    .replace(/(fill|stroke)\s*:\s*(#[0-9A-Fa-f]{3,8})/gi, remapCss)
}

/* ---------- ECharts 注入 ---------- */

/**
 * ECharts 暗色注入（浅合并，用户显式配置优先——不覆盖任何已写字段）：
 * backgroundColor/textStyle/色板 + 坐标轴文字与网格线（默认灰 #54555a 在暗底对比不足，实测）。
 * 只做一层注入（xAxis/yAxis/series 为数组或对象时逐项浅合并 axisLabel/lineStyle）。
 */
export function injectEchartsDark(option: Record<string, unknown>, p: DiagramPalette): Record<string, unknown> {
  const o: Record<string, unknown> = { ...option }
  if (o.backgroundColor === undefined) o.backgroundColor = p.bg
  if (o.textStyle === undefined) o.textStyle = { color: p.text }
  if (o.color === undefined) o.color = echartsPalette(p)
  for (const axis of ["xAxis", "yAxis", "radiusAxis", "angleAxis", "singleAxis"] as const) {
    const cur = o[axis]
    if (cur === undefined) continue
    const items = Array.isArray(cur) ? cur : [cur]
    o[axis] = items.map((it: Record<string, unknown>) => {
      if (typeof it !== "object" || it === null) return it
      const out = { ...it }
      if (out.axisLabel === undefined) out.axisLabel = { color: p.textMuted }
      if (out.axisLine === undefined) out.axisLine = { lineStyle: { color: p.border } }
      if (out.axisTick === undefined) out.axisTick = { lineStyle: { color: p.border } }
      if (out.splitLine === undefined) out.splitLine = { lineStyle: { color: mix(p.border, p.bg, 0.4) } }
      if (out.nameTextStyle === undefined) out.nameTextStyle = { color: p.textMuted }
      return out
    })
    if (!Array.isArray(cur)) o[axis] = (o[axis] as unknown[])[0]
  }
  // 图例/标题默认文字色跟随 textStyle，但 legend 项文字独立配置项较多，显式补默认
  const legend = o.legend
  if (legend !== undefined) {
    const items = Array.isArray(legend) ? legend : [legend]
    const mapped = items.map((it: Record<string, unknown>) => (typeof it === "object" && it !== null && it.textColor === undefined ? { ...it, textColor: p.text } : it))
    o.legend = Array.isArray(legend) ? mapped : mapped[0]
  }
  return o
}

/** ECharts 系列色板（8 色，用户未写 color 时使用）。 */
export function echartsPalette(p: DiagramPalette): string[] {
  return [p.accent, p.accent2, shade(p.accent, 0.3), mix(p.accent2, p.text, 0.35), mix(p.accent, p.text, 0.5), shade(p.accent2, 0.3), mix(p.accent, p.text, 0.65), mix(p.accent2, p.text, 0.6)]
}
