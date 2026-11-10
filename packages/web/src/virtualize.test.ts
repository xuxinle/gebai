/**
 * 窗口化 DOM 编排（virtualize.ts）的**渲染失败降级**契约。
 *
 * 事故形态（2026-11-10 实测，会话 ace35552d）：某块的消息渲染回调抛了 TypeError（待办卡片拿到
 * 白名单外的状态值）。异常穿过 `sync()` 的挂载循环 —— 那一块永远挂不上、spacer 也不更新，
 * 于是「提交里混入了…」之后几千像素全白，而且**每次滚动都重试、每次都再抛一次**。
 *
 * 契约：单块渲染失败 → 就地降级成错误占位，不中断挂载循环、不丢其它块、同块不反复重试。
 *
 * 用最小 fake DOM（bun test 无真实布局）：容器与块自带高度表，`getBoundingClientRect` 按
 * scrollTop 换算，浏览器里那套几何语义足够覆盖本契约要守的路径。
 */
import { afterAll, describe, expect, test } from "bun:test"
import { createVirtualizer } from "./virtualize"

/** 最小元素：className/style/children + 可设高度；几何按父容器顺序叠加（gap 记为 0）。 */
class FakeEl {
  tagName: string
  className = ""
  style: Record<string, string> = {}
  children: FakeEl[] = []
  parent: FakeEl | null = null
  /** 本元素（块）实测高度（px）；spacer 用 style.height。 */
  height = 0
  /** 自身文本（真实 DOM 的 textContent 语义：含子节点文本聚合，写入即清空子节点）。 */
  private ownText = ""
  get textContent(): string {
    return this.ownText + this.children.map((c) => c.textContent).join("")
  }
  set textContent(v: string) {
    this.ownText = String(v ?? "")
    for (const c of this.children) c.parent = null
    this.children = []
  }
  scrollTop = 0
  clientHeight = 0
  private listeners = new Map<string, Array<() => void>>()

  constructor(tag = "div") {
    this.tagName = tag
  }

  get classList() {
    const self = this
    const has = (c: string) => self.className.split(/\s+/).filter(Boolean).includes(c)
    return { contains: (c: string) => has(c), add: (c: string) => void (self.className = `${self.className} ${c}`.trim()) }
  }

  get isConnected(): boolean {
    return this.parent !== null || this.isRoot
  }

  /** 测试根容器标记（无父节点仍视为已连接）。 */
  isRoot = false

  /** 自身占位高度：spacer 看 style.height，块看实测 height。 */
  get ownHeight(): number {
    const h = parseInt(this.style.height ?? "", 10)
    if (Number.isFinite(h) && (this.style.height ?? "").endsWith("px")) return h
    return this.height
  }

  get childrenHeight(): number {
    return this.children.reduce((a, c) => a + c.ownHeight, 0)
  }

  get scrollHeight(): number {
    return this.childrenHeight
  }

  getBoundingClientRect() {
    // 真实几何：子节点按顺序叠加，减去 scrollTop
    let top = -this.scrollTop
    if (this.parent) {
      const idx = this.parent.children.indexOf(this)
      for (let i = 0; i < idx; i++) top += this.parent.children[i]!.ownHeight
      top += this.parent.getBoundingClientRect().top
    }
    const height = this.ownHeight
    return { top, height, bottom: top + height, left: 0, right: 0, width: 0, x: 0, y: top }
  }

  appendChild(c: FakeEl): FakeEl {
    c.parent = this
    this.children.push(c)
    return c
  }

  append(...nodes: FakeEl[]): void {
    for (const n of nodes) this.appendChild(n)
  }

  insertBefore(c: FakeEl, before: FakeEl | null): FakeEl {
    c.parent = this
    const i = before ? this.children.indexOf(before) : -1
    if (i < 0) this.children.push(c)
    else this.children.splice(i, 0, c)
    return c
  }

  replaceChildren(...nodes: FakeEl[]): void {
    for (const c of this.children) c.parent = null
    this.children = []
    for (const n of nodes) this.appendChild(n)
  }

  remove(): void {
    if (!this.parent) return
    const i = this.parent.children.indexOf(this)
    if (i >= 0) this.parent.children.splice(i, 1)
    this.parent = null
  }

  addEventListener(type: string, cb: () => void): void {
    this.listeners.set(type, [...(this.listeners.get(type) ?? []), cb])
  }

  removeEventListener(): void {}

