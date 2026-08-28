/**
 * 后端图表渲染器（show 图表分支 `render=backend` 与飞书通道共用）：四种图表语言 → SVG（本地引擎，零网络）→ PNG（@resvg/resvg-js）。
 * - **plantuml**：复用 `feishu-bot/plantuml.ts`（TeaVM 引擎 + DOM shim，浅色主题白底）。
 * - **mermaid**：`mermaid` npm 包 + happy-dom DOM 垫层——**垫层必须先在 mermaid 导入前安装**（mermaid 模块求值即建样式表）；
 *   固定浅色主题（`theme: "default"`）+ `htmlLabels: false`（resvg 不支持 foreignObject，纯 SVG 输出）。
 * - **d2**：`@terrastruct/d2` 官方 WASM（node-esm 构建，文件路径 Worker——bun build 无法内联）：dev 模式直接 import 包；
 *   **二进制模式**从内嵌产物（`d2js.embedded.generated.json`，构建脚本 `scripts/build-d2js.ts` 生成，gzip base64）
 *   物化到 `{GEBAI_HOME}/vendor/d2js/{version}/` 后动态 import（与 playwright 子Agent 的 driver.mjs 复制同思路的打包闭环）。
 * - **echarts**：`echarts` npm 包 SSR 渲染（`ssr:true` + SVGRenderer，零 DOM）——**顶层急切导入**：zrender 环境探测在模块求值期
 *   完成，若在 happy-dom/PlantUML 垫层污染全局 window/document 后才加载会误判浏览器环境（文本测量需真 canvas，垫层不支持）；
 *   本文件经引擎惰性 import，急切导入不增加启动开销。
 * - 各语言渲染经**串行队列**（mermaid 的 happy-dom document 与 d2 单 Worker 均为共享状态，防并发冲突）；依赖全部可注入（测试用 fake）。
 * - 失败抛错携带渲染原因（供回传模型修正源码）。
 */
import type { DiagramFormat } from "@gebai/sdk"
import * as echarts from "echarts"
import { dirname, join } from "node:path"
import { existsSync, mkdirSync, readdirSync, rmSync, writeFileSync } from "node:fs"
import { pathToFileURL } from "node:url"
import { Window, CSSStyleSheet } from "happy-dom"
import { isBinaryMode, resolveGebaiHome } from "./config"

/** 后端渲染器接口（引擎 ToolContext 与飞书桥接依赖注入，测试可伪造）。 */
export interface DiagramRenderer {
  /** 渲染图表源码为 PNG 字节；失败抛错（错误信息含渲染原因）。format 缺省 plantuml。 */
  renderPng(code: string, opts?: { format?: DiagramFormat; background?: string; maxWidth?: number; maxHeight?: number }): Promise<Uint8Array>
}

/** SVG → PNG 栅格化参数。 */
export interface RasterizeOpts {
  background?: string
  maxWidth?: number
  maxHeight?: number
}

/* ---------------- SVG → PNG 栅格化（共享，plantuml.ts 复用） ---------------- */

/** 默认导出尺寸上限（px）：超出按比例缩放，防超大 PNG 超出飞书图片限制。 */
const DEFAULT_MAX_WIDTH = 1600
const DEFAULT_MAX_HEIGHT = 2400

/** 从 SVG 根元素解析逻辑尺寸（px）：优先 width/height 属性，非绝对数值（如 width="100%"）回退 viewBox 尺寸（mermaid 输出形态）。 */
export function svgLogicalSize(svg: string): { width: number; height: number } {
  const attrs = svg.match(/<svg[^>]*>/)
  const head = attrs ? attrs[0] : ""
  const attr = (name: string): number => {
    const m = head.match(new RegExp(`${name}="([\\d.]+)"`))
    if (!m || !Number.isFinite(Number(m[1]))) return 0
    return Number(m[1])
  }
  let w = attr("width")
  let h = attr("height")
  if (!(w > 0) || !(h > 0)) {
    const vb = head.match(/viewBox="([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)"/)
    if (vb) {
      w = Number(vb[3])
      h = Number(vb[4])
    }
  }
  return { width: w > 0 ? w : 0, height: h > 0 ? h : 0 }
}

/**
 * SVG 根元素规范化（resvg 兼容）：
 * ① 根宽高以逻辑尺寸显式化（resvg 无法解析百分比尺寸，如 mermaid 的 width="100%"）；
 * ② **负原点 viewBox 归一**（resvg 对负原点 viewBox 会 panic——geom IntRect 反演）：去掉 viewBox 后
 *    以平移组 `translate(-minX, -minY)` 把内容移进正象限，视觉结果与浏览器一致；
 *    平移组**不包裹**开头的 `<style>`/`<defs>`/`<title>`（svgdom 解析要求其保持 svg 直接子元素，实测包入 g 会报解析错误）。
 */
