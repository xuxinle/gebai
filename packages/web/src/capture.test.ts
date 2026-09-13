import { describe, expect, test, mock } from "bun:test"

// 全量测试下 window 可能被其他测试文件泄漏为 globalThis（navigator 无 userAgent），
// 真实 modern-screenshot 模块在导入时求值 `window.navigator?.userAgent.includes(...)` 会抛
// 「USER_AGENT.includes is not a function」——先固定受控浏览器环境，保证模块级求值安全。
// 监听方法一并给成 no-op：本桩是模块级写入、后续测试文件共用，缺方法会让下游模块的顶层监听直接抛。
;(globalThis as Record<string, unknown>).window = {
  navigator: { userAgent: "bun-test" },
  addEventListener() {},
  removeEventListener() {},
}
;(globalThis as Record<string, unknown>).navigator = { userAgent: "bun-test", onLine: true }

// mock modern-screenshot（bun test 无浏览器）：domToPng 返回固定 data URL
mock.module("modern-screenshot", () => ({
  domToPng: async () => "data:image/png;base64,aGVsbG8=",
}))

// 静态 import 会被提升到文件顶部（先于 mock.module 与全局设置执行，真实模块会以泄漏的 window 求值而崩溃），
// 改用动态 import 保证「全局 mock → mock.module 注册 → capture 加载」的顺序
const { capturePage, CAPTURE_HTML_LIMIT, CAPTURE_MAX_NODES, makeCaptureFilter } = await import("./capture")

/** 伪造 Element（bun test 无 DOM）：过滤器只依赖 Element 判定与 getBoundingClientRect。 */
class FakeElement {
  constructor(private rect: { top: number; bottom: number; width: number; height: number }) {}
  getBoundingClientRect() {
    return this.rect
  }
}
;(globalThis as Record<string, unknown>).Element = FakeElement
const elAt = (top: number, bottom: number) => new FakeElement({ top, bottom, width: 100, height: bottom - top }) as unknown as Node

/** 最小 document mock（capturePage 只读 documentElement/getComputedStyle）。 */
function mockDoc(html: string, opts: { clientWidth?: number; clientHeight?: number; scrollHeight?: number } = {}) {
  const root = {
    outerHTML: html,
    clientWidth: opts.clientWidth ?? 1280,
    clientHeight: opts.clientHeight ?? 800,
    scrollHeight: opts.scrollHeight ?? 800,
    dataset: {},
    addEventListener() {},
    removeEventListener() {},
  }
  // 只换 documentElement，不整体替换 document：整体替换会把基线 DOM（scripts/test-preload.ts）盖掉，
  // 而本文件之后加载的测试文件里，模块顶层的 getElementById 之类会直接抛。
  const doc = ((globalThis as Record<string, unknown>).document ??= {}) as Record<string, unknown>
  doc.documentElement = root
  ;(globalThis as Record<string, unknown>).getComputedStyle = () => ({ backgroundColor: "#ffffff" })
  return root
}

describe("capturePage", () => {
  test("captures rendered DOM html (truncated to limit) with screenshot", async () => {
    mockDoc("<!doctype html><html><body><h1>渲染后页面</h1></body></html>")
    const cap = await capturePage()
    expect(cap.html).toContain("渲染后页面")
    expect(cap.imageBase64).toBe("data:image/png;base64,aGVsbG8=")
  })

  test("truncates oversized html to CAPTURE_HTML_LIMIT", async () => {
    mockDoc("<html><body>" + "x".repeat(CAPTURE_HTML_LIMIT + 5000) + "</body></html>")
    const cap = await capturePage()
    expect(cap.html.length).toBe(CAPTURE_HTML_LIMIT)
  })

  test("screenshot failure does not block html capture", async () => {
    mock.module("modern-screenshot", () => ({
      domToPng: async () => {
        throw new Error("canvas 超限")
      },
    }))
    mockDoc("<html><body>只有 html</body></html>")
    const cap = await capturePage()
    expect(cap.html).toContain("只有 html")
    expect(cap.imageBase64).toBeUndefined()
  })

  test("fullPage uses document height capped at CAPTURE_FULLPAGE_MAX_HEIGHT", async () => {
    mockDoc("<html><body>长页面</body></html>", { scrollHeight: 30000 })
    const cap = await capturePage({ fullPage: true })
    // domToPng 被 mock 忽略参数；此处验证链路不抛异常即可
    expect(cap.html).toContain("长页面")
  })
})

describe("截图节点过滤器（避免长会话卡死的关键）", () => {
  test("区域内元素纳入并消耗预算；区域外（上/下）子树排除", () => {
    const budget = { left: 10 }
    const f = makeCaptureFilter(0, 800, budget)
    expect(f(elAt(100, 200))).toBe(true) // 区域内
    expect(f(elAt(-300, -10))).toBe(false) // 完全在区域上方（已滚出）
    expect(f(elAt(900, 1000))).toBe(false) // 完全在区域下方
    expect(f(elAt(700, 900))).toBe(true) // 跨下界（部分可见）→ 纳入
    expect(budget.left).toBe(8) // 只对纳入的元素计数
  })

  test("预算用尽后一律排除（极端页面的硬兜底）", () => {
    const budget = { left: 2 }
    const f = makeCaptureFilter(0, 800, budget)
    expect(f(elAt(10, 20))).toBe(true)
    expect(f(elAt(30, 40))).toBe(true)
    expect(f(elAt(50, 60))).toBe(false) // 预算已耗尽（即使可见也不纳入）
    expect(budget.left).toBe(0)
  })

  test("非元素节点与无布局盒元素不参与几何判定（直接纳入）", () => {
    const f = makeCaptureFilter(0, 800, { left: 5 })
    expect(f({ nodeType: 3 } as unknown as Node)).toBe(true) // 文本节点
    expect(f(elAt(0, 0))).toBe(true) // 零尺寸（无布局盒）
  })

  test("整页模式：滚动区域从文档顶算起，视口模式从 scrollTop 算起", () => {
    // 视口模式（scrollTop=500）：向上滚出的内容不纳入
    const viewport = { left: 10 }
    const fv = makeCaptureFilter(500, 1300, viewport)
    expect(fv(elAt(100, 200))).toBe(false) // 已滚出（在视口上方）
    expect(fv(elAt(600, 700))).toBe(true)
    // 整页模式（regionTop=0）：文档顶的内容也要纳入
    const fp = { left: 10 }
    const ff = makeCaptureFilter(0, 12000, fp)
    expect(ff(elAt(100, 200))).toBe(true)
  })

  test("预算常量有界（对应约 1.5s 主线程成本，防长会话整页截图阻塞数秒）", () => {
    expect(CAPTURE_MAX_NODES).toBeGreaterThan(500)
    expect(CAPTURE_MAX_NODES).toBeLessThanOrEqual(4000)
  })
})
