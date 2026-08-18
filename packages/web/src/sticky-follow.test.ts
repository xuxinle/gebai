import { describe, expect, test } from "bun:test"
import { createStickyFollow } from "./sticky-follow"

/**
 * 粘底跟随核心（sticky-follow.ts，意图驱动）单测：
 * 跟随状态只由三类信号翻转——用户输入意图（滚轮/触摸/键盘/滚动条，先于其滚动效果）、
 * 几何贴底（任何滚动事件落在阈值内）、静默窗口兜底（未贴底且无法归因为程序滚动/DOM 变化）。
 * 用户滚动必须先发对应输入事件（wheel 等）再改位置/发 scroll 事件——真实浏览器中输入
 * 事件必然先于其滚动效果，这正是意图驱动机制免于滚动事件取证误判的基础。
 */

interface FakeEl {
  clientHeight: number
  clientWidth: number
  offsetWidth: number
  scrollHeight: number
  scrollTop: number
  getBoundingClientRect(): { right: number }
  addEventListener(type: string, fn: (ev?: unknown) => void): void
  emit(type: string, ev?: unknown): void
}

/** 最小滚动容器 fake：scrollTop 按浏览器语义 clamp 到 scrollHeight - clientHeight。 */
function makeEl(init: { scrollHeight?: number; clientHeight?: number; offsetWidth?: number } = {}): FakeEl {
  const listeners = new Map<string, Array<(ev?: unknown) => void>>()
  const st = { h: init.scrollHeight ?? 1000, top: 0 }
  const el: FakeEl = {
    clientHeight: init.clientHeight ?? 200,
    clientWidth: 200,
    offsetWidth: init.offsetWidth ?? 200,
    get scrollHeight() {
      return st.h
    },
    set scrollHeight(v) {
      st.h = v
    },
    get scrollTop() {
      return st.top
    },
    set scrollTop(v) {
      st.top = Math.max(0, Math.min(v, st.h - el.clientHeight))
    },
    getBoundingClientRect: () => ({ right: 100 }),
    addEventListener(type, fn) {
      listeners.set(type, [...(listeners.get(type) ?? []), fn])
    },
    emit(type, ev) {
      for (const fn of listeners.get(type) ?? []) fn(ev)
    },
  }
  return el
}

/** 键盘监听目标 stub。 */
function makeKeyTarget() {
  const listeners = new Map<string, Array<(ev?: unknown) => void>>()
  return {
    addEventListener(type: string, fn: (ev?: unknown) => void) {
      listeners.set(type, [...(listeners.get(type) ?? []), fn])
    },
    emit(type: string, ev?: unknown) {
      for (const fn of listeners.get(type) ?? []) fn(ev)
    },
  }
}

// window stub（滚动条拖动的 pointerup/pointercancel 监听挂 window）；rAF 同步执行
const winListeners = new Map<string, Array<() => void>>()
;(globalThis as Record<string, unknown>).window = {
  addEventListener(type: string, fn: () => void) {
    winListeners.set(type, [...(winListeners.get(type) ?? []), fn])
  },
  removeEventListener() {},
}
function emitWindow(type: string) {
  for (const fn of winListeners.get(type) ?? []) fn()
}
;(globalThis as Record<string, unknown>).requestAnimationFrame = (cb: () => void) => {
  cb()
  return 0
}
;(globalThis as Record<string, unknown>).cancelAnimationFrame = () => {}

/** 构造实例：内容 1000、视口 200（最大滚动位置 800），时钟可推进。 */
function setup(init?: { scrollHeight?: number; clientHeight?: number; offsetWidth?: number }) {
  const el = makeEl(init)
  let t = 0
  const h = createStickyFollow(el as unknown as HTMLElement, { now: () => t })
  return { el, h, advance: (ms: number) => (t += ms) }
}

