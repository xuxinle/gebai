/**
 * 飞书桥接后端渲染器：PlantUML 源码 → SVG（@plantuml/core，TeaVM 引擎本地渲染，零网络请求）→ PNG（@resvg/resvg-js）。
 * - @plantuml/core 面向浏览器（依赖 window/document/XMLSerializer），在 Bun 中以极简 DOM shim 垫层运行；
 *   viz-global.js（Graphviz WASM，base64 内嵌）与 plantuml.js（TeaVM ESM）均为自包含单文件，无需网络。
 * - **导入顺序依赖**：`plantuml-dom-shim`（DOM 垫层）必须最先静态导入——ESM 依赖按导入顺序求值，
 *   保证全局 window/document 就绪后再执行 viz-global.js（UMD 注入 globalThis.Viz）与 plantuml.js。
 * - 引擎 renderToString 不支持并发，全部渲染经串行队列（与前端 diagram.ts 一致）。
 * - 依赖全部可注入（测试可用 fake 替身），失败抛错携带渲染原因（供回传模型修正源码）。
 */
// 1) DOM 垫层（副作用：设置 window/document/XMLSerializer 等全局）
import "./plantuml-dom-shim"
// 2) Graphviz 布局引擎（UMD）：Bun 按 CommonJS 加载 UMD，Viz 落进模块导出而非 globalThis.Viz，
//    需手动补全局（plantuml.js 运行时按裸全局名读取 Viz；已在全局时不重复覆盖）。
import * as vizGlobal from "@plantuml/core/viz-global.js"
;(globalThis as Record<string, unknown>).Viz ??= (vizGlobal as { default?: unknown }).default ?? vizGlobal
// 3) TeaVM 引擎（浏览器 DOM API 运行时读取上述全局）
import { renderToString } from "@plantuml/core"
import { svgRasterize } from "../core/support/diagram-render"
import { normalizePlantUml } from "../core/support/artifacts"
import { applyPlantUmlDomShim } from "./plantuml-dom-shim"

/** 渲染器接口（bot 依赖注入，测试可伪造）。 */
export interface PlantUmlRenderer {
  /** 渲染 PlantUML 源码为 PNG 字节；失败抛错（错误信息含渲染原因）。 */
  renderPng(code: string, opts?: { background?: string; maxWidth?: number; maxHeight?: number }): Promise<Uint8Array>
}

/** TeaVM 引擎 API 形状（@plantuml/core/plantuml.js 导出）。 */
interface PlantUmlApi {
  renderToString: (lines: string[], onSuccess: (svg: string) => void, onError: (msg: string) => void) => void
}

/* ---------------- 渲染 ---------------- */

/** 后端固定浅色主题（飞书聊天内白底图可读；追加在源码末尾、@end 之前，覆盖用户 !theme）。 */
const LIGHT_SKIN =
  "skinparam backgroundColor #FFFFFF\n" +
  "skinparam shadowing true\n" +
  "skinparam defaultFontColor #1F2328\n" +
  "skinparam ArrowColor #57606A\n" +
  "skinparam classBackgroundColor #F6F8FA\n" +
  "skinparam classBorderColor #D0D7DE\n" +
  "skinparam objectBackgroundColor #F6F8FA\n" +
  "skinparam objectBorderColor #D0D7DE\n" +
  "skinparam packageBackgroundColor #F6F8FA\n" +
  "skinparam packageBorderColor #D0D7DE\n" +
  "skinparam stateBackgroundColor #F6F8FA\n" +
  "skinparam stateBorderColor #D0D7DE\n" +
  "skinparam noteBackgroundColor #FFF8C5\n" +
  "skinparam noteBorderColor #D0D7DE\n" +
  "skinparam activityBackgroundColor #F6F8FA\n" +
  "skinparam activityBorderColor #D0D7DE\n" +
  "skinparam activityDiamondBackgroundColor #F6F8FA\n" +
  "skinparam activityDiamondBorderColor #D0D7DE\n" +
  "skinparam legendBackgroundColor #F6F8FA\n" +
  "skinparam legendBorderColor #D0D7DE\n" +
  "skinparam titleBackgroundColor #F6F8FA\n" +
  "skinparam titleBorderColor #D0D7DE\n" +
  "skinparam SequenceBoxBackgroundColor #F6F8FA\n" +
  "skinparam SequenceBoxBorderColor #D0D7DE\n" +
  "skinparam SequenceGroupBackgroundColor #F6F8FA\n" +
  "skinparam SequenceGroupBorderColor #D0D7DE\n" +
  "skinparam SequenceLifeLineBackgroundColor #F6F8FA\n" +
  "skinparam SequenceLifeLineBorderColor #D0D7DE\n" +
  "skinparam SequenceReferenceBackgroundColor #F6F8FA\n" +
  "skinparam SequenceReferenceBorderColor #D0D7DE\n" +
  "skinparam defaultTextAlignment center\n"

