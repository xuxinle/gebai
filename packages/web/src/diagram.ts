import type { ContentBlock, DiagramFormat } from "@gebai/sdk"
import DOMPurify from "dompurify"
import { el } from "./state"
import { copyText, toast, tip } from "./ui"
import { isLowPower } from "./low-power"
import { cssVarToHex } from "./css-color"
import { injectPlantUmlLayout } from "./plantuml-layout"

/* ---------- 图表渲染：四种图表语言本地引擎（PlantUML @plantuml/core / Mermaid mermaid / D2 @terrastruct/d2 / ECharts echarts），配色跟随 UI 主题 ---------- */

/** 图表语言 → 产物文件扩展名（与 draw 工具落盘约定一致）。 */
export const DIAGRAM_EXT_FOR: Record<DiagramFormat, string> = { plantuml: "puml", mermaid: "mmd", d2: "d2", echarts: "echarts" }
/** 图表语言展示名（源码查看器/导出用）。 */
export const DIAGRAM_LABEL: Record<DiagramFormat, string> = { plantuml: "PlantUML", mermaid: "Mermaid", d2: "D2", echarts: "ECharts" }

/** 读取 CSS 变量为不透明 hex（rgba 与 body 背景合成，见 css-color.ts）。 */
function cssVar(name: string, fallback: string): string {
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim()
  return cssVarToHex(v, getComputedStyle(document.body).backgroundColor, fallback)
}

/** 计算 #rrggbb 的感知亮度（0-255）。cssVar 已保证返回 6 位 hex。 */
function hexLuminance(hex: string): number {
  const m = hex.replace("#", "")
  const r = parseInt(m.slice(0, 2), 16)
  const g = parseInt(m.slice(2, 4), 16)
  const b = parseInt(m.slice(4, 6), 16)
  return 0.299 * r + 0.587 * g + 0.114 * b
}

/** 按当前主题 CSS 变量生成 PlantUML skinparam（仅引擎确认支持的项；其余元素默认色由 fixThemeColors 渲染后兜底修正）。 */
function plantUmlThemeSkin(): string {
  const bg = cssVar("--bg-inset", "#0d1117")
  const text = cssVar("--text", "#e6e6e6")
  const muted = cssVar("--text-muted", "#8a93a8")
  const node = cssVar("--bg-hover", "#232a3d")
  const border = cssVar("--border-strong", "#35405c")
  const dark = hexLuminance(text) >= 128
  const lines = [
    "skinparam backgroundColor " + bg,
    "skinparam shadowing " + (dark ? "false" : "true"),
    "skinparam defaultFontColor " + text,
    "skinparam ArrowColor " + border,
    "skinparam classBackgroundColor " + node,
    "skinparam classBorderColor " + border,
    "skinparam classFontColor " + text,
    "skinparam classAttributeFontColor " + text,
    "skinparam classHeaderBackgroundColor " + node,
    "skinparam objectBackgroundColor " + node,
    "skinparam objectBorderColor " + border,
    "skinparam packageBackgroundColor " + node,
    "skinparam packageBorderColor " + border,
    "skinparam stateBackgroundColor " + node,
    "skinparam stateBorderColor " + border,
    "skinparam noteBackgroundColor " + node,
    "skinparam noteBorderColor " + border,
    "skinparam activityBackgroundColor " + node,
    "skinparam activityBorderColor " + border,
    "skinparam activityDiamondBackgroundColor " + node,
    "skinparam activityDiamondBorderColor " + border,
    "skinparam legendBackgroundColor " + node,
    "skinparam legendBorderColor " + border,
    "skinparam titleBackgroundColor " + node,
    "skinparam titleBorderColor " + border,
    "skinparam StereotypeFontColor " + text,
    "skinparam SequenceBoxBackgroundColor " + node,
    "skinparam SequenceBoxBorderColor " + border,
    "skinparam SequenceGroupBackgroundColor " + node,
    "skinparam SequenceGroupBorderColor " + border,
    "skinparam SequenceLifeLineBackgroundColor " + node,
    "skinparam SequenceLifeLineBorderColor " + border,
    "skinparam SequenceReferenceBackgroundColor " + node,
    "skinparam SequenceReferenceBorderColor " + border,
    "skinparam sequenceDividerBackgroundColor " + node,
    "skinparam sequenceDividerBorderColor " + border,
    "skinparam sequenceReferenceHeaderBackgroundColor " + node,
    "skinparam SwimlaneTitleBackgroundColor " + node,
    "skinparam SwimlaneBorderColor " + border,
    "skinparam PartitionBackgroundColor " + node,
    "skinparam PartitionBorderColor " + border,
    "skinparam participantClickableBackgroundColor " + node,
    "skinparam dividerBackgroundColor " + node,
    "skinparam dividerFontColor " + muted,
    "skinparam defaultTextAlignment center",
  ]
  return lines.join("\n")
}

/** 各类图块的 end 指令（与 @start 对应；服务端已补全，此处兜底编辑态源码）。 */
const START_END: Record<string, string> = {
  uml: "@enduml",
  mindmap: "@endmindmap",
  wbs: "@endwbs",
  gantt: "@endgantt",
  salt: "@endsalt",
  json: "@endjson",
  yaml: "@endyaml",
}

/** 原样数据块（内容必须是严格 JSON/YAML，注入任何指令都会破坏数据解析）：不注入主题 skinparam（颜色由 fixThemeColors 兜底）。 */
const RAW_DATA_BLOCKS = ["json", "yaml"]

/** 源码规范化（兜底）：无 @start 指令时自动补全 @startuml/@enduml；布局默认参数与主题 skinparam 追加在末尾（end 指令前）——最后应用，覆盖用户 !theme/skinparam，保证跟随 UI 主题。 */
function themedPlantUmlCode(code: string): string {
  const m = code.match(/@start(\w*)/)
  let body = code
  if (m) {
    const end = START_END[m[1]]
    if (end && !body.includes(end)) body = `${body.trimEnd()}\n${end}`
    if (RAW_DATA_BLOCKS.includes(m[1])) return body // 数据块原样返回（仅补全缺失的 @end）
    body = injectPlantUmlLayout(body)
  } else {
    body = injectPlantUmlLayout(`@startuml\n${code.trimEnd()}\n@enduml`)
  }
  const endIdx = body.search(/@end\w*/)
  const skin = plantUmlThemeSkin()
  if (endIdx >= 0) return `${body.slice(0, endIdx)}${skin}\n${body.slice(endIdx)}`
  return `${body}\n${skin}`
}

/** SVG 缓存（key 含主题 skinparam，主题切换后失效）。 */
const svgCache = new Map<string, Promise<string>>()

/** @plantuml/core 渲染 API（TeaVM 编译的官方 PlantUML 引擎，本地渲染无网络请求）。 */
type PlantUmlApi = {
  renderToString: (lines: string[], onSuccess: (svg: string) => void, onError: (msg: string) => void) => void
}

