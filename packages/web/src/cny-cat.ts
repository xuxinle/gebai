/**
 * 人民币主题招财猫（cny 专属）：悬浮可拖动玩偶 + 爆金币交互（粒子由 coin-fx.ts 渲染）。
 * - 拖动：沿指针撒金币 + 纸币混排轨迹；点击（位移未超阈值）：爆少量金币，900ms 内连续点击连击递增、越爆越多。
 * - 单轮任务完成时由 main.ts 调 `cnyCatTurnEnd(elapsedMs)`：从猫位置大量爆发，运行越久越多（封顶 230）。
 * - 仅 `data-theme=cny` 时挂载（监听 gebai:theme-change 同步增删）；核心定位样式内联，装饰动效在 cny.css。
 * - 位置持久化 localStorage `gebai.ui.cnyCat`（左上角 px，视口变化钳制）；低性能时猫保留但粒子不发射。
 */
import { burstMoney, popTrailMoney } from "./coin-fx"

const CAT_WIDTH = 86
const CAT_HEIGHT = 95
const POS_KEY = "gebai.ui.cnyCat"
/** 点击/拖动判定阈值（px 累计位移）：超过视为拖动，未超过视为点击。 */
const MOVE_THRESHOLD = 6
/** 拖动轨迹撒币间隔（ms）：每拍一枚小金币 + 半数概率一张小纸币。 */
const TRAIL_INTERVAL = 30
/** 连击窗口（ms）：窗口内再次点击连击 +1，越爆越多。 */
const COMBO_WINDOW = 900

/** 招财猫 SVG（挥手手臂独立分组，cny.css 中对其做摆动动画）。 */
const CAT_SVG = `<svg viewBox="0 0 120 132" role="img" aria-label="招财猫" style="display:block;width:100%">
<defs>
<linearGradient id="cnyCatBody" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="#fffdf6"/><stop offset="1" stop-color="#f1e4cd"/>
</linearGradient>
<radialGradient id="cnyCatGold" cx="0.35" cy="0.3" r="0.95">
<stop offset="0" stop-color="#ffe9a3"/><stop offset="0.55" stop-color="#f2c94c"/><stop offset="1" stop-color="#d9a441"/>
</radialGradient>
</defs>
<ellipse cx="60" cy="124" rx="35" ry="6" fill="rgba(60,10,10,0.28)"/>
<path d="M94 110 q17 -3 15 -19 q-1.5 -9 -10 -10 q-5.5 -0.5 -5.5 4.5 q0 4.5 4.5 5 q4.5 0.5 3.5 5.5 q-1 5.5 -9 5 z" fill="#fffdf6" stroke="#e5d5bd" stroke-width="1.4" stroke-linejoin="round"/>
<path d="M60 52 C36 52 26 76 25 96 C24 114 38 124 60 124 C82 124 96 114 95 96 C94 76 84 52 60 52 Z" fill="url(#cnyCatBody)" stroke="#e5d5bd" stroke-width="1.5"/>
<path d="M34 64 Q60 78 86 64 L86 71 Q60 85 34 71 Z" fill="#d92d3a"/>
<circle cx="60" cy="77" r="6" fill="url(#cnyCatGold)" stroke="#b8831e" stroke-width="1"/>
<path d="M56.5 77 h7 M60 77 v3.6" stroke="#8a5c10" stroke-width="1.4" stroke-linecap="round" fill="none"/>
<circle cx="60" cy="93" r="14" fill="url(#cnyCatGold)" stroke="#b8831e" stroke-width="1.5"/>
<circle cx="60" cy="93" r="10.5" fill="none" stroke="rgba(150,98,15,0.6)" stroke-width="1"/>
<text x="60" y="97.6" text-anchor="middle" font-size="12" font-weight="700" fill="#92600c" font-family="'Segoe UI', system-ui, sans-serif">¥</text>
<g class="cny-cat-arm">
<path d="M80 68 Q85 52 93 39 Q98 31 105 36 Q112 42 105 49 Q96 59 90 71 Z" fill="#fffdf6" stroke="#e5d5bd" stroke-width="1.5" stroke-linejoin="round"/>
<circle cx="103" cy="40" r="8" fill="#fffdf6" stroke="#e5d5bd" stroke-width="1.5"/>
<path d="M98.5 37 v4 M103 36 v4.6 M107.5 37.5 v3.4" stroke="#e5d5bd" stroke-width="1.3" stroke-linecap="round"/>
</g>
<path d="M38 72 Q28 82 31 92 Q33 98 40 96 Q47 94 45 87 Q43 79 42 73 Z" fill="#fffdf6" stroke="#e5d5bd" stroke-width="1.5" stroke-linejoin="round"/>
<path d="M33 28 Q27 7 33 8 Q44 10 50 17 Z" fill="#fffdf6" stroke="#e5d5bd" stroke-width="1.5" stroke-linejoin="round"/>
<path d="M35 24 Q31.5 12 34 12.5 Q41 14 45 18 Z" fill="#f3b8c0"/>
<path d="M87 28 Q93 7 87 8 Q76 10 70 17 Z" fill="#e8963a" stroke="#d97b29" stroke-width="1.5" stroke-linejoin="round"/>
<path d="M85 24 Q88.5 12 86 12.5 Q79 14 75 18 Z" fill="#f3b8c0"/>
<ellipse cx="60" cy="40" rx="30" ry="26" fill="url(#cnyCatBody)" stroke="#e5d5bd" stroke-width="1.5"/>
<path d="M60 14 Q78 13 87 28 Q80 24 71 25 Q64 19 60 14 Z" fill="#e8963a" opacity="0.92"/>
<circle cx="48" cy="38" r="4.6" fill="#3a2b1e"/><circle cx="49.6" cy="36.4" r="1.5" fill="#fff"/>
<circle cx="72" cy="38" r="4.6" fill="#3a2b1e"/><circle cx="73.6" cy="36.4" r="1.5" fill="#fff"/>
<path d="M57.6 44.6 h4.8 l-2.4 3.2 z" fill="#e88a97"/>
<path d="M55.4 49.6 q2.4 2.8 4.6 0.6 q2.2 2.2 4.6 -0.6" stroke="#7a5a44" stroke-width="1.4" fill="none" stroke-linecap="round"/>
<ellipse cx="39.5" cy="47" rx="4.4" ry="2.5" fill="#f3b8c0" opacity="0.75"/>
<ellipse cx="80.5" cy="47" rx="4.4" ry="2.5" fill="#f3b8c0" opacity="0.75"/>
<path d="M22 36 h11 M21 42 q6 1.2 12 0 M23 48 q6 2 11 -0.8 M98 36 h11 M99 42 q6 1.2 12 0 M97 48 q6 2 11 -0.8" stroke="#d9c6a8" stroke-width="1.4" stroke-linecap="round" fill="none"/>
</svg>`

