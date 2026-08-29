import { msgEl, msgNav, ROLE_NAME } from "./state"
import { stopFollowing } from "./jump-bottom"
import { createPressGesture } from "./press-gesture"

/* ---------- 会话内消息导航 ----------
 * 右侧窄导航列：每条消息一个短横线，等间距集中展示在导航列中部（不按消息实际距离分布），
 * 静止态固定 1px 细线（任何 DPR/缩放下渲染厚度恒定，不出现 2/3px 交替），活动/悬停加粗变亮（hover 3px）。
 * 悬停：在导航条左侧浮出消息预览气泡（角色 + 截断文本；用户消息省略"我"的称谓标签）。
 * 点击：滚动到对应消息（按下未拖动 = 明确点击，位移超阈值才进入拖动，防右缘扫过误触即跳）；
 * 按住拖动可"搓"过消息列（接管指针连续跳转）。
 * 短横线命中带上的滚轮转发给消息列（msg-nav 不在 #messages 滚动链上，默认滚轮无滚动目标）。
 * 设计参考 DESIGN.md「Web UI · 聊天页」。 */

const NAV_PAD = 10 // 与 chat-wrap 顶/底的留白
const SEG_H = 2 // 布局间距基准（短横线静止高 1px，间距按 2px 高度计算预留）
const SEG_GAP = 16 // 固定垂直间距：短横线紧凑聚在一起展示（间距宽松）
const TOOLTIP_MAX = 6 // 预览气泡最多展示行数
/** 按下后位移超过该值才进入拖动搓动（小于视为点击）：短横线 ::before 扩展命中区连成竖带，
 *  指针扫过右缘时轻触即跳 + 立即 pointer capture 会劫持后续拖动（「一碰就跳顶、滚轮失灵」的放大器）。 */
const DRAG_SLOP = 6

/** 按压周期内的 pointerId（手势状态机回调里接管/释放 capture 用）。 */
let pressId = -1

const gesture = createPressGesture({
  slop: DRAG_SLOP,
  onDragStart() {
    if (pressId >= 0) {
      try { msgNav.setPointerCapture(pressId) } catch { /* 已失活 */ }
    }
    msgNav.classList.add("dragging")
  },
  onDrag(p) {
    const idx = idxFromClientY(p.y)
    if (idx >= 0) jumpToIdx(idx)
  },
  onClick(p) {
    // 明确点击：命中带内按 Y 比例定位目标条（等距布局下比例最近 = 点击的那条）
    const idx = idxFromClientY(p.y)
    if (idx >= 0) jumpToIdx(idx)
  },
})

function onPointerDown(e: PointerEvent) {
  if (!segs.length) return
  pressId = e.pointerId
  gesture.down({ x: e.clientX, y: e.clientY })
  e.preventDefault()
}

function onPointerMove(e: PointerEvent) {
  gesture.move({ x: e.clientX, y: e.clientY })
}

function onPointerUp(e: PointerEvent) {
  if (gesture.isDragging()) {
    try { msgNav.releasePointerCapture(e.pointerId) } catch { /* 已被释放 */ }
    msgNav.classList.remove("dragging")
  }
  pressId = -1
  gesture.up({ x: e.clientX, y: e.clientY })
}

/** 系统取消手势（窗口失焦/触控打断）：只清理状态，不触发点击跳转。 */
function onPointerCancel(e: PointerEvent) {
  if (gesture.isDragging()) {
    try { msgNav.releasePointerCapture(e.pointerId) } catch { /* 已被释放 */ }
    msgNav.classList.remove("dragging")
  }
  pressId = -1
  gesture.cancel()
}

interface Seg {
  bar: HTMLElement
  msg: HTMLElement
  role: "user" | "assistant" | "tool" | "system"
  text: string
  name: string
}

let segs: Seg[] = []
let tooltip: HTMLElement | null = null
let rafPending = false
let activeIdx = -1 // 当前位置高亮的短横线索引（-1 = 无）

function makeTooltip(): HTMLElement {
  const tip = document.createElement("div")
  tip.className = "msg-nav-tip"
  tip.hidden = true
  document.body.appendChild(tip)
  return tip
}

