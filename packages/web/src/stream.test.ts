import { afterEach, describe, expect, mock, test } from "bun:test"
import type { ChatChunk } from "@gebai/sdk"
// markdown.ts 模块级 import dompurify：bun test 无 DOM 环境 sanitize 不可用，先 mock 模块（须早于动态 import stream）
mock.module("dompurify", () => ({ default: { sanitize: (s: unknown) => s } }))

// ---------- 最小 DOM mock（与 messages.test.ts 同构：Proxy 兜底未定义成员为 no-op） ----------
// 本文件全部用例走后台会话路径（getCurrentSession ≠ 运行会话）：渲染/计时/滚动分支均被
// 会话守卫早退，不依赖 DOM 行为——mock 仅为 state.ts 模块加载期取元素引用兜底（本文件
// 先于 messages.test.ts 加载时）。断言只读 runs 运行态（模块单例，与 DOM 归属无关）。
interface MockEl {
  children: MockEl[]
  className: string
  tagName: string
  textContent: string
  append(...nodes: unknown[]): void
  prepend(...nodes: unknown[]): void
  appendChild(n: unknown): void
  remove(): void
  readonly childNodes: MockEl[]
}
function makeMockEl(tag = "div"): MockEl & { addEventListener(): void; setPointerCapture(): void; releasePointerCapture(): void; classList: { add(c: string): void; remove(c: string): void; contains(c: string): boolean; toggle(c: string, v?: boolean): void } } {
  const classList = {
    add(c: string) {
      if (!el.className.split(" ").includes(c)) el.className = `${el.className} ${c}`.trim()
    },
    remove(c: string) {
      el.className = el.className.split(" ").filter((x) => x && x !== c).join(" ")
    },
    contains: (c: string) => el.className.split(" ").includes(c),
    toggle(c: string, v?: boolean) {
      const on = v === undefined ? !classList.contains(c) : v
      on ? classList.add(c) : classList.remove(c)
    },
  }
  const el: MockEl & Record<string, unknown> = {
    children: [] as MockEl[],
    className: "",
    tagName: tag.toUpperCase(),
    get textContent() {
      return el.children.map((c) => c.textContent ?? "").join("")
    },
    set textContent(v: string) {
      el.children.length = 0
      el.children.push({ children: [] as MockEl[], className: "", tagName: "SPAN", textContent: String(v ?? ""), append() {}, prepend() {}, appendChild() {}, remove() {}, childNodes: [] as MockEl[] } as MockEl)
    },
    append(...nodes: unknown[]) {
      for (const n of nodes) if (n && typeof n === "object") el.children.push(n as MockEl)
    },
    prepend(...nodes: unknown[]) {
      for (const n of [...nodes].reverse()) if (n && typeof n === "object") el.children.unshift(n as MockEl)
    },
    appendChild(n: unknown) {
      if (n && typeof n === "object") el.children.push(n as MockEl)
    },
    remove() {
      /* 单测不挂父子引用 */
    },
    get childNodes(): MockEl[] {
      return el.children
    },
    addEventListener() {},
    setPointerCapture() {},
    releasePointerCapture() {},
    classList,
    value: "", // 输入框等表单元素占位（syncSendButton 读 input.value）
    hidden: false,
    disabled: false,
    dataset: {} as Record<string, string>, // data-tip 等自定义属性（ui.ts tip 助手）
    querySelector: () => null,
    querySelectorAll: () => [] as unknown[], // 审批卡清理等遍历（后台路径无卡片，空集即可）
  }
  return el as unknown as ReturnType<typeof makeMockEl>
}

const base = makeMockEl("div")
const g = globalThis as unknown as Record<string, unknown>
if (!g.document) {
  const doc = {
    getElementById: () => makeMockEl(),
    createElement: (tag?: string) => makeMockEl(tag ?? "div"),
    querySelector: () => null,
    querySelectorAll: () => [],
    addEventListener() {},
    body: base,
    documentElement: base,
    title: "",
  }
  g.document = new Proxy(doc, {
    get(t, k) {
      if (typeof k === "string" && k in t) return (t as Record<string, unknown>)[k]
      return () => {}
    },
  })
  g.window = globalThis
  g.requestAnimationFrame = (cb: () => void) => {
    cb()
    return 0
  }
  g.cancelAnimationFrame = () => {}
  g.navigator = { onLine: true }
  g.location = { protocol: "http:", host: "localhost" }
  g.MutationObserver = class {
    observe() {}
    disconnect() {}
  }
}

