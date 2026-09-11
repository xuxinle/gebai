/**
 * 主题引擎（核心）：多套 UI 风格的定义 / 解析 / 按需加载 / 持久化 / 生效。
 *
 * 与 `theme.ts` 的分工：本模块**刻意不依赖任何界面模块**（不 import ./state 之类），
 * 因此主界面（`/`）与文件工作台（独立入口 `/files`）可以共用同一套主题逻辑。
 * 主题是**用户级偏好**（localStorage `gebai.ui.style`），两个页面必须完全一致——
 * 否则从主界面切到工作台会换一套配色；`theme.ts` 只在其上叠加「🎨 主题面板」界面。
 *
 * 风格优先级（高 → 低）：
 *   1. 用户本次会话手动选择（setTheme）
 *   2. URL 参数 `?gb_style=<id>`（宿主 / iframe 注入，如业务系统嵌入）
 *   3. 用户级持久化（localStorage `gebai.ui.style`）
 *   4. 全局默认（服务端注入 `window.__GEBAI_UI_STYLE__`，来自 `GEBAI_UI_STYLE`）
 *   5. 内置默认 `acrylic`
 *
 * **文件工作台（`/files`）不读 URL 上的主题参数**（`initTheme({ urlPrefs: false })`）：它靠
 * localStorage 与主界面共享同一份用户级偏好（另加跨标签页 `storage` 同步），URL 上既不带、
 * 也不覆盖主题——否则一个带旧参数的链接就能把两页拆成两套配色。宿主定制仍可用 `?gb_vars`（主界面）。
 *
 * 主题切换走 View Transitions API（Chrome 111+ / Edge / Safari 18+ / Firefox 128+），
 * 在不支持时回退为渐变覆盖层扫描动画，保证"丝滑"观感。
 *
 * 自定义品牌化覆盖：URL 参数 `?gb_vars=--accent:%236366f1,--radius-md:8px`
 * 以逗号分隔的 `--变量:值` 列表，设置到根元素内联样式（优先级最高）。
 *
 * 强调色由各主题自行定义（主题视觉灵魂的一部分），不提供通用覆盖。
 */

export const THEMES = [
  { id: "acrylic", label: "默认", swatch: "#0c0c0e" }, // 不归属任何分组，独立显示于列表顶部
  { id: "matrix", label: "矩阵", desc: "终端绿", swatch: "#00ff41", group: "科技风" },
  { id: "tokyo-night", label: "东京夜", desc: "紫蓝夜空", swatch: "#7aa2f7", group: "科技风" },
  { id: "cyberpunk", label: "赛博", desc: "霓虹朋克", swatch: "#ff2d78", group: "科技风" },
  { id: "synthwave", label: "浪潮", desc: "霓虹日落", swatch: "#ff6ec7", group: "科技风" },
  { id: "aether", label: "以太", desc: "光之玻璃", swatch: "linear-gradient(135deg, #06b6d4, #8b5cf6, #ec4899)", group: "氛围风" },
  { id: "aurora", label: "极光", desc: "青绿紫", swatch: "#2dd4bf", group: "氛围风" },
  { id: "ink", label: "水墨", desc: "宣纸墨韵", swatch: "linear-gradient(135deg, #f6f1e7, #cfc6b2)", group: "氛围风" },
  { id: "cny", label: "人民币", desc: "中国红", swatch: "linear-gradient(135deg, #d92d3a, #e8a33d)", group: "特色" },
  { id: "qinhan", label: "秦汉", desc: "玄朱鎏金", swatch: "linear-gradient(135deg, #171009, #b2452f 55%, #d0a13f)", group: "特色" },
] as const satisfies readonly { id: string; label: string; desc?: string; swatch: string; group?: string }[]

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
/** 是否允许 URL 参数参与偏好解析（`initTheme` 设定；`/files` 传 false——主题只认 localStorage） */
let urlPrefsEnabled = true
/** 跨标签页同步只绑一次（两个入口各调一次 initTheme 也只会绑一个监听器） */
let storageBound = false

export function isTheme(v: unknown): v is ThemeId {
  return typeof v === "string" && (THEMES as readonly { id: string }[]).some((t) => t.id === v)
}
export function isCnyScheme(v: unknown): v is CnySchemeId {
  return typeof v === "string" && (CNY_SCHEMES as readonly { id: string }[]).some((s) => s.id === v)
}
export function isAcrylicLt(v: unknown): v is AcrylicLtId {
  return typeof v === "string" && (ACRYLIC_LT_MODES as readonly { id: string }[]).some((s) => s.id === v)
}

/** 所有主题均为独立样式文件，按需加载。 */
function themeCssUrl(id: ThemeId): string {
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
    link.id = "gb-theme-css"
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
  const fromUrl = urlPrefsEnabled ? new URLSearchParams(location.search).get("gb_style") : null
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
  const fromUrl = urlPrefsEnabled ? new URLSearchParams(location.search).get("gb_cny") : null
  if (fromUrl && isCnyScheme(fromUrl)) return fromUrl
  const saved = localStorage.getItem(SCHEME_KEY)
  if (saved && isCnyScheme(saved)) return saved
  return null
}