function roleOf(msg: HTMLElement): Seg["role"] {
  if (msg.classList.contains("user")) return "user"
  if (msg.classList.contains("tool")) return "tool"
  if (msg.classList.contains("system")) return "system"
  return "assistant"
}

function previewOf(msg: HTMLElement): { name: string; text: string } {
  const role = roleOf(msg)
  // 用户消息的预览不显示称谓（内容本身即"我"发的，省略角色标签）
  const name = role === "user" ? "" : (ROLE_NAME[role] ?? role)
  // 跳过压缩通知（无 bubble），仅展示时间
  if (role === "system") {
    const summary = msg.querySelector(".compact-summary")?.textContent?.trim()
    return { name, text: summary || "（系统消息）" }
  }
  const bubble = msg.querySelector(".bubble")
  const raw = (bubble?.textContent ?? "").trim()
  return { name, text: raw || "（无内容）" }
}

export function addMsgNavSeg(msg: HTMLElement) {
  const role = roleOf(msg)
  // 只为用户消息建导航：assistant/tool/系统消息不画条
  if (role !== "user") return
  const seg: Seg = { bar: document.createElement("div"), msg, role, ...previewOf(msg) }
  seg.bar.className = `msg-nav-seg ${role}`
  seg.bar.dataset.role = role
  seg.bar.tabIndex = 0
  msgNav.appendChild(seg.bar)
  segs.push(seg)
  updateMsgNav()
}

export function clearMsgNav() {
  segs = []
  activeIdx = -1
  msgNav.innerHTML = ""
  msgNav.hidden = true
  hideTooltip()
}

/** 重新计算所有短横线的位置（消息增删 / 容器尺寸变化后调用）。 */
export function updateMsgNav() {
  if (rafPending) return
  rafPending = true
  requestAnimationFrame(() => {
    rafPending = false
    runUpdate()
  })
}

function runUpdate() {
  if (!segs.length || !msgEl.scrollHeight) {
    msgNav.hidden = true
    updateActiveSeg()
    return
  }
  const total = msgEl.scrollHeight
  // 滚动区高度 < 视口：没有可滚动空间，导航无意义
  const scrollable = total - msgEl.clientHeight > 1
  msgNav.hidden = !scrollable
  if (msgNav.hidden) return
  const navH = msgNav.clientHeight - NAV_PAD * 2
  if (navH <= 0) return

  // 固定垂直间距紧凑排列：短横线按固定步长聚在一起，整体在导航列垂直居中；
  // 仅当条数过多超出可用高度时压缩间距，避免溢出
  const n = segs.length
  const gap = Math.max(2, Math.min(SEG_GAP, (navH - n * SEG_H) / Math.max(1, n - 1)))
  const step = SEG_H + gap
  const blockH = n * step - gap
  const startTop = NAV_PAD + (navH - blockH) / 2
  for (let i = 0; i < n; i++) {
    segs[i].bar.style.top = `${Math.round(startTop + i * step)}px`
  }
  updateActiveSeg() // 消息增删 / 初次定位时同步当前位置高亮
}

/* ---------- 当前位置高亮（滚动跟随） ---------- */

function setActive(idx: number) {
  if (idx === activeIdx) return
  if (activeIdx >= 0) segs[activeIdx]?.bar.classList.remove("active")
  activeIdx = idx
  if (idx >= 0) segs[idx]?.bar.classList.add("active")
}

/** 找到视口中心所在的（用户）消息，高亮其短横线。
 * 区域划分：每条用户消息的管辖区间 = [该输入顶部, 下一条输入顶部)，
 * 输入之后的整段输出都归属该输入（不按输出中点平分）。
 * 消息顶部沿文档顺序单调 → 从上次位置向两侧线性探测即可，避免全量布局。 */
