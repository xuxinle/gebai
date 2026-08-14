import { describe, expect, mock, test } from "bun:test"
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
}
function makeMockEl(tag = "div"): MockEl {
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
      for (const n of nodes) if (n && typeof n === "object") el.children.push(n as MockEl)
    },
    appendChild(n: unknown) {
      if (n && typeof n === "object") el.children.push(n as MockEl)
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
  return new Proxy(el, {
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
const { sealSegment, sessionRunBox, finishSessionRun, sealSessionSegment, sealBlockResultSegment, bindSessionScroll, scrollSessionSticky, renderSessionArchive, renderLegacySubAgentArchive, renderBlock, appendAskUserCard, appendToolResult } = await import("./messages")
const { runs, pendingTools, pendingToolsKey } = await import("./state")
const { isBlockOnly, toolBubbleFor, __setToolCardMetaForTest } = await import("./tool-cards")

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
      ["draw", { args: "block" }],
      ["write", { args: "code", codeField: "content" }],
      ["playwright_open", { args: "block" }],
    ])
    // block 声明命中
    expect(isBlockOnly("draw")).toBe(true)
    expect(isBlockOnly("playwright_open")).toBe(true) // 子 Agent 全名
    // 非 block 声明 / 未声明：false
    expect(isBlockOnly("write")).toBe(false)
    expect(isBlockOnly("current_time")).toBe(false)
    expect(isBlockOnly("open")).toBe(false) // 未注册短名
    expect(isBlockOnly("")).toBe(false)
  })

  test("resets with empty cache (未声明回退默认渲染)", () => {
    __setToolCardMetaForTest([])
    expect(isBlockOnly("draw")).toBe(false)
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
    expect(img.src).toContain("files/content?path=tmp%2Fflow.png")
    expect(typeof img.onclick).toBe("function")
    expect(img.dataset.tip).toBe("点击查看大图")
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
    // 伪造可滚动 body（scrollTop/scrollHeight/clientHeight 可读写，监听记录回调）
    let listener: (() => void) | null = null
    const body = {
      dataset: {} as Record<string, string>,
      scrollTop: 0,
      scrollHeight: 100,
      clientHeight: 50,
      addEventListener: (_: string, cb: () => void) => {
        listener = cb
      },
    }
    bindSessionScroll(body as unknown as HTMLElement)
    // 初始跟随：内容增长 → 滚到底部
    body.scrollHeight = 200
    scrollSessionSticky(body as unknown as HTMLElement)
    expect(body.scrollTop).toBe(200)
    // 用户上翻（未贴底）：停止跟随
    body.scrollTop = 40
    listener!()
    body.scrollHeight = 300
    scrollSessionSticky(body as unknown as HTMLElement)
    expect(body.scrollTop).toBe(40) // 不再跟随
    // 用户滚回底部：恢复跟随
    body.scrollTop = 250
    body.scrollHeight = 300
    listener!()
    scrollSessionSticky(body as unknown as HTMLElement)
    expect(body.scrollTop).toBe(300)
  })

  test("scrollSessionSticky delayed program scroll event after content growth keeps following (工具卡片追加场景)", () => {
    // 伪造可滚动 body：scrollTop 按浏览器语义 clamp 到 scrollHeight - clientHeight
    let listener: (() => void) | null = null
    let scrollTop = 0
    const body = {
      dataset: {} as Record<string, string>,
      clientHeight: 50,
      get scrollTop() {
        return scrollTop
      },
      set scrollTop(v: number) {
        scrollTop = Math.max(0, Math.min(v, body.scrollHeight - body.clientHeight))
      },
      scrollHeight: 200,
      addEventListener: (_: string, cb: () => void) => {
        listener = cb
      },
    }
    bindSessionScroll(body as unknown as HTMLElement)
    // 容器内流式内容 + 工具卡片追加：程序滚动后、scroll 事件送达前内容增长
    scrollSessionSticky(body as unknown as HTMLElement)
    expect(scrollTop).toBe(150) // clamp 落位（200-50）
    body.scrollHeight = 600 // 工具卡片在事件送达前追加
    listener!() // 迟到的程序滚动事件（位置 150 已不在当前底部）
    expect(scrollTop).toBe(550) // 识别为程序滚动：续滚到最新底部，跟随未失效
    expect(body.dataset.sticky).toBe("1")
    // 用户上翻仍能解除跟随
    scrollTop = 100
    listener!()
    expect(body.dataset.sticky).toBe("0")
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

describe("ask_user 消息流问答卡片（像 draw 一样中断并开启输出卡片）", () => {
  test("appendAskUserCard renders question + static options into the flow (展示态不可交互)", () => {
    const wrapper = appendAskUserCard("选择方案", [{ title: "A", description: "方案A" }, "B"], false)
    expect(wrapper.className).toContain("msg")
    expect(wrapper.className).toContain("tool")
    const head = wrapper.querySelector("div.tool-head")
    expect(head?.textContent).toContain("请选择")
    expect(wrapper.querySelector("div.block-text")?.textContent).toBe("选择方案")
    // 选项为禁用按钮静态展示（交互作答由审批容器选择卡片承载）
    const opts = wrapper.querySelectorAll("button.choice-opt")
    expect(opts.length).toBe(2)
    for (const o of opts) expect((o as HTMLButtonElement).disabled).toBe(true)
  })

  test("appendToolResult kind=ask_user 更新头部为完成态并追加回答", () => {
    const wrapper = appendAskUserCard("选择方案", ["A", "B"], false)
    const body = wrapper.querySelector(".msg-body") as HTMLElement
    pendingTools.set(pendingToolsKey("s1", "tc1"), { wrapper, body, session: "s1", kind: "ask_user" })
    appendToolResult("s1", "tc1", "ask_user", "用户选择：A")
    expect(wrapper.querySelector("div.tool-head")?.textContent).toBe("✓ 用户回答")
    expect(wrapper.querySelector("div.choice-answer")?.textContent).toBe("用户选择：A")
    expect(pendingTools.has(pendingToolsKey("s1", "tc1"))).toBe(false)
    pendingTools.clear()
  })

  test("appendToolResult 无配对 ask_user 兜底独立结果消息（不抛错、不残留配对）", () => {
    // 无配对（切走/重载场景）：appendMsg 兜底独立结果消息，无 DOM 断言（测试进程内
    // msgEl 可能指向其他测试文件的 mock，跨文件共享模块注册表导致断言不可靠）
    expect(() => appendToolResult("s1", "tc-unknown", "ask_user", "用户选择：B")).not.toThrow()
    expect(pendingTools.size).toBe(0)
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

  test("超长标题参数不缩略：头部不放后缀，降级为参数气泡（键值行全文展示）", () => {
    const long = `very/long/path/${"nested/".repeat(20)}file.ts`
    __setToolCardMetaForTest([["read", { titleParams: ["path"] }]])
    const bubble = toolBubbleFor({ id: "tt3", role: "tool", name: "read", content: "", arguments: { path: long }, createdAt: 0 }, "")
    // 头部只显示工具名（后缀全文超长，一行放不下，不放入头部也不缩略）
    expect(bubble.querySelector("span.tool-suffix")).toBeNull()
    // 参数气泡：键值行展示 key + 完整路径（不截断）
    const rows = bubble.querySelectorAll("div.tool-kv-row")
    expect(rows.length).toBe(1)
    expect(rows[0]?.textContent).toContain("path")
    expect(rows[0]?.textContent).toContain(long)
    expect(bubble.querySelector("pre.tool-code")).toBeNull()
  })

  test("超长标题参数与其余参数一并展示为键值行（不重复、不省略）", () => {
    const long = `${"sub/".repeat(30)}deep/file.ts`
    __setToolCardMetaForTest([["read", { titleParams: ["path"] }]])
    const bubble = toolBubbleFor({ id: "tt3b", role: "tool", name: "read", content: "", arguments: { path: long, offset: 5 }, createdAt: 0 }, "")
    expect(bubble.querySelector("span.tool-suffix")).toBeNull()
    const rows = bubble.querySelectorAll("div.tool-kv-row")
    expect(rows.length).toBe(2)
    expect(rows[0]?.textContent).toContain(long)
    expect(rows[1]?.textContent).toContain("offset")
    expect(rows[1]?.textContent).toContain("5")
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
})
