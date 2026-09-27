/**
 * 通用「按钮轮盘」原语：hover 入口按钮 → 双弧扇形展开一组按钮。
 *
 * 为什么抽出来：标题栏最右（`wheel.ts`）与文件工作台编辑器右上角（`files/main.ts`）用的是**同一套交互**——
 * 常驻位只留高频动作，其余收进轮盘。复制两遍意味着两套坐标/保持区/收起时序，改一处忘一处
 * （典型症状：一个入口 hover 就能弹、另一个点了才弹；一个外点收起、另一个不收起）。
 * 这里只放**几何与交互**，按钮长什么样、有哪些，全部由调用方给（`items` 里是已经绑好事件的元素）。
 *
 * 布局：**按空间自动分圈**——第一圈半径 = `firstRingR`，之后每圈 = 上一圈 + 按钮边长 + 间隙（步进不再写死）；
 * 每圈**容量**由「该圈半径 × 可用张角」算（相邻按钮不叠，见 `capacityAt`），当前圈放不下的项**自动落到下一圈**；
 * 分组边界（`inner` → 其它）**必换圈**，语义分组不会被拆散在同一圈里。
 * 可用空间以入口为圆心向左/下取（视口边界 + `margin`）：半径再往外就超空间的那些圈并进最后一圈
 * （宁可挤一点也不出屏 / 不压入口那一行）。
 * 屏幕角 0°=正右、90°=正下，扇形朝**下（左）方**展开（入口都在界面上缘，只有向下有空间）。
 * 角度**从起始角向左侧长**（起始角固定，对称外扩会把首个按钮顶出屏幕右缘），
 * 张角取**刚好放下**（满足相邻方块「边长 + 间隙」的最小跨度，见 `fitArc`）——
 * 一圈只有两三项时按钮就挨在一起，不会空出一大截弧；真放不下时才停在上限（宁可挤一点）。
 * 每圈画一条**引导弧线**（半径 = 该圈半径，穿过按钮圆心、画在按钮之下——看得见的是按钮之间那几段短弧），
 * “一圈圈”的结构据此自己显出来。
 *
 * 保持区 = 入口按钮 ∪ 各可见扇形按钮的**边界盒**（外扩 KEEP_PAD）：指针在盒内不收起。
 * 用边界盒而不是精确扇形，是为了容忍指针在两个按钮之间抄近路穿过空隙——精确扇形会在
 * 空隙里判定"离开"，手一抖菜单就收了。
 *
 * **保持区不吃指针事件**：容器是一块覆盖整盒的实心矩形，而盒下面往往正是标签栏/消息区里的真实控件
 * （入口在界面右上角，扇形向下左展开，盒子自然压住它们）。容器若可命中，展开期间那些控件就都点不到
 * 了——点击落在容器上，既不触发下方按钮、也不算“点了外面”（典型症状：hover 轮盘入口后，旁边那颗
 * 常驻按钮就点不动了）。所以容器一律 `pointer-events: none`（见 css/wheel.css），只有扇形按钮
 * 自己 `pointer-events: auto`；“指针还在保持区内”改由 document 上的 pointermove 用坐标比对判定
 * （展开期间才挂，收起态不跑）。
 *
 * 交互：鼠标（细指针）入口 hover 展开（OPEN_DELAY 默认 0）、指针离开保持区 CLOSE_DELAY 后收起；
 * 触屏（粗指针）没有 hover，改为**点按入口开合**（同一个入口按钮再点一下收起），
 * 同时不挂 hover 的开/收时序（触屏上 pointerenter/leave 会紧跟同一根手指触发，
 * 会把刚展开的扇形立刻定时收起）。外点 / Esc / resize 立即收起，两种指针都一样；
 * 键盘可及性——入口按钮获焦后 Enter/空格/↓ 同样展开，之后 Tab 进扇形按钮。
 * 点击扇形里的按钮后自动收起（点击事件在容器上冒泡到，与容器是否可命中无关）。
 *
 * 用法：
 * ```ts
 * const w = createWheel({ trigger: btn, items: [{ el: a, group: "inner" }, { el: b }] })
 * // 容器与监听由 destroy() 一并清理（重渲染前务必调用，否则每次重建都多留一份 DOM 与监听）
 * w.destroy()
 * ```
 * 样式见 `css/wheel.css`（容器类默认 `wheel`，扇形按钮统一被加上 `wheel-item` / `wheel-inner`）。
 */
