/**
 * 分屏模式：把文件工作台嵌进主界面（一个**停靠侧** + 一个**宽度**，两者都可调、都持久化）。
 *
 * ## 停靠侧（会话区与工作台左右互换）
 *
 * 缺省**左侧**（文件工作区在左、会话区在右，`gebai.ui.filesSplitSide` 持久化）；工作台「更多」菜单里
 * 可随时改到另一侧（嵌入态才有的「停靠改到左侧 / 右侧」）。布局全部由 `body[data-files-split-side]`
 * 驱动——列序、标题栏归哪一列、分界线朝向、拖条贴哪条边、主题的太阳以哪块为中心，CSS 一处都不用改 JS；
 * 换侧因此只是改一个属性 + 重发一条消息，**iframe 不重载**（工作台的标签、滚动位置、Git 状态全留着）。
 *
 * ## 为什么用 iframe，而不是把工作台组件化搬进主界面
 *
 * 工作台本身就是独立页面（vite 多入口 `files.html`：Monaco + Git 面板 + 完整状态模型）。
 * iframe 让它保持**单实例**、**故障隔离**（编辑器崩了不带走会话，反之亦然）与**状态独立**
 * （滚到哪、开了哪些标签、底部工具窗高低都是它自己的事）；主界面这边只多出「一个容器」的概念。
 * 组件化则要把整套状态搬进 SPA 生命周期，且与「新标签打开」形成两套运行形态，收益不抵成本。
 *
 * 代价是跨界的几个动作（主题变更、关闭、停靠侧）要走 `postMessage` 桥接——就是下面 `bridge` 那几段。
 *
 * ## 状态与生命周期
 *
 * - 关闭分屏**不销毁 iframe**（只 `hidden`）：IDE 里工具窗关掉再开也是原样，工作台重新加载
 *   一次要重建 Monaco/Git 状态，几秒白屏不值当。真正销毁是页面刷新。
 * - 宽度持久化（localStorage），默认 **50vw**（五五开）；**停靠侧**同样持久化；打开状态不持久化——
 *   页面加载即拉起一个重工作台，对多数访问是浪费。
 * - 折叠/展开带 **220ms 宽度过渡**（`#files-split.anim`，见 files-split.css）：面板宽度是普通可过渡
 *   属性，网格列用 `auto` 跟着它走，于是左列会话区在同一帧里平滑吃掉/让出这段宽度。
 */
import { filesUrl, type FilesOpenOpts } from "./files-entry"
import { clampSplitWidth, normalizeSplitSide, splitWidthFromPointer, SPLIT_MIN_WINDOW, type SplitSide } from "./files-split-core"

export type { SplitSide }

/** 分屏宽度（px）持久化键；缺省用 50vw。 */
const W_KEY = "gebai.ui.filesSplitW"
/** 停靠侧持久化键；缺省由 files-split-core 的 SPLIT_DEFAULT_SIDE 决定（左侧）。 */
const SIDE_KEY = "gebai.ui.filesSplitSide"
/** 折叠/展开过渡时长（ms）——与 files-split.css 里 `#files-split.anim` 的 width 过渡同值。 */
const ANIM_MS = 220

let pane: HTMLElement | null = null
let frame: HTMLIFrameElement | null = null
/** 主按钮（分屏开关，常驻可见）。副按钮（新标签打开）的绑定在 files-entry.ts 里，本模块不管它。 */
let mainBtn: HTMLButtonElement | null = null
/** 记住上一次进入分屏时的参数，重新打开时沿用（会话/根/主题由 filesUrl 现取）。 */
let lastOpts: FilesOpenOpts = {}
/**
 * 分屏是否**打开**。
 * 收起的过渡期内即为 false——此刻再点开关是「重新推开」（enterSplit 会把宽度又交回 CSS 变量，
 * 过渡从当前宽度续上，不会先塌到 0 再展开）。
 */
let open = false
/** 当前停靠侧（内存态；读自 readSide，写在 setSplitSide）。 */
let side: SplitSide = readSide()
/** 生效宽度的 px 值；null = 交给 CSS 的 50vw（真正的五五开，窗口缩放时自己跟）。 */
let targetW: number | null = null
/** 收起动画的兜底/收尾定时器（transitionend 不保证来：过渡被中途打断、元素被隐藏、浏览器优化掉远帧都可能）。 */
let animTimer = 0

