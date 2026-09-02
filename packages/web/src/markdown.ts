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
import scss from "highlight.js/lib/languages/scss"
import less from "highlight.js/lib/languages/less"
import go from "highlight.js/lib/languages/go"
import rust from "highlight.js/lib/languages/rust"
import java from "highlight.js/lib/languages/java"
import kotlin from "highlight.js/lib/languages/kotlin"
import ruby from "highlight.js/lib/languages/ruby"
import c from "highlight.js/lib/languages/c"
import cpp from "highlight.js/lib/languages/cpp"
import csharp from "highlight.js/lib/languages/csharp"
import php from "highlight.js/lib/languages/php"
import sql from "highlight.js/lib/languages/sql"
import lua from "highlight.js/lib/languages/lua"
import swift from "highlight.js/lib/languages/swift"
import dart from "highlight.js/lib/languages/dart"
import { el } from "./state"
import { copyText } from "./ui"
import { isLowPower } from "./low-power"

// 语言集与 EXT_LANG（file-card.ts / 服务端 core/diff.ts）保持一致；未注册语言由 highlightAuto 兜底
for (const [name, lang] of Object.entries({
  bash, python, json, javascript, typescript, css, xml, markdown,
  scss, less, go, rust, java, kotlin, ruby, c, cpp, csharp, php, sql, lua, swift, dart,
})) {
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

/** GFM 任务列表项（`- [ ]` / `- [x]`）：markdown-it 无内置支持，`[ ]` 会泄漏为列表项字面文本——
 *  渲染后把列表项行首的 `[ ]`/`[x]` 占位转为勾选框 span（紧凑 `<li>` 与松散 `<li><p>` 两种形态；
 *  作用于 renderMarkdown 全量输出，计划审批勾选清单与模型输出中的任务列表一并生效）。 */
export function applyTaskLists(html: string): string {
  return html.replace(/(<li>)(\s*(?:<p>)?)\[( |x|X)\] /g, (_m, _li: string, prefix: string, mark: string) => {
    return `<li class="task-item">${prefix}<span class="task-box${mark.trim() ? " done" : ""}" aria-hidden="true"></span> `
  })
}

export function renderMarkdown(text: string): string {
  // ADD_ATTR: ["target"] —— target 不在 DOMPurify 默认允许属性列表，需显式放行
  // （rel 在默认列表内；html:false + 渲染器统一注入，无其他 target 来源，无注入面）
  return DOMPurify.sanitize(applyTaskLists(md.render(text)), { ADD_ATTR: ["target"] })
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
