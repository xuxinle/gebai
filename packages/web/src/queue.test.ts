/**
 * 排队条渲染（queue.ts）：**增量对齐**而非整体重建。
 *
 * 背景：排队数据被推送的频率远高于它真正变化的频率——`state.snapshot` 每次都携带全量 `queues`，
 * 页面可见时自检每 3 秒一拍。整体重建的后果是「编辑排队项时随会话更新反复跳动」：编辑框被销毁、
 * 已输入未保存的文字被抹回服务端旧值、焦点与光标丢失。本文件锁住这几条不变式：
 *
 * 1. 内容没变的重复推送**一个 DOM 都不动**（节点同一性 + textarea 同一性）；
 * 2. 编辑中的条目只认 textarea 草稿，服务端推来的正文不得覆盖；
 * 3. 保存后本地镜像先落新正文，服务端回推前不回闪旧文字；
 * 4. 切会话整条重建、编辑态清空；条目撤回后就地移除并重排序号。
 */
import { afterAll, describe, expect, test } from "bun:test"
import type { QueuedPrompt } from "@gebai/sdk"

/* ---------- 最小真实 DOM（属性/子节点/文本/移除都要真语义） ---------- */

class TestEl {
  tagName: string
  nodeType = 1
  childNodes: TestEl[] = []
  parentNode: TestEl | null = null
  dataset: Record<string, string> = {}
  style: Record<string, string> = {}
  hidden = false
  value = ""
  rows = 0
  isConnected = true
  onclick: (() => void) | null = null
  oninput: (() => void) | null = null
  scrollTop = 0
  className = ""
  private text = ""
  private attrs = new Map<string, string>()
  constructor(tag: string) {
    this.tagName = tag.toUpperCase()
  }
  get classList(): { add: (...n: string[]) => void; remove: (...n: string[]) => void; contains: (n: string) => boolean; toggle: (n: string, f?: boolean) => void } {
    const set = new Set(this.className.split(/\s+/).filter(Boolean))
    const sync = () => {
      this.className = [...set].join(" ")
    }
    return {
      add: (...names: string[]) => {
        for (const n of names) set.add(n)
        sync()
      },
      remove: (...names: string[]) => {
        for (const n of names) set.delete(n)
        sync()
      },
      contains: (n: string) => set.has(n),
      toggle: (n: string, force?: boolean) => {
        const next = force === undefined ? !set.has(n) : force
        if (next) set.add(n)
        else set.delete(n)
        sync()
      },
    }
  }
  get children(): TestEl[] {
    return this.childNodes
  }
  get innerHTML(): string {
    return ""
  }
  set innerHTML(_v: string) {}
  get textContent(): string {
    return this.text + this.childNodes.map((c) => c.textContent).join("")
  }
  set textContent(v: string) {
    this.text = String(v)
    for (const c of this.childNodes) c.parentNode = null
    this.childNodes = []
  }
  appendChild<T extends TestEl>(c: T): T {
    c.parentNode = this
    this.childNodes.push(c)
    return c
  }
  append(...nodes: TestEl[]): void {
    for (const n of nodes) this.appendChild(n)
  }
  prepend(...nodes: TestEl[]): void {
    for (const n of [...nodes].reverse()) {
      n.parentNode = this
      this.childNodes.unshift(n)
    }
  }
  replaceChildren(...nodes: TestEl[]): void {
    for (const c of this.childNodes) c.parentNode = null
    this.text = ""
    this.childNodes = []
    for (const n of nodes) this.appendChild(n)
  }
  remove(): void {
    this.parentNode?.removeChild(this)
  }
  removeChild(c: TestEl): void {
    const i = this.childNodes.indexOf(c)
    if (i >= 0) this.childNodes.splice(i, 1)
    c.parentNode = null
  }
  setAttribute(k: string, v: string): void {
    this.attrs.set(k, String(v))
  }
  getAttribute(k: string): string | null {
    return this.attrs.get(k) ?? null
  }
  querySelector(): null {
    return null
  }
  querySelectorAll(): TestEl[] {
    return []
  }
  insertAdjacentHTML(): void {}
  focus(): void {}
  setSelectionRange(): void {}
}

const g = globalThis as unknown as Record<string, unknown>
const prevDoc = g.document
const prevWin = g.window
const byId = new Map<string, TestEl>()
function elById(id: string): TestEl {
  let e = byId.get(id)
  if (!e) {
    e = new TestEl("div")
    byId.set(id, e)
  }
  return e
}
const documentStub = {
  getElementById: (id: string) => elById(String(id)),
  createElement: (tag: string) => new TestEl(tag),
  createElementNS: (_ns: string, tag: string) => new TestEl(tag),
  createTextNode: (text: string) => {
    const e = new TestEl("#text")
    e.textContent = text
    return e
  },
  createDocumentFragment: () => new TestEl("fragment"),
  querySelector: () => null,
  querySelectorAll: () => [],
  addEventListener: () => {},
  removeEventListener: () => {},
  dispatchEvent: () => true,
  body: new TestEl("body"),
  visibilityState: "visible",
  hidden: false,
}
g.document = documentStub
g.window = g.window ?? {}

