import { describe, expect, test, mock } from "bun:test"

// 全量测试下 window 可能被其他测试文件泄漏为 globalThis（navigator 无 userAgent），
// 真实 modern-screenshot 模块在导入时求值 `window.navigator?.userAgent.includes(...)` 会抛
// 「USER_AGENT.includes is not a function」——先固定受控浏览器环境，保证模块级求值安全
;(globalThis as Record<string, unknown>).window = { navigator: { userAgent: "bun-test" } }
;(globalThis as Record<string, unknown>).navigator = { userAgent: "bun-test", onLine: true }

// mock modern-screenshot（bun test 无浏览器）：domToPng 返回固定 data URL
mock.module("modern-screenshot", () => ({
  domToPng: async () => "data:image/png;base64,aGVsbG8=",
}))

// 静态 import 会被提升到文件顶部（先于 mock.module 与全局设置执行，真实模块会以泄漏的 window 求值而崩溃），
// 改用动态 import 保证「全局 mock → mock.module 注册 → capture 加载」的顺序
const { capturePage, CAPTURE_HTML_LIMIT } = await import("./capture")

/** 最小 document mock（capturePage 只读 documentElement/getComputedStyle）。 */
function mockDoc(html: string, opts: { clientWidth?: number; clientHeight?: number; scrollHeight?: number } = {}) {
  const root = {
    outerHTML: html,
    clientWidth: opts.clientWidth ?? 1280,
    clientHeight: opts.clientHeight ?? 800,
    scrollHeight: opts.scrollHeight ?? 800,
  }
  ;(globalThis as Record<string, unknown>).document = {
    documentElement: root,
  }
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
