/**
 * 通用按钮轮盘原语（wheel-core）：几何 + 生命周期。
 *
 * 覆盖四件事：
 * ① 项被搬进容器并打上组类名（外层样式靠它定尺寸与组标识）；
 * ② 展开后按钮落在**对应弧位**（每个按钮到圆心的距离等于它那一弧的半径，且扇形朝下展开）；
 * ③ 外点收起（document 上的 pointerdown 不落在容器里）；
 * ④ destroy 摘干净（容器离场、监听退订，之后再 hover 不展开）——工作台标签栏每次重渲染都要 destroy 一次，
 *    漏掉就是每重建一次多留一份扇形 DOM 与一组 document 监听。
 *
 * 元素桩在这里就地实现：测试基线（scripts/test-preload.ts）的 Proxy 桩把 classList 做成 no-op、
 * document 的事件注册也是 no-op，断言不了展开态与外点收起，所以本文件临时换一套最小实现
 * （classList 直接读写 className、document 事件真实登记），用完还原。
 */
import { afterEach, beforeEach, describe, expect, test } from "bun:test"
import { createWheel } from "./wheel-core"
import { createKeymap, setActiveKeymap, type KeyTarget } from "./keymap"

interface StubEl {
  tagName: string
  className: string
  classList: { add(c: string): void; remove(c: string): void; contains(c: string): boolean; toggle(c: string): void }
  style: Record<string, string>
  dataset: Record<string, string>
  hidden: boolean
  offsetWidth: number
  offsetHeight: number
  parentNode: StubEl | null
  children: StubEl[]
  textContent: string
  appendChild(c: StubEl): StubEl
  remove(): void
  contains(c: StubEl): boolean
  setAttribute(k: string, v: string): void
  getAttribute(k: string): string | null
  addEventListener(t: string, cb: (ev?: unknown) => void): void
  removeEventListener(t: string, cb: (ev?: unknown) => void): void
  dispatchEvent(ev: { type: string; target?: unknown; key?: string }): boolean
  getBoundingClientRect(): { left: number; top: number; right: number; bottom: number; width: number; height: number }
  listenerCount(): number
  /** 可覆写的矩形（默认 0,0,32,32）：分圈与可用空间都按圆心位置算，测试要能把它放到真实位置（如右上角）。 */
  rect: { left: number; top: number; right: number; bottom: number; width: number; height: number }
}

/** classList 直接读写 className 字符串——`el(tag, cls)` 是**赋 className**，两套存储会互相看不见。 */
function stub(tag = "div"): StubEl {
  const listeners = new Map<string, Array<(ev?: unknown) => void>>()
  const attrs = new Map<string, string>()
  const has = (c: string): boolean => el.className.split(/\s+/).filter(Boolean).includes(c)
  const el: StubEl = {
    tagName: tag.toUpperCase(),
    className: "",
    classList: {
      add: (c) => {
        if (!has(c)) el.className = el.className ? `${el.className} ${c}` : c
      },
      remove: (c) => {
        el.className = el.className.split(/\s+/).filter((x) => x && x !== c).join(" ")
      },
      contains: (c) => has(c),
      toggle: (c) => (has(c) ? el.classList.remove(c) : el.classList.add(c)),
    },
    style: {},
    dataset: {},
    hidden: false,
    offsetWidth: 32,
    offsetHeight: 32,
    parentNode: null,
    children: [],
    textContent: "",
    appendChild(c) {
      c.parentNode = el
      el.children.push(c)
      return c
    },
    remove() {
      const p = el.parentNode
      if (!p) return
      const i = p.children.indexOf(el)
      if (i >= 0) p.children.splice(i, 1)
      el.parentNode = null
    },
    contains(c) {
      return el.children.includes(c)
    },
    setAttribute: (k, v) => void attrs.set(k, v),
    getAttribute: (k) => attrs.get(k) ?? null,
    addEventListener: (t, cb) => void listeners.set(t, [...(listeners.get(t) ?? []), cb]),
    removeEventListener: (t, cb) => void listeners.set(t, (listeners.get(t) ?? []).filter((f) => f !== cb)),
    dispatchEvent: (ev) => {
      for (const cb of listeners.get(ev.type) ?? []) cb({ preventDefault() {}, ...ev, target: ev.target ?? el })
      return true
    },
    getBoundingClientRect: () => el.rect,
    rect: { left: 0, top: 0, right: 32, bottom: 32, width: 32, height: 32 },
    listenerCount: () => [...listeners.values()].reduce((n, l) => n + l.length, 0),
  }
  return el
}

