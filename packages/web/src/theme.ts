/**
 * 主题面板（header 🎨 下拉）——主题引擎见 `theme-core.ts`。
 *
 * 引擎与面板分离的原因：主界面与文件工作台是两个独立入口（`/` 与 `/files`），
 * 但主题是**用户级偏好**、必须完全一致，所以解析/生效逻辑只有一份（theme-core），
 * 本模块只负责把它渲染成界面，并**原样转出**引擎的全部导出（既有调用点无需改动）。
 */

import { input, themeBtn, themePop } from "./state"
import { tip } from "./ui"
import {
  ACRYLIC_LT_MODES,
  CNY_SCHEMES,
  THEMES,
  resolveAcrylicLt,
  resolveCnyScheme,
  setAcrylicLt,
  setCnyScheme,
  setTheme,
} from "./theme-core"

export * from "./theme-core"

/* ---------- 主题面板（header 🎨 下拉） ---------- */

function syncThemePop() {
  const active = document.documentElement.dataset.theme
  for (const opt of themePop.querySelectorAll<HTMLButtonElement>(".theme-opt")) {
    const id = opt.dataset.themeId
    opt.classList.toggle("active", id === active)
  }
  const scheme = resolveCnyScheme()
  const activeScheme = scheme ?? "100" // null（含显式重置）视为默认 100 元红，激活其按钮
  for (const btn of themePop.querySelectorAll<HTMLButtonElement>(".cny-opt")) {
    btn.classList.toggle("active", btn.dataset.cnySchemeId === activeScheme)
  }
  const lt = resolveAcrylicLt() ?? "dark" // null（含显式重置）视为默认暗色
  for (const btn of themePop.querySelectorAll<HTMLButtonElement>(".acrylic-lt-opt")) {
    btn.classList.toggle("active", btn.dataset.acrylicLtId === lt)
  }
}

/** 渲染一组方案色块（分组标题 + accent-row 色块 + 重置），人民币配色/亚克力调节共用。 */
function renderSchemeGroup<T extends string>(
  head: string,
  optCls: string,
  dataKey: string,
  opts: readonly { id: T; label: string; desc: string; swatch?: string; grad?: string }[],
  onPick: (id: T | null) => void,
  resetTip: string,
) {
  const label = document.createElement("div")
  label.className = "theme-section-label"
  label.textContent = head
  themePop.appendChild(label)

  const row = document.createElement("div")
  row.className = "accent-row"
  for (const s of opts) {
    const btn = document.createElement("button")
    btn.type = "button"
    btn.className = `accent-opt ${optCls}`
    btn.dataset[dataKey] = s.id
    tip(btn, `${s.label} · ${s.desc}`)
    btn.style.setProperty("--swatch", s.swatch ?? s.grad ?? "#888")
    btn.onclick = () => {
      onPick(s.id)
      syncThemePop()
    }
    row.appendChild(btn)
  }
  const reset = document.createElement("button")
  reset.type = "button"
  reset.className = "accent-reset"
  tip(reset, resetTip)
  reset.textContent = "↺"
  reset.onclick = () => {
    onPick(null)
    syncThemePop()
  }
  row.appendChild(reset)
  themePop.appendChild(row)
}

function renderThemePop() {
  themePop.innerHTML = ""

  // 人民币面额配色分组（仅人民币主题显示）
  if (document.documentElement.dataset.theme === "cny") {
    renderSchemeGroup(
      "人民币配色",
      "cny-opt",
      "cnySchemeId",
      CNY_SCHEMES,
      (id) => setCnyScheme(id),
      "重置为默认 100 元红",
    )
  }

  // 默认主题黑白切换：独立一行、不归属任何分组（仅默认主题显示）
  if (document.documentElement.dataset.theme === "acrylic") {
    const row = document.createElement("div")
    row.className = "accent-row"
    for (const s of ACRYLIC_LT_MODES) {
      const btn = document.createElement("button")
      btn.type = "button"
      btn.className = "accent-opt acrylic-lt-opt"
      btn.dataset.acrylicLtId = s.id
      tip(btn, `黑白：${s.label} · ${s.desc}`)
      btn.style.setProperty("--swatch", s.swatch)
      btn.onclick = () => {
        setAcrylicLt(s.id)
        syncThemePop()
      }
      row.appendChild(btn)
    }
    const reset = document.createElement("button")
    reset.type = "button"
    reset.className = "accent-reset"
    tip(reset, "重置为默认暗色")
    reset.textContent = "↺"
    reset.onclick = () => {
      setAcrylicLt(null)
      syncThemePop()
    }
    row.appendChild(reset)
    themePop.appendChild(row)
  }

  // 主题分组（无 group 的主题为独立项：不归属任何分组、无分组标题）
  let lastGroup: string | null = null
  for (const t of THEMES) {
    const g = "group" in t ? t.group : null
    if (g !== lastGroup) {
      lastGroup = g
      if (g) {
        const label = document.createElement("div")
        label.className = "theme-section-label"
        label.textContent = g
        themePop.appendChild(label)
      }
    }
    const opt = document.createElement("button")
    opt.type = "button"
    opt.className = "theme-opt"
    opt.dataset.themeId = t.id
    const swatch = document.createElement("span")
    swatch.className = "swatch"
    swatch.style.background = t.swatch
    const labels = document.createElement("span")
    labels.append(Object.assign(document.createElement("span"), { className: "t-label", textContent: t.label }))
    if ("desc" in t) labels.append(Object.assign(document.createElement("span"), { className: "t-desc", textContent: t.desc }))
    opt.append(swatch, labels)
    opt.onclick = async () => {
      await setTheme(t.id)
      syncThemePop()
      themePop.hidden = true
      themeBtn.setAttribute("aria-expanded", "false")
      input?.focus()
    }
    themePop.appendChild(opt)
  }
  syncThemePop()
}