const { applyQueued, applyQueuedUpdate, renderQueue } = await import("./queue")
const state = await import("./state")

/** 排队条根节点：**以 state 里的实际引用为准**。
 *  bun test 同进程共享模块——若别的测试文件先 import 了 state.ts，`queueEl` 已在它的桩下求值，
 * 此时自己再造一个同名节点去断言等于在看另一个对象（单文件过、全量挂的典型成因）。 */
const root = () => state.queueEl as unknown as TestEl

/** 子树遍历：只读 children 与 className/tagName（各文件桩都保证这二者），不依赖 TestEl 独有方法。 */
function walkAll(node: TestEl, out: TestEl[] = []): TestEl[] {
  for (const c of (node.children ?? []) as TestEl[]) {
    out.push(c)
    walkAll(c, out)
  }
  return out
}
function hasClass(node: TestEl, cls: string): boolean {
  return String(node.className ?? "").split(/\s+/).includes(cls)
}

// 桩必须活到用例结束才还原：queue.ts 的 chip 构造在运行时调 document.createElement，
// 提前还原会拿到基线代理桩（无 findAll、子节点语义也不同）
afterAll(() => {
  g.document = prevDoc
  g.window = prevWin
})

const SID = "sess-queue"
const OTHER = "sess-other"

function item(id: string, text: string, files = 0): QueuedPrompt {
  return {
    id,
    sessionId: SID,
    text,
    attachments: Array.from({ length: files }, (_, i) => ({ path: `tmp/f${i}.png`, mime: "image/png", name: `f${i}.png`, size: 1 })),
    messageId: `m-${id}`,
    createdAt: 0,
  } as QueuedPrompt
}

/** 当前排队条的 chip 节点（`.queue-item`）。 */
function chips(): TestEl[] {
  return ((root().children ?? []) as TestEl[]).filter((c) => hasClass(c, "queue-item"))
}
function chipById(id: string): TestEl | undefined {
  return chips().find((c) => c.dataset?.id === id)
}
/** 该 chip 的编辑态 textarea（无编辑态返回 undefined）。 */
function editorOf(id: string): TestEl | undefined {
  const chip = chipById(id)
  if (!chip) return undefined
  const wrap = walkAll(chip).find((c) => hasClass(c, "queue-editor"))
  return wrap ? walkAll(wrap).find((c) => c.tagName === "TEXTAREA") : undefined
}
function ordOf(id: string): string | undefined {
  const chip = chipById(id)
  return chip ? walkAll(chip).find((c) => hasClass(c, "queue-ord"))?.textContent : undefined
}
/** chip 里按类名找节点（正文/按钮）。 */
function inChip(id: string, cls: string): TestEl | undefined {
  const chip = chipById(id)
  return chip ? walkAll(chip).find((c) => hasClass(c, cls)) : undefined
}
/** 点某条 chip 的 ✎（按 data-tip 定位，按钮顺序不参与断言）。 */
function clickEdit(id: string): void {
  const chip = chipById(id)
  const btn = chip ? walkAll(chip).find((c) => c.tagName === "BUTTON" && String(c.dataset?.tip ?? "").includes("编辑")) : undefined
  btn?.onclick?.()
}
/** 点某条 chip 的 ✕（撤回）。 */
function clickRemove(id: string): void {
  inChip(id, "queue-del")?.onclick?.()
}

function reset(sessionId = SID): void {
  state.setCurrentSession({ id: sessionId, name: "排队会话", createdAt: 0, updatedAt: 0 } as never)
  applyQueued({})
  renderQueue()
}

