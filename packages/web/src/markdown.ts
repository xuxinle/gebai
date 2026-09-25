import type { ContentBlock, DiagramFormat } from "@gebai/sdk"
import { DIAGRAM_LABEL, renderDiagram } from "./diagram"
import { highlightCode, renderMarkdown } from "./md-core"
import { el } from "./state"
import { copyText } from "./ui"

// 渲染核心（markdown-it 规则 / 代码高亮 / DOMPurify 净化）在 md-core.ts —— 文件工作台的 markdown 预览共用同一套。
export { highlightCode, renderMarkdown, applyLinkTargetRule, applyTaskLists, escapePlain } from "./md-core"

/** 高亮代码元素（复用：代码块/文件预览/工具参数共用）。 */
export function highlightedCode(lang: string, code: string): HTMLElement {
  const codeEl = document.createElement("code")
  codeEl.innerHTML = highlightCode(lang, code)
  return codeEl
}

/** 代码块（语法高亮，不显示语言标签）。 */
export function codeBlock(lang: string, code: string): HTMLElement {
  const pre = el("pre")
  pre.className = "tool-code"
  pre.appendChild(highlightedCode(lang, code))
  return pre
}

export function blockText(text: string): HTMLElement {
  return el("div", "block-text", text)
}

export function markdownBlock(text: string): HTMLElement {
  const div = el("div", "markdown")
  div.innerHTML = renderMarkdown(text)
  enhanceDiagramBlocks(div, text)
  enhanceCodeBlocks(div)
  return div
}

/* ---------- 图表围栏：` ```mermaid ` / ` ```plantuml ` / ` ```d2 ` / ` ```echarts ` 代码块按图表渲染 ---------- */

/** 围栏语言标记 → 图表语言（别名 mmd/puml 与 SDK DiagramFormat 对齐）；其余语言按普通代码块保留。
 *  模型常把图表源码直接写进正文而不经 show 工具，这里兜底渲染——与 show 图表块共用同一套引擎、卡片与查看器。 */
const FENCE_DIAGRAM: Record<string, DiagramFormat> = {
  mermaid: "mermaid",
  mmd: "mermaid",
  plantuml: "plantuml",
  puml: "plantuml",
  d2: "d2",
  echarts: "echarts",
}

/** 围栏语言标记 → 图表语言（大小写不敏感；非图表语言返回 null）。 */
export function fenceDiagramFormat(lang: string): DiagramFormat | null {
  return FENCE_DIAGRAM[lang.trim().toLowerCase()] ?? null
}

/** markdown 文本末尾仍未闭合的围栏语言标记（无围栏/已闭合返回 null）：流式半成品的源码仍在增长，
 *  此时渲染既无意义又反复失败，等围栏闭合后的下一次渲染再出图。 */
export function openFenceLang(text: string): string | null {
  let fence: string | null = null
  let lang = ""
  for (const line of text.split("\n")) {
    const m = /^ {0,3}(`{3,}|~{3,})\s*([^\s]*)/.exec(line)
    if (fence) {
      if (m && m[1][0] === fence[0] && m[1].length >= fence.length) fence = null
      continue
    }
    if (m) {
      fence = m[1]
      lang = m[2]
    }
  }
  return fence ? lang : null
}

/** 图表渲染回调（可注入：单测以桩替身断言替换行为，不触发真实渲染引擎）。 */
export type DiagramRenderFn = (container: HTMLElement, block: Extract<ContentBlock, { type: "diagram" }>) => unknown

function diagramFormatOf(code: Element): DiagramFormat | null {
  const m = /(?:^|\s)language-(\S+)/.exec(code.className)
  return m ? fenceDiagramFormat(m[1]) : null
}

/** 图表围栏代码块 → 图表卡片（缩略图 + 点击查看大图/源码/复制下载），替换原代码块。
 *  跳过两类：源码为空的块，以及文本末尾未闭合的那一块（必为整篇最后一个块，见 openFenceLang）。 */
export function enhanceDiagramBlocks(root: HTMLElement, text: string, render: DiagramRenderFn = renderDiagram): void {
  const codes = Array.from(root.querySelectorAll<HTMLElement>("pre > code"))
  if (!codes.length) return
  const openLang = openFenceLang(text)
  const openFormat = openLang ? fenceDiagramFormat(openLang) : null
  for (const code of codes) {
    const format = diagramFormatOf(code)
    const pre = code.parentElement
    const source = code.textContent ?? ""
    if (!format || !pre || !source.trim()) continue
    if (openFormat === format && pre === root.lastElementChild) continue
    const holder = el("div", "diagram-embed")
    pre.replaceWith(holder)
    void render(holder, { type: "diagram", format, code: source, name: DIAGRAM_LABEL[format] })
  }
}

/** 代码块复制按钮（hover 显示）。 */
export function enhanceCodeBlocks(root: HTMLElement) {
  for (const pre of root.querySelectorAll("pre")) {
    if (pre.querySelector(".copy-btn")) continue
    const btn = el("button", "copy-btn", "复制")
    btn.onclick = async () => {
      const code = pre.querySelector("code")?.textContent ?? ""
      try {
        await copyText(code)
        btn.textContent = "已复制"
        btn.classList.add("done")
        setTimeout(() => {
          btn.textContent = "复制"
          btn.classList.remove("done")
        }, 1600)
      } catch {
        /* 忽略 */
      }
    }
    pre.appendChild(btn)
  }
}