  dispatch(type: string): void {
    for (const cb of this.listeners.get(type) ?? []) cb()
  }

  querySelector(): null {
    return null
  }
}

/** 装一套 fake DOM（仅 createElement），测试结束必须还原基线。 */
const g = globalThis as unknown as Record<string, unknown>
const prevDocument = g.document
g.document = { createElement: (tag?: string) => new FakeEl(tag ?? "div") }
afterAll(() => {
  g.document = prevDocument
})

/** 造一个可滚动的容器（视口 200px）。 */
function makeContainer(clientHeight = 200): FakeEl {
  const c = new FakeEl("div")
  c.isRoot = true
  c.clientHeight = clientHeight
  return c
}

/** 块高度表：index → 高度（含渲染失败那块，占位也有高度）。 */
const HEIGHTS = [60, 80, 60]

function setup(opts: { failAt?: number } = {}) {
  const container = makeContainer()
  const rendered: number[] = []
  const v = createVirtualizer({
    container: container as unknown as HTMLElement,
    renderBlock: (index, host) => {
      rendered.push(index)
      if (index === opts.failAt) throw new TypeError("Cannot read properties of undefined (reading 'svg')")
      ;(host as unknown as FakeEl).height = HEIGHTS[index] ?? 50
      ;(host as unknown as FakeEl).textContent = `块 ${index} 的消息`
    },
  })
  return { container, v, rendered }
}

describe("窗口化：单块渲染失败就地降级（不中断 sync、不留空白）", () => {
  test("失败的块降级为错误占位，且异常不逃出 sync", () => {
    const { container, v, rendered } = setup({ failAt: 1 })
    v.reset([{ key: "b0", weight: 1 }, { key: "b1", weight: 1 }, { key: "b2", weight: 1 }])
    // 降级占位也要有高度（否则那块在布局里是 0 高，等于又变回空白）
    const blocks = container.children.filter((c) => c.classList.contains("vz-block"))
    expect(blocks.length).toBe(3)
    const failed = blocks[1]!
    expect(failed.textContent).toContain("渲染失败")
    expect(failed.textContent).toContain("Cannot read properties of undefined")
    // 出错的块仍然「已挂载」：不会被反复重试（重试只会每次都再抛一次）
    expect(rendered.filter((i) => i === 1).length).toBe(1)
    // 其它块照常渲染（事故形态里它们全被同一个异常挡在后面）
    expect(blocks[0]!.textContent).toContain("块 0")
    expect(blocks[2]!.textContent).toContain("块 2")
  })

  test("回滚滚动位置/再次 sync 不重试失败块，也不产生新的异常", () => {
    const { container, v, rendered } = setup({ failAt: 1 })
    v.reset([{ key: "b0", weight: 1 }, { key: "b1", weight: 1 }, { key: "b2", weight: 1 }])
    const callsAfterReset = rendered.length
    container.scrollTop = 0
    v.sync()
    container.scrollTop = 60
    v.sync()
    expect(() => v.sync()).not.toThrow()
    // 已渲染过的块不再走渲染回调
    expect(rendered.length).toBe(callsAfterReset)
  })

  test("视口落在失败块上时，它仍是容器里的真实节点（用户看得见占位，而不是白屏）", () => {
    // 块数多到足以让容器不处于「贴底」状态（贴底时窗口取末尾，看不到中间失败块）
    const heights = [60, 80, 60, 60, 60, 60, 60, 60]
    const container = makeContainer()
    const v = createVirtualizer({
      container: container as unknown as HTMLElement,
      renderBlock: (index, host) => {
        if (index === 1) throw new TypeError("boom")
        ;(host as unknown as FakeEl).height = heights[index] ?? 60
        ;(host as unknown as FakeEl).textContent = `块 ${index} 的消息`
      },
    })
    v.reset(heights.map((_, i) => ({ key: `b${i}`, weight: 1 })))
    container.scrollTop = 90 // 视口顶落在失败块（块 1）范围内
    v.sync()
    // 失败块必须仍在挂载集合里（事故形态是它根本没进 DOM，于是只能白）
    const failed = container.children.filter((c) => c.textContent.includes("渲染失败"))
    expect(failed.length).toBe(1)
    // 上下相邻的块也在（窗口没被一个异常截断）
    expect(container.children.some((c) => c.textContent.includes("块 0"))).toBe(true)
    expect(container.children.some((c) => c.textContent.includes("块 2"))).toBe(true)
  })
})
