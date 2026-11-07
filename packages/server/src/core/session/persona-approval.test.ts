/**
 * 审批桥的反向验证。
 *
 * 每条用例都对应一个「错了就会出真事故」的形态：
 *
 * | 变异 | 后果 |
 * |---|---|
 * | 相期不在时放行 | 一张送不出去的卡变成空白支票：命令照跑，人完全不知情 |
 * | 推卡抛错当批准 | 异常被解释成放行令 |
 * | 超时当批准 | 人没看见 = 默认执行（与 fail-closed 相反） |
 * | 重试再推一张卡 | 引擎重试同一调用，窗口被刷成瀑布，用户不知道哪张有效 |
 * | 不判人格会话 | 普通 web 会话的审批被重复推一张到桌面，用户在网页点了也照旧挂着 |
 * | 选项映射错（title → label） | 用户在卡上看到「选项 1」，答回来是空值，模型以为没人答 |
 */

import { describe, expect, test } from "bun:test"
import { ApprovalBridge, describeTool, summarizeArgs, type AskCardToolLike } from "./persona-approval"

/** 可编排的 AskCard 替身：记录每次调用，按预设回答。 */
function makeAsk(reply: { delivered?: boolean; answered?: boolean; choice?: string; throw?: string }) {
  const calls: Array<Record<string, unknown>> = []
  const tool: AskCardToolLike = {
    async execute(args) {
      calls.push(args)
      if (reply.throw) throw new Error(reply.throw)
      return {
        output: "ok",
        data: {
          delivered: reply.delivered !== false,
          answered: reply.answered === true,
          ...(reply.choice !== undefined ? { answer: { choice: reply.choice } } : {}),
        },
      }
    },
  }
  return { tool, calls }
}

/** 可编排的引擎替身：记录结论提交。 */
function makeEngine(opts: { persona?: boolean } = {}) {
  const approvals: Array<{ sessionId: string; toolCallId: string; approve: boolean }> = []
  const choices: Array<{ sessionId: string; choiceId: string; selection: string | string[] | null }> = []
  return {
    approvals,
    choices,
    engine: {
      async decideApproval(sessionId: string, toolCallId: string, approve: boolean) {
        approvals.push({ sessionId, toolCallId, approve })
        return "ok"
      },
      async decideChoice(sessionId: string, choiceId: string, selection: string | string[] | null) {
        choices.push({ sessionId, choiceId, selection })
        return "ok"
      },
      async isPersonaSession() {
        return opts.persona !== false
      },
    },
  }
}

const REQ = { sessionId: "s1", toolCallId: "tc1", tool: "sh", args: { command: "rm -rf /tmp/x" } }

describe("审批桥：fail-closed 的三条路径", () => {
  test("相期卡未装配 → 拒绝（不能静默放行）", async () => {
    const { engine, approvals } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "ask" } })
    await b.handleApproval(REQ)
    expect(approvals).toEqual([{ sessionId: "s1", toolCallId: "tc1", approve: false }])
  })

  test("相期未连接 → 拒绝且**不等 5 分钟**（送不到 ≠ 没人反对）", async () => {
    const ask = makeAsk({ delivered: true, answered: true, choice: "approve" })
    const { engine, approvals } = makeEngine()
    const b = new ApprovalBridge({
      engine,
      deps: { mode: "ask", askCard: ask.tool, isXiangqiConnected: () => false },
    })
    await b.handleApproval(REQ)
    expect(ask.calls).toHaveLength(0) // 连推都不推（推也是白推）
    expect(approvals[0]!.approve).toBe(false)
  })

  test("推卡抛错 → 拒绝（异常不得变成放行令）", async () => {
    const ask = makeAsk({ throw: "相期炸了" })
    const { engine, approvals } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleApproval(REQ)
    expect(approvals[0]!.approve).toBe(false)
  })

  test("送卡未送达（delivered=false）→ 拒绝", async () => {
    const ask = makeAsk({ delivered: false })
    const { engine, approvals } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleApproval(REQ)
    expect(approvals[0]!.approve).toBe(false)
  })

  test("等待超时（answered=false）→ 拒绝，且**不记住**这个结论（卡还在，人稍后点了还认）", async () => {
    const ask = makeAsk({ delivered: true, answered: false })
    const { engine, approvals } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleApproval(REQ)
    expect(approvals[0]!.approve).toBe(false)
    expect(b.stats().remembered).toBe(0)
  })
})

