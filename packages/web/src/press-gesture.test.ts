import { describe, expect, test } from "bun:test"
import { createPressGesture } from "./press-gesture"

/**
 * 按下手势状态机单测（msg-nav 短横线搓动用）：
 * 背景——短横线 ::before 扩展命中区在导航列右侧连成连续竖带，旧实现 pointerdown 即跳 + 立即
 * setPointerCapture：指针扫过右缘误触一下就跳消息（偏上位置=跳顶）且 capture 劫持后续拖动。
 * 现约束为「位移超阈值才拖动、未拖动松开才算点击、系统取消不触发点击」。
 */

function recorder() {
  const calls: string[] = []
  return {
    calls,
    opts: {
      onDragStart: (p: { x: number; y: number }) => calls.push(`start(${p.x},${p.y})`),
      onDrag: (p: { x: number; y: number }) => calls.push(`drag(${p.x},${p.y})`),
      onClick: (p: { x: number; y: number }) => calls.push(`click(${p.x},${p.y})`),
    },
  }
}

describe("createPressGesture", () => {
  test("按下未拖动即松开 = 明确点击：onClick 一次，无 onDragStart/onDrag", () => {
    const r = recorder()
    const g = createPressGesture(r.opts)
    g.down({ x: 100, y: 200 })
    g.move({ x: 102, y: 203 }) // 阈值内微动（3px < slop 6）
    g.move({ x: 101, y: 201 })
    g.up({ x: 101, y: 201 })
    expect(r.calls).toEqual(["click(101,201)"])
    expect(g.isDragging()).toBe(false)
  })

  test("位移超过阈值才进入拖动：onDragStart（按下点）+ 首 move 的 onDrag，松开不触发点击", () => {
    const r = recorder()
    const g = createPressGesture(r.opts)
    g.down({ x: 100, y: 200 })
    g.move({ x: 100, y: 206 }) // 6px = slop，不大于不算（严格大于判定）
    expect(r.calls).toEqual([])
    g.move({ x: 100, y: 210 }) // 10px > 6：进入拖动
    expect(r.calls).toEqual(["start(100,200)", "drag(100,210)"])
    expect(g.isDragging()).toBe(true)
    g.move({ x: 100, y: 300 })
    g.up({ x: 100, y: 300 })
    expect(r.calls).toEqual(["start(100,200)", "drag(100,210)", "drag(100,300)"])
    expect(g.isDragging()).toBe(false)
  })

  test("系统取消（pointercancel）：只复位，不触发点击；后续 move/up 无副作用", () => {
    const r = recorder()
    const g = createPressGesture(r.opts)
    g.down({ x: 50, y: 50 })
    g.cancel()
    g.move({ x: 200, y: 200 })
    g.up({ x: 200, y: 200 })
    expect(r.calls).toEqual([])
    // 取消后状态已复位：下一次按下-松开是新的点击
    g.down({ x: 10, y: 10 })
    g.up({ x: 12, y: 12 })
    expect(r.calls).toEqual(["click(12,12)"])
  })

  test("未按下时 move/up 不触发任何回调", () => {
    const r = recorder()
    const g = createPressGesture(r.opts)
    g.move({ x: 1, y: 1 })
    g.up({ x: 1, y: 1 })
    expect(r.calls).toEqual([])
  })

  test("拖动结束后再次按下：重新按点击/拖动判定，不残留拖动态", () => {
    const r = recorder()
    const g = createPressGesture(r.opts)
    g.down({ x: 0, y: 0 })
    g.move({ x: 100, y: 0 }) // 拖动
    g.up({ x: 100, y: 0 })
    g.down({ x: 0, y: 0 })
    g.up({ x: 1, y: 1 }) // 新按压周期内未超阈值 = 点击
    expect(r.calls).toEqual(["start(0,0)", "drag(100,0)", "click(1,1)"])
  })
})
