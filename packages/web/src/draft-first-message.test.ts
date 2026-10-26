import { afterEach, describe, expect, test } from "bun:test"
// markdown.ts 模块级 import dompurify：bun test 无 DOM 环境 sanitize 不可用，先 mock 模块（须早于动态 import messages/sessions）
import { mock } from "bun:test"
mock.module("dompurify", () => ({ default: { sanitize: (s: unknown) => s } }))

const sessions = await import("./sessions")
const panes = await import("./msg-panes")
const state = await import("./state")
const messages = await import("./messages")

afterEach(() => {
  state.setCurrentSession(null)
  panes.resetPanes()
})

/** 容器里挂了几个消息节点（测试基线的 classList.contains 是恒 false 的桩，按 className 判定）。 */
function msgNodes(pane: { container: HTMLElement }): Element[] {
  return [...pane.container.children].filter((c) => (c.className ?? "").split(" ").includes("msg"))
}

describe("草稿页首条消息上屏（新会话创建时消息视图必须就位）", () => {
  test("无活动容器时 appendMsg 不落 DOM——这正是首条消息只落盘不上屏的原因", () => {
    // 草稿页：会话刚由 createSession 建出、尚未建立任何消息容器
    state.setCurrentSession({ id: "s1", name: "新会话" } as never)
    panes.deactivatePane()
    expect(panes.activePane()).toBeNull()
    messages.appendMsg({ id: "m1", role: "user", content: "第一条", createdAt: Date.now() })
    // 无活动容器：宿主上只有草稿页的空状态，没有消息节点
    expect([...state.msgEl.children].filter((c) => (c.className ?? "").split(" ").includes("msg")).length).toBe(0)
  })

  test("enterSessionView 之后 appendMsg 立刻可见（同一容器内）", () => {
    state.setCurrentSession({ id: "s2", name: "新会话" } as never)
    panes.deactivatePane()
    sessions.enterSessionView("s2")
    expect(panes.activePane()?.sessionId).toBe("s2")
    const pane = panes.activePane()!
    messages.appendMsg({ id: "m2", role: "user", content: "第一条", createdAt: Date.now() })
    const inPane = msgNodes(pane)
    expect(inPane.length).toBe(1)
    // 基线测试桩不聚合 textContent：按写进 dataset 的消息 id 确认挂的是这一条
    expect((inPane[0] as unknown as { dataset: Record<string, string> }).dataset.msgId).toBe("m2")
    // 容器可见（草稿页把它隐藏了，就位后必须显示，否则内容渲染进隐藏容器）
    expect(pane.container.hidden).toBe(false)
  })

  test("就位对目标会话已有的容器无副作用（已有历史/在途流不被清掉）", () => {
    state.setCurrentSession({ id: "prev", name: "旧" } as never)
    sessions.enterSessionView("prev")
    const prevPane = panes.activePane()!
    prevPane.chunks = { msgs: [], bounds: [0, 1], sessionId: "prev", index: new Map() }
    messages.appendMsg({ id: "keep", role: "user", content: "旧会话已有消息", createdAt: Date.now() })
    // 切到新会话后就位：旧容器的块表与内容原样保留（切回是纯显隐、不重拉）
    state.setCurrentSession({ id: "next", name: "新会话" } as never)
    sessions.enterSessionView("next")
    expect(panes.activePane()!.sessionId).toBe("next")
    expect(prevPane.chunks?.sessionId).toBe("prev")
    expect(msgNodes(prevPane).length).toBe(1)
    // 新会话容器是新建的：没有块表，也不含旧会话的消息
    expect(panes.activePane()!.chunks ?? null).toBeNull()
    expect(msgNodes(panes.activePane()!).length).toBe(0)
  })
})
