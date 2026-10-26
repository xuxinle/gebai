/**
 * 拟人浮层（标题栏轮盘「拟人」按钮打开的**可拖动**浮层）。
 *
 * ## 为什么不放设置面板里
 *
 * 拟人模式的日常问题是「**它现在到底在不在干活**」——凭证有没有过期、群里来的消息
 * 有没有被人格接管、分身是不是卡住了。这些是要**顺手看一眼**的状态，不是一次性配置。
 * 塞进设置里意味着：每次想看都要打开设置 → 切 tab → 读完 → 关掉，四步换一眼。
 *
 * 而它又**不该顶到主界面上**：#app 是严格 2×2 grid（header 占 `1 / -1`，aside/main
 * 精确占格），往里面插任何常驻元素都会多出一个 grid item、把 aside/main 挤到错格的
 * （历史踩坑：授权横幅插进 grid 后侧栏被挤出屏幕）。所以走**浮层**——脱离文档流，
 * 与主界面布局完全无关。
 *
 * ## 与待办浮层同一套范式
 *
 * 拖动/位置持久化/「只有 ✕ 才关」都照抄 `todo-pop.ts`：那是已经过验证的交互，
 * 重写一遍只会多一处行为不一致。
 *
 * 差异只在两点：
 * - **没有编辑态**：纯展示 + 几个动作按钮，所以不避讳「点外面关闭」
 * - **打开时静默轮询**（15s）：状态是外部世界在变的（凭据会过期、消息会进来），
 *   打开着就该看到它变，而不是停在打开那一刻的快照
 */
import { clampPos, defaultPos, parsePos, type PopPos } from "./todo-core"
import { el } from "./state"
import { fetchPersonaState, renderPersonaPanel, type PersonaStateView } from "./persona-panel"

const POS_KEY = "gebai.ui.persona.pos"
const OPEN_KEY = "gebai.ui.persona.open"
const POLL_MS = 15_000

interface PersonaPopRefs {
  root: HTMLDivElement
  body: HTMLDivElement
  badge: HTMLSpanElement
}

let refs: PersonaPopRefs | null = null
let opened = false
let bound = false
let pollTimer: number | null = null
/** 上次渲染的数据签名（无变化时跳过重绘——不打断滚动）。 */
let lastSig = ""
/** 浮层拖动会话（指针位移起点）。 */
let dragWindow: { dx: number; dy: number; pid: number } | null = null
/** 上次拉到的快照（供签名与徽标使用）。 */
let lastSnapshot: PersonaStateView | null = null

function readLocal(key: string): string | null {
  try {
    return localStorage.getItem(key)
  } catch {
    return null
  }
}

function writeLocal(key: string, value: string): void {
  try {
    localStorage.setItem(key, value)
  } catch {
    /* 隐私模式/配额满：位置与开关状态丢失不影响功能 */
  }
}

/**
 * 绑定轮盘「拟人」按钮（main.ts 初始化时调用一次）。
 *
 * 按钮默认 `hidden`：拟人模式未启用（`/state` 404）时不该在轮盘上留一个点开就说
 * 「未启用」的入口。启用与否在这里探一次——**只在启动时探一次**，
 * 因为部署形态不会在页面生命周期内改变（改了要重启服务）。
 */
export function bindPersonaPop(): void {
  const btn = document.getElementById("persona-btn")
  if (!btn || bound) return
  bound = true
  btn.addEventListener("click", (e) => {
    e.preventDefault()
    void openPersonaPop()
  })
  void (async () => {
    const snapshot = await fetchPersonaState().catch(() => null)
    if (!snapshot) return // 未启用：按钮保持隐藏
    btn.hidden = false
    // 启动时就把**待授权提示点**点亮。
    //
    // 为何不能只靠 openPersonaPop 里那句：那个提示点存在的全部意义就是
    // 「告诉用户这里有东西需要他动手」——如果只在打开浮层时才点亮，
    // 那就变成“你得先点开才知道要点开”，提示等于不存在。
    lastSnapshot = snapshot
    onPersonaStateChanged()
    // 上次退出时浮层是打开的 → 自动恢复（与待办一致：重启不该丢掉你正在看的东西）
    if (readLocal(OPEN_KEY) === "1") void openPersonaPop()
  })()
}

export function isPersonaPopOpen(): boolean {
  return opened
}

