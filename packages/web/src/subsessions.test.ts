import { beforeEach, describe, expect, test } from "bun:test"
import { finishRun, mergeFromMessage, noteMerge, noteRound, noteToolCall, registerArchive, startRun } from "./subsessions"

/**
 * 面板记录层（DOM 由测试基线 scripts/test-preload.ts 提供最小桩）：
 * 记录的状态/轮次/工具数/合入次数是消息流入口与面板行的唯一文案来源，文案不对两处一起错。
 */
describe("子会话观察：运行记录（subsessions）", () => {
  beforeEach(() => {
    // 每个用例独立 runId，避免跨用例共享 store
  })

  test("startRun 幂等：同 runId 重推（服务端每轮重推 start）只刷新元信息，不重置状态与计数", () => {
    const rec = startRun({ sessionId: "s1", runId: "run-a", name: "左路", model: "fast", agents: ["code"], input: "调研A" })
    expect(rec.status).toBe("running")
    expect(rec.name).toBe("左路")
    expect(rec.stateText).toContain("运行中")
    noteRound("s1", "run-a", "m1")
    noteRound("s1", "run-a", "m1") // 同一轮重复到达：不重复计数
    noteToolCall("s1", "run-a")
    expect(rec.rounds).toBe(1)
    expect(rec.toolCalls).toBe(1)

    const again = startRun({ sessionId: "s1", runId: "run-a", name: "左路", model: "fast", agents: ["code", "playwright"], input: "调研A" })
    expect(again).toBe(rec) // 同一记录对象（过程区 DOM 不重建）
    expect(again.rounds).toBe(1)
    expect(again.toolCalls).toBe(1)
    expect(again.agents).toEqual(["code", "playwright"]) // 元信息以最新为准
  })

  test("轮次按 messageId 变化计数（一轮一次）", () => {
    const rec = startRun({ sessionId: "s1", runId: "run-b", agents: [], input: "x" })
    noteRound("s1", "run-b", "m1")
    noteRound("s1", "run-b", "m1")
    noteRound("s1", "run-b", "m2")
    expect(rec.rounds).toBe(2)
  })

  test("noteMerge 累加阶段性合入次数并留全文（最终报告合入不计，它就是本 run 的输出）", () => {
    const rec = startRun({ sessionId: "s1", runId: "run-c", agents: [], input: "x" })
    noteMerge("s1", "run-c", "【智体·子会话「左路」阶段性合入】\n第一阶段性成果")
    noteMerge("s1", "run-c", "【智体·子会话「左路」阶段性合入（摘要）】\n第二阶段性成果")
    expect(rec.merges).toBe(2)
    expect(rec.stateText).toContain("合入 2")
    // 最终报告合入：不计合入（详情「最终报告」区已有同一份内容）
    noteMerge("s1", "run-c", "【智体·子会话「左路」已合并】\n最终报告")
    expect(rec.merges).toBe(2)
  })

  test("finishRun 定稿：状态转完成、带上耗时与输出摘要；空输出为「无返回」", () => {
    const rec = startRun({ sessionId: "s1", runId: "run-d", name: "右路", agents: [], input: "x" })
    noteRound("s1", "run-d", "m1")
    finishRun("s1", "run-d", "## 结论\n已完成")
    expect(rec.status).toBe("done")
    expect(rec.endedAt).toBeGreaterThanOrEqual(rec.startedAt)
    expect(rec.stateText).toContain("完成")
    expect(rec.stateText).toContain("## 结论 已完成")

    const empty = startRun({ sessionId: "s1", runId: "run-e", agents: [], input: "x" })
    finishRun("s1", "run-e", "")
    expect(empty.stateText).toContain("无返回")
  })

  test("registerArchive：历史存档登记为已结束记录（过程按需渲染），重复登记不重建过程区", () => {
    const rec = registerArchive("s1", {
      runId: "run-f",
      agents: ["code"],
      input: "改文件",
      output: "**已完成**",
      subsession: { name: "历史运行", model: "slow" },
      messages: [{ role: "user", content: "改文件" }, { role: "assistant", content: "**已完成**" }],
    })
    expect(rec.status).toBe("done")
    expect(rec.name).toBe("历史运行")
    expect(rec.model).toBe("slow")
    expect(rec.output).toBe("**已完成**")
    expect(rec.archiveRendered).toBe(false) // 懒渲染：详情打开时才铺进过程区
    expect(rec.endedAt).toBeUndefined() // 历史存档无起止时刻：不编造耗时
    expect(rec.stateText).toContain("完成")
    const body = rec.body

    const again = registerArchive("s1", { runId: "run-f", agents: ["code"], input: "改文件", output: "**已完成**", messages: [] })
    expect(again).toBe(rec)
    expect(again.body).toBe(body)
  })

  test("未知 runId 的计数事件为 no-op（start 事件缺失于断线前）", () => {
    expect(() => noteRound("s1", "nope", "m1")).not.toThrow()
    expect(() => noteToolCall("s1", "nope")).not.toThrow()
    expect(() => noteMerge("s1", "nope", "【智体·子会话「x」阶段性合入】\n文本")).not.toThrow()
    expect(() => finishRun("s1", "nope", "")).not.toThrow()
  })

  test("mergeFromMessage（历史回放）：只认阶段性合入，记录缺失时补建一条已结束运行", () => {
    mergeFromMessage("s2", { runId: "hist-1", name: "历史" }, "【智体·子会话「历史」阶段性合入】\n阶段一")
    mergeFromMessage("s2", { runId: "hist-1", name: "历史" }, "【智体·子会话「历史」已合并】\n最终报告")
    const rec = registerArchive("s2", { runId: "hist-1", agents: [], input: "x", output: "", messages: [] })
    expect(rec.merges).toBe(1) // 最终合入不重复计数
    expect(rec.status).toBe("done")
  })
})
