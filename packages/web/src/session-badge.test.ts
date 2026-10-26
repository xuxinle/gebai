/**
 * 会话列表运行态角标（`●` 运行中 / `⌛` 等待你的输入）——审批作答后必须回到运行图标。
 *
 * 角标的 waiting 计数有两个来源：服务端快照的 `runtime[sid].pending`（**只在建连/登录时推送一次**，
 * 运行期不再更新）与本页 DOM 里实际存在的待决卡片（实时真值）。分流规则见 `resolveWaitingCount`：
 * 本页接管着这次运行 → 用 DOM；本页没接管（别处发起）→ 用快照。
 */
import { afterEach, beforeEach, describe, expect, mock, test } from "bun:test"
import type { SessionInfo } from "@gebai/sdk"
mock.module("dompurify", () => ({ default: { sanitize: (s: unknown) => s } }))

const sessions = await import("./sessions")
const state = await import("./state")

const SESSION = { id: "s-badge", name: "角标会话", createdAt: 0, updatedAt: 0 } as SessionInfo

type Node = { className: string; dataset: Record<string, string>; textContent: string; children: Node[] }
function childrenOf(root: unknown): Node[] {
  return ((root as { children?: Node[] }).children ?? []) as Node[]
}
/** 当前列表里该会话的角标文字（无角标返回 undefined）。
 *  测试基线的 `innerHTML = ""` 只赋值、不清子节点（与真实 DOM 不同），列表重渲染会堆叠——
 *  所以从后往前找最后一次渲染出的那一行。 */
function badgeText(sid: string): string | undefined {
  const rows = childrenOf(state.sessionList)
  for (let i = rows.length - 1; i >= 0; i--) {
    const li = rows[i]
    if (li.dataset.sid !== sid) continue
    const dot = childrenOf(li).find((c) => (c.className ?? "").split(" ").includes("session-run"))
    return dot?.textContent
  }
  return undefined
}

beforeEach(async () => {
  state.setCurrentSession(null)
  sessions.setRuntimeInfo({})
  await sessions.refreshSessions([SESSION])
})

afterEach(() => {
  state.runs.clear()
  sessions.setRuntimeInfo({})
  state.setCurrentSession(null)
})

describe("角标等待项取值规则（resolveWaitingCount）", () => {
  test("本页接管着这次运行：一律以本地卡为准——批完（本地 0）就是 ● 而不是快照的旧 1", () => {
    // 快照在建连那一刻是「正在等 1 条审批」；用户随即在本页批完，卡已消失
    expect(sessions.resolveWaitingCount({ pageOwned: true, local: 0, snapshotPending: 1 })).toBe(0)
    // 本页有新卡在等
    expect(sessions.resolveWaitingCount({ pageOwned: true, local: 2, snapshotPending: 0 })).toBe(2)
    expect(sessions.resolveWaitingCount({ pageOwned: true, local: 1, snapshotPending: 3 })).toBe(1)
  })

  test("本页未接管（别处发起的运行）：以快照为准——本地没有卡不代表没人在等", () => {
    expect(sessions.resolveWaitingCount({ pageOwned: false, local: 0, snapshotPending: 1 })).toBe(1)
    expect(sessions.resolveWaitingCount({ pageOwned: false, local: 0, snapshotPending: 0 })).toBe(0)
  })
})

describe("角标渲染：等待态与运行态互斥", () => {
  test("快照里有旧待批 + 本页接管：角标是 ●（不再卡在 ⌛）", () => {
    sessions.setRuntimeInfo({
      [SESSION.id]: { startedAt: Date.now(), pending: [{ type: "approval", toolCallId: "t0" }], toolCalls: 0, bgTasks: [], subRuns: [] },
    } as never)
    state.runs.set(SESSION.id, {} as never) // 本页接管：DOM 即真值，此刻没有卡
    sessions.refreshSessions([SESSION])
    expect(badgeText(SESSION.id)).toBe("●")
  })

  test("快照说在等、本页未接管：角标是 ⌛（别处的卡只能靠快照看见）", () => {
    sessions.setRuntimeInfo({
      [SESSION.id]: { startedAt: Date.now(), pending: [{ type: "approval", toolCallId: "t0" }], toolCalls: 0, bgTasks: [], subRuns: [] },
    } as never)
    sessions.refreshSessions([SESSION])
    expect(badgeText(SESSION.id)).toBe("⌛")
  })

  test("任务结束：角标整体消失（不残留 ⌛）", () => {
    sessions.setRuntimeInfo({
      [SESSION.id]: { startedAt: Date.now(), pending: [{ type: "approval", toolCallId: "t0" }], toolCalls: 0, bgTasks: [], subRuns: [] },
    } as never)
    sessions.refreshSessions([SESSION])
    expect(badgeText(SESSION.id)).toBe("⌛")

    sessions.markSessionRunning(SESSION.id, false)
    expect(badgeText(SESSION.id)).toBeUndefined()
  })
})
