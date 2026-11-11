/**
 * 拟人面板：**人格 / 通道 两个标签页**的划分，以及渠道监听开关的渲染条件。
 *
 * 这几条为什么值得钉住：
 *
 * 1. **分页归属** —— 渠道类内容放错到人格页，或反之，用户就又要“滑过一整页找开关”。
 *    属于哪一页的判据是「它服务哪个对象」，不是历史上写在哪个位置。
 * 2. **开关只在 canListen 时出现** —— 出站实例本来就不建连接，给它一个“开监听”
 *    按钮，用户点下去只会得到一个 400；而静默不给按钮又会被当成「本来就没有」。
 * 3. **只停监听** 的文案必须写在按钮旁边 —— 否则用户以为会把整个渠道下线，
 *    不敢点。
 */
import { describe, expect, test } from "bun:test"
import { renderPersonaPanel, type PersonaStateView } from "./persona-panel"

// ── 极简 DOM 桩（面板只用到这几个 API） ──
class StubEl {
  tag: string
  className = ""
  textContent = ""
  style: Record<string, string> = {}
  children: StubEl[] = []
  dataset: Record<string, string> = {}
  disabled = false
  type = ""
  onclick: (() => void) | null = null
  private handlers: Array<() => void> = []
  constructor(tag: string, className?: string, text?: string) {
    this.tag = tag
    if (className) this.className = className
    if (text !== undefined) this.textContent = text
  }
  append(...kids: Array<StubEl | string>): void {
    for (const k of kids) {
      if (typeof k === "string") this.children.push(new StubEl("#text", "", k))
      else this.children.push(k)
    }
  }
  appendChild(k: StubEl): StubEl {
    this.children.push(k)
    return k
  }
  addEventListener(_ev: string, fn: () => void): void {
    this.handlers.push(fn)
  }
  click(): void {
    for (const h of this.handlers) h()
    this.onclick?.()
  }
  classList = {
    toggle: (name: string, on: boolean) => {
      const set = new Set(this.className.split(/\s+/).filter(Boolean))
      if (on) set.add(name)
      else set.delete(name)
      this.className = [...set].join(" ")
    },
    add: (name: string) => {
      this.className = `${this.className} ${name}`.trim()
    },
    contains: (name: string) => this.className.split(/\s+/).includes(name),
  }
  /** 递归取文本（断言用）。 */
  text(): string {
    return this.textContent + this.children.map((c) => c.text()).join(" ")
  }
  /** 递归找元素（断言用）。 */
  find(pred: (e: StubEl) => boolean): StubEl | null {
    if (pred(this)) return this
    for (const c of this.children) {
      const hit = c.find(pred)
      if (hit) return hit
    }
    return null
  }
  findAll(pred: (e: StubEl) => boolean, acc: StubEl[] = []): StubEl[] {
    if (pred(this)) acc.push(this)
    for (const c of this.children) c.findAll(pred, acc)
    return acc
  }
}

// 面板模块依赖全局 document.createElement。
//
// **不能**在模块顶层直接改 globalThis.document：bun test 把同一次运行的所有测试
// 放在一个进程里，改完不还原就会让别的测试（滚动诊断、角标渲染等）拿到假 DOM，
// 表现为一堆与本次改动无关的失败（实际踩过）。
// 所以只在 render() 期间临时接管，返回后立刻还原。
const realDocument = (globalThis as unknown as { document?: unknown }).document
function withStubDocument<T>(fn: () => T): T {
  ;(globalThis as unknown as { document: unknown }).document = {
    createElement: (tag: string) => new StubEl(tag),
  }
  try {
    return fn()
  } finally {
    ;(globalThis as unknown as { document?: unknown }).document = realDocument
  }
}

function baseSnapshot(over: Partial<PersonaStateView> = {}): PersonaStateView {
  return {
    enabled: true,
    personas: [
      {
        id: "xiaoshuang",
        name: "小霜",
        drivers: [
          {
            id: "support",
            name: "值班",
            focus: "值班分身",
            subscriptions: ["feishu"],
            active: true,
            running: 0,
            counters: { accepted: 0, deduped: 0, overflowed: 0, failed: 0 },
            schedules: [{ id: "health", cron: "0 9 * * 1-5", description: "体检" }],
          },
        ],
      },
    ],
    sources: [],
    sourceErrors: {},
    channelInput: {},
    totals: { drivers: 1, activeDrivers: 1, running: 0, queued: 0, accepted: 0, deduped: 0 },
    credentials: { ready: true, state: "ready", message: "授权可用" },
    workers: [],
    ...over,
  } as unknown as PersonaStateView
}

function render(snapshot: PersonaStateView): StubEl {
  return withStubDocument(() => {
    const host = new StubEl("div")
    renderPersonaPanel(host as unknown as HTMLElement, snapshot)
    return host
  })
}

/** 带定时控制面行渲染（同样包在桩 DOM 里）。 */
function renderWithSchedules(snapshot: PersonaStateView, rows: unknown[]): StubEl {
  return withStubDocument(() => {
    const host = new StubEl("div")
    renderPersonaPanel(host as unknown as HTMLElement, snapshot, undefined, rows as never)
    return host
  })
}

const tabLabels = (host: StubEl): string[] =>
  host.findAll((e) => e.className.split(/\s+/).includes("persona-tab")).map((e) => e.textContent)

const paneOf = (host: StubEl, idx: number): StubEl =>
  host.findAll((e) => e.className.split(/\s+/).includes("persona-pane"))[idx]!

