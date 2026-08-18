import { describe, expect, test } from "bun:test"
import { createStickyScroll } from "./sticky-scroll"

/**
 * 粘底滚动（sticky-scroll.ts，jump-bottom 纯逻辑）单测：
 * 粘底锁定与「跳到最新」按钮显隐完全一致（同一 64px 阈值）：按钮隐藏 = 锁定跟随，
 * 按钮显示 = 用户阅读历史中不打扰。程序滚动与 scroll 事件送达之间存在时间窗，期间内容
 * 增长（工具调用卡片/长输出追加）会让迟到事件报告的位置「在目标却不在当前底部」，按
 * clamp 后的实际落位比对识别程序滚动——保持锁定并续滚到最新底部（`scrollTop = scrollHeight`
 * 会被钳制到 `scrollHeight - clientHeight`，须赋值后读回 scrollTop 记录，否则比对永不命中）。
 */

interface Listener {
  (): void
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
    emit(type: string) {
      for (const fn of listeners[type] ?? []) fn()
    },
  }
  return el as unknown as HTMLElement & { emit(type: string): void; scrollHeight: number; scrollTop: number }
}

function makeBtn() {
  return { hidden: true, onclick: null as (() => void) | null, click() { this.onclick?.() } }
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
    expect(el.scrollTop).toBe(1800) // 识别为程序滚动：续滚到最新底部，跟随未失效
    // 后续内容仍持续跟随
    el.scrollHeight = 2400
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(2200)
  })

  test("用户上滚阅读历史：按钮显示、内容增长不拽回底部；滚回底部自动恢复锁定", () => {
    const { el, btn, sticky } = setup()
    sticky.lockToBottom()
    el.scrollTop = 50
    el.emit("scroll") // 用户上滚（距底部 750px > 阈值）→ 解除锁定，按钮显示
    expect(btn.hidden).toBe(false)
    el.scrollHeight = 2000
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(50) // 不打扰阅读历史
    el.scrollTop = 1800 // 滚回底部
    el.emit("scroll") // → 恢复锁定，按钮隐藏
    expect(btn.hidden).toBe(true)
    el.scrollHeight = 2400
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(2200) // 恢复跟随
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

  test("跳到最新按钮：按 clamp 落位比对，动画期间内容增长终点过期仍续滚并重新锁定", () => {
    const { el, btn, sticky } = setup()
    sticky.lockToBottom()
    el.scrollTop = 50
    el.emit("scroll") // 用户已上滚（解除锁定）
    btn.click() // 滚动到旧底部 800
    expect(el.scrollTop).toBe(800)
    el.scrollHeight = 2000 // 动画期间内容增长（终点过期）
    el.emit("scroll") // 滚动终点事件
    expect(el.scrollTop).toBe(1800) // 终点不在当前底部 → 续滚并重新锁定
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

  test("用户上翻但 scroll 事件未送达（rAF 先行）：对齐保持循环按程序落位比对停转，不拽回底部", async () => {
    // 异步 rAF stub（keepTick 跨帧存活的拽底窗口）
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
      sticky.lockToBottom() // 落底 800 并启动 240 帧对齐保持循环
      expect(el.scrollTop).toBe(800)
      el.scrollTop = 500 // 用户上翻，scroll 事件未送达（不 emit）——旧 following 仍为 true
      await Bun.sleep(60) // 数个 keepTick 帧窗口
      expect(el.scrollTop).toBe(500) // 不拽回底部
      expect(btn.hidden).toBe(false) // 按位置解除锁定，按钮显示
      // 迟到的 scroll 事件到达：按位置维持同一结论（阅读历史，内容增长不打扰）
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
    el.scrollTop = 500 // 用户上翻，scroll 事件未送达（不 emit）
    el.scrollHeight = 2000 // 内容增长触发跟随（rAF 同步执行）
    sticky.scrollIfSticky()
    expect(el.scrollTop).toBe(500) // rAF 先行：按程序落位比对识别用户上翻，不拽回
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
