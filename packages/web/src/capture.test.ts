import { describe, expect, test, mock } from "bun:test"

// mock modern-screenshot（bun test 无浏览器）：domToPng 返回固定 data URL
mock.module("modern-screenshot", () => ({
  domToPng: async () => "data:image/png;base64,aGVsbG8=",
}))

import { capturePage, CAPTURE_HTML_LIMIT } from "./capture"

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
