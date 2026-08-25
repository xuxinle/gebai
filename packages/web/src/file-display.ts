/**
 * 文件展示方式（read/write/edit/patch 等文件工具的**产物文件卡**，含 code 子Agent 同款工具）：
 * - inline 嵌入展示（默认，现状）：产物 file 块渲染为文件内容卡（按需加载内容内联展示）
 * - popup 弹窗查看：产物 file 块收敛为文件链接 chip，点击弹窗查看（会话相对与项目绝对路径均经 files/preview 取数）
 * - 参数区与工具输出不受影响（只有下方产物文件卡在两种形态间切换）
 * - 手动设置：localStorage `gebai.ui.fileDisplay` = "popup"；不存/其它值 = inline
 * - 生效方式：渲染时经 isFilePopup() 读取；变更派发 `gebai:file-display-change`（main 重载当前会话消息）
 */

export type FileDisplaySetting = "inline" | "popup"

const KEY = "gebai.ui.fileDisplay"

/** 用户设置（默认 inline 直接展示）。 */
export function getFileDisplaySetting(): FileDisplaySetting {
  try {
    if (localStorage.getItem(KEY) === "popup") return "popup"
  } catch {
    /* 隐私模式等场景忽略 */
  }
  return "inline"
}

/** 当前是否弹窗查看模式（文件内容收敛为链接）。 */
export function isFilePopup(): boolean {
  return getFileDisplaySetting() === "popup"
}

/** 手动设置（设置面板「外观」）：持久化 + 派发变更事件（已渲染消息由 main 重载当前会话）。 */
export function setFileDisplaySetting(v: FileDisplaySetting): void {
  try {
    if (v === "popup") localStorage.setItem(KEY, v)
    else localStorage.removeItem(KEY) // inline = 默认直接展示，不留冗余存储
  } catch {
    /* ignore */
  }
  document.dispatchEvent(new CustomEvent("gebai:file-display-change", { detail: { popup: v === "popup" } }))
}

/** 初始化：跨标签页同步（其他标签修改设置后本标签派发事件重载）。 */
export function initFileDisplay(): void {
  window.addEventListener("storage", (e) => {
    if (e.key !== KEY) return
    document.dispatchEvent(new CustomEvent("gebai:file-display-change", { detail: { popup: isFilePopup() } }))
  })
}
