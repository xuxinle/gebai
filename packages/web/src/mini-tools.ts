/**
 * HTML 小工具库：标题栏「小工具」按钮 → 弹窗列表 → 点击加载到独立悬浮窗口（沙箱 iframe）。
 * 重复点击同一工具 = 恢复到之前的窗口（置顶聚焦），不重新创建/重载。
 */

import type { MiniToolInfo, MiniToolMeta } from "@gebai/sdk"
import { client, el, miniToolsBtn } from "./state"
import { confirmDialog, tip, toast } from "./ui"
import { currentThemeColors, ensureThemeSync, sandboxedHtml } from "./html-view"

/* ---------- 弹窗（工具列表） ---------- */

const SCOPE_LABEL: Record<string, string> = { public: "公用", private: "私有" }

let pop: HTMLElement | null = null

function closePop(): void {
  pop?.remove()
  pop = null
  document.removeEventListener("pointerdown", onOutside)
  document.removeEventListener("keydown", onPopKey)
}

function onPopKey(e: KeyboardEvent): void {
  if (e.key === "Escape") closePop()
}

function onOutside(e: Event): void {
  if (!miniToolsBtn.contains(e.target as Node) && !(pop && pop.contains(e.target as Node))) closePop()
}

async function refreshList(listEl: HTMLElement, emptyEl: HTMLElement): Promise<void> {
  listEl.innerHTML = ""
  let tools: MiniToolMeta[] = []
  try {
    tools = await client.listMiniTools()
  } catch {
    toast("小工具列表加载失败")
    return
  }
  if (!tools.length) {
    emptyEl.hidden = false
    return
  }
  emptyEl.hidden = true
  for (const t of tools) {
    const row = el("div", "mini-tool-row")
    row.setAttribute("role", "button")
    const name = el("span", "mini-tool-name", t.name)
    tip(name, `点击加载「${t.name}」`)
    row.appendChild(name)
    row.appendChild(el("span", `mini-tool-badge ${t.scope}`, SCOPE_LABEL[t.scope] ?? t.scope))
    const del = el("button", "mini-tool-del", "✕")
    del.type = "button"
    del.setAttribute("aria-label", `删除小工具「${t.name}」`)
    tip(del, "删除小工具")
    del.onclick = (e) => {
      e.stopPropagation()
      void (async () => {
        if (await deleteToolRow(t.name, t.scope)) void refreshList(listEl, emptyEl)
      })()
    }
    row.append(del)
    row.onclick = () => {
      closePop()
      openMiniTool(t)
    }
    listEl.appendChild(row)
  }
}

/** 删除小工具（确认后调 REST；成功返回 true）。公用工具删除影响所有用户，文案强调。 */
async function deleteToolRow(name: string, scope: "public" | "private"): Promise<boolean> {
  const isPublic = scope === "public"
  const ok = await confirmDialog({
    title: "删除小工具",
    text: `删除${isPublic ? "公用" : "私有"}小工具「${name}」？删除后不可恢复${isPublic ? "，所有用户不可用" : ""}。`,
    okLabel: "删除",
    danger: true,
  })
  if (!ok) return false
  try {
    await client.deleteMiniTool(name, scope)
    // 已打开的窗口一并关闭（工具已不存在，保留 iframe 无意义）
    const w = wins.get(name)
    if (w) {
      w.el.remove()
      wins.delete(name)
    }
    toast(`已删除「${name}」`)
    return true
  } catch (err) {
    toast(`删除失败：${(err as Error).message}`, "error")
    return false
  }
}

