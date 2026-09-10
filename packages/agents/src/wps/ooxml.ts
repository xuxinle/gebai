/**
 * OOXML 读取基础设施（wps 子Agent 共用）：.docx/.pptx 是 ZIP 容器 + XML 部件，
 * 本文件负责解包（fflate）、XML 解析（happy-dom DOMParser，text/xml 模式支持命名空间前缀）、
 * 以及 docx 正文 / pptx 幻灯片的读取模型（word_read / ppt_read / word_append 的解析基础）。
 */
import { Window } from "happy-dom"
import type { Document, Element } from "happy-dom"
import { strFromU8, unzipSync, zipSync } from "fflate"

// DOMParser 无文档级状态，模块级共享一个 Window 实例即可（与 mermaid 的 happy-dom 垫层互不影响）
const domWindow = new Window()

export function parseXml(text: string): Document {
  // 兼容性规范化：python-docx 等库写出的 XML 声明用单引号（<?xml version='1.0' ...?>），
  // happy-dom 的 text/xml 解析器不识别，会静默降级为 HTML 解析（根元素变 HTML/BODY），
  // 导致所有按命名空间前缀的标签查询落空。解析前统一把声明改为双引号形式。
  const normalized = text.replace(/^\s*(<\?xml[\s\S]*?\?>)/, (decl) => decl.replace(/'/g, '"'))
  const doc = new domWindow.DOMParser().parseFromString(normalized, "text/xml")
  // 防静默降级兜底：输入是 XML 却解析出 HTML 根元素 → 显式报错，避免下游误诊为「文件损坏/需另存」
  if (/^\s*<\?xml/.test(text) && doc.documentElement?.tagName === "HTML") {
    throw new Error(`XML 解析失败（解析器降级为 HTML）：${text.slice(0, 120)}`)
  }
  return doc
}

export function unzipFiles(bytes: Uint8Array): Record<string, Uint8Array> {
  return unzipSync(bytes)
}

export function zipFiles(files: Record<string, Uint8Array>): Uint8Array {
  return zipSync(files, { level: 6 })
}

export function xmlStr(bytes: Uint8Array): string {
  return strFromU8(bytes)
}

export function escapeXml(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&apos;")
}

/** 按标签名取直接/全部后代元素列表（happy-dom getElementsByTagName 为全后代搜索）。 */
export function tagAll(root: Element | Document, name: string): Element[] {
  return [...root.getElementsByTagName(name)]
}

function tagAttr(el: Element | null, name: string): string | null {
  return el ? el.getAttribute(name) : null
}

// ---------------------------------------------------------------------------
// docx 读取模型
// ---------------------------------------------------------------------------

/** docx 正文块（word_read 渲染 markdown 用；ordered 列表由渲染层按连续同组计数）。 */
export type DocxBlock =
  | { kind: "heading"; text: string; level: number }
  | { kind: "paragraph"; text: string }
  | { kind: "listItem"; text: string; ordered: boolean; level: number; group: number }
  | { kind: "table"; rows: string[][] }

export interface DocxRead {
  blocks: DocxBlock[]
  mediaCount: number
  /** 正文节页面尺寸（twips）与方向（body 级 sectPr 缺省时为 null）。 */
  page?: { widthTw: number; heightTw: number; landscape: boolean }
}

/** 解析 docx ZIP 部件为正文块序列：w:p（标题样式/编号/文本）与 w:tbl（表格），内嵌图片以 [图片] 占位。 */
export function readDocx(files: Record<string, Uint8Array>): DocxRead {
  const docBytes = files["word/document.xml"]
  if (!docBytes) throw new Error("缺少 word/document.xml 部件（不是有效的 .docx 文件）")
  const doc = parseXml(xmlStr(docBytes))
  const body = doc.getElementsByTagName("w:body")[0]
  if (!body) throw new Error("document.xml 缺少 w:body")

  // numbering.xml：numId → 一级编号格式（bullet=无序，其余按有序渲染）
  const orderedByNumId = new Map<number, boolean>()
  const numBytes = files["word/numbering.xml"]
  if (numBytes) {
    const abstractFmt = new Map<string, boolean>()
    for (const an of tagAll(parseXml(xmlStr(numBytes)), "w:abstractNum")) {
      const lvl0 = tagAll(an, "w:lvl").find((l) => l.getAttribute("w:ilvl") === "0") ?? tagAll(an, "w:lvl")[0]
      const fmt = lvl0 ? tagAll(lvl0, "w:numFmt")[0]?.getAttribute("w:val") : null
      abstractFmt.set(an.getAttribute("w:abstractNumId") ?? "", fmt !== "bullet")
    }
    for (const num of tagAll(parseXml(xmlStr(numBytes)), "w:num")) {
      const abs = tagAll(num, "w:abstractNumId")[0]?.getAttribute("w:val") ?? ""
      orderedByNumId.set(Number(num.getAttribute("w:numId")), abstractFmt.get(abs) ?? true)
    }
  }

  // 目录条目（word/media/ 本身）不计——zip 容器可能带目录项
  const mediaCount = Object.keys(files).filter((k) => k.startsWith("word/media/") && !k.endsWith("/")).length
  const blocks: DocxBlock[] = []
  for (const child of [...body.children]) {
    if (child.tagName === "w:p") {
      // happy-dom 的 children 是 HTMLCollection（数组方法不可用），转真数组后查找
      const pPr = [...child.children].find((c) => c.tagName === "w:pPr") ?? null
      const style = tagAttr(tagAll(child, "w:pStyle")[0] ?? null, "w:val") ?? ""
      const text = paragraphText(child)
      const numPr = pPr ? tagAll(pPr, "w:numPr")[0] ?? null : null
      const hm = /^(?:Heading|标题)\s*(\d)$/i.exec(style)
      if (hm) {
        blocks.push({ kind: "heading", text, level: Math.min(9, Number(hm[1])) })
      } else if (style.toLowerCase() === "title") {
        blocks.push({ kind: "heading", text, level: 1 })
      } else if (numPr) {
        const numId = Number(tagAttr(tagAll(numPr, "w:numId")[0] ?? null, "w:val") ?? "0")
        const ilvl = Number(tagAttr(tagAll(numPr, "w:ilvl")[0] ?? null, "w:val") ?? "0")
        blocks.push({ kind: "listItem", text, ordered: orderedByNumId.get(numId) ?? true, level: ilvl, group: numId })
      } else if (text.trim()) {
        blocks.push({ kind: "paragraph", text })
      }
    } else if (child.tagName === "w:tbl") {
      const rows = tagAll(child, "w:tr").map((tr) =>
        tagAll(tr, "w:tc").map((tc) => tagAll(tc, "w:p").map(paragraphText).filter((t) => t.trim()).join(" ")),
      )
      blocks.push({ kind: "table", rows })
    }
  }
  return { blocks, mediaCount, page: readSection(body) }
}

/** 段落纯文本：w:t 拼接，w:tab→制表符、w:br/w:cr→换行、w:drawing/w:pict→[图片] 占位。 */
function paragraphText(p: Element): string {
  let out = ""
  const walk = (el: Element) => {
    for (const c of el.children) {
      if (c.tagName === "w:t") out += c.textContent ?? ""
      else if (c.tagName === "w:tab") out += "\t"
      else if (c.tagName === "w:br" || c.tagName === "w:cr") out += "\n"
      else if (c.tagName === "w:drawing" || c.tagName === "w:pict") out += "[图片]"
      else walk(c)
    }
  }
  walk(p)
  return out
}

function readSection(body: Element): DocxRead["page"] {
  const sectPr = [...body.children].reverse().find((c) => c.tagName === "w:sectPr")
  const pgSz = sectPr ? tagAll(sectPr, "w:pgSz")[0] : undefined
  if (!pgSz) return undefined
  const w = Number(pgSz.getAttribute("w:w") ?? "0")
  const h = Number(pgSz.getAttribute("w:h") ?? "0")
  if (!w || !h) return undefined
  return { widthTw: w, heightTw: h, landscape: pgSz.getAttribute("w:orient") === "landscape" }
}

// ---------------------------------------------------------------------------
// pptx 读取模型
// ---------------------------------------------------------------------------

export interface PptxSlide {
  /** 各文本框文本（shape 内多段落已按换行拆分）。 */
  texts: string[]
  tables: string[][][]
  notes: string
  hasChart: boolean
  imageCount: number
}

export interface PptxRead {
  widthIn: number
  heightIn: number
  slides: PptxSlide[]
}

/** 解析 pptx ZIP 部件：幻灯片按文件序号排序，文本/表格/备注/图表与图片计数（来自各 slide 的 rels）。 */
export function readPptx(files: Record<string, Uint8Array>): PptxRead {
  const presBytes = files["ppt/presentation.xml"]
  if (!presBytes) throw new Error("缺少 ppt/presentation.xml 部件（不是有效的 .pptx 文件）")
  const pres = parseXml(xmlStr(presBytes))
  const sldSz = tagAll(pres, "p:sldSz")[0]
  const EMU = 914400
  const widthIn = sldSz ? Number(sldSz.getAttribute("cx") ?? "0") / EMU : 13.33
  const heightIn = sldSz ? Number(sldSz.getAttribute("cy") ?? "0") / EMU : 7.5

  const slideKeys = Object.keys(files)
    .filter((k) => /^ppt\/slides\/slide\d+\.xml$/.test(k))
    .sort((a, b) => slideNo(a) - slideNo(b))
  const slides: PptxSlide[] = slideKeys.map((key) => {
    const slide = parseXml(xmlStr(files[key]))
    const texts: string[] = []
    for (const sp of tagAll(slide, "p:sp")) {
      const text = tagAll(sp, "a:p")
        .map((p) => tagAll(p, "a:t").map((t) => t.textContent ?? "").join(""))
        .filter((s) => s.trim())
        .join("\n")
      if (text.trim()) texts.push(text)
    }
    const tables = tagAll(slide, "a:tbl").map((tbl) =>
      tagAll(tbl, "a:tr").map((tr) =>
        tagAll(tr, "a:tc").map((tc) => tagAll(tc, "a:t").map((t) => t.textContent ?? "").join("").trim()),
      ),
    )
    // 图表与图片经 slide 的 rels 关系判定（类型后缀 /chart、/image）
    let hasChart = false
    let imageCount = 0
    let notes = ""
    const relKey = key.replace("ppt/slides/", "ppt/slides/_rels/") + ".rels"
    const rels = files[relKey] ? parseXml(xmlStr(files[relKey])) : null
    if (rels) {
      for (const rel of tagAll(rels, "Relationship")) {
        const type = rel.getAttribute("Type") ?? ""
        if (type.endsWith("/chart")) hasChart = true
        else if (type.endsWith("/image")) imageCount++
        else if (type.endsWith("/notesSlide")) {
          const target = (rel.getAttribute("Target") ?? "").replace(/^\.\.\//, "ppt/")
          const notesBytes = files[target]
          if (notesBytes) {
            // 备注页含页码占位符（ph type=sldNum），只取正文占位文本
            const notesDoc = parseXml(xmlStr(notesBytes))
            const parts: string[] = []
            for (const sp of tagAll(notesDoc, "p:sp")) {
              if (tagAll(sp, "p:ph").some((ph) => ph.getAttribute("type") === "sldNum")) continue
              const t = tagAll(sp, "a:t").map((n) => n.textContent ?? "").join("")
              if (t.trim()) parts.push(t)
            }
            notes = parts.join("\n").trim()
          }
        }
      }
    }
    return { texts, tables, notes, hasChart, imageCount }
  })
  return { widthIn, heightIn, slides }
}

function slideNo(key: string): number {
  return Number(/slide(\d+)\.xml$/.exec(key)?.[1] ?? "0")
}