import { el, isCoarsePointer } from "./state"
import { popKeyScope, pushEscScope } from "./keymap"

export interface WheelItem {
  /** 扇形按钮本体（调用方建好、事件已绑） */
  el: HTMLElement
  /** 内弧（主组）还是外弧（次组），缺省外弧 */
  group?: "inner" | "outer"
}

export interface WheelOptions {
  /** 入口按钮（hover 展开，位置即扇形圆心） */
  trigger: HTMLElement
  items: WheelItem[]
  /** 容器类名（默认 `wheel`） */
  containerClass?: string
  /**
   * 第一圈半径（px）。之后每圈 = 上一圈 + 按钮边长 + 间隙，**按尺寸自动步进**——
   * 调用方只管首圈半径，圈数与各圈半径由项数与可用空间算出来（见文件头「分圈」）。
   */
  firstRingR?: number
  /** 扇形按钮边长（量不到实际尺寸时的兜底；也是分圈步进与可用空间估算的基准） */
  buttonSize?: number
  /** 同圈相邻按钮的期望间隙（px）：圈内排布尽量拉开到「边长 + 间隙」 */
  buttonGap?: number
  /** 单圈最大张角（度）：超过则不再拉大角度（宁可挤一点，也不把弧撑到不该去的方向） */
  maxSpan?: number
  /** 扇形起始角（屏幕角，度；0=正右、90=正下）：扇形**从起始角向左侧长** */
  startAngle?: number
  /** 可用空间相对视口的内边距（px）：按钮不贴边、不出屏 */
  margin?: number
  /** 每圈是否画一条引导弧线（穿过该圈按钮圆心的细弧，画在按钮之下） */
  arcs?: boolean
  /** hover 到展开的延迟（0 = 立即） */
  openDelay?: number
  /** 指针离开保持区后的收起延迟 */
  closeDelay?: number
}

export interface WheelHandle {
  open(): void
  close(): void
  isOpen(): boolean
  /** 摘下容器与全部监听（重渲染前调用；之后 handle 不可再用） */
  destroy(): void
}

/** 引导弧线画布的最小边长（弧线用 SVG 画，圆心在画布中心；实际按最大圈半径放大）。 */
const ARC_SVG_MIN = 300
/** 保持区相对边界盒的外扩（px）。 */
const KEEP_PAD = 8
/** 同圈相邻按钮之间的**最小**视觉间隙（px）：容量判定用「边长 + 它」，比这个还小就算挤。 */
const MIN_BUTTON_GAP = 2
/** 引导弧线默认超出本圈首/末按钮的角度（度）。 */
const ARC_PAD = 6
/** 圈数上限（防御：项数极端时也不至于把扇形扩成一大片）。 */
const MAX_RINGS = 8
/** 扇形弹出动画时长（ms；与 css/wheel.css 里的 transition 对齐）。 */
const ANIM_MS = 140
/** 按钮错落弹出的间隔（ms）。 */
const STAGGER_MS = 14

/** 一圈的布局结果：半径、该圈项与各项角度（角度供引导弧线取张角）。 */
interface RingPlan {
  r: number
  list: WheelItem[]
  angles: number[]
}

/** 单弧几何结果：有效半径与各项角度（度）。 */
interface ArcFit {
  r: number
  angles: number[]
}

/** 角度（度）转弧度。 */
const rad = (deg: number): number => (deg * Math.PI) / 180

/** 在起始角 start 起、张角 span 上均布 count 项的角度。 */
function spreadAngles(start: number, span: number, count: number): number[] {
  if (count <= 1) return [start]
  return Array.from({ length: count }, (_, i) => start + (span * i) / (count - 1))
}

/**
 * 该角度序列在半径 r 上是否「两两不叠」。
 *
 * 判定用**方块的实际不重叠条件**（|Δx| ≥ 边长 或 |Δy| ≥ 边长），不是圆心距/弦长——
 * 斜向相邻（弦与坐标轴成 45° 附近）时弦长达标、两个正方形仍会压住几个像素，那正是「看着叠在一起」。
 */
