/**
 * 可拖浮层（fw/draggable.ts）单测。
 *
 * 这条最有价值的断言是「**点按钮不启动拖动**」：待办弹窗标题栏上有刷新/关闭按钮，
 * 若 exclude 失效，「点刷新」会变成「拖一下窗口」，位置还会被写进 localStorage。
 * 手动实现两份（todo-pop / persona-pop）时这处逻辑各写一遍，已出现过一边漏 closest 检查的分歧。
 *
 * 注意：这里只测**拖动状态机与位置钳制**，不测真实浏览器里的 pointer capture 行为
 * （那需要真浏览器，由端到端用例覆盖）。
 */
import { afterAll, beforeAll, test, expect } from "bun:test"
import { installMiniDom } from "./mini-dom.test-helper"

let restore: () => void = () => {}
beforeAll(() => {
  restore = installMiniDom().restore
})
afterAll(() => {
  restore()
})

const { makeDraggable } = await import("./draggable")

interface El {
  className: string
  dispatchEvent: (ev: { type?: string; button?: number; clientX?: number; clientY?: number; pointerId?: number; target?: unknown }) => boolean
  classList: { contains: (n: string) => boolean }
  style: { left?: string; top?: string; visibility?: string }
  appendChild: (c: unknown) => void
  querySelector: (s: string) => El | null
  querySelectorAll: (s: string) => El[]
  getBoundingClientRect: () => { left: number; top: number; width: number; height: number }
}

const E = (x: unknown): El => x as unknown as El

/** 造一个「根节点 + 标题栏 + 一个按钮」的浮层骨架（与真实面板结构一致）。 */
function makePopover(): { root: El; head: El; btn: El } {
  const root = E(document.createElement("div"))
  root.className = "popover"
  const head = E(document.createElement("div"))
  head.className = "popover-head"
  const btn = E(document.createElement("button"))
  root.appendChild?.(head as unknown as never)
  head.appendChild?.(btn as unknown as never)
  ;(document.body as unknown as { appendChild: (c: unknown) => void }).appendChild(root)
  return { root, head, btn }
}

test("拖动：按下标题栏 → 移动 → 位置更新 → 抬起结束", () => {
  const { root, head } = makePopover()
  const d = makeDraggable({ root: root as unknown as HTMLElement })
  head.dispatchEvent({ type: "pointerdown", button: 0, clientX: 10, clientY: 10, pointerId: 1, target: head })
  expect(d.isDragging()).toBe(true)
  expect(root.classList.contains("dragging")).toBe(true)
  head.dispatchEvent({ type: "pointermove", clientX: 60, clientY: 40, pointerId: 1 })
  expect(root.style.left).toBe("50px")
  expect(root.style.top).toBe("30px")
  head.dispatchEvent({ type: "pointerup", pointerId: 1 })
  expect(d.isDragging()).toBe(false)
  expect(root.classList.contains("dragging")).toBe(false)
})

test("**点按钮不启动拖动**（否则「点刷新」会变成拖一下窗口）", () => {
  const { root, head, btn } = makePopover()
  const d = makeDraggable({ root: root as unknown as HTMLElement })
  head.dispatchEvent({ type: "pointerdown", button: 0, clientX: 10, clientY: 10, pointerId: 1, target: btn })
  expect(d.isDragging()).toBe(false)
  expect(root.classList.contains("dragging")).toBe(false)
})

test("非左键不启动拖动（右键菜单不该拖窗口）", () => {
  const { root, head } = makePopover()
  const d = makeDraggable({ root: root as unknown as HTMLElement })
  head.dispatchEvent({ type: "pointerdown", button: 2, clientX: 10, clientY: 10, pointerId: 1, target: head })
  expect(d.isDragging()).toBe(false)
})

test("指针丢失（lostpointercapture）会复位拖动状态，避免下次移动从旧位移继续跳", () => {
  const { root, head } = makePopover()
  const d = makeDraggable({ root: root as unknown as HTMLElement })
  head.dispatchEvent({ type: "pointerdown", button: 0, clientX: 10, clientY: 10, pointerId: 1, target: head })
  expect(d.isDragging()).toBe(true)
  head.dispatchEvent({ type: "lostpointercapture", pointerId: 1 })
  expect(d.isDragging()).toBe(false)
  expect(root.classList.contains("dragging")).toBe(false)
})

test("show()：钳制到视口内并设为可见（浮层不会跑到屏幕外抓不回来）", () => {
  const { root } = makePopover()
  const d = makeDraggable({ root: root as unknown as HTMLElement })
  d.show()
  const left = Number(String(root.style.left).replace("px", ""))
  const top = Number(String(root.style.top).replace("px", ""))
  expect(left).toBeGreaterThanOrEqual(0)
  expect(top).toBeGreaterThanOrEqual(0)
  expect(root.style.visibility).toBe("visible")
})

test("dispose 后不再响应拖动（面板关闭后不留监听）", () => {
  const { root, head } = makePopover()
  const d = makeDraggable({ root: root as unknown as HTMLElement })
  d.dispose()
  head.dispatchEvent({ type: "pointerdown", button: 0, clientX: 10, clientY: 10, pointerId: 1, target: head })
  expect(d.isDragging()).toBe(false)
})