/* ---------------- 临时 DOM 接线（beforeEach 装、afterEach 还原） ---------------- */

const docListeners = new Map<string, Array<(ev?: unknown) => void>>()
const origCreate = document.createElement.bind(document)
const origCreateNS = document.createElementNS.bind(document)
const origAdd = document.addEventListener.bind(document)
const origRemove = document.removeEventListener.bind(document)
const origDispatch = document.dispatchEvent.bind(document)
const origBody = document.body

beforeEach(() => {
  docListeners.clear()
  // body 也换成桩：基线 body 的 appendChild 不记 parentNode，容器的 remove() 会静默失败（用例间互相污染）
  ;(document as unknown as { body: unknown }).body = stub("body")
  ;(document as unknown as { createElement: unknown }).createElement = (t?: string) => stub(t)
  ;(document as unknown as { createElementNS: unknown }).createElementNS = (_ns: string, t?: string) => stub(t)
  ;(document as unknown as { addEventListener: unknown }).addEventListener = (t: string, cb: (ev?: unknown) => void) => {
    docListeners.set(t, [...(docListeners.get(t) ?? []), cb])
  }
  ;(document as unknown as { removeEventListener: unknown }).removeEventListener = (t: string, cb: (ev?: unknown) => void) => {
    docListeners.set(t, (docListeners.get(t) ?? []).filter((f) => f !== cb))
  }
  ;(document as unknown as { dispatchEvent: unknown }).dispatchEvent = (ev: { type: string }) => {
    for (const cb of docListeners.get(ev.type) ?? []) cb(ev)
    return true
  }
})

afterEach(() => {
  for (const c of containers()) c.remove()
  ;(document as unknown as { body: unknown }).body = origBody
  ;(document as unknown as { createElement: unknown }).createElement = origCreate
  ;(document as unknown as { createElementNS: unknown }).createElementNS = origCreateNS
  ;(document as unknown as { addEventListener: unknown }).addEventListener = origAdd
  ;(document as unknown as { removeEventListener: unknown }).removeEventListener = origRemove
  ;(document as unknown as { dispatchEvent: unknown }).dispatchEvent = origDispatch
})

/** 在测试里给一个视口（layout 读 window.innerWidth 算右缘约束；测试环境无 window）：
 *  展开是异步的（openDelay → setTimeout → layout），所以传入的回调可以 await。 */
async function withViewport(width: number, fn: () => Promise<void>): Promise<void> {
  const had = "window" in globalThis
  const prev = (globalThis as { window?: unknown }).window
  ;(globalThis as { window?: unknown }).window = { innerWidth: width, innerHeight: 720, setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout }
  try {
    await fn()
  } finally {
    if (had) (globalThis as { window?: unknown }).window = prev
    else delete (globalThis as { window?: unknown }).window
  }
}

/** 桩 → HTMLElement（createWheel 面向真实元素，测试只关心它用到的那些成员）。 */
const asEl = (e: StubEl): HTMLElement => e as unknown as HTMLElement

/** 容器（挂在 body 上的那一层）。 */
const containers = (): StubEl[] => (document.body as unknown as { children: StubEl[] }).children.filter((c) => c.classList.contains("wheel"))

/** 展开是异步的（错落动画前先走一个宏任务）。 */
const tick = (): Promise<void> => new Promise((r) => setTimeout(r, 5))

/** 从 transform="translate(dxpx, dypx)" 里取弧位偏移。 */
function offsetOf(el: StubEl): { dx: number; dy: number; radius: number } {
  const m = /translate\((-?[\d.]+)px,\s*(-?[\d.]+)px\)/.exec(el.style.transform ?? "")
  const dx = m ? Number(m[1]) : NaN
  const dy = m ? Number(m[2]) : NaN
  return { dx, dy, radius: Math.hypot(dx, dy) }
}

