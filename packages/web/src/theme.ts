/**
 * 主题引擎：多套 UI 风格解析 / 按需加载 / 持久化 / 外部注入 / 丝滑切换。
 *
 * 风格优先级（高 → 低）：
 *   1. 用户本次会话手动选择（setTheme）
 *   2. URL 参数 `?gb_style=<id>`（宿主 / iframe 注入，如业务系统嵌入）
 *   3. 用户级持久化（localStorage `gebai.ui.style`）
 *   4. 全局默认（服务端注入 `window.__GEBAI_UI_STYLE__`，来自 `GEBAI_UI_STYLE`）
 *   5. 内置默认 `acrylic`
 *
 * 主题切换走 View Transitions API（Chrome 111+ / Edge / Safari 18+ / Firefox 128+），
 * 在不支持时回退为渐变覆盖层扫描动画，保证"丝滑"观感。
 *
 * 自定义品牌化覆盖：URL 参数 `?gb_vars=--accent:%236366f1,--radius-md:8px`
 * 以逗号分隔的 `--变量:值` 列表，设置到根元素内联样式（优先级最高）。
 *
 * 强调色由各主题自行定义（主题视觉灵魂的一部分），不提供通用覆盖。
 */

import { input, themeBtn, themePop } from "./state"
import { tip } from "./ui"

type ThemeDef = { id: string; label: string; desc?: string; swatch: string; group?: string }

export const THEMES = [
  { id: "acrylic", label: "默认", swatch: "#0c0c0e" }, // 不归属任何分组，独立显示于列表顶部
  { id: "classic", label: "经典", desc: "深蓝复古", swatch: "#3b82f6", group: "基础" },
  { id: "dark", label: "暗夜", desc: "高对比", swatch: "#4f9cf9", group: "基础" },
  { id: "modern", label: "现代", desc: "明亮玻璃", swatch: "#8b5cf6", group: "基础" },
  { id: "minimal", label: "极简", desc: "黑白留白", swatch: "#111111", group: "基础" },
  { id: "matrix", label: "矩阵", desc: "终端绿", swatch: "#00ff41", group: "科技风" },
  { id: "tokyo-night", label: "东京夜", desc: "紫蓝夜空", swatch: "#7aa2f7", group: "科技风" },
  { id: "cyberpunk", label: "赛博", desc: "霓虹朋克", swatch: "#ff2d78", group: "科技风" },
  { id: "synthwave", label: "浪潮", desc: "霓虹日落", swatch: "#ff6ec7", group: "科技风" },
  { id: "aether", label: "以太", desc: "光之玻璃", swatch: "linear-gradient(135deg, #06b6d4, #8b5cf6, #ec4899)", group: "氛围风" },
  { id: "aurora", label: "极光", desc: "青绿紫", swatch: "#2dd4bf", group: "氛围风" },
  { id: "cny", label: "人民币", desc: "中国红", swatch: "linear-gradient(135deg, #d92d3a, #e8a33d)", group: "特色" },
] as const satisfies readonly ThemeDef[]

export type ThemeId = (typeof THEMES)[number]["id"]

/** 人民币主题面额配色（第五套人民币纸币主色），仅主题为 cny 时生效；null = 默认 100 元红 */
export const CNY_SCHEMES = [
  { id: "100", label: "100 元", desc: "中国红", base: "#d92d3a", grad: "linear-gradient(135deg, #9f1239, #d92d3a 55%, #e8a33d)" },
  { id: "50", label: "50 元", desc: "翠绿", base: "#2f9e5d", grad: "linear-gradient(135deg, #14532d, #2f9e5d 55%, #e0c34c)" },
  { id: "20", label: "20 元", desc: "赭棕", base: "#be6e1c", grad: "linear-gradient(135deg, #78350f, #be6e1c 55%, #e8b44a)" },
  { id: "10", label: "10 元", desc: "蓝黑", base: "#2d54af", grad: "linear-gradient(135deg, #172554, #2d54af 55%, #d8c36a)" },
  { id: "5", label: "5 元", desc: "绛紫", base: "#7c4ad6", grad: "linear-gradient(135deg, #4c1d95, #7c4ad6 55%, #e0c34c)" },
  { id: "1", label: "1 元", desc: "橄榄绿", base: "#5f803c", grad: "linear-gradient(135deg, #365314, #5f803c 55%, #e0c34c)" },
] as const

export type CnySchemeId = (typeof CNY_SCHEMES)[number]["id"]

/** 默认主题（acrylic）黑白切换（根元素 data-acrylic-lt），仅主题为 acrylic 时生效；null = 暗（默认） */
export const ACRYLIC_LT_MODES = [
  { id: "dark", label: "暗", desc: "黑色（默认）", swatch: "linear-gradient(135deg, #0c0c0e, #26262e)" },
  { id: "light", label: "亮", desc: "白色", swatch: "linear-gradient(135deg, #f4f4f8, #c6c6d0)" },
] as const