export function normalizeSvgRoot(svg: string, size: { width: number; height: number }): string {
  const m = svg.match(/<svg([^>]*)>/)
  if (!m) return svg
  const attrs = m[1]
  const vb = attrs.match(/viewBox="([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)"/)
  const head = attrs.replace(/\sviewBox="[^"]*"/, "").replace(/\s+(?:width|height)="[^"]*"/g, "")
  const bodyAll = svg.slice(m[0].length).replace(/<\/svg>\s*$/, "")
  let out = `<svg width="${size.width}" height="${size.height}"${head}>`
  if (vb && (Number(vb[1]) !== 0 || Number(vb[2]) !== 0) && Number(vb[3]) > 0 && Number(vb[4]) > 0) {
    // 开头元数据元素（style/defs/title）保持为 svg 直接子元素，平移组包裹其余内容
    let pos = 0
    while (true) {
      const next = bodyAll.slice(pos).match(/^<(style|defs|title)\b[\s\S]*?<\/\1>/)
      if (!next) break
      pos += next[0].length
    }
    out = `${out}${bodyAll.slice(0, pos)}<g transform="translate(${-Number(vb[1])},${-Number(vb[2])})">${bodyAll.slice(pos)}</g></svg>`
  } else {
    out = `${out}${bodyAll}</svg>`
  }
  return out
}

/** SVG → PNG（@resvg/resvg-js；默认 2x 超采样，超大图按上限等比缩放）。 */
export async function svgRasterize(svg: string, o: RasterizeOpts = {}): Promise<Uint8Array> {
  const { Resvg } = await import("@resvg/resvg-js")
  const { width: w, height: h } = svgLogicalSize(svg)
  const m = svg.match(/<svg([^>]*)>/)
  const needsNormalize = w > 0 && h > 0 && (m == null || !/^<svg[^>]*width="[\d.]+"/.test(svg) || /viewBox="[^"]*-/.test(m[1]))
  const input = needsNormalize ? normalizeSvgRoot(svg, { width: w, height: h }) : svg
  let zoom = 2
  if (w > 0 && h > 0) {
    const maxW = o.maxWidth ?? DEFAULT_MAX_WIDTH
    const maxH = o.maxHeight ?? DEFAULT_MAX_HEIGHT
    zoom = Math.min(zoom, maxW / w, maxH / h)
    if (zoom <= 0 || !Number.isFinite(zoom)) zoom = 1
  }
  const r = new Resvg(input, { fitTo: { mode: "zoom", value: zoom }, background: o.background ?? "#ffffff" })
  return r.render().asPng()
}

/* ---------------- mermaid：happy-dom DOM 垫层 + 本地渲染 ---------------- */

type MermaidApi = {
  initialize: (cfg: Record<string, unknown>) => void
  render: (id: string, code: string) => Promise<{ svg: string }>
}

let domShimInstalled = false
/** happy-dom Window 实例（installDomShim 创建；withGlobalWindow 临时注入 globalThis.window 用）。 */
let domWindow: Window | null = null
/** 文本宽度逐字符估算：全角字符（CJK 表意/扩展、假名/谚文、CJK 标点、全角形式等）≈ 1.0em，
 *  其余（ASCII/数字/标点）≈ 0.6em（sans-serif 平均近似）。全角统一按 0.6em 计算会低估约 40%，
 *  中文标签经 getBBox → viewBox 传导导致整图偏窄、文字被裁剪。 */
export function textWidthEstimate(text: string, fontSize: number): number {
  let width = 0
  for (const ch of text) {
    const cp = ch.codePointAt(0) ?? 0
    const fullWidth =
      (cp >= 0x2e80 && cp <= 0x9fff) || // CJK 部首/标点/表意文字（含扩展 A）、假名
      (cp >= 0xac00 && cp <= 0xd7af) || // 谚文音节
      (cp >= 0xf900 && cp <= 0xfaff) || // CJK 兼容表意
      (cp >= 0x3000 && cp <= 0x303f) || // CJK 标点
      (cp >= 0xff00 && cp <= 0xffef) // 全角形式（全角字母/数字/符号）
    width += fullWidth ? fontSize : fontSize * 0.6
  }
  return width
}