/** 弹窗开关：打开时重新拉取列表（数据随时可能被 Agent 更新）。 */
export function toggleMiniToolsPop(): void {
  if (pop) {
    closePop()
    return
  }
  pop = el("div", "mini-tools-pop")
  const head = el("div", "mini-tools-pop-head")
  head.append(el("span", "mini-tools-pop-title", "HTML 小工具"))
  const refresh = el("button", "mini-tools-refresh", "刷新")
  tip(refresh, "重新加载列表")
  refresh.onclick = () => void refreshList(listEl, emptyEl)
  head.appendChild(refresh)
  const listEl = el("div", "mini-tools-list")
  const emptyEl = el("div", "mini-tools-empty", "暂无小工具 — 让 Agent 先用 render_html 调试，再用 save_tool 保存")
  pop.append(head, listEl, emptyEl)
  document.body.appendChild(pop)
  // 定位：按钮下方右对齐
  const r = miniToolsBtn.getBoundingClientRect()
  pop.style.left = `${Math.max(8, Math.min(r.right - 260, window.innerWidth - 268))}px`
  pop.style.top = `${Math.min(r.bottom + 6, window.innerHeight - 12 - 220)}px`
  document.addEventListener("pointerdown", onOutside)
  document.addEventListener("keydown", onPopKey)
  void refreshList(listEl, emptyEl)
}

/* ---------- 悬浮窗口（沙箱 iframe 加载工具） ---------- */

interface MiniToolWin {
  el: HTMLElement
  frame: HTMLIFrameElement | null
  /** 已加载的完整信息（刷新时复用 name/scope 重新拉取）。 */
  info: MiniToolMeta
}

const wins = new Map<string, MiniToolWin>()
let winZ = 100
let cascade = 0

/** 读取宿主当前主题快照（与 html-view 卡片一致：变量集 + 背景/滚动条 + 主题 id，注入 iframe）。 */
function themeSnapshot(): { bg?: string; thumb?: string; thumbHover?: string; vars?: Record<string, string>; theme?: string } {
  const c = currentThemeColors()
  return { bg: c.bg || undefined, thumb: c.thumb || undefined, thumbHover: c.thumbHover || undefined, vars: c.vars, theme: c.theme || undefined }
}

function bringToFront(win: HTMLElement): void {
  winZ += 1
  win.style.zIndex = String(winZ)
  for (const w of wins.values()) w.el.classList.toggle("active", w.el === win)
}

function clampWindow(x: number, y: number): [number, number] {
  const vw = document.documentElement.clientWidth
  const vh = document.documentElement.clientHeight
  return [Math.min(Math.max(0, x), Math.max(0, vw - 160)), Math.min(Math.max(0, y), Math.max(0, vh - 60))]
}

/** 拖拽：标题栏按下 → 移动更新位置（指针捕获重定向到 win，move/up 监听须挂在 win 上，
 *  否则松开后 dragging 状态残留导致窗口持续跟随光标）。 */
function makeDraggable(head: HTMLElement, win: HTMLElement): void {
  let dragging = false
  let sx = 0
  let sy = 0
  let ox = 0
  let oy = 0
  head.addEventListener("pointerdown", (e) => {
    if ((e.target as HTMLElement).closest("button")) return // 标题栏按钮不触发拖拽
    dragging = true
    sx = e.clientX
    sy = e.clientY
    ox = win.offsetLeft
    oy = win.offsetTop
    win.setPointerCapture(e.pointerId)
    win.classList.add("dragging")
    e.preventDefault()
  })
  win.addEventListener("pointermove", (e) => {
    if (!dragging) return
    const [x, y] = clampWindow(ox + e.clientX - sx, oy + e.clientY - sy)
    win.style.left = `${x}px`
    win.style.top = `${y}px`
  })
  const end = (e: PointerEvent) => {
    if (!dragging) return
    dragging = false
    win.classList.remove("dragging")
    if (win.hasPointerCapture(e.pointerId)) win.releasePointerCapture(e.pointerId)
  }
  win.addEventListener("pointerup", end)
  win.addEventListener("pointercancel", end)
  win.addEventListener("lostpointercapture", end)
}

/** 窗口标题栏按钮。 */
function winButton(title: string, icon: string): HTMLButtonElement {
  const btn = el("button", "mini-tool-win-btn")
  btn.type = "button"
  tip(btn, title)
  btn.innerHTML = icon
  return btn
}

const ICON_REFRESH = '<svg viewBox="0 0 16 16"><path d="M13.5 8a5.5 5.5 0 1 1-1.6-3.9M13.5 1.5v3h-3"/></svg>'
const ICON_CLOSE = '<svg viewBox="0 0 16 16"><path d="M4 4l8 8M12 4l-8 8"/></svg>'

