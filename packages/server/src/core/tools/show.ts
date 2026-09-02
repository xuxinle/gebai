/** 展示类全局工具（show/fetch_url）——自 core/tools.ts 按域拆分。 */
import { createHash } from "node:crypto"
import { join, relative, sep } from "node:path"
import type { ContentBlock, DiagramFormat } from "@gebai/sdk"
import type { Tool, ToolContext, ToolResult } from "../base/types"
import { sessionPath } from "../base/paths"
import { inferLang } from "../base/diff"
import { truncate } from "../support/truncate"
import {
  diagramFormatFor,
  DIAGRAM_EXT,
  DIAGRAM_EXT_FOR,
  DIAGRAM_LABEL,
  IMAGE_EXT,
  injectPlantUmlLayout,
  mimeFor,
  normalizePlantUml,
  RENDER_HTML_MAX_HEIGHT,
  RENDER_HTML_MAX_WIDTH,
} from "../support/artifacts"
import { assertPublicHttpUrl, fetchWithRedirectGuard } from "../security/fetch-guard"
import { schema, type GlobalToolEntry } from "./shared"

/** fetch_url：响应大小上限与超时；STREAM 上限为流式读取的内存护栏（超限中止读取）。 */
const FETCH_URL_MAX_BYTES = 200 * 1024
const FETCH_URL_TIMEOUT = 15000
const FETCH_URL_STREAM_MAX_BYTES = 10 * 1024 * 1024
export const fetchUrlTool: Tool = {
  name: "fetch_url",
  description: "抓取 URL 内容（网页/API/文档）。服务端部署模式限制公网地址（防 SSRF，含重定向逐跳校验）；响应超过阈值截断。",
  card: { titleParams: ["url"], args: "none" },
  parameters: schema({ url: { type: "string" } }, ["url"]),
  outputSchema: schema({
    ok: { type: "boolean", description: "是否抓取成功（输出文本可用）" },
    status: { type: "integer", description: "HTTP 状态码（发起请求并有响应时）" },
    contentType: { type: "string", description: "响应 Content-Type" },
    error: { type: "string", description: "失败原因（ok=false 时）" },
  }, ["ok"]),
  async execute(args, ctx) {
    const url = String(args.url)
    let res: Response
    try {
      // 沙箱模式：重定向守卫逐跳校验目标地址（初始 URL 与每跳 Location 均需公网），
      // 防「初始公网 → 302 内网」跳板绕过；本地模式正常跟随重定向
      res = ctx.sandboxed
        ? await fetchWithRedirectGuard(url, { signal: AbortSignal.timeout(FETCH_URL_TIMEOUT) }, assertPublicHttpUrl)
        : await fetch(url, { signal: AbortSignal.timeout(FETCH_URL_TIMEOUT) })
    } catch (err) {
      return { output: `fetch_url 失败: ${(err as Error).message}`, data: { ok: false, error: (err as Error).message } }
    }
    if (!res.ok) return { output: `fetch_url 失败: HTTP ${res.status} ${res.statusText}`, data: { ok: false, status: res.status } }
    const ct = res.headers.get("content-type") || ""
    if (!/text|json|xml|html|markdown|javascript|css/i.test(ct)) {
      return { output: `非文本内容（${ct || "未知类型"}），已跳过内容抓取。`, data: { ok: false, status: res.status, contentType: ct } }
    }
    // 流式限量读取（先下载后截断防不住内存：超大/无限流响应在截断前就已全量入内存）
    const reader = res.body?.getReader()
    let text = ""
    if (reader) {
      const chunks: Uint8Array[] = []
      let received = 0
      let oversized = false
      const dec = new TextDecoder()
      for (;;) {
        const { done, value } = await reader.read()
        if (done) break
        if (value) {
          received += value.byteLength
          if (received > FETCH_URL_STREAM_MAX_BYTES) {
            oversized = true
            void reader.cancel().catch(() => {})
            break
          }
          chunks.push(value)
          text += dec.decode(value, { stream: true })
        }
      }
      text += dec.decode()
      if (oversized) text += `
…（响应超过 ${Math.round(FETCH_URL_STREAM_MAX_BYTES / 1024 / 1024)}MB 读取上限，已截断）`
    } else {
      text = await res.text()
    }
    return { ...(await truncate(text.slice(0, FETCH_URL_MAX_BYTES), "fetch_url", ctx)), data: { ok: true, status: res.status, contentType: ct } }
  },
}
/** show：非会话 tmp/ 文件复制进会话文件区的尺寸上限（超出提示改为告知路径，防大文件拖垮磁盘/传输）。 */
export const SHOW_MAX_BYTES = 100 * 1024 * 1024
/** show：文本直读展示的读取上限（字节，超出不再内联文本，仅给查看/下载卡片）。 */
export const SHOW_TEXT_DIRECT_BYTES = 512 * 1024
/** show：内联文本/代码块字符上限（超出截断展示 + 附 file 卡片取全文）。 */
export const SHOW_TEXT_MAX_CHARS = 40_000

