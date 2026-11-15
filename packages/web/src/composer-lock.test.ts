/**
 * 输入锁判据（`composerLocked`）：会话有待处理审批卡时禁输入。
 *
 * 现场：审批通过后输入框仍然 `disabled`（回车与 Ctrl+Enter 全无反应），刷新页面才恢复。
 * 原因是用**计数**表达「这个会话还有几张审批卡」——同一条审批请求会被重复推送
 * （断线重连的事件重放、attach 快照与实时事件各来一次），而重复推送的卡片走的是
 * 「替换旧卡」路径：加一进去、替换时没减。计数于是虚高，用户处理完最后一张卡后
 * 计数仍大于零，锁永远不解。判据改为直接看卡片集合里还有没有这个会话的卡。
 */
import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"
import { composerLocked } from "./composer-lock"

const src = (file: string) => readFileSync(join(import.meta.dirname, file), "utf8")


describe("输入锁判据", () => {
  test("有属于该会话的卡 → 锁", () => {
    expect(composerLocked(["a", "b"], "a")).toBe(true)
  })

  test("没有属于该会话的卡 → 解锁（别的会话在等审批不该锁住当前会话）", () => {
    expect(composerLocked(["b"], "a")).toBe(false)
    expect(composerLocked([], "a")).toBe(false)
  })

  test("无当前会话（草稿页/未选中）→ 不解锁不误锁，按未锁处理", () => {
    // 草稿页还没有会话 id：审批卡必然不属于它，锁住输入只会让人发不出首条消息
    expect(composerLocked(["a"], undefined)).toBe(false)
    expect(composerLocked([], undefined)).toBe(false)
  })

  test("判据是集合而不是计数：同一会话多张卡与一张卡等价，最后一张收走后必然解锁", () => {
    // 重复推送过的卡被替换成一张 —— 集合里只留一份；收走后集合为空，锁必须解开
    expect(composerLocked(["", "a", "a"], "a")).toBe(true)
    expect(composerLocked([], "a")).toBe(false)
  })
})

describe("接线：审批卡不得再维护自己的输入锁计数", () => {
  test("approvals.ts 的锁判据走 composerLocked，没有「加一减一」的会话计数", () => {
    const s = src("approvals.ts")
    expect(s).toContain("composerLocked(")
    // 计数与卡片替换/任务结束清理两条路径都难对齐（加一进去、替换时没减），重回计数即复发
    expect(s).not.toContain("pendingBySession")
  })
})