/** 从自身几何属性估算单元素包围盒（getBBox 覆盖用；transform 仅支持 translate）。 */
export function estimateGeometry(el: unknown): { x: number; y: number; width: number; height: number } | null {
  const e = el as {
    tagName?: string
    getAttribute?: (n: string) => string | null
    textContent?: string | null
    children?: unknown[]
  }
  const attr = (n: string): string | null => e.getAttribute?.(n) ?? null
  const num = (v: string | null, dflt = 0): number => {
    const n = Number.parseFloat(v ?? "")
    return Number.isFinite(n) ? n : dflt
  }
  const fontSize = (): number => {
    const style = attr("style")
    const m = style?.match(/font-size:\s*([\d.]+)px/)
    if (m) return Number(m[1])
    return num(attr("font-size"), 16)
  }
  switch (e.tagName) {
    case "text":
    case "tspan": {
      // 无布局引擎：文本宽度按全角 1.0em / 半角 0.6em 逐字符估算（见 textWidthEstimate）
      const fs = fontSize()
      const w = textWidthEstimate(e.textContent ?? "", fs)
      const h = fs * 1.2
      const x = num(attr("x"))
      const y = num(attr("y"), fs)
      return { x, y: y - h, width: w, height: h }
    }
    case "line": {
      const x1 = num(attr("x1"))
      const y1 = num(attr("y1"))
      const x2 = num(attr("x2"), x1)
      const y2 = num(attr("y2"), y1)
      return { x: Math.min(x1, x2), y: Math.min(y1, y2), width: Math.abs(x2 - x1), height: Math.abs(y2 - y1) }
    }
    case "rect":
      return { x: num(attr("x")), y: num(attr("y")), width: num(attr("width")), height: num(attr("height")) }
    case "circle":
    case "ellipse": {
      const cx = num(attr("cx"))
      const cy = num(attr("cy"))
      const r = Math.max(num(attr("r")), num(attr("rx")), num(attr("ry")))
      return { x: cx - r, y: cy - r, width: r * 2, height: r * 2 }
    }
    case "path": {
      // 粗估：提取 d 中全部数值坐标，min/max 即包围盒（曲线控制点含于其中，结果略大无碍）
      const d = attr("d") ?? ""
      const coords = d.match(/-?[\d.]+(?:e-?\d+)?/g)?.map(Number) ?? []
      if (!coords.length) return null
      const xs: number[] = []
      const ys: number[] = []
      for (let i = 0; i < coords.length; i++) (i % 2 === 0 ? xs : ys).push(coords[i])
      if (!xs.length || !ys.length) return null
      const minX = Math.min(...xs)
      const maxX = Math.max(...xs)
      const minY = Math.min(...ys)
      const maxY = Math.max(...ys)
      return { x: minX, y: minY, width: maxX - minX, height: maxY - minY }
    }
    case "polygon":
    case "polyline": {
      const pts = (attr("points") ?? "").trim().split(/[\s,]+/).map(Number).filter(Number.isFinite)
      if (pts.length < 4) return null
      const xs = pts.filter((_, i) => i % 2 === 0)
      const ys = pts.filter((_, i) => i % 2 === 1)
      return { x: Math.min(...xs), y: Math.min(...ys), width: Math.max(...xs) - Math.min(...xs), height: Math.max(...ys) - Math.min(...ys) }
    }
    default:
      return null
  }
}

/** 解析 translate(x,y) 变换偏移。 */
function translateOf(el: unknown): { x: number; y: number } {
  const t = (el as { getAttribute?: (n: string) => string | null }).getAttribute?.("transform") ?? ""
  const m = t.match(/translate\(\s*(-?[\d.]+)[\s,]+(-?[\d.]+)\s*\)/)
  return m ? { x: Number(m[1]), y: Number(m[2]) } : { x: 0, y: 0 }
}

/**
 * getBBox 估算实现（覆盖 happy-dom 返回零矩形的实现）：
 * mermaid 依赖 getBBox 计算标签尺寸与最终 viewBox（`setupGraphViewbox` 取根元素 bbox 加 padding），
 * happy-dom 无布局引擎返回全零 → 布局坍缩为 16px。此处按几何属性（rect/path 坐标、文本长度估算）重建包围盒，
 * 容器元素为子树并集并叠加 translate 变换。
 */
function estimatedBBox(el: unknown): { x: number; y: number; width: number; height: number } {
  const self = estimateGeometry(el)
  let minX = self?.x ?? 0
  let minY = self?.y ?? 0
  let maxX = minX + (self?.width ?? 0)
  let maxY = minY + (self?.height ?? 0)
  const t = translateOf(el)
  const children = (el as { children?: unknown[] }).children ?? []
  let any = !!self
  for (const c of children) {
    if (!(c as { tagName?: string }).tagName) continue
    const b = estimatedBBox(c)
    const x0 = t.x + b.x
    const y0 = t.y + b.y
    const x1 = x0 + b.width
    const y1 = y0 + b.height
    if (!any) {
      minX = x0
      minY = y0
      maxX = x1
      maxY = y1
      any = true
    } else {
      minX = Math.min(minX, x0)
      minY = Math.min(minY, y0)
      maxX = Math.max(maxX, x1)
      maxY = Math.max(maxY, y1)
    }
  }
  return any ? { x: minX, y: minY, width: Math.max(0, maxX - minX), height: Math.max(0, maxY - minY) } : { x: 0, y: 0, width: 0, height: 0 }
}