function arcFits(r: number, angles: number[], minGap: number): boolean {
  for (let i = 1; i < angles.length; i++) {
    const dx = Math.abs(r * (Math.cos(rad(angles[i]!)) - Math.cos(rad(angles[i - 1]!))))
    const dy = Math.abs(r * (Math.sin(rad(angles[i]!)) - Math.sin(rad(angles[i - 1]!))))
    if (Math.max(dx, dy) + 1e-6 < minGap) return false
  }
  return true
}

/**
 * 算一弧的角度：让相邻按钮方块尽量拉开到「边长 + 间隙」。
 *
 * **半径固定不动**（上游给多少就是多少）：为了多塞按钮而把弧撑大，是在用“看着还是两圈吗”换“一排能放下”——
 * 弧位拥挤的正确解法是减项或改分组（内圈往外挪按钮），不是拿半径去让路。所以本函数只调**角度**。
 *
 * 张角取**刚好放下**（紧凑）：二分出满足「相邻方块间隙 ≥ size+gap」的最小跨度——不硬撑到某个固定值。
 * 这一条是实测逼出来的：工作台第二圈只有 2 项时，旧实现（下界 = 首选张角 54°）会把两个按钮推到
 * 93° 与 147°，中间空出 55px 的弧——看上去就不在一圈上。上限内真放不下时停在上限（宁可挤一点）。
 *
 * 张角**从起始角向左侧长**（起始角固定）——对称外扩会在入口靠窗口右缘时把首个按钮顶出屏幕。
 * 张角上限 = min(maxSpan, 终点角不超过 dyMin 对应的角)，后者保证最上方那个按钮仍落在锚点行**下方**。
 *
 * 纯函数（不读闭包状态）：几何是轮盘最容易被改坏的部分，参数化后能直接单测。
 */
function fitArc(o: {
  count: number
  size: number
  gap: number
  r: number
  start: number
  maxSpan: number
  /** 终点角处的最小纵向偏移（px）：按钮中心相对圆心的 dy 不得小于它（否则压住锚点行） */
  dyMin: number
}): ArcFit {
  const { count, size, gap, r, start, maxSpan, dyMin } = o
  if (count <= 0) return { r, angles: [] }
  // 只一项就不必“排开”：落在起始角（正下方偏左一点），与其它圈同一方向
  if (count === 1) return { r, angles: [start] }
  const endLimit = dyMin <= 0 || dyMin >= r ? 180 : 180 - (Math.asin(dyMin / r) * 180) / Math.PI
  const cap = Math.max(0, Math.min(maxSpan, endLimit - start))
  const need = size + gap
  // 在 [0, 上限] 里二分出「刚好拉开到间隙要求」的最小张角（紧凑：不要空出多余的弧）
  let lo = 0
  let hi = cap
  if (arcFits(r, spreadAngles(start, hi, count), need)) {
    for (let i = 0; i < 16; i++) {
      const mid = (lo + hi) / 2
      if (arcFits(r, spreadAngles(start, mid, count), need)) hi = mid
      else lo = mid
    }
  }
  return { r, angles: spreadAngles(start, hi, count) }
}

/** 半径 + 屏幕角 → [dx, dy] 偏移。 */
function polar(r: number, deg: number): [number, number] {
  const rad = (deg * Math.PI) / 180
  return [r * Math.cos(rad), r * Math.sin(rad)]
}

