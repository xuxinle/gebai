import { describe, expect, test } from "bun:test"
import { createStickyScroll } from "./sticky-scroll"

/**
 * 粘底滚动（sticky-scroll.ts，jump-bottom 纯逻辑）单测：
 * 按钮显隐 = 几何贴底（同一 64px 阈值），跟随切换机制见 sticky-follow.ts（意图驱动）——
 * 用户滚动必须先发对应输入事件（wheel 等）再改位置/发 scroll 事件：真实浏览器中输入事件
 * 必然先于其滚动效果。程序滚动与 scroll 事件送达之间存在时间窗，期间内容增长/收缩产生的
 * 钳制与迟到事件由静默窗口归因为内部动作（保持跟随并回正），不误判为用户滚动。
 */

interface Listener {
  (ev?: unknown): void
}

/** 最小滚动容器 fake：模拟浏览器 scrollTop 的 clamp（上限 scrollHeight - clientHeight）与事件监听。 */
function makeEl(init: { scrollHeight: number; clientHeight: number }) {
  const listeners: Record<string, Listener[]> = {}
  let scrollHeight = init.scrollHeight
  let scrollTop = 0
  const el = {
    clientHeight: init.clientHeight,
    get scrollHeight() {
      return scrollHeight
    },
    set scrollHeight(v: number) {
      scrollHeight = v
    },
    get scrollTop() {
      return scrollTop
    },
    set scrollTop(v: number) {
      scrollTop = Math.max(0, Math.min(v, scrollHeight - el.clientHeight))
    },
    addEventListener(type: string, fn: Listener) {
      ;(listeners[type] ??= []).push(fn)
    },
    scrollTo(opts: { top: number }) {
      scrollTop = Math.max(0, Math.min(opts.top, scrollHeight - el.clientHeight))
    },
    emit(type: string, ev?: unknown) {
      for (const fn of listeners[type] ?? []) fn(ev)
    },
  }
  return el as unknown as HTMLElement & { emit(type: string, ev?: unknown): void; scrollHeight: number; scrollTop: number }
}

function makeBtn() {
  return { hidden: true, onclick: null as (() => void) | null, click() { this.onclick?.() } }
}

/** 用户上滚：wheel 输入事件（先于滚动效果）+ 位置变化 + scroll 事件。 */
function userScrollUp(el: ReturnType<typeof makeEl>, top: number) {
  el.emit("wheel", { deltaY: -120 })
  el.scrollTop = top
  el.emit("scroll")
}

// RAF 同步执行（测试无需真实帧循环）；MutationObserver 仅注册
;(globalThis as Record<string, unknown>).requestAnimationFrame = (cb: () => void) => {
  cb()
  return 0
}
;(globalThis as Record<string, unknown>).cancelAnimationFrame = () => {}
;(globalThis as Record<string, unknown>).MutationObserver = class {
  observe() {}
  disconnect() {}
}

/** 构造实例：内容 1000、视口 200，最大滚动位置 800。 */
function setup() {
  const el = makeEl({ scrollHeight: 1000, clientHeight: 200 })
  const btn = makeBtn()
  return { el, btn, sticky: createStickyScroll(el as unknown as HTMLElement, btn as unknown as HTMLElement) }
}