// mock 就位后动态加载被测模块
const { consumeTaskStream } = await import("./stream")
const stateMod = await import("./state")
const { runs, setCurrentSession } = stateMod
// 合跑时其他测试文件先加载 state.ts（模块单例）：其 mock 的共享 base 对象可能缺表单字段，
// 防御性补齐 consumeTaskStream 收尾链路（composer.syncSendButton）读取的 value——
// 不补时 input.value.trim() 报 TypeError（本文件后于 markdown/messages.test.ts 加载时）
{
  const inputEl = stateMod.input as unknown as { value?: string }
  if (typeof inputEl.value !== "string") inputEl.value = ""
}

function text(t: string, messageId?: string): ChatChunk {
  return { kind: "text", text: t, ...(messageId ? { messageId } : {}) }
}

/** 驱动一次任务流：按序投喂 chunks；done 处理后（finally 清理前）快照 run.acc/messageId 供断言。 */
async function runChunks(sessionId: string, chunks: ChatChunk[]): Promise<{ acc: string; messageId: string }> {
  let snap = { acc: "", messageId: "" }
  await consumeTaskStream(sessionId, (run) =>
    (async function* () {
      for (const c of chunks) {
        yield c
        if (c.kind === "done") snap = { acc: run.acc, messageId: run.messageId }
      }
    })(),
  )
  return snap
}

afterEach(() => {
  runs.clear()
  setCurrentSession(null)
})

describe("consumeTaskStream 主循环轮界（后台会话多轮回复不并入同一累积）", () => {
  test("后台会话：两轮回复（messageId 变化）累积被轮界重置，最终 run.acc 只含最后一轮", async () => {
    setCurrentSession({ id: "other" } as never) // 切走：目标会话 s1 在后台运行（复现缺陷场景）
    const snap = await runChunks("s1", [
      text("第一轮回答", "m1"),
      { kind: "tool_call", toolCall: { id: "t1", name: "ls", arguments: {} } },
      { kind: "tool_result", toolCall: { id: "t1", name: "ls", arguments: {} }, output: "done" },
      text("第二轮回答", "m2"),
      { kind: "done" },
    ])
    // 修复前：run.acc === "第一轮回答第二轮回答"（跨轮累积）——切回时渲染进同一张流式卡片，
    // 后续新回复继续追加在前面的卡片内
    expect(snap.acc).toBe("第二轮回答")
    expect(snap.messageId).toBe("m2")
    expect(runs.has("s1")).toBe(false) // 任务结束运行态已清
  })

  test("同轮多 chunk 累积不受影响（messageId 不变只拼接），跨轮才重置", async () => {
    setCurrentSession({ id: "other" } as never)
    const snap = await runChunks("s1", [text("你", "m1"), text("好", "m1"), text("第二", "m2"), text("轮", "m2"), { kind: "done" }])
    expect(snap.acc).toBe("第二轮") // 同轮拼接「第二」「轮」；跨轮（m1→m2）重置后不含「你好」
  })

  test("无 messageId 的文本不触发轮界（兼容缺省字段），照常拼接", async () => {
    setCurrentSession({ id: "other" } as never)
    const snap = await runChunks("s1", [text("第一轮回答", "m1"), text("无标记追加"), { kind: "done" }])
    expect(snap.acc).toBe("第一轮回答无标记追加")
  })

  test("重连 resume：轮界基准重置（重放不误判轮界、断线前累积不残留），同 messageId 重新累积", async () => {
    setCurrentSession({ id: "other" } as never)
    const snap = await runChunks("s1", [
      text("断线前文本", "m1"),
      { kind: "resume" },
      text("重放文本", "m1"),
      { kind: "done" },
    ])
    expect(snap.acc).toBe("重放文本")
  })
})
