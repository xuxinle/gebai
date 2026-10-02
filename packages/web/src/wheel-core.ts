/**
 * 通用「按钮轮盘」原语：hover 入口按钮 → 双弧扇形展开一组按钮。
 *
 * 为什么抽出来：标题栏最右（`wheel.ts`）与文件工作台编辑器右上角（`files/main.ts`）用的是**同一套交互**——
 * 常驻位只留高频动作，其余收进轮盘。复制两遍意味着两套坐标/保持区/收起时序，改一处忘一处
 * （典型症状：一个入口 hover 就能弹、另一个点了才弹；一个外点收起、另一个不收起）。
 * 这里只放**几何与交互**，按钮长什么样、有哪些，全部由调用方给（`items` 里是已经绑好事件的元素）。
 *
 * 布局：**按空间自动分圈**——第一圈半径 = `firstRingR`，之后每圈 = 上一圈 + 按钮边长 + 间隙（步进不再写死）；
 * 每圈**容量**由「该圈半径 × 可用半张角」算（相邻按钮不叠，见 `capacityAt`）。
 * **分圈均衡**：按组分段后，每组先算**最少圈数**（k 圈容量之和能装下），再把该组项数尽量均分到 k 圈
 * （每圈项数差 ≤ 1）——不是贪心填圈。贪心会出现「剩 1 项单独挂一圈」的**孤项圈**（4 项、容量 3 → 3+1），
 * 那个孤零零的 1 看起来就像没排布。
 * 分组边界（`inner` → 其它）**必换圈**，语义分组不会被拆散在同一圈里。
 * 可用空间以入口为圆心向左/下取（视口边界 + `margin`）：半径再往外就超空间的那些圈并进最后一圈
 * （宁可挤一点也不出屏 / 不压入口那一行）。
 * 屏幕角 0°=正右、90°=正下，扇形朝**下（左）方**展开（入口都在界面上缘，只有向下有空间）。
 * **各圈围绕共同轴心对称展开**（`FAN_AXIS_DEG` = 90°，即正下方；同心扇面）——项围绕轴心**双向**拉开，
 * 张角取**刚好放下**（满足相邻方块「边长 + 间隙」的最小半张角，见 `fitArc`），一圈只有两三项时
 * 按钮就挨在一起，不会空出一大截弧；真放不下时才停在上限（宁可挤一点）。
 * 为什么不是「统一起始角 + 向左长」：不压入口行的角度上限是**绝对角**，起点越靠右可用跨度越小
 * （实测起始角 93° 时只剩 64.9°，内圈容量被低估成 3）；各圈都从 93° 起还会让首项叠成右缘竖列。
 * 围绕轴心对中后可用跨度翻倍、容量回到真实值，孤项圈与竖列同时消失。
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
/** 屏幕右缘的安全留白（px）：按钮中心到右缘至少留这么多，扇形不会探出屏外。 */
const EDGE_PAD = 6

/**
 * 角度窗口内均布 count 项：从 lo 到 hi 均匀铺开。
 * 各圈用**自己的**窗口对中（见下方 angleWindow）——不用全局轴心，因为窗口是**不对称**的：
 * 右缘约束只压右半边（角度小于 90° 那侧），上界（不压入口行）只压左半边，
 * 围着 90° 对称铺开会把半数按钮送出右缘。
 */
function spreadAngles(lo: number, hi: number, count: number): number[] {
  if (count <= 1) return [(lo + hi) / 2]
  return Array.from({ length: count }, (_, i) => lo + ((hi - lo) * i) / (count - 1))
}

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

/** 已落位点（屏幕坐标）：圈间避让的参照。 */
interface PlacedPoint {
  x: number
  y: number
}

/** 两方块（边长 size）是否重叠：坐标差在 x 与 y 两轴上都小于边长即重叠。 */
function hitsAny(x: number, y: number, size: number, placed: ReadonlyArray<PlacedPoint>): boolean {
  for (const p of placed) {
    if (Math.abs(p.x - x) < size && Math.abs(p.y - y) < size) return true
  }
  return false
}