/** show：可内联直显的文本/代码扩展名（命中则内容直接进 code 块展示）。 */
const SHOW_TEXT_EXT = new Set(
  "txt md markdown csv tsv json jsonl xml yaml yml log toml ini env properties conf cfg ts tsx js jsx mjs cjs py pyw rb go rs java kt kts c h cpp hpp cc cs php sh bash zsh bat cmd ps1 ps1m1 sql lua dart swift scala r pl vue svelte css scss less sass".split(
    " ",
  ),
)

/** show 文件归属处理：会话 tmp/ 内文件直接引用（零复制）；会话外（本地模式工作区/绝对路径）复制一份到
 * tmp/shown/（内容哈希命名，重复展示复用同一副本）后引用——前端文件接口只服务会话 tmp/，复制保证
 * 可见性与文件面板留存。归属判定用会话 tmp 真实绝对路径（项目绑定子Agent 的 resolvePath 基准是项目根，
 * 不能作判定依据）；复制目标写会话 tmp 绝对路径（同样绕开项目根基准，保证落在真实会话文件区）。 */
async function stageShowFile(
  ctx: ToolContext,
  rawPath: string,
  display: string,
): Promise<{ error: string } | { logical: string; abs: string; size: number; inSessionTmp: boolean; copiedBytes: Uint8Array | null }> {
  let abs: string
  try {
    abs = ctx.resolvePath(rawPath)
  } catch (err) {
    return { error: `show 失败：路径被拒绝（${err instanceof Error ? err.message : String(err)}）。请确认路径在允许范围内。` }
  }
  const { stat } = await import("node:fs/promises")
  let size: number
  try {
    const st = await stat(abs)
    if (!st.isFile()) return { error: `show 失败：${rawPath} 不是文件（目录请用 ls 列出后选择具体文件）。` }
    size = st.size
  } catch {
    return { error: `show 失败：文件不存在（${rawPath}）。请确认路径（可用 ls 查看）。` }
  }
  const sessionTmp = join(sessionPath(ctx.home, ctx.user, ctx.sessionId), "tmp")
  const inSessionTmp = abs === sessionTmp || abs.startsWith(sessionTmp + sep)
  // 非会话文件先复制（所有分支统一：内联展示的同时产物留存会话文件区，文件面板可见/可下载）
  let logical: string
  let copiedBytes: Uint8Array | null = null
  if (inSessionTmp) {
    logical = `tmp/${relative(sessionTmp, abs).split(sep).join("/")}`
  } else {
    if (size > SHOW_MAX_BYTES) {
      return { error: `show 失败：文件过大（${size} 字节，复制上限 ${SHOW_MAX_BYTES} 字节）。请改为在回复中告知文件路径。` }
    }
    if (!ctx.readBinaryFile || !ctx.writeBinaryFile) {
      return { error: "show 失败：当前环境不支持二进制复制（readBinaryFile/writeBinaryFile 未启用），仅支持会话 tmp/ 内文件。" }
    }
    try {
      copiedBytes = await ctx.readBinaryFile(abs)
    } catch (err) {
      return { error: `show 失败：无法读取 ${rawPath}（${err instanceof Error ? err.message : String(err)}）。` }
    }
    const base = display.replace(/\.[^.]+$/, "") || "file"
    const ext = display.match(/\.[^.]+$/)?.[0] ?? ""
    const hash = createHash("sha256").update(copiedBytes).digest("hex").slice(0, 8)
    logical = `tmp/shown/${base}-${hash}${ext}`
    // 复制目标写会话 tmp 绝对路径（项目绑定子Agent 的 resolvePath 基准是项目根，直接写会落错位置）
    await ctx.writeBinaryFile(join(sessionTmp, "shown", `${base}-${hash}${ext}`), copiedBytes)
  }
  return { logical, abs, size, inSessionTmp, copiedBytes }
}