function updateActiveSeg() {
  if (!segs.length) {
    setActive(-1)
    return
  }
  const rect = msgEl.getBoundingClientRect()
  const center = rect.top + rect.height / 2
  const topOf = (i: number) => segs[i].msg.getBoundingClientRect().top
  let best = activeIdx >= 0 && activeIdx < segs.length ? activeIdx : 0
  // 向下：下一输入顶部仍在视口中心上方 → 归属其前的输入
  while (best < segs.length - 1 && topOf(best + 1) <= center) best++
  // 向上：当前输入顶部已越过视口中心 → 回退到上一个输入
  while (best > 0 && topOf(best) > center) best--
  setActive(best)
}

let activeRaf = false
function scheduleActiveUpdate() {
  if (activeRaf) return
  activeRaf = true
  requestAnimationFrame(() => {
    activeRaf = false
    updateActiveSeg()
  })
}

function showTooltip(seg: Seg) {
  if (!tooltip) tooltip = makeTooltip()
  tooltip.innerHTML = ""
  // 用户消息无称谓标签（name 为空），其余角色显示角色名
  if (seg.name) {
    tooltip.append(Object.assign(document.createElement("div"), { className: "mn-role", textContent: seg.name }))
  }
  tooltip.append(Object.assign(document.createElement("div"), { className: "mn-text", textContent: seg.text }))
  tooltip.style.setProperty("-webkit-line-clamp", String(TOOLTIP_MAX))
  tooltip.hidden = false
  // 离屏预布局：先放到视口外并强制布局，确保首次测量拿到真实尺寸。
  // 长内容（含换行/-webkit-box 截断）在刚显示的同一帧测量可能得到未就绪宽度，
  // 导致卡片以 0 宽定位、右缘溢出视口（"贴右边界"），随后才被 pointermove 校正。
  tooltip.style.left = "-9999px"
  void tooltip.offsetHeight
  positionTooltip(seg.bar)
}

function positionTooltip(anchor: HTMLElement) {
  if (!tooltip || tooltip.hidden) return
  // 同步测量定位：getBoundingClientRect 强制布局，内容刚填充/刚显示时尺寸即准确；
  // 不用 rAF 延迟——否则切换短横线时卡片会先停留在旧位置（已换新内容）再瞬移到新位置，
  // 长卡片尺寸差大时产生明显"滑动"感
  const a = anchor.getBoundingClientRect()
  const r = tooltip.getBoundingClientRect()
  const x = Math.max(8, a.left - r.width - 12)
  const y = Math.max(8, Math.min(a.top + a.height / 2 - r.height / 2, window.innerHeight - r.height - 8))
  tooltip.style.left = `${x}px`
  tooltip.style.top = `${y}px`
}

function hideTooltip() {
  if (tooltip) tooltip.hidden = true
}

/** 跳转到第 idx 条消息（不破坏当前滚动监听）。 */
function jumpToIdx(idx: number) {
  const seg = segs[idx]
  if (!seg) return
  stopFollowing() // 用户导航：解除粘底跟随，平滑滚动过程不被跟随循环拽回底部
  const top = seg.msg.offsetTop - 12
  msgEl.scrollTo({ top, behavior: gesture.isDragging() ? "auto" : "smooth" })
}

/** 根据指针 Y 坐标找最近的短横线（拖动 / 直接点击导航条空白处时用）。 */
function idxFromClientY(clientY: number): number {
  const rect = msgNav.getBoundingClientRect()
  if (!segs.length) return -1
  const navH = rect.height
  const ratio = Math.min(1, Math.max(0, (clientY - rect.top) / navH))
  // 找到与比例最接近的短横线中点位置
  let best = 0
  let bestDist = Infinity
  for (let i = 0; i < segs.length; i++) {
    const c = parseFloat(segs[i].bar.style.top || "0") + segs[i].bar.offsetHeight / 2
    const dist = Math.abs(c / navH - ratio)
    if (dist < bestDist) {
      bestDist = dist
      best = i
    }
  }
  return best
}

function onScroll() {
  // 条位置由消息位置决定、滚动不变，无需重排；仅跟随当前位置高亮
  scheduleActiveUpdate()
}

