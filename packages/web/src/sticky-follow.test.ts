import { afterAll, describe, expect, test } from "bun:test"
import { createStickyFollow } from "./sticky-follow"

/**
 * 粘底跟随（sticky-follow.ts）单测：跟随状态由意图识别核（scroll-intent.ts，判定规则见其
 * 单测）判定，本文件验证 DOM 层集成——位移触发判定、程序动作（pin/内容变化）的静默归因、
 * 显式输入（键盘/中键/滚动条拖动）即时表态、拖动结束按几何归位。
 *
 * 用户滚动在该机制下表现为「位置变化 + scroll 事件」（不再依赖 wheel/touch 事件类型），
 * 程序动作会先给 lastInternalAt 打时间戳，其迟到事件在静默窗口内不参与判定。
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
// 存档与还原：整体替换全局而不放回会泄漏给后续测试文件——基线那份 window = globalThis 带
// setTimeout 等真实能力，后续用例（如轮盘展开动画）靠它（见 scripts/test-preload.ts）
const winListeners = new Map<string, Array<() => void>>()
const prevWindow = (globalThis as Record<string, unknown>).window
;(globalThis as Record<string, unknown>).window = {
  addEventListener(type: string, fn: () => void) {
    winListeners.set(type, [...(winListeners.get(type) ?? []), fn])
  },
  removeEventListener() {},
}
afterAll(() => {
  ;(globalThis as Record<string, unknown>).window = prevWindow
})
function emitWindow(type: string) {
  for (const fn of winListeners.get(type) ?? []) fn()
}
;(globalThis as Record<string, unknown>).requestAnimationFrame = (cb: () => void) => {
  cb()
  return 0
}
;(globalThis as Record<string, unknown>).cancelAnimationFrame = () => {}

/** 构造实例：内容 1000、视口 200（最大滚动位置 800），时钟与确认定时器均可控。 */
function setup(init?: { scrollHeight?: number; clientHeight?: number; offsetWidth?: number }) {
  const el = makeEl(init)
  let t = 0
  const timers = new Map<number, () => void>()
  let seq = 0
  const h = createStickyFollow(el as unknown as HTMLElement, {
    now: () => t,
    intentOptions: {
      setTimer: (fn) => {
        const id = ++seq
        timers.set(id, fn)
        return id
      },
      clearTimer: (handle) => {
        timers.delete(handle as number)
      },
    },
  })
  return {
    el,
    h,
    advance: (ms: number) => (t += ms),
    /** 确认时长到达（离开底部的确认定时器）。 */
    fireTimers: () => {
      const fns = [...timers.values()]
      timers.clear()
      for (const fn of fns) fn()
    },
    /** 用户滚动到某位置（位置变化 + scroll 事件；浏览器语义由 fake 的 clamp 保证）。 */
    scrollTo(top: number) {
      el.scrollTop = top
      el.emit("scroll")
    },
  }
}

