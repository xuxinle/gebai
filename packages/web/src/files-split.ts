/**
 * 分屏模式：把文件工作台嵌在主界面右侧，左侧是被折叠了会话列表的主视图。
 *
 * ## 为什么用 iframe，而不是把工作台组件化搬进主界面
 *
 * 工作台本身就是独立页面（vite 多入口 `files.html`：Monaco + Git 面板 + 完整状态模型）。
 * iframe 让它保持**单实例**、**故障隔离**（编辑器崩了不带走会话，反之亦然）与**状态独立**
 * （滚到哪、开了哪些标签、底部工具窗高低都是它自己的事）；主界面这边只多出「一个容器」的概念。
 * 组件化则要把整套状态搬进 SPA 生命周期，且与「新标签打开」形成两套运行形态，收益不抵成本。
 *
 * 代价是跨界的两个动作（主题变更、关闭）要走 `postMessage` 桥接——就是下面 `bridge` 那几段。
 *
 * ## 状态与生命周期
 *
 * - 关闭分屏**不销毁 iframe**（只 `hidden`）：IDE 里工具窗关掉再开也是原样，工作台重新加载
 *   一次要重建 Monaco/Git 状态，几秒白屏不值当。真正销毁是页面刷新。
 * - 宽度持久化（localStorage），默认 **50vw**（五五开）；打开状态不持久化——页面加载即拉起
 *   一个重工作台，对多数访问是浪费。
 */
import { filesUrl, type FilesOpenOpts } from "./files-entry"

/** 分屏宽度（px）持久化键；缺省用 50vw。 */
const W_KEY = "gebai.ui.filesSplitW"
/** 左栏最小宽度（主视图再窄就没法看消息了）。 */
const MIN_LEFT = 420
/** 右侧工作台最小宽度（窄于此 ID/编辑器就没意义，此时不如新标签打开）。 */
const MIN_SPLIT = 360
/** 低于此窗口宽度不提供分屏（左右都挤成条），改为新标签打开。 */
const MIN_WINDOW = 1100

let pane: HTMLElement | null = null
let frame: HTMLIFrameElement | null = null
let peekBtn: HTMLButtonElement | null = null
let mainBtn: HTMLButtonElement | null = null
/** 记住上一次进入分屏时的参数，重新打开时沿用（会话/根/主题由 filesUrl 现取）。 */
let lastOpts: FilesOpenOpts = {}

export function isSplitOpen(): boolean {
  return !!pane && !pane.hidden
}

/* --------------------------- 宽度 --------------------------- */

function storedWidth(): number | null {
  try {
    const raw = localStorage.getItem(W_KEY)
    if (!raw) return null
    const n = Number(raw)
    return Number.isFinite(n) && n > 0 ? n : null
  } catch {
    return null
  }
}

/** 把宽度限制在「右侧不小于 MIN_SPLIT、左侧不小于 MIN_LEFT」区间内。 */
function clampWidth(w: number): number {
  const max = Math.max(MIN_SPLIT, window.innerWidth - MIN_LEFT)
  return Math.round(Math.min(Math.max(w, MIN_SPLIT), max))
}

function applyWidth(w: number | null): void {
  const root = document.documentElement
  if (w === null) root.style.removeProperty("--files-split-w")
  else root.style.setProperty("--files-split-w", `${clampWidth(w)}px`)
}

/* --------------------------- 分屏面板 --------------------------- */

/** 分屏面板内的图标（内联 SVG：与标题栏按钮同一套视觉，2px 描边 + currentColor）。 */
function svgIcon(d: string, opts: { fill?: boolean } = {}): string {
  return opts.fill
    ? `<svg viewBox="0 0 24 24" width="14" height="14" fill="currentColor" aria-hidden="true"><path d="${d}"/></svg>`
    : `<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="${d}"/></svg>`
}

const ICON = {
  新标签: "M14 3h7v7M21 3l-9 9M19 14v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2h5",
  重新加载: "M21 12a9 9 0 1 1-2.6-6.4M21 3v6h-6",
  关闭: "M6 6l12 12M18 6L6 18",
  /** 分屏图标：窗口 + 代码尖括号（与 index.html 里的入口副按钮同一形状，实心） */
  分屏: "M4 4h16c1.1 0 2 .9 2 2v12c0 1.1-.9 2-2 2H4c-1.1 0-2-.9-2-2V6c0-1.1.9-2 2-2zm-1 4v10h18V8H3zm7.6 4.4L7.5 15.5l3.1 3.1 1.2-1.2-1.9-1.9 1.9-1.9-1.2-1.2zm2.8 0-1.2 1.2 1.9 1.9-1.9 1.9 1.2 1.2 3.1-3.1-3.1-3.1z",
}