/** 追加浅色主题 skinparam（在 @end 指令前，保证覆盖用户主题设置）。 */
function withLightSkin(code: string): string {
  const endIdx = code.search(/@end\w*/)
  if (endIdx >= 0) return `${code.slice(0, endIdx)}${LIGHT_SKIN}\n${code.slice(endIdx)}`
  return `${code}\n${LIGHT_SKIN}`
}

/** 错误页文本特征（TeaVM 引擎对语法错误/不支持图型不回调 onError，而是输出含错误文本的 SVG）。 */
const ERROR_SVG_MARK = /Syntax Error|Parse error|Fatal parsing error|not supported|Some diagram description contains errors|From textarea/i

/** 从错误 SVG 中提取可读的错误详情（优先找 error/line 相关文本行）。 */
function extractSvgError(svg: string): string {
  const lines = svg.match(/<text[^>]*>([^<]*)<\/text>/g)?.map((t) => t.replace(/<[^>]+>/g, "").trim()).filter(Boolean) ?? []
  const detail = lines.find((l) => /error|line \d|not supported/i.test(l)) ?? lines[lines.length - 1] ?? "请检查源码"
  return detail.slice(0, 160)
}

/** 渲染串行队列：@plantuml/core 的 renderToString 不支持并发，必须一次渲染一个。 */
let renderQueue: Promise<unknown> = Promise.resolve()
function enqueueRender<T>(task: () => Promise<T>): Promise<T> {
  const run = renderQueue.then(task, task)
  renderQueue = run.catch(() => {}) // 单个失败不阻塞后续
  return run
}

/** 渲染超时（大图/引擎首次加载慢时兜底，防阻塞任务）；测试可注入小值加速超时用例。 */
export const PLANTUML_TIMEOUT_MS = 20000

export function createPlantUmlRenderer(opts: {
  /** 引擎渲染实现（测试注入 fake；缺省走静态导入的真实引擎）。 */
  engine?: () => Promise<PlantUmlApi>
  /** PNG 栅格化（测试注入；缺省共享 @resvg/resvg-js 栅格化，见 core/diagram-render.ts）。 */
  rasterize?: (svg: string, opts: { background?: string; maxWidth?: number; maxHeight?: number }) => Promise<Uint8Array>
  /** 渲染超时（毫秒，缺省 20 秒；测试注入小值加速超时用例）。 */
  timeoutMs?: number
} = {}): PlantUmlRenderer {
  const engine = opts.engine ?? (async () => ({ renderToString }) as PlantUmlApi)
  const rasterize = opts.rasterize ?? svgRasterize

  return {
    async renderPng(code, o = {}) {
      // 每次渲染前重放 DOM 垫层（组合渲染器在 happy-dom 垫层与本垫层之间切换全局环境，直接使用时也保证 window/document 就绪）
      applyPlantUmlDomShim()
      const full = withLightSkin(normalizePlantUml(code))
      return enqueueRender(async () => {
        const api = await engine()
        const svg = await new Promise<string>((resolve, reject) => {
          const timer = setTimeout(() => reject(new Error("PlantUML 渲染超时（图表过大或引擎繁忙）")), opts.timeoutMs ?? PLANTUML_TIMEOUT_MS)
          try {
            api.renderToString(
              full.split("\n"),
              (s) => {
                clearTimeout(timer)
                resolve(s)
              },
              (msg) => {
                clearTimeout(timer)
                reject(new Error(`PlantUML 渲染错误：${String(msg).slice(0, 160)}`))
              },
            )
          } catch (err) {
            clearTimeout(timer)
            reject(err instanceof Error ? err : new Error(String(err)))
          }
        })
        if (ERROR_SVG_MARK.test(svg)) {
          throw new Error(`PlantUML 渲染错误：${extractSvgError(svg)}`)
        }
        return rasterize(svg, { background: o.background ?? "#ffffff", maxWidth: o.maxWidth, maxHeight: o.maxHeight })
      })
    },
  }
}