/** 解析默认主题黑白切换：会话手动选择 → URL 参数 `?gb_acrylic_lt=<id>` → 用户持久化 → null（暗，默认）；显式重置（"reset"）跳过 URL 参数 */
export function resolveAcrylicLt(): AcrylicLtId | null {
  if (userAcrylicLt === "reset") return null
  if (userAcrylicLt) return userAcrylicLt
  const fromUrl = urlPrefsEnabled ? new URLSearchParams(location.search).get("gb_acrylic_lt") : null
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
    await loadCss(url)
  }
  // View Transitions API
  const doc = document as Document & {
    startViewTransition?: (cb: () => Promise<void> | void) => { ready?: Promise<void>; finished: Promise<void> }
  }
  if (typeof doc.startViewTransition === "function") {
    const vt = doc.startViewTransition(swap)
    // `ready` 会在过渡被**跳过**时 reject，必须接住：
    // 分屏模式下（页面里有 iframe）Chrome 会跳过 View Transition，若不处理就是一个
    // 未捕获的 AbortError 打进控制台（用户看到的是主题照常切换、控制台一条红）。
    // 跳过只是没有动画，swap 仍会执行，主题照常生效。
    vt.ready?.catch(() => {})
    try {
      await vt.finished
    } catch {
      /* 切换失败/被跳过：静默回退（无动画，但主题已换） */
    }
  } else {
    // 兜底：扫描光动画
    playSweepFallback()
    await swap()
  }
  // 通知依赖主题的组件（如 Mermaid 图表重绘、Monaco 换色、分屏里的工作台）
  notifyThemeChange()
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

/**
 * 通知依赖主题的组件重新同步（Mermaid/Monaco 重绘、画布特效、分屏桥接…）。
 *
 * 凡是改变"生效主题"的操作都要发：改主题 id 固然要发，换人民币面额配色 / 默认主题黑白
 * **同样改变最终配色**——只发 applyTheme 的话，这些配色变化不会通知任何人
 * （表现：主界面把面额改成 50 元（翠绿），分屏里的工作台还是 100 元红）。
 * 倾听者一律只做"重新同步"，不读 detail，所以这里无需区分变化源。
 */
function notifyThemeChange(): void {
  document.dispatchEvent(new CustomEvent("gebai:theme-change", { detail: { theme: document.documentElement.dataset.theme } }))
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
  notifyThemeChange()
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
  notifyThemeChange()
}

/**
 * 初始化主题：入口调用一次即可（主界面与文件工作台**共用**同一实现）。
 * 一次性把用户级偏好全部落到位——data-theme、主题 CSS、人民币面额配色、默认主题黑白、品牌化变量。
 * 漏掉任何一项都会导致"从主界面进工作台换了一套配色"。
 */
export function initTheme(opts: { urlPrefs?: boolean } = {}): void {
  urlPrefsEnabled = opts.urlPrefs ?? true
  if (urlPrefsEnabled) applyCustomVars(new URLSearchParams(location.search).get("gb_vars"))
  applyCnyScheme(resolveCnyScheme())
  applyAcrylicLt(resolveAcrylicLt())
  void applyTheme(resolveTheme())
  bindStorageSync()
}

/**
 * 跨标签页同步：主题是**用户级偏好**（localStorage），主界面改主题后，已打开的
 * 工作台标签页（以及反向）应即时跟随——否则「两页共享一套主题」在长开的标签页上名不副实，
 * 要等到刷新才变。只重放实际变化的项，避免无谓的 View Transition 与组件重绘。
 */
function bindStorageSync(): void {
  if (storageBound) return
  storageBound = true
  window.addEventListener("storage", (e) => {
    if (e.storageArea !== localStorage) return
    const all = e.key === null // localStorage.clear()
    if (all || e.key === STYLE_KEY) {
      userOverride = null // 持久化值即权威（本标签的手动选择被其他标签的显式选择取代）
      const next = resolveTheme()
      if (next !== document.documentElement.dataset.theme) void applyTheme(next)
    }
    if (all || e.key === SCHEME_KEY) {
      userCnyScheme = null
      const next = resolveCnyScheme()
      if (next !== (document.documentElement.dataset.cnyScheme ?? null)) {
        applyCnyScheme(next)
        notifyThemeChange()
      }
    }
    if (all || e.key === ACRYLIC_LT_KEY) {
      userAcrylicLt = null
      const next = resolveAcrylicLt()
      if (next !== (document.documentElement.dataset.acrylicLt ?? null)) {
        applyAcrylicLt(next)
        notifyThemeChange()
      }
    }
  })
}