/** 安装 happy-dom DOM 垫层（幂等；mermaid 模块求值即建样式表，必须先于其导入执行）。
 *  ⚠️ **不设置 globalThis.window**——实测 Bun 的 `node:worker_threads` 在 `globalThis.window` 存在时 Worker 启动挂起
 *  （d2 WASM 渲染器依赖 worker_threads）；mermaid 仅在模块导入期读取裸 `window`，由 `withGlobalWindow` 临时注入。
 *  force=true 时重新应用（PlantUML 垫层会覆盖全局 document/XMLSerializer，mermaid 渲染前需还原）。 */
function installDomShim(force = false): void {
  if (domShimInstalled && !force) return
  const window = new Window()
  domWindow = window
  const g = globalThis as Record<string, unknown>
  for (const k of Object.getOwnPropertyNames(window)) {
    if (k === "undefined" || k in g || k === "window") continue
    try {
      g[k] = (window as unknown as Record<string, unknown>)[k]
    } catch {
      /* 只读属性跳过 */
    }
  }
  g.document = window.document
  g.navigator = window.navigator
  g.getComputedStyle = window.getComputedStyle.bind(window)
  g.CSSStyleSheet ??= CSSStyleSheet
  g.CSS ??= { escape: (s: string) => s, supports: () => false }
  g.requestAnimationFrame ??= (cb: () => void) => setTimeout(cb, 16)
  g.cancelAnimationFrame ??= clearTimeout
  // happy-dom document.baseURI 为 "about:blank"，`new URL(relative, about:blank)` 解析失败——PlantUML 的
  // viz-global.js 运行时按 document.baseURI 拼相对路径，此处改为有效基址（只读属性，经 defineProperty 覆盖）
  try {
    Object.defineProperty(window.document, "baseURI", { value: "file:///gebai/", writable: true, configurable: true })
  } catch {
    /* 覆盖失败不影响 mermaid 主路径 */
  }
  // 覆盖 happy-dom 返回零矩形的 getBBox（SVGSVGElement/SVGGraphicsElement 各自原型上均有定义）
  const w = window as unknown as { SVGSVGElement?: { prototype: { getBBox: unknown } }; SVGGraphicsElement?: { prototype: { getBBox: unknown } } }
  const override = function (this: unknown) {
    return estimatedBBox(this)
  }
  if (w.SVGSVGElement) w.SVGSVGElement.prototype.getBBox = override as never
  if (w.SVGGraphicsElement) w.SVGGraphicsElement.prototype.getBBox = override as never
  domShimInstalled = true
}

/** 临时注入 globalThis.window 执行回调（Bun worker_threads 与常驻 globalThis.window 冲突，仅 mermaid 导入/渲染期间存在）。 */
async function withGlobalWindow<T>(fn: () => Promise<T>): Promise<T> {
  const g = globalThis as Record<string, unknown>
  const prev = g.window
  g.window = domWindow
  try {
    return await fn()
  } finally {
    if (prev === undefined) delete g.window
    else g.window = prev
  }
}

let mermaidLoad: Promise<MermaidApi> | null = null
let mermaidSeq = 0

/** 加载 mermaid（先装垫层；浅色主题白底、纯 SVG 输出；导入期临时注入 window），失败可重试。 */
function loadMermaid(): Promise<MermaidApi> {
  if (!mermaidLoad) {
    mermaidLoad = (async () => {
      installDomShim()
      const m = await withGlobalWindow(async () => {
        const mod = (await import("mermaid")).default as MermaidApi
        mod.initialize({ startOnLoad: false, theme: "default", securityLevel: "loose", htmlLabels: false })
        return mod
      })
      return m
    })().catch((err) => {
      mermaidLoad = null
      throw err
    })
  }
  return mermaidLoad
}

/** 渲染 Mermaid 源码为 SVG（浅色主题；渲染期临时注入 window，防止部分图型运行时读取裸 window）；语法错误抛错。 */
async function renderMermaidSvg(code: string): Promise<string> {
  const m = await loadMermaid()
  const id = `gebai-bmmd-${++mermaidSeq}`
  const { svg } = await withGlobalWindow(() => m.render(id, code))
  return svg
}

/* ---------------- echarts：npm 包 SSR 渲染（JSON option → SVG，零 DOM） ---------------- */

