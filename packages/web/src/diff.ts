import type { ContentBlock, DiffLine } from "@gebai/sdk"
import { el } from "./state"
import { highlightCode, codeBlock } from "./markdown"

/* ---------- diff 内容块：并排对比渲染（按文本类型语法高亮） ---------- */

/**
 * 整块语法高亮后的 HTML 按行拆分，保持跨行 span（注释/字符串等）闭合平衡：
 * 每行行首重开未闭合的 span，行尾补闭合，避免跨行着色截断导致样式错乱。
 */
function splitHighlightedLines(html: string): string[] {
  const open: string[] = []
  const re = /<span class="[^"]*">|<\/span>/g
  return html.split("\n").map((raw) => {
    const prefix = open.join("")
    let m: RegExpExecArray | null
    while ((m = re.exec(raw))) {
      if (m[0] === "</span>") open.pop()
      else open.push(m[0])
    }
    return prefix + raw + "</span>".repeat(open.length)
  })
}

/** 按「连续同类型段」整段高亮再切行：跨行注释/字符串在同类段内着色正确。 */
function highlightRows(lines: DiffLine[], language: string | undefined): string[] {
  const rows: string[] = []
  let i = 0
  while (i < lines.length) {
    const kind = lines[i].kind
    const buf: string[] = []
    while (i < lines.length && lines[i].kind === kind) buf.push(lines[i++].text)
    const html = highlightCode(language ?? "", buf.join("\n"))
    const split = splitHighlightedLines(html)
    for (let k = 0; k < buf.length; k++) rows.push(split[k] ?? "")
  }
  return rows
}

/** 单行渲染：行号 + 高亮代码。 */
function lineSpan(no: number, html: string): HTMLElement {
  const span = el("span", "diff-ln", String(no))
  const code = document.createElement("code")
  code.className = "diff-code"
  code.innerHTML = html
  const cell = el("div", "diff-cell")
  cell.append(span, code)
  return cell
}

/** 并排对比视图（缺 lines 的旧数据降级为两个高亮代码块）。 */
export function renderDiffBlock(container: HTMLElement, b: Extract<ContentBlock, { type: "diff" }>) {
  const view = el("div", "diff-view")
  const head = el("div", "diff-head")
  head.appendChild(el("span", "diff-title", b.name || "文本对比"))
  if (b.language) head.appendChild(el("span", "diff-lang", b.language))
  view.appendChild(head)

  if (!b.lines?.length) {
    // 降级：无行级数据时展示原始两侧文本
    const panes = el("div", "diff-panes")
    for (const [label, text] of [[b.oldName || "旧", b.oldText], [b.newName || "新", b.newText]] as const) {
      const pane = el("div", "diff-pane")
      pane.appendChild(el("div", "diff-pane-head", label))
      const code = codeBlock(b.language ?? "", text)
      code.style.margin = "0"
      code.style.border = "none"
      pane.appendChild(code)
      panes.appendChild(pane)
    }
    view.appendChild(panes)
    container.appendChild(view)
    return
  }

  const panes = el("div", "diff-panes")
  for (const label of [b.oldName || "旧", b.newName || "新"] as const) {
    const pane = el("div", "diff-pane")
    pane.appendChild(el("div", "diff-pane-head", label))
    panes.appendChild(pane)
  }
  view.appendChild(panes)

  const body = el("div", "diff-body")
  const rows = highlightRows(b.lines, b.language)
  let oldNo = 0
  let newNo = 0
  b.lines.forEach((l, idx) => {
    const row = el("div", `diff-row ${l.kind === "add" ? "diff-add" : l.kind === "del" ? "diff-del" : "diff-eq"}`)
    const html = rows[idx] ?? ""
    if (l.kind !== "add") oldNo++
    if (l.kind !== "del") newNo++
    row.append(
      l.kind === "add" ? el("div", "diff-cell diff-placeholder") : lineSpan(oldNo, html),
      l.kind === "del" ? el("div", "diff-cell diff-placeholder") : lineSpan(newNo, html),
    )
    body.appendChild(row)
  })
  view.appendChild(body)
  container.appendChild(view)
}