export async function openPersonaPop(): Promise<void> {
  if (!refs) refs = buildPop()
  const wasOpen = opened
  opened = true
  writeLocal(OPEN_KEY, "1")
  refs.root.hidden = false
  refs.root.classList.add("show")
  if (!wasOpen) applyPos(clampNow(loadPos()), true)
  await refresh(true)
  startPoll()
}

export function closePersonaPop(): void {
  if (!refs) return
  opened = false
  writeLocal(OPEN_KEY, "0")
  refs.root.hidden = true
  refs.root.classList.remove("show")
  stopPoll()
}

/* ---------- 数据 ---------- */

/** 数据签名：只取用户能看见变化的字段（全员计数 + 凭据状态）。 */
function signature(s: PersonaStateView): string {
  const cred = s.credentials ? `${s.credentials.ready}:${s.credentials.message}:${s.credentials.remainingMs ?? 0}` : "null"
  const drivers = s.personas.flatMap((p) => p.drivers.map((d) => `${d.id}:${d.accepted}:${d.queued}:${d.running}:${d.failed}:${d.active}`)).join(",")
  const sources = s.sources.map((x) => `${x.name}:${x.started}:${x.stats.emitted}:${x.stats.accepted}:${x.stats.unattended}`).join(",")
  const chans = Object.entries(s.channelInput).map(([k, v]) => `${k}:${v.emitted}:${v.accepted}:${v.unrouted}`).join(",")
  return `${cred}|${drivers}|${sources}|${chans}|${s.totals.failed}|${s.setupError ?? ""}`
}

async function refresh(force = false): Promise<void> {
  if (!refs) return
  let snapshot: PersonaStateView | null = null
  try {
    snapshot = await fetchPersonaState()
  } catch (err) {
    refs.body.innerHTML = ""
    refs.body.appendChild(el("div", "persona-pop-error", `运行态读取失败：${(err as Error).message}`))
    return
  }
  if (!snapshot) {
    // 从「启用」变成「未启用」：只可能是服务端关了开关并重启。如实说，并把入口按钮一并隐藏
    const btn = document.getElementById("persona-btn")
    if (btn) btn.hidden = true
    refs.body.innerHTML = ""
    refs.body.appendChild(
      el("div", "persona-pop-hint", "拟人模式当前未启用（GEBAI_PERSONA=false）。重新启用后刷新页面即可。"),
    )
    setBadge("未启用", "warn")
    return
  }
  lastSnapshot = snapshot
  const sig = signature(snapshot)
  if (!force && sig === lastSig) return
  lastSig = sig
  // 凭据健康度挂在标题上：这是最需要一眼看到的那个状态
  if (snapshot.setupError) setBadge("装配失败", "bad")
  else if (!snapshot.credentials) setBadge("无凭据", "warn")
  else if (!snapshot.credentials.ready) setBadge("待授权", "warn")
  else if (snapshot.totals.failed > 0) setBadge("有失败", "bad")
  else setBadge("运行中", "ok")

  refs.body.innerHTML = ""
  if (snapshot.setupError) {
    refs.body.appendChild(el("div", "persona-pop-error", `拟人装配失败（服务仍正常运行）：${snapshot.setupError}`))
  }
  renderPersonaPanel(refs.body, snapshot, () => {
    void refresh(true)
    onPersonaStateChanged()
  })
}

/** 状态变化时需要同步的外部表现（轮盘按钮的提示态）。 */
function onPersonaStateChanged(): void {
  const btn = document.getElementById("persona-btn")
  if (!btn) return
  // 待授权是**需要人动手**的状态，值得在轮盘按钮上标出来（否则用户不会想到去点它）
  const needAuth = !!lastSnapshot && (!lastSnapshot.credentials || !lastSnapshot.credentials.ready)
  btn.classList.toggle("persona-btn-attention", needAuth)
  btn.dataset.tip = needAuth ? "拟人（有待处理：点击查看）" : "拟人"
}

function setBadge(text: string, kind: "ok" | "warn" | "bad"): void {
  if (!refs) return
  refs.badge.textContent = text
  refs.badge.className = `persona-pop-badge persona-pop-badge-${kind}`
  onPersonaStateChanged()
}

function startPoll(): void {
  stopPoll()
  pollTimer = window.setInterval(() => {
    if (!opened || document.visibilityState !== "visible") return
    void refresh()
  }, POLL_MS)
}