/** echarts 默认画布尺寸（信封未指定时）。 */
export const ECHARTS_DEFAULT_WIDTH = 960
export const ECHARTS_DEFAULT_HEIGHT = 600
const ECHARTS_MIN_SIZE = 200
const ECHARTS_MAX_SIZE = 4000

/** JSON 宽松解析：严格 JSON 失败后剥掉注释与尾逗号重试（模型常产出带注释/尾逗号的对象字面量；`//` 前带 `:`（URL）不剥）。 */
function lenientJsonParse(code: string): unknown {
  try {
    return JSON.parse(code)
  } catch {
    /* 降级宽松解析 */
  }
  const stripped = code
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/(^|[^:"'\\\w])\/\/[^\n\r]*/g, "$1")
    .replace(/,(\s*[}\]])/g, "$1")
  return JSON.parse(stripped)
}

/** echarts 渲染输入：option + 画布尺寸。 */
export interface EchartsInput {
  option: Record<string, unknown>
  width: number
  height: number
}

/* ---------------- echarts 标题/图例防重叠（与前端 diagram.ts 同规则，改一处须同步另一处） ---------------- */

/** 图例与标题/绘图区的纵向间距（px）。 */
const ECHARTS_LEGEND_GAP = 6

/** 纵向定位 → 像素：数值原样、'top'→0、百分比按画布高；'middle'/'center'/'bottom' 等其余值返回 null。 */
function echartsTopPx(v: unknown, height: number): number | null {
  if (typeof v === "number" && Number.isFinite(v)) return v
  if (v === "top") return 0
  if (typeof v === "string" && /^-?[\d.]+%$/.test(v)) return (height * parseFloat(v)) / 100
  return null
}

/** 上下 padding 合计（echarts padding 格式：数值 | 数组 [上,右,下,左]，少于 4 位按 CSS 展开）。 */
function echartsPadV(v: unknown, dflt: number): number {
  if (typeof v === "number" && Number.isFinite(v)) return v * 2
  if (Array.isArray(v) && v.length) {
    const n = v.map((x) => (typeof x === "number" && Number.isFinite(x) ? x : 0))
    return n.length === 1 ? n[0] * 2 : n[0] + (n[2] ?? n[0])
  }
  return dflt * 2
}

/** 标题条目的纵向占带 [top, bottom]（echarts 6 默认：top=15、padding=5、主/副标题字号 18/12、itemGap=10，行高按字号 ×1.2
 *  估算，实测默认标题带 15–46.6）。不在顶部区域（middle/bottom、隐藏、无文本）返回 null。 */
function echartsTitleBand(t: Record<string, unknown>, height: number): [number, number] | null {
  if (t.show === false || (!t.text && !t.subtext)) return null
  const top = t.top === undefined ? 15 : echartsTopPx(t.top, height)
  if (top === null) return null
  const style = (t.textStyle ?? {}) as Record<string, unknown>
  const fs = typeof style.fontSize === "number" && Number.isFinite(style.fontSize) ? style.fontSize : 18
  const lh = typeof style.lineHeight === "number" && Number.isFinite(style.lineHeight) ? style.lineHeight : fs * 1.2
  let h = echartsPadV(t.padding, 5) + lh
  if (t.subtext) {
    const sub = (t.subtextStyle ?? {}) as Record<string, unknown>
    const sfs = typeof sub.fontSize === "number" && Number.isFinite(sub.fontSize) ? sub.fontSize : 12
    h += (typeof t.itemGap === "number" && Number.isFinite(t.itemGap) ? t.itemGap : 10) + sfs * 1.2
  }
  return [top, top + h]
}

/** 图例高度估算（px）：单行 ≈ 30（padding 10 + 行高 20）；条目总宽（图标 25 + 图标文本距 10 + 文本宽 + 项间距 8，
 *  文本宽沿用全角 1em / 半角 0.6em 估算）超出画布宽按折行数每行加 28（行高 20 + 行距 8）；滚动型恒单行。
 *  条目名缺省取 series 名。 */
function echartsLegendHeight(lg: Record<string, unknown>, option: Record<string, unknown>, width: number): number {
  if (lg.type === "scroll") return 30
  let names: unknown[] = Array.isArray(lg.data) ? lg.data : []
  if (!names.length && Array.isArray(option.series)) names = option.series.map((s) => (s as Record<string, unknown> | null)?.name)
  const labels = names
    .map((d) => (typeof d === "string" ? d : ((d as Record<string, unknown> | null)?.name as unknown)))
    .filter((s): s is string => typeof s === "string" && !!s)
  if (!labels.length) return 30
  const style = (lg.textStyle ?? {}) as Record<string, unknown>
  const fs = typeof style.fontSize === "number" && Number.isFinite(style.fontSize) ? style.fontSize : 12
  const textW = (s: string): number => textWidthEstimate(s, fs)
  const total = labels.reduce((a, s) => a + 25 + 10 + textW(s) + 8, 0) + 20
  const rows = Math.max(1, Math.ceil(total / Math.max(1, width)))
  return 10 + 20 + (rows - 1) * 28
}