let plantUmlPromise: Promise<PlantUmlApi> | null = null

/** 加载本地渲染引擎：先注入 viz-global.js（Graphviz 布局，classic script），再动态加载 plantuml.js。失败/挂起可重试。 */
function loadPlantUml(): Promise<PlantUmlApi> {
  if (!plantUmlPromise) {
    plantUmlPromise = (async () => {
      await withTimeout(loadVizGlobal(), 8000, "本地渲染引擎加载超时（viz-global.js）")
      const mod: any = await withTimeout(import(/* @vite-ignore */ `${import.meta.env.BASE_URL}vendor/plantuml.js`), 10000, "本地渲染引擎加载超时（plantuml.js）")
      if (typeof mod.renderToString !== "function") throw new Error("本地 PlantUML 引擎加载失败")
      return mod as PlantUmlApi
    })().catch((err) => {
      plantUmlPromise = null // 失败重置，允许下次重试
      throw err
    })
  }
  return plantUmlPromise
}

/** 带超时的 Promise（挂起时抛错而非永久等待）。 */
function withTimeout<T>(p: Promise<T>, ms: number, msg: string): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(msg)), ms)
    p.then(
      (v) => {
        clearTimeout(timer)
        resolve(v)
      },
      (e) => {
        clearTimeout(timer)
        reject(e)
      },
    )
  })
}

let vizReady: Promise<void> | null = null

/** viz-global.js（Viz.js/Graphviz）必须以 classic script 注入全局，供 plantuml.js 使用。 */
function loadVizGlobal(): Promise<void> {
  if (!vizReady) {
    vizReady = loadVendorScript(`${import.meta.env.BASE_URL}vendor/viz-global.js`, "本地渲染引擎加载失败（viz-global.js）").catch((err) => {
      vizReady = null // 失败重置，允许下次重试
      throw err
    })
  }
  return vizReady
}

/** classic script 注入 public/vendor/ 静态脚本（稳定文件名，无内容 hash——重建后 URL 不变，动态加载资源 404 从根上消除）。 */
function loadVendorScript(url: string, errMsg: string): Promise<void> {
  return new Promise<void>((resolve, reject) => {
    const script = document.createElement("script")
    script.src = url
    script.onload = () => resolve()
    script.onerror = () => {
      script.remove()
      reject(new Error(errMsg))
    }
    document.head.appendChild(script)
  })
}

/** 动态导入 vendor 静态模块：失败（瞬时网络错误等）带时间戳缓存穿透重试一次。 */
async function importVendor(url: string): Promise<Record<string, unknown>> {
  try {
    return (await import(/* @vite-ignore */ url)) as Record<string, unknown>
  } catch {
    return (await import(/* @vite-ignore */ `${url}?t=${Date.now()}`)) as Record<string, unknown>
  }
}

/** 渲染 PlantUML 源码为 SVG；失败时抛出带原因的错误（供回传给模型）。 */
export async function renderPlantUmlSvg(code: string): Promise<string> {
  const full = themedPlantUmlCode(code)
  let p = svgCache.get(full)
  if (!p) {
    p = renderLocal(full)
    svgCache.set(full, p)
  }
  try {
    return await p
  } catch (err) {
    svgCache.delete(full) // 失败不缓存，允许重试
    throw err
  }
}

/** 渲染串行队列：@plantuml/core 的 renderToString 不支持并发（并发调用会互相覆盖状态导致回调丢失），必须一次渲染一个。 */
let renderQueue: Promise<unknown> = Promise.resolve()
function enqueueRender<T>(task: () => Promise<T>): Promise<T> {
  const run = renderQueue.then(task, task)
  renderQueue = run.catch(() => {}) // 单个失败不阻塞后续
  return run
}

/** 引擎硬编码的默认色 → 主题色（TeaVM 版仅支持少量 skinparam，其余元素默认色在此渲染后兜底修正）。 */
function fixThemeColors(svg: string): string {
  const bg = cssVar("--bg-inset", "#0d1117")
  const node = cssVar("--bg-hover", "#232a3d")
  const border = cssVar("--border-strong", "#35405c")
  const text = cssVar("--text", "#e6e6e6")
  // 引擎默认亮色（节点/激活条/分组/note 背景等）→ fill 用节点色、stroke 用边框色（(?![0-9A-Fa-f]) 防止误伤 8 位透明色如 #00000000）
  const light = "F1F1F1|FFFFFF|E2E2F0|EEEEEE|FEFFDD|ADD1B2|FEFECE|FFFF44|FBFB77|FFE5E5|E8E8FF|EB937F|E3664A|FEF6F3"
  let out = svg.replace(new RegExp(`fill="#(?:${light})(?![0-9A-Fa-f])"`, "gi"), `fill="${node}"`)
  out = out.replace(new RegExp(`stroke="#(?:${light})(?![0-9A-Fa-f])"`, "gi"), `stroke="${border}"`)
  // 引擎默认深灰画布/描边 → 画布色/边框色
  out = out.replace(/fill="#222222(?![0-9A-Fa-f])"/gi, `fill="${bg}"`)
  out = out.replace(/stroke="#222222(?![0-9A-Fa-f])"/gi, `stroke="${border}"`)
  // 引擎默认暗色（文字/实心点/描边）→ fill 用文字色（暗色主题下可见）、stroke 用边框色
  out = out.replace(/fill="#(?:000000|181818|676767)(?![0-9A-Fa-f])"/gi, `fill="${text}"`)
  out = out.replace(/stroke="#(?:000000|181818|676767)(?![0-9A-Fa-f])"/gi, `stroke="${border}"`)
  return out
}

async function renderLocal(full: string): Promise<string> {
  const api = await loadPlantUml()
  const svg = await enqueueRender(() =>
    withTimeout(
      new Promise<string>((resolve, reject) => {
        api.renderToString(full.split("\n"), resolve, (msg) => reject(new Error(`PlantUML 渲染错误：${msg}`)))
      }),
      20000,
      "PlantUML 渲染超时（图表过大或引擎繁忙）",
    ),
  )
  // TeaVM 引擎对语法错误/不支持的图型（ditaa/salt/nwdiag 等）不回调 onError，而是输出含错误文本的 SVG：显式检测并提取
  // （错误页特征文本：Syntax Error / Parse error / Fatal parsing error / not supported / 错误统计 / 调试横幅 From textarea）
  if (/Syntax Error|Parse error|Fatal parsing error|not supported|Some diagram description contains errors|From textarea/i.test(svg)) {
    const lines = svg.match(/<text[^>]*>([^<]*)<\/text>/g)?.map((t) => t.replace(/<[^>]+>/g, "").trim()).filter(Boolean) ?? []
    const detail = lines.find((l) => /error|line \d|not supported/i.test(l)) ?? lines[lines.length - 1] ?? "请检查源码"
    throw new Error(`PlantUML 渲染错误：${detail.slice(0, 160)}`)
  }
  // PlantUML 支持 <html>/<img> 嵌入（creole），渲染结果可能携带任意 HTML：注入 DOM 前净化（移除 script/事件属性等）
  const clean = DOMPurify.sanitize(svg, { USE_PROFILES: { svg: true, svgFilters: true } })
  if (!clean) throw new Error("PlantUML 渲染结果安全校验失败")
  return fixThemeColors(clean)
}

