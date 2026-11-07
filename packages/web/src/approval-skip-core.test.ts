import { describe, expect, test } from "bun:test"
import { approvalSkipTargets } from "./approval-skip-core"

describe("approvalSkipTargets（重启/重连后补齐审批跳过的会话集合）", () => {
  test("在跑会话 + 当前会话合并去重，保序", () => {
    expect(approvalSkipTargets(["a", "b"], "b")).toEqual(["a", "b"])
    expect(approvalSkipTargets(["a"], "c")).toEqual(["a", "c"])
  })

  test("不只看当前会话：快照里的其他在跑会话同样要补（定时任务/飞书桥接/其他标签页）", () => {
    // 只补当前会话会让其余会话重启后照旧逐条弹审批——这条是本模块存在的理由
    expect(approvalSkipTargets(["task-session", "feishu-session"], "chat-session")).toEqual(["task-session", "feishu-session", "chat-session"])
  })

  test("空值与非字符串剔除，重复只留一个", () => {
    expect(approvalSkipTargets(["", "  ", "a", "a"], undefined)).toEqual(["a"])
    expect(approvalSkipTargets([], "solo")).toEqual(["solo"])
    expect(approvalSkipTargets([], undefined)).toEqual([])
  })
})
