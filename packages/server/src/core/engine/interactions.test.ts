/** 交互等待机制：决策提交的失效语义（expired）、等待期心跳，与交互了结事件的发布。 */
import { describe, expect, test } from "bun:test"
import {
  decideApproval,
  decideChoice,
  decideCaptureResult,
  decideDrawResult,
  decideEnvResult,
  markSettled,
  startInteractionAlive,
  waitForChoice,
  waitForEnv,
  type TaskState,
} from "./interactions"

/** 最小任务状态（只填交互相关字段；其余用不到）。 */
function fakeTask(): TaskState {
  return {
    controller: new AbortController(),
    startedAt: Date.now(),
    user: "default",
    settled: new Set(),
    activeTools: new Map(),
    approvals: new Map(),
    pendingDecisions: new Map(),
    retries: new Map(),
    choices: new Map(),
    pendingChoices: new Map(),
    draws: new Map(),
    pendingDraws: new Map(),
    captures: new Map(),
    pendingCaptures: new Map(),
    disabledTools: [],
    interactionMode: "realtime",
    outputMode: "streaming",
    env: {},
    envRequests: new Map(),
    pendingEnvRequests: new Map(),
  }
}

describe("交互决策的失效语义", () => {
  test("任务不存在（已结束/服务重启）→ expired，不再静默假成功", () => {
    expect(decideApproval(undefined, "a1", true)).toBe("expired")
    expect(decideChoice(undefined, "c1", "x")).toBe("expired")
    expect(decideEnvResult(undefined, "e1", "v", () => true)).toBe("expired")
    expect(decideDrawResult(undefined, "d1", { ok: true })).toBe("expired")
    expect(decideCaptureResult(undefined, "cap1", { html: "" }, 100, 100)).toBe("expired")
  })

  test("已了结的交互：迟到的审批/选择/填值判 expired（不排队、不假成功）", () => {
    const task = fakeTask()
    markSettled(task, "a1")
    markSettled(task, "c1")
    markSettled(task, "e1")
    expect(decideApproval(task, "a1", true)).toBe("expired")
    expect(decideChoice(task, "c1", "x")).toBe("expired")
    expect(decideEnvResult(task, "e1", "v", () => true)).toBe("expired")
    expect(task.pendingDecisions.size).toBe(0) // 不排队
    expect(task.pendingChoices.size).toBe(0)
    expect(task.pendingEnvRequests.size).toBe(0)
  })

  test("先于注册到达的决策仍排队（同任务内的正常竞态）：返回 queued", () => {
    const task = fakeTask()
    expect(decideApproval(task, "a1", true)).toBe("queued")
    expect(task.pendingDecisions.get("a1")).toBe(true)
    expect(decideChoice(task, "c1", "x")).toBe("queued")
    expect(decideEnvResult(task, "e1", "v", () => true)).toBe("queued")
  })

  test("送达等待中的交互：返回 ok 并消费等待回调（审批拒绝同时中止任务）", () => {
    const task = fakeTask()
    let verdict: unknown
    task.approvals.set("a1", {
      sessionId: "s1",
      toolCallId: "a1",
      tool: "sh",
      resolve: (v) => (verdict = v),
      timer: setTimeout(() => {}, 60_000),
    })
    expect(decideApproval(task, "a1", false)).toBe("ok")
    expect(verdict).toBe("rejected")
    expect(task.approvals.size).toBe(0)
    expect(task.settled.has("a1")).toBe(true)
    expect(task.controller.signal.aborted).toBe(true) // 拒绝即中止当前生成
  })
})

describe("交互等待心跳", () => {
  test("按间隔发布 event.interaction.alive（带 kind/id），停止后不再发", async () => {
    const seen: Array<{ sessionId: string; type: string; payload: Record<string, unknown> }> = []
    const stop = startInteractionAlive("s1", (sessionId, type, payload) => seen.push({ sessionId, type, payload }), "approval", "a1", 10)
    await new Promise((r) => setTimeout(r, 35))
    stop()
    const n = seen.length
    expect(n).toBeGreaterThanOrEqual(2)
    expect(seen[0]).toEqual({ sessionId: "s1", type: "event.interaction.alive", payload: { kind: "approval", id: "a1" } })
    await new Promise((r) => setTimeout(r, 30))
    expect(seen.length).toBe(n) // 停止后不再发布
  })
})

describe("交互了结告知（多页面各自的卡片随之处置）", () => {
  function collector() {
    const seen: Array<{ type: string; payload: Record<string, unknown> }> = []
    const publish = (_sessionId: string, type: string, payload: Record<string, unknown>) => void seen.push({ type, payload })
    return { seen, publish }
  }

  test("选择被作答：发布 event.interaction.settled（kind=choice，id=choiceId）", async () => {
    const task = fakeTask()
    const { seen, publish } = collector()
    const pending = waitForChoice("s1", task, publish, "选一个", ["A", "B"])
    const choiceId = String(seen.find((e) => e.type === "event.choice.request")!.payload.choiceId)
    expect(decideChoice(task, choiceId, "A")).toBe("ok")
    expect(await pending).toEqual({ kind: "option", value: "A" })
    expect(seen.filter((e) => e.type === "event.interaction.settled")).toEqual([
      { type: "event.interaction.settled", payload: { kind: "choice", id: choiceId } },
    ])
  })

  test("选择等待超时/取消：同样发布了结（别处留着的卡片据此处置）", async () => {
    const task = fakeTask()
    const { seen, publish } = collector()
    const ac = new AbortController()
    const pending = waitForChoice("s1", task, publish, "选一个", ["A"], undefined, ac.signal)
    const choiceId = String(seen.find((e) => e.type === "event.choice.request")!.payload.choiceId)
    ac.abort()
    expect(await pending).toBeNull()
    expect(seen.filter((e) => e.type === "event.interaction.settled")).toEqual([
      { type: "event.interaction.settled", payload: { kind: "choice", id: choiceId } },
    ])
  })

  test("填值被作答：发布 event.interaction.settled（kind=env，id=envId）", async () => {
    const task = fakeTask()
    const { seen, publish } = collector()
    const pending = waitForEnv("s1", task, publish, () => true, "MY_KEY", "用途", false)
    const envId = String(seen.find((e) => e.type === "event.env.request")!.payload.envId)
    expect(decideEnvResult(task, envId, "v", () => true)).toBe("ok")
    expect(await pending).toBe(true)
    expect(seen.filter((e) => e.type === "event.interaction.settled")).toEqual([
      { type: "event.interaction.settled", payload: { kind: "env", id: envId } },
    ])
  })
})