describe("sticky-scroll 粘底滚动", () => {
  test("内容未超出高度时不滚动（无溢出）", () => {
    const el = makeEl({ scrollHeight: 150, clientHeight: 200 })
    const sticky = createStickyScroll(el as unknown as HTMLElement, makeBtn() as unknown as HTMLElement)
    sticky.lockToBottom()
    expect(el.scrollTop).toBe(0)
  })

  test("程序滚动与 scroll 事件送达之间内容增长（工具卡片场景）：迟到事件不解除锁定，续滚到最新底部", () => {
    const { el, sticky } = setup()
    sticky.lockToBottom() // 粘底落底
    expect(el.scrollTop).toBe(800)
    // 工具调用卡片追加：内容在程序滚动事件送达前增长 → 迟到事件位置=旧底部、已不在当前底部
    el.scrollHeight = 2000
    el.emit("scroll")
    expect(el.scrollTop).toBe(1800) // 静默窗口内归因为内部动作：续滚到最新底部，跟随未失效
    // 后续内容仍持续跟随
    el.scrollHeight = 2400
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(2200)
  })

  test("用户上滚阅读历史：按钮显示、内容增长不拽回底部；滚回底部自动恢复锁定", () => {
    const { el, btn, sticky } = setup()
    sticky.lockToBottom()
    userScrollUp(el, 50)
    expect(btn.hidden).toBe(false)
    el.scrollHeight = 2000
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(50) // 不打扰阅读历史
    el.scrollTop = 1800 // 滚回底部
    el.emit("scroll")
    expect(btn.hidden).toBe(true)
    el.scrollHeight = 2400
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(2200) // 恢复跟随
  })

  test("内容收缩钳制（推理折叠/容器折叠）：贴底钳制事件保持锁定，收缩后增长继续跟随", () => {
    const { el, btn, sticky } = setup()
    sticky.lockToBottom()
    expect(el.scrollTop).toBe(800)
    el.scrollHeight = 600 // 内容收缩：浏览器钳制 scrollTop 到新底部
    el.scrollTop = 400
    el.emit("scroll")
    expect(btn.hidden).toBe(true) // 贴底钳制：锁定保持、按钮隐藏
    el.scrollHeight = 1000 // 收缩后内容又增长（位置已离开底部）
    el.emit("scroll")
    expect(el.scrollTop).toBe(800) // 回正到底，跟随未失效（旧实现此处静默死亡）
    el.scrollHeight = 1400
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(1200)
  })

  test("小幅上滚（阈值内，按钮不显示）仍视为锁定：内容增长继续跟随到底", () => {
    const { el, btn, sticky } = setup()
    sticky.lockToBottom()
    expect(el.scrollTop).toBe(800)
    el.scrollTop = 760 // 距底部 40px <= 64px：按钮保持隐藏
    el.emit("scroll")
    expect(btn.hidden).toBe(true)
    el.scrollHeight = 2000
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(1800) // 锁定未解除，拽回最新底部
  })

  test("跳到最新按钮：点击落底重新锁定，事件迟到期间内容增长仍续滚", () => {
    const { el, btn, sticky } = setup()
    sticky.lockToBottom()
    userScrollUp(el, 50) // 用户已上滚（解除锁定）
    btn.click() // 滚动到底部 800
    expect(el.scrollTop).toBe(800)
    el.scrollHeight = 2000 // 事件送达前内容增长（终点过期）
    el.emit("scroll") // 滚动终点事件
    expect(el.scrollTop).toBe(1800) // 终点不在当前底部 → 续滚并保持锁定
    el.scrollHeight = 2400
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(2200) // 锁定保持，持续跟随
  })

  test("无增长时程序滚动事件到达：保持锁定，不做多余滚动", () => {
    const { el, sticky } = setup()
    sticky.lockToBottom()
    expect(el.scrollTop).toBe(800)
    el.emit("scroll") // 内容无变化，程序滚动事件正常到达
    expect(el.scrollTop).toBe(800) // 位置即当前底部，无需续滚
    el.scrollHeight = 1400
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(1200) // 锁定保持，继续跟随
  })

  test("无事件内容增长（content-visibility 异步高度修正）：跟随帧内自动续滚对齐，最新消息不被切出视口", async () => {
    // 真实浏览器中 rAF 按帧推进，粘底对齐保持循环跨帧存活——用异步 rAF stub 模拟
    const origRaf = globalThis.requestAnimationFrame
    let rafId = 0
    const timers = new Set<ReturnType<typeof setTimeout>>()
    ;(globalThis as Record<string, unknown>).requestAnimationFrame = (cb: (t: number) => void) => {
      const id = rafId++
      const t = setTimeout(() => cb(performance.now()), 8)
      timers.add(t)
      return id
    }
    ;(globalThis as Record<string, unknown>).cancelAnimationFrame = () => {}
    try {
      const el = makeEl({ scrollHeight: 1000, clientHeight: 200 })
      const sticky = createStickyScroll(el as unknown as HTMLElement, makeBtn() as unknown as HTMLElement)
      sticky.lockToBottom()
      expect(el.scrollTop).toBe(800)
      // 模拟 content-visibility 懒布局修正：无 DOM 变化、无 scroll 事件的内容高度增长
      el.scrollHeight = 1142
      await Bun.sleep(40) // 数个跟随帧窗口
      expect(el.scrollTop).toBe(942) // 跟随循环自动续滚到新底部，最新消息不被切在视口外
      // 修正收敛后不再滚动（不产生多余滚动事件）
      const before = el.scrollTop
      await Bun.sleep(40)
      expect(el.scrollTop).toBe(before)
    } finally {
      for (const t of timers) clearTimeout(t)
      ;(globalThis as Record<string, unknown>).requestAnimationFrame = origRaf
    }
  })

  test("用户上翻但 scroll 事件未送达（rAF 先行）：对齐保持循环停转，不拽回底部", async () => {
    // 异步 rAF stub（keepTick 跨帧存活的拽底窗口）
    const origRaf = globalThis.requestAnimationFrame
    const timers = new Set<ReturnType<typeof setTimeout>>()
    ;(globalThis as Record<string, unknown>).requestAnimationFrame = (cb: () => void) => {
      const t = setTimeout(() => cb(), 8)
      timers.add(t)
      return 0
    }
    ;(globalThis as Record<string, unknown>).cancelAnimationFrame = () => {}
    try {
      const el = makeEl({ scrollHeight: 1000, clientHeight: 200 })
      const btn = makeBtn()
      const sticky = createStickyScroll(el as unknown as HTMLElement, btn as unknown as HTMLElement)
      sticky.lockToBottom() // 落底 800 并启动 240 帧对齐保持循环
      expect(el.scrollTop).toBe(800)
      // 用户上翻：wheel 输入事件先于滚动效果送达（scroll 事件可能滞后）
      el.emit("wheel", { deltaY: -120 })
      el.scrollTop = 500
      await Bun.sleep(60) // 数个 keepTick 帧窗口
      expect(el.scrollTop).toBe(500) // 不拽回底部
      expect(btn.hidden).toBe(false) // 按钮显示
      // 迟到的 scroll 事件到达：维持同一结论（阅读历史，内容增长不打扰）
      el.emit("scroll")
      el.scrollHeight = 2000
      sticky.scrollIfSticky()
      expect(el.scrollTop).toBe(500)
    } finally {
      for (const t of timers) clearTimeout(t)
      ;(globalThis as Record<string, unknown>).requestAnimationFrame = origRaf
    }
  })

  test("生成中用户上翻（事件未送达）后内容增长：scrollIfSticky 的 rAF 回调先行不拽回", () => {
    const { el, sticky } = setup()
    sticky.lockToBottom()
    expect(el.scrollTop).toBe(800)
    el.emit("wheel", { deltaY: -120 }) // 用户上翻意图（scroll 事件未送达）
    el.scrollTop = 500
    el.scrollHeight = 2000 // 内容增长触发跟随（rAF 同步执行）
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(500) // rAF 先行：执行时跟随已解除，不拽回
  })

  test("stopFollowing（消息导航跳转）：显式解除后内容增长不拽走", () => {
    const { el, sticky } = setup()
    sticky.lockToBottom()
    sticky.stopFollowing()
    el.scrollHeight = 2000
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(800)
  })

  test("restoreScroll：恢复历史位置同步锁定状态，未决跟随回调不把位置拽到底（loadMessages 尾部竞态）", async () => {
    const origRaf = globalThis.requestAnimationFrame
    const timers = new Set<ReturnType<typeof setTimeout>>()
    ;(globalThis as Record<string, unknown>).requestAnimationFrame = (cb: (t: number) => void) => {
      const t = setTimeout(() => cb(performance.now()), 8)
      timers.add(t)
      return 0
    }
    ;(globalThis as Record<string, unknown>).cancelAnimationFrame = () => {}
    try {
      const el = makeEl({ scrollHeight: 1000, clientHeight: 200 })
      const btn = makeBtn()
      const sticky = createStickyScroll(el as unknown as HTMLElement, btn as unknown as HTMLElement)
      // loadMessages 尾部时序：flushMsgBatch 排期 scrollIfSticky rAF → lockToBottom（启动保持循环）→ 恢复 scrollTop=mem
      sticky.scrollIfSticky()
      sticky.lockToBottom()
      sticky.restoreScroll(300)
      await Bun.sleep(60) // 未决 rAF 与对齐保持循环的执行窗口
      expect(el.scrollTop).toBe(300) // 不被拽到底部
      expect(btn.hidden).toBe(false) // 按钮显示（阅读历史）
      el.scrollHeight = 2000 // 后续内容增长不打扰阅读
      sticky.scrollIfSticky()
      await Bun.sleep(30)
      expect(el.scrollTop).toBe(300)
      // 恢复位置在底部阈值内：保持粘底跟随语义
      sticky.restoreScroll(1800) // 2000-1800-200=0，贴底
      el.scrollHeight = 2400
      sticky.scrollIfSticky()
      await Bun.sleep(30)
      expect(el.scrollTop).toBe(2200)
    } finally {
      for (const t of timers) clearTimeout(t)
      ;(globalThis as Record<string, unknown>).requestAnimationFrame = origRaf
    }
  })
})
