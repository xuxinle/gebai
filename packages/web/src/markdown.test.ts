import { afterAll, describe, expect, test } from "bun:test"
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
const prevGlobals = {
  document: (globalThis as Record<string, unknown>).document,
  navigator: (globalThis as Record<string, unknown>).navigator,
  location: (globalThis as Record<string, unknown>).location,
  MutationObserver: (globalThis as Record<string, unknown>).MutationObserver,
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
// document/navigator/location/MutationObserver 是整体替换的：本文件跑完必须放回基线那一份，
// 否则同进程后续测试文件读到这里的桩（成员多为 no-op），表现为单文件全过、全量随顺序失败
// （约定见 scripts/test-preload.ts）。
afterAll(() => {
  const g = globalThis as Record<string, unknown>
  g.document = prevGlobals.document
  g.navigator = prevGlobals.navigator
  g.location = prevGlobals.location
  g.MutationObserver = prevGlobals.MutationObserver
})

const { applyLinkTargetRule, applyTaskLists, enhanceDiagramBlocks, fenceDiagramFormat, openFenceLang } = await import("./markdown")

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

describe("applyTaskLists（GFM 任务列表勾选框，- [ ] 不再泄漏字面 []）", () => {
  test("未勾选/已勾选转勾选框，普通列表项不受影响", () => {
    const out = applyTaskLists(render("- [ ] 待办\n- [x] 已完成\n- 普通项"))
    expect(out).toContain('<li class="task-item"><span class="task-box" aria-hidden="true"></span> 待办</li>')
    expect(out).toContain('<span class="task-box done" aria-hidden="true"></span> 已完成')
    expect(out).toContain("<li>普通项</li>")
    expect(out).not.toContain("[]")
  })

  test("松散列表（<li><p> 包裹）与有序列表任务项同样转换", () => {
    const loose = applyTaskLists(render("- [ ] 待办\n\n- [x] 完成"))
    expect(loose).toContain('<li class="task-item">')
    expect(loose).toContain('<p><span class="task-box')
    expect(loose).not.toContain("[ ]")
    const ordered = applyTaskLists(render("1. [ ] 第一步"))
    expect(ordered).toContain('<li class="task-item"><span class="task-box"')
  })

  test("正文中的 [ ] 非行首占位不误转（仅列表项行首转换）", () => {
    const out = applyTaskLists(render("说明 [ ] 见列表"))
    expect(out).toContain("说明 [ ] 见列表")
  })
})

/* ---------- markdown 内图表围栏（```mermaid 等按图表渲染，模型不经 show 工具直接嵌入源码时的兜底） ---------- */

interface FakeEl {
  className: string
  textContent: string
  parentElement: FakeEl | null
  replaceWith(node: FakeEl): void
}

/** 代码块替身：一组 pre>code，root 提供 enhanceDiagramBlocks 依赖的 lastElementChild 与查询能力，
 *  替换按真实 DOM 语义原位改写 root 子元素表。 */
function fakeBlocks(items: Array<{ lang: string; code: string }>) {
  const kids: FakeEl[] = []
  const codes: FakeEl[] = []
  const root = {
    get lastElementChild() {
      return kids[kids.length - 1] ?? null
    },
    querySelectorAll: () => codes,
  }
  for (const it of items) {
    const pre: FakeEl = {
      className: "",
      textContent: "",
      parentElement: null,
      replaceWith(node: FakeEl) {
        const i = kids.indexOf(pre)
        if (i >= 0) kids.splice(i, 1, node)
      },
    }
    const code: FakeEl = { className: `language-${it.lang}`, textContent: it.code, parentElement: pre, replaceWith() {} }
    kids.push(pre)
    codes.push(code)
  }
  return { root, kids }
}

type RenderCall = [HTMLElement, { type?: string; format?: string; code?: string; name?: string }]
function collector(calls: RenderCall[]) {
  return (container: HTMLElement, block: RenderCall[1]) => {
    calls.push([container, block])
  }
}

describe("markdown 图表围栏识别", () => {
  test("语言标记映射图表语言（别名/大小写归一，非图表语言不映射）", () => {
    expect(fenceDiagramFormat("mermaid")).toBe("mermaid")
    expect(fenceDiagramFormat("MMD")).toBe("mermaid")
    expect(fenceDiagramFormat("puml")).toBe("plantuml")
    expect(fenceDiagramFormat("d2")).toBe("d2")
    expect(fenceDiagramFormat("echarts")).toBe("echarts")
    expect(fenceDiagramFormat("bash")).toBeNull()
    expect(fenceDiagramFormat("json")).toBeNull()
  })

  test("末尾未闭合围栏识别（含波浪围栏与围栏长度配对）", () => {
    expect(openFenceLang("```mermaid\ngraph TD\n```\n")).toBeNull()
    expect(openFenceLang("```mermaid\ngraph TD\n")).toBe("mermaid")
    expect(openFenceLang("~~~d2\na -> b")).toBe("d2")
    expect(openFenceLang("```python\nprint(1)\n```\n```mermaid\ngraph TD")).toBe("mermaid")
    expect(openFenceLang("普通文本\n无围栏")).toBeNull()
  })
})

describe("enhanceDiagramBlocks（图表围栏替换为图表卡片）", () => {
  test("闭合的 mermaid 块替换为图表容器并交给渲染器（源码与语言透传）", () => {
    const { root, kids } = fakeBlocks([{ lang: "mermaid", code: "graph TD\n  A-->B" }])
    const calls: RenderCall[] = []
    enhanceDiagramBlocks(root as unknown as HTMLElement, "```mermaid\ngraph TD\n  A-->B\n```\n", collector(calls))
    expect(calls.length).toBe(1)
    expect(calls[0][1]).toMatchObject({ type: "diagram", format: "mermaid", code: "graph TD\n  A-->B", name: "Mermaid" })
    expect(kids[0].className).toBe("diagram-embed") // 原代码块被图表容器替换
  })

  test("末尾未闭合的图表围栏跳过，前面闭合的仍渲染", () => {
    const { root } = fakeBlocks([
      { lang: "mermaid", code: "graph LR\n  A-->B" },
      { lang: "mermaid", code: "graph TD\n  C-->D" },
    ])
    const calls: RenderCall[] = []
    const text = "```mermaid\ngraph LR\n  A-->B\n```\n\n图表二：\n\n```mermaid\ngraph TD\n  C-->D"
    enhanceDiagramBlocks(root as unknown as HTMLElement, text, collector(calls))
    expect(calls.length).toBe(1)
    expect(calls[0][1].code).toBe("graph LR\n  A-->B")
  })

  test("非图表语言与空源码块保持原样（不替换、不渲染）", () => {
    const { root, kids } = fakeBlocks([
      { lang: "bash", code: "ls -la" },
      { lang: "mermaid", code: "   \n" },
    ])
    const calls: RenderCall[] = []
    enhanceDiagramBlocks(root as unknown as HTMLElement, "```bash\nls -la\n```\n\n```mermaid\n   \n```\n", collector(calls))
    expect(calls.length).toBe(0)
    expect(kids.map((k) => k.className)).toEqual(["", ""])
  })

  test("plantuml/d2/echarts 围栏同样渲染（四语言与 show 对齐）", () => {
    const { root } = fakeBlocks([
      { lang: "plantuml", code: "@startuml\nA -> B\n@enduml" },
      { lang: "d2", code: "a -> b" },
      { lang: "echarts", code: '{"series":[]}' },
    ])
    const calls: RenderCall[] = []
    const text = "```plantuml\n@startuml\nA -> B\n@enduml\n```\n\n```d2\na -> b\n```\n\n```echarts\n{\"series\":[]}\n```\n"
    enhanceDiagramBlocks(root as unknown as HTMLElement, text, collector(calls))
    expect(calls.map((c) => c[1].format)).toEqual(["plantuml", "d2", "echarts"])
  })
})