function buildPane(): HTMLElement {
  const el = document.createElement("div")
  el.id = "files-split"
  el.hidden = true

  const bar = document.createElement("div")
  bar.className = "files-split-bar"

  const title = document.createElement("span")
  title.className = "files-split-title"
  title.textContent = "文件工作台"

  const spacer = document.createElement("div")
  spacer.className = "files-split-spacer"

  const mkBtn = (d: string, tip: string, onClick: () => void, fill = false): HTMLButtonElement => {
    const b = document.createElement("button")
    b.className = "icon-btn"
    b.type = "button"
    b.dataset.tip = tip
    b.setAttribute("aria-label", tip)
    b.innerHTML = svgIcon(d, { fill })
    b.addEventListener("click", onClick)
    return b
  }

  bar.append(
    title,
    spacer,
    mkBtn(ICON.新标签, "在新标签打开", () => {
      window.open(frame?.src ?? filesUrl(lastOpts), "_blank", "noopener")
    }),
    mkBtn(ICON.重新加载, "重新加载工作台", () => {
      if (frame) frame.src = filesUrl({ ...lastOpts, path: undefined })
    }),
    mkBtn(ICON.关闭, "关闭分屏（Esc 亦可；工作台状态保留）", () => exitSplit()),
  )

  // 分界拖条：全高，hover 高亮；双击回到五五开
  const resizer = document.createElement("div")
  resizer.className = "files-split-resizer"
  resizer.title = "拖动调整宽度（双击恢复五五开）"
  bindResizer(resizer)

  const f = document.createElement("iframe")
  f.className = "files-split-frame"
  f.title = "文件工作台"
  // 不用 sandbox：同源同信任级别（工作台就是本站页面），sandbox 反而会切断 localStorage
  // （主题偏好）与下载等能力；它需要的只是一个「容器」。
  f.setAttribute("referrerpolicy", "same-origin")

  el.append(bar, resizer, f)
  el.addEventListener("keydown", (e) => {
    if (e.key === "Escape") exitSplit()
  })
  pane = el
  frame = f
  return el
}

function bindResizer(resizer: HTMLElement): void {
  let dragging = false

  const onMove = (e: PointerEvent): void => {
    if (!dragging) return
    // 右侧面板贴着窗口右边：宽度 = 窗口宽 - 指针 x
    applyWidth(window.innerWidth - e.clientX)
  }
  /*
   * pointerup 可能落在 iframe 里：指针一旦进入 iframe，它自己的文档接管事件，
   * 拖条上的 pointerleave/pointerup 就收不到，拖动会"粘住"不结束。
   * 两道保险：① iframe 上的 pointer-events 在拖动时置 none（见 files-split.css，
   * 但那是"下一次移动时"才生效，快速拖动仍可能丢）；② pointerup 同时监听 window
   * 的**捕获阶段**——window 在文档之外，iframe 内松开也收得到。
   */
  const onUp = (): void => {
    if (!dragging) return
    dragging = false
    resizer.classList.remove("dragging")
    document.body.classList.remove("files-split-dragging")
    const w = pane ? Math.round(pane.getBoundingClientRect().width) : 0
    if (w) {
      try {
        localStorage.setItem(W_KEY, String(w))
      } catch {
        /* 隐私模式忽略 */
      }
    }
  }

  resizer.addEventListener("pointerdown", (e) => {
    e.preventDefault()
    dragging = true
    resizer.classList.add("dragging")
    document.body.classList.add("files-split-dragging")
    resizer.setPointerCapture(e.pointerId)
  })
  resizer.addEventListener("pointermove", onMove)
  resizer.addEventListener("pointerup", onUp)
  resizer.addEventListener("pointercancel", onUp)
  window.addEventListener("pointerup", onUp, true)
  window.addEventListener("blur", onUp) // 切窗口（如 Alt+Tab）也算松手
  // 双击：清掉自定义宽度 → 回到 CSS 里的 50vw
  resizer.addEventListener("dblclick", () => {
    try {
      localStorage.removeItem(W_KEY)
    } catch {
      /* 忽略 */
    }
    applyWidth(null)
  })
}

/* --------------------------- 进入 / 退出 --------------------------- */