/* 悬浮交互延迟：入口 hover 短延迟展开（扫过轮盘扇区不误弹，同轮盘可调回 0），
   离开入口/面板 250ms 后收起（入口 → 面板间移动空隙不误关，与轮盘 CLOSE_DELAY 一致） */
const POP_OPEN_DELAY = 120
const POP_CLOSE_DELAY = 250

export function bindThemePop() {
  renderThemePop()
  let openTimer: number | null = null
  let closeTimer: number | null = null
  const cancelOpen = () => {
    if (openTimer) {
      clearTimeout(openTimer)
      openTimer = null
    }
  }
  const cancelClose = () => {
    if (closeTimer) {
      clearTimeout(closeTimer)
      closeTimer = null
    }
  }
  const closePop = () => {
    cancelOpen()
    cancelClose()
    themePop.hidden = true
    themeBtn.setAttribute("aria-expanded", "false")
  }
  const openPop = () => {
    cancelClose()
    if (!themePop.hidden) return
    renderThemePop() // 每次打开重渲染：人民币配色分组随当前主题显示/隐藏
    themePop.hidden = false
    themeBtn.setAttribute("aria-expanded", "true")
    positionPop()
  }
  // 面板跟随按钮弹出（按钮可能位于标题栏轮盘等右侧位置）：
  // 面板右缘对齐按钮右缘并钳制在视口内（左对齐会让 252px 面板右侧出界）；
  // 按钮下方空间足够则向下展开，否则向上展开
  const positionPop = () => {
    const r = themeBtn.getBoundingClientRect()
    const w = themePop.offsetWidth
    themePop.style.left = `${Math.max(8, Math.min(r.right - w, window.innerWidth - w - 8))}px`
    themePop.style.right = "auto"
    const spaceBelow = window.innerHeight - r.bottom
    if (spaceBelow >= 300) {
      themePop.style.top = `${r.bottom + 8}px`
      themePop.style.bottom = "auto"
      themePop.style.maxHeight = `${spaceBelow - 16}px`
    } else {
      themePop.style.top = "auto"
      themePop.style.bottom = `${window.innerHeight - r.top + 8}px`
      themePop.style.maxHeight = `${Math.max(200, r.top - 16)}px`
    }
  }
  const scheduleOpen = () => {
    cancelClose()
    if (!themePop.hidden || openTimer) return
    openTimer = window.setTimeout(() => {
      openTimer = null
      openPop()
    }, POP_OPEN_DELAY)
  }
  const scheduleClose = () => {
    cancelOpen()
    if (themePop.hidden || closeTimer) return
    closeTimer = window.setTimeout(() => {
      closeTimer = null
      closePop()
    }, POP_CLOSE_DELAY)
  }
  // 悬浮自动展开：入口/面板互为保持区，两者都离开才收起
  themeBtn.addEventListener("pointerenter", scheduleOpen)
  themeBtn.addEventListener("pointerleave", scheduleClose)
  themePop.addEventListener("pointerenter", cancelClose)
  themePop.addEventListener("pointerleave", scheduleClose)
  // 键盘可达：焦点进入入口即展开；焦点离开入口+面板之外收起
  themeBtn.addEventListener("focusin", openPop)
  themePop.addEventListener("focusin", cancelClose)
  const onfocusout = (e: FocusEvent) => {
    const to = e.relatedTarget as Node | null
    if (to && (themeBtn.contains(to) || themePop.contains(to))) return
    closePop()
  }
  themeBtn.addEventListener("focusout", onfocusout)
  themePop.addEventListener("focusout", onfocusout)
  // Enter/空格仅展开不切换（hover 已展开时点击不误关）
  themeBtn.onclick = () => openPop()
  // 外点/Esc/resize 收起
  document.addEventListener("pointerdown", (e) => {
    if (!themePop.hidden && !themePop.contains(e.target as Node) && !themeBtn.contains(e.target as Node)) closePop()
  })
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") closePop()
  })
  window.addEventListener("resize", closePop)
}