let catEl: HTMLElement | null = null
let pos = { x: 0, y: 0 }
let resizeHandler: (() => void) | null = null
let inited = false

/** 连击状态。 */
let combo = 0
let comboAt = 0

function clampPos(p: { x: number; y: number }): { x: number; y: number } {
  return {
    x: Math.max(8, Math.min(window.innerWidth - CAT_WIDTH - 8, p.x)),
    y: Math.max(64, Math.min(window.innerHeight - CAT_HEIGHT - 12, p.y)),
  }
}

function loadPos(): { x: number; y: number } {
  try {
    const raw = localStorage.getItem(POS_KEY)
    if (raw) {
      const p = JSON.parse(raw) as { x?: unknown; y?: unknown }
      if (typeof p.x === "number" && typeof p.y === "number" && Number.isFinite(p.x) && Number.isFinite(p.y)) {
        return clampPos({ x: p.x, y: p.y })
      }
    }
  } catch {
    /* ignore */
  }
  // 默认：视口右侧偏中上
  return clampPos({ x: window.innerWidth - CAT_WIDTH - 28, y: window.innerHeight * 0.36 })
}

function savePos(): void {
  try {
    localStorage.setItem(POS_KEY, JSON.stringify(pos))
  } catch {
    /* ignore */
  }
}

function applyPos(): void {
  if (!catEl) return
  catEl.style.left = `${pos.x}px`
  catEl.style.top = `${pos.y}px`
}

/** 猫的爆发原点（身体中心偏上，金币从怀里爆出）。 */
function catCenter(): { x: number; y: number } {
  if (!catEl) return { x: window.innerWidth / 2, y: window.innerHeight / 2 }
  const r = catEl.getBoundingClientRect()
  return { x: r.left + r.width / 2, y: r.top + r.height * 0.55 }
}