describe("审批桥：问人与结论", () => {
  test("用户批准 → approve=true，卡片带完整命令（那是他要判断的东西）", async () => {
    const ask = makeAsk({ answered: true, choice: "approve" })
    const { engine, approvals } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleApproval({ ...REQ, personaName: "小霜" })
    expect(approvals[0]!.approve).toBe(true)
    const body = String(ask.calls[0]!.body)
    expect(body).toContain("rm -rf /tmp/x")
    expect(String(ask.calls[0]!.title)).toContain("小霜")
    expect(String(ask.calls[0]!.title)).toContain("执行一条命令")
  })

  test("用户驳回 → approve=false，且消息提示“不要重试”", async () => {
    const ask = makeAsk({ answered: true, choice: "reject" })
    const { engine, approvals } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleApproval(REQ)
    expect(approvals[0]!.approve).toBe(false)
  })

  test("同一调用重试（引擎重投同一 toolCallId）→ 不重复推卡，按记住的结论回答", async () => {
    const ask = makeAsk({ answered: true, choice: "approve" })
    const { engine, approvals } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleApproval(REQ)
    await b.handleApproval({ ...REQ, retries: 1 })
    expect(ask.calls).toHaveLength(1) // 窗口上只有一张卡
    expect(approvals.map((a) => a.approve)).toEqual([true, true])
  })

  test("并发重复请求（同一 toolCallId 同时到达）→ 单飞，只推一张", async () => {
    const ask = makeAsk({ answered: true, choice: "approve" })
    const { engine } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await Promise.all([b.handleApproval(REQ), b.handleApproval(REQ)])
    expect(ask.calls).toHaveLength(1)
  })
})

describe("审批桥：姿态与作用域", () => {
  test("mode=auto → 直接通过，且不推卡", async () => {
    const ask = makeAsk({ answered: true, choice: "approve" })
    const { engine, approvals } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "auto", askCard: ask.tool } })
    await b.handleApproval(REQ)
    expect(approvals[0]!.approve).toBe(true)
    expect(ask.calls).toHaveLength(0)
  })

  test("mode=deny → 直接拒绝，且不推卡", async () => {
    const ask = makeAsk({ answered: true, choice: "approve" })
    const { engine, approvals } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "deny", askCard: ask.tool } })
    await b.handleApproval(REQ)
    expect(approvals[0]!.approve).toBe(false)
    expect(ask.calls).toHaveLength(0)
  })

  test("非人格会话 → 完全不干预（网页里有真前端，不重复弹卡）", async () => {
    const ask = makeAsk({ answered: true, choice: "approve" })
    const { engine, approvals } = makeEngine({ persona: false })
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleApproval(REQ)
    expect(ask.calls).toHaveLength(0)
    expect(approvals).toHaveLength(0)
  })

  test("判定人格会话失败 → 不推卡也不给结论（引擎自己按超时收口）", async () => {
    const ask = makeAsk({ answered: true, choice: "approve" })
    const { approvals } = makeEngine()
    const engine = {
      decideApproval: async () => "ok",
      decideChoice: async () => "ok",
      isPersonaSession: async () => {
        throw new Error("store 挂了")
      },
    }
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleApproval(REQ)
    expect(ask.calls).toHaveLength(0)
    expect(approvals).toHaveLength(0)
  })
})

describe("审批桥：选择请求（ask 选项分支）", () => {
  const CH = { sessionId: "s1", choiceId: "c1", prompt: "用哪个方案？", options: ["甲", "乙"], multi: false }

  test("字符串选项 → 卡片 label/value 都是原文，答复原样回引擎", async () => {
    const ask = makeAsk({ answered: true, choice: "乙" })
    const { engine, choices } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleChoice(CH)
    const opts = ask.calls[0]!.options as Array<{ label: string; value: string }>
    expect(opts.map((o) => o.label)).toEqual(["甲", "乙"])
    expect(choices[0]!.selection).toBe("乙")
  })

  test("{title,description} 选项 → label 取 title、hint 取 description（否则用户在卡上看到「选项 1」）", async () => {
    const ask = makeAsk({ answered: true, choice: "方案 A" })
    const { engine } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleChoice({ ...CH, options: [{ title: "方案 A", description: "快但糙" }, { title: "方案 B" }] })
    const opts = ask.calls[0]!.options as Array<{ label: string; value: string; hint?: string }>
    expect(opts[0]).toEqual({ label: "方案 A", value: "方案 A", hint: "快但糙" })
  })

  test("超时/未送达 → 回 null（= 用户未作答，模型自行决策），不是“拒绝”", async () => {
    const ask = makeAsk({ delivered: true, answered: false })
    const { engine, choices } = makeEngine()
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleChoice(CH)
    expect(choices[0]!.selection).toBeNull()
  })

  test("非人格会话的选择请求 → 不干预", async () => {
    const ask = makeAsk({ answered: true, choice: "甲" })
    const { engine, choices } = makeEngine({ persona: false })
    const b = new ApprovalBridge({ engine, deps: { mode: "ask", askCard: ask.tool } })
    await b.handleChoice(CH)
    expect(ask.calls).toHaveLength(0)
    expect(choices).toHaveLength(0)
  })
})

describe("审批桥：卡片文案", () => {
  test("已知工具说人话，未知工具诚实显示原名", () => {
    expect(describeTool("sh")).toBe("执行一条命令")
    expect(describeTool("weird_tool")).toBe("weird_tool")
  })

  test("参数摘要限长（人要在小卡片上读完）", () => {
    const long = summarizeArgs({ command: "x".repeat(3000) })
    expect(long.length).toBeLessThan(2000)
    expect(long).toContain("共 3000 字符")
  })

  test("空参数如实说明", () => {
    expect(summarizeArgs(undefined)).toBe("（无参数）")
    expect(summarizeArgs({})).toBe("（无参数）")
  })
})
