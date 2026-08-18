import { describe, expect, test } from "bun:test"
import { scrollReasoningSticky } from "./reasoning-scroll"

/** 最小滚动容器 fake：模拟浏览器 scrollTop 的 clamp（上限 scrollHeight - clientHeight）与事件监听。 */
function makeBody(init: { scrollHeight: number; clientHeight: number; isConnected?: boolean }) {
  const listeners: Record<string, Array<(ev?: unknown) => void>> = {}
  let scrollHeight = init.scrollHeight
  let scrollTop = 0
  const body = {
    clientHeight: init.clientHeight,
    isConnected: init.isConnected ?? true,
    get scrollTop() {
      return scrollTop
    },
    set scrollTop(v: number) {
      scrollTop = Math.max(0, Math.min(v, scrollHeight - body.clientHeight))
    },
    get scrollHeight() {
      return scrollHeight
    },
    set scrollHeight(v: number) {
      scrollHeight = v
    },
    addEventListener(type: string, fn: (ev?: unknown) => void) {
      ;(listeners[type] ??= []).push(fn)
    },
    emit(type: string, ev?: unknown) {
      for (const fn of listeners[type] ?? []) fn(ev)
    },
  }
  return body as unknown as HTMLElement & { emit(type: string, ev?: unknown): void; scrollHeight: number; scrollTop: number }
}

// rAF 同步执行（跟随落底经 rAF 节流）
;(globalThis as Record<string, unknown>).requestAnimationFrame = (cb: () => void) => {
  cb()
  return 0
}
;(globalThis as Record<string, unknown>).cancelAnimationFrame = () => {}

describe("scrollReasoningSticky", () => {
  test("内容未超出高度时不滚动（无溢出）", () => {
    const body = makeBody({ scrollHeight: 150, clientHeight: 200 })
    scrollReasoningSticky(body)
    expect(body.scrollTop).toBe(0)
  })

  test("流式增长自动贴底", () => {
    const body = makeBody({ scrollHeight: 500, clientHeight: 200 })
    scrollReasoningSticky(body)
    expect(body.scrollTop).toBe(300)
    // 内容继续增长：仍贴底跟随
    body.scrollHeight = 800
    scrollReasoningSticky(body)
    expect(body.scrollTop).toBe(600)
  })

  test("用户上滚翻阅历史后不再被拽回底部", () => {
    const body = makeBody({ scrollHeight: 500, clientHeight: 200 })
    scrollReasoningSticky(body) // 首次调用绑定跟随核心
    body.emit("wheel", { deltaY: -3 }) // 用户上滚：输入事件先于滚动效果
    body.scrollTop = 50
    body.emit("scroll")
    body.scrollHeight = 800
    scrollReasoningSticky(body)
    expect(body.scrollTop).toBe(50) // 保持用户位置
  })

  test("用户滚回底部自动恢复跟随", () => {
    const body = makeBody({ scrollHeight: 500, clientHeight: 200 })
    scrollReasoningSticky(body)
    body.emit("wheel", { deltaY: -3 })
    body.scrollTop = 50
    body.emit("scroll")
    body.scrollTop = 300
    body.emit("scroll") // 滚回底部 → 恢复跟随
    body.scrollHeight = 800
    scrollReasoningSticky(body)
    expect(body.scrollTop).toBe(600)
  })

  test("程序滚动迟到事件（内容已增长、位置过期）仍继续跟随到底", () => {
    const body = makeBody({ scrollHeight: 500, clientHeight: 200 })
    scrollReasoningSticky(body) // 程序滚动：scrollTop=300
    expect(body.scrollTop).toBe(300)
    body.scrollHeight = 800 // 内容在 scroll 事件送达前已增长
    body.emit("scroll") // 迟到的程序滚动事件
    expect(body.scrollTop).toBe(600) // 静默窗口内归因为内部动作：续滚到最新底部，跟随未失效
  })

  test("内容收缩钳制（markdown 重解析高度跳变）：贴底钳制保持跟随、回长续滚", () => {
    const body = makeBody({ scrollHeight: 500, clientHeight: 200 })
    scrollReasoningSticky(body)
    expect(body.scrollTop).toBe(300)
    body.scrollHeight = 280 // 收缩：浏览器钳制 scrollTop 到新底部
    body.scrollTop = 80
    body.emit("scroll")
    body.scrollHeight = 600 // 又增长（位置已离开底部）
    body.emit("scroll")
    expect(body.scrollTop).toBe(400) // 回正，跟随未失效（旧实现续滚不登记目标、此处静默死亡）
  })

  test("脱离文档（isConnected=false）时不动", () => {
    const body = makeBody({ scrollHeight: 500, clientHeight: 200, isConnected: false })
    scrollReasoningSticky(body)
    expect(body.scrollTop).toBe(0)
  })
})
