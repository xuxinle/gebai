import { describe, expect, test } from "bun:test"
import { formatElapsed, inlineSummary, runStateLine, sortRuns, statusText } from "./subsessions-core"

describe("子会话观察：状态文案（subsessions-core）", () => {
  test("formatElapsed：亚秒 / 秒 / 分秒 / 时分四档", () => {
    expect(formatElapsed(0)).toBe("<1s")
    expect(formatElapsed(999)).toBe("<1s")
    expect(formatElapsed(1000)).toBe("1s")
    expect(formatElapsed(42_000)).toBe("42s")
    expect(formatElapsed(61_000)).toBe("1m1s")
    expect(formatElapsed(120_000)).toBe("2m")
    expect(formatElapsed(3_600_000 + 5 * 60_000)).toBe("1h5m")
    // 负值（时钟回拨/未开始）不产生「-1s」
    expect(formatElapsed(-500)).toBe("<1s")
  })

  test("statusText：只有运行中/完成两态（失败与取消都是空输出，不臆测成败）", () => {
    expect(statusText("running")).toBe("运行中")
    expect(statusText("done")).toBe("完成")
  })

  test("inlineSummary：去推理块、压平空白、超长保留头尾", () => {
    expect(inlineSummary("  a\n\n b  ")).toBe("a b")
    expect(inlineSummary("<think>内部推理</think>结论")).toBe("结论")
    const long = `${"头".repeat(60)}${"尾".repeat(20)}`
    const out = inlineSummary(long, 40)
    expect(out.length).toBeLessThanOrEqual(41) // 40 + 省略号
    expect(out.startsWith("头")).toBe(true)
    expect(out.endsWith("尾")).toBe(true)
    expect(out).toContain("…")
  })

  test("runStateLine 运行中：不给耗时（未结束），0 轮 0 工具省略，有合入才列", () => {
    expect(runStateLine({ status: "running", rounds: 0, toolCalls: 0, merges: 0, startedAt: 0 })).toBe("运行中")
    expect(runStateLine({ status: "running", rounds: 3, toolCalls: 5, merges: 0, startedAt: 0 })).toBe("运行中 · 3 轮 · 5 工具")
    expect(runStateLine({ status: "running", rounds: 3, toolCalls: 5, merges: 2, startedAt: 0 })).toBe("运行中 · 3 轮 · 5 工具 · 合入 2")
  })

  test("runStateLine 完成：带耗时与输出摘要；空输出显示「无返回」", () => {
    const base = { status: "done" as const, rounds: 2, toolCalls: 1, merges: 0, startedAt: 1000, endedAt: 43_000 }
    expect(runStateLine({ ...base, output: "## 结论\n已修改 a.ts" })).toBe("完成 · 42s · 2 轮 · 1 工具 → ## 结论 已修改 a.ts")
    expect(runStateLine({ ...base, output: "   " })).toBe("完成 · 42s · 2 轮 · 1 工具 → 无返回")
    expect(runStateLine({ ...base, rounds: 0, toolCalls: 0, output: "" })).toBe("完成 · 42s → 无返回")
  })

  test("runStateLine 历史存档（无起止时刻）：不编造耗时", () => {
    expect(runStateLine({ status: "done", rounds: 0, toolCalls: 0, merges: 0, startedAt: 1000, output: "旧结果" })).toBe("完成 → 旧结果")
  })

  test("sortRuns：运行中在前（先开始的上），已结束按结束时间倒序", () => {
    const r = (id: string, status: "running" | "done", startedAt: number, endedAt?: number) => ({ id, status, startedAt, endedAt })
    const list = [r("done-old", "done", 100, 200), r("run-2", "running", 500), r("done-new", "done", 300, 900), r("run-1", "running", 400)]
    expect(sortRuns(list).map((x) => x.id)).toEqual(["run-1", "run-2", "done-new", "done-old"])
    // 不改动入参（面板渲染按排序结果，不污染原记录顺序）
    expect(list.map((x) => x.id)).toEqual(["done-old", "run-2", "done-new", "run-1"])
  })
})