function stopPoll(): void {
  if (pollTimer !== null) {
    clearInterval(pollTimer)
    pollTimer = null
  }
}

/* ---------- 浮层构建与拖动 ---------- */

function buildPop(): PersonaPopRefs {
  const root = el("div", "persona-pop")
  root.hidden = true
  root.setAttribute("role", "dialog")
  root.setAttribute("aria-label", "拟人模式")

  const head = el("div", "persona-pop-head")
  const title = el("span", "persona-pop-title", "拟人")
  const badge = el("span", "persona-pop-badge persona-pop-badge-warn", "读取中")
  const refreshBtn = el("button", "persona-pop-icon", "↻")
  refreshBtn.type = "button"
  refreshBtn.dataset.tip = "刷新"
  refreshBtn.setAttribute("aria-label", "刷新")
  refreshBtn.addEventListener("click", () => void refresh(true))
  const closeBtn = el("button", "persona-pop-icon", "✕")
  closeBtn.type = "button"
  closeBtn.dataset.tip = "关闭"
  closeBtn.setAttribute("aria-label", "关闭拟人面板")
  closeBtn.addEventListener("click", () => closePersonaPop())
  head.append(title, badge, refreshBtn, closeBtn)

  const hint = el(
    "div",
    "persona-pop-hint",
    "拖动标题栏移动窗口。这里看四件事：凭据通不通、事件进不进得来、谁在干活、有没有配错。",
  )
  const body = el("div", "persona-pop-body")

  root.append(head, hint, body)
  document.body.appendChild(root)

  const r: PersonaPopRefs = { root, body, badge }
  bindHeadDrag(r)
  return r
}

/** 标题栏拖动（照 todo-pop：pointerdown + 捕获 + 位移钳制 + 丢失捕获兜底）。 */
function bindHeadDrag(r: PersonaPopRefs): void {
  const head = r.root.querySelector<HTMLElement>(".persona-pop-head")
  if (!head) return
  head.addEventListener("pointerdown", (e) => {
    if (e.button !== 0) return
    // 点在按钮上不启动拖动（否则「刷新」会变成拖一下）
    if ((e.target as HTMLElement).closest("button")) return
    const rect = r.root.getBoundingClientRect()
    dragWindow = { dx: e.clientX - rect.left, dy: e.clientY - rect.top, pid: e.pointerId }
    head.setPointerCapture(e.pointerId)
    r.root.classList.add("dragging")
  })
  head.addEventListener("pointermove", (e) => {
    if (!dragWindow || dragWindow.pid !== e.pointerId) return
    applyPos(clampTo(e.clientX - dragWindow.dx, e.clientY - dragWindow.dy), false)
  })
  const end = (e: PointerEvent) => {
    if (!dragWindow || dragWindow.pid !== e.pointerId) return
    dragWindow = null
    r.root.classList.remove("dragging")
    try {
      head.releasePointerCapture(e.pointerId)
    } catch {
      /* 已释放 */
    }
    const rect = r.root.getBoundingClientRect()
    writeLocal(POS_KEY, JSON.stringify({ x: Math.round(rect.left), y: Math.round(rect.top) }))
  }
  head.addEventListener("pointerup", end)
  head.addEventListener("pointercancel", end)
}

/* ---------- 位置 ---------- */

function loadPos(): PopPos | null {
  return parsePos(readLocal(POS_KEY))
}

/** 把位置钳制到当前视口内（窗口缩小后旧坐标可能整体在屏幕外）。 */
function clampNow(pos: PopPos | null): PopPos {
  if (!refs) return pos ?? defaultPos(420, window.innerWidth)
  const rect = refs.root.getBoundingClientRect()
  const w = rect.width || 420
  const h = rect.height || 480
  return clampPos(pos?.x ?? window.innerWidth - w - 16, pos?.y ?? 64, w, h, window.innerWidth, window.innerHeight)
}

function clampTo(x: number, y: number): PopPos {
  if (!refs) return { x, y }
  const rect = refs.root.getBoundingClientRect()
  return clampPos(x, y, rect.width || 420, rect.height || 480, window.innerWidth, window.innerHeight)
}

function applyPos(pos: PopPos, initial: boolean): void {
  if (!refs) return
  refs.root.style.left = `${pos.x}px`
  refs.root.style.top = `${pos.y}px`
  if (initial) refs.root.style.visibility = "visible"
}
