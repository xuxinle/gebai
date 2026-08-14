import { describe, expect, test } from "bun:test"
import { scrollReasoningSticky } from "./reasoning-scroll"

/** 最小滚动容器 fake：模拟浏览器 scrollTop 的 clamp（上限 scrollHeight - clientHeight）与事件监听。 */
function makeBody(init: { scrollHeight: number; clientHeight: number; isConnected?: boolean }) {
  const listeners: Record<string, Array<() => void>> = {}
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
    addEventListener(type: string, fn: () => void) {
      ;(listeners[type] ??= []).push(fn)
    },
    emit(type: string) {
      for (const fn of listeners[type] ?? []) fn()
    },
  }
  return body as unknown as HTMLElement & { emit(type: string): void; scrollHeight: number; scrollTop: number }
}

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
    scrollReasoningSticky(body) // 首次调用绑定监听
    body.scrollTop = 50
    body.emit("scroll") // 用户上滚 → 解除跟随
    body.scrollHeight = 800
    scrollReasoningSticky(body)
    expect(body.scrollTop).toBe(50) // 保持用户位置
  })

  test("用户滚回底部自动恢复跟随", () => {
    const body = makeBody({ scrollHeight: 500, clientHeight: 200 })
    scrollReasoningSticky(body)
    body.scrollTop = 50
    body.emit("scroll") // 上滚解除
    body.scrollTop = 300
    body.emit("scroll") // 滚回底部 → 恢复跟随
    body.scrollHeight = 800
    scrollReasoningSticky(body)
    expect(body.scrollTop).toBe(600)
  })

  test("程序滚动迟到事件（内容已增长、位置过期）仍继续跟随到底", () => {
    const body = makeBody({ scrollHeight: 500, clientHeight: 200 })
    scrollReasoningSticky(body) // 程序滚动：scrollTop=300，目标 300
    expect(body.scrollTop).toBe(300)
    body.scrollHeight = 800 // 内容在 scroll 事件送达前已增长
    body.emit("scroll") // 迟到的程序滚动事件
    expect(body.scrollTop).toBe(600) // 继续滚到最新底部，跟随未失效
  })

  test("脱离文档（isConnected=false）时不动", () => {
    const body = makeBody({ scrollHeight: 500, clientHeight: 200, isConnected: false })
    scrollReasoningSticky(body)
    expect(body.scrollTop).toBe(0)
  })
})
