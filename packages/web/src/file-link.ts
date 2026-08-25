/**
 * 文件链接 chip（「弹窗查看」模式下文件工具卡的统一文件入口，DESIGN「文件链接弹窗查看」）：
 * 图标 + 文件名 + 路径（弱化展示）+ 下载；点击弹窗查看文件内容。
 * - 路径来源：工具结果解析后的真实路径（msg.file——会话 tmp/ 逻辑路径或项目绝对路径）优先，缺省回退原始参数路径
 *   （会话相对路径可直接解析；项目相对路径在结果到达前可能无法解析，弹窗内提示）；
 * - write 类工具可携带参数内容（content）——点击直接弹窗内联渲染待写入内容，不取数（审批前文件尚未落盘）；
 * - 结果到达后经 upgradeFileLinks 把 chip 升级为解析后路径（code 项目文件的粘性根/预置项目解析在服务端完成）。
 */
import { el, getCurrentSession } from "./state"
import { desktopDownloadHint } from "./ui"
import { openFilePreview, openTextPreview } from "./file-card"
import { ICON_DOWNLOAD } from "./html-view"

/** 文件链接 chip：click 弹窗查看（content 提供时内联渲染不取数）；下载图标经 files/preview 附件形式获取。 */
export function fileLinkChip(opts: { name: string; path: string; content?: string; contentLang?: string; sessionId?: string }): HTMLElement {
  const chip = el("div", "file-link")
  chip.dataset.path = opts.path
  chip.dataset.name = opts.name
  if (opts.content != null) chip.dataset.inline = "1"
  chip.setAttribute("role", "button")
  chip.setAttribute("tabindex", "0")
  chip.append(el("span", "file-link-ico", "📄"), el("span", "file-link-name", opts.name), el("span", "file-link-path", opts.path))
  const sessionId = opts.sessionId ?? getCurrentSession()?.id ?? ""
  const content = opts.content
  const contentLang = opts.contentLang
  const open = () => {
    const cur = sessionId || getCurrentSession()?.id || ""
    // write 类：弹窗内联渲染参数内容（审批审查的就是待写入内容本身；落盘前后不受影响）
    if (content != null && chip.dataset.inline === "1") {
      openTextPreview(chip.dataset.name || opts.name, content, contentLang)
      return
    }
    openFilePreview(cur, chip.dataset.name || opts.name, chip.dataset.path || opts.path)
  }
  chip.onclick = open
  chip.addEventListener("keydown", (e) => {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault()
      open()
    }
  })
  if (sessionId) {
    const a = document.createElement("a")
    a.className = "icon-btn file-dl-icon"
    a.title = "下载"
    a.innerHTML = ICON_DOWNLOAD
    a.href = `/api/v1/sessions/${sessionId}/files/preview?path=${encodeURIComponent(opts.path)}&download=1`
    a.onclick = (e) => {
      e.stopPropagation()
      desktopDownloadHint(opts.name)
    }
    chip.appendChild(a)
  }
  return chip
}

/** 结果到达升级：chip 路径/文件名替换为服务端解析后的真实路径（项目文件原始参数路径无法由 files 接口解析），
 *  下载地址同步重建。内联内容 chip（write 待写入内容）不升级——审查对象是参数内容本身。 */
export function upgradeFileLinks(scope: HTMLElement, file: { path: string; name?: string }): void {
  const sessionId = getCurrentSession()?.id ?? ""
  for (const chip of scope.querySelectorAll<HTMLElement>(".file-link")) {
    if (chip.dataset.inline === "1") continue
    chip.dataset.path = file.path
    if (file.name) chip.dataset.name = file.name
    const nameEl = chip.querySelector<HTMLElement>(".file-link-name")
    if (nameEl && file.name) nameEl.textContent = file.name
    const pathEl = chip.querySelector<HTMLElement>(".file-link-path")
    if (pathEl) pathEl.textContent = file.path
    const dl = chip.querySelector<HTMLAnchorElement>("a.file-dl-icon")
    if (dl && sessionId) dl.href = `/api/v1/sessions/${sessionId}/files/preview?path=${encodeURIComponent(file.path)}&download=1`
  }
}