export function createWheel(opts: WheelOptions): WheelHandle {
  const trigger = opts.trigger
  const items = opts.items
  const firstRingR0 = opts.firstRingR ?? 85
  const gap = opts.buttonGap ?? 8
  const maxSpan = opts.maxSpan ?? 100
  const startAngle = opts.startAngle ?? 93
  const margin = opts.margin ?? 8
  const fallbackSize = opts.buttonSize ?? 32
  const openDelay = opts.openDelay ?? 0
  const closeDelay = opts.closeDelay ?? 250

  // 容器 = hover 保持区 + 各圈引导弧线（挂 body，fixed，不随任何 transform 祖先偏移）
  const keep = el("div", opts.containerClass ?? "wheel")
  document.body.appendChild(keep)
  /* 引导弧线（每圈一条；半径/张角在 layout 里按实际圈位重算）。
     它是容器的**第一个子级**、按钮跟在后面——两层都已 fixed 且无 z-index，同层靠 DOM 顺序决胜，
     于是弧线自然画在按钮之下（看得见的只是按钮之间那几段），不用额外抬 z-index。 */
  const arcSvg = opts.arcs === false ? null : document.createElementNS("http://www.w3.org/2000/svg", "svg")
  if (arcSvg) {
    arcSvg.setAttribute("class", "wheel-arc")
    keep.appendChild(arcSvg)
  }

  // 按钮移入容器（事件绑定在元素上，搬移不失效），并打上统一类名供 CSS 定尺寸
  for (const it of items) {
    it.el.classList.add("wheel-item")
    if (it.group === "inner") it.el.classList.add("wheel-inner")
    keep.appendChild(it.el)
  }

  // 初始收起态（无过渡，先落位再启用动画）
  for (const it of items) {
    it.el.style.transition = "none"
    it.el.style.transform = "translate(0, 0) scale(0.4)"
    it.el.style.opacity = "0"
  }
  void keep.offsetWidth
  for (const it of items) it.el.style.transition = ""

  let expanded = false
  let openTimer: number | null = null
  let closeTimer: number | null = null
  let hideTimer: number | null = null
  let destroyed = false

  /** 按钮实际尺寸（用 offsetWidth/Height 而非 rect：rect 含 transform，收起态的 scale 会缩掉）。 */
  const sizeOf = (node: HTMLElement): { w: number; h: number } => ({ w: node.offsetWidth || fallbackSize, h: node.offsetHeight || fallbackSize })

  /**
   * 按可用空间分圈排布（契约见文件头「分圈」）。
   *
   * 与旧版的关键区别：**半径不再写死**——旧版内/外弧半径由调用方给，项一多就只能在那一弧上撑角度
   * （半径固定不变），结果是按钮要么挤成一排、要么溢出。现在每圈半径 = 上一圈 + 边长 + 间隙，
   * 每圈能放几项由「该圈半径 × 可用张角」算，放不下的落到下一圈——即“按空间一圈圈地排”。
   */
  function layout(): void {
    const rect = trigger.getBoundingClientRect()
    const cx = rect.left + rect.width / 2
    const cy = rect.top + rect.height / 2
    for (const it of items) {
      const { w, h } = sizeOf(it.el)
      it.el.style.left = `${cx - w / 2}px`
      it.el.style.top = `${cy - h / 2}px`
    }
    const visible = items.filter((it) => !it.el.hidden)
    /** 扇形里最大的按钮边长（分圈步进、容量与可用角度均按最大者算，大小不一也不会叠）。 */
    const size = visible.reduce((m, it) => { const s = sizeOf(it.el); return Math.max(m, s.w, s.h) }, fallbackSize)
    const step = size + gap
    /**
     * 终点角处按钮中心的最小纵向偏移：按钮顶边不得超过入口按钮的底边（否则压到入口那一行）。
     * 半径越小这个约束越紧（@r=85 时大约只能用到 155°），所以**跨度上限得逐圈算**，不能一次算好。
     */
    const dyMin = (rect.bottom - cy) + size / 2 + 4
    /** 半径 r 那一圈的可用张角（受 maxSpan 与「不压入口行」两条约束）。 */
    const spanCap = (r: number): number => {
      const endLimit = dyMin <= 0 || dyMin >= r ? 180 : 180 - (Math.asin(dyMin / r) * 180) / Math.PI
      return Math.max(0, Math.min(maxSpan, endLimit - startAngle))
    }
    /**
     * 半径 r 那一圈的容量：相邻按钮不叠（且留 MIN_BUTTON_GAP）的前提下最多几项。
     * 角度取该圈的**可用上限**（张角越大越容易拉开，是容量最宽松的取法），
     * 实际排布再按首选张角收紧（见下面 fitArc）。
     */
    const capacityAt = (r: number, span: number): number => {
      let n = 0
      while (n < 32) {
        const next = n + 1
        if (!arcFits(r, spreadAngles(startAngle, span, next), size + MIN_BUTTON_GAP)) break
        n = next
      }
      return Math.max(1, n)
    }
    /**
     * 可用半径上限：以入口为圆心向左/下取空间。
     * |cos| 与 sin 都 ≤ 1，故两向距离的**较小值**就是安全上界（扇形朝左下展开，不会比它更远）。
     * 取不到视口高度（测试环境 / 非浏览器）时按“无约束”算，不然会把一切圈都误判成超空间。
     */
    const vh = typeof window === "undefined" ? 0 : window.innerHeight
    const roomY = Number.isFinite(vh) && vh > 0 ? vh - margin - size / 2 - cy : Infinity
    const roomX = cx - margin
    const maxR = Math.max(firstRingR0, Math.min(roomX, roomY))

    /* ---------- 第一步：把可见项分圈 ---------- */
    const rings: { r: number; list: WheelItem[] }[] = []
    let i = 0
    let r = firstRingR0
    while (i < visible.length && rings.length < MAX_RINGS) {
      const cap = capacityAt(r, spanCap(r))
      let take = Math.min(cap, visible.length - i)
      // 组边界必换圈：分组语义（会话操作 / 应用操作）不能混进同一圈
      const firstInner = visible[i]!.group === "inner"
      for (let k = i + 1; k < i + take; k++) {
        if ((visible[k]!.group === "inner") !== firstInner) {
          take = k - i
          break
        }
      }
      // 半径再往外就超出可用空间：剩余项全并进这一圈（宁可挤一点也不出屏 / 不压入口行）
      if (r + step > maxR) take = visible.length - i
      rings.push({ r, list: visible.slice(i, i + Math.max(1, take)) })
      i += Math.max(1, take)
      r += step
    }
    // 兜底：圈数撞上 MAX_RINGS 而还有剩余时，并进最后一圈（极端项数下的降级）
    if (i < visible.length && rings.length) rings[rings.length - 1]!.list = rings[rings.length - 1]!.list.concat(visible.slice(i))

    /* ---------- 第二步：逐圈定角（半径由分圈定死，不为塞按钮而变） ---------- */
    const plans: RingPlan[] = rings.map((ring) => {
      const fit = fitArc({
        count: ring.list.length,
        size,
        gap,
        r: ring.r,
        start: startAngle,
        maxSpan,
        dyMin,
      })
      return { r: fit.r, list: ring.list, angles: fit.angles }
    })

    /* ---------- 第三步：落位 + 保持区收紧为扇形边界盒（入口按钮 ∪ 各可见扇形按钮） ---------- */
    let minX = rect.left
    let minY = rect.top
    let maxX = rect.right
    let maxY = rect.bottom
    for (const plan of plans) {
      plan.list.forEach((it, k) => {
        const { w, h } = sizeOf(it.el)
        const [dx, dy] = polar(plan.r, plan.angles[k]!)
        it.el.dataset.wheel = `translate(${dx}px, ${dy}px)`
        minX = Math.min(minX, cx - w / 2 + dx)
        minY = Math.min(minY, cy - h / 2 + dy)
        maxX = Math.max(maxX, cx + w / 2 + dx)
        maxY = Math.max(maxY, cy + h / 2 + dy)
      })
    }
    for (const it of items) if (it.el.hidden) it.el.dataset.wheel = "translate(0px, 0px) scale(0.4)"

    /* ---------- 引导弧线：每圈一条（半径 = 该圈半径，穿过按钮圆心、画在按钮之下） ---------- */
    if (arcSvg) {
      for (const child of [...arcSvg.children]) child.remove()
      const maxRingR = plans.reduce((m, p) => Math.max(m, p.r), 0)
      arcSvg.style.display = plans.length ? "" : "none"
      if (plans.length) {
        const box = Math.max(ARC_SVG_MIN, Math.ceil((maxRingR + 24) * 2))
        const c = box / 2
        const pt = (deg: number, rad: number): [number, number] => {
          const t = (deg * Math.PI) / 180
          return [c + rad * Math.cos(t), c + rad * Math.sin(t)]
        }
        for (const plan of plans) {
          const lo = plan.angles[0]! - ARC_PAD
          const hi = plan.angles[plan.angles.length - 1]! + ARC_PAD
          const path = document.createElementNS("http://www.w3.org/2000/svg", "path")
          const [x0, y0] = pt(lo, plan.r)
          const [x1, y1] = pt(hi, plan.r)
          path.setAttribute("d", `M ${x0.toFixed(1)} ${y0.toFixed(1)} A ${plan.r} ${plan.r} 0 0 1 ${x1.toFixed(1)} ${y1.toFixed(1)}`)
          arcSvg.appendChild(path)
        }
        arcSvg.setAttribute("viewBox", `0 0 ${box} ${box}`)
        // 显式定尺寸（CSS 里的 300px 只是兜底）：SVG 根元素没有 width/height 时会按包含块缩放，半径就不是算出来的值了
        arcSvg.style.left = `${cx - box / 2}px`
        arcSvg.style.top = `${cy - box / 2}px`
        arcSvg.style.width = `${box}px`
        arcSvg.style.height = `${box}px`
      }
    }

    keep.style.left = `${minX - KEEP_PAD}px`
    keep.style.top = `${minY - KEEP_PAD}px`
    keep.style.width = `${maxX - minX + KEEP_PAD * 2}px`
    keep.style.height = `${maxY - minY + KEEP_PAD * 2}px`
  }

  function open(): void {
    if (expanded || destroyed) return
    expanded = true
    scopeId = pushEscScope("main.wheel", "收起动作轮盘", close)
    if (closeTimer) {
      clearTimeout(closeTimer)
      closeTimer = null
    }
    if (hideTimer) {
      clearTimeout(hideTimer)
      hideTimer = null
    }
    layout()
    document.addEventListener("pointermove", onDocPointerMove)
    // 捕获阶段：文档根的 pointerleave 不冒泡，靠捕获才收得到
    document.addEventListener("pointerleave", onDocPointerLeave, true)
    keep.classList.add("open")
    trigger.classList.add("active")
    trigger.setAttribute("aria-expanded", "true")
    let i = 0
    for (const it of items) {
      if (it.el.hidden) continue
      it.el.style.transitionDelay = `${i++ * STAGGER_MS}ms`
      it.el.style.transform = it.el.dataset.wheel ?? ""
      it.el.style.opacity = "1"
    }
  }

/** 展开期间入栈的 Esc 作用域 id（收起即出栈，见 open / close）。 */
  let scopeId: string | null = null

  function close(): void {
    if (!expanded) return
    expanded = false
    if (scopeId) {
      popKeyScope(scopeId)
      scopeId = null
    }
    document.removeEventListener("pointermove", onDocPointerMove)
    document.removeEventListener("pointerleave", onDocPointerLeave, true)
    if (openTimer) clearTimeout(openTimer)
    for (const it of items) {
      it.el.style.transitionDelay = "0ms"
      it.el.style.transform = "translate(0, 0) scale(0.4)"
      it.el.style.opacity = "0"
    }
    trigger.classList.remove("active")
    trigger.setAttribute("aria-expanded", "false")
    hideTimer = window.setTimeout(() => keep.classList.remove("open"), ANIM_MS + 20)
  }

  function scheduleOpen(): void {
    if (isCoarsePointer()) return // 触屏走点按开合，hover 时序只在细指针下生效
    if (closeTimer) {
      clearTimeout(closeTimer)
      closeTimer = null
    }
    if (expanded || openTimer || destroyed) return
    openTimer = window.setTimeout(() => {
      openTimer = null
      open()
    }, openDelay)
  }

  function scheduleClose(): void {
    if (isCoarsePointer()) return
    if (openTimer) clearTimeout(openTimer)
    openTimer = null
    if (!expanded || closeTimer) return
    closeTimer = window.setTimeout(() => {
      closeTimer = null
      close()
    }, closeDelay)
  }

  const onKeepEnter = (): void => {
    if (closeTimer) {
      clearTimeout(closeTimer)
      closeTimer = null
    }
  }
  /**
   * 指针是否还在保持区内。
   *
   * 容器 `pointer-events: none`（不能吞掉下方控件的点击，见文件头），所以容器自身的
   * pointerenter/pointerleave 永远不会触发——“指针还在扇形附近”只能靠坐标判定：
   * 展开期间在 document 上挂一个 pointermove，逐次拿指针坐标比对容器矩形。
   * 挂/摘跟着展开态走（收起态全程不跑回调）；矩形每次都现读：保持区尺寸随弧位在 layout 里重算，
   * 展开期间窗口尺寸变化会先 close，不存在“盒子变了而监听还指着旧坐标”的窗口。
   */
  const onDocPointerMove = (e: PointerEvent): void => {
    if (!expanded) return
    const b = keep.getBoundingClientRect()
    const inside = e.clientX >= b.left && e.clientX <= b.right && e.clientY >= b.top && e.clientY <= b.bottom
    if (inside) onKeepEnter()
    else scheduleClose()
  }
  /**
   * 指针移出**文档**（切到别的窗口、贴到系统界面）：之后不会再产生 pointermove，保持区判定
   * 就收不到“离开”信号了，得手动收起。只在事件目标就是文档根时响应——元素级的 pointerleave
   * 也会被这个捕获监听收到（如指针从入口滑到扇形按钮上），而那些恰恰是“指针还在页面里”。
   */
  const onDocPointerLeave = (e: PointerEvent): void => {
    if (expanded && e.target === document.documentElement) scheduleClose()
  }
  const onDocPointerDown = (e: PointerEvent): void => {
    if (expanded && !keep.contains(e.target as Node) && !trigger.contains(e.target as Node)) close()
  }
  /** 键盘入口：入口按钮获焦后 Enter/空格/↓ 展开（扇形不靠鼠标也能看到；之后 Tab 进扇形按钮）。 */
  const onTriggerKeyDown = (e: KeyboardEvent): void => {
    if (e.key !== "Enter" && e.key !== " " && e.key !== "ArrowDown") return
    e.preventDefault()
    open()
  }
  const onResize = (): void => close()
  const onContainerClick = (e: MouseEvent): void => {
    // 点扇形里的按钮就收起（动作已由按钮自己的处理器执行；收起在冒泡阶段做，
    // 保证按钮处理器先跑——有的处理器会顺手重建标签栏，把整个轮盘一起拆掉）
    if ((e.target as HTMLElement | null)?.closest("button")) close()
  }

  /** 触屏点按入口开合（细指针下 hover 已经展开，这里不参与）。 */
  const onTriggerClick = (): void => {
    if (!isCoarsePointer()) return
    if (expanded) close()
    else open()
  }
  trigger.addEventListener("pointerenter", scheduleOpen)
  trigger.addEventListener("pointerleave", scheduleClose)
  trigger.addEventListener("click", onTriggerClick)
  trigger.addEventListener("keydown", onTriggerKeyDown)
  keep.addEventListener("click", onContainerClick)
  document.addEventListener("pointerdown", onDocPointerDown)
  window.addEventListener("resize", onResize)
  /* 这里不监听入口按钮是否被重建（早期用 MutationObserver 做过）：
     入口重建的场景（工作台标签栏重渲染）会 **destroy() 整个轮盘**，容器与监听一并摘掉；
     而 MutationObserver 要看着整个 body 的 childList（工作台里 Monaco 每次击键都会改 DOM），
     为了一个已被 destroy 覆盖的场景常驻一个每帧跑的回调，不值。 */

  return {
    open,
    close,
    isOpen: () => expanded,
    destroy: () => {
      destroyed = true
      if (openTimer) clearTimeout(openTimer)
      if (closeTimer) clearTimeout(closeTimer)
      if (hideTimer) clearTimeout(hideTimer)
      trigger.removeEventListener("pointerenter", scheduleOpen)
      trigger.removeEventListener("pointerleave", scheduleClose)
      trigger.removeEventListener("click", onTriggerClick)
      trigger.removeEventListener("keydown", onTriggerKeyDown)
      keep.removeEventListener("click", onContainerClick)
      document.removeEventListener("pointermove", onDocPointerMove)
      document.removeEventListener("pointerleave", onDocPointerLeave, true)
      document.removeEventListener("pointerdown", onDocPointerDown)
      window.removeEventListener("resize", onResize)
      trigger.classList.remove("active")
      trigger.setAttribute("aria-expanded", "false")
      keep.remove()
    },
  }
}