describe("sticky-follow 粘底跟随核心", () => {
  test("内容未超出高度时不滚动（无溢出）", () => {
    const { el, h } = setup({ scrollHeight: 150, clientHeight: 200 })
    h.follow()
    expect(el.scrollTop).toBe(0)
  })

  test("跟随中内容增长：rAF 节流落底", () => {
    const { el, h } = setup()
    h.follow()
    expect(el.scrollTop).toBe(800)
    el.scrollHeight = 2000
    h.contentChanged()
    expect(el.scrollTop).toBe(1800)
  })

  test("滚轮上滚即时解除；滚回底部恢复跟随", () => {
    const { el, h, advance } = setup()
    h.follow()
    advance(500)
    el.emit("wheel", { deltaY: -120 }) // 用户上滚：输入事件先于滚动效果
    expect(h.isFollowing()).toBe(false)
    el.scrollTop = 500
    el.emit("scroll")
    el.scrollHeight = 2000
    h.contentChanged()
    expect(el.scrollTop).toBe(500) // 不打扰阅读历史
    el.scrollTop = 1800 // 滚回最新底部
    el.emit("scroll")
    expect(h.isFollowing()).toBe(true)
    el.scrollHeight = 2400
    h.contentChanged()
    expect(el.scrollTop).toBe(2200) // 恢复跟随
  })

  test("迟到的程序滚动事件（内容已增长、位置离开底部）：保持跟随并回正", () => {
    const { el, h, advance } = setup()
    h.follow()
    expect(el.scrollTop).toBe(800)
    el.scrollHeight = 2000 // 程序滚动事件送达前内容增长（工具卡片追加场景）
    advance(16) // 下一帧事件送达
    el.emit("scroll")
    expect(el.scrollTop).toBe(1800) // 静默窗口内归因为内部动作：回正到底
    expect(h.isFollowing()).toBe(true)
  })

  test("内容收缩钳制（推理折叠/容器折叠场景）：跟随不死、后续增长继续跟随", () => {
    const { el, h, advance } = setup()
    h.follow()
    expect(el.scrollTop).toBe(800)
    // 收缩（如推理块自动折叠）：浏览器把 scrollTop 钳制到新底部
    el.scrollHeight = 600
    el.scrollTop = 400
    advance(16)
    el.emit("scroll") // 钳制事件：贴底 → 跟随保持
    expect(h.isFollowing()).toBe(true)
    // 收缩后内容又增长（位置已离开新底部）
    el.scrollHeight = 1000
    el.emit("scroll")
    expect(el.scrollTop).toBe(800) // 回正，跟随未失效
    el.scrollHeight = 1400
    h.contentChanged()
    expect(el.scrollTop).toBe(1200)
  })

  test("静默窗口外未贴底滚动（未知输入：中键自动滚动/查找定位）：兜底解除", () => {
    const { el, h, advance } = setup()
    h.follow()
    advance(500) // 距最近程序滚动/内容变化已超静默窗口
    el.scrollTop = 500
    el.emit("scroll")
    expect(h.isFollowing()).toBe(false)
    el.scrollHeight = 2000
    h.contentChanged()
    expect(el.scrollTop).toBe(500) // 不拽回
  })

  test("滚动条拖动：命中槽区后未贴底即时解除（静默窗口内亦然）", () => {
    const { el, h } = setup({ offsetWidth: 220 }) // 滚动条宽 = 220-200 = 20
    h.follow()
    expect(el.scrollTop).toBe(800)
    el.emit("pointerdown", { clientX: 90, button: 0 }) // 90 > 100-20-4：命中滚动条槽区
    el.scrollTop = 500
    el.emit("scroll") // 静默窗口内（距 follow <80ms）但拖动中：即时解除
    expect(h.isFollowing()).toBe(false)
    el.scrollHeight = 2000
    h.contentChanged()
    expect(el.scrollTop).toBe(500)
    emitWindow("pointerup") // 拖动结束
    el.scrollTop = 1800
    el.emit("scroll")
    expect(h.isFollowing()).toBe(true) // 拖回底部恢复跟随
  })

  test("触摸上滑解除跟随、下滑不解除", () => {
    const { el, h } = setup()
    h.follow()
    el.emit("touchstart", { touches: [{ clientY: 300 }] })
    el.emit("touchmove", { touches: [{ clientY: 320 }] }) // 手指下移 20px = 内容上滚
    expect(h.isFollowing()).toBe(false)
    h.follow()
    el.emit("touchstart", { touches: [{ clientY: 300 }] })
    el.emit("touchmove", { touches: [{ clientY: 288 }] }) // 手指上移 = 内容下滚
    expect(h.isFollowing()).toBe(true)
  })

  test("向上滚动键解除跟随；输入框内方向键与向下键不解除", () => {
    const el = makeEl()
    const keys = makeKeyTarget()
    let t = 0
    const h = createStickyFollow(el as unknown as HTMLElement, { keyTarget: keys as unknown as Window, now: () => t })
    h.follow()
    keys.emit("keydown", { key: "PageUp", defaultPrevented: false, target: { closest: () => null } })
    expect(h.isFollowing()).toBe(false)
    h.follow()
    keys.emit("keydown", { key: "ArrowUp", defaultPrevented: false, target: { closest: () => null } })
    expect(h.isFollowing()).toBe(false)
    h.follow()
    keys.emit("keydown", { key: "ArrowUp", defaultPrevented: false, target: { closest: () => ({}) } }) // 输入框内：滚动的是光标
    expect(h.isFollowing()).toBe(true)
    keys.emit("keydown", { key: "End", defaultPrevented: false, target: { closest: () => null } }) // 向下键由几何贴底恢复
    expect(h.isFollowing()).toBe(true)
  })

  test("restore 历史位置：按落位同步跟随；未决跟随回调不拽回", async () => {
    // 异步 rAF stub（未决回调跨帧存活的窗口）
    const origRaf = globalThis.requestAnimationFrame
    const timers = new Set<ReturnType<typeof setTimeout>>()
    ;(globalThis as Record<string, unknown>).requestAnimationFrame = (cb: () => void) => {
      const timer = setTimeout(() => cb(), 8)
      timers.add(timer)
      return 0
    }
    try {
      const el = makeEl()
      let t = 0
      const h = createStickyFollow(el as unknown as HTMLElement, { now: () => t })
      h.contentChanged() // loadMessages 尾部竞态：先排期跟随回调
      h.follow() // 再落底（启动对齐保持循环）
      h.restore(300) // 恢复历史位置
      await Bun.sleep(60)
      expect(el.scrollTop).toBe(300) // 未决回调按执行时状态失效，不拽到底部
      expect(h.isFollowing()).toBe(false)
      el.scrollHeight = 2000
      h.contentChanged()
      await Bun.sleep(30)
      expect(el.scrollTop).toBe(300) // 阅读历史不打扰
      h.restore(1800) // 恢复位置贴底（2000-1800-200=0）
      el.scrollHeight = 2400
      h.contentChanged()
      await Bun.sleep(30)
      expect(el.scrollTop).toBe(2200) // 贴底恢复：保持粘底跟随语义
    } finally {
      for (const timer of timers) clearTimeout(timer)
      ;(globalThis as Record<string, unknown>).requestAnimationFrame = origRaf
    }
  })

  test("无事件内容增长（content-visibility 异步高度修正）：对齐保持循环按帧续滚，预算耗尽自停", async () => {
    const origRaf = globalThis.requestAnimationFrame
    const timers = new Set<ReturnType<typeof setTimeout>>()
    ;(globalThis as Record<string, unknown>).requestAnimationFrame = (cb: () => void) => {
      const timer = setTimeout(() => cb(), 8)
      timers.add(timer)
      return 0
    }
    try {
      const el = makeEl()
      const h = createStickyFollow(el as unknown as HTMLElement, { keepFrames: 240 })
      h.follow()
      expect(el.scrollTop).toBe(800)
      el.scrollHeight = 1142 // 无 DOM 变化、无 scroll 事件的高度增长
      await Bun.sleep(40)
      expect(el.scrollTop).toBe(942) // 跟随循环自动续滚到新底部
      const before = el.scrollTop
      await Bun.sleep(40) // 修正收敛后预算耗尽，不再滚动
      expect(el.scrollTop).toBe(before)
    } finally {
      for (const timer of timers) clearTimeout(timer)
      ;(globalThis as Record<string, unknown>).requestAnimationFrame = origRaf
    }
  })

  test("stopFollowing：显式解除（消息导航跳转）", () => {
    const { el, h } = setup()
    h.follow()
    h.stopFollowing()
    el.scrollHeight = 2000
    h.contentChanged()
    expect(el.scrollTop).toBe(800) // 导航后不被内容增长拽走
  })

  test("非溢出容器滚轮不解除（无可滚动空间）", () => {
    const { el, h } = setup({ scrollHeight: 150, clientHeight: 200 })
    h.follow()
    el.emit("wheel", { deltaY: -120 })
    expect(h.isFollowing()).toBe(true)
  })
})
