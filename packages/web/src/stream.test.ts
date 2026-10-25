import { afterEach, describe, expect, mock, test } from "bun:test"
import type { ChatChunk } from "@gebai/sdk"
// markdown.ts 模块级 import dompurify：bun test 无 DOM 环境 sanitize 不可用，先 mock 模块（须早于动态 import stream）
mock.module("dompurify", () => ({ default: { sanitize: (s: unknown) => s } }))

// ---------- DOM：由测试基线（scripts/test-preload.ts 的 preload）统一装好 ----------
// 本文件全部用例走后台会话路径（getCurrentSession ≠ 运行会话）：渲染/计时/滚动分支均被
// 会话守卫早退，不依赖 DOM 行为——基线提供的 document 已足够 state.ts 模块加载期取元素引用。
// 本文件不再自带 document 桩：在基线之后 `if (!g.document)` 恒为 false，自带桩永不生效，
// 留着只会让人误以为它在起作用。

// mock 就位后动态加载被测模块
const { consumeTaskStream } = await import("./stream")
const stateMod = await import("./state")
const panesMod = await import("./msg-panes")
const { runs, setCurrentSession } = stateMod
const { paneCount, paneOf, resetPanes } = panesMod
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
  resetPanes()
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

describe("后台会话的 DOM 维护（切回即所见即所得）", () => {
  test("后台运行的会话同样建容器并渲染正文（不再只累积）", async () => {
    setCurrentSession({ id: "other" } as never) // 目标会话 s1 在后台
    let paneDuringRun: unknown = null
    await consumeTaskStream("s1", () =>
      (async function* () {
        yield text("后台回答", "m1")
        paneDuringRun = paneOf("s1")
        yield { kind: "done" }
      })(),
    )
    // 运行期间就有自己的容器（不是只累积待切回重建）
    expect(paneDuringRun).toBeTruthy()
  })

  test("任务结束容器降为可回收（仍在看则保留、切走后可释放）", async () => {
    setCurrentSession({ id: "other" } as never)
    await runChunks("s1", [text("回答", "m1"), { kind: "done" }])
    const pane = paneOf("s1")
    expect(pane).toBeTruthy()
    expect(pane!.running).toBe(false) // 已降为可回收（不再是运行态锁定）
  })

  test("两个会话可同时持有各自己的容器（互不顶掉）", async () => {
    setCurrentSession({ id: "other" } as never)
    await runChunks("s1", [text("甲", "m1"), { kind: "done" }])
    await runChunks("s2", [text("乙", "m2"), { kind: "done" }])
    expect(paneOf("s1")).toBeTruthy()
    expect(paneOf("s2")).toBeTruthy()
    expect(paneCount()).toBeGreaterThanOrEqual(2)
  })
})