/** show 图表分支渲染管线（code 源码与 path 图表文件共用）：PlantUML 自动包装+布局注入、ECharts JSON
 * 预校验、frontend 实时渲染验证闭环（渲染成功才返回成功）、backend 服务端 PNG、产物落盘 tmp/。
 * fromPath 非空 = path 模式（源文件已留存，文案注明来源）。 */
async function showDiagram(
  ctx: ToolContext,
  raw: string,
  format: DiagramFormat,
  base: string,
  fromPath: string,
  render: unknown,
): Promise<ToolResult> {
  const label = DIAGRAM_LABEL[format]
  const code = format === "plantuml" ? injectPlantUmlLayout(normalizePlantUml(raw)) : raw
  // 产物写入会话 tmp/（与 read/write/truncate 的 tmp/ 约定一致，UI 文件面板可见）
  const rel = `tmp/${base}.${DIAGRAM_EXT_FOR[format]}`
  // render=backend：服务端直接渲染 PNG 图片（不经前端/飞书通道），落盘 tmp/{name}.png 并返回 image 内容块（四语言均支持）
  if (render === "backend") {
    if (!ctx.renderDiagram || !ctx.writeBinaryFile) {
      return { output: "画图失败：后端渲染不可用（当前环境未启用服务端图表渲染）。" }
    }
    let png: Uint8Array
    try {
      png = await ctx.renderDiagram(code, { format })
    } catch (err) {
      return { output: `画图失败（后端渲染错误）：${err instanceof Error ? err.message : String(err)}。请修正 ${label} 源码后重试。` }
    }
    const pngRel = `tmp/${base}.png`
    await ctx.writeBinaryFile(ctx.resolvePath(pngRel), png)
    await ctx.writeFile(ctx.resolvePath(rel), code)
    return {
      output: fromPath
        ? `图表已渲染为图片: ${pngRel}（源文件 ${fromPath}，${raw.length} 字符）`
        : `图表已生成并渲染为图片: ${pngRel}（${raw.length} 字符）`,
      blocks: [{ type: "image", path: pngRel, name: `${base}.png`, mime: "image/png" }],
    }
  }
  // echarts：服务端预校验 JSON（纯解析零渲染开销）——无效 JSON 立即精确报错，不白跑前端一轮；
  // 旧版前端会把未知语言当 PlantUML 渲染出误导性错误，预校验先拦掉真正的源码问题
  if (format === "echarts") {
    const { parseEchartsInput } = await import("../support/diagram-render")
    try {
      parseEchartsInput(code)
    } catch (err) {
      return { output: `画图失败：${err instanceof Error ? err.message : String(err)}` }
    }
  }
  // 无交互模式（REST 单次调用）无前端渲染通道：不空等 5 秒超时，直接引导后端渲染
  if (ctx.interactionMode === "none") {
    return { output: "画图能力受限：当前通道无前端渲染（无交互调用）。请改用 render=backend（服务端渲染 PNG 图片）后重试。" }
  }
  // 实时渲染（Web 前端按 format 本地渲染或飞书后端渲染，通道实现不同但结果一致）：成功才返回成功，渲染错误回传模型，5 秒超时判定画图能力受限
  const rendered = await ctx.waitForDraw({ code, name: base, format })
  if (rendered === null) {
    return { output: "画图能力受限：未能在 5 秒内完成渲染（渲染端离线或超时），请稍后重试，或用其他方式表达图表内容。" }
  }
  if (!rendered.ok) {
    // 报错引擎与请求语言不符（如请求 echarts 却由 PlantUML 引擎报错）= 前端为旧版本（旧代码把未知
    // 语言静默当 PlantUML 渲染）：明确诊断引导刷新页面——前端渲染是默认通道，不自动换通道旁路
    const engine = (rendered.error ?? "").match(/^(PlantUML|Mermaid|D2|ECharts) 渲染/)?.[1]
    if (engine && engine !== label) {
      return { output: `画图失败：前端渲染器版本过旧（请求 ${label} 图表却由 ${engine} 引擎渲染报错）。请提示用户刷新页面（加载新前端版本）后重试；确需本次立即出图可改用 render=backend。` }
    }
    return { output: `画图失败（渲染错误）：${rendered.error ?? "未知错误"}。请修正 ${label} 源码后重试。` }
  }
  await ctx.writeFile(ctx.resolvePath(rel), code)
  return {
    output: fromPath
      ? `图表已渲染成功: ${rel}（源文件 ${fromPath}，${raw.length} 字符）`
      : `图表已生成并渲染成功: ${rel}（${raw.length} 字符）`,
    blocks: [{ type: "diagram", format, code: raw, name: `${base}.${DIAGRAM_EXT_FOR[format]}`, version: 1 }],
  }
}

