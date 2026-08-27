import { afterEach, describe, expect, mock, test } from "bun:test"
import type { ToolInfo } from "@gebai/sdk"
import type { RunState, SessionRunState } from "./state"
// markdown.ts 模块级 import dompurify：bun test 无 DOM 环境 sanitize 不可用，先 mock 模块（须早于动态 import messages）
mock.module("dompurify", () => ({ default: { sanitize: (s: unknown) => s } }))

// state.ts 模块加载期访问 document（getElementById("messages") 等），bun test 无 DOM：
// 最小 DOM mock（Proxy 兜底未定义成员为 no-op）；createElement 返回带真实子节点树的对象，
// querySelectorAll 支持按「tag.class」（如 details.subagent-run）遍历（子Agent 容器回放断言用）；
// textContent 语义贴近真实 DOM：读取聚合自身文本 + 子节点文本（工具卡片结构化头部断言用），写入清空子节点
interface MockEl {
  children: MockEl[]
  className: string
  tagName: string
  textContent: string
  append(...nodes: unknown[]): void
  appendChild(n: unknown): void
  remove(): void
}
/** MockEl + 查询能力（由 Proxy 陷阱提供，类型上补齐声明）。 */
type MockElWithQuery = MockEl & { querySelector(sel: string): unknown; querySelectorAll(sel: string): unknown[] }
function makeMockEl(tag = "div"): MockElWithQuery {
  const dataset: Record<string, string> = {}
  let ownText = ""
  const el: MockEl = {
    children: [],
    className: "",
    tagName: tag.toUpperCase(),
    get textContent() {
      return ownText + el.children.map((c) => c.textContent ?? "").join("")
    },
    set textContent(v: string) {
      ownText = String(v ?? "")
      el.children.length = 0
    },
    append(...nodes: unknown[]) {
      for (const n of nodes) {
        if (n && typeof n === "object") {
          ;(n as MockEl & { parentRef?: MockEl }).parentRef = el
          el.children.push(n as MockEl)
        }
      }
    },
    appendChild(n: unknown) {
      if (n && typeof n === "object") {
        ;(n as MockEl & { parentRef?: MockEl }).parentRef = el
        el.children.push(n as MockEl)
      }
    },
    remove() {
      const p = (el as MockEl & { parentRef?: MockEl }).parentRef
      const self = (el as unknown as { selfProxy?: MockEl }).selfProxy ?? el
      if (p) p.children = p.children.filter((c) => c !== self)
    },
  }
  // classList 直接操作 className（与真实 DOM 语义一致：属性赋值与 classList 增删互不覆盖）
  const classList = {
    add(c: string) {
      if (!el.className.split(" ").includes(c)) el.className = `${el.className} ${c}`.trim()
    },
    remove(c: string) {
      el.className = el.className.split(" ").filter((x) => x && x !== c).join(" ")
    },
    contains: (c: string) => el.className.split(" ").includes(c),
    toggle(c: string) {
      classList.contains(c) ? classList.remove(c) : classList.add(c)
    },
  }
  const proxy = new Proxy(el, {
    get(t, k) {
      if (typeof k === "string" && k in t) return (t as unknown as Record<string, unknown>)[k]
      if (k === "classList") return classList
      if (k === "open") return false
      if (k === "dataset") return dataset
      if (k === "querySelector" || k === "querySelectorAll") {
        return (sel: string) => {
          const [tag, cls] = sel.split(".")
          const hits: MockEl[] = []
          const walk = (n: MockEl) => {
            for (const c of n.children) {
              const tagOk = !tag || tag === "*" || c.tagName === tag.toUpperCase()
              if (tagOk && (!cls || (c.className ?? "").split(" ").includes(cls))) hits.push(c)
              walk(c)
            }
          }
          walk(t)
          return k === "querySelector" ? hits[0] ?? null : hits
        }
      }
      if (k === "appendChild") return t.appendChild
      if (k === "dataset") return {}
      return () => {}
    },
  })
  // remove() 以 children 中存的外部引用（proxy）比对自身：挂 self 引用供比对
  ;(el as unknown as { selfProxy?: MockEl }).selfProxy = proxy as unknown as MockEl
  return proxy as unknown as MockElWithQuery
}
const base = makeMockEl("div")
base.textContent = ""
base.className = ""
const doc = {
  getElementById: () => base,
  createElement: (tag?: string) => makeMockEl(tag ?? "div"),
  querySelector: (sel: string) => (base as unknown as { querySelector(s: string): unknown }).querySelector(sel),
  querySelectorAll: (sel: string) => (base as unknown as { querySelectorAll(s: string): unknown[] }).querySelectorAll(sel),
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
;(globalThis as Record<string, unknown>).requestAnimationFrame = (cb: () => void) => {
  cb()
  return 0
}
;(globalThis as Record<string, unknown>).cancelAnimationFrame = () => {}
;(globalThis as Record<string, unknown>).navigator = { onLine: true }
;(globalThis as Record<string, unknown>).location = { protocol: "http:", host: "localhost" }
;(globalThis as Record<string, unknown>).MutationObserver = class {
  observe() {}
  disconnect() {}
}

// 动态 import：mock 之后加载依赖 DOM 的模块
const { sealSegment, sessionRunBox, finishSessionRun, sealSessionSegment, sealBlockResultSegment, bindSessionScroll, scrollSessionSticky, renderSessionArchive, renderLegacySubAgentArchive, renderBlock, appendAskUserRecord, appendPlanCard, appendToolResult, renderChoiceCard } = await import("./messages")
const { runs, pendingTools, pendingToolsKey, approvalsEl } = await import("./state")
const { isBlockOnly, toolBubbleFor, __setToolCardMetaForTest, buildPlanMarkdown, planResultHead, askUserResultHead } = await import("./tool-cards")

function fakeRun(overrides: Partial<RunState> = {}): RunState {
  return {
    sessionId: "s1",
    acc: "",
    el: null,
    reasoningAcc: "思考中",
    reasoningEl: null,
    messageId: "",
    lastActivity: Date.now(),
    abort: new AbortController(),
    ...overrides,
  } as RunState
}

describe("isBlockOnly (card.args=block 声明驱动，替代前端 BLOCK_ONLY_TOOLS 硬编码)", () => {
  test("follows server-declared card metadata", () => {
    __setToolCardMetaForTest([
      ["show", { args: "block" }],
      ["write", { args: "code", codeField: "content" }],
      ["playwright_open", { args: "block" }],
    ])
    // block 声明命中
    expect(isBlockOnly("show")).toBe(true)
    expect(isBlockOnly("playwright_open")).toBe(true) // 子 Agent 全名
    // 非 block 声明 / 未声明：false
    expect(isBlockOnly("write")).toBe(false)
    expect(isBlockOnly("plain_tool")).toBe(false)
    expect(isBlockOnly("open")).toBe(false) // 未注册短名
    expect(isBlockOnly("")).toBe(false)
  })

  test("resets with empty cache (未声明回退默认渲染)", () => {
    __setToolCardMetaForTest([])
    expect(isBlockOnly("show")).toBe(false)
  })
})

describe("sealSegment", () => {
  test("collapses an open reasoning block before dropping its reference (工具调用处推理结束折叠)", () => {
    const reasoningEl = { isConnected: true, open: true } as unknown as HTMLElement
    runs.set("s1", fakeRun({ reasoningEl }))
    sealSegment("s1")
    // 推理块折叠（不再残留展开态）且引用已丢弃
    expect((reasoningEl as HTMLDetailsElement).open).toBe(false)
    expect(runs.get("s1")!.reasoningEl).toBeNull()
    runs.clear()
  })

  test("collapses even when run.el is null (工具调用封段后流结束的兜底路径)", () => {
    // 模拟：最后一次工具调用封段后 run.el 为 null，但推理块仍在 DOM（open）
    const reasoningEl = { isConnected: true, open: true } as unknown as HTMLElement
    runs.set("s1", fakeRun({ el: null, reasoningEl }))
    sealSegment("s1")
    expect((reasoningEl as HTMLDetailsElement).open).toBe(false)
    runs.clear()
  })

  test("leaves already-collapsed or detached blocks untouched", () => {
    const collapsed = { isConnected: true, open: false } as unknown as HTMLElement
    const detached = { isConnected: false, open: true } as unknown as HTMLElement
    runs.set("s1", fakeRun({ reasoningEl: collapsed }))
    runs.set("s2", fakeRun({ sessionId: "s2", reasoningEl: detached }))
    sealSegment("s1")
    expect((collapsed as HTMLDetailsElement).open).toBe(false)
    expect((detached as HTMLDetailsElement).open).toBe(true) // 不在 DOM 的块不动
    runs.clear()
  })
})

describe("sealBlockResultSegment（块级工具结果封段：图表卡片独立展示、后续输出另起新卡片）", () => {
  function fakeSub(overrides: Partial<SessionRunState> = {}): SessionRunState {
    return {
      runId: "r1",
      agents: [],
      input: "",
      container: makeMockEl("details") as unknown as HTMLElement,
      body: makeMockEl("div") as unknown as HTMLElement,
      outputEl: makeMockEl("span") as unknown as HTMLElement,
      acc: "容器内文本",
      el: null,
      messageId: "",
      reasoningAcc: "",
      reasoningEl: null,
      ...overrides,
    } as SessionRunState
  }

  test("主循环：封存当前文本段（el 置空、推理折叠），后续输出惰性另起新卡片", () => {
    const el = makeMockEl("div") as unknown as HTMLElement
    const reasoningEl = { isConnected: true, open: true } as unknown as HTMLElement
    runs.set("s1", fakeRun({ el, reasoningEl }))
    sealBlockResultSegment("s1")
    expect(runs.get("s1")!.el).toBeNull()
    expect(runs.get("s1")!.reasoningEl).toBeNull()
    expect((reasoningEl as HTMLDetailsElement).open).toBe(false)
    runs.clear()
  })

  test("新会话执行容器内：封存容器文本段；容器缺失时 no-op 不崩溃", () => {
    const subEl = makeMockEl("div") as unknown as HTMLElement
    const sub = fakeSub({ el: subEl })
    runs.set("s1", fakeRun({ sessionRuns: new Map([["r1", sub]]) }))
    sealBlockResultSegment("s1", "r1")
    expect(sub.el).toBeNull()
    expect(sub.acc).toBe("")
    // 容器缺失（切走场景）：不封任何段、不抛错
    runs.set("s1", fakeRun({ sessionRuns: new Map() }))
    sealBlockResultSegment("s1", "rX")
    expect(runs.get("s1")!.sessionRuns!.size).toBe(0)
    runs.clear()
  })
})

describe("renderBlock image 块（点击进入全屏查看器）", () => {
  test("image 块渲染为可点击的 img：onclick 打开查看器、src 指向会话文件、悬浮提示", () => {
    const container = makeMockEl("div")
    renderBlock(container as unknown as HTMLElement, { type: "image", path: "tmp/flow.png", name: "flow.png", mime: "image/png" }, "s1")
    expect(container.children.length).toBe(1)
    const img = container.children[0] as unknown as { tagName: string; className: string; src: string; onclick?: unknown; dataset: Record<string, string> }
    expect(img.tagName).toBe("IMG")
    expect(img.className).toBe("block-img")
    // files/preview 统一取数（会话相对与项目绝对路径均支持）
    expect(img.src).toContain("files/preview?path=tmp%2Fflow.png")
    expect(typeof img.onclick).toBe("function")
    expect(img.dataset.tip).toBe("点击查看大图")
  })
})

describe("文件内容卡（code/file 块统一渲染：按类型分派 + 工具栏）", () => {
  test("code 块 markdown 语言：卡片容器 + markdown 渲染 + 复制/原文件/下载工具栏", () => {
    const container = makeMockEl("div")
    renderBlock(container as unknown as HTMLElement, { type: "code", text: "# 标题\n\n- 项", language: "markdown", path: "tmp/plans/重构.md", name: "重构.md" }, "s1")
    const card = container.children[0] as unknown as MockElWithQuery
    expect(card.className).toContain("file-card")
    expect((card.querySelector("span.file-title") as unknown as { textContent?: string })?.textContent).toBe("重构.md")
    // markdown 渲染容器（mock 选择器仅支持单段 tag.class，容器级查询断言）
    expect(card.querySelector("div.markdown")).not.toBeNull()
    // 工具栏：复制 + 原文件 + 下载（下载经 files/preview 附件形式——会话相对与项目绝对路径统一入口）
    const dl = card.querySelector("a.file-dl-icon") as unknown as { href: string; download: string }
    expect(dl).not.toBeNull()
    expect(dl.href).toContain("files/preview?path=tmp%2Fplans%2F")
    expect(dl.href).toContain("download=1")
    expect(card.querySelectorAll("button").length).toBeGreaterThanOrEqual(2)
  })

  test("code 块源码语言：语法高亮 pre（file-code），语言作徽标", () => {
    const container = makeMockEl("div")
    renderBlock(container as unknown as HTMLElement, { type: "code", text: "const a = 1", language: "typescript", path: "tmp/a.ts", name: "a.ts" }, "s1")
    const card = container.children[0] as unknown as MockElWithQuery
    expect(card.querySelector("pre.file-code")).not.toBeNull()
    expect((card.querySelector("span.file-badge") as unknown as { textContent?: string })?.textContent).toBe("typescript")
  })

  test("code 块无 path（防御降级）：仍渲染卡片与内容，无下载/原文件入口", () => {
    const container = makeMockEl("div")
    renderBlock(container as unknown as HTMLElement, { type: "code", text: "裸文本", language: "bash" }, "s1")
    const card = container.children[0] as unknown as MockElWithQuery
    expect(card.className).toContain("file-card")
    expect(card.querySelector("a.file-dl-icon")).toBeNull()
    expect(card.querySelector("pre.file-code")).not.toBeNull()
  })

  test("file 块图片类型：卡内 img 内联（src 指向会话文件）", () => {
    const container = makeMockEl("div")
    renderBlock(container as unknown as HTMLElement, { type: "file", path: "tmp/shot.png", name: "shot.png", mime: "image/png" }, "s1")
    const card = container.children[0] as unknown as MockElWithQuery
    const img = card.querySelector("img.file-img") as unknown as { src: string }
    expect(img).not.toBeNull()
    expect(img.src).toContain("files/preview?path=tmp%2Fshot.png")
  })

  test("file 块文本类型：进入视口按需 fetch 后语法高亮渲染（无 IntersectionObserver 环境立即加载）", async () => {
    const origFetch = globalThis.fetch
    globalThis.fetch = (async () => new Response("console.log(1)")) as unknown as typeof fetch
    try {
      const container = makeMockEl("div")
      renderBlock(container as unknown as HTMLElement, { type: "file", path: "tmp/a.js", name: "a.js", mime: "text/javascript" }, "s1")
      const card = container.children[0] as unknown as MockElWithQuery
      await new Promise((r) => setTimeout(r, 10))
      expect(card.querySelector("pre.file-code")).not.toBeNull()
      expect(card.querySelector("a.file-dl-icon")).not.toBeNull()
    } finally {
      globalThis.fetch = origFetch
    }
  })

  test("file 块 office 类型（docx/xlsx/pptx）：render=office 取数后沙箱 iframe 渲染阅读视图，下载常驻", async () => {
    const origFetch = globalThis.fetch
    let calledUrl = ""
    globalThis.fetch = (async (url: unknown) => {
      calledUrl = String(url)
      return new Response('<!doctype html><html><head></head><body><main>office-view-marker</main></body></html>')
    }) as unknown as typeof fetch
    try {
      const container = makeMockEl("div")
      renderBlock(container as unknown as HTMLElement, { type: "file", path: "tmp/r.docx", name: "r.docx", mime: "application/vnd.openxmlformats-officedocument.wordprocessingml.document" }, "s1")
      const card = container.children[0] as unknown as MockElWithQuery
      await new Promise((r) => setTimeout(r, 10))
      // 取数带 render=office（服务端阅读视图分支）
      expect(calledUrl).toContain("files/preview")
      expect(calledUrl).toContain("render=office")
      const frame = card.querySelector("iframe.html-frame") as unknown as { srcdoc: string } | null
      expect(frame).not.toBeNull()
      expect(frame!.srcdoc).toContain("office-view-marker")
      expect(card.querySelector("a.file-dl-icon")).not.toBeNull()
    } finally {
      globalThis.fetch = origFetch
    }
  })

  test("file 块 404（历史卡片指向已删除文件，如 agent 测试后清理）：明确提示文件已不存在，不引导下载", async () => {
    const origFetch = globalThis.fetch
    globalThis.fetch = (async () => new Response("not found", { status: 404 })) as unknown as typeof fetch
    try {
      const container = makeMockEl("div")
      renderBlock(container as unknown as HTMLElement, { type: "file", path: "tmp/gone.ts", name: "gone.ts", mime: "text/plain" }, "s1")
      const card = container.children[0] as unknown as MockElWithQuery
      await new Promise((r) => setTimeout(r, 10))
      expect(card.textContent).toContain("文件已不存在")
      expect(card.textContent).toContain("已被删除或清理")
      expect(card.textContent).not.toContain("下载")
    } finally {
      globalThis.fetch = origFetch
    }
  })
})

describe("sessionRunBox / finishSessionRun / sealSessionSegment（新会话执行折叠容器）", () => {
  test("container renders input as param block, stays open while running, collapses with output summary when done", () => {
    const box = sessionRunBox({ runId: "r1", agents: ["code"], input: "改个文件" })
    expect(box.container.className).toContain("session-run")
    expect(box.container.dataset.runId).toBe("r1")
    // 执行中：展开 + 输出区占位
    expect((box.container as HTMLDetailsElement).open).toBe(true)
    expect(box.outputEl.textContent).toContain("执行中")
    // 输入以参数块渲染在容器顶部，标题无「输入: …」提示
    const inputBlock = box.body.querySelector("div.agent-run-input")
    expect(inputBlock?.textContent).toBe("改个文件")
    expect(box.container.querySelector(".session-input")).toBeNull()
    expect(box.container.querySelector(".session-title")?.textContent).not.toContain("输入:")
    // 结束后：自动折叠，只显示最终返回（summary 内）
    finishSessionRun(box.container, box.outputEl, "已完成修改")
    expect((box.container as HTMLDetailsElement).open).toBe(false)
    expect(box.outputEl.textContent).toContain("已完成修改")
    expect(box.container.classList.contains("done")).toBe(true)
  })

  test("多 Agent 预加载：标题列出全部预加载子Agent", () => {
    const box = sessionRunBox({ runId: "r4", agents: ["code", "playwright"], input: "改个文件" })
    expect(box.container.querySelector(".session-title")?.textContent).toContain("code")
    expect(box.container.querySelector(".session-title")?.textContent).toContain("playwright")
  })

  test("finishSessionRun: undefined keeps running state, empty string collapses with 无返回 (失败/取消不误导)", () => {
    const box = sessionRunBox({ runId: "r2", agents: ["code"], input: "x" })
    // undefined/null：run 未收尾 → 保持执行中态（不折叠）
    finishSessionRun(box.container, box.outputEl, undefined)
    expect((box.container as HTMLDetailsElement).open).toBe(true)
    expect(box.outputEl.textContent).toContain("执行中")
    // 空串：已结束但无返回（失败/取消/风暴终止）→ 折叠并显示无返回，不再误导为执行中
    const box2 = sessionRunBox({ runId: "r3", agents: ["code"], input: "x" })
    finishSessionRun(box2.container, box2.outputEl, "")
    expect((box2.container as HTMLDetailsElement).open).toBe(false)
    expect(box2.outputEl.textContent).toContain("无返回")
  })

  test("sealSessionSegment seals text/reasoning state like sealSegment", () => {
    const reasoningEl = { isConnected: true, open: true } as unknown as HTMLElement
    const sub = {
      runId: "r1",
      agents: ["code"],
      input: "",
      container: base as unknown as HTMLDetailsElement,
      body: base as unknown as HTMLElement,
      outputEl: base as unknown as HTMLElement,
      acc: "text",
      el: { isConnected: true, classList: { remove() {} } } as unknown as HTMLElement,
      messageId: "m1",
      reasoningAcc: "think",
      reasoningEl,
    }
    sealSessionSegment(sub)
    expect(sub.acc).toBe("")
    expect(sub.el).toBeNull()
    expect(sub.reasoningAcc).toBe("")
    expect(sub.reasoningEl).toBeNull()
    expect((reasoningEl as HTMLDetailsElement).open).toBe(false)
  })

  test("scrollSessionSticky follows bottom while sticky and stops after user scrolls up", () => {
    // 伪造可滚动 body（scrollTop/scrollHeight/clientHeight 可读写，监听按类型记录）
    const listeners: Record<string, Array<(ev?: unknown) => void>> = {}
    const body = {
      scrollTop: 0,
      scrollHeight: 100,
      clientHeight: 50,
      addEventListener: (type: string, cb: (ev?: unknown) => void) => {
        ;(listeners[type] ??= []).push(cb)
      },
      emit: (type: string, ev?: unknown) => {
        for (const cb of listeners[type] ?? []) cb(ev)
      },
    }
    bindSessionScroll(body as unknown as HTMLElement)
    // 初始跟随：内容增长 → 滚到底部
    body.scrollHeight = 200
    scrollSessionSticky(body as unknown as HTMLElement)
    expect(body.scrollTop).toBe(200)
    // 用户上翻（滚轮输入先于滚动效果）：停止跟随
    body.emit("wheel", { deltaY: -1 })
    body.scrollTop = 40
    body.emit("scroll")
    body.scrollHeight = 300
    scrollSessionSticky(body as unknown as HTMLElement)
    expect(body.scrollTop).toBe(40) // 不再跟随
    // 用户滚回底部：恢复跟随
    body.scrollTop = 250
    body.scrollHeight = 300
    body.emit("scroll")
    scrollSessionSticky(body as unknown as HTMLElement)
    expect(body.scrollTop).toBe(300)
  })

  test("scrollSessionSticky delayed program scroll event after content growth keeps following (工具卡片追加场景)", () => {
    // 伪造可滚动 body：scrollTop 按浏览器语义 clamp 到 scrollHeight - clientHeight
    const listeners: Record<string, Array<(ev?: unknown) => void>> = {}
    let scrollTop = 0
    const body = {
      clientHeight: 50,
      get scrollTop() {
        return scrollTop
      },
      set scrollTop(v: number) {
        scrollTop = Math.max(0, Math.min(v, body.scrollHeight - body.clientHeight))
      },
      scrollHeight: 200,
      addEventListener: (type: string, cb: (ev?: unknown) => void) => {
        ;(listeners[type] ??= []).push(cb)
      },
      emit: (type: string, ev?: unknown) => {
        for (const cb of listeners[type] ?? []) cb(ev)
      },
    }
    bindSessionScroll(body as unknown as HTMLElement)
    // 容器内流式内容 + 工具卡片追加：程序滚动后、scroll 事件送达前内容增长
    scrollSessionSticky(body as unknown as HTMLElement)
    expect(scrollTop).toBe(150) // clamp 落位（200-50）
    body.scrollHeight = 600 // 工具卡片在事件送达前追加
    body.emit("scroll") // 迟到的程序滚动事件（位置 150 已不在当前底部）
    expect(scrollTop).toBe(550) // 静默窗口内归因为内部动作：续滚到最新底部，跟随未失效
    // 用户上翻仍能解除跟随（滚轮输入先于滚动效果）
    body.emit("wheel", { deltaY: -1 })
    scrollTop = 100
    body.emit("scroll")
    scrollSessionSticky(body as unknown as HTMLElement)
    expect(scrollTop).toBe(100) // 不再跟随
  })

  test("renderSessionArchive renders archive container with full process and nested runs (历史回放)", () => {
    const box = renderSessionArchive({
      runId: "run1",
      agents: ["code"],
      input: "改个文件",
      output: "**已完成**",
      messages: [
        { role: "user", content: "改个文件" },
        { role: "assistant", content: "先看一下", toolCalls: [{ id: "c1", name: "code_read", arguments: { path: "a.ts" } }] },
        { role: "tool", name: "code_read", content: "文件内容", toolCallId: "c1", arguments: { path: "a.ts" } },
        { role: "assistant", content: "**已完成**" },
      ],
    })
    // 已结束存档：默认折叠，摘要含最终返回，body 渲染完整过程消息
    expect(box.container.className).toContain("session-run")
    expect((box.container as HTMLDetailsElement).open).toBe(false)
    expect(box.outputEl.textContent).toContain("→ 返回")
    expect(box.body.querySelector(".msg")).toBeTruthy()
    // 嵌套执行存档：新会话内再 agent_run → 递归渲染进外层容器 body
    const combo = renderSessionArchive({
      runId: "run2",
      agents: ["combo"],
      input: "组合调用",
      output: "ok",
      messages: [
        { role: "user", content: "组合调用" },
        { role: "assistant", content: "委托给 code", toolCalls: [{ id: "c2", name: "agent_run", arguments: { agents: ["code"], input: "x" } }] },
        {
          role: "tool",
          name: "agent_run",
          content: "ok",
          toolCallId: "c2",
          sessionRun: {
            runId: "inner",
            agents: ["code"],
            input: "x",
            output: "内部结果",
            messages: [{ role: "user", content: "x" }, { role: "assistant", content: "内部结果" }],
          },
        },
      ],
    })
    const inner = combo.body.querySelector("details.session-run")
    expect(inner).toBeTruthy()
    expect(inner?.querySelector(".session-output")?.textContent).toContain("内部结果")
  })

  test("renderLegacySubAgentArchive renders old-format archive (agent 单值兼容回放)", () => {
    const box = renderLegacySubAgentArchive({
      runId: "old1",
      agent: "code",
      input: "x",
      output: "旧结果",
      messages: [{ role: "user", content: "x" }, { role: "assistant", content: "旧结果" }],
    })
    expect(box.container.className).toContain("session-run")
    expect(box.outputEl.textContent).toContain("旧结果")
    expect(box.body.querySelector(".msg")).toBeTruthy()
  })
})

describe("ask 问答记录卡（等待期消息流不预览问题，结果到达落记录卡）", () => {
  test("appendAskUserRecord 渲染问题 + 禁用选项 + 回答结果（头部按结果文案更新）", () => {
    const wrapper = appendAskUserRecord({ prompt: "选择方案", options: [{ title: "A", description: "方案A" }, "B"] }, "用户选择：A")
    expect(wrapper.className).toContain("msg")
    expect(wrapper.className).toContain("tool")
    expect(wrapper.querySelector("div.tool-head")?.textContent).toBe("✓ 用户回答")
    expect(wrapper.querySelector("div.block-text")?.textContent).toBe("选择方案")
    // 选项为禁用按钮静态展示（等待期交互作答由审批容器选择卡片承载，消息流不重复渲染问题卡）
    const opts = wrapper.querySelectorAll("button.choice-opt")
    expect(opts.length).toBe(2)
    for (const o of opts) expect((o as unknown as { disabled: boolean }).disabled).toBe(true)
    expect(wrapper.querySelector("div.choice-answer")?.textContent).toBe("用户选择：A")
  })

  test("appendAskUserRecord 选中选项高亮（selected）：多选命中全部标记，自定义文本不命中", () => {
    const wrapper = appendAskUserRecord({ prompt: "选择方案", options: ["A", "B", "C"], multi: true }, "用户选择：A、C")
    const opts = wrapper.querySelectorAll("button.choice-opt") as unknown as Array<{ classList: { contains(c: string): boolean } }>
    expect(opts.length).toBe(3)
    expect(opts[0].classList.contains("selected")).toBe(true)
    expect(opts[1].classList.contains("selected")).toBe(false)
    expect(opts[2].classList.contains("selected")).toBe(true)
    // 自定义文本回答不命中任何选项：无高亮，回答块仍示原文
    const custom = appendAskUserRecord({ prompt: "选", options: ["A", "B"] }, "用户选择：自己写的答案")
    for (const o of custom.querySelectorAll("button.choice-opt") as unknown as Array<{ classList: { contains(c: string): boolean } }>) {
      expect(o.classList.contains("selected")).toBe(false)
    }
    expect(custom.querySelector("div.choice-answer")?.textContent).toContain("自己写的答案")
  })

  test("askUserResultHead 按输出前缀识别结果态", () => {
    expect(askUserResultHead("用户选择：A")).toBe("✓ 用户回答")
    expect(askUserResultHead("用户选择：A、B")).toBe("✓ 用户回答")
    expect(askUserResultHead("用户拒绝了本次询问。")).toBe("✕ 用户拒绝")
    expect(askUserResultHead("用户未在时限内做出选择，已取消本次询问。")).toBe("⏱ 选择超时")
  })

  test("appendToolResult kind=ask_choice 按登记参数落问答记录卡（无 wrapper：等待期未渲染预览）", () => {
    // approvalsEl/msgEl 为共享 mock 基座（跨文件注册表可能指向其他文件的 no-op stub）：
    // runId + 显式 parent 把记录卡落进本地容器断言，不依赖共享 DOM
    const parent = makeMockEl("div")
    pendingTools.set(pendingToolsKey("s1", "tc1", "r1"), { session: "s1", kind: "ask_choice", runId: "r1", askArgs: { prompt: "选择方案", options: ["A", "B"], multi: false } })
    appendToolResult("s1", "tc1", "ask", "用户选择：A", undefined, "r1", parent as unknown as HTMLElement)
    expect(pendingTools.has(pendingToolsKey("s1", "tc1", "r1"))).toBe(false)
    const answers = parent.querySelectorAll("div.choice-answer") as unknown as MockEl[]
    expect(answers.length).toBe(1)
    expect(answers[0]?.textContent).toBe("用户选择：A")
    expect((parent.querySelector("div.tool-head") as unknown as MockEl | undefined)?.textContent).toBe("✓ 用户回答")
    pendingTools.clear()
  })

  test("appendToolResult 无配对 ask 兜底独立结果消息（不抛错、不残留配对）", () => {
    // 无配对（切走/重载场景）：appendMsg 兜底独立结果消息，无 DOM 断言（测试进程内
    // msgEl 可能指向其他测试文件的 mock，跨文件共享模块注册表导致断言不可靠）
    expect(() => appendToolResult("s1", "tc-unknown", "ask", "用户选择：B")).not.toThrow()
    expect(pendingTools.size).toBe(0)
  })
})

describe("ask 卡片按参数形态分流（options → 问答记录卡 / title → 计划卡 / 其余 → 通用卡）", () => {
  test("history card (toolCard): name=ask + options 参数渲染问答记录卡", () => {
    const bubble = toolBubbleFor(
      { id: "t1", role: "tool", name: "ask", content: "用户选择：B", arguments: { prompt: "选择方案", options: ["A", "B"] }, createdAt: 0 },
      "用户选择：B",
    )
    expect(bubble.querySelector("div.tool-head")?.textContent).toBe("✓ 用户回答")
    expect(bubble.querySelector("div.choice-answer")?.textContent).toBe("用户选择：B")
  })

  test("history card (toolCard): name=ask + title 参数渲染计划卡片", () => {
    const bubble = toolBubbleFor(
      { id: "t2", role: "tool", name: "ask", content: "计划已批准：「重构」。", arguments: { title: "重构", steps: ["a", "b"] }, createdAt: 0 },
      "计划已批准",
    )
    expect(bubble.querySelector("div.tool-head")?.textContent).toBe("✓ 计划已批准")
    expect(bubble.querySelector("div.markdown")).not.toBeNull()
  })

  test("history card (toolCard): name=ask + name 参数（填值分支）走通用工具卡不误判", () => {
    const bubble = toolBubbleFor(
      { id: "t3", role: "tool", name: "ask", content: "环境变量 MY_KEY 已由用户设置。", arguments: { name: "MY_KEY" }, createdAt: 0 },
      "已设置",
    )
    // 通用卡：非问答/计划形态，不命中问答/计划分支
    expect(bubble.querySelector("div.choice-answer")).toBeNull()
    expect(bubble.querySelector("div.markdown")).toBeNull()
  })
})

describe("选择卡片去重（同一 choiceId 重复推送替换旧卡，断线重放不堆叠）", () => {
  test("renderChoiceCard 同一 choiceId 二次推送只剩一张交互卡", () => {
    // approvalsEl 为共享 mock 基座（跨文件注册表可能指向其他文件的 no-op stub）：
    // 临时把 appendChild/querySelectorAll 改道到本地沙箱容器断言，测试后还原
    const sandbox = makeMockEl("div")
    const target = approvalsEl as unknown as Record<string, unknown>
    const origAppend = target.appendChild
    const origQsa = target.querySelectorAll
    target.appendChild = (n: unknown) => sandbox.appendChild(n)
    target.querySelectorAll = (sel: string) => (sel === ".interaction-card" ? sandbox.querySelectorAll(sel) : [])
    try {
      renderChoiceCard("选择方案", ["A", "B"], "cid1", "s1")
      renderChoiceCard("选择方案", ["A", "B"], "cid1", "s1")
      const cards = sandbox.querySelectorAll("div.interaction-card").filter((c) => (c as unknown as { dataset: Record<string, string> }).dataset.reqId === "cid1")
      expect(cards.length).toBe(1)
    } finally {
      target.appendChild = origAppend
      target.querySelectorAll = origQsa
    }
  })
})

describe("ask 计划卡片（消息流展示计划全文 + 审批结果更新）", () => {
  test("buildPlanMarkdown 与服务端同构：content 优先，否则 title + 勾选清单", () => {
    expect(buildPlanMarkdown("重构订单模块", ["梳理现状", "拆分接口"])).toBe(
      "# 重构订单模块\n\n## 执行计划\n\n- [ ] 梳理现状\n- [ ] 拆分接口",
    )
    const content = "# 迁移方案\n\n| 步骤 | 说明 |"
    expect(buildPlanMarkdown("数据库迁移", ["无关步骤"], content)).toBe(content)
  })

  test("appendPlanCard renders title + plan markdown container into the flow (展示态)", () => {
    const wrapper = appendPlanCard({ title: "重构订单模块", steps: ["梳理现状", "拆分接口"] })
    expect(wrapper.className).toContain("msg")
    expect(wrapper.className).toContain("tool")
    const head = wrapper.querySelector("div.tool-head")
    expect(head?.textContent).toContain("计划")
    expect(head?.textContent).toContain("重构订单模块")
    // 计划全文经 markdown 渲染容器展示（内容正确性由 buildPlanMarkdown 单测覆盖；测试 DOM mock 无 innerHTML）
    expect(wrapper.querySelector("div.markdown")).not.toBeNull()
  })

  test("appendToolResult kind=ask_plan 更新头部为审批结果并追加结果文本", () => {
    const wrapper = appendPlanCard({ title: "重构订单模块", steps: ["梳理现状"] })
    const body = wrapper.querySelector(".msg-body") as HTMLElement
    pendingTools.set(pendingToolsKey("s1", "tc1"), { wrapper, body, session: "s1", kind: "ask_plan" })
    appendToolResult("s1", "tc1", "plan", "计划已批准：「重构订单模块」。请严格按计划逐步执行。")
    expect(wrapper.querySelector("div.tool-head")?.textContent).toBe("✓ 计划已批准")
    expect(wrapper.querySelector("div.choice-answer")?.textContent).toContain("计划已批准")
    expect(pendingTools.has(pendingToolsKey("s1", "tc1"))).toBe(false)
    // 拒绝场景：头部与文本更新
    pendingTools.set(pendingToolsKey("s1", "tc2"), { wrapper, body, session: "s1", kind: "ask_plan" })
    appendToolResult("s1", "tc2", "plan", "计划已拒绝：「重构订单模块」。用户修改意见：缺少回归测试步骤。")
    expect(wrapper.querySelector("div.tool-head")?.textContent).toBe("✕ 计划已拒绝")
    pendingTools.clear()
  })

  test("planResultHead 按输出前缀映射审批结果（与服务端输出文案一致）", () => {
    expect(planResultHead("计划已批准：x")).toBe("✓ 计划已批准")
    expect(planResultHead("计划已拒绝：x")).toBe("✕ 计划已拒绝")
    expect(planResultHead("用户拒绝审核计划：x")).toBe("✕ 计划已取消")
    expect(planResultHead("计划审批超时：「x」（5 分钟未响应）。")).toBe("⏱ 计划审批超时")
    expect(planResultHead("计划文档保存失败：磁盘满")).toBe("✓ 计划已处理")
  })

  test("history card (toolCard): ask 计划分支历史消息渲染计划卡片（带审批结果态与结果文本）", () => {
    const bubble = toolBubbleFor(
      { id: "t1", role: "tool", name: "ask", content: "计划已批准：「重构订单模块」。请严格按计划逐步执行。", arguments: { title: "重构订单模块", steps: ["梳理现状", "迁移数据"] }, createdAt: 0 },
      "计划已批准",
    )
    // 历史重载：头部直接呈现审批结果态，结果文本追加（与实时流一致）
    expect(bubble.querySelector("div.tool-head")?.textContent).toBe("✓ 计划已批准")
    expect(bubble.querySelector("div.choice-answer")?.textContent).toContain("计划已批准")
    // 计划全文 markdown 容器（mock 无 innerHTML，结构断言即可）
    expect(bubble.querySelector("div.markdown")).not.toBeNull()
  })

  test("history card (toolCard): ask 计划分支无结果文本（仅调用）时保持计划卡样式", () => {
    const bubble = toolBubbleFor(
      { id: "t2", role: "tool", name: "ask", content: "", arguments: { title: "重构订单模块", steps: ["梳理现状"] }, createdAt: 0 },
      "",
    )
    expect(bubble.querySelector("div.tool-head")?.textContent).toBe("📋 计划 · 重构订单模块")
    expect(bubble.querySelector("div.choice-answer")).toBeNull()
  })
})

describe("agent_run 工具卡片（头部列全部预加载子Agent 名，参数区输入以块展示）", () => {
  test("history card (toolCard): 头部含全部预加载子Agent，参数区只显示输入", () => {
    const bubble = toolBubbleFor(
      { id: "t1", role: "tool", name: "agent_run", content: "结果", arguments: { agents: ["code", "playwright"], input: "改文件并验证" }, createdAt: 0 },
      "结果",
    )
    const head = bubble.querySelector("div.tool-head")
    expect(head?.textContent).toContain("agent_run")
    expect(head?.textContent).toContain("code + playwright")
    expect(head?.textContent).not.toContain("agents=")
    // 参数区输入以块展示（预加载子Agent 名已入标题，不重复渲染 agents JSON）
    const input = bubble.querySelector("div.agent-run-input")
    expect(input?.textContent).toBe("改文件并验证")
    expect(bubble.querySelector("div.tool-rest")).toBeNull()
  })

  test("realtime card (toolBubble): → agent_run 调用同样头部列全部子Agent + 下方输入", () => {
    const bubble = toolBubbleFor(
      { id: "t2", role: "tool", content: "", createdAt: 0 },
      `→ agent_run {"agents":["code","playwright","feishu_docs"],"input":"先改代码再写文档"}`,
    )
    const head = bubble.querySelector("div.tool-head")
    expect(head?.textContent).toContain("code + playwright + feishu_docs")
    const input = bubble.querySelector("div.agent-run-input")
    expect(input?.textContent).toBe("先改代码再写文档")
    expect(bubble.querySelector("div.agent-run-label")).toBeNull()
  })

  test("无输入（空 input）时不渲染参数区，仅头部列出子Agent", () => {
    const bubble = toolBubbleFor(
      { id: "t3", role: "tool", name: "agent_run", content: "", arguments: { agents: ["code"] }, createdAt: 0 },
      "",
    )
    expect(bubble.querySelector("div.agent-run-input")).toBeNull()
    expect(bubble.querySelector("div.tool-head")?.textContent).toContain("code")
  })
})

describe("工具卡片标题与参数区（灵活标题 + 自适应参数格式）", () => {
  test("单标题参数仅显示值（省略 key= 前缀），标题参数不在参数区重复", () => {
    __setToolCardMetaForTest([["read", { titleParams: ["path"] }]])
    const bubble = toolBubbleFor(
      { id: "tt1", role: "tool", name: "read", content: "", arguments: { path: "src/main.ts", offset: 2 }, createdAt: 0 },
      "",
    )
    const head = bubble.querySelector("div.tool-head")
    expect(head?.textContent).toContain("read")
    expect(head?.textContent).toContain("src/main.ts")
    expect(head?.textContent).not.toContain("path=")
    // path 已入标题：参数区只展示 offset（键值行），无 JSON 块
    const rows = bubble.querySelectorAll("div.tool-kv-row")
    expect(rows.length).toBe(1)
    expect(rows[0]?.textContent).toContain("offset")
    expect(rows[0]?.textContent).toContain("2")
    expect(bubble.querySelector("pre.tool-code")).toBeNull()
  })

  test("多标题参数 key=value 展示", () => {
    __setToolCardMetaForTest([["cfg", { titleParams: ["a", "b"] }]])
    const bubble = toolBubbleFor({ id: "tt2", role: "tool", name: "cfg", content: "", arguments: { a: "1", b: "x" }, createdAt: 0 }, "")
    const head = bubble.querySelector("div.tool-head")
    expect(head?.textContent).toContain("a=1")
    expect(head?.textContent).toContain("b=x")
  })

  test("project 参数入卡片头：传了显示 project=… · path=…（相对路径不再缺项目上下文），未传保持单值裸显", () => {
    __setToolCardMetaForTest([["read", { titleParams: ["project", "path"] }]])
    const withProj = toolBubbleFor({ id: "tp1", role: "tool", name: "read", content: "", arguments: { project: "todo-app", path: "src/main.ts" }, createdAt: 0 }, "")
    const head1 = withProj.querySelector("div.tool-head")
    expect(head1?.textContent).toContain("project=todo-app")
    expect(head1?.textContent).toContain("path=src/main.ts")
    // project 已入头部：参数区不重复展示（无键值行）
    expect(withProj.querySelectorAll("div.tool-kv-row").length).toBe(0)
    // 未传 project：实际存在的参数仅 path 一个 → 裸值显示（不出现 path= 前缀），与既有形态一致
    const noProj = toolBubbleFor({ id: "tp2", role: "tool", name: "read", content: "", arguments: { path: "src/main.ts" }, createdAt: 0 }, "")
    const head2 = noProj.querySelector("div.tool-head")
    expect(head2?.textContent).toContain("src/main.ts")
    expect(head2?.textContent).not.toContain("path=")
    expect(head2?.textContent).not.toContain("project")
  })

  test("超长标题参数智能截断入头部：路径型保留尾部，悬浮 title 见全文，参数区不重复", () => {
    const long = `very/long/path/${"nested/".repeat(20)}file.ts`
    __setToolCardMetaForTest([["read", { titleParams: ["path"] }]])
    const bubble = toolBubbleFor({ id: "tt3", role: "tool", name: "read", content: "", arguments: { path: long }, createdAt: 0 }, "")
    // 头部后缀保留（路径型截断保留尾部，不整体丢弃）
    const sfx = bubble.querySelector("span.tool-suffix") as unknown as { textContent: string; title?: string }
    expect(sfx).not.toBeNull()
    expect(sfx.textContent).toContain("…")
    expect(sfx.textContent).not.toBe(long)
    expect(sfx.textContent?.endsWith("file.ts")).toBe(true)
    // 悬浮 title 携带全文
    expect(sfx.title).toContain(long)
    // 标题参数已入头部：参数区不再以键值行重复展示
    expect(bubble.querySelectorAll("div.tool-kv-row").length).toBe(0)
  })

  test("超长 URL 型标题参数保留头部；其余参数仍键值行展示", () => {
    const url = `https://example.com/${"seg/".repeat(30)}page`
    __setToolCardMetaForTest([["fetch_url", { titleParams: ["url"], args: "none" }]])
    const bubble = toolBubbleFor({ id: "tt3b", role: "tool", name: "fetch_url", content: "", arguments: { url }, createdAt: 0 }, "")
    const sfx = bubble.querySelector("span.tool-suffix") as unknown as { textContent: string; title?: string }
    expect(sfx).not.toBeNull()
    expect(sfx.textContent).toContain("…")
    expect(sfx.textContent).toContain("https://example.com")
    expect(sfx.title).toContain(url)
  })

  test("无参数工具调用卡片：仅头部（🛠 工具名），无参数区", () => {
    __setToolCardMetaForTest([])
    const bubble = toolBubbleFor({ id: "tt3c", role: "tool", content: "", createdAt: 0 }, "→ ls")
    const head = bubble.querySelector("div.tool-head")
    expect(head?.textContent).toContain("🛠")
    expect(head?.textContent).toContain("ls")
    expect(bubble.querySelector("div.tool-kv")).toBeNull()
    expect(bubble.querySelector("pre.tool-code")).toBeNull()
  })

  test("嵌套参数回退 JSON 高亮块（无键值行）", () => {
    __setToolCardMetaForTest([])
    const bubble = toolBubbleFor({ id: "tt4", role: "tool", name: "some_tool", content: "", arguments: { cfg: { a: 1 } }, createdAt: 0 }, "")
    expect(bubble.querySelector("pre.tool-code")).not.toBeNull()
    expect(bubble.querySelector("div.tool-kv")).toBeNull()
  })

  test("超长参数自动折叠为「查看参数」折叠块", () => {
    __setToolCardMetaForTest([])
    const bubble = toolBubbleFor({ id: "tt5", role: "tool", name: "some_tool", content: "", arguments: { text: "x".repeat(1000) }, createdAt: 0 }, "")
    const fold = bubble.querySelector("details.tool-fold")
    expect(fold).not.toBeNull()
    expect(fold?.textContent).toContain("查看参数")
  })

  test("显式 json 声明：标题参数不省略、键值行不生效（完整 JSON 保真展示）", () => {
    __setToolCardMetaForTest([["read", { titleParams: ["path"], args: "json" }]])
    const bubble = toolBubbleFor({ id: "tt6", role: "tool", name: "read", content: "", arguments: { path: "a.ts" }, createdAt: 0 }, "")
    expect(bubble.querySelector("pre.tool-code")).not.toBeNull()
    expect(bubble.querySelector("div.tool-kv")).toBeNull()
  })

  test("显式 kv 声明：嵌套值紧凑 JSON 单行展示", () => {
    __setToolCardMetaForTest([["cfg", { args: "kv" }]])
    const bubble = toolBubbleFor({ id: "tt7", role: "tool", name: "cfg", content: "", arguments: { opts: { a: 1 } }, createdAt: 0 }, "")
    expect(bubble.querySelector("pre.tool-code")).toBeNull()
    const rows = bubble.querySelectorAll("div.tool-kv-row")
    expect(rows.length).toBe(1)
    expect(rows[0]?.textContent).toContain("opts")
    expect(rows[0]?.textContent).toContain('{"a":1}')
  })
})

describe("edit 工具 edits 参数模式（旧/新对比块，替代 JSON）", () => {
  const meta: Array<[string, NonNullable<ToolInfo["card"]>]> = [["edit", { titleParams: ["path"], args: "edits", codeField: "edits" }]]

  test("多处修改：编号 + 每处旧/新块，path 入标题不重复", () => {
    __setToolCardMetaForTest(meta)
    const bubble = toolBubbleFor(
      {
        id: "te1",
        role: "tool",
        name: "edit",
        content: "",
        arguments: { path: "src/a.ts", edits: [{ oldString: "foo", newString: "bar" }, { oldString: "x", newString: "y" }] },
        createdAt: 0,
      },
      "",
    )
    const head = bubble.querySelector("div.tool-head")
    expect(head?.textContent).toContain("edit")
    expect(head?.textContent).toContain("src/a.ts")
    // 无 JSON 块；两处修改各有编号与旧/新块
    expect(bubble.querySelector("pre.tool-code")).toBeNull()
    const idx = bubble.querySelectorAll("div.tool-edit-idx")
    expect(idx.length).toBe(2)
    expect(idx[0]?.textContent).toBe("修改 1/2")
    const olds = bubble.querySelectorAll("pre.tool-edit-old")
    const news = bubble.querySelectorAll("pre.tool-edit-new")
    expect(olds.length).toBe(2)
    expect(news.length).toBe(2)
    expect(olds[0]?.textContent).toBe("foo")
    expect(news[0]?.textContent).toBe("bar")
  })

  test("单处修改无编号；纯新增只显示新块", () => {
    __setToolCardMetaForTest(meta)
    const bubble = toolBubbleFor(
      { id: "te2", role: "tool", name: "edit", content: "", arguments: { path: "a.ts", edits: [{ oldString: "", newString: "added" }] }, createdAt: 0 },
      "",
    )
    expect(bubble.querySelector("div.tool-edit-idx")).toBeNull()
    expect(bubble.querySelector("pre.tool-edit-old")).toBeNull()
    expect(bubble.querySelector("pre.tool-edit-new")?.textContent).toBe("added")
  })

  test("edits 形态不符（非字符串对数组）回退 JSON 高亮块", () => {
    __setToolCardMetaForTest(meta)
    const bubble = toolBubbleFor({ id: "te3", role: "tool", name: "edit", content: "", arguments: { path: "a.ts", edits: { bad: 1 } }, createdAt: 0 }, "")
    expect(bubble.querySelector("div.tool-edits")).toBeNull()
    expect(bubble.querySelector("pre.tool-code")).not.toBeNull()
  })

  test("超长修改内容自动折叠", () => {
    __setToolCardMetaForTest(meta)
    const bubble = toolBubbleFor(
      { id: "te4", role: "tool", name: "edit", content: "", arguments: { path: "a.ts", edits: [{ oldString: "o".repeat(500), newString: "n".repeat(500) }] }, createdAt: 0 },
      "",
    )
    const fold = bubble.querySelector("details.tool-fold")
    expect(fold).not.toBeNull()
    expect(fold?.textContent).toContain("查看参数")
    // 折叠块内仍是旧/新对比块
    expect(fold?.querySelectorAll("pre.tool-edit-old").length).toBe(1)
  })

  test("card 声明缺失（清单拉取失败/旧服务端）按参数形态兜底渲染对比块，不直显 JSON", () => {
    __setToolCardMetaForTest([])
    const bubble = toolBubbleFor(
      { id: "te5", role: "tool", name: "edit", content: "", arguments: { path: "src/a.ts", edits: [{ oldString: "foo", newString: "bar" }] }, createdAt: 0 },
      "",
    )
    expect(bubble.querySelector("div.tool-edits")).not.toBeNull()
    expect(bubble.querySelector("pre.tool-edit-old")?.textContent).toBe("foo")
    expect(bubble.querySelector("pre.tool-code")).toBeNull()
    // 其余参数（path）键值行展示
    expect(bubble.querySelector("div.tool-kv-row")?.textContent).toContain("path")
  })

  test("兜底渲染的超长修改同样先渲染后折叠", () => {
    __setToolCardMetaForTest([])
    const bubble = toolBubbleFor(
      { id: "te6", role: "tool", name: "edit", content: "", arguments: { path: "a.ts", edits: [{ oldString: "o".repeat(600), newString: "n".repeat(600) }] }, createdAt: 0 },
      "",
    )
    const fold = bubble.querySelector("details.tool-fold")
    expect(fold).not.toBeNull()
    expect(fold?.querySelectorAll("pre.tool-edit-new").length).toBe(1)
  })
})

describe("code 参数折叠（write/patch/js 等长内容默认收起）", () => {
  test("超长 code 参数自动折叠", () => {
    __setToolCardMetaForTest([["write", { titleParams: ["path"], args: "code", codeField: "content" }]])
    const bubble = toolBubbleFor({ id: "tc1", role: "tool", name: "write", content: "", arguments: { path: "a.ts", content: "x".repeat(900) }, createdAt: 0 }, "")
    expect(bubble.querySelector("details.tool-fold")).not.toBeNull()
  })

  test("短 code 参数不折叠", () => {
    __setToolCardMetaForTest([["write", { titleParams: ["path"], args: "code", codeField: "content" }]])
    const bubble = toolBubbleFor({ id: "tc2", role: "tool", name: "write", content: "", arguments: { path: "a.ts", content: "short" }, createdAt: 0 }, "")
    expect(bubble.querySelector("details.tool-fold")).toBeNull()
  })
})

describe("ask 历史回放（带结果渲染问答记录卡）", () => {
  test("带结果（content）渲染问答记录卡：头部结果态 + 回答块，选项禁用展示", () => {
    __setToolCardMetaForTest([])
    const bubble = toolBubbleFor(
      { id: "ta1", role: "tool", name: "ask", content: "用户选择：A", arguments: { prompt: "选哪个方案", options: ["A", "B"] }, createdAt: 0 },
      "",
    )
    expect(bubble.querySelector("div.tool-head")?.textContent).toBe("✓ 用户回答")
    expect(bubble.querySelector("div.block-text")?.textContent).toBe("选哪个方案")
    expect(bubble.querySelector("div.choice-answer")?.textContent).toBe("用户选择：A")
    // 展示态：无自定义输入/拒绝按钮（不再重复可交互选择卡）
    expect(bubble.querySelector("div.choice-custom")).toBeNull()
    expect(bubble.querySelector("button.choice-refuse")).toBeNull()
  })

  test("无结果的裸调用（异常中断）回退可交互选择卡", () => {
    __setToolCardMetaForTest([])
    const bubble = toolBubbleFor(
      { id: "ta2", role: "tool", name: "ask", content: "", arguments: { prompt: "选哪个方案", options: ["A", "B"] }, createdAt: 0 },
      "",
    )
    expect(bubble.querySelector("div.choice-custom")).not.toBeNull()
    expect(bubble.querySelector("button.choice-refuse")).not.toBeNull()
  })
})

/* ---------- 文件展示方式（弹窗查看/直接展示，card.file 声明驱动） ---------- */

/** localStorage 桩：弹窗查看模式（file-display.ts 读取时机在渲染期，桩可按测试切换）。 */
function setFileDisplayStub(v: "inline" | "popup") {
  ;(globalThis as Record<string, unknown>).localStorage = {
    getItem: (k: string) => (k === "gebai.ui.fileDisplay" && v === "popup" ? "popup" : null),
    setItem: () => {},
    removeItem: () => {},
  }
}
describe("文件展示方式（弹窗查看：文件工具产物 file 块收敛为文件链接，参数区与输出不受影响）", () => {
  afterEach(() => setFileDisplayStub("inline"))

  test("read 弹窗模式：参数/输出照常渲染，产物 file 块 → 文件链接 chip（路径为解析后块路径）", () => {
    setFileDisplayStub("popup")
    __setToolCardMetaForTest([["read", { titleParams: ["path"], file: "path" }]])
    const bubble = toolBubbleFor(
      {
        id: "tp1",
        role: "tool",
        name: "read",
        content: "const a = 1\nconst b = 2",
        arguments: { path: "src/a.ts", offset: 2 },
        blocks: [{ type: "file", path: "tmp/src/a.ts", name: "a.ts", mime: "text/plain" }],
        createdAt: 0,
      },
      "",
    )
    // 参数区不受影响：offset 键值行照常、头部路径后缀照常
    const rows = bubble.querySelectorAll("div.tool-kv-row")
    expect(rows.length).toBe(1)
    expect(rows[0]?.textContent).toContain("offset")
    // 输出不受影响：文件内容文本照常直显
    expect(bubble.textContent).toContain("const a = 1")
    // 产物 file 块不在气泡内——由 appendMsg 层分流（此处断言气泡内无文件内容卡）
    expect(bubble.querySelector("div.file-card")).toBeNull()
  })

  test("edit 弹窗模式：旧/新对比块照常渲染（参数区不受影响）", () => {
    setFileDisplayStub("popup")
    __setToolCardMetaForTest([["edit", { titleParams: ["path"], args: "edits", codeField: "edits", file: "path" }]])
    const bubble = toolBubbleFor(
      { id: "tp2", role: "tool", name: "edit", content: "已对 a.ts 应用 1 处修改", arguments: { path: "a.ts", edits: [{ oldString: "foo", newString: "bar" }] }, createdAt: 0 },
      "",
    )
    expect(bubble.querySelector("pre.tool-edit-old")).not.toBeNull()
    expect(bubble.textContent).toContain("已对 a.ts 应用")
    expect(bubble.querySelector("div.file-link")).toBeNull()
  })

  test("弹窗模式分流（fileBlocksAsLinks）：文件工具 true，未声明/关闭模式 false", async () => {
    __setToolCardMetaForTest([["read", { titleParams: ["path"], file: "path" }]])
    const { fileBlocksAsLinks } = await import("./tool-cards")
    setFileDisplayStub("popup")
    expect(fileBlocksAsLinks("read")).toBe(true)
    expect(fileBlocksAsLinks(undefined)).toBe(false)
    expect(fileBlocksAsLinks("show")).toBe(false) // 非文件卡工具（show 为主动展示）不受影响
    setFileDisplayStub("inline")
    expect(fileBlocksAsLinks("read")).toBe(false)
  })

  test("appendMsg 产物块分流：弹窗模式 file 块 → chip（图片等其余块照常），嵌入模式 → 文件内容卡", async () => {
    const { appendMsg } = await import("./messages")
    __setToolCardMetaForTest([["read", { titleParams: ["path"], file: "path" }]])
    const blocks = [
      { type: "file", path: "tmp/src/a.ts", name: "a.ts", mime: "text/plain" },
    ] as never
    const mk = () => appendMsg({ id: "tp3", role: "tool", name: "read", content: "ok", arguments: { path: "src/a.ts" }, blocks, createdAt: 0 })
    setFileDisplayStub("popup")
    const popupWrap = mk()
    const chip = popupWrap.querySelector(".file-link") as unknown as { dataset: Record<string, string>; textContent: string }
    expect(chip).not.toBeNull()
    expect(chip.dataset.path).toBe("tmp/src/a.ts")
    expect(chip.textContent).toContain("a.ts")
    expect(popupWrap.querySelector("div.file-card")).toBeNull()
    setFileDisplayStub("inline")
    const inlineWrap = mk()
    expect(inlineWrap.querySelector("div.file-card")).not.toBeNull()
    expect(inlineWrap.querySelector(".file-link")).toBeNull()
  })

  test("renderBlocksLinked：file 块 → chip，image 块照常内联渲染", async () => {
    const { renderBlocksLinked } = await import("./file-link")
    const container = makeMockEl("div")
    renderBlocksLinked(
      container as unknown as HTMLElement,
      [
        { type: "file", path: "tmp/a.ts", name: "a.ts" },
        { type: "image", path: "tmp/b.png", name: "b.png", mime: "image/png" },
      ],
      (c, b) => {
        const d = makeMockEl("div")
        d.className = `rendered-${b.type}`
        ;(c as unknown as MockEl).appendChild(d)
      },
      "s1",
    )
    expect(container.querySelector("div.file-link")).not.toBeNull()
    expect(container.querySelector("div.rendered-image")).not.toBeNull()
    expect(container.querySelector("div.rendered-file")).toBeNull()
  })
})

describe("原文件查看弹窗（previewShell：标题栏下载入口）", () => {
  test("弹窗头部含下载按钮（files/preview 附件形式），图片类型不取数直接内联", async () => {
    const { openFilePreview } = await import("./file-card")
    openFilePreview("s1", "shot.png", "tmp/shot.png", "image/png")
    const overlay = base.querySelector("div.preview-overlay") as unknown as MockElWithQuery
    expect(overlay).not.toBeNull()
    const dl = overlay.querySelector("a.preview-dl") as unknown as { href: string; download: string }
    expect(dl).not.toBeNull()
    expect(dl.href).toContain("files/preview?path=tmp%2Fshot.png")
    expect(dl.href).toContain("download=1")
    expect(dl.download).toBe("shot.png")
    expect(overlay.querySelector("img")).not.toBeNull()
  })
})
