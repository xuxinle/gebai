import MarkdownIt from "markdown-it"
import type { MarkdownIt as MarkdownItInstance } from "markdown-it"
import DOMPurify from "dompurify"
import hljs from "highlight.js/lib/core"
import bash from "highlight.js/lib/languages/bash"
import python from "highlight.js/lib/languages/python"
import json from "highlight.js/lib/languages/json"
import javascript from "highlight.js/lib/languages/javascript"
import typescript from "highlight.js/lib/languages/typescript"
import css from "highlight.js/lib/languages/css"
import xml from "highlight.js/lib/languages/xml"
import markdown from "highlight.js/lib/languages/markdown"
import { el } from "./state"
import { copyText } from "./ui"
import { isLowPower } from "./low-power"

for (const [name, lang] of Object.entries({ bash, python, json, javascript, typescript, css, xml, markdown })) {
  hljs.registerLanguage(name, lang)
}

/** 代码语法高亮：lang 为空时自动检测（低性能模式跳过自动检测——highlightAuto 穷举全部语言是流式/历史加载时的重开销，改为纯转义）。 */
export function highlightCode(lang: string, code: string): string {
  try {
    if (lang && hljs.getLanguage(lang)) return hljs.highlight(code, { language: lang }).value
    if (isLowPower()) return escapePlain(code)
    return hljs.highlightAuto(code).value
  } catch {
    return escapePlain(code)
  }
}

/** 纯转义（无高亮）：低性能模式无语言标注代码块的兜底，避免 highlightAuto 的开销。 */
function escapePlain(code: string): string {
  return code.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
}

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

const md = new MarkdownIt({ html: false, linkify: true, breaks: true })

/** markdown-it 链接渲染规则：所有链接一律新标签页打开（含普通链接、autolink、linkify 自动链接），
 * 防止原地跳转打断会话使用；rel 防反向 tabnabbing。 */
export function applyLinkTargetRule(md: MarkdownItInstance): void {
  const defaultLinkOpen =
    md.renderer.rules.link_open ??
    ((tokens, idx, options, _env, self) => self.renderToken(tokens, idx, options))

  md.renderer.rules.link_open = (tokens, idx, options, env, self) => {
    const token = tokens[idx]
    token.attrSet("target", "_blank")
    token.attrSet("rel", "noopener noreferrer")
    return defaultLinkOpen(tokens, idx, options, env, self)
  }
}

applyLinkTargetRule(md)

export function renderMarkdown(text: string): string {
  // ADD_ATTR: ["target"] —— target 不在 DOMPurify 默认允许属性列表，需显式放行
  // （rel 在默认列表内；html:false + 渲染器统一注入，无其他 target 来源，无注入面）
  return DOMPurify.sanitize(md.render(text), { ADD_ATTR: ["target"] })
}

export function blockText(text: string): HTMLElement {
  return el("div", "block-text", text)
}

export function markdownBlock(text: string): HTMLElement {
  const div = el("div", "markdown")
  div.innerHTML = renderMarkdown(text)
  enhanceCodeBlocks(div)
  return div
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