/**
 * 标题/图例防重叠：echarts 6 的 legend 默认在底部，但模型常按 v5 习惯显式写 `legend.top: 0/'top'/小数值`——
 * 置顶图例与顶部标题（默认占带 15–46.6）纵向冲突时必然压字。冲突时把图例下移到冲突标题底边之下；下移后图例
 * 底边越过 grid 顶边（默认 65）时联动下调未显式设置的 grid.top（显式 grid.top/height 不动）。
 * 不干预：图例 top 未设置（v6 默认底部）或非顶部区域（middle/bottom）、标题不在顶部、二者水平分居左右两侧、图例隐藏。
 * 直接修改传入 option（解析自模型 JSON，无共享引用）。
 */
export function fixEchartsLegendOverlap(option: Record<string, unknown>, width: number, height: number): void {
  const objEntries = (v: unknown): Record<string, unknown>[] =>
    (Array.isArray(v) ? v : [v]).filter((t): t is Record<string, unknown> => !!t && typeof t === "object" && !Array.isArray(t))
  const titles = objEntries(option.title)
  const legends = objEntries(option.legend)
  if (!titles.length || !legends.length) return
  const side = (o: Record<string, unknown>): "left" | "right" | "mid" =>
    o.left === "left" || o.left === 0 ? "left" : o.left === "right" || (o.left === undefined && o.right !== undefined) ? "right" : "mid"
  let legendBottomMax: number | null = null
  for (const lg of legends) {
    if (lg.show === false) continue
    const lt = echartsTopPx(lg.top, height)
    if (lt === null) continue
    const lh = echartsLegendHeight(lg, option, width)
    const ls = side(lg)
    let below = 0
    for (const t of titles) {
      const band = echartsTitleBand(t, height)
      if (!band) continue
      const ts = side(t)
      if ((ts === "left" && ls === "right") || (ts === "right" && ls === "left")) continue
      if (band[0] < lt + lh && lt < band[1] + ECHARTS_LEGEND_GAP) below = Math.max(below, band[1])
    }
    if (!below) continue
    const newTop = Math.ceil(below) + ECHARTS_LEGEND_GAP
    lg.top = newTop
    legendBottomMax = Math.max(legendBottomMax ?? 0, newTop + lh)
  }
  if (legendBottomMax === null) return
  const g = option.grid
  if (g !== undefined && (typeof g !== "object" || g === null)) return
  const grid = (g ?? {}) as Record<string, unknown>
  if (grid.top === undefined && grid.height === undefined && grid.show !== false) {
    const need = Math.ceil(legendBottomMax + ECHARTS_LEGEND_GAP)
    if (need > 65) option.grid = { ...grid, top: need }
  }
}

/**
 * 解析 echarts 源码为渲染输入：整个 JSON 对象即 option，或 `{"option": {...}, "width": 960, "height": 600}` 信封指定画布尺寸
 * （尺寸钳制 200-4000）。注入 `animation: false`（SSR 静态输出必须）；`darkMode` 缺省取参数（后端固定浅色，前端按主题传入）；
 * 标题/图例防重叠修正（见 fixEchartsLegendOverlap）。
 */
export function parseEchartsInput(code: string, dark = false): EchartsInput {
  let obj: unknown
  try {
    obj = lenientJsonParse(code)
  } catch (err) {
    throw new Error(
      `ECharts 源码必须是合法 JSON（键名与字符串一律双引号；不支持单引号/裸键名/…省略号缩写；值禁止函数，格式化用字符串模板如 "{b}: {c}"）：${err instanceof Error ? err.message : String(err)}`,
    )
  }
  if (typeof obj !== "object" || obj === null || Array.isArray(obj)) {
    throw new Error("ECharts 源码必须是 JSON 对象（option），不能是数组或标量")
  }
  const record = obj as Record<string, unknown>
  let option = record
  let width = ECHARTS_DEFAULT_WIDTH
  let height = ECHARTS_DEFAULT_HEIGHT
  if (typeof record.option === "object" && record.option !== null && !Array.isArray(record.option)) {
    option = record.option as Record<string, unknown>
    const clampSize = (v: unknown): number | undefined =>
      typeof v === "number" && Number.isFinite(v) ? Math.min(ECHARTS_MAX_SIZE, Math.max(ECHARTS_MIN_SIZE, Math.round(v))) : undefined
    width = clampSize(record.width) ?? width
    height = clampSize(record.height) ?? height
  }
  const merged: Record<string, unknown> = { ...option, animation: false, darkMode: (option.darkMode as boolean | undefined) ?? dark }
  fixEchartsLegendOverlap(merged, width, height)
  return { option: merged, width, height }
}

