/**
 * PowerPoint（.pptx）工具：ppt_create（pptxgenjs 生成——标题/要点/文本框/图片/表格/图表/形状，
 * 备注与背景）、ppt_read（解析幻灯片文本/表格/备注/图表与图片计数，markdown 输出）。
 * 注意 pptxgenjs 在 Bun 下的严格模式限制：文本数组一律归一为 {text, options} 对象形态（不传裸字符串数组）。
 */
import PptxGenJS from "pptxgenjs"
import type { Tool } from "../../core/base/types"
import { truncate } from "../../core/tools"
import { readPptx, unzipFiles } from "./ooxml"
import { asNum, blindOverwriteGuard, fileBlocks, fitImage, normColor, readImage, schema, writeGuards } from "./shared"

const CHART_TYPES: Record<string, string> = {
  bar: "bar",
  hbar: "bar",
  column: "bar",
  line: "line",
  area: "area",
  pie: "pie",
  doughnut: "doughnut",
  scatter: "scatter",
}

const SHAPES: Record<string, string> = {
  rect: "rect",
  rectangle: "rect",
  roundrect: "roundRect",
  ellipse: "ellipse",
  circle: "ellipse",
  triangle: "triangle",
  line: "line",
  arrow: "line",
}

const ELEMENT_TYPES = new Set(["text", "image", "table", "chart", "shape"])

/** 元素 type 缺失/未知时按字段签名推断（容错层，推断均回报 warning 引导显式声明）。 */
function inferElementType(e: Record<string, unknown>): string | null {
  if (typeof e.type === "string" && SHAPES[e.type.toLowerCase()]) return "shape"
  if (typeof e.path === "string" && e.chartType == null && !Array.isArray(e.rows)) return "image"
  if (e.chartType != null || Array.isArray(e.data)) return "chart"
  if (Array.isArray(e.rows)) return "table"
  if (e.shape != null || e.fill != null || e.lineWidth != null) return "shape"
  if (e.text != null || e.runs != null) return "text"
  return null
}

function color(v: unknown, def: string): string {
  return normColor(v) ?? def
}

/** 文本 run 归一：字符串/对象数组 → pptxgenjs {text, options} 数组（Bun 严格模式兼容形态）。 */
function toTextItems(v: unknown, base: Record<string, unknown>): Array<{ text: string; options: Record<string, unknown> }> {
  const arr = Array.isArray(v) ? v : [v]
  const out: Array<{ text: string; options: Record<string, unknown> }> = []
  for (const item of arr) {
    if (typeof item === "string" || typeof item === "number") {
      if (String(item) !== "") out.push({ text: String(item), options: { ...base } })
      continue
    }
    if (!item || typeof item !== "object") continue
    const o = item as Record<string, unknown>
    if (o.text == null || String(o.text) === "") continue
    const opts: Record<string, unknown> = { ...base }
    if (o.bold === true) opts.bold = true
    if (o.italic === true) opts.italic = true
    if (o.fontSize != null) opts.fontSize = asNum(o.fontSize, 18)
    const c = normColor(o.color)
    if (c) opts.color = c
    if (o.href) opts.hyperlink = { url: String(o.href) }
    if (o.level != null) opts.indentLevel = Math.max(0, Math.min(8, Math.round(asNum(o.level, 0))))
    out.push({ text: String(o.text), options: opts })
  }
  return out
}

function boxOpts(o: Record<string, unknown>, base: Record<string, unknown>): Record<string, unknown> {
  const opts: Record<string, unknown> = { ...base }
  for (const k of ["x", "y", "w", "h"]) if (o[k] != null) opts[k] = asNum(o[k], 1)
  if (o.fontSize != null) opts.fontSize = asNum(o.fontSize, 18)
  if (o.bold === true) opts.bold = true
  if (o.italic === true) opts.italic = true
  const c = normColor(o.color)
  if (c) opts.color = c
  if (typeof o.fontFace === "string" && o.fontFace.trim()) opts.fontFace = o.fontFace.trim()
  if (o.align === "left" || o.align === "center" || o.align === "right") opts.align = o.align
  if (o.valign === "top" || o.valign === "middle" || o.valign === "bottom") opts.valign = o.valign
  if (o.lineSpacingMultiple != null) opts.lineSpacingMultiple = asNum(o.lineSpacingMultiple, 1)
  if (o.wrap === false) opts.wrap = false
  return opts
}