export type AcrylicLtId = (typeof ACRYLIC_LT_MODES)[number]["id"]

const STYLE_KEY = "gebai.ui.style"
const SCHEME_KEY = "gebai.ui.cnyScheme"
const ACRYLIC_LT_KEY = "gebai.ui.acrylicLt"
const GLOBAL_KEY = "__GEBAI_UI_STYLE__"

let userOverride: ThemeId | null = null
/** 三态：CnySchemeId=已选；"reset"=用户显式重置（覆盖 URL 参数）；null=从未选择 */
let userCnyScheme: CnySchemeId | "reset" | null = null
/** 默认主题黑白切换，同样三态 */
let userAcrylicLt: AcrylicLtId | "reset" | null = null
let themeLink: HTMLLinkElement | null = null
let transitionTimer: number | null = null

function isTheme(v: unknown): v is ThemeId {
  return typeof v === "string" && (THEMES as readonly { id: string }[]).some((t) => t.id === v)
}
function isCnyScheme(v: unknown): v is CnySchemeId {
  return typeof v === "string" && (CNY_SCHEMES as readonly { id: string }[]).some((s) => s.id === v)
}
function isAcrylicLt(v: unknown): v is AcrylicLtId {
  return typeof v === "string" && (ACRYLIC_LT_MODES as readonly { id: string }[]).some((s) => s.id === v)
}

/** classic 为主题变量兜底（:root），无需额外样式文件；其余主题按需加载。 */
function themeCssUrl(id: ThemeId): string | null {
  if (id === "classic") return null
  return new URL(`./themes/${id}.css`, import.meta.url).href
}

/** 待结算的主题加载 promise：连续快速切主题时先结算上一个，避免旧 link 被移除后其
 * load 事件永不触发导致该次 applyTheme 永久挂起。 */
let pendingThemeResolve: (() => void) | null = null

function loadCss(href: string): Promise<void> {
  if (themeLink) {
    themeLink.remove()
    themeLink = null
  }
  pendingThemeResolve?.() // 上一个加载已被本次替换：先结算
  pendingThemeResolve = null
  return new Promise((resolve) => {
    const link = document.createElement("link")
    link.rel = "stylesheet"
    link.href = href
    const settle = () => {
      if (pendingThemeResolve === settle) pendingThemeResolve = null
      resolve()
    }
    pendingThemeResolve = settle
    link.addEventListener("load", settle, { once: true })
    link.addEventListener("error", settle, { once: true }) // 加载失败静默回退到 :root 兜底
    document.head.appendChild(link)
    themeLink = link
  })
}

/** 解析 URL 参数 `?gb_vars=--a:%23fff,--b:8px`，注入根元素内联样式（品牌化覆盖）。 */
export function applyCustomVars(raw: string | null): void {
  const style = document.documentElement.style
  if (!raw) return
  for (const part of raw.split(",")) {
    const m = part.match(/^(\s*--[\w-]+)\s*:\s*(.*)$/)
    if (m) style.setProperty(m[1].trim(), m[2].trim())
  }
}

export function resolveTheme(): ThemeId {
  if (userOverride) return userOverride
  const fromUrl = new URLSearchParams(location.search).get("gb_style")
  if (fromUrl && isTheme(fromUrl)) return fromUrl
  const saved = localStorage.getItem(STYLE_KEY)
  if (saved && isTheme(saved)) return saved
  const global = (window as unknown as Record<string, unknown>)[GLOBAL_KEY]
  if (global && isTheme(global)) return global
  return "acrylic"
}

/** 解析人民币主题面额配色：会话手动选择 → URL 参数 `?gb_cny=<id>` → 用户持久化 → null（100 元默认）；显式重置（"reset"）跳过 URL 参数 */
export function resolveCnyScheme(): CnySchemeId | null {
  if (userCnyScheme === "reset") return null
  if (userCnyScheme) return userCnyScheme
  const fromUrl = new URLSearchParams(location.search).get("gb_cny")
  if (fromUrl && isCnyScheme(fromUrl)) return fromUrl
  const saved = localStorage.getItem(SCHEME_KEY)
  if (saved && isCnyScheme(saved)) return saved
  return null
}

/** 解析默认主题黑白切换：会话手动选择 → URL 参数 `?gb_acrylic_lt=<id>` → 用户持久化 → null（暗，默认）；显式重置（"reset"）跳过 URL 参数 */
export function resolveAcrylicLt(): AcrylicLtId | null {
  if (userAcrylicLt === "reset") return null
  if (userAcrylicLt) return userAcrylicLt
  const fromUrl = new URLSearchParams(location.search).get("gb_acrylic_lt")
  if (fromUrl && isAcrylicLt(fromUrl)) return fromUrl
  const saved = localStorage.getItem(ACRYLIC_LT_KEY)
  if (saved && isAcrylicLt(saved)) return saved
  return null
}