/** 渲染 ECharts JSON option 为 SVG（SSR 模式，浅色；白底由栅格化兜底）；option 非法抛错（供回传模型修正）。 */
async function renderEchartsSvg(code: string): Promise<string> {
  const { option, width, height } = parseEchartsInput(code)
  const chart = echarts.init(null, null, { renderer: "svg", ssr: true, width, height })
  try {
    chart.setOption(option)
    return chart.renderToSVGString()
  } finally {
    chart.dispose()
  }
}

/* ---------------- d2：@terrastruct/d2 WASM（dev import / 二进制物化） ---------------- */

type D2Api = {
  compile: (input: string, options?: { themeID?: number; noXMLTag?: boolean }) => Promise<{ diagram: unknown; renderOptions?: { themeID?: number; noXMLTag?: boolean } }>
  render: (diagram: unknown, options?: { themeID?: number; noXMLTag?: boolean }) => Promise<string>
}

/** D2 主题 ID：后端固定浅色主题（0=Neutral Default，白底图适合飞书/导出场景）。 */
const D2_THEME_LIGHT = 0

let d2Load: Promise<D2Api> | null = null

/** 加载 D2 引擎：dev 直接 import 包；二进制模式物化内嵌产物后 import（失败可重试）。 */
function loadD2(): Promise<D2Api> {
  if (!d2Load) {
    d2Load = (async () => {
      const mod = isBinaryMode() ? await importBinaryD2() : await import("@terrastruct/d2")
      return new (mod as { D2: new () => D2Api }).D2()
    })().catch((err) => {
      d2Load = null
      throw err
    })
  }
  return d2Load
}

/** 内嵌产物（构建脚本 scripts/build-d2js.ts 生成的 JSON；动态 import 字面量路径随产物打进二进制）。 */
type D2Embedded = { version: string; files: Record<string, string> }

let d2EmbeddedLoad: Promise<D2Embedded> | null = null

function loadD2Embedded(): Promise<D2Embedded> {
  if (!d2EmbeddedLoad) {
    d2EmbeddedLoad = import("./d2js.embedded.generated.json")
      .then((m) => m.default as D2Embedded)
      .catch((err) => {
        d2EmbeddedLoad = null
        throw new Error(`D2 后端渲染不可用：内嵌 D2.js 产物缺失或损坏（请先运行 bun run scripts/build-d2js.ts；${err instanceof Error ? err.message : String(err)}）`)
      })
  }
  return d2EmbeddedLoad
}

/** 内嵌产物目录（`{GEBAI_HOME}/vendor/d2js/{version}/`）；版本变更自动换目录（旧版本清理）。 */
function embeddedD2Dir(embedded: D2Embedded): string {
  return join(resolveGebaiHome(), "vendor", "d2js", embedded.version)
}

/** 物化内嵌 D2.js 文件到磁盘（幂等：目录已存在即跳过；gzip 还原后写入；旧版本目录清理）。 */
export function materializeD2Files(dir: string, embedded: D2Embedded): void {
  const versionFile = join(dir, ".version")
  if (existsSync(versionFile)) return
  mkdirSync(dir, { recursive: true })
  for (const [name, b64] of Object.entries(embedded.files)) {
    writeFileSync(join(dir, name), Bun.gunzipSync(Buffer.from(b64, "base64")))
  }
  writeFileSync(versionFile, embedded.version)
  const parent = dirname(dir)
  for (const e of readdirSync(parent, { withFileTypes: true })) {
    if (e.isDirectory() && e.name !== embedded.version) {
      rmSync(join(parent, e.name), { recursive: true, force: true })
    }
  }
}

/** 二进制模式：物化后动态 import D2.js（文件路径 Worker 需要真实磁盘文件）。 */
async function importBinaryD2(): Promise<unknown> {
  const embedded = await loadD2Embedded()
  const dir = embeddedD2Dir(embedded)
  materializeD2Files(dir, embedded)
  const mod = await import(pathToFileURL(join(dir, "index.js")).href)
  return mod
}

/** 渲染 D2 源码为 SVG（浅色主题）；编译/渲染错误抛错。 */
async function renderD2Svg(code: string): Promise<string> {
  const d2 = await loadD2()
  const opts = { themeID: D2_THEME_LIGHT, noXMLTag: true }
  const compiled = await d2.compile(code, opts)
  return d2.render(compiled.diagram, compiled.renderOptions ?? opts)
}

/* ---------------- 组合渲染器 ---------------- */