export const pptCreateTool: Tool = {
  name: "ppt_create",
  description:
    "创建 .pptx 演示文稿。slides 数组每页两种写法：简式 {title, subtitle?, bullets?: [文本或 {text,level,bold,color}], notes?, background?}（标题顶部+要点正文的标准版式）或全式 {elements: [...]} 自由布局（坐标英寸）。元素 type：text（text/runs + 样式）、image（path 嵌入会话/项目内图片）、table（rows 二维表，首行默认加粗底纹）、chart（chartType: bar/hbar/line/area/pie/doughnut/scatter + data: [{name,labels,values}]）、shape（shape: rect（默认）/roundRect/ellipse/line/arrow + fill/line/text）。type 缺失或无法识别时按字段推断（path→image、chartType/data→chart、rows→table、形状名/fill→shape、text→text）并在输出提示；页级误传元素对象、或携带元素级字段（chartType/data/rows/path）未包 elements 数组时自动按附加元素处理并提示。layout: wide（默认 16:9 13.33×7.5）/ 4x3 / {width,height}；theme 调全局字体字号（默认微软雅黑，标题 30/正文 18）。已有文件本会话未读取过时拒绝（防盲覆盖）。旧版 .ppt 不支持。",
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "输出路径（.pptx）" },
      slides: { type: "array", description: "幻灯片数组：[{title, subtitle, bullets, notes, background, elements}]，详见工具描述" },
      layout: { type: "string", description: "页面布局：wide（默认，16:9 13.33×7.5 英寸）/ 4x3，或传 {width,height} 对象（英寸）" },
      theme: { type: "object", description: "全局主题：{fontFace, titleFontSize, bodyFontSize, titleColor, bodyColor}" },
      meta: { type: "object", description: "文档属性：{title, author, company, subject}" },
    },
    ["path", "slides"],
  ),
  safeMode: false,
  async execute(args, ctx) {
    const abs = ctx.resolvePath(String(args.path))
    const guardMsg = await writeGuards(abs, ctx)
    if (guardMsg) return { output: guardMsg }
    const blindMsg = await blindOverwriteGuard(abs, String(args.path), ctx)
    if (blindMsg) return { output: blindMsg }
    if (!Array.isArray(args.slides) || !args.slides.length) return { output: "slides 参数须为非空数组（至少一页）。" }

    const themeRaw = args.theme && typeof args.theme === "object" ? (args.theme as Record<string, unknown>) : {}
    const theme = {
      fontFace: typeof themeRaw.fontFace === "string" && themeRaw.fontFace.trim() ? themeRaw.fontFace.trim() : "Microsoft YaHei",
      titleFontSize: asNum(themeRaw.titleFontSize, 30),
      bodyFontSize: asNum(themeRaw.bodyFontSize, 18),
      titleColor: color(themeRaw.titleColor, "1F3864"),
      bodyColor: color(themeRaw.bodyColor, "333333"),
    }
    const meta = args.meta && typeof args.meta === "object" ? (args.meta as Record<string, unknown>) : {}

    const pptx = new PptxGenJS()
    let layoutName = "LAYOUT_WIDE"
    if (args.layout === "4x3" || args.layout === "4:3") {
      pptx.layout = "LAYOUT_4x3"
      layoutName = "LAYOUT_4x3"
    } else if (args.layout && typeof args.layout === "object") {
      const lo = args.layout as Record<string, unknown>
      const w = asNum(lo.width, 13.33)
      const h = asNum(lo.height, 7.5)
      pptx.defineLayout({ name: "GEBAI_CUSTOM", width: w, height: h })
      pptx.layout = "GEBAI_CUSTOM"
      layoutName = `${w}×${h}in`
    } else {
      pptx.layout = "LAYOUT_WIDE"
    }
    if (typeof meta.title === "string") pptx.title = meta.title
    if (typeof meta.author === "string") pptx.author = meta.author
    if (typeof meta.company === "string") pptx.company = meta.company
    if (typeof meta.subject === "string") pptx.subject = meta.subject

    const warnings: string[] = []
    let slideCount = 0 // pptxgenjs 的 slides 属性为私有类型，自行计数
    let chartCount = 0
    let imageCount = 0
    const slideW = pptx.presLayout?.width ?? 13.33
    const slideH = pptx.presLayout?.height ?? 7.5

    for (const raw of args.slides) {
      if (!raw || typeof raw !== "object") continue
      let s = raw as Record<string, unknown>
      // 容错：页级直接传了元素形态——带 type 字段，或携带元素级字段签名（chartType/data/rows/path 等）
      // 但未包 elements 数组——按单元素/附加元素页包裹处理（页对象没有这些字段，出现即误传；不静默吞掉）
      if (!Array.isArray(s.elements) && (typeof s.type === "string" || inferElementType(s))) {
        warnings.push("页对象携带元素级字段但未包 elements 数组——已按附加元素处理，建议包进 {elements: [...]}")
        s = { ...s, elements: [s] }
      }
      const slide = pptx.addSlide()
      slideCount++
      if (s.background && typeof s.background === "object") {
        const bg = s.background as Record<string, unknown>
        if (typeof bg.color === "string") slide.background = { color: color(bg.color, "FFFFFF") }
        else if (typeof bg.path === "string") {
          try {
            const { bytes, type } = await readImage(bg.path, ctx)
            slide.background = { data: `image/${type === "jpg" ? "jpeg" : type};base64,${Buffer.from(bytes).toString("base64")}` }
          } catch (err) {
            warnings.push(`背景图 ${bg.path} 嵌入失败：${(err as Error).message}`)
          }
        }
      }
      // 简式版式：标题 + 副标题/要点正文
      if (s.title != null && String(s.title) !== "") {
        slide.addText(String(s.title), {
          x: 0.6,
          y: 0.35,
          w: slideW - 1.2,
          h: 0.95,
          fontSize: theme.titleFontSize,
          bold: true,
          color: theme.titleColor,
          fontFace: theme.fontFace,
        })
      }
      if (s.subtitle != null && String(s.subtitle) !== "") {
        slide.addText(String(s.subtitle), { x: 0.9, y: 1.3, w: slideW - 1.8, h: 0.5, fontSize: theme.bodyFontSize - 4, color: "666666", fontFace: theme.fontFace })
      }
      if (s.bullets != null) {
        const items = toTextItems(s.bullets, { bullet: true, fontSize: theme.bodyFontSize, color: theme.bodyColor, fontFace: theme.fontFace })
        if (items.length) slide.addText(items, { x: 0.9, y: 1.85, w: slideW - 1.8, h: slideH - 2.4, valign: "top" })
      }
      if (typeof s.notes === "string" && s.notes.trim()) slide.addNotes(s.notes)

      for (const el of Array.isArray(s.elements) ? s.elements : []) {
        if (!el || typeof el !== "object") continue
        const e = el as Record<string, unknown>
        try {
          // 容错：type 缺失/无法识别时按字段签名推断（path→image、chartType/data→chart、rows→table、
          // shape/fill/line→shape、text/runs→text），推断成功回报提示而非静默跳过
          let type = String(e.type ?? "")
          if (!ELEMENT_TYPES.has(type)) {
            const inferred = inferElementType(e)
            if (inferred) {
              warnings.push(`元素 type "${type || "缺失"}" 无法识别，按字段推断为 ${inferred}（建议显式声明 type）`)
              type = inferred
            } else {
              warnings.push(`未知元素 type: ${type || "(空)"}（支持 text/image/table/chart/shape）`)
              continue
            }
          }
          if (type === "text") {
            const base = boxOpts(e, { fontSize: theme.bodyFontSize, color: theme.bodyColor, fontFace: theme.fontFace })
            if (e.bullet === true) base.bullet = true
            const items = toTextItems(e.runs ?? e.text, {})
            if (!items.length) {
              warnings.push("text 元素内容为空，已跳过")
              continue
            }
            slide.addText(items, { ...base, ...(e.bullet === true ? { bullet: { indent: 12 } } : {}), margin: 4 })
          } else if (type === "image") {
            if (typeof e.path !== "string") throw new Error("image 元素缺少 path")
            const { bytes, type: it } = await readImage(e.path, ctx)
            imageCount++
            const fit = fitImage(bytes, it, e.w != null ? asNum(e.w, 4) : undefined, e.h != null ? asNum(e.h, 3) : undefined)
            slide.addImage({
              data: `image/${it === "jpg" ? "jpeg" : it};base64,${Buffer.from(bytes).toString("base64")}`,
              x: asNum(e.x, 1),
              y: asNum(e.y, 1),
              w: e.w != null ? asNum(e.w, 4) : Math.min(fit.width / 96, slideW - 2),
              h: e.h != null ? asNum(e.h, 3) : Math.min(fit.height / 96, slideH - 2),
            })
          } else if (type === "table") {
            const rowsRaw = Array.isArray(e.rows) ? e.rows : []
            if (!rowsRaw.length) throw new Error("table 元素缺少 rows")
            const headerBold = e.headerBold !== false
            const rows = rowsRaw.map((r: unknown, ri: number) =>
              (Array.isArray(r) ? r : [r]).map((c: unknown) => {
                if (c && typeof c === "object") {
                  const o = c as Record<string, unknown>
                  return { text: String(o.text ?? ""), options: { bold: o.bold === true || (ri === 0 && headerBold), ...(o.align ? { align: o.align } : {}) } }
                }
                return { text: String(c ?? ""), options: ri === 0 && headerBold ? { bold: true, fill: { color: "EEF2F8" } } : {} }
              }),
            )
            const opts: Record<string, unknown> = { x: asNum(e.x, 1), y: asNum(e.y, 1), fontSize: asNum(e.fontSize, 12), border: { pt: 0.5, color: "C9C9C9" }, fontFace: theme.fontFace, color: theme.bodyColor }
            if (e.w != null) opts.w = asNum(e.w, 8)
            if (e.h != null) opts.h = asNum(e.h, 3)
            if (Array.isArray(e.colW)) opts.colW = e.colW.map((w: unknown) => asNum(w, 1))
            slide.addTable(rows as never, opts as never)
          } else if (type === "chart") {
            const ct = CHART_TYPES[String(e.chartType ?? "bar").toLowerCase()]
            if (!ct) throw new Error(`未知 chartType: ${String(e.chartType)}（支持 bar/hbar/line/area/pie/doughnut/scatter）`)
            const dataRaw = Array.isArray(e.data) ? e.data : []
            const data = dataRaw.map((d: unknown) => {
              const o = d && typeof d === "object" ? (d as Record<string, unknown>) : {}
              return {
                name: String(o.name ?? "系列"),
                labels: Array.isArray(o.labels) ? o.labels.map((l: unknown) => String(l)) : [],
                values: Array.isArray(o.values) ? o.values.map((v: unknown) => asNum(v, 0)) : [],
              }
            })
            if (!data.length || !data.some((d) => d.values.length)) throw new Error("chart 元素缺少 data（[{name,labels,values}]）")
            chartCount++
            const opts: Record<string, unknown> = {
              x: asNum(e.x, 1),
              y: asNum(e.y, 1),
              w: asNum(e.w, 6),
              h: asNum(e.h, 4),
              showLegend: e.legend != null ? e.legend === true : data.length > 1 || data[0].name !== "系列",
            }
            if (typeof e.title === "string" && e.title.trim()) opts.title = e.title
            if (e.catAxisTitle) opts.catAxisTitle = String(e.catAxisTitle)
            if (e.valAxisTitle) opts.valAxisTitle = String(e.valAxisTitle)
            if (ct === "bar" && String(e.chartType).toLowerCase() === "hbar") opts.barDir = "bar"
            const chartData = ct === "scatter" ? [{ name: "X", values: data[0].labels.length ? data[0].labels.map(Number) : data[0].values.map((_, i) => i + 1) }, ...data] : data
            slide.addChart(ct as never, chartData as never, opts as never)
          } else if (type === "shape") {
            const shape = SHAPES[String(e.shape ?? "rect").toLowerCase()]
            if (!shape) throw new Error(`未知 shape: ${String(e.shape)}`)
            const isArrow = String(e.shape).toLowerCase() === "arrow"
            const opts: Record<string, unknown> = { x: asNum(e.x, 1), y: asNum(e.y, 1), w: asNum(e.w, 3), h: asNum(e.h, 2) }
            if (e.fill != null) opts.fill = { color: color(e.fill, "4472C4") }
            else opts.fill = { color: "4472C4" }
            if (e.line != null) opts.line = { color: color(e.line, "333333"), width: asNum(e.lineWidth, 1) }
            if (isArrow) opts.line = { ...(opts.line as Record<string, unknown>), endArrowType: "triangle" }
            if (e.text != null && String(e.text) !== "") {
              slide.addText(String(e.text), {
                shape: shape as never,
                ...opts,
                align: "center",
                valign: "middle",
                fontSize: asNum(e.fontSize, theme.bodyFontSize),
                color: e.textColor != null ? color(e.textColor, "FFFFFF") : "FFFFFF",
                fontFace: theme.fontFace,
              } as never)
            } else {
              slide.addShape(shape as never, opts as never)
            }
          } else {
            warnings.push(`未知元素 type: ${type || "(空)"}（支持 text/image/table/chart/shape）`)
          }
        } catch (err) {
          warnings.push(`元素处理失败：${(err as Error).message}`)
        }
      }
    }
    if (!slideCount) return { output: "slides 参数中没有有效幻灯片。" }

    const buf = (await pptx.write({ outputType: "nodebuffer" })) as Buffer
    await ctx.writeBinaryFile!(abs, new Uint8Array(buf))
    ctx.fileGuard?.markRead(abs)

    const sizeNote = layoutName === "LAYOUT_WIDE" ? "16:9（13.33×7.5 英寸）" : layoutName === "LAYOUT_4x3" ? "4:3（10×7.5 英寸）" : layoutName
    return {
      output: `已创建 ${args.path}（${slideCount} 页 · ${sizeNote}${chartCount ? ` · 图表×${chartCount}` : ""}${imageCount ? ` · 图片×${imageCount}` : ""}）——前端文件面板可下载。${warnings.length ? `\n注意：\n- ${warnings.join("\n- ")}` : ""}`,
      blocks: fileBlocks(abs, ctx),
    }
  },
}