/** html 内容块构造：显式尺寸为正整数且不超上限时附加（其余值忽略，回退默认）。 */
function htmlBlock(html: string, name: string, width: unknown, height: unknown): ContentBlock {
  const block: Record<string, unknown> = { type: "html", html, name }
  const w = typeof width === "number" && Number.isFinite(width) ? Math.round(width) : undefined
  const h = typeof height === "number" && Number.isFinite(height) ? Math.round(height) : undefined
  if (w !== undefined && w > 0 && w <= RENDER_HTML_MAX_WIDTH) block.width = w
  if (h !== undefined && h > 0 && h <= RENDER_HTML_MAX_HEIGHT) block.height = h
  return block as ContentBlock
}

/** show HTML 分支（html 源码）：沙箱 iframe 域隔离预览（仅实时前端通道），产物落盘 tmp/ 并返回 html 内容块。 */
async function showHtml(ctx: ToolContext, html: string, base: string, width: unknown, height: unknown): Promise<ToolResult> {
  if (ctx.interactionMode && ctx.interactionMode !== "realtime") {
    return { output: "show 失败：当前通道不支持 HTML 页面预览（仅 Web 前端实时会话可用）。请改为产出 .html 文件（write）后用 path 交付文件，或在回复中描述内容。" }
  }
  // 产物写入会话 tmp/（与图表分支的 tmp/ 约定一致，UI 文件面板可见）
  const rel = `tmp/${base}.html`
  await ctx.writeFile(ctx.resolvePath(rel), html)
  return {
    output: `HTML 页面已生成并渲染: ${rel}（${html.length} 字符）`,
    blocks: [htmlBlock(html, `${base}.html`, width, height)],
  }
}

/**
 * show：把内容**直接展示给用户**——统一入口，内容源三选一（code / html / path），内容在消息流内联呈现：
 * - `code`+`format`（图表创作）：四语言实时渲染验证（成功才返回成功，失败返回错误供修正），
 *   `render=backend` 服务端渲染 PNG；产物落盘 tmp/ 并返回 diagram 块。
 * - `html`（HTML 页面）：沙箱 iframe 域隔离预览，返回 html 块；仅实时前端通道（分支内校验）。
 * - `path`（已有文件直显）：图片 → image 块；图表源文件 → 渲染验证 + diagram 块（与 code 同一管线）；
 *   `.html` → html 块；文本/代码 → code 块内联（超长截断 + 附 file 卡片）；其余 → file 卡片。
 */
