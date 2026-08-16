import { el } from "./state"

/* 标题栏最右按钮轮盘：入口按钮（#wheel-btn）悬浮展开双弧扇形快捷菜单——
 * 内弧 r=85 会话操作组（小工具/导出/压缩），外弧 r=145 应用操作组（自动审批/极简模式/主题/设置/登出），
 * 两弧之间 r=115 细弧线分区；纯 hover 交互（入口 hover 展开、指针离开扇形区域延迟收起，不支持点击切换），
 * 外点/Esc/resize 关闭。8 个按钮自隐藏源容器移入（事件绑定在元素上，移动不失效）。 */

const INNER_R = 85 // 内弧半径（会话组）
const OUTER_R = 145 // 外弧半径（应用组）
const DIVIDER_R = 115 // 分区弧线半径
// 分组角度（屏幕角：0°=正右，90°=正下；扇形受标题栏高度限制只能朝左下方展开）
const INNER_ANGS = [93, 120, 147] // 会话组固定 3 个
const OUTER_RANGE: [number, number] = [97, 153] // 应用组按可见按钮数在区间内均布
const KEEP_PAD = 8 // hover 保持区相对扇形边界盒的外扩（指针在区域内不收起）
const BTN = 32 // 按钮边长（.icon-btn）
const SVG_SIZE = 300
const OPEN_DELAY = 120 // hover 展开延迟（防扫过即弹）
const CLOSE_DELAY = 250 // 收起延迟（按钮间移动空隙不误收）
const ANIM_MS = 140 // 扇形弹出动画时长
const STAGGER_MS = 14 // 按钮错落弹出间隔

/** 按角度区间均布 n 个角度。 */
function angs(n: number, range: [number, number]): number[] {
  if (n <= 1) return [range[0]]
  return Array.from({ length: n }, (_, i) => range[0] + ((range[1] - range[0]) * i) / (n - 1))
}

/** 半径 + 屏幕角 → [dx, dy] 偏移。 */
function polar(r: number, deg: number): [number, number] {
  const rad = (deg * Math.PI) / 180
  return [r * Math.cos(rad), r * Math.sin(rad)]
}