describe("标签页：人格 / 通道", () => {
  test("只有两个标签，且顺序是「人格」在前", () => {
    const host = render(baseSnapshot())
    const labels = tabLabels(host)
    console.log(`    实测标签: ${JSON.stringify(labels)}`)
    expect(labels).toEqual(["人格", "通道"])
  })

  test("默认停在人格页（其余页 display:none）", () => {
    const host = render(
      baseSnapshot({
        channelState: [{ name: "feishu_in", description: "飞书", enabled: true, started: true, listening: true, dir: "in", canListen: true }],
      }),
    )
    const tabs = host.findAll((e) => e.className.split(/\s+/).includes("persona-tab"))
    const panes = host.findAll((e) => e.className.split(/\s+/).includes("persona-tab-pane"))
    expect(tabs[0]!.classList.contains("persona-tab-active")).toBe(true)
    expect(tabs[1]!.classList.contains("persona-tab-active")).toBe(false)
    expect(panes[0]!.style.display).toBe("")
    expect(panes[1]!.style.display).toBe("none")
  })

  test("点第二个标签 → 切到通道页", () => {
    const host = render(
      baseSnapshot({
        channelState: [{ name: "feishu_in", description: "飞书", enabled: true, started: true, listening: true, dir: "in", canListen: true }],
      }),
    )
    const tabs = host.findAll((e) => e.className.split(/\s+/).includes("persona-tab"))
    const panes = host.findAll((e) => e.className.split(/\s+/).includes("persona-tab-pane"))
    tabs[1]!.click()
    expect(tabs[1]!.classList.contains("persona-tab-active")).toBe(true)
    expect(panes[1]!.style.display).toBe("")
    expect(panes[0]!.style.display).toBe("none")
  })

  test("归属：渠道实例在通道页，人格/定时/派活在人格页", () => {
    const host = render(
      baseSnapshot({
        channelState: [
          { name: "feishu_in", description: "飞书入站", enabled: true, started: true, listening: true, dir: "in", canListen: true },
        ],
      }),
    )
    const personaPane = paneOf(host, 0)
    const channelPane = paneOf(host, 1)
    // 通道页：有渠道实例名
    expect(channelPane.text()).toContain("feishu_in")
    // 人格页：有分身与定时任务（不该出现在通道页）
    expect(personaPane.text()).toContain("值班")
    expect(personaPane.text()).toContain("0 9 * * 1-5")
    expect(channelPane.text()).not.toContain("0 9 * * 1-5")
  })
})

describe("渠道监听开关", () => {
  const inChan = { name: "feishu_in", description: "飞书", enabled: true, started: true, listening: true, dir: "in" as const, canListen: true }
  const outChan = { name: "feishu_out", description: "飞书出", enabled: true, started: true, listening: false, dir: "out" as const, canListen: false }

  test("canListen=true → 有开关，按钮文案「停监听」，并写明只管收", () => {
    const host = render(baseSnapshot({ channelState: [inChan] }))
    const channelPane = paneOf(host, 1)
    const btn = channelPane.find((e) => e.className === "mini-btn" && e.textContent === "停监听")
    console.log(`    实测: 找到按钮=${btn !== null}`)
    expect(btn).not.toBeNull()
    expect(channelPane.text()).toContain("只停收消息")
    expect(channelPane.text()).toContain("仅本次运行有效")
  })

  test("已停监听 → 按钮变「开监听」", () => {
    const host = render(baseSnapshot({ channelState: [{ ...inChan, listening: false }] }))
    const channelPane = paneOf(host, 1)
    expect(channelPane.find((e) => e.className === "mini-btn" && e.textContent === "开监听")).not.toBeNull()
  })

  test("出站实例（canListen=false）→ 不给开关", () => {
    const host = render(baseSnapshot({ channelState: [outChan] }))
    const channelPane = paneOf(host, 1)
    const btn = channelPane.find((e) => e.textContent === "开监听" || e.textContent === "停监听")
    expect(btn).toBeNull()
  })

  test("入站但 canListen=false（渠道类型没实现 startListen）→ 明确说明，不静默", () => {
    const host = render(baseSnapshot({ channelState: [{ ...inChan, canListen: false }] }))
    const channelPane = paneOf(host, 1)
    expect(channelPane.text()).toContain("不支持手动启停监听")
  })
})

describe("定时任务：启停与立即运行仍在人格页", () => {
  test("声明列出 + 「停用」按钮（控制面认得该 key 时）", () => {
    const host = render(
      baseSnapshot(),
    )
    // 没传 schedules → 控制面行缺失 → 不给按钮（宁可不给，也不给点了没用的）
    const personaPane = paneOf(host, 0)
    expect(personaPane.text()).toContain("体检")
    const toggle = personaPane.find((e) => e.textContent === "停用" || e.textContent === "启用")
    expect(toggle).toBeNull()
  })

  test("传了控制面行 → 有「停用」与「立即跑一次」", () => {
    const host = renderWithSchedules(baseSnapshot(), [
      { key: "xiaoshuang/support/health", enabled: true, personaId: "xiaoshuang", driverId: "support", scheduleId: "health", cron: "0 9 * * 1-5" },
    ])
    const personaPane = paneOf(host, 0)
    expect(personaPane.find((e) => e.textContent === "停用")).not.toBeNull()
    expect(personaPane.find((e) => e.textContent === "立即跑一次")).not.toBeNull()
  })
})
