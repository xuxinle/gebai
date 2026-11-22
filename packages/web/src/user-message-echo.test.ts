/**
 * 用户消息回显（`event.user.message`）：用户消息此前没有任何实时事件——本页输入框发送靠乐观上屏，
 * 其余来源（其他标签页/飞书桥接/定时任务/`session` 工具注入/重启续跑）完全没有回显通道，
 * 表现为「回答在流、提问却不存在」，刷新才出现。本文件钉住三件事：
 *
 * ① 回显入口按 messageId 幂等（本页乐观上屏的那条不得重复出现）；
 * ② 落点按会话归属（后台会话渲进它自己的容器、不侵当前视图；已切走且无容器时不渲染）；
 * ③ 回显缓冲：历史重载（loadMessages 重置容器）后能补回「本次快照还没有」的那条。
 */
import { afterEach, beforeEach, describe, expect, mock, test } from "bun:test"
// markdown.ts 模块级 import dompurify：bun test 无 DOM 环境 sanitize 不可用，先 mock 模块（须早于动态 import）
mock.module("dompurify", () => ({ default: { sanitize: (s: unknown) => s } }))

const panes = await import("./msg-panes")
const state = await import("./state")
const messages = await import("./messages")

afterEach(() => {
  state.runs.clear()
  state.setCurrentSession(null)
  panes.resetPanes()
})

type Node = { className: string; dataset: Record<string, string>; children: Node[] }
/** 容器内所有 .msg 节点（测试基线按 className 判定，querySelectorAll 是恒空桩）。 */
function msgNodes(pane: { container: HTMLElement }): Node[] {
  return (pane.container as unknown as Node).children.filter((c) => (c.className ?? "").split(" ").includes("msg"))
}

beforeEach(() => {
  panes.resetPanes()
})

describe("用户消息回显入口（幂等 + 会话归属）", () => {
  test("同 messageId 已挂载（乐观上屏的那条）时不重复渲染", () => {
    state.setCurrentSession({ id: "s1", name: "会话" } as never)
    panes.activatePane("s1")
    // 模拟本页乐观上屏：与随后的回显事件同一 messageId
    messages.appendMsg({ id: "m-1", role: "user", content: "你好", createdAt: 1 })
    const again = messages.appendServerUserMessage("s1", { id: "m-1", content: "你好", createdAt: 1 })
    expect(again).toBeNull()
    expect(msgNodes(panes.paneOf("s1")!).length).toBe(1)
  })

  test("初次回显（非本页发起）渲染进该会话自己的容器", () => {
    state.setCurrentSession({ id: "s1", name: "会话" } as never)
    panes.activatePane("s1")
    const el = messages.appendServerUserMessage("s1", { id: "m-2", content: "来自飞书", createdAt: 2 })
    expect(el).not.toBeNull()
    const nodes = msgNodes(panes.paneOf("s1")!)
    expect(nodes.length).toBe(1)
    expect(nodes[0].dataset.msgId).toBe("m-2")
  })

  test("后台会话有容器时渲进它自己的容器，不侵当前会话视图", () => {
    state.setCurrentSession({ id: "cur", name: "当前" } as never)
    panes.activatePane("cur")
    panes.ensurePane("bg") // 后台会话容器（运行中会话全程持有）
    messages.appendServerUserMessage("bg", { id: "m-3", content: "别处发的", createdAt: 3 })
    expect(msgNodes(panes.paneOf("bg")!).length).toBe(1)
    expect(msgNodes(panes.paneOf("cur")!).length).toBe(0)
    expect(panes.activePane()?.sessionId).toBe("cur")
  })

  test("已切走且无容器时不渲染（切回由历史重建，不得往当前视图里写）", () => {
    state.setCurrentSession({ id: "cur", name: "当前" } as never)
    panes.activatePane("cur")
    const el = messages.appendServerUserMessage("gone", { id: "m-4", content: "旧会话", createdAt: 4 })
    expect(el).toBeNull()
    expect(msgNodes(panes.paneOf("cur")!).length).toBe(0)
  })
})

describe("回显缓冲：历史重载后补回尚未进历史的消息", () => {
  test("重载窗口内刚回显的消息被补回（快照里还没有它）", () => {
    state.setCurrentSession({ id: "s9", name: "会话" } as never)
    panes.activatePane("s9")
    messages.appendServerUserMessage("s9", { id: "m-5", content: "刚发的", createdAt: 5 })
    const pane = panes.paneOf("s9")!
    expect(msgNodes(pane).length).toBe(1)

    // 历史重载：容器被重置（等价于 loadMessages → msgWindow.reset）
    pane.virtualizer.reset([])
    expect(msgNodes(pane).length).toBe(0)
    // 补回：present 里没有它（本次 session.get 的快照早于它落盘）
    messages.replayUserMessageEcho("s9", new Set())
    const restored = msgNodes(pane)
    expect(restored.length).toBe(1)
    expect(restored[0].dataset.msgId).toBe("m-5")
  })

  test("已进历史的消息不再补回（避免重载后重复一条）", () => {
    state.setCurrentSession({ id: "s8", name: "会话" } as never)
    panes.activatePane("s8")
    messages.appendServerUserMessage("s8", { id: "m-6", content: "已落盘", createdAt: 6 })
    const pane = panes.paneOf("s8")!
    pane.virtualizer.reset([])
    // 本次快照读到了它：由历史渲染负责，缓冲条目随即作废
    messages.replayUserMessageEcho("s8", new Set(["m-6"]))
    expect(msgNodes(pane).length).toBe(0)
    // 再次重载也不会复活（缓冲已清）
    pane.virtualizer.reset([])
    messages.replayUserMessageEcho("s8", new Set())
    expect(msgNodes(pane).length).toBe(0)
  })

  test("没有缓冲时重载后不凭空造消息", () => {
    state.setCurrentSession({ id: "s7", name: "会话" } as never)
    panes.activatePane("s7")
    messages.replayUserMessageEcho("s7", new Set())
    expect(msgNodes(panes.paneOf("s7")!).length).toBe(0)
  })
})

describe("压缩通知标题写明用户输入条数", () => {
  test("有 userMessages 时标题带「含 N 条你的输入」", () => {
    expect(messages.compactNoticeTitle(12, undefined, 3)).toBe("已压缩 12 条历史消息（含 3 条你的输入）")
  })
  test("无 userMessages（或 0）时保持原样", () => {
    expect(messages.compactNoticeTitle(12)).toBe("已压缩 12 条历史消息")
    expect(messages.compactNoticeTitle(12, undefined, 0)).toBe("已压缩 12 条历史消息")
  })
  test("降级/裁剪语义不被 userMessages 改写", () => {
    expect(messages.compactNoticeTitle(0, "trim", 3)).toBe("历史消息条数超限裁剪")
    expect(messages.compactNoticeTitle(0, "user-images", 3)).toBe("上下文溢出护栏")
  })
})
