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
/** 主按钮（分屏开关，常驻可见）。副按钮（新标签打开）的绑定在 files-entry.ts 里，本模块不管它。 */
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

/**
 * 分屏面板 = **纯容器**：只有 iframe 与左侧分界拖条，没有标题栏。
 *
 * 为什么不留标题栏：它要为一条 30px 的横条付出整个编辑区的垂直空间，而里面那些按钮各有更好的去处——
 *   · 「打开」= 标题栏入口主按钮（分屏）与副按钮（新标签）；
 *   · 「重新加载 / 在新标签打开 / 关闭分屏」= 扫进工作台自己的「更多」菜单（页面级动作归页面自己）；
 *   · 「关闭」另有**面板内**与全局几条路径（活动栏最下方的「关闭分屏」、Esc（两侧都能触发）、
 *     Ctrl+Shift+E；另有点标题栏「会话列表」也会关，见 bindFilesSplit 末尾）。
 * 面板因此完全让位给工作台本身：它自己就是 IDE 式界面，自带顶栏与状态栏。
 */
function buildPane(): HTMLElement {
  const el = document.createElement("div")
  el.id = "files-split"
  el.hidden = true

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

  el.append(resizer, f)
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

/**
 * 主按钮的提示文案随分屏态变（副按钮的文案固定，它是“新标签打开”、与分屏态无关）。
 *
 * 标题栏上不再有✕：关闭分屏改由**工作台自己**（嵌入态下它就在面板里，那里才是"关掉我"的自然位置）：
 * 「更多」菜单的「关闭分屏」、活动栏最下方的「关闭分屏」、工作台内的 Esc（它自己转发给宿主，
 * 见 files/main.ts）、以及全局的 Ctrl+Shift+E；此外点标题栏的「会话列表」也会关分屏（见 bindFilesSplit 末尾）。
 */
function syncEntry(): void {
  const open = isSplitOpen()
  if (mainBtn) {
    /*
     * 文案取「动作（快捷键）」两句式，与标题栏其他入口同调（如「新会话（Ctrl+N / Ctrl+Shift+O）」）。
     * 早先这里是「分屏打开（右侧对照，可拖动分界 · Ctrl+Shift+E）」——一个 30 字的单行气泡，
     * 比按钮宽四倍、压在按钮下方，悬浮时相当抢眼；而「右侧对照 / 可拖动分界」是点下去一眼就懂的事，
     * 不必写进提示。
     */
    const tip = open ? "关闭分屏（Ctrl+Shift+E / Esc）" : "分屏打开（Ctrl+Shift+E）"
    mainBtn.dataset.tip = tip
    mainBtn.setAttribute("aria-label", tip)
    mainBtn.setAttribute("aria-expanded", String(open))
    // 按钮互换后，主按钮**就是那个开关**，得自己表达开关态（以前靠旁边的✕，现✕已移除）。
    // .icon-btn.active 是全站通用的"已开启"语义（轮盘按钮同款）。
    mainBtn.classList.toggle("active", open)
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

  // 窗口缩小到分屏下限以下：自动退出（否则两侧都挤成条，比新标签更糟）。
  // 合并到一帧：拖动窗口时 resize 每事件一次，量 rect + 写 CSS 变量会连带着抖动
  let resizeRaf = 0
  window.addEventListener("resize", () => {
    if (resizeRaf) return
    resizeRaf = requestAnimationFrame(() => {
      resizeRaf = 0
      if (!isSplitOpen()) return
      if (window.innerWidth < MIN_WINDOW) exitSplit()
      else applyWidth(pane ? Math.round(pane.getBoundingClientRect().width) : null)
    })
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
  if (!mainBtn) return
  /*
   * 主按钮 = 分屏开关。
   * 指针点击时 `:focus-visible` 为 false，于是按钮淡出后**仍然握着焦点**：
   * 此后随手按一下 Enter/空格，分屏会"隐形地"再切一次；聚焦提示气泡也会悬在那儿不散。
   * 所以指针触发的点击后主动让出焦点；键盘触发的保留（:focus-visible 会让它继续显形）。
   */
  mainBtn.addEventListener("click", () => {
    const fromKeyboard = mainBtn!.matches(":focus-visible")
    toggleSplit()
    if (!fromKeyboard) mainBtn?.blur()
  })
  // 没有标题栏✕：关闭走工作台自己的「更多」菜单（嵌入态的「关闭分屏」）、Esc（两侧都能触发）
  // 与全局 Ctrl+Shift+E（由 main 的快捷键表统一处理）。
  // Esc 关闭：焦点在主界面这一侧时可用（焦点在工作台内部时由它自己转发，见 files/main.ts）
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && isSplitOpen()) exitSplit()
  })
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