describe("sticky-follow 粘底跟随（意图识别）", () => {
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

  test("微调（容忍区内上翻）后内容增长：手势窗内不抢位置，窗后仍保留用户的距底位移", () => {
    const { el, h, advance, scrollTo } = setup()
    h.follow()
    advance(500)
    scrollTo(760) // 微调上翻 40px（容忍区内：不解除跟随）
    expect(h.isFollowing()).toBe(true)
    el.scrollHeight = 1400 // 内容增长（+400）：手势窗内 → 不抢位置（尊重刚发生的动作）
    h.contentChanged()
    expect(el.scrollTop).toBe(760)
    advance(400) // 手势窗过去：内容变化照常钉底，但钉的是「底部 − 用户的距底位移」
    el.scrollHeight = 1800
    h.contentChanged()
    expect(el.scrollTop).toBe(1560)
  })

  test("容忍区内上翻后内容增长：保留用户的距底位移，不被弹回底部（滚不出去的回归）", () => {
    const { el, h, advance, scrollTo } = setup()
    h.follow()
    advance(500)
    scrollTo(760) // 上翻 40px（距底 40 ≤ 阈值 64：仍在容忍区内，不解除跟随）
    expect(h.isFollowing()).toBe(true)
    advance(400) // 手势窗过去：此后内容变化照常钉底
    el.scrollHeight = 1400 // 图表/图片异步长高
    h.contentChanged()
    expect(el.scrollTop).toBe(1160) // 保留用户的 40px（钉底钉距离，不写死到底）
    el.scrollHeight = 1800
    h.contentChanged()
    expect(el.scrollTop).toBe(1560)
    el.emit("wheel", { deltaY: 1 }) // 用户向下滚回（清掉静默窗口，与真实输入一致）
    scrollTo(1600) // 滚回真正到底（高度 1800 − 视口 200，距底 0）
    el.scrollHeight = 2000
    h.contentChanged()
    expect(el.scrollTop).toBe(1800) // 到底后恢复绝对钉底（offset 归 0）
  })

  test("窗口化回正落点的判据（pinTarget）：跟随态给落点（含距底位移），非跟随态为 null", () => {
    const { h, advance, scrollTo, fireTimers } = setup()
    h.follow()
    expect(h.pinTarget()).toBe(800) // 已在底部：落点即绝对底部
    advance(500)
    scrollTo(760) // 容忍区内上翻 40px（不解除跟随）
    expect(h.pinTarget()).toBe(760) // 落点保留用户的 40px（窗口化写它就不抹掉微调）
    scrollTo(500) // 明确上翻 → 阅读态
    fireTimers()
    expect(h.pinTarget()).toBeNull() // 不在追最新：窗口化不得回正
    scrollTo(800) // 滚回底部恢复跟随
    expect(h.pinTarget()).toBe(800)
  })

  test("明确上翻解除跟随（位移判定）；滚回底部恢复跟随", () => {
    const { el, h, advance, scrollTo, fireTimers } = setup()
    h.follow()
    advance(500)
    scrollTo(500) // 上翻 300px
    fireTimers() // 确认时长到达 → 解除
    expect(h.isFollowing()).toBe(false)
    el.scrollHeight = 2000
    h.contentChanged()
    expect(el.scrollTop).toBe(500) // 不打扰阅读历史
    scrollTo(1800) // 滚回最新底部（向下滚到底）
    expect(h.isFollowing()).toBe(true)
    el.scrollHeight = 2400
    h.contentChanged()
    expect(el.scrollTop).toBe(2200) // 恢复跟随
  })

  test("底部微调（位置仍在阈值内）不解除跟随", () => {
    const { h, advance, scrollTo, fireTimers } = setup()
    h.follow()
    advance(500)
    scrollTo(760) // 距底 40 < 64：仍在底部
    expect(h.isFollowing()).toBe(true)
    scrollTo(730) // 距底 70 > 64：已离开底部
    expect(h.isFollowing()).toBe(true) // 确认期未到
    scrollTo(750) // 回到底部 → 确认取消
    fireTimers()
    expect(h.isFollowing()).toBe(true)
  })

  test("程序落底后的迟到滚动事件（内容已增长、位置离开底部）：保持跟随", () => {
    const { el, h, advance } = setup()
    h.follow()
    expect(el.scrollTop).toBe(800)
    el.scrollHeight = 2000 // 程序滚动事件送达前内容增长（工具卡片追加场景）
    advance(16) // 下一帧事件送达（仍在静默窗口内）
    el.emit("scroll")
    expect(h.isFollowing()).toBe(true) // 归因内部动作，跟随未失效
    h.contentChanged()
    expect(el.scrollTop).toBe(1800) // 内容变化路径负责回正到底
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
    expect(h.isFollowing()).toBe(true) // 迟到的钳制事件未把它判成用户上翻
    h.contentChanged()
    expect(el.scrollTop).toBe(800) // 回正，跟随未失效
    el.scrollHeight = 1400
    h.contentChanged()
    expect(el.scrollTop).toBe(1200)
  })

  test("内容收缩把内容变短（位置自然落进阈值）不恢复跟随：不在追最新时就不跟", () => {
    const { el, h, advance, scrollTo, fireTimers } = setup()
    h.follow()
    advance(500)
    scrollTo(500) // 明确上翻 → 阅读态
    fireTimers()
    expect(h.isFollowing()).toBe(false)
    el.scrollHeight = 700 // 内容收缩：位置自然落进阈值内（非用户动作）
    el.emit("scroll")
    expect(h.isFollowing()).toBe(false) // 不被拽回底部
  })

  test("未知输入（中键自动滚动）：位移不被回正拽底", () => {
    const { el, h, advance, scrollTo, fireTimers } = setup()
    h.follow()
    advance(500)
    el.emit("pointerdown", { button: 1, clientX: 10 }) // 已在底部：按中键本身不改变状态
    expect(h.isFollowing()).toBe(true)
    scrollTo(500) // 自动滚动位移（上滚 300px）
    fireTimers()
    expect(h.isFollowing()).toBe(false)
    el.scrollHeight = 2000
    h.contentChanged()
    expect(el.scrollTop).toBe(500) // 不被拽回
  })

  test("滚动条拖动：拖动期间可解除；拖回底部结束拖动后恢复跟随", () => {
    const { el, h, advance, scrollTo, fireTimers } = setup({ offsetWidth: 220 }) // 滚动条宽 = 220-200 = 20
    h.follow()
    expect(el.scrollTop).toBe(800)
    advance(500)
    el.emit("pointerdown", { clientX: 90, button: 0 }) // 90 > 100-20-4：命中滚动条槽区
    scrollTo(500) // 拖动位移照常判定（允许解除）
    fireTimers()
    expect(h.isFollowing()).toBe(false)
    el.scrollHeight = 2000
    h.contentChanged()
    expect(el.scrollTop).toBe(500)
    scrollTo(1800)
    expect(h.isFollowing()).toBe(false) // 拖动中不允许「向下滚到底恢复」（拖到阈值区内不被拽到底）
    emitWindow("pointerup") // 拖动结束：按几何归位
    expect(h.isFollowing()).toBe(true) // 已在底部 → 跟随
    el.scrollHeight = 2400
    h.contentChanged()
    expect(el.scrollTop).toBe(2200)
  })

  test("触摸上滑（内容上滚）解除跟随、下滑（内容下滚）不解除", () => {
    const { h, advance, scrollTo, fireTimers } = setup()
    h.follow()
    advance(500)
    scrollTo(500) // 上滑 300px
    fireTimers()
    expect(h.isFollowing()).toBe(false)
    h.follow()
    advance(500)
    scrollTo(800) // 已在底部再下滑：无位移、无解除
    fireTimers()
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
    expect(h.state()).toBe("reading")
    h.follow()
    keys.emit("keydown", { key: "ArrowUp", defaultPrevented: false, target: { closest: () => null } })
    expect(h.isFollowing()).toBe(false)
    h.follow()
    keys.emit("keydown", { key: "ArrowUp", defaultPrevented: false, target: { closest: () => ({}) } }) // 输入框内：滚动的是光标
    expect(h.isFollowing()).toBe(true)
    keys.emit("keydown", { key: "End", defaultPrevented: false, target: { closest: () => null } }) // 向下键不解除
    expect(h.isFollowing()).toBe(true)
  })

  test("restore 历史位置：按落位同步状态；未决跟随回调不拽回", async () => {
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

  test("无事件内容增长（异步高度修正）：对齐保持循环按帧续滚，预算耗尽自停", async () => {
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
    expect(h.state()).toBe("reading")
    el.scrollHeight = 2000
    h.contentChanged()
    expect(el.scrollTop).toBe(800) // 导航后不被内容增长拽走
  })

  test("非溢出容器：滚动事件不解除（无可滚动空间）", () => {
    const { el, h, scrollTo } = setup({ scrollHeight: 150, clientHeight: 200 })
    h.follow()
    scrollTo(0)
    expect(h.isFollowing()).toBe(true)
    expect(el.scrollHeight).toBe(150)
  })

  test("三态归属：底部附近停住 = hold，远离 = reading", () => {
    const { h, advance, scrollTo, fireTimers } = setup()
    h.follow()
    advance(500)
    scrollTo(750) // 距底 50：仍在底部
    expect(h.state()).toBe("follow")
    scrollTo(500) // 距底 300（≤ 64 + 200*1.5 = 364）
    fireTimers()
    expect(h.state()).toBe("hold")
    h.follow()
    advance(500)
    scrollTo(200) // 距底 600 > 364
    fireTimers()
    expect(h.state()).toBe("reading")
  })
})