/* ---------- Mermaid 渲染：mermaid npm 包本地引擎（SVG 输出），主题按当前 UI 明暗初始化 ---------- */

/** 根 <svg> 显式化逻辑尺寸（width/height 属性取自 viewBox）：mermaid（width="100%" + 内联 max-width）与 d2（无宽高属性）
 *  的 SVG 在查看器的 flex 容器内解析为 0×0（CSS width:auto 依赖容器宽度，与内容宽度循环引用），显式化后查看器测量/缩放与 PNG 导出正常。 */
function explicitSvgSize(svg: string): string {
  const m = svg.match(/<svg([^>]*)>/)
  if (!m) return svg
  const attrs = m[1]
  const vb = attrs.match(/viewBox="([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)"/)
  if (!vb) return svg
  const w = Number(vb[3])
  const h = Number(vb[4])
  if (!(w > 0) || !(h > 0)) return svg
  const rest = attrs.replace(/\s+(?:width|height)="[^"]*"/g, "").replace(/\sviewBox="[^"]*"/, "")
  return `<svg viewBox="${vb[1]} ${vb[2]} ${vb[3]} ${vb[4]}" width="${w}" height="${h}"${rest}>${svg.slice(m[0].length)}`
}

/** 当前 UI 是否为暗色主题（按文字色感知亮度判定，与 PlantUML 主题皮肤同一规则）。 */
function isDarkTheme(): boolean {
  return hexLuminance(cssVar("--text", "#e6e6e6")) >= 128
}

type MermaidApi = { initialize: (cfg: Record<string, unknown>) => void; render: (id: string, code: string) => Promise<{ svg: string }> }

let mermaidLoadPromise: Promise<MermaidApi> | null = null

/** 加载 mermaid 本地渲染引擎（vendor/mermaid.js 自包含 UMD，classic script 注入 globalThis.mermaid；
 *  懒加载，失败/挂起可重试；模块体积大，慢机器加载可超 15s，超时放宽至 30s）。 */
function loadMermaid(): Promise<MermaidApi> {
  if (!mermaidLoadPromise) {
    mermaidLoadPromise = (async () => {
      await withTimeout(loadVendorScript(`${import.meta.env.BASE_URL}vendor/mermaid.js`, "本地渲染引擎加载失败（mermaid.js）"), 30000, "本地渲染引擎加载超时（mermaid.js）")
      const m = (globalThis as { mermaid?: MermaidApi }).mermaid
      if (typeof m?.render !== "function") throw new Error("本地 Mermaid 引擎加载失败")
      return m
    })().catch((err) => {
      mermaidLoadPromise = null
      throw err
    })
  }
  return mermaidLoadPromise
}

/** 已初始化主题 key（明暗切换时重新 initialize——mermaid 主题为全局状态，配色随 UI 主题）。 */
let mermaidThemeKey = ""

async function ensureMermaidTheme(): Promise<MermaidApi> {
  const m = await loadMermaid()
  const dark = isDarkTheme()
  const key = dark ? "dark" : "light"
  if (mermaidThemeKey !== key) {
    m.initialize({
      startOnLoad: false,
      theme: dark ? "dark" : "default",
      securityLevel: "strict",
      // 渲染失败默认在 document.body 遗留「错误图标」容器（Syntax error in text，无法关闭）：
      // 抑制内置错误渲染（渲染期临时元素一并清理），错误仅经 Promise 拒绝抛出，
      // 由调用方统一处理（缩略图占位/查看器回退/回传模型修正源码）
      suppressErrorRendering: true,
      // 纯 SVG 文本标签：htmlLabels=true 时标签渲染为 <span>/<div> 等 HTML 元素，
      // 会被注入 DOM 前的 DOMPurify SVG 净化剥离，导致「有框无文字」
      htmlLabels: false,
      themeVariables: dark
        ? { background: cssVar("--bg-inset", "#0d1117"), primaryTextColor: cssVar("--text", "#e6e6e6") }
        : { background: cssVar("--bg-inset", "#ffffff"), primaryTextColor: cssVar("--text", "#333333") },
    })
    mermaidThemeKey = key
  }
  return m
}

/** mermaid.render 需要唯一 id（DOM 内不重复）。 */
let mermaidSeq = 0

/** Mermaid 渲染缓存（key 含主题，主题切换后失效重建）。 */
const mermaidCache = new Map<string, Promise<string>>()

/** 渲染 Mermaid 源码为 SVG；语法错误抛错（供回传给模型）。 */
async function renderMermaidSvg(code: string): Promise<string> {
  const key = `${isDarkTheme() ? "dark" : "light"}:${code}`
  let p = mermaidCache.get(key)
  if (!p) {
    p = (async () => {
      const m = await ensureMermaidTheme()
      const id = `gebai-mmd-${++mermaidSeq}`
      const { svg } = await withTimeout(m.render(id, code), 20000, "Mermaid 渲染超时（图表过大或引擎繁忙）")
      // 渲染结果可能携带任意 HTML：注入 DOM 前净化（securityLevel=strict 之外的双重防线）
      const clean = DOMPurify.sanitize(svg, { USE_PROFILES: { svg: true, svgFilters: true } })
      if (!clean) throw new Error("Mermaid 渲染结果安全校验失败")
      return explicitSvgSize(clean)
    })()
    mermaidCache.set(key, p)
  }
  try {
    return await p
  } catch (err) {
    mermaidCache.delete(key) // 失败不缓存，允许重试
    throw err
  }
}

/* ---------- D2 渲染：@terrastruct/d2 官方 WASM 本地引擎（自包含单文件，零网络），暗色主题 ID 200 ---------- */

/** D2 主题 ID：0=Neutral Default（亮色默认）、200=Dark Mauve（暗色，官方默认暗色主题）。 */
const D2_THEME_LIGHT = 0
const D2_THEME_DARK = 200

type D2Api = {
  compile: (input: string, options?: { themeID?: number; noXMLTag?: boolean }) => Promise<{ diagram: unknown; renderOptions?: { themeID?: number; noXMLTag?: boolean } }>
  render: (diagram: unknown, options?: { themeID?: number; noXMLTag?: boolean }) => Promise<string>
}

