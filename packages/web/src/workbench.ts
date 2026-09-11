/**
 * 「在文件工作台中打开」——消息流文件产物（文件卡 / 文件链接 chip / 原文件弹窗）的统一跳转入口。
 *
 * 为什么不在主界面解析根：产物路径有两类——会话相对（`tmp/` 逻辑路径）与项目绝对路径
 * （`code` 等 project 绑定工具的产物）。主界面若自行匹配「路径属于哪个根」，就得复制一份根清单
 * 与匹配规则，根的增删/改名都会让它失配。此处只把**原始路径**透传给工作台页面
 * （`/files?session=…&path=…[&line=…]`），由工作台按自有根清单定位（绝对路径取最长前缀匹配的根、
 * 相对路径落会话根）——解析逻辑只此一份，主界面无需知道工作台有哪些根。
 */
import { openFiles } from "./files-entry"

/** 「在外部窗口打开」图标（工作台按钮共用）。 */
export const ICON_WORKBENCH =
  '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' +
  '<path d="M14 4h6v6"/><path d="M20 4l-8.5 8.5"/>' +
  '<path d="M19 14.5V19a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1h4.5"/></svg>'

/** 在工作台中打开文件（新标签页）。line 为 1 起始行号，给定则打开后跳到该行。 */
export function openInWorkbench(sessionId: string, path: string, line?: number): void {
  openFiles({ path, line, session: sessionId })
}

/**
 * 工作台入口按钮（文件 chip / 文件卡工具栏 / 原文件弹窗标题栏共用）。
 * 复用 `.file-dl-icon` 的图标按钮外观（与下载按钮同款式），并 stopPropagation——
 * 外层容器（chip / 卡片）自身有点击动作，不能让子按钮的点击冒泡触发它。
 */
export function workbenchButton(sessionId: string, path: string, line?: number): HTMLButtonElement {
  const btn = document.createElement("button")
  btn.type = "button"
  btn.className = "file-dl-icon file-wb-icon"
  btn.title = "在文件工作台中打开"
  btn.setAttribute("aria-label", "在文件工作台中打开")
  btn.innerHTML = ICON_WORKBENCH
  btn.onclick = (e) => {
    e.stopPropagation()
    openInWorkbench(sessionId, path, line)
  }
  return btn
}