export const showTool: Tool = {
  name: "show",
  description:
    "向用户展示内容（聊天界面内联呈现），内容源三选一：①图表——code 传源码 + format 指定语言（Mermaid 通用首选 / PlantUML 标准 UML 建模 / D2 美观架构图 / ECharts 数据图表，选型指南见 format 参数），前端实时渲染验证，渲染成功才返回成功、失败返回错误信息供修正；②HTML 页面——html 传源码，沙箱 iframe 域隔离预览，仅 Web 前端通道；③已有文件——path 传路径按类型直显：图片内联显示、图表源文件（.puml/.mmd/.d2/.echarts）渲染成图表、.html 页面预览、文本/代码内联展示、无法内联的类型（PDF/压缩包/Office/音视频等）给查看/下载卡片。产物保存到会话 tmp/ 并返回对应内容块。",
  card: { args: "block" },
  parameters: schema(
    {
      code: { type: "string", description: "图表源码（与 html/path 三选一，需同时传 format）。PlantUML 布局：流程类显式 `left to right direction` 或保持默认纵向，勿逐条连线硬控方向；关系紧密的节点用 `together { … }` 保持相邻；节点 ≤20 个，大图按层拆包。PlantUML 勿手动添加 @startuml/@enduml（自动补全）。ECharts：传 option 的严格 JSON（键名与字符串一律双引号，不支持单引号/裸键名/…省略号缩写；容错 //注释 与尾逗号），格式化用字符串模板如 \"{b}: {c}\"" },
      html: { type: "string", description: "HTML 页面源码（与 code/path 三选一；完整文档或片段均可，自动补全为完整页面）。沙箱 iframe 域隔离预览：脚本可执行但运行在隔离源内，无法访问宿主页面 DOM/存储/顶层导航。适合网页原型、数据报表、卡片/徽章、可视化组件、带交互脚本的小页面。样式用内联 CSS，图片可用 data: URI 或外部 URL，脚本内联或外部均可" },
      path: { type: "string", description: "已有文件路径（与 code/html 三选一），按类型直显：图片内联、图表源文件（.mmd/.puml/.plantuml/.d2/.echarts）渲染成图表、.html 页面预览、文本/代码内联、其余查看/下载卡片——适合交付产物或需要用户过目的文件。会话内路径（tmp/ 前缀可省略）；本地模式也可给工作区/绝对路径，不在会话文件区内的文件会复制一份（≤100MB）到会话文件区再展示；显式传 format 可按指定图表语言渲染任意文本文件" },
      name: { type: "string", description: "展示名/产物主名（不含扩展名；未传时图表默认 diagram、HTML 默认 page、path 模式默认取文件主名）" },
      format: {
        enum: ["mermaid", "plantuml", "d2", "echarts"],
        description:
          "图表语言（code 模式必选；path 模式可选，未传时按文件扩展名推断）：\n" +
          "【mermaid】流程图/时序图/状态图/甘特图/用户旅程、Markdown 文档嵌入、简单架构；语法最简。\n" +
          "【plantuml】类图/组件图/部署图/用例图/活动图/ER 图等标准 UML 与严谨建模，功能最全。\n" +
          "【d2】系统架构/云架构/网络拓扑/微服务等对外展示场景（PPT/汇报），默认布局最现代。\n" +
          "【echarts】柱状/折线/饼图/散点/雷达/仪表盘/热力图/地图等数据可视化与统计图表；code 传 option 的严格 JSON（键名与字符串一律双引号，值禁止函数），可选信封 {\"option\": {...}, \"width\": 960, \"height\": 600} 指定画布尺寸（默认 960×600）；图例默认在画布底部，与标题同顶冲突时渲染器自动下移避让，无需手动设置 legend.top。\n" +
          "组合场景：设计文档=plantuml 类图/组件图 + mermaid 流程图；架构汇报=d2 全景架构图 + plantuml 详细组件图；数据分析=echarts 统计图表。",
      },
      render: { enum: ["frontend", "backend"], default: "frontend", description: "渲染通道（默认 frontend，首选前端渲染降低服务端负载）：frontend（浏览器本地渲染 SVG，可交互缩放、零服务端开销）/ backend（服务端渲染成 PNG 图片落盘 tmp/，仅导出/分享图片等确需 PNG 文件时使用，四语言均支持；前端渲染不可用（收到「画图能力受限」）时改用 backend 重试）" },
      width: { type: "number", description: "HTML 预览宽度（px，可选，默认铺满消息流宽度）" },
      height: { type: "number", description: "HTML 预览高度（px，可选，不传默认取会话区域高度的 2/3）" },
    },
    [],
  ),
  async execute(args, ctx) {
    const codeArg = args.code != null ? String(args.code) : ""
    const htmlArg = args.html != null ? String(args.html) : ""
    const pathArg = args.path != null ? String(args.path) : ""
    const formatArg = String(args.format ?? "")
    // format 校验前置：非法值立即报错而非静默回退 plantuml——实测复盘：漏传 format 时
    // ECharts JSON 被当 PlantUML 渲染，报「PlantUML 渲染错误」误导模型连续多轮失败
    if (formatArg && formatArg !== "mermaid" && formatArg !== "plantuml" && formatArg !== "d2" && formatArg !== "echarts") {
      return { output: `show 失败：format 参数无效（"${formatArg}"）。可选值：mermaid / plantuml / d2 / echarts。` }
    }
    if (!codeArg && !htmlArg && !pathArg.trim()) {
      return { output: "show 失败：缺少内容源——code（图表源码）/ html（HTML 源码）/ path（已有文件）三选一。" }
    }
    // ① 图表分支：code 源码（format 必选）
    if (codeArg) {
      if (!formatArg) return { output: "show 失败：code（图表源码）必须同时传 format（mermaid / plantuml / d2 / echarts）。" }
      const base = (args.name ? String(args.name) : "diagram").replace(DIAGRAM_EXT, "")
      return showDiagram(ctx, codeArg, formatArg as DiagramFormat, base, "", args.render)
    }
    // ② HTML 分支：html 源码（沙箱 iframe 预览，仅实时前端通道）
    if (htmlArg) {
      const base = (args.name ? String(args.name) : "page").replace(/\.html?$/, "")
      return showHtml(ctx, htmlArg, base, args.width, args.height)
    }
    // ③ 文件分支：path 按类型直显（归属处理 → 图片/图表/HTML/文本/文件卡片）
    const rawPath = pathArg.trim()
    const display = args.name ? String(args.name) : rawPath.replace(/[\\/]+$/, "").split(/[\\/]/).pop() || "file"
    const staged = await stageShowFile(ctx, rawPath, display)
    if ("error" in staged) return { output: staged.error }
    const { logical, abs, size, inSessionTmp, copiedBytes } = staged
    const readText = async (): Promise<string> => {
      if (copiedBytes) return new TextDecoder().decode(copiedBytes)
      return ctx.readFile(abs)
    }
    const copiedNote = inSessionTmp ? "" : `（源 ${rawPath}，已复制到会话文件区 ${logical}）`
    // path+format：按指定图表语言渲染该文件内容（不限扩展名，与 code 模式同一管线）
    if (formatArg) {
      return showDiagram(ctx, await readText(), formatArg as DiagramFormat, display.replace(/\.[^.]+$/, "") || "diagram", rawPath, args.render)
    }
    if (IMAGE_EXT.test(abs)) {
      return {
        output: `已向用户展示文件 ${display}（${logical}，${size} 字节，图片内联展示，点击可全屏查看）${copiedNote}。`,
        blocks: [{ type: "image", path: logical, name: display, mime: mimeFor(abs) }],
      }
    }
    if (DIAGRAM_EXT.test(abs)) {
      // 图表源文件：走渲染验证管线（渲染成功才返回成功；重新渲染/换通道 base 取文件主名）
      return showDiagram(ctx, await readText(), diagramFormatFor(abs) ?? "plantuml", display.replace(/\.[^.]+$/, "") || "diagram", rawPath, args.render)
    }
    if (/\.html?$/i.test(abs)) {
      if (ctx.interactionMode && ctx.interactionMode !== "realtime") {
        return { output: `show 失败：当前通道不支持 HTML 页面预览（仅 Web 前端实时会话可用）。文件本体已留存 ${logical}，可告知用户路径。` }
      }
      return {
        output: `已向用户展示文件 ${display}（${logical}，${size} 字符，HTML 页面预览展示）${copiedNote}。`,
        blocks: [htmlBlock(await readText(), display, args.width, args.height)],
      }
    }
    const blocks: ContentBlock[] = []
    let how = ""
    if (SHOW_TEXT_EXT.has(abs.split(".").pop()?.toLowerCase() ?? "") && size <= SHOW_TEXT_DIRECT_BYTES) {
      const text = await readText()
      if (text.length <= SHOW_TEXT_MAX_CHARS) {
        blocks.push({ type: "code", text, language: inferLang(display) || undefined, path: logical, name: display })
        how = `内容内联展示（${text.length} 字符）`
      } else {
        // 超长文本截断展示；全文经文件卡工具栏下载/「原文件」查看获取（files/content 按需加载），不再附独立 file 卡片
        blocks.push({
          type: "code",
          text: `${text.slice(0, SHOW_TEXT_MAX_CHARS)}\n…（文本过长已截断，全文 ${text.length} 字符可下载或点「原文件」查看）`,
          language: inferLang(display) || undefined,
          path: logical,
          name: display,
        })
        how = `内容截断内联展示（前 ${SHOW_TEXT_MAX_CHARS} 字符）`
      }
    } else {
      blocks.push({ type: "file", path: logical, name: display, mime: mimeFor(abs) })
      how = "该类型无法内联展示，已提供查看/下载卡片（点击时才加载内容）"
    }
    return { output: `已向用户展示文件 ${display}（${logical}，${size} 字节，${how}）${copiedNote}。`, blocks }
  }
}

export const globalTools: GlobalToolEntry[] = [
  { name: "show", tool: showTool },
  { name: "fetch_url", tool: fetchUrlTool },
]