/**
 * 算一弧的角度：在可用角窗口内均布 count 项，尽量拉开到「边长 + 间隙」，并**避开已落位的圈**。
 *
 * **半径固定不动**（上游给多少就是多少）：为了多塞按钮而把弧撑大，是在用“看着还是两圈吗”换“一排能放下”——
 * 弧位拥挤的正确解法是减项或改分组，不是拿半径去让路。所以只调**角度**。
 *
 * **角窗口对中**：各圈在**自己的窗口**内居中——窗口下界由「右缘不越屏」定、上界由「不压入口行」定，
 * 两者对 90° 不一定对称，所以不能用一个全局轴心铺（会把靠右那半送出屏）。
 *
 * **圈间避让**：径向步进（边长 + 间隙）只保证径向上分开；相邻圈角度接近、方向又偏 45° 时，
 * 两按钮的 x/y 坐标差会被压到小于边长（实测内圈斜向相邻的两圈重叠 3~5px）。
 * 修法不是把半径再推远（那是拿“还是两圈吗”换“不重叠”），而是**整圈在当前窗口内滑动**
 * （微调中心角）找无碰撞位置，找不到才退回窗口中心（重叠最小优先）。
 */
function fitArc(o: {
  count: number
  size: number
  gap: number
  r: number
  /** 本圈的可用角窗口下/上界（度，屏幕角）：右缘不越屏 → 上界不压入口行 */
  winLo: number
  winHi: number
  /** 圆心（入口中心）屏幕坐标：把角度转成坐标比碰撞用 */
  cx: number
  cy: number
  /** 已落位点（内圈已排完的按钮中心） */
  placed: ReadonlyArray<PlacedPoint>
}): ArcFit {
  const { count, size, gap, r, winLo, winHi, cx, cy, placed } = o
  if (count <= 0) return { r, angles: [] }
  const need = size + gap
  const center0 = (winLo + winHi) / 2
  const halfWin = Math.max(0, (winHi - winLo) / 2)
  /** 以 center0 为中心、半张角 half，算 count 项角度。 */
  const at = (half: number, center = center0): number[] => spreadAngles(center - half, center + half, count)
  /** 该角度序列在半径 r 上的落点是否与已落位点不重叠。 */
  const clear = (angles: number[]): boolean =>
    angles.every((deg) => {
      const [dx, dy] = polar(r, deg)
      return !hitsAny(cx + dx, cy + dy, size, placed)
    })
  // 半张角：先取「刚好拉开到间隙要求」的最小值（紧凑），但也得避开已落位点
  let half = halfWin
  if (arcFits(r, at(half), need)) {
    let lo = 0
    let hi = halfWin
    for (let i = 0; i < 16; i++) {
      const mid = (lo + hi) / 2
      if (arcFits(r, at(mid), need)) hi = mid
      else lo = mid
    }
    half = hi
  }
  if (count === 1) {
    // 单按钮：先试窗口中心，不清晰再在整个窗口内扫描
    let best = center0
    if (!clear(at(half, center0))) {
      let bestHits = Infinity
      for (let d = -halfWin; d <= halfWin; d += 2) {
        const c = center0 + d
        const hits = placed.filter((p) => {
          const [dx, dy] = polar(r, c)
          return Math.abs(p.x - (cx + dx)) < size && Math.abs(p.y - (cy + dy)) < size
        }).length
        if (hits === 0) {
          best = c
          bestHits = 0
          break
        }
        if (hits < bestHits) {
          bestHits = hits
          best = c
        }
      }
    }
    return { r, angles: [best] }
  }
  // 多按钮：整圈在窗口内滑动，找无碰撞的中心角（步进 4°，够细且不贵）
  let bestCenter = center0
  if (!clear(at(half, center0))) {
    let bestHits = Infinity
    for (let d = -halfWin; d <= halfWin; d += 4) {
      const c = Math.min(winHi - half, Math.max(winLo + half, center0 + d))
      const angles = at(half, c)
      const hits = angles.filter((deg) => {
        const [dx, dy] = polar(r, deg)
        return hitsAny(cx + dx, cy + dy, size, placed)
      }).length
      if (hits === 0) {
        bestCenter = c
        bestHits = 0
        break
      }
      if (hits < bestHits) {
        bestHits = hits
        bestCenter = c
      }
    }
  }
  return { r, angles: at(half, bestCenter) }
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
    /** 圈间步进：**取最坏情况的下界**——相邻两圈同角度时，两按钮坐标差为 (step·cosθ, step·sinθ)，
     *  要任两轴中至少一轴 ≥ 边长，则 step·max(|cosθ|,|sinθ|) ≥ size 恒成立。
     *  max(|cos|,|sin|) 在 45° 处取最小 √2/2，故 **step ≥ size·√2** 才能保证斜向 45° 也不叠。
     *  用旧值 size + gap（40px）时，45° 处两轴差仅 28px < 32 → 相邻圈重叠 3~5px，
     *  只能靠整圈滑动避让补救（各圈中心被挤散、看着不像同心扇形）。取对角落差后
     *  同角度即天然分开，各圈可共享同一中心角。 */
    const step = Math.ceil((size * Math.SQRT2 + gap) / 2) * 2
    /**
     * 终点角处按钮中心的最小纵向偏移：按钮顶边不得超过入口按钮的底边（否则压到入口那一行）。
     * 半径越小这个约束越紧（@r=85 时大约只能用到 155°），所以**跨度上限得逐圈算**，不能一次算好。
     */
    const dyMin = (rect.bottom - cy) + size / 2 + 4
    /** 半径 r 那一圈的**可用角窗口** [下界, 上界]（度）：
     *  ① 下界（最小角，靠右上）：按钮不得探出屏幕右缘——按钮宽 size，其中心在 cx + r·cos(θ) 处，
     *     要求 cx + r·cos(θ) + size/2 ≤ vw - EDGE_PAD；
     *  ② 上界（最大角，靠左下）：按钮不得压入口那一行——dy = r·sin(θ) ≥ dyMin；
     *  两者不一定对称于 90°，所以各圈的窗口中心各不相同，不能用一个全局轴心。 */
    const vw = typeof window === "undefined" ? Infinity : window.innerWidth
    /**
     * 半径 r 那一圈的**可用角窗口** [lo, hi]（度，屏幕角）。两条硬约束：
     *  ① 右缘不越屏：按钮中心 cx + r·cosθ、宽 size，要求 cx + r·cosθ + size/2 ≤ vw - EDGE_PAD
     *     → cosθ ≤ C，即 **θ ≥ acos(C)**（靠右/上的一侧被削掉）；
     *  ② 不压入口行：按钮顶边须在入口底边之下（dyMin = 入口底到圆心的距离 + 半按钮 + 余量）
     *     → r·sinθ ≥ dyMin，即 sinθ ≥ S → **θ ∈ [asin(S), 180 - asin(S)]**
     *     （θ 接近 0° 与 180° 时 sin 小、按钮贴近水平线，两侧都要排）。
     *  两约束对 90° **不一定对称**（入口在右上角，右缘约束只削一侧），
     *  所以窗口中心逐圈不同，不能用一个全局扇面轴心去对中。
     */
    const angleWindow = (r: number): [number, number] => {
      const cosMax = Number.isFinite(vw) ? (vw - EDGE_PAD - size / 2 - cx) / r : 1
      const loEdge = cosMax >= 1 ? 0 : cosMax <= -1 ? 180 : (Math.acos(cosMax) * 180) / Math.PI
      const sinMin = dyMin <= 0 || dyMin >= r ? 0 : dyMin / r
      const dev = (Math.asin(Math.max(-1, Math.min(1, sinMin))) * 180) / Math.PI
      const lo = Math.max(loEdge, dev)
      const hi = Math.min(180, 180 - dev, lo + maxSpan)
      return [lo, Math.max(lo, hi)]
    }
    /** 半径 r 那一圈的容量：在角窗口内均布时不叠（且留 MIN_BUTTON_GAP）前提下最多几项。 */
    const capacityAt = (r: number): number => {
      const [lo, hi] = angleWindow(r)
      let n = 0
      while (n < 32) {
        const next = n + 1
        if (!arcFits(r, spreadAngles(lo, hi, next), size + MIN_BUTTON_GAP)) break
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

    /* ---------- 第一步：把可见项分圈（**组内均衡**，不出孤项圈） ----------
     * 贪心填圈（能塞几个塞几个）会把「剩 1 项」单独挂一圈（如 4 项、容量 3 → 3+1），
     * 那个孤零零的 1 看着就像没排布。改为：按组分段 → 每组先算**最少圈数**（k 圈需容量之和能装下），
     * 再把该组项数**尽量均分**到 k 圈（前 remainder 圈各多 1）——每圈项数差 ≤ 1，不出孤项圈。 */
    const rings: { r: number; list: WheelItem[] }[] = []
    /** 从首圈半径出发，第 n 圈的半径（圈半径按「边长 + 间隙」步进）。 */
    const ringR = (n: number): number => firstRingR0 + n * step
    // 按组切成连续段（分组语义：会话操作 / 应用操作）
    const segments: WheelItem[][] = []
    for (const it of visible) {
      const g = it.group === "inner" ? "inner" : "outer"
      const last = segments[segments.length - 1]
      if (last && (last[0]!.group === "inner") === (g === "inner")) last.push(it)
      else segments.push([it])
    }
    let ringIdx = 0
    for (const seg of segments) {
      // 每段的最少圈数：逐步加圈直到已开出的圈容量之和能装下这一段
      let k = 1
      const capSum = (n: number): number => {
        let s = 0
        for (let j = 0; j < n; j++) {
          // 半径超出可用空间时不再往外开圈：该圈容量按「装得下剩余全部」算（宁可挤也不出屏）
          s += (ringR(ringIdx + j) + step > maxR ? seg.length : capacityAt(ringR(ringIdx + j)))
        }
        return s
      }
      while (k < MAX_RINGS && capSum(k) < seg.length) k++
      // 均衡分配：把 seg.length 项尽量均分到 k 圈（前 remainder 圈各多 1）
      const base = Math.floor(seg.length / k)
      const remainder = seg.length % k
      for (let j = 0; j < k && rings.length < MAX_RINGS; j++) {
        const take = base + (j < remainder ? 1 : 0)
        if (take <= 0) continue
        rings.push({ r: ringR(ringIdx + j), list: seg.splice(0, take) })
      }
      ringIdx = rings.length
      // 半径再往外就超出可用空间：剩余项并进最后一圈（宁可挤一点也不出屏 / 不压入口行）
      if (seg.length && rings.length) {
        const over = rings[rings.length - 1]!
        if (ringR(rings.length) + step > maxR) over.list = over.list.concat(seg.splice(0))
      }
      // 兜底：圈数撞上 MAX_RINGS 而还有剩余时，并进最后一圈（极端项数下的降级）
      if (seg.length && rings.length) rings[rings.length - 1]!.list = rings[rings.length - 1]!.list.concat(seg.splice(0))
    }

    /* ---------- 第二步：逐圈定角（各自角窗口内均布 + 避让已落位的圈） ---------- */
    const plans: RingPlan[] = []
    const placed: PlacedPoint[] = [] // 已排完的圈：按钮中心屏幕坐标（圈间避让参照）
    for (const ring of rings) {
      const [winLo, winHi] = angleWindow(ring.r)
      const fit = fitArc({ count: ring.list.length, size, gap, r: ring.r, winLo, winHi, cx, cy, placed })
      plans.push({ r: fit.r, list: ring.list, angles: fit.angles })
      for (const deg of fit.angles) {
        const [dx, dy] = polar(ring.r, deg)
        placed.push({ x: cx + dx, y: cy + dy })
      }
    }

    /* ---------- 第三步：落位 + 保持区收紧为扇形边界盒（入口按钮 ∪ 各可见扇形按钮） ---------- */
    let minX = rect.left
    let minY = rect.top
    let maxX = rect.right
    let maxY = rect.bottom
    for (const plan of plans) {
      plan.list.forEach((it, k) => {
        const { w, h } = sizeOf(it.el)
        const [dx0, dy0] = polar(plan.r, plan.angles[k])
        // 定精度：cos/sin 在 90°/180° 上会算出 1e-15 量级的浮点残差，直接拼进 translate 会写出
        // `7.6e-15px`（科学计数法）——浏览器能解析，但可读性差、消费方（距离解析）容易踩坑，取 3 位小数。
        const dx = Number(dx0.toFixed(3))
        const dy = Number(dy0.toFixed(3))
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