function bindBars() {
  // 事件委托：每条短横线挂相同处理函数
  msgNav.addEventListener("pointerover", (e) => {
    const bar = (e.target as HTMLElement).closest(".msg-nav-seg")
    if (bar && msgNav.contains(bar)) {
      const idx = segs.findIndex((s) => s.bar === bar)
      if (idx >= 0) showTooltip(segs[idx])
    }
  })
  msgNav.addEventListener("pointerout", (e) => {
    const bar = (e.target as HTMLElement).closest(".msg-nav-seg")
    if (bar && msgNav.contains(bar)) hideTooltip()
  })
  msgNav.addEventListener("pointermove", (e) => {
    if (gesture.isDragging()) return
    const bar = (e.target as HTMLElement).closest(".msg-nav-seg")
    if (bar && msgNav.contains(bar)) positionTooltip(bar as HTMLElement)
  })
  // 键盘：↑/↓ 切换 focus 条，Enter 跳到当前 focus 条
  msgNav.addEventListener("keydown", (e) => {
    if (!segs.length) return
    const current = segs.findIndex((s) => s.bar === document.activeElement)
    if (e.key === "ArrowDown" || e.key === "ArrowRight") {
      e.preventDefault()
      // 焦点不在任何条上（current=-1）时定位到第一条（0），而非 Math.max(0,current)+1 跳过第一条
      const next = Math.min(segs.length - 1, (current < 0 ? -1 : current) + 1)
      segs[next].bar.focus()
      showTooltip(segs[next])
    } else if (e.key === "ArrowUp" || e.key === "ArrowLeft") {
      e.preventDefault()
      const next = Math.max(0, Math.max(0, current) - 1)
      segs[next].bar.focus()
      showTooltip(segs[next])
    } else if (e.key === "Home") {
      e.preventDefault()
      segs[0].bar.focus()
      showTooltip(segs[0])
    } else if (e.key === "End") {
      e.preventDefault()
      segs[segs.length - 1].bar.focus()
      showTooltip(segs[segs.length - 1])
    } else if (e.key === "Enter" || e.key === " ") {
      e.preventDefault()
      if (current >= 0) jumpToIdx(current)
    } else if (e.key === "Escape") {
      hideTooltip()
    }
  })
}

/** 初始化导航：滚动监听 + 拖动 + ResizeObserver 重算。 */
export function bindMsgNav() {
  msgNav.addEventListener("pointerdown", onPointerDown)
  msgNav.addEventListener("pointermove", onPointerMove)
  msgNav.addEventListener("pointerup", onPointerUp)
  msgNav.addEventListener("pointercancel", onPointerCancel)
  msgNav.addEventListener("scroll", onScroll) // 自带滚动条时同步活动条
  msgEl.addEventListener("scroll", onScroll, { passive: true })
  // 短横线命中带（::before 扩展成 40×18 连续竖带）悬停时滚轮转发给消息列：
  // msg-nav 是 #messages 的兄弟元素、不在其滚动链上，默认滚轮在此无任何滚动目标
  // （「滚轮失灵」根因）；上滚同步解除粘底跟随——与 #messages 自身 wheel 监听同语义，
  // 否则流式静默窗口会把手动落位归因为内部动作而拽回底部。
  msgNav.addEventListener(
    "wheel",
    (e) => {
      const ev = e as WheelEvent
      let dy = ev.deltaY
      if (ev.deltaMode === 1) dy *= 16 // 行
      else if (ev.deltaMode === 2) dy *= msgEl.clientHeight // 页
      if (!dy) return
      if (dy < 0) stopFollowing()
      msgEl.scrollTop += dy
    },
    { passive: true },
  )
  // 容器尺寸 / 消息内容变化时重算位置
  if (typeof ResizeObserver !== "undefined") {
    const ro = new ResizeObserver(() => updateMsgNav())
    ro.observe(msgEl)
    ro.observe(msgNav)
  } else {
    window.addEventListener("resize", updateMsgNav)
  }
  // 消息增删 / 流式内容更新 → 重算
  const mo = new MutationObserver(() => updateMsgNav())
  mo.observe(msgEl, { childList: true, subtree: true, characterData: true })
  bindBars()
  updateMsgNav()
}
