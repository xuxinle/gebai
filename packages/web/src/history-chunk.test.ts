import { describe, expect, test } from "bun:test"
import { historySplitIndex, isRunMessage, runIdOfMessage, type RunMessageLike } from "./history-chunk"

/** 普通消息（不进折叠容器）。 */
const plain = (): RunMessageLike => ({})
/** 执行过程消息（同一 runId 归入同一折叠容器）。 */
const run = (runId: string): RunMessageLike => ({ session: true, sessionRunId: runId })

describe("历史分片切分（historySplitIndex）", () => {
  test("消息数不超过首批容量：全部渲染（起点 0）", () => {
    const msgs = Array.from({ length: 40 }, plain)
    expect(historySplitIndex(msgs, 40)).toBe(0)
    expect(historySplitIndex([], 40)).toBe(0)
    expect(historySplitIndex([plain()], 40)).toBe(0)
  })

  test("超出容量：尾部取 tailCount 条", () => {
    const msgs = Array.from({ length: 100 }, plain)
    expect(historySplitIndex(msgs, 40)).toBe(60)
  })

  test("起点落在执行过程组中间：向前扩展到组首（不拆容器）", () => {
    // 组 [50..59] 同一 runId，total=80、tailCount=25 → 起点 55 落在组中间 → 应回退到 50
    const msgs: RunMessageLike[] = [...Array.from({ length: 50 }, plain), ...Array.from({ length: 10 }, () => run("r1")), ...Array.from({ length: 20 }, plain)]
    expect(historySplitIndex(msgs, 25)).toBe(50)
  })

  test("相邻两组不同 runId：不回退到上一组（组边界即切点）", () => {
    const msgs: RunMessageLike[] = [...Array.from({ length: 30 }, plain), ...Array.from({ length: 10 }, () => run("r1")), ...Array.from({ length: 10 }, () => run("r2"))]
    // tailCount=15 → 起点 35 落在 r1 组内 → 回退到 r1 组首 30（r2 组首为 40，不能跨组吞并）
    expect(historySplitIndex(msgs, 15)).toBe(30)
  })

  test("普通消息不会被误判为组（不回退）", () => {
    const msgs: RunMessageLike[] = Array.from({ length: 100 }, plain)
    expect(historySplitIndex(msgs, 10)).toBe(90)
  })

  test("组一直延伸到列表开头：起点收敛到 0（全部渲染，不产生空批次）", () => {
    const msgs: RunMessageLike[] = Array.from({ length: 60 }, () => run("r1"))
    expect(historySplitIndex(msgs, 20)).toBe(0)
  })

  test("旧版 subAgent 存档同样按组切分（subAgentRunId）", () => {
    const legacy: RunMessageLike[] = [{ subAgent: true, subAgentRunId: "old" }]
    expect(isRunMessage(legacy[0])).toBe(true)
    expect(runIdOfMessage(legacy[0])).toBe("old")
    const msgs: RunMessageLike[] = [...Array.from({ length: 30 }, plain), ...Array.from({ length: 6 }, () => ({ subAgent: true, subAgentRunId: "old" })), ...Array.from({ length: 10 }, plain)]
    expect(historySplitIndex(msgs, 12)).toBe(30)
  })

  test("分组边界扩展收敛到列表开头：起点 0（不越界、不死循环）", () => {
    // 组占据前半段 [0..29]，起点 25 落在组内 → 一路回退到 0
    const msgs: RunMessageLike[] = [...Array.from({ length: 30 }, () => run("r1")), ...Array.from({ length: 30 }, plain)]
    expect(historySplitIndex(msgs, 35)).toBe(0)
  })
})