/**
 * 分屏是否处于打开态。
 * 收起的 220ms 过渡里返回 false：这段时间里按钮已是「分屏打开」，语义上就该按已关对待
 * （Esc/点会话列表再关一次没有意义，而重新点是「推开」）。
 */
export function isSplitOpen(): boolean {
  return open
}

/* --------------------------- 停靠侧 --------------------------- */

function readSide(): SplitSide {
  try {
    // 归一到 left/right 在 files-split-core（脏值 → 缺省侧），那支有单测守着
    return normalizeSplitSide(localStorage.getItem(SIDE_KEY))
  } catch {
    return normalizeSplitSide(null)
  }
}

/**
 * 换停靠侧（文件工作区在左 ↔ 在右）。
 * 只改 `body[data-files-split-side]`：列序/标题栏列/分界线/拖条边全在 CSS 里按它分支，
 * 面板宽度不变，因此 iframe 内的编辑区一点也不用重排（工作台只多收一条「侧」消息，用于换图标朝向后重画活动栏）。
 */
export function setSplitSide(next: SplitSide): void {
  if (next !== "left" && next !== "right") return
  if (next === side) return
  side = next
  try {
    localStorage.setItem(SIDE_KEY, side)
  } catch {
    /* 隐私模式忽略 */
  }
  applySide()
  syncEntry()
}

/** 左右互换（工作台「更多」菜单里的那一项走这里）。 */
export function toggleSplitSide(): void {
  setSplitSide(side === "left" ? "right" : "left")
}