describe("createWheel（按钮轮盘原语）", () => {
  test("项被搬进容器并分组打标；收起态不可见", () => {
    const trigger = stub("button")
    const a = stub("button")
    const b = stub("button")
    const c = stub("button")
    const w = createWheel({
      trigger: asEl(trigger),
      items: [
        { el: asEl(a), group: "inner" },
        { el: asEl(b), group: "inner" },
        { el: asEl(c) },
      ],
    })
    const keep = containers()[0]
    expect(keep).toBeDefined()
    expect(keep.children).toContain(a)
    expect(keep.children).toContain(b)
    expect(keep.children).toContain(c)
    expect(a.classList.contains("wheel-item")).toBe(true)
    expect(a.classList.contains("wheel-inner")).toBe(true)
    expect(c.classList.contains("wheel-item")).toBe(true)
    expect(c.classList.contains("wheel-inner")).toBe(false)
    // 收起态：透明 + 缩到 0.4 + 容器没有 open
    expect(a.style.opacity).toBe("0")
    expect(a.style.transform).toBe("translate(0, 0) scale(0.4)")
    expect(keep.classList.contains("open")).toBe(false)
    w.destroy()
  })

  test("hover 展开：按钮落在各自圈位上（内圈半径 < 外圈半径，扇形朝下）", async () => {
    const trigger = stub("button")
    // 入口放到窗口右上角（真实位置）：分圈要按圆心位置算可用空间，摆在原点会被当成“贴左边界”
    trigger.rect = { left: 1100, top: 5, right: 1124, bottom: 29, width: 24, height: 24 }
    const inner1 = stub("button")
    const inner2 = stub("button")
    const outer1 = stub("button")
    const w = createWheel({
      trigger: asEl(trigger),
      items: [
        { el: asEl(inner1), group: "inner" },
        { el: asEl(inner2), group: "inner" },
        { el: asEl(outer1) },
      ],
      firstRingR: 85,
    })
    const keep = containers()[0]
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    expect(keep.classList.contains("open")).toBe(true)
    expect(trigger.getAttribute("aria-expanded")).toBe("true")
    expect(inner1.style.opacity).toBe("1")
    expect(inner1.style.transitionDelay).toBe("0ms")
    expect(inner2.style.transitionDelay).toBe("14ms")

    // 内圈两项同半径；外圈（组边界）必换圈，半径 = 首圈 + 边长 + 间隙
    for (const el of [inner1, inner2]) expect(offsetOf(el).radius).toBeCloseTo(85, 3)
    // 圈间步进 = ceil((size·√2 + gap)/2)·2（最坏情况 45° 对角下也不叠，见 wheel-core.ts 的 step）
    const STEP = Math.ceil((32 * Math.SQRT2 + 8) / 2) * 2
    expect(offsetOf(outer1).radius).toBeCloseTo(85 + STEP, 3)
    // 扇形朝下（入口在界面上缘，只有向下有空间）
    for (const el of [inner1, inner2, outer1]) expect(offsetOf(el).dy).toBeGreaterThan(0)
    w.destroy()
  })

  test("外点收起：pointerdown 落在容器之外", async () => {
    const trigger = stub("button")
    const a = stub("button")
    const w = createWheel({ trigger: asEl(trigger), items: [{ el: asEl(a), group: "inner" }] })
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    expect(a.style.opacity).toBe("1")
    document.dispatchEvent({ type: "pointerdown", target: stub() } as unknown as Event)
    expect(trigger.getAttribute("aria-expanded")).toBe("false")
    expect(a.style.opacity).toBe("0")
    w.destroy()
  })

  test("保持区不吃指针事件：容器不接 pointerenter/pointerleave，指针在盒内由 pointermove 判定", async () => {
    const trigger = stub("button")
    const a = stub("button")
    const w = createWheel({ trigger: asEl(trigger), items: [{ el: asEl(a), group: "inner" }] })
    const keep = containers()[0]
    // 容器只留 click（点扇形按钮后收起靠它冒泡），不挂 pointerenter/pointerleave——
    // 它若可命中，就会把下方标签栏/消息区的控件一起挡掉
    expect(keep.listenerCount()).toBe(1)
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    expect(trigger.getAttribute("aria-expanded")).toBe("true")
    // 桩元素的 rect 统一为 0,0,32,32：指针在盒内（10,10）不收起，离开盒子（500,500）约 CLOSE_DELAY 后收起
    document.dispatchEvent({ type: "pointermove", clientX: 500, clientY: 500 } as unknown as Event)
    document.dispatchEvent({ type: "pointermove", clientX: 10, clientY: 10 } as unknown as Event)
    await new Promise((r) => setTimeout(r, 320))
    expect(trigger.getAttribute("aria-expanded")).toBe("true")
    document.dispatchEvent({ type: "pointermove", clientX: 500, clientY: 500 } as unknown as Event)
    await new Promise((r) => setTimeout(r, 320))
    expect(trigger.getAttribute("aria-expanded")).toBe("false")
    w.destroy()
  })

  test("指针移出文档（切窗口）后收起：只认文档根的 pointerleave", async () => {
    const trigger = stub("button")
    const a = stub("button")
    const w = createWheel({ trigger: asEl(trigger), items: [{ el: asEl(a), group: "inner" }] })
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    expect(trigger.getAttribute("aria-expanded")).toBe("true")
    // 元素级 pointerleave（如指针从入口滑到扇形按钮上）不算离开——目标不是文档根
    document.dispatchEvent({ type: "pointerleave", target: stub() } as unknown as Event)
    await new Promise((r) => setTimeout(r, 320))
    expect(trigger.getAttribute("aria-expanded")).toBe("true")
    // 文档根的 pointerleave = 指针离开整个文档（切到别的窗口）：再没有 pointermove 可指望，直接收起
    document.dispatchEvent({ type: "pointerleave", target: document.documentElement } as unknown as Event)
    await new Promise((r) => setTimeout(r, 320))
    expect(trigger.getAttribute("aria-expanded")).toBe("false")
    w.destroy()
  })

  test("pointermove / pointerleave 监听跟着展开态挂/摘（收起态不跑每帧回调）", async () => {
    const trigger = stub("button")
    const a = stub("button")
    const w = createWheel({ trigger: asEl(trigger), items: [{ el: asEl(a), group: "inner" }] })
    const moveCount = () => (docListeners.get("pointermove") ?? []).length
    const leaveCount = () => (docListeners.get("pointerleave") ?? []).length
    expect(moveCount()).toBe(0)
    expect(leaveCount()).toBe(0)
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    expect(moveCount()).toBe(1)
    expect(leaveCount()).toBe(1)
    document.dispatchEvent({ type: "pointerdown", target: stub() } as unknown as Event)
    expect(moveCount()).toBe(0)
    expect(leaveCount()).toBe(0)
    w.destroy()
  })

  test("destroy：容器离场 + 监听退订（之后再 hover 不展开）", async () => {
    const trigger = stub("button")
    const a = stub("button")
    const w = createWheel({ trigger: asEl(trigger), items: [{ el: asEl(a), group: "inner" }] })
    expect(containers().length).toBe(1)
    w.destroy()
    expect(containers().length).toBe(0)
    expect(trigger.listenerCount()).toBe(0)
    expect((docListeners.get("pointermove") ?? []).length).toBe(0)
    expect((docListeners.get("pointerleave") ?? []).length).toBe(0)
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    expect(containers().length).toBe(0)
  })

  test("键盘入口：入口获焦后 Enter 展开、Esc 收起（Esc 走键位作用域）", async () => {
    const map = createKeymap([])
    setActiveKeymap(map)
    map.install(document as unknown as KeyTarget)
    const trigger = stub("button")
    const a = stub("button")
    const w = createWheel({ trigger: asEl(trigger), items: [{ el: asEl(a), group: "inner" }] })
    trigger.dispatchEvent({ type: "keydown", key: "Enter" })
    await tick()
    expect(trigger.getAttribute("aria-expanded")).toBe("true")
    document.dispatchEvent({ type: "keydown", key: "Escape", preventDefault() {}, stopPropagation() {} } as unknown as Event)
    expect(trigger.getAttribute("aria-expanded")).toBe("false")
    w.destroy()
    map.uninstall()
    setActiveKeymap(null)
  })

  test("hidden 项不参与弧位排布（保持收起态）", async () => {
    const trigger = stub("button")
    const shown = stub("button")
    const hidden = stub("button")
    hidden.hidden = true
    const w = createWheel({
      trigger: asEl(trigger),
      items: [
        { el: asEl(shown), group: "inner" },
        { el: asEl(hidden), group: "inner" },
      ],
    })
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    // 只剩一个可见项：落在区间中心角度上（单圈只一项时居中），半径仍是首圈半径
    expect(offsetOf(shown).radius).toBeCloseTo(85, 3)
    expect(hidden.style.opacity).toBe("0")
    expect(hidden.style.transform).toBe("translate(0, 0) scale(0.4)")
    w.destroy()
  })

  test("分圈：同租放不下时**自动落到下一圈**，各圈半径按「边长 + 间隙」步进", async () => {
    /* 7 个同类项 @ 32px（先全排一圈看看能放几个）：首圈半径 85 在可用张角内最多 3 项，
       余项落到 85+40=125，再放不下就 165……断言取“实际出现的圈半径”集合，不写死项数分配——
       容量公式（半径 × 可用张角）改了就跟着变，这正是这份用例要盯住的东西。 */
    const trigger = stub("button")
    trigger.rect = { left: 1100, top: 5, right: 1124, bottom: 29, width: 24, height: 24 }
    const all = Array.from({ length: 7 }, () => stub("button"))
    const w = createWheel({
      trigger: asEl(trigger),
      items: all.map((e) => ({ el: asEl(e) })),
      firstRingR: 85,
    })
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    const radii = all.map((e) => Number(offsetOf(e).radius.toFixed(2)))
    const rings = [...new Set(radii)].sort((x, y) => x - y)
    expect(rings[0]).toBe(85)
    expect(rings.length).toBeGreaterThan(1) // 一圈真放不下：必须用上第二圈
    // 圈间步进按「最坏情况 45° 对角」下界取值（size·√2 + gap 向上取偶），不是 size + gap
    const STEP = Math.ceil((32 * Math.SQRT2 + 8) / 2) * 2
    for (let i = 1; i < rings.length; i++) expect(rings[i]! - rings[i - 1]!).toBeCloseTo(STEP, 2)
    // 每圈内部：相邻按钮不叠（用方块的实际不重叠条件：|Δx| 或 |Δy| ≥ 边长）
    for (const r of rings) {
      const pts = all.map((e) => offsetOf(e)).filter((p) => Math.abs(p.radius - r) < 0.5)
      for (let i = 1; i < pts.length; i++) {
        const dx = Math.abs(pts[i]!.dx - pts[i - 1]!.dx)
        const dy = Math.abs(pts[i]!.dy - pts[i - 1]!.dy)
        expect(Math.max(dx, dy)).toBeGreaterThanOrEqual(32)
      }
    }
    w.destroy()
  })

  test("分圈：可用空间不够时**不再往外开圈**，剩余项并进最后一圈（宁可挤也不出屏）", async () => {
    const trigger = stub("button")
    // 入口贴近屏幕左缘（cx 只有 20px）：可用半径很小，来不及开第二轮
    trigger.rect = { left: 8, top: 5, right: 32, bottom: 29, width: 24, height: 24 }
    const all = Array.from({ length: 6 }, () => stub("button"))
    const w = createWheel({
      trigger: asEl(trigger),
      items: all.map((e) => ({ el: asEl(e) })),
      firstRingR: 85,
      margin: 8,
    })
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    for (const e of all) expect(offsetOf(e).radius).toBeCloseTo(85, 3)
    w.destroy()
  })

  test("圈位：项少时只占「刚好放下」的跨度（不硬撑到固定张角）", async () => {
    const trigger = stub("button")
    trigger.rect = { left: 1100, top: 5, right: 1124, bottom: 29, width: 24, height: 24 }
    const a = stub("button")
    const b = stub("button")
    const c = stub("button")
    const w = createWheel({
      trigger: asEl(trigger),
      items: [
        { el: asEl(a), group: "inner" },
        { el: asEl(b), group: "inner" },
        { el: asEl(c) },
      ],
      firstRingR: 85,
    })
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    expect(offsetOf(a).radius).toBeCloseTo(85, 3)
    expect(offsetOf(b).radius).toBeCloseTo(85, 3)
    // 角窗口内均布（同心扇面）：两项以窗口中心为中线对称展开；窗口受右缘约束整体靠右上，
    // 不再固定 90°（围绕 90° 对称会把一半按钮送出屏右缘）
    const degOf = (e: StubEl): number => (Math.atan2(offsetOf(e).dy, offsetOf(e).dx) * 180) / Math.PI
    expect((degOf(a) + degOf(b)) / 2).toBeLessThan(90) // 窗口中心靠右上（右缘约束）
    expect((degOf(a) + degOf(b)) / 2).toBeGreaterThan(60)
    // 圆环上只有 2 项：跨度就是“刚好分开”的那一点，不能空出一大截弧
    // （旧实现固定撑到 54°，实测两个按钮隔了 55px 的弧，看着就不在一圈上）
    const span = degOf(b) - degOf(a)
    expect(span).toBeGreaterThan(10)
    expect(span).toBeLessThan(40)
    // 相邻按钮实际间隙达标（方块不叠且留出 gap）：|Δx| 或 |Δy| ≥ 边长 + 间隙
    const dx = Math.abs(offsetOf(b).dx - offsetOf(a).dx)
    const dy = Math.abs(offsetOf(b).dy - offsetOf(a).dy)
    expect(Math.max(dx, dy)).toBeGreaterThanOrEqual(32 + 8 - 0.5)
    // 只一项的圈：落在该圈角窗口的中心（受右缘与入口行两约束，不再固定 90°）
    expect(degOf(c)).toBeLessThan(90)
    expect(degOf(c)).toBeGreaterThan(60)
    w.destroy()
  })

  test("同心扇面：各圈围绕同一轴心对中（不出竖列、圈内项数均衡不出孤项圈）", async () => {
    const trigger = stub("button")
    trigger.rect = { left: 1100, top: 5, right: 1124, bottom: 29, width: 24, height: 24 }
    // 内圈 4 项（容量 3 的真实旧例）：旧贪心填圈会装成 3+1，那个孤零零的 1 看着就像没排布
    const els = Array.from({ length: 4 }, () => stub("button"))
    const w = createWheel({
      trigger: asEl(trigger),
      items: els.map((e) => ({ el: asEl(e), group: "inner" as const })),
      firstRingR: 85,
    })
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    const degOf = (e: StubEl): number => (Math.atan2(offsetOf(e).dy, offsetOf(e).dx) * 180) / Math.PI
    const byRing = new Map<number, StubEl[]>()
    for (const e of els) {
      const r = Number(offsetOf(e).radius.toFixed(2))
      byRing.set(r, [...(byRing.get(r) ?? []), e])
    }
    const radii = [...byRing.keys()].sort((x, y) => x - y)
    // 均衡分圈：4 项两圈 → 2+2（不是 3+1 的孤项圈）；每圈项数差 ≤ 1
    const counts = radii.map((r) => byRing.get(r)!.length)
    expect(Math.max(...counts) - Math.min(...counts)).toBeLessThanOrEqual(1)
    // 同心扇面：每圈项在**自己的角窗口**内对中——窗口中心受右缘/入口行约束逐圈微调，
    // 同一窗口下各圈中心相近（不再是“各圈都从 93° 向左长”也不叠成竖列）
    for (const r of radii) {
      const avg = byRing.get(r)!.reduce((s, e) => s + degOf(e), 0) / byRing.get(r)!.length
      expect(avg).toBeGreaterThan(60)
      expect(avg).toBeLessThanOrEqual(90)
    }
    w.destroy()
  })

  test("引导弧线：每圈一条（圈数 = 弧线数），半径等于该圈半径", async () => {
    const trigger = stub("button")
    trigger.rect = { left: 1100, top: 5, right: 1124, bottom: 29, width: 24, height: 24 }
    const all = Array.from({ length: 7 }, () => stub("button"))
    const w = createWheel({ trigger: asEl(trigger), items: all.map((e) => ({ el: asEl(e) })), firstRingR: 85 })
    const keep = containers()[0]
    // 桩元素的 setAttribute 不回流到 classList，所以按**标签**找弧线 SVG（真实浏览器里它是 .wheel-arc）
    const svg = keep.children.find((c) => c.tagName === "SVG")!
    trigger.dispatchEvent({ type: "pointerenter" })
    await tick()
    const radii = [...new Set(all.map((e) => Number(offsetOf(e).radius.toFixed(2))))].sort((x, y) => x - y)
    const paths = svg.children.filter((c) => c.tagName === "PATH")
    expect(paths.length).toBe(radii.length)
    // 每条弧线半径 = 对应圈半径（“M x y A r r …” 里的 r）
    const arcR = paths.map((p) => Number(/A (\d+(?:\.\d+)?) /.exec(p.getAttribute("d") ?? "")?.[1]))
    for (const r of radii) expect(arcR.some((v) => Math.abs(v - r) < 0.5)).toBe(true)
    // arcs: false 时一条不画
    const w2 = createWheel({ trigger: asEl(stub("button")), items: [{ el: asEl(stub("button")) }], arcs: false })
    const keep2 = containers().at(-1)!
    expect(keep2.children.some((c) => c.tagName === "SVG")).toBe(false)
    w2.destroy()
    w.destroy()
  })

  test("右缘约束：入口靠屏右缘时所有按钮留在屏内（角窗口下界抬高，不向 90° 右侧对称铺开）", async () => {
    // 复现真实缺陷：入口在窗口右上角（cx = 1256, vw = 1280），若各圈围绕 90° 对称展开，
    // 靠右那半（角度 < 90°）的按钮中心会跑到 cx + r·cosθ，内圈就探出右缘。
    // 角窗口把下界抬到「右缘不越屏」对应的 acos 上，整圈整体向左上偏移。
    const trigger = stub("button")
    trigger.rect = { left: 1244, top: 5, right: 1268, bottom: 29, width: 24, height: 24 }
    const els = Array.from({ length: 6 }, () => stub("button"))
    const w = createWheel({ trigger: asEl(trigger), items: els.map((e) => ({ el: asEl(e) })), firstRingR: 85 })
    await withViewport(1280, async () => {
      trigger.dispatchEvent({ type: "pointerenter" })
      await tick()
    })
    const cx = trigger.rect.left + trigger.rect.width / 2
    // 每个按钮中心的 x 坐标都在右缘内（中心 + 半按钮 ≤ vw）
    for (const e of els) {
      const centerX = cx + offsetOf(e).dx
      expect(centerX + 32 / 2).toBeLessThanOrEqual(1280)
    }
    w.destroy()
  })

  test("圈间避让：跨圈不重叠（同角度斜向 45° 也不叠）", async () => {
    // 复现真实缺陷：径向 step = size + gap = 40px 只保证径向分开；相邻圈角度接近且方向偏 45° 时，
    // 两按钮的 x/y 坐标差会被压到 28px < 边长（实测重叠 3~5px）。
    // 修法：圈间步进取最坏情况下界 size·√2 + gap（同角度即天然分开），
    // 单个偶发拥挤由整圈滑动避让补救。本用例只盯跨圈（同圈拥挤是容量降级、另有用例覆盖）。
    const trigger = stub("button")
    trigger.rect = { left: 1244, top: 5, right: 1268, bottom: 29, width: 24, height: 24 }
    const els = Array.from({ length: 8 }, () => stub("button"))
    const w = createWheel({ trigger: asEl(trigger), items: els.map((e) => ({ el: asEl(e) })), firstRingR: 85 })
    await withViewport(1280, async () => {
      trigger.dispatchEvent({ type: "pointerenter" })
      await tick()
    })
    const cx = trigger.rect.left + trigger.rect.width / 2
    const cy = trigger.rect.top + trigger.rect.height / 2
    const pts = els.map((e) => ({ x: cx + offsetOf(e).dx, y: cy + offsetOf(e).dy, r: Math.round(offsetOf(e).radius) }))
    for (let i = 0; i < pts.length; i++) {
      for (let j = i + 1; j < pts.length; j++) {
        if (pts[i]!.r === pts[j]!.r) continue // 同圈：拥挤是容量降级（见“分圈”用例）
        const dx = Math.abs(pts[i]!.x - pts[j]!.x)
        const dy = Math.abs(pts[i]!.y - pts[j]!.y)
        // 方块边长 32：跨圈任意两项至少一轴差 ≥ 32 才算不重叠
        expect(Math.max(dx, dy)).toBeGreaterThanOrEqual(32 - 0.5)
      }
    }
    w.destroy()
  })
})