describe("排队条：重复推送不重建（编辑不被打断）", () => {
  test("同一份队列连推三次：chip 节点与编辑框都是同一份，草稿不被服务端正文覆盖", () => {
    reset()
    applyQueued({ [SID]: [item("a", "原始正文")] })
    clickEdit("a")
    const ta = editorOf("a")!
    expect(ta.value).toBe("原始正文")

    // 用户输入，尚未保存
    ta.value = "我改了一半"
    ta.oninput?.()

    const chipBefore = chipById("a")!
    // 三拍快照（每拍都带全量 queues）+ 一次队列事件推来的是**服务端旧正文**
    applyQueued({ [SID]: [item("a", "原始正文")] })
    applyQueued({ [SID]: [item("a", "原始正文")] })
    applyQueuedUpdate(SID, [item("a", "原始正文")])
    renderQueue()

    expect(chipById("a")).toBe(chipBefore) // 节点同一：chip 没被重建
    expect(editorOf("a")).toBe(ta) // 编辑框同一：焦点与光标位置都还在
    expect(ta.value).toBe("我改了一半") // 草稿没被服务端旧值抹掉
  })

  test("非编辑态的相同内容推送不写 DOM：正文节点保持同一份", () => {
    reset()
    applyQueued({ [SID]: [item("a", "文一"), item("b", "文二")] })
    const textA = inChip("a", "queue-text")!
    applyQueued({ [SID]: [item("a", "文一"), item("b", "文二")] })
    expect(inChip("a", "queue-text")).toBe(textA)
  })

  test("正文真的变了才更新该条：其余条目不受影响", () => {
    reset()
    applyQueued({ [SID]: [item("a", "旧"), item("b", "乙")] })
    const chipB = chipById("b")!
    const textA = inChip("a", "queue-text")!
    applyQueuedUpdate(SID, [item("a", "新"), item("b", "乙")])
    expect(chipById("b")).toBe(chipB) // 未变条目原样保留
    const textA2 = inChip("a", "queue-text")!
    expect(textA2).not.toBe(textA)
    expect(textA2.textContent).toBe("新")
  })
})

describe("保存与顺序", () => {
  test("保存后退出编辑态，正文即新值（服务端回推前不回闪旧文字）", () => {
    reset()
    applyQueued({ [SID]: [item("a", "旧正文")] })
    clickEdit("a")
    const ta = editorOf("a")!
    ta.value = "改好了"
    inChip("a", "queue-save")!.onclick?.()
    expect(editorOf("a")).toBeUndefined() // 编辑态结束
    expect(inChip("a", "queue-text")!.textContent).toBe("改好了")
    // 服务端确认后回推同内容：不再有变化
    applyQueuedUpdate(SID, [item("a", "改好了")])
    expect(inChip("a", "queue-text")!.textContent).toBe("改好了")
  })

  test("撤回一条后其余条目就地重排序号（节点不重建）", () => {
    reset()
    applyQueued({ [SID]: [item("a", "一"), item("b", "二"), item("c", "三")] })
    const chipC = chipById("c")!
    expect(ordOf("c")).toBe("3")
    clickRemove("a")
    applyQueuedUpdate(SID, [item("b", "二"), item("c", "三")])
    expect(chipById("a")).toBeUndefined()
    expect(chipById("c")).toBe(chipC)
    expect(ordOf("b")).toBe("1")
    expect(ordOf("c")).toBe("2")
  })

  test("入队新条目追加在尾部，已有条目节点不变", () => {
    reset()
    applyQueued({ [SID]: [item("a", "一")] })
    const chipA = chipById("a")!
    applyQueuedUpdate(SID, [item("a", "一"), item("b", "二")])
    expect(chipById("a")).toBe(chipA)
    expect(chips().map((c) => c.dataset.id)).toEqual(["a", "b"])
  })
})

describe("会话边界", () => {
  test("编辑中换成另一条编辑：上一条退回普通 chip（不会两条同时停在编辑态）", () => {
    reset()
    applyQueued({ [SID]: [item("a", "甲"), item("b", "乙")] })
    clickEdit("a")
    expect(editorOf("a")).toBeDefined()
    clickEdit("b")
    expect(chipById("a")).toBeDefined()
    expect(editorOf("a")).toBeUndefined()
    expect(inChip("a", "queue-text")!.textContent).toBe("甲")
    expect(editorOf("b")!.value).toBe("乙")
  })

  test("切会话整条重建：编辑态清空、只显示当前会话的队列", () => {
    reset()
    applyQueued({ [SID]: [item("a", "本会话")], [OTHER]: [{ ...item("x", "别的会话"), sessionId: OTHER }] })
    clickEdit("a")
    expect(editorOf("a")).toBeDefined()

    state.setCurrentSession({ id: OTHER, name: "另一个", createdAt: 0, updatedAt: 0 } as never)
    renderQueue()
    expect(chips().map((c) => c.dataset.id)).toEqual(["x"])

    // 切回来：不再处于编辑态（编辑态属旧视图，不跨会话残留）
    state.setCurrentSession({ id: SID, name: "排队会话", createdAt: 0, updatedAt: 0 } as never)
    renderQueue()
    expect(chips().map((c) => c.dataset.id)).toEqual(["a"])
    expect(editorOf("a")).toBeUndefined()
  })

  test("队列表空：整条隐藏（不留空壳与残留 chip）", () => {
    reset()
    applyQueued({ [SID]: [item("a", "一")] })
    applyQueuedUpdate(SID, [])
    expect(chips().length).toBe(0)
    expect(root().hidden).toBe(true)
  })
})