async function loadFrame(win: MiniToolWin, info: MiniToolInfo): Promise<void> {
  const body = win.el.querySelector<HTMLElement>(".mini-tool-body")
  if (!body) return
  body.innerHTML = ""
  const frame = document.createElement("iframe")
  frame.className = "mini-tool-frame"
  frame.setAttribute("sandbox", "allow-scripts")
  frame.setAttribute("referrerpolicy", "no-referrer")
  frame.setAttribute("data-html-frame", "")
  frame.srcdoc = sandboxedHtml(info.html, themeSnapshot())
  // 主题广播：加载后补发一次当前主题（与 html-view 卡片一致）
  frame.addEventListener("load", () => {
    const t = themeSnapshot()
    if (t.bg || t.thumb) frame.contentWindow?.postMessage({ source: "gebai-host", type: "theme", ...t }, "*")
  })
  body.appendChild(frame)
  win.frame = frame
}

async function openMiniTool(info: MiniToolMeta): Promise<void> {
  ensureThemeSync()
  const existing = wins.get(info.name)
  if (existing) {
    // 重复点击：恢复之前的窗口（取消隐藏并置顶聚焦，保留 iframe 状态），不重新创建/重载
    existing.el.classList.remove("hidden")
    bringToFront(existing.el)
    return
  }
  const win = el("div", "mini-tool-window")
  win.style.zIndex = String(++winZ)
  const head = el("div", "mini-tool-head")
  const title = el("span", "mini-tool-title")
  tip(title, `小工具「${info.name}」` + (info.scope === "public" ? "（公用）" : "（私有）"))
  title.textContent = info.name
  const scopeBadge = el("span", `mini-tool-badge ${info.scope}`, SCOPE_LABEL[info.scope] ?? info.scope)
  const refresh = winButton("重新加载", ICON_REFRESH)
  const close = winButton("关闭", ICON_CLOSE)
  head.append(title, scopeBadge, refresh, close)
  const body = el("div", "mini-tool-body")
  body.appendChild(el("div", "mini-tool-loading", "加载中…"))
  win.append(head, body)
  document.body.appendChild(win)
  // 层叠定位（新窗口依次偏移，避免完全重叠）
  const vw = document.documentElement.clientWidth
  const vh = document.documentElement.clientHeight
  const w = Math.min(480, vw - 24)
  const h = Math.min(400, vh - 80)
  win.style.width = `${w}px`
  win.style.height = `${h}px`
  const [x, y] = clampWindow(28 + (cascade % 6) * 32, 52 + (cascade % 6) * 28)
  cascade += 1
  win.style.left = `${x}px`
  win.style.top = `${y}px`
  makeDraggable(head, win)
  win.addEventListener("pointerdown", () => bringToFront(win))
  close.onclick = () => {
    // 微信小程序式关闭：仅隐藏（保留 iframe 运行状态），再次点击同工具恢复；刷新按钮才重新加载
    win.classList.add("hidden")
  }
  refresh.onclick = () => void refreshWin(win)
  const rec: MiniToolWin = { el: win, frame: null, info }
  wins.set(info.name, rec)
  bringToFront(win)
  try {
    const full = await client.getMiniTool(info.name)
    await loadFrame(rec, full)
  } catch {
    body.innerHTML = ""
    body.appendChild(el("div", "mini-tool-loading error", "加载失败：工具不存在或已被删除"))
  }
}

/** 重新拉取并重载窗口内容（工具更新后可见新版本）。 */
async function refreshWin(win: HTMLElement): Promise<void> {
  const rec = [...wins.values()].find((w) => w.el === win)
  if (!rec) return
  const body = win.querySelector<HTMLElement>(".mini-tool-body")
  if (!body) return
  body.innerHTML = ""
  body.appendChild(el("div", "mini-tool-loading", "加载中…"))
  try {
    const full = await client.getMiniTool(rec.info.name)
    await loadFrame(rec, full)
  } catch {
    body.innerHTML = ""
    body.appendChild(el("div", "mini-tool-loading error", "加载失败：工具不存在或已被删除"))
  }
}

/** 初始化：标题栏按钮绑定弹窗开关。 */
export function bindMiniTools(): void {
  miniToolsBtn.onclick = () => toggleMiniToolsPop()
}