// ---------------------------------------------------------------------------
// ppt_read
// ---------------------------------------------------------------------------

export const pptReadTool: Tool = {
  name: "ppt_read",
  description:
    "读取 .pptx 结构为 markdown：每页文本框（标题/正文）、表格、备注、图表与图片计数；首行给页面尺寸与页数摘要。maxSlides（默认 40）截断长演示。旧版 .ppt 不支持（请先另存为 .pptx）。",
  card: { titleParams: ["path"], file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "演示文稿路径（.pptx）" },
      maxSlides: { type: "integer", description: "最多读取页数（默认 40）" },
    },
    ["path"],
  ),
  async execute(args, ctx) {
    const abs = ctx.resolvePath(String(args.path))
    let bytes: Uint8Array
    try {
      bytes = await ctx.readBinaryFile(abs)
    } catch {
      return { output: `ppt_read 失败：文件不存在或不可读（${args.path}）。目标在项目内时用 project 参数指定项目。` }
    }
    let read
    try {
      read = readPptx(unzipFiles(bytes))
    } catch (err) {
      return { output: `ppt_read 失败：${(err as Error).message}——可能是旧版 .ppt 或损坏文件，请先在 Office/WPS 中另存为 .pptx。` }
    }
    ctx.fileGuard?.markRead(abs)

    const maxSlides = Math.max(1, Math.round(asNum(args.maxSlides, 40)))
    const total = read.slides.length
    const chartTotal = read.slides.filter((s) => s.hasChart).length
    const imageTotal = read.slides.reduce((n, s) => n + s.imageCount, 0)
    const lines: string[] = [`（${read.widthIn.toFixed(2)}×${read.heightIn.toFixed(2)} 英寸 · ${total} 页 · 图表×${chartTotal} · 图片×${imageTotal}）`]

    read.slides.slice(0, maxSlides).forEach((sl, i) => {
      lines.push("", `## 第 ${i + 1} 页`)
      // 首个文本框按惯例是页标题
      sl.texts.forEach((t, ti) => lines.push(ti === 0 ? `- 【标题】${t.replace(/\n/g, " / ")}` : `- ${t.replace(/\n/g, " ⏎ ")}`))
      for (const tbl of sl.tables) {
        lines.push(`- 表格 ${tbl.length}×${tbl[0]?.length ?? 0}：${tbl.map((r) => r.join(" | ")).join(" ⏎ ")}`)
      }
      if (sl.hasChart) lines.push("- 图表")
      if (sl.notes) lines.push(`- 备注：${sl.notes}`)
      if (!sl.texts.length && !sl.tables.length && !sl.hasChart && !sl.imageCount) lines.push("- （空页）")
      else if (!sl.texts.length && sl.imageCount) lines.push(`- （图片页，${sl.imageCount} 张图）`)
    })
    if (total > maxSlides) lines.push(`（已显示前 ${maxSlides} 页，共 ${total} 页——maxSlides 调大继续读）`)
    const truncated = await truncate(lines.join("\n"), "ppt_read", ctx)
    return { ...truncated, blocks: fileBlocks(abs, ctx) }
  },
}