/** 弹跳反馈（重启动画：移除类 → 强制重排 → 加回）。 */
function bounce(): void {
  if (!catEl) return
  catEl.classList.remove("bounce")
  void catEl.offsetWidth
  catEl.classList.add("bounce")
}

/** 点击爆发：连击窗口内递增，越点越多。 */
function clickBurst(): void {
  const now = performance.now()
  combo = now - comboAt < COMBO_WINDOW ? Math.min(combo + 1, 12) : 1
  comboAt = now
  const c = catCenter()
  burstMoney(c.x, c.y, Math.min(10 + (combo - 1) * 7, 58), { speed: 0.9 + Math.min(combo, 6) * 0.07 })
  bounce()
}

function mount(): void {
  catEl = document.createElement("div")
  catEl.id = "gb-cny-cat"
  catEl.innerHTML = CAT_SVG
  // 核心定位样式内联（主题 CSS 卸载时序不影响）；装饰动效（挥手/悬浮/弹跳）在 cny.css
  catEl.style.cssText = `position:fixed;width:${CAT_WIDTH}px;z-index:440;cursor:grab;touch-action:none;user-select:none;-webkit-user-select:none;`
  pos = loadPos()
  applyPos()
  document.body.appendChild(catEl)

  let pressed = false
  let dragging = false
  let movedDist = 0
  let lastX = 0
  let lastY = 0
  let lastTrail = 0
  let activeId = -1

  catEl.addEventListener("pointerdown", (e) => {
    if (e.button !== 0) return
    pressed = true
    dragging = false
    movedDist = 0
    lastX = e.clientX
    lastY = e.clientY
    activeId = e.pointerId
    catEl!.setPointerCapture(e.pointerId)
  })
  catEl.addEventListener("pointermove", (e) => {
    if (!pressed || e.pointerId !== activeId) return
    const dx = e.clientX - lastX
    const dy = e.clientY - lastY
    lastX = e.clientX
    lastY = e.clientY
    movedDist += Math.abs(dx) + Math.abs(dy)
    if (!dragging && movedDist > MOVE_THRESHOLD) {
      dragging = true
      catEl!.classList.add("dragging")
    }
    if (!dragging) return
    pos = clampPos({ x: pos.x + dx, y: pos.y + dy })
    applyPos()
    const now = performance.now()
    if (now - lastTrail > TRAIL_INTERVAL) {
      lastTrail = now
      popTrailMoney(e.clientX, e.clientY)
    }
  })
  const release = (e: PointerEvent) => {
    if (!pressed || e.pointerId !== activeId) return
    pressed = false
    activeId = -1
    catEl!.classList.remove("dragging")
    if (dragging) savePos()
    else clickBurst()
    dragging = false
  }
  catEl.addEventListener("pointerup", release)
  catEl.addEventListener("pointercancel", release)
  // 捕获被夺走/拖拽被系统取消时不派发 pointerup：lostpointercapture 兜底收尾（防位置不持久化、按压态悬挂）
  catEl.addEventListener("lostpointercapture", release)

  resizeHandler = () => {
    pos = clampPos(pos)
    applyPos()
  }
  window.addEventListener("resize", resizeHandler, { passive: true })
}

function unmount(): void {
  if (resizeHandler) {
    window.removeEventListener("resize", resizeHandler)
    resizeHandler = null
  }
  catEl?.remove()
  catEl = null
}

function syncMount(): void {
  const active = document.documentElement.dataset.theme === "cny"
  if (active && !catEl) mount()
  else if (!active && catEl) unmount()
}

/** 单轮任务完成庆祝：从猫位置大爆发，运行越久爆得越多（36 起、封顶 230，分 2-4 波）。 */
export function cnyCatTurnEnd(elapsedMs: number): void {
  if (!catEl?.isConnected) return
  const sec = Math.max(0, elapsedMs) / 1000
  const count = Math.round(Math.min(230, 36 + sec * 1.4))
  const c = catCenter()
  burstMoney(c.x, c.y, count, { speed: 1.15, waves: Math.max(2, Math.min(4, Math.round(count / 70))) })
  bounce()
}

/** 初始化（main.ts 一次调用）：随主题切换挂载/卸载。 */
export function initCnyCat(): void {
  if (inited) return
  inited = true
  document.addEventListener("gebai:theme-change", syncMount)
  syncMount()
}
