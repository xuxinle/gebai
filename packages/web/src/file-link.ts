/**
 * 文件链接 chip（「弹窗查看」模式下文件工具产物文件卡的替代渲染，DESIGN「文件展示方式」）：
 * 图标 + 文件名 + 路径（弱化展示）+ 下载；点击弹窗查看文件内容。参数区与工具输出不受影响——
 * 只有产物 file 块（文件内容卡）在「嵌入 ↔ 链接弹窗」间切换；块路径为服务端解析后的真实路径
 * （会话 tmp/ 逻辑路径或项目绝对路径），取数统一经 files/preview（按用户隔离边界解析）。
 */
import { el } from "./state"
import { downloadAnchor, openFilePreview } from "./file-card"
import type { ContentBlock } from "@gebai/sdk"

/** 文件链接 chip：click 弹窗查看；下载图标常驻（经 files/preview 附件形式获取）。 */
export function fileLinkChip(opts: { sessionId: string; name: string; path: string }): HTMLElement {
  const chip = el("div", "file-link")
  chip.dataset.path = opts.path
  chip.setAttribute("role", "button")
  chip.setAttribute("tabindex", "0")
  chip.append(el("span", "file-link-ico", "📄"), el("span", "file-link-name", opts.name), el("span", "file-link-path", opts.path))
  const open = () => openFilePreview(opts.sessionId, opts.name, opts.path)
  chip.onclick = open
  chip.addEventListener("keydown", (e) => {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault()
      open()
    }
  })
  chip.appendChild(downloadAnchor(opts.sessionId, opts.path, opts.name))
  return chip
}

/** 产物块按弹窗查看模式渲染：file 块（文件内容卡）→ 文件链接 chip，其余块（图片/图表等视觉产物）
 *  照常内联渲染。非弹窗模式下整体按原样渲染（调用方据此分流）。 */
export function renderBlocksLinked(container: HTMLElement, blocks: ContentBlock[], render: (c: HTMLElement, b: ContentBlock, sessionId: string) => void, sessionId: string): void {
  for (const b of blocks) {
    if (b.type === "file") {
      container.appendChild(fileLinkChip({ sessionId, name: b.name || b.path, path: b.path }))
      continue
    }
    render(container, b, sessionId)
  }
}