let d2LoadPromise: Promise<D2Api> | null = null

/** 加载 D2 本地渲染引擎（vendor/d2js/ 官方浏览器构建，8MB 自包含 WASM，懒加载；WASM 大，
 *  慢机器加载可超 15s，超时放宽至 30s；失败/挂起可重试）。 */
function loadD2(): Promise<D2Api> {
  if (!d2LoadPromise) {
    d2LoadPromise = withTimeout(
      importVendor(`${import.meta.env.BASE_URL}vendor/d2js/index.js`).then((mod) => new (mod as { D2: new () => D2Api }).D2()),
      30000,
      "本地渲染引擎加载超时（@terrastruct/d2）",
    ).catch((err) => {
      d2LoadPromise = null
      throw err
    })
  }
  return d2LoadPromise
}

/** D2 渲染缓存（key 含主题，主题切换后失效重建）。 */
const d2Cache = new Map<string, Promise<string>>()

/** @terrastruct/d2 编译错误为 JSON 数组字符串（[{range,errmsg}]）：提取可读 errmsg，避免把原始 JSON 暴露给主页面/回传模型。 */
function formatD2Error(msg: string): string {
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

/** D2 渲染串行队列：@terrastruct/d2 浏览器构建为单 Worker，主线程共享 currentResolve/currentReject，
 *  并发调用互相覆盖导致前序调用挂起直到超时（多图同时渲染/渲染与 draw 工具渲染并发时大量「编译超时」），必须一次一个。 */
let d2Queue: Promise<unknown> = Promise.resolve()
function enqueueD2<T>(task: () => Promise<T>): Promise<T> {
  const run = d2Queue.then(task, task)
  d2Queue = run.catch(() => {}) // 单个失败不阻塞后续
  return run
}

/** 渲染 D2 源码为 SVG（主题 ID 按当前 UI 明暗选择）；编译错误抛错（可读信息，供回传给模型）。 */
async function renderD2Svg(code: string): Promise<string> {
  const dark = isDarkTheme()
  const key = `${dark ? "dark" : "light"}:${code}`
  let p = d2Cache.get(key)
  if (!p) {
    p = enqueueD2(async () => {
      const d2 = await loadD2()
      const opts = { themeID: dark ? D2_THEME_DARK : D2_THEME_LIGHT, noXMLTag: true }
      let compiled: { diagram: unknown; renderOptions?: { themeID?: number; noXMLTag?: boolean } }
      try {
        compiled = await withTimeout(d2.compile(code, opts), 20000, "D2 编译超时（图表过大或引擎繁忙）")
      } catch (err) {
        throw new Error(formatD2Error((err as Error).message))
      }
      const svg = await withTimeout(d2.render(compiled.diagram, compiled.renderOptions ?? opts), 20000, "D2 渲染超时（图表过大或引擎繁忙）")
      const clean = DOMPurify.sanitize(svg, { USE_PROFILES: { svg: true, svgFilters: true } })
      if (!clean) throw new Error("D2 渲染结果安全校验失败")
      return explicitSvgSize(clean)
    })
    d2Cache.set(key, p)
  }
  try {
    return await p
  } catch (err) {
    d2Cache.delete(key) // 失败不缓存，允许重试
    throw err
  }
}

/* ---------- ECharts 渲染：echarts 官方 UMD（vendor/echarts.js）SSR 模式直接输出 SVG 字符串，JSON option → SVG 纯计算 ---------- */

type EchartsChart = { setOption: (o: Record<string, unknown>) => void; renderToSVGString: () => string; dispose: () => void }
type EchartsApi = { init: (el: null, theme: null, opts: { renderer: "svg"; ssr: boolean; width: number; height: number }) => EchartsChart }

let echartsLoadPromise: Promise<EchartsApi> | null = null

/** 加载 ECharts 本地渲染引擎（vendor/echarts.js 自包含 UMD，classic script 注入 globalThis.echarts，懒加载，失败/挂起可重试）。 */
function loadEcharts(): Promise<EchartsApi> {
  if (!echartsLoadPromise) {
    echartsLoadPromise = (async () => {
      await withTimeout(loadVendorScript(`${import.meta.env.BASE_URL}vendor/echarts.js`, "本地渲染引擎加载失败（echarts.js）"), 15000, "本地渲染引擎加载超时（echarts.js）")
      const m = (globalThis as { echarts?: EchartsApi }).echarts
      if (typeof m?.init !== "function") throw new Error("本地 ECharts 引擎加载失败")
      return m
    })().catch((err) => {
      echartsLoadPromise = null
      throw err
    })
  }
  return echartsLoadPromise
}

/** JSON 宽松解析：严格 JSON 失败后剥掉注释与尾逗号重试（`//` 前带 `:`（URL）不剥）。 */
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

/** 解析 ECharts 源码（JSON option 或 {"option":…,"width":…,"height":…} 信封）：注入 animation:false 与 darkMode（按当前 UI 明暗）。 */
function parseEchartsOption(code: string, dark: boolean): { option: Record<string, unknown>; width: number; height: number } {
  let obj: unknown
  try {
    obj = lenientJsonParse(code)
  } catch (err) {
    throw new Error(
      `ECharts 源码必须是合法 JSON（键名与字符串一律双引号；不支持单引号/裸键名/…省略号缩写；值禁止函数，格式化用字符串模板如 "{b}: {c}"）：${err instanceof Error ? err.message : String(err)}`,
    )
  }
  if (typeof obj !== "object" || obj === null || Array.isArray(obj)) throw new Error("ECharts 源码必须是 JSON 对象（option）")
  const record = obj as Record<string, unknown>
  let option = record
  let width = 960
  let height = 600
  if (typeof record.option === "object" && record.option !== null && !Array.isArray(record.option)) {
    option = record.option as Record<string, unknown>
    const clampSize = (v: unknown): number | undefined =>
      typeof v === "number" && Number.isFinite(v) ? Math.min(4000, Math.max(200, Math.round(v))) : undefined
    width = clampSize(record.width) ?? width
    height = clampSize(record.height) ?? height
  }
  return {
    option: { ...option, animation: false, darkMode: (option.darkMode as boolean | undefined) ?? dark },
    width,
    height,
  }
}

/** ECharts 渲染缓存（key 含主题明暗，主题切换后失效）。 */
const echartsCache = new Map<string, Promise<string>>()

/** 渲染 ECharts JSON option 为 SVG（SSR 模式，零 DOM 挂载）；option 非法抛错（供回传模型修正）。 */
async function renderEchartsSvg(code: string): Promise<string> {
  const dark = isDarkTheme()
  const key = `${dark ? "dark" : "light"}:${code}`
  let p = echartsCache.get(key)
  if (!p) {
    p = (async () => {
      const echarts = await loadEcharts()
      const { option, width, height } = parseEchartsOption(code, dark)
      const chart = echarts.init(null, null, { renderer: "svg", ssr: true, width, height })
      try {
        chart.setOption(option)
        const svg = chart.renderToSVGString()
        // 输出含文本/图形等用户数据：注入 DOM 前净化（与其余图表引擎同一防线）
        const clean = DOMPurify.sanitize(svg, { USE_PROFILES: { svg: true, svgFilters: true } })
        if (!clean) throw new Error("ECharts 渲染结果安全校验失败")
        return clean
      } finally {
        chart.dispose()
      }
    })()
    echartsCache.set(key, p)
  }
  try {
    return await p
  } catch (err) {
    echartsCache.delete(key) // 失败不缓存，允许重试
    throw err
  }
}

/** 动态加载失败兜底（最后一层防线）：引擎已全部改为 public/vendor/ 稳定文件名静态伺服，
 *  重建后 URL 不变、hash 分块 404 已从根上消除；此处兜底理论上仅极端场景（vendor 文件缺失/网络异常）
 *  触发——整页刷新一次恢复。 */
let staleChunkReloaded = false
function handleStaleChunk(err: unknown): void {
  if (staleChunkReloaded) return
  if (typeof err === "object" && err !== null && /Failed to fetch dynamically imported module/.test(String((err as { message?: unknown }).message ?? ""))) {
    staleChunkReloaded = true
    setTimeout(() => location.reload(), 300)
  }
}

/** 按图表语言分派渲染（draw 工具 event.draw.render 与内容块展示共用）；未知语言显式报错引导换通道，
 *  不静默回退 PlantUML——服务端新增图表语言而前端为旧版本时，回退会把源码当 PlantUML 渲染出误导性错误。 */
export function renderDiagramSvg(format: DiagramFormat, code: string): Promise<string> {
  let p: Promise<string>
  switch (format) {
    case "mermaid":
      p = renderMermaidSvg(code)
      break
    case "d2":
      p = renderD2Svg(code)
      break
    case "echarts":
      p = renderEchartsSvg(code)
      break
    case "plantuml":
      p = renderPlantUmlSvg(code)
      break
    default:
      p = Promise.reject(new Error(`前端暂不支持图表语言「${String(format)}」（前端版本较旧或语言未识别），请改用 render=backend 渲染`))
  }
  p.catch(handleStaleChunk)
  return p
}

function showFallback(canvas: HTMLElement, code: string, message: string): void {
  canvas.appendChild(el("div", "diagram-error", message))
  const pre = el("pre")
  pre.textContent = code
  canvas.appendChild(pre)
}

/* ---------- 查看/下载源码与渲染图片 ---------- */

/** 工具栏图标（16px 线性 SVG，跟随 currentColor）。 */
const ICON_FIT = '<svg viewBox="0 0 16 16"><path d="M3 3h3.5M3 3v3.5M13 3H9.5M13 3v3.5M3 13h3.5M3 13V9.5M13 13H9.5M13 13V9.5"/></svg>'
const ICON_MINUS = '<svg viewBox="0 0 16 16"><path d="M3 8h10"/></svg>'
const ICON_PLUS = '<svg viewBox="0 0 16 16"><path d="M3 8h10M8 3v10"/></svg>'
const ICON_DOWNLOAD = '<svg viewBox="0 0 16 16"><path d="M8 2v8m0 0L4.5 6.5M8 10l3.5-3.5M3 13h10"/></svg>'
const ICON_COPY = '<svg viewBox="0 0 16 16"><rect x="4.5" y="4.5" width="8" height="8" rx="1.5"/><path d="M11.5 3.5h-6a1.5 1.5 0 0 0-1.5 1.5v6"/></svg>'
const ICON_SOURCE = '<svg viewBox="0 0 16 16"><path d="M6 3.5 2.5 8l3.5 4.5M10 3.5l3.5 4.5-3.5 4.5"/></svg>'

/** 图标按钮：SVG 图标 + title 提示。 */
function iconButton(title: string, icon: string): HTMLButtonElement {
  const btn = el("button")
  tip(btn, title)
  btn.innerHTML = icon
  return btn
}

/** 按钮成功反馈：图标变 ✓ + 高亮，1.5s 后恢复。 */
function flashButton(btn: HTMLButtonElement, label: string): void {
  const orig = btn.innerHTML
  const origTitle = btn.title
  btn.innerHTML = '<svg viewBox="0 0 16 16"><path d="M3 8.5l3.5 3.5L13 4.5"/></svg>'
  tip(btn, label)
  btn.classList.add("ok")
  setTimeout(() => {
    btn.innerHTML = orig
    tip(btn, origTitle)
    btn.classList.remove("ok")
  }, 1500)
}

/** 触发浏览器下载（Blob）。 */
function downloadBlob(blob: Blob, filename: string): void {
  const a = document.createElement("a")
  a.href = URL.createObjectURL(blob)
  a.download = filename
  a.click()
  setTimeout(() => URL.revokeObjectURL(a.href), 1000)
}

function safeName(name: string): string {
  return (name || "diagram").replace(/\.[^.]+$/, "")
}

/** 下载图表源码（扩展名按图表语言）。 */
export function downloadDiagramSource(code: string, name: string, format: DiagramFormat): void {
  downloadBlob(new Blob([code], { type: "text/plain;charset=utf-8" }), `${safeName(name)}.${DIAGRAM_EXT_FOR[format]}`)
}

/** 将渲染出的 SVG 转为 PNG 并下载（本地绘制，无网络；3x 超采样输出高清图，低性能模式降为 1.5x 减少绘制开销）。 */
export async function downloadPumlPng(svg: string, name: string): Promise<void> {
  const img = new Image()
  const svgUrl = URL.createObjectURL(new Blob([svg], { type: "image/svg+xml;charset=utf-8" }))
  try {
    await new Promise<void>((resolve, reject) => {
      img.onload = () => resolve()
      img.onerror = () => reject(new Error("SVG 加载失败"))
      img.src = svgUrl
    })
    const w = img.naturalWidth || 800
    const h = img.naturalHeight || 600
    // SVG 为矢量：按 3x 超采样绘制得到高清 PNG；超大图按 canvas 尺寸上限自动降倍率
    const SCALE = isLowPower() ? 1.5 : 3
    const MAX_DIM = 8192
    const scale = Math.min(SCALE, MAX_DIM / Math.max(w, h))
    const canvas = document.createElement("canvas")
    canvas.width = Math.max(1, Math.round(w * scale))
    canvas.height = Math.max(1, Math.round(h * scale))
    const ctx = canvas.getContext("2d")
    if (!ctx) throw new Error("canvas 不可用")
    ctx.scale(scale, scale)
    ctx.drawImage(img, 0, 0, w, h)
    const png = canvas.toBlob ? await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/png")) : null
    if (!png) throw new Error("PNG 编码失败")
    downloadBlob(png, `${safeName(name)}.png`)
  } finally {
    URL.revokeObjectURL(svgUrl)
  }
}

/** 将渲染出的 SVG 转为 PNG 复制到剪贴板（2x 超采样，本地绘制；低性能模式降为 1x）。 */
async function copyPngToClipboard(svg: string): Promise<void> {
  if (!navigator.clipboard?.write) throw new Error("当前环境不支持复制图片")
  const img = new Image()
  const svgUrl = URL.createObjectURL(new Blob([svg], { type: "image/svg+xml;charset=utf-8" }))
  try {
    await new Promise<void>((resolve, reject) => {
      img.onload = () => resolve()
      img.onerror = () => reject(new Error("SVG 加载失败"))
      img.src = svgUrl
    })
    const w = img.naturalWidth || 800
    const h = img.naturalHeight || 600
    const SCALE = isLowPower() ? 1 : 2
    const MAX_DIM = 8192
    const scale = Math.min(SCALE, MAX_DIM / Math.max(w, h))
    const canvas = document.createElement("canvas")
    canvas.width = Math.max(1, Math.round(w * scale))
    canvas.height = Math.max(1, Math.round(h * scale))
    const ctx = canvas.getContext("2d")
    if (!ctx) throw new Error("canvas 不可用")
    ctx.scale(scale, scale)
    ctx.drawImage(img, 0, 0, w, h)
    const png = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/png"))
    if (!png) throw new Error("PNG 编码失败")
    await navigator.clipboard.write([new ClipboardItem({ "image/png": png })])
  } finally {
    URL.revokeObjectURL(svgUrl)
  }
}

/** 源码查看弹窗（只读 + 复制 + 下载源码，扩展名按图表语言）。 */
function openSourceView(code: string, name: string, format: DiagramFormat): void {
  const overlay = el("div", "preview-overlay")
  const card = el("div", "preview-card")
  const head = el("div", "preview-head")
  head.append(el("span", "preview-title", `${DIAGRAM_LABEL[format]} 源码 · ${name}`))
  const closeBtn = el("button", "preview-close", "✕")
  tip(closeBtn, "关闭（Esc）")
  head.appendChild(closeBtn)
  const body = el("div", "preview-body")
  const editor = el("textarea", "diagram-editor")
  editor.value = code
  editor.readOnly = true
  editor.style.minHeight = "60vh"
  const actions = el("div", "diagram-toolbar")
  const copy = el("button", undefined, "复制")
  copy.onclick = async () => {
    try {
      await copyText(code)
      copy.textContent = "已复制 ✓"
      setTimeout(() => (copy.textContent = "复制"), 1500)
    } catch {
      toast("复制失败")
    }
  }
  const dl = el("button", undefined, `下载 .${DIAGRAM_EXT_FOR[format]}`)
  dl.onclick = () => downloadDiagramSource(code, name, format)
  actions.append(copy, dl)
  body.append(editor, actions)
  card.append(head, body)
  overlay.appendChild(card)
  document.body.appendChild(overlay)
  const close = () => {
    overlay.remove()
    document.removeEventListener("keydown", onKey)
  }
  const onKey = (ev: KeyboardEvent) => {
    if (ev.key === "Escape") close()
  }
  closeBtn.onclick = close
  overlay.onclick = (ev) => {
    if (ev.target === overlay) close()
  }
  document.addEventListener("keydown", onKey)
}

/** 下载当前图表的 PNG（渲染后）。 */
async function downloadCurrentPng(code: string, name: string, format: DiagramFormat): Promise<void> {
  try {
    const svg = await renderDiagramSvg(format, code)
    await downloadPumlPng(svg, name)
  } catch (err) {
    toast(`下载图片失败：${(err as Error).message}`)
  }
}

/** 已渲染图表：canvas -> 最新源码与语言（主题切换时重绘）。WeakMap 不阻止 canvas 回收——
 *  强引用 Map 在主题切换清理前会累积脱离 DOM 的完整 SVG 节点（反复切换会话的渐进泄漏）。 */
const rendered = new WeakMap<HTMLElement, { code: string; format: DiagramFormat }>()

/** 活跃 canvas 轻列表（主题重绘遍历用）：条目为 WeakRef，不阻止回收，遍历时滤除已回收/离 DOM 的。 */
const activeCanvases = new Set<WeakRef<HTMLElement>>()
function trackRendered(canvas: HTMLElement, entry: { code: string; format: DiagramFormat }): void {
  rendered.set(canvas, entry)
  activeCanvases.add(new WeakRef(canvas))
  if (activeCanvases.size > 500) {
    // 惰性收缩：清掉已回收/离 DOM 的引用
    for (const ref of activeCanvases) {
      const c = ref.deref()
      if (!c || !c.isConnected) activeCanvases.delete(ref)
    }
  }
}

// 主题切换后按最新主题配色重绘所有已渲染缩略图（编辑态/已移除的跳过）
document.addEventListener("gebai:theme-change", () => {
  svgCache.clear() // PlantUML skinparam 随主题变化，缓存失效
  mermaidCache.clear() // mermaid 主题为全局初始化状态，切换后重渲染
  d2Cache.clear() // d2 主题 ID 随明暗变化，缓存失效
  echartsCache.clear() // echarts darkMode 随明暗变化，缓存失效
  void (async () => {
    for (const ref of [...activeCanvases]) {
      const canvas = ref.deref()
      const entry = canvas ? rendered.get(canvas) : undefined
      if (!canvas || !entry) {
        activeCanvases.delete(ref) // 已回收
        continue
      }
      if (!canvas.isConnected) {
        activeCanvases.delete(ref) // 已脱离 DOM（如会话切换清空）
        continue
      }
      canvas.innerHTML = ""
      await renderThumbnail(canvas, entry.code, entry.format)
    }
  })()
})

// 空闲预热本地渲染引擎（PlantUML 6.9MB + mermaid 3.5MB + echarts 1MB 懒加载）：避免首次 draw 调用时引擎加载吃掉 5 秒渲染窗口；
// D2（8MB WASM）加载开销大且架构图频率低，不预热；低性能模式跳过预热（引擎内存/加载开销大），首次渲染由消息流触发
if (typeof window !== "undefined") {
  window.addEventListener(
    "load",
    () => {
      if (isLowPower()) return
      const raf = window as { requestIdleCallback?: (cb: () => void, o?: { timeout: number }) => void }
      const schedule = raf.requestIdleCallback ? (cb: () => void) => raf.requestIdleCallback!(cb, { timeout: 3000 }) : (cb: () => void) => setTimeout(cb, 800)
      schedule(() => {
        void loadPlantUml().catch(() => {})
        void loadMermaid().catch(() => {})
        void loadEcharts().catch(() => {})
      })
    },
    { once: true },
  )
}

/** 缩略图渲染：小尺寸展示（CSS 约束）。渲染失败不把错误细节暴露到主页面（仅占位提示，详情进查看器/控制台）。 */
async function renderThumbnail(canvas: HTMLElement, code: string, format: DiagramFormat): Promise<void> {
  try {
    const svg = await renderDiagramSvg(format, code)
    canvas.innerHTML = svg
  } catch (err) {
    console.error("diagram thumb failed:", err)
    canvas.appendChild(el("div", "diagram-error", "图表渲染失败，点击查看详情"))
  }
}

/** 消息流内图表卡片：缩略图 + 文件名；悬浮时右下角显示复制/下载图标；点击缩略图/文件名进入全屏查看器。 */
export async function renderDiagram(container: HTMLElement, b: Extract<ContentBlock, { type: "diagram" }>) {
  const format = (b.format ?? "plantuml") as DiagramFormat
  const box = el("div", "diagram diagram-card")

  const thumb = el("div", "diagram-thumb")
  tip(thumb, "点击查看大图")
  box.appendChild(thumb)

  const title = el("div", "diagram-title clickable", b.name || "图表")
  box.appendChild(title)

  // 悬浮操作：右下角复制/下载图标（点击不触发打开查看器）
  const hoverBar = el("div", "diagram-hover-bar")
  const copyBtn = iconButton("复制图片", ICON_COPY)
  const dlBtn = iconButton("下载图片", ICON_DOWNLOAD)
  hoverBar.append(copyBtn, dlBtn)
  box.appendChild(hoverBar)
  container.appendChild(box)

  const render = async (code: string) => {
    thumb.innerHTML = ""
    trackRendered(thumb, { code, format })
    await renderThumbnail(thumb, code, format)
  }
  const open = () => openViewer(b.code, b.name || "图表", format)

  // 始终默认渲染缩略图（本地引擎懒加载 + 失败占位提示，不阻塞消息流）；渲染失败/成功均可点击进查看器
  thumb.onclick = open
  title.onclick = open
  await render(b.code)
  copyBtn.onclick = (e) => {
    e.stopPropagation()
    void (async () => {
      try {
        const svg = await renderDiagramSvg(format, b.code)
        await copyPngToClipboard(svg)
        flashButton(copyBtn, "已复制到剪贴板")
      } catch (err) {
        toast(`复制图片失败：${(err as Error).message}`)
      }
    })()
  }
  dlBtn.onclick = (e) => {
    e.stopPropagation()
    void downloadCurrentPng(b.code, b.name || "图表", format)
  }
}

/** 全屏查看器共享骨架：预览遮罩 + 单栏工具栏（缩放组 + 操作组 + 关闭），图表/图片查看器共用。 */
function viewerShell(name: string, actions: { copy: () => void; download: () => void; source?: () => void }) {
  const overlay = el("div", "preview-overlay")
  const card = el("div", "preview-card diagram-preview")
  const toolbar = el("div", "diagram-zoom-toolbar")
  const titleEl = el("span", "preview-title", name)
  const fit = iconButton("还原视图", ICON_FIT)
  const zoomOut = iconButton("缩小", ICON_MINUS)
  const zoomIn = iconButton("放大", ICON_PLUS)
  const copyBtn = iconButton("复制图片", ICON_COPY)
  const dlBtn = iconButton("下载图片", ICON_DOWNLOAD)
  const srcBtn = actions.source ? iconButton("查看源码", ICON_SOURCE) : null
  const closeBtn = el("button", "preview-close", "✕")
  tip(closeBtn, "关闭（Esc）")
  // 分组：缩放一组（放大/缩小/还原），操作一组（复制/下载/源码）
  const zoomGroup = el("div", "diagram-toolbar-group")
  zoomGroup.append(zoomIn, zoomOut, fit)
  const actionGroup = el("div", "diagram-toolbar-group")
  actionGroup.append(copyBtn, dlBtn)
  if (srcBtn) actionGroup.appendChild(srcBtn)
  toolbar.append(titleEl, zoomGroup, actionGroup, closeBtn)
  const body = el("div", "preview-body")
  const zoom = el("div", "diagram-zoom")
  body.appendChild(zoom)
  card.append(toolbar, body)
  overlay.appendChild(card)
  document.body.appendChild(overlay)

  const zoomCtl = bindViewerZoom(body, zoom)
  const close = () => {
    overlay.remove()
    zoomCtl.dispose()
    document.removeEventListener("keydown", onKey)
  }
  const onKey = (ev: KeyboardEvent) => {
    if (ev.key === "Escape") close()
  }
  closeBtn.onclick = close
  overlay.onclick = (ev) => {
    if (ev.target === overlay) close()
  }
  document.addEventListener("keydown", onKey)

  zoomIn.onclick = () => zoomCtl.centerZoom(1.25)
  zoomOut.onclick = () => zoomCtl.centerZoom(1 / 1.25)
  fit.onclick = zoomCtl.fitView
  copyBtn.onclick = () => void actions.copy()
  dlBtn.onclick = () => void actions.download()
  if (srcBtn) srcBtn.onclick = () => void actions.source!()

  return { body, zoom, close, zoomCtl, copyBtn }
}

/** 查看器画布缩放控制器：内容元素（svg/img）transform 视觉缩放 + zoom 容器撑开滚动区域；
 *  滚轮以光标为锚点缩放、鼠标拖拽平移、按钮以图中心为锚点缩放。 */
function bindViewerZoom(body: HTMLElement, zoom: HTMLElement) {
  let scale = 1
  let baseW = 0 // 内容原始显示尺寸（缩放基准，不随 transform 变化）
  let baseH = 0
  let contentEl: Element | null = null

  /** 应用缩放：内容用 transform（视觉缩放），zoom 容器撑开滚动区域。 */
  const applyScale = (s: number) => {
    scale = Math.min(5, Math.max(0.2, s))
    if (!contentEl) return
    ;(contentEl as HTMLElement).style.transform = `scale(${scale})`
    zoom.style.width = `${baseW * scale}px`
    zoom.style.height = `${baseH * scale}px`
  }

  /** 以内容坐标 (cx, cy) 为锚点缩放（缩放前后该内容点保持在容器内同一位置）。 */
  const zoomAt = (next: number, cx: number, cy: number) => {
    const ratio = next / scale
    applyScale(next)
    body.scrollLeft = cx * ratio - (cx - body.scrollLeft)
    body.scrollTop = cy * ratio - (cy - body.scrollTop)
  }

  /** 按钮缩放：以图中心为锚点。 */
  const centerZoom = (factor: number) => {
    const rect = body.getBoundingClientRect()
    zoomAt(scale * factor, body.scrollLeft + rect.width / 2, body.scrollTop + rect.height / 2)
  }

  /** 自动适应视口：初始状态放大到 125%（小图更清晰、不超屏），并居中显示。 */
  const fitView = () => {
    if (!contentEl || !baseW || !baseH) return
    const cw = body.clientWidth - 28 // 减去 padding
    const ch = body.clientHeight - 28
    scale = 1 // 重置基准后一次到位
    applyScale(Math.min(1.25, cw / baseW, ch / baseH))
    // 居中：图超出视口时让图中心落在视口中心；未超出时由 flex margin auto 居中
    body.scrollLeft = Math.max(0, (body.scrollWidth - body.clientWidth) / 2)
    body.scrollTop = Math.max(0, (body.scrollHeight - body.clientHeight) / 2)
  }

  /** 装载内容元素并固定原始尺寸（百分比宽度内容在 flex 容器内 getBoundingClientRect 可能解析为 0，尺寸由调用方给出）。 */
  const setContent = (found: Element, w: number, h: number) => {
    contentEl = found
    baseW = w
    baseH = h
    const st = (found as HTMLElement).style
    st.maxWidth = "none"
    st.width = `${w}px`
    st.height = `${h}px`
    st.transformOrigin = "0 0"
    st.transform = "scale(1)"
    zoom.style.width = `${w}px`
    zoom.style.height = `${h}px`
  }

  body.addEventListener(
    "wheel",
    (e) => {
      e.preventDefault()
      // 以光标位置为锚点缩放
      const rect = body.getBoundingClientRect()
      const cx = body.scrollLeft + (e.clientX - rect.left)
      const cy = body.scrollTop + (e.clientY - rect.top)
      zoomAt(scale * (e.deltaY < 0 ? 1.1 : 0.9), cx, cy)
    },
    { passive: false },
  )

  // 鼠标拖拽平移查看（抓取手势）
  let panning = false
  let panX = 0
  let panY = 0
  let panL = 0
  let panT = 0
  const onPanMove = (e: MouseEvent) => {
    if (!panning) return
    body.scrollLeft = panL - (e.clientX - panX)
    body.scrollTop = panT - (e.clientY - panY)
  }
  const onPanUp = () => {
    if (!panning) return
    panning = false
    body.classList.remove("panning")
  }
  body.addEventListener("mousedown", (e) => {
    if (e.button !== 0) return
    panning = true
    panX = e.clientX
    panY = e.clientY
    panL = body.scrollLeft
    panT = body.scrollTop
    body.classList.add("panning")
    e.preventDefault()
  })
  document.addEventListener("mousemove", onPanMove)
  document.addEventListener("mouseup", onPanUp)

  return {
    setContent,
    centerZoom,
    fitView,
    /** 关闭时解绑 document 级监听（body 上的监听随 overlay 移除一并销毁）。 */
    dispose: () => {
      document.removeEventListener("mousemove", onPanMove)
      document.removeEventListener("mouseup", onPanUp)
    },
  }
}

/** 图表全屏查看器：大图 + 单栏工具栏（标题/缩放/下载/源码/关闭），渲染失败显示错误与源码。 */
function openViewer(code: string, name: string, format: DiagramFormat) {
  const shell = viewerShell(name, {
    copy: () => {
      void (async () => {
        try {
          const svg = await renderDiagramSvg(format, code)
          await copyPngToClipboard(svg)
          flashButton(shell.copyBtn, "已复制到剪贴板")
        } catch (err) {
          toast(`复制图片失败：${(err as Error).message}`)
        }
      })()
    },
    download: () => void downloadCurrentPng(code, name, format),
    source: () => openSourceView(code, name, format),
  })
  /** 渲染代码到大图并初始化缩放。 */
  const renderCode = async (c: string) => {
    shell.zoom.innerHTML = ""
    shell.zoom.appendChild(el("div", "diagram-error", "渲染中…"))
    try {
      const svg = await renderDiagramSvg(format, c)
      shell.zoom.innerHTML = svg
    } catch (err) {
      shell.zoom.innerHTML = "" // 移除「渲染中…」占位
      showFallback(shell.zoom, c, `图表渲染失败（${(err as Error).message}），以下为源码`)
      return
    }
    const found = shell.zoom.querySelector("svg")
    if (found) {
      // 固定原始尺寸，缩放用 transform（保持光标/中心锚点）
      // 逻辑尺寸优先取 viewBox（无宽高属性/百分比宽度的 SVG 在 flex 容器内 getBoundingClientRect 可能解析为 0）
      const vb = (found as SVGSVGElement).viewBox?.baseVal
      const r = (found as SVGSVGElement).getBoundingClientRect()
      shell.zoomCtl.setContent(found, vb?.width && vb.width > 0 ? vb.width : r.width, vb?.height && vb.height > 0 ? vb.height : r.height)
    }
    shell.zoomCtl.fitView()
  }
  void renderCode(code)
}

/** 从同源图片 URL 复制到剪贴板（fetch 为 Blob 后按实际 MIME 写入）。 */
async function copyImageUrlToClipboard(url: string): Promise<void> {
  if (!navigator.clipboard?.write) throw new Error("当前环境不支持复制图片")
  const res = await fetch(url)
  if (!res.ok) throw new Error(`图片加载失败（HTTP ${res.status}）`)
  const blob = await res.blob()
  if (!blob.type.startsWith("image/")) throw new Error("图片资源不可用")
  await navigator.clipboard.write([new ClipboardItem({ [blob.type]: blob })])
}

/** 触发同源图片 URL 下载（download 属性仅对同源生效；会话 tmp/ 图片满足）。 */
function downloadImageUrl(url: string, filename: string): void {
  const a = document.createElement("a")
  a.href = url
  a.download = filename
  a.click()
}

/** 图片全屏查看器（draw 工具 render=backend 产出的 PNG 图片块等）：与图表查看器同骨架，
 *  图片直显 + 缩放/平移/复制/下载。 */
export function openImageViewer(url: string, name: string) {
  const shell = viewerShell(name, {
    copy: () => {
      void (async () => {
        try {
          await copyImageUrlToClipboard(url)
          flashButton(shell.copyBtn, "已复制到剪贴板")
        } catch (err) {
          toast(`复制图片失败：${(err as Error).message}`)
        }
      })()
    },
    download: () => downloadImageUrl(url, `${safeName(name)}.png`),
  })
  shell.zoom.appendChild(el("div", "diagram-error", "渲染中…"))
  const img = new Image()
  img.alt = name
  img.onload = () => {
    shell.zoom.innerHTML = ""
    shell.zoom.appendChild(img)
    shell.zoomCtl.setContent(img, img.naturalWidth || 800, img.naturalHeight || 600)
    shell.zoomCtl.fitView()
  }
  img.onerror = () => {
    shell.zoom.innerHTML = ""
    showFallback(shell.zoom, name, "图片加载失败")
  }
  img.src = url
}