export function enterSplit(opts: FilesOpenOpts = {}): void {
  if (window.innerWidth < MIN_WINDOW) {
    // 左右都挤成条时不提供分屏，直接新标签——比给一个残废的分屏好
    window.open(filesUrl(opts), "_blank", "noopener")
    return
  }
  lastOpts = { ...opts }
  const app = document.getElementById("app")
  if (!app) return
  const el = pane ?? buildPane()
  if (!el.isConnected) app.appendChild(el)
  ensureBridge()

  const url = filesUrl(opts)
  // 已经开着且是同一个目标就不用换 src（保留工作台的标签与滚动位置）
  if (frame && frame.src !== new URL(url, location.href).href) frame.src = url
  applyWidth(storedWidth())
  el.hidden = false
  document.body.classList.add("files-split")
  syncEntry()
}

export function exitSplit(): void {
  if (!pane) return
  pane.hidden = true
  document.body.classList.remove("files-split")
  applyWidth(null)
  syncEntry()
}

export function toggleSplit(opts: FilesOpenOpts = {}): void {
  if (isSplitOpen()) exitSplit()
  else enterSplit(opts)
}

/** 按钮态与分屏态保持一致（主按钮/悬浮按钮的提示与高亮）。 */
function syncEntry(): void {
  const open = isSplitOpen()
  mainBtn?.classList.toggle("active", open)
  peekBtn?.classList.toggle("active", open)
  peekBtn?.setAttribute("aria-expanded", String(open))
  if (peekBtn) {
    const tip = open ? "关闭分屏" : "分屏打开（右侧对照，可拖动分界）"
    peekBtn.dataset.tip = tip
    peekBtn.setAttribute("aria-label", tip)
    peekBtn.innerHTML = open ? svgIcon(ICON.关闭) : svgIcon(ICON.分屏, { fill: true })
  }
}

/* --------------------------- 跨界桥接 --------------------------- */

let bridged = false

/**
 * 主题与关闭这两件事必须跨界，其余一概不桥（桥越多耦合越紧）。
 * 主题：工作台是独立文档，改了主界面主题它不会自己变，只能显式通知；
 * 关闭：嵌在 iframe 里的工作台点「返回歌白主界面」时应关掉分屏，而不是把 iframe 导航到主界面。
 */
function ensureBridge(): void {
  if (bridged) return
  bridged = true

  document.addEventListener("gebai:theme-change", () => postTheme())
  frame?.addEventListener("load", () => postTheme())

  window.addEventListener("message", (e: MessageEvent) => {
    if (e.origin !== location.origin || e.source !== frame?.contentWindow) return
    const data = e.data as { type?: string } | null
    if (data?.type === "gebai:files-close-split") exitSplit()
    if (data?.type === "gebai:files-open-tab") window.open(frame?.src ?? filesUrl(lastOpts), "_blank", "noopener")
  })

  // 窗口缩小到分屏下限以下：自动退出（否则两侧都挤成条，比新标签更糟）
  window.addEventListener("resize", () => {
    if (isSplitOpen() && window.innerWidth < MIN_WINDOW) exitSplit()
    else if (isSplitOpen()) applyWidth(pane ? Math.round(pane.getBoundingClientRect().width) : null)
  })
}

/** 把当前生效的主题/配色推给 iframe（主界面 documentElement 上的 dataset 是权威值）。 */
function postTheme(): void {
  const el = document.documentElement
  frame?.contentWindow?.postMessage(
    { type: "gebai:theme", theme: el.dataset.theme ?? null, cnyScheme: el.dataset.cnyScheme ?? null, acrylicLt: el.dataset.acrylicLt ?? null },
    location.origin,
  )
}

/* --------------------------- 绑定入口 --------------------------- */

export function bindFilesSplit(): void {
  mainBtn = document.getElementById("files-btn") as HTMLButtonElement | null
  peekBtn = document.getElementById("files-split-btn") as HTMLButtonElement | null
  if (!peekBtn) return
  // 主按钮仍是「新标签打开」（默认行为不变）；悬浮弹出的副按钮才进分屏
  peekBtn.addEventListener("click", () => toggleSplit())
  // 分屏开着时点「会话列表」按钮 = 我要看会话：退出分屏把列表拿回来，
  // 而不是去切一个此刻根本看不见的栏位（否则那个按钮在分屏期间形同死去）。
  // 挂 document 捕获阶段：先于按钮自己的处理器，才拦得住。
  document.addEventListener(
    "click",
    (e) => {
      if (!isSplitOpen()) return
      if (!(e.target as HTMLElement | null)?.closest("#sidebar-toggle")) return
      e.stopPropagation()
      e.preventDefault()
      exitSplit()
    },
    true,
  )
  syncEntry()
}