export function bindWheel() {
  const wheelBtn = document.getElementById("wheel-btn") as HTMLButtonElement
  const inner = ["mini-tools-btn", "export-btn", "compact-btn"].map((id) => document.getElementById(id) as HTMLButtonElement)
  const outer = ["approval-skip", "minimal-mode", "theme-btn", "settings-btn", "logout-btn"].map((id) => document.getElementById(id) as HTMLButtonElement)
  const all = [...inner, ...outer]

  // 容器 = hover 保持区 + 分区弧线（挂 body，fixed，不随任何 transform 祖先偏移）
  const keep = el("div", "wheel")
  document.body.appendChild(keep)
  // 分区弧线：内外弧之间的细弧段（SVG path，圆心在 SVG 中心）
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg")
  svg.setAttribute("viewBox", `0 0 ${SVG_SIZE} ${SVG_SIZE}`)
  const C = SVG_SIZE / 2
  const pt = (deg: number): [number, number] => {
    const rad = (deg * Math.PI) / 180
    return [C + DIVIDER_R * Math.cos(rad), C + DIVIDER_R * Math.sin(rad)]
  }
  const [x0, y0] = pt(88)
  const [x1, y1] = pt(158)
  const path = document.createElementNS("http://www.w3.org/2000/svg", "path")
  path.setAttribute("d", `M ${x0.toFixed(1)} ${y0.toFixed(1)} A ${DIVIDER_R} ${DIVIDER_R} 0 0 1 ${x1.toFixed(1)} ${y1.toFixed(1)}`)
  svg.appendChild(path)
  keep.appendChild(svg)

  // 8 个按钮移入容器（绑定保留在元素上），内弧按钮加分区底色
  for (const b of all) keep.appendChild(b)
  for (const b of inner) b.classList.add("wheel-inner")

  // 初始收起态（无过渡，先落位再启用动画）
  for (const b of all) {
    b.style.transition = "none"
    b.style.transform = "translate(0, 0) scale(0.4)"
    b.style.opacity = "0"
  }
  void keep.offsetWidth
  for (const b of all) b.style.transition = ""

  let expanded = false
  let openTimer: number | null = null
  let closeTimer: number | null = null
  let hideTimer: number | null = null

  function layout() {
    const r = wheelBtn.getBoundingClientRect()
    const cx = r.left + r.width / 2
    const cy = r.top + r.height / 2
    svg.style.left = `${cx - C}px`
    svg.style.top = `${cy - C}px`
    for (const b of all) {
      b.style.left = `${cx - BTN / 2}px`
      b.style.top = `${cy - BTN / 2}px`
    }
    // 弧位计算 + 保持区收紧为扇形边界盒（入口按钮 ∪ 各可见扇形按钮，外扩 KEEP_PAD）：
    // 指针只要在该区域内就不收起，离开边界盒即开始收起计时
    let minX = r.left, minY = r.top, maxX = r.right, maxY = r.bottom
    const place = (b: HTMLButtonElement, deg: number, rad: number) => {
      const [dx, dy] = polar(rad, deg)
      b.dataset.wheel = `translate(${dx}px, ${dy}px)`
      minX = Math.min(minX, cx - BTN / 2 + dx)
      minY = Math.min(minY, cy - BTN / 2 + dy)
      maxX = Math.max(maxX, cx + BTN / 2 + dx)
      maxY = Math.max(maxY, cy + BTN / 2 + dy)
    }
    const visOuter = outer.filter((b) => !b.hidden)
    const outerAngs = angs(visOuter.length, OUTER_RANGE)
    inner.forEach((b, i) => place(b, INNER_ANGS[i], INNER_R))
    visOuter.forEach((b, i) => place(b, outerAngs[i], OUTER_R))
    for (const b of outer.filter((b) => b.hidden)) b.dataset.wheel = "translate(0px, 0px) scale(0.4)"
    keep.style.left = `${minX - KEEP_PAD}px`
    keep.style.top = `${minY - KEEP_PAD}px`
    keep.style.width = `${maxX - minX + KEEP_PAD * 2}px`
    keep.style.height = `${maxY - minY + KEEP_PAD * 2}px`
  }

  function open() {
    if (expanded) return
    expanded = true
    if (closeTimer) {
      clearTimeout(closeTimer)
      closeTimer = null
    }
    if (hideTimer) {
      clearTimeout(hideTimer)
      hideTimer = null
    }
    layout()
    keep.classList.add("open")
    wheelBtn.classList.add("active")
    wheelBtn.setAttribute("aria-expanded", "true")
    let i = 0
    for (const b of all) {
      if (b.hidden) continue
      b.style.transitionDelay = `${i++ * STAGGER_MS}ms`
      b.style.transform = b.dataset.wheel ?? ""
      b.style.opacity = "1"
    }
  }

  function close() {
    if (!expanded) return
    expanded = false
    if (openTimer) clearTimeout(openTimer)
    for (const b of all) {
      b.style.transitionDelay = "0ms"
      b.style.transform = "translate(0, 0) scale(0.4)"
      b.style.opacity = "0"
    }
    wheelBtn.classList.remove("active")
    wheelBtn.setAttribute("aria-expanded", "false")
    hideTimer = window.setTimeout(() => keep.classList.remove("open"), ANIM_MS + 20)
  }

  function scheduleOpen() {
    if (closeTimer) {
      clearTimeout(closeTimer)
      closeTimer = null
    }
    if (expanded) return
    if (openTimer) return
    openTimer = window.setTimeout(() => {
      openTimer = null
      open()
    }, OPEN_DELAY)
  }

  function scheduleClose() {
    if (openTimer) clearTimeout(openTimer)
    openTimer = null
    if (!expanded) return
    if (closeTimer) return
    closeTimer = window.setTimeout(() => {
      closeTimer = null
      close()
    }, CLOSE_DELAY)
  }

  // hover 保持区 = 扇形边界盒容器（指针在区域内不收起）：
  // 入口 hover 展开，移入保持区取消收起计时，离开保持区 250ms 后收起
  wheelBtn.addEventListener("pointerenter", scheduleOpen)
  wheelBtn.addEventListener("pointerleave", scheduleClose)
  keep.addEventListener("pointerenter", () => {
    if (closeTimer) {
      clearTimeout(closeTimer)
      closeTimer = null
    }
  })
  keep.addEventListener("pointerleave", scheduleClose)
  document.addEventListener("pointerdown", (e) => {
    if (expanded && !keep.contains(e.target as Node) && !wheelBtn.contains(e.target as Node)) close()
  })
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") close()
  })
  window.addEventListener("resize", close)
}