/** 应用停靠侧：写 body 属性（CSS 据此换列序与朝向）+ 通知 iframe 里的工作台。 */
function applySide(): void {
  document.body.dataset.filesSplitSide = side
  postSide()
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

/**
 * 写入生效宽度：根元素上的 `--files-split-w` —— 面板宽度、主题的太阳定位、分屏的栅格列都读它。
 * 只写变量、不碰面板的内联宽度：面板宽度归 CSS（`width: var(--files-split-w, 50vw)`），
 * 折叠动画期间才用内联宽度把它压到 0（见 openPane/closePane）。
 */
function applyWidth(w: number | null): void {
  const root = document.documentElement
  if (w === null) {
    root.style.removeProperty("--files-split-w")
    targetW = null
  } else {
    const c = clampSplitWidth(w, window.innerWidth)
    root.style.setProperty("--files-split-w", `${c}px`)
    targetW = c
  }
}

/* --------------------------- 分屏面板 --------------------------- */

/**
 * 分屏面板 = **纯容器**：只有 iframe 与分界线上的拖条，没有标题栏。
 *
 * 为什么不留标题栏：它要为一条 30px 的横条付出整个编辑区的垂直空间，而里面那些按钮各有更好的去处——
 *   · 「打开」= 标题栏入口主按钮（分屏）与副按钮（新标签）；
 *   · 「重新加载 / 在新标签打开 / 停靠改到左侧 / 关闭分屏」= 扫进工作台自己的「更多」菜单（页面级动作归页面自己）；
 *   · 「关闭」另有**面板内**与全局几条路径（活动栏最下方的「关闭分屏」、Esc（两侧都能触发）、
 *     Ctrl+Shift+E；另有点标题栏「会话列表」也会关，见 bindFilesSplit 末尾）。
 * 面板因此完全让位给工作台本身：它自己就是 IDE 式界面，自带顶栏与状态栏。
 */
function buildPane(): HTMLElement {
  const el = document.createElement("div")
  el.id = "files-split"
  el.hidden = true

  // 分界拖条：全高，hover 高亮；双击回到五五开（贴哪条边由停靠侧决定，见 files-split.css）
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
  // 加载完成即补一次主题与停靠侧：首帧的消息早于子页监听器注册是常态，靠 load 事件兜底。
  f.addEventListener("load", () => {
    postTheme()
    postSide()
  })

  el.append(resizer, f)
  // 过渡的收尾统一走 settle（transitionend 与兜底定时器都进它）：
  // 展开完撤 .anim（否则窗口缩放会拖出 220ms 橡皮筋），收起完藏面板。
  el.addEventListener("transitionend", (e) => {
    const ev = e as TransitionEvent
    if (ev.target !== el || ev.propertyName !== "width") return
    settle(el)
  })
  pane = el
  frame = f
  return el
}

function bindResizer(resizer: HTMLElement): void {
  let dragging = false

  const onMove = (e: PointerEvent): void => {
    if (!dragging || !pane) return
    // 面板贴着窗口的哪一侧，就用「指针到那一侧边缘」的距离当宽度（换算在 files-split-core，有单测）
    applyWidth(splitWidthFromPointer(e.clientX, pane.getBoundingClientRect().left, window.innerWidth, side))
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
    if (targetW !== null) {
      try {
        localStorage.setItem(W_KEY, String(targetW))
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

/** 用户关掉了动效（系统级「减少动态效果」）：折叠/展开直接到位，不留过渡。 */
function prefersReducedMotion(): boolean {
  return typeof window.matchMedia === "function" && window.matchMedia("(prefers-reduced-motion: reduce)").matches
}

export function enterSplit(opts: FilesOpenOpts = {}): void {
  if (window.innerWidth < SPLIT_MIN_WINDOW) {
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
  applySide()
  applyWidth(storedWidth())

  const alreadyOpen = open
  open = true
  clearTimeout(animTimer)
  animTimer = 0
  if (!alreadyOpen || el.hidden) openPane(el)
  syncEntry()
}


export function exitSplit(opts: { animate?: boolean } = {}): void {
  if (!pane || !open) return
  open = false
  clearTimeout(animTimer)
  animTimer = 0
  syncEntry()
  // 窗口窄到分屏下限以下时的自动退出不播动画：面板马上要被挤没，过渡只会拖出一个残影
  if (opts.animate === false || prefersReducedMotion()) finishClose(pane)
  else closePane(pane)
}

export function toggleSplit(opts: FilesOpenOpts = {}): void {
  if (isSplitOpen()) exitSplit()
  else enterSplit(opts)
}

/**
 * 推开面板（带宽度过渡）。
 *
 * 顺序是关键：**先把内联宽度写成 0 并强制一帧**，再移除它把宽度交回 CSS 变量——
 * 否则浏览器只看到「hidden → 显示 + 目标宽度」这一次状态变化，没有"上一个宽度"可插值，动画根本不会发生。
 */
function openPane(el: HTMLElement): void {
  document.body.classList.add("files-split")
  el.hidden = false
  if (prefersReducedMotion()) {
    el.classList.remove("anim")
    el.style.removeProperty("width")
    return
  }
  el.classList.add("anim")
  el.style.width = "0px"
  void el.offsetWidth
  el.style.removeProperty("width")
  // 兜底：过渡被中途打断（拖分界/换侧）而不再有 transitionend 时，.anim 不能永远留着
  armSettleTimer(el)
}

/** 收合面板（带宽度过渡）：先过渡到 0 宽，收尾交给 settle（transitionend 或定时器先到者）。 */
function closePane(el: HTMLElement): void {
  el.classList.add("anim")
  el.style.width = "0px"
  armSettleTimer(el)
}

/** 兜底定时器：比 CSS 过渡时长略长，收起时万一 transitionend 不来也能收尾。 */
function armSettleTimer(el: HTMLElement): void {
  clearTimeout(animTimer)
  animTimer = window.setTimeout(() => settle(el), ANIM_MS + 120)
}

/**
 * 过渡收尾（transitionend 与兜底定时器共用，幂等）：
 * 展开态只把 `.anim` 撤掉（面板保持打开），收起态才真藏面板（见 finishClose）。
 */
function settle(el: HTMLElement): void {
  clearTimeout(animTimer)
  animTimer = 0
  if (open) el.classList.remove("anim")
  else finishClose(el)
}

/** 收起收尾：藏面板、撤过渡态、退出分屏栅格（`--files-split-w` 与 body 类一起收回）。 */
function finishClose(el: HTMLElement): void {
  clearTimeout(animTimer)
  animTimer = 0
  if (open) return // 收起途中又被推开：收尾交给展开流程
  el.hidden = true
  el.style.removeProperty("width")
  el.classList.remove("anim")
  document.body.classList.remove("files-split")
  applyWidth(null)
  syncEntry()
}

/**
 * 主按钮的提示文案随分屏态变（副按钮的文案固定，它是“新标签打开”、与分屏态无关）。
 *
 * 标题栏上不再有✕：关闭分屏改由**工作台自己**（嵌入态下它就在面板里，那里才是"关掉我"的自然位置）：
 * 「更多」菜单的「关闭分屏」、活动栏最下方的「关闭分屏」、工作台内的 Esc（它自己转发给宿主，
 * 见 files/main.ts）、以及全局的 Ctrl+Shift+E；此外点标题栏的「会话列表」也会关分屏（见 bindFilesSplit 末尾）。
 */
function syncEntry(): void {
  const isOpen = isSplitOpen()
  if (mainBtn) {
    /*
     * 文案取「动作（快捷键）」两句式，与标题栏其他入口同调（如「新会话（Ctrl+N / Ctrl+Shift+O）」）。
     * 早先这里是「分屏打开（右侧对照，可拖动分界 · Ctrl+Shift+E）」——一个 30 字的单行气泡，
     * 比按钮宽四倍、压在按钮下方，悬浮时相当抢眼；而「右侧对照 / 可拖动分界」是点下去一眼就懂的事，
     * 不必写进提示。
     */
    const tip = isOpen ? "关闭分屏（Ctrl+Shift+E / Esc）" : "分屏打开（Ctrl+Shift+E）"
    mainBtn.dataset.tip = tip
    mainBtn.setAttribute("aria-label", tip)
    mainBtn.setAttribute("aria-expanded", String(isOpen))
    // 按钮互换后，主按钮**就是那个开关**，得自己表达开关态（以前靠旁边的✕，现✕已移除）。
    // .icon-btn.active 是全站通用的"已开启"语义（轮盘按钮同款）。
    mainBtn.classList.toggle("active", isOpen)
  }
}

/* --------------------------- 跨界桥接 --------------------------- */

let bridged = false

/**
 * 主题、停靠侧与关闭这三件事必须跨界，其余一概不桥（桥越多耦合越紧）。
 * 主题：工作台是独立文档，改了主界面主题它不会自己变，只能显式通知；
 * 停靠侧：工作台的「关闭分屏」箭头要指出向、「更多」菜单那项要写对文案；
 * 关闭：嵌在 iframe 里的工作台点「关闭分屏」时应关掉面板，而不是把 iframe 导航到主界面。
 */
function ensureBridge(): void {
  if (bridged) return
  bridged = true

  document.addEventListener("gebai:theme-change", () => postTheme())

  window.addEventListener("message", (e: MessageEvent) => {
    if (e.origin !== location.origin || e.source !== frame?.contentWindow) return
    const data = e.data as { type?: string } | null
    if (data?.type === "gebai:files-close-split") exitSplit()
    if (data?.type === "gebai:files-open-tab") window.open(frame?.src ?? filesUrl(lastOpts), "_blank", "noopener")
    if (data?.type === "gebai:files-split-swap") toggleSplitSide()
  })

  // 窗口缩小到分屏下限以下：自动退出（否则两侧都挤成条，比新标签更糟）。
  // 合并到一帧：拖动窗口时 resize 每事件一次，量 rect + 写 CSS 变量会连带着抖动
  let resizeRaf = 0
  window.addEventListener("resize", () => {
    if (resizeRaf) return
    resizeRaf = requestAnimationFrame(() => {
      resizeRaf = 0
      if (!isSplitOpen()) return
      if (window.innerWidth < SPLIT_MIN_WINDOW) exitSplit({ animate: false })
      // 自定义宽度重新夹进新窗口（缺省五五开不用管：CSS 里的 50vw 自己跟）
      else if (targetW !== null) applyWidth(targetW)
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

/** 把停靠侧推给 iframe（工作台据此选「关闭分屏」箭头朝向与「停靠改到左/右」文案）。 */
function postSide(): void {
  frame?.contentWindow?.postMessage({ type: "gebai:files-split-side", side }, location.origin)
}

/* --------------------------- 绑定入口 --------------------------- */

export function bindFilesSplit(): void {
  // 停靠侧先落到 body 上：CSS 的列序/朝向分支（含 #files-split 自身）读它，不必等第一次进分屏
  applySide()
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