/** 渲染串行队列（全局单一）：三种语言共享全局 DOM/worker 环境（happy-dom 与 PlantUML 垫层相互覆盖全局、
 *  d2 单 Worker），必须整体串行防并发冲突。 */
let renderQueue: Promise<unknown> = Promise.resolve()
function enqueue<T>(task: () => Promise<T>): Promise<T> {
  const run = renderQueue.then(task, task)
  renderQueue = run.catch(() => {})
  return run
}

/** @terrastruct/d2 编译错误为 JSON 数组字符串（[{range,errmsg}]）：提取可读 errmsg（第 N 行），避免原始 JSON 回传模型。 */
export function formatD2Error(msg: string): string {
  if (msg.startsWith("[")) {
    try {
      const arr = JSON.parse(msg) as Array<{ errmsg?: string }>
      const lines = arr.map((e) => e.errmsg).filter((s): s is string => !!s)
      if (lines.length) return `D2 语法错误：${lines.join("；")}`
    } catch {
      /* 非 JSON 保持原样 */
    }
  }
  return msg
}

/** 错误信息归一（截断 + 语言前缀，供回传模型；D2 错误 JSON 转可读文本）。 */
function wrapError(format: DiagramFormat, err: unknown): Error {
  const label = { plantuml: "PlantUML", mermaid: "Mermaid", d2: "D2", echarts: "ECharts" }[format]
  let msg = err instanceof Error ? err.message : String(err)
  if (format === "d2") msg = formatD2Error(msg)
  return new Error(`${label} 渲染错误：${msg.slice(0, 200)}`)
}

export interface DiagramRendererDeps {
  /** PlantUML 渲染器（测试注入 fake；缺省懒加载 feishu-bot/plantuml.ts 实现）。 */
  plantuml?: () => Promise<DiagramRenderer>
  /** Mermaid 渲染（测试注入 fake；缺省本地引擎）。 */
  mermaid?: (code: string) => Promise<string>
  /** D2 渲染（测试注入 fake；缺省本地引擎）。 */
  d2?: (code: string) => Promise<string>
  /** ECharts 渲染（测试注入 fake；缺省本地引擎）。 */
  echarts?: (code: string) => Promise<string>
  /** SVG → PNG 栅格化（测试注入 fake；缺省 @resvg/resvg-js）。 */
  rasterize?: (svg: string, opts: RasterizeOpts) => Promise<Uint8Array>
}

export function createDiagramRenderer(deps: DiagramRendererDeps = {}): DiagramRenderer {
  const mermaidRender = deps.mermaid ?? renderMermaidSvg
  const d2Render = deps.d2 ?? renderD2Svg
  const echartsRender = deps.echarts ?? renderEchartsSvg
  const rasterize = deps.rasterize ?? svgRasterize
  const plantumlRender = deps.plantuml ?? (async () => {
    const { createPlantUmlRenderer } = await import("../feishu-bot/plantuml")
    return createPlantUmlRenderer()
  })

  return {
    async renderPng(code, opts = {}) {
      const format: DiagramFormat = opts.format ?? "plantuml"
      const rasterOpts: RasterizeOpts = { background: opts.background ?? "#ffffff", maxWidth: opts.maxWidth, maxHeight: opts.maxHeight }
      return enqueue(async () => {
        try {
          if (format === "echarts") {
            // SSR 渲染零 DOM，与 mermaid/d2 的垫层互不影响（echarts 已在模块求值期以 Node 环境加载）
            return await rasterize(await echartsRender(code), rasterOpts)
          }
          if (format === "mermaid") {
            // PlantUML 垫层会覆盖全局 document/XMLSerializer：mermaid 渲染前强制还原 happy-dom 环境
            installDomShim(true)
            return await rasterize(await mermaidRender(code), rasterOpts)
          }
          if (format === "d2") {
            // PlantUML 垫层设置了 globalThis.window（与 Bun worker_threads 冲突）：d2 渲染前移除
            if ("window" in (globalThis as Record<string, unknown>)) delete (globalThis as Record<string, unknown>).window
            return await rasterize(await d2Render(code), rasterOpts)
          }
          // plantuml：重新应用其 DOM 垫层（happy-dom 垫层可能已覆盖全局），渲染后移除 window 防破坏 d2 worker 启动
          const { applyPlantUmlDomShim } = await import("../feishu-bot/plantuml-dom-shim")
          applyPlantUmlDomShim()
          try {
            return await (await plantumlRender()).renderPng(code, rasterOpts)
          } finally {
            if ("window" in (globalThis as Record<string, unknown>)) delete (globalThis as Record<string, unknown>).window
          }
        } catch (err) {
          throw wrapError(format, err)
        }
      })
    },
  }
}
