import { describe, expect, test } from "bun:test"
import MarkdownIt from "markdown-it"

// markdown.ts 模块加载期访问 document（state.ts 顶层 getElementById 等），bun test 无 DOM：
// 必须在动态 import 前 mock（最小 DOM mock，Proxy 兜底未定义成员为 no-op），同 messages.test.ts
const base = {
  classList: { add() {}, remove() {}, contains: () => false, toggle() {} },
  style: {},
  dataset: {},
  childNodes: [],
  children: [],
  append() {},
  appendChild() {},
  prepend() {},
  remove() {},
  insertAdjacentHTML() {},
  addEventListener() {},
  removeEventListener() {},
  setAttribute() {},
  getAttribute: () => null,
  querySelector: () => null,
  querySelectorAll: () => [],
  textContent: "",
  innerHTML: "",
  isConnected: true,
  open: true,
}
const doc = {
  getElementById: () => base,
  createElement: () => base,
  querySelector: () => null,
  querySelectorAll: () => [],
  addEventListener() {},
  body: base,
  documentElement: base,
  currentScript: null,
  baseURI: "http://localhost/",
}
;(globalThis as Record<string, unknown>).document = new Proxy(doc, {
  get(t, k) {
    if (typeof k === "string" && k in t) return (t as Record<string, unknown>)[k]
    return () => {}
  },
})
;(globalThis as Record<string, unknown>).window = globalThis
;(globalThis as Record<string, unknown>).navigator = { onLine: true }
;(globalThis as Record<string, unknown>).location = { protocol: "http:", host: "localhost" }
;(globalThis as Record<string, unknown>).MutationObserver = class {
  observe() {}
  disconnect() {}
}

const { applyLinkTargetRule } = await import("./markdown")

/** 与 markdown.ts 同配置实例化并应用链接规则（不含 DOMPurify，避免无 DOM 环境 sanitize 不可用）。 */
function render(text: string): string {
  const md = new MarkdownIt({ html: false, linkify: true, breaks: true })
  applyLinkTargetRule(md)
  return md.render(text)
}

describe("applyLinkTargetRule（markdown 链接一律新标签页打开）", () => {
  test("普通链接带 target=_blank 与 rel=noopener noreferrer", () => {
    const out = render('[歌白](https://example.com/a)')
    expect(out).toContain('href="https://example.com/a"')
    expect(out).toContain('target="_blank"')
    expect(out).toContain('rel="noopener noreferrer"')
  })

  test("linkify 自动识别链接同样生效", () => {
    const out = render("自动 https://example.org/b 链接")
    expect(out).toContain('href="https://example.org/b"')
    expect(out).toContain('target="_blank"')
    expect(out).toContain('rel="noopener noreferrer"')
  })

  test("尖括号 autolink 同样生效", () => {
    const out = render("<https://example.net/c>")
    expect(out).toContain('href="https://example.net/c"')
    expect(out).toContain('target="_blank"')
    expect(out).toContain('rel="noopener noreferrer"')
  })

  test("链接标题（title 属性）保留", () => {
    const out = render('[t](https://x.com "标题")')
    expect(out).toContain('title="标题"')
    expect(out).toContain('target="_blank"')
    expect(out).toContain('rel="noopener noreferrer"')
  })

  test("相对链接同样新标签页打开", () => {
    const out = render("[rel](/some/path)")
    expect(out).toContain('href="/some/path"')
    expect(out).toContain('target="_blank"')
  })

  test("无链接文本不产出 <a>", () => {
    const out = render("纯文本 `code` 无链接")
    expect(out).not.toContain("<a")
  })

  test("html:false 下原始 HTML 链接标签被转义，不产生可执行标签", () => {
    const out = render('<a href="https://evil.example">x</a>')
    expect(out).not.toContain('<a href="https://evil.example">') // 原始标签未透传
    expect(out).toContain("&lt;a") // 已转义为纯文本
  })
})
