/**
 * 后台会话的 DOM 归属（子会话入口行 / 异常记录 / 收尾说明）：这些节点都挂在「消息流尾部活动区」，
 * 缺省目标是**当前活动容器**——后台会话渲染时必须显式指到它自己的容器，否则会侵当前视图
 * （现象：切到别的会话后，输入框上方仍挂着上个会话的「🌿 子会话 · …」入口行）。
 */
import { afterEach, describe, expect, mock, test } from "bun:test"
import type { ChatChunk } from "@gebai/sdk"
// markdown.ts 模块级 import dompurify：bun test 无 DOM 环境 sanitize 不可用，先 mock 模块（须早于动态 import）
// 只 mock 这一个模块：mock.module 对同进程后续测试文件全局生效，多 mock 一个就多一份顺序污染源
mock.module("dompurify", () => ({ default: { sanitize: (s: unknown) => s } }))

const panes = await import("./msg-panes")
const state = await import("./state")
const stream = await import("./stream")
const messages = await import("./messages")

afterEach(() => {
  state.runs.clear()
  state.setCurrentSession(null)
  panes.resetPanes()
})

type Node = { className: string; dataset: Record<string, string>; children: Node[] }
/** 某容器内所有带该 class 的节点（测试基线的 querySelectorAll 是恒空的桩，按子节点树直接走）。 */
function findAll(root: { children?: unknown[] }, cls: string): Node[] {
  const out: Node[] = []
  const walk = (n: { children?: unknown[] }) => {
    for (const c of (n.children ?? []) as Node[]) {
      if ((c.className ?? "").split(" ").includes(cls)) out.push(c)
      walk(c)
    }
  }
  walk(root as { children?: unknown[] })
  return out
}

async function* genChunks(list: ChatChunk[]): AsyncIterable<ChatChunk> {
  for (const c of list) yield c
}

describe("后台会话的尾部挂载不得侵当前会话视图", () => {
  test("子会话入口行落在运行它的会话容器里，不落到当前会话", async () => {
    state.setCurrentSession({ id: "A", name: "A" } as never)
    const paneA = panes.activatePane("A")
    await stream.consumeTaskStream("B", () =>
      genChunks([
        { kind: "subsession_start", subSessionId: "r1", subSessionMeta: { subsession: "左路", agents: ["code"], model: "fast", input: "干点活" } },
        { kind: "done" },
      ]),
    )
    expect(panes.activePane()?.sessionId).toBe("A") // 全程没抢走当前视图
    expect(findAll(paneA.container as never, "subsession-entry").length).toBe(0)
    const paneB = panes.paneOf("B")!
    expect(findAll(paneB.container as never, "subsession-entry").length).toBe(1)
  })

  test("子会话入口行带会话归属（面板定位与切回渲染依赖它）", async () => {
    state.setCurrentSession({ id: "A", name: "A" } as never)
    panes.activatePane("A")
    messages.subSessionEntry({ sessionId: "B", runId: "r9", name: "右路", agents: [], state: "运行中", status: "running" }, undefined, panes.ensurePane("B"))
    const host = panes.paneOf("B")!
    const entry = findAll(host.container as never, "subsession-entry")[0]
    expect(entry.dataset.sessionId).toBe("B")
    expect(entry.dataset.runId).toBe("r9")
    // 当前会话容器里一个都没有
    expect(findAll(panes.paneOf("A")!.container as never, "subsession-entry").length).toBe(0)
  })

  test("无输出收尾说明落在运行会话的容器里（不在当前会话里冒出一句话）", async () => {
    state.setCurrentSession({ id: "A", name: "A" } as never)
    const paneA = panes.activatePane("A")
    // 正常结束但没有任何内容 → 收尾补一条说明气泡
    await stream.consumeTaskStream("B", () => genChunks([{ kind: "done" }]))
    expect(findAll(paneA.container as never, "msg").length).toBe(0)
    const paneB = panes.paneOf("B")!
    expect(findAll(paneB.container as never, "msg").length).toBe(1)
  })
})