/**
 * 应用主题：设置 data-theme + 按需加载主题 CSS。
 * 通过 View Transitions API 实现"丝滑"切换（不支持时回退为瞬时切换 + 扫描光动画）。
 */
export async function applyTheme(id: ThemeId): Promise<void> {
  document.documentElement.dataset.theme = id
  const url = themeCssUrl(id)
  const swap = async () => {
    if (url) await loadCss(url)
    else if (themeLink) {
      themeLink.remove()
      themeLink = null
    }
  }
  // View Transitions API
  const doc = document as Document & {
    startViewTransition?: (cb: () => Promise<void> | void) => { finished: Promise<void> }
  }
  if (typeof doc.startViewTransition === "function") {
    const vt = doc.startViewTransition(swap)
    try {
      await vt.finished
    } catch {
      /* 切换失败静默回退 */
    }
  } else {
    // 兜底：扫描光动画
    playSweepFallback()
    await swap()
  }
  // 通知依赖主题的组件（如 Mermaid 图表重绘）跟随新配色
  document.dispatchEvent(new CustomEvent("gebai:theme-change", { detail: { theme: id } }))
}

/** 兜底动画：极光扫描光从顶部到底部贯穿整个屏幕 */
function playSweepFallback() {
  if (transitionTimer) {
    clearTimeout(transitionTimer)
    document.getElementById("gb-theme-sweep")?.remove()
  }
  const sweep = document.createElement("div")
  sweep.id = "gb-theme-sweep"
  document.body.appendChild(sweep)
  // 强制重排以触发动画
  void sweep.offsetWidth
  transitionTimer = window.setTimeout(() => {
    sweep.remove()
    transitionTimer = null
  }, 650)
}

/** 用户手动切换主题：内存优先 + 持久化（用户级） + 丝滑过渡。 */
export async function setTheme(id: ThemeId): Promise<void> {
  userOverride = id
  try {
    localStorage.setItem(STYLE_KEY, id)
  } catch {
    /* 隐私模式等场景忽略 */
  }
  await applyTheme(id)
}

/** 应用人民币面额配色：设置/移除根元素 data-cny-scheme（cny.css 中的配色变量块据此生效）。 */
export function applyCnyScheme(scheme: CnySchemeId | null): void {
  const el = document.documentElement
  if (scheme) el.dataset.cnyScheme = scheme
  else delete el.dataset.cnyScheme
}

/** 用户手动切换人民币面额配色：立即生效 + 持久化（不重载主题）；传 null 表示显式重置为默认 100 元红。 */
export function setCnyScheme(scheme: CnySchemeId | null): void {
  userCnyScheme = scheme ?? "reset"
  try {
    if (scheme) localStorage.setItem(SCHEME_KEY, scheme)
    else localStorage.removeItem(SCHEME_KEY)
  } catch {
    /* ignore */
  }
  applyCnyScheme(scheme)
}

/** 应用默认主题黑白切换：设置/移除根元素 data-acrylic-lt（acrylic.css 亮色覆盖块据此生效）。 */
export function applyAcrylicLt(lt: AcrylicLtId | null): void {
  const el = document.documentElement
  if (lt) el.dataset.acrylicLt = lt
  else delete el.dataset.acrylicLt
}

/** 用户手动切换黑白：立即生效 + 持久化（不重载主题）；传 null 表示显式重置为默认暗色。 */
export function setAcrylicLt(lt: AcrylicLtId | null): void {
  userAcrylicLt = lt ?? "reset"
  try {
    if (lt) localStorage.setItem(ACRYLIC_LT_KEY, lt)
    else localStorage.removeItem(ACRYLIC_LT_KEY)
  } catch {
    /* ignore */
  }
  applyAcrylicLt(lt)
}

export function initTheme(): void {
  const params = new URLSearchParams(location.search)
  applyCustomVars(params.get("gb_vars"))
  applyCnyScheme(resolveCnyScheme())
  applyAcrylicLt(resolveAcrylicLt())
  void applyTheme(resolveTheme())
}

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

export function bindThemePop() {
  renderThemePop()
  themeBtn.onclick = () => {
    // 每次打开重渲染：人民币配色分组随当前主题显示/隐藏
    if (themePop.hidden) renderThemePop()
    themePop.hidden = !themePop.hidden
    themeBtn.setAttribute("aria-expanded", String(!themePop.hidden))
    if (!themePop.hidden) {
      // 面板跟随按钮弹出（按钮可能位于标题栏轮盘等右侧位置）：
      // 面板右缘对齐按钮右缘并钳制在视口内（左对齐会让 252px 面板右侧出界）；
      // 按钮下方空间足够则向下展开，否则向上展开
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
  }
  document.addEventListener("click", (e) => {
    if (!themePop.hidden && !themePop.contains(e.target as Node) && e.target !== themeBtn) {
      themePop.hidden = true
      themeBtn.setAttribute("aria-expanded", "false")
    }
  })
}
