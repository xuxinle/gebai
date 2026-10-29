/**
 * 人格出站治理用例（core/tools/persona-outbound-review.ts）。
 *
 * 判定链：非 feishu / 非 send-reply 放行 → 普通会话放行 → 人格 as_user=false 拒绝 →
 * 审核关（env）放行 → 相期卡缺失拒绝 → 卡未送达拒绝 → 超时拒绝 → 驳回拒绝 → 批准放行。
 */
import { describe, expect, test, beforeEach } from "bun:test"
import { outboundReviewEnabled, personaOutboundGate, isAuthFailure, notifyAuthNeeded, ensureXiangqiReady, _resetXiangqiRecoveryForTest } from "./persona-outbound-review"

function ctxWithPersona(personaId: string | null, sessionId = "s1") {
  return {
    sessionId,
    user: "admin",
    sessions: {
      store: {
        load: async () =>
          personaId
            ? ({ id: sessionId, persona: { personaId, driverId: "d1", boundAt: 1 } } as never)
            : ({ id: sessionId } as never),
      },
    },
  }
}

/** fake askCard：按脚本返回。 */
function askCardOf(script: "deliver-approve" | "deliver-reject" | "not-delivered" | "timeout") {
  const calls: Array<Record<string, unknown>> = []
  return {
    calls,
    execute: async (args: Record<string, unknown>) => {
      calls.push(args)
      switch (script) {
        case "deliver-approve":
          return { output: "用户已答复：批准", data: { id: "c1", delivered: true, answered: true, answer: { choice: "approve" } } }
        case "deliver-reject":
          return { output: "用户已答复：驳回", data: { id: "c1", delivered: true, answered: true, answer: { choice: "reject" } } }
        case "not-delivered":
          return { output: "相期未连接", data: { id: "c1", delivered: false, answered: false } }
        case "timeout":
          return { output: "超时", data: { id: "c1", delivered: true, answered: false, timedOut: true } }
      }
    },
  }
}

describe("outboundReviewEnabled", () => {
  test("缺省 true（审核默认开）", () => {
    expect(outboundReviewEnabled({})).toBe(true)
  })
  test("off/0/false/no → false", () => {
    expect(outboundReviewEnabled({ GEBAI_PERSONA_OUTBOUND_REVIEW: "false" })).toBe(false)
    expect(outboundReviewEnabled({ GEBAI_PERSONA_OUTBOUND_REVIEW: "off" })).toBe(false)
    expect(outboundReviewEnabled({ GEBAI_PERSONA_OUTBOUND_REVIEW: "0" })).toBe(false)
  })
  test("非关闭值 → true", () => {
    expect(outboundReviewEnabled({ GEBAI_PERSONA_OUTBOUND_REVIEW: "true" })).toBe(true)
  })
})

describe("personaOutboundGate 判定链", () => {
  test("非 feishu 目标放行（jira 等）", async () => {
    const r = await personaOutboundGate(ctxWithPersona("xiaoshuang") as never, { target: "jira:PROJ", verb: "submit", args: {} })
    expect(r.allowed).toBe(true)
  })

  test("feishu observe 类动词放行（只拦 send/reply）", async () => {
    const r = await personaOutboundGate(ctxWithPersona("xiaoshuang") as never, { target: "feishu:oc_x", verb: "other", args: {} })
    expect(r.allowed).toBe(true)
  })

  test("普通会话放行（as_user 任意）", async () => {
    const r = await personaOutboundGate(ctxWithPersona(null) as never, { target: "feishu:oc_x", verb: "send", args: { as_user: false } })
    expect(r.allowed).toBe(true)
    expect(r.review).toBe("not-required")
  })

  test("人格 as_user=false → 拒绝（禁机器人回退）", async () => {
    const r = await personaOutboundGate(ctxWithPersona("xiaoshuang") as never, { target: "feishu:oc_x", verb: "send", args: { as_user: false, text: "hi" } }, { askCard: askCardOf("deliver-approve") })
    expect(r.allowed).toBe(false)
    expect(r.reason).toContain("机器人身份")
    expect(r.reason).toContain("as_user=true")
  })

  test("人格 as_user 缺省（未传）→ 拒绝（必须显式 true）", async () => {
    const r = await personaOutboundGate(ctxWithPersona("xiaoshuang") as never, { target: "feishu:oc_x", verb: "send", args: { text: "hi" } }, { askCard: askCardOf("deliver-approve") })
    expect(r.allowed).toBe(false)
    expect(r.reason).toContain("机器人身份")
  })

  test("审核关（env）→ 放行（review=disabled）", async () => {
    const r = await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi" } },
      { askCard: askCardOf("deliver-reject"), env: { GEBAI_PERSONA_OUTBOUND_REVIEW: "false" } },
    )
    expect(r.allowed).toBe(true)
    expect(r.review).toBe("disabled")
  })

  test("askCard 未装配 → 拒绝（不静默放行）", async () => {
    const r = await personaOutboundGate(ctxWithPersona("xiaoshuang") as never, { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi" } }, {})
    expect(r.allowed).toBe(false)
    expect(r.reason).toContain("未装配")
  })

  test("卡未送达（相期未连）→ 拒绝 fail-closed", async () => {
    const r = await personaOutboundGate(ctxWithPersona("xiaoshuang") as never, { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi" } }, { askCard: askCardOf("not-delivered") })
    expect(r.allowed).toBe(false)
    expect(r.reason).toContain("相期未连接")
  })

  test("审核超时 → 拒绝 fail-closed", async () => {
    const r = await personaOutboundGate(ctxWithPersona("xiaoshuang") as never, { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi" } }, { askCard: askCardOf("timeout") })
    expect(r.allowed).toBe(false)
    expect(r.reason).toContain("无人答复")
  })

  test("用户驳回 → 拒绝（不重试）", async () => {
    const r = await personaOutboundGate(ctxWithPersona("xiaoshuang") as never, { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi" } }, { askCard: askCardOf("deliver-reject") })
    expect(r.allowed).toBe(false)
    expect(r.reason).toContain("驳回")
  })

  test("用户批准 → 放行（review=approved，卡正文含目标与全文）", async () => {
    const ask = askCardOf("deliver-approve")
    const r = await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "今天几号", why: "回复许鑫乐" } },
      { askCard: ask },
    )
    expect(r.allowed).toBe(true)
    expect(r.review).toBe("approved")
    // 卡内容检查：正文带目标群、消息全文、缘由
    const sent = ask.calls[0] as { title: string; body: string; options: Array<{ value: string }> }
    expect(sent.body).toContain("feishu:oc_x")
    expect(sent.body).toContain("今天几号")
    expect(sent.body).toContain("回复许鑫乐")
    expect(sent.options.map((o) => o.value)).toContain("approve")
  })

  test("reply 也走审核（带 reply_to 上下文）", async () => {
    const ask = askCardOf("deliver-approve")
    const r = await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "reply", args: { as_user: true, text: "ok", reply_to: "om_1" } },
      { askCard: ask },
    )
    expect(r.allowed).toBe(true)
    const sent = ask.calls[0] as { body: string }
    expect(sent.body).toContain("om_1")
  })

  test("超长消息截断（审核卡正文 800 上限）", async () => {
    const ask = askCardOf("deliver-approve")
    const long = "长".repeat(2000)
    await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: long } },
      { askCard: ask },
    )
    const sent = ask.calls[0] as { body: string }
    expect(sent.body).toContain("已截断")
  })
})

describe("授权失败识别与弹卡", () => {
  test("isAuthFailure 匹配两类授权失败文案", () => {
    expect(isAuthFailure("动作失败：as_user=true 但飞书用户授权不可用：access_token 已过期")).toBe(true)
    expect(isAuthFailure("as_user=true 需要拟人模式已启用（GEBAI_PERSONA=true）且已完成飞书授权——当前未注入 personaCredentials")).toBe(true)
    expect(isAuthFailure("动作失败：飞书接口失败 code=230027")).toBe(false)
  })

  test("notifyAuthNeeded：推送成功 true / 抛错 false / 未注入 false", async () => {
    expect(await notifyAuthNeeded(async () => {})).toBe(true)
    expect(await notifyAuthNeeded(async () => { throw new Error("boom") })).toBe(false)
    expect(await notifyAuthNeeded(undefined)).toBe(false)
  })
})

describe("ensureXiangqiReady 恢复闭环", () => {
  beforeEach(() => _resetXiangqiRecoveryForTest())

  test("已连接 → 不重启直接可用", async () => {
    let restarted = 0
    const r = await ensureXiangqiReady({ isXiangqiConnected: () => true, restartXiangqi: async () => { restarted++ } })
    expect(r).toEqual({ ok: true, restarted: false })
    expect(restarted).toBe(0)
  })

  test("可恢复：未连接 → 重启 → 等连上 → ok", async () => {
    let connected = false
    let restarted = 0
    const r = await ensureXiangqiReady({
      isXiangqiConnected: () => connected,
      restartXiangqi: async () => { restarted++; connected = true }, // 重启后即连上（模拟）
      reconnectWaitMs: 500,
    })
    expect(r.ok).toBe(true)
    expect(r.restarted).toBe(true)
    expect(restarted).toBe(1)
  })

  test("重启后仍未连上 → 不 ok（fail-closed）", async () => {
    const r = await ensureXiangqiReady({
      isXiangqiConnected: () => false,
      restartXiangqi: async () => {},
      reconnectWaitMs: 300,
    })
    expect(r.ok).toBe(false)
    expect(r.restarted).toBe(true)
  })

  test("无重启能力（未注入）→ 不 ok 且不重启", async () => {
    const r = await ensureXiangqiReady({ isXiangqiConnected: () => false })
    expect(r.ok).toBe(false)
    expect(r.restarted).toBe(false)
  })

  test("防抖：冷却期内不重复重启（60s）", async () => {
    let connected = false
    let restarted = 0
    const deps = { isXiangqiConnected: () => connected, restartXiangqi: async () => { restarted++ }, reconnectWaitMs: 300 }
    // 第一次：重启 + 等连（模拟一直连不上）
    await ensureXiangqiReady(deps)
    expect(restarted).toBe(1)
    // 第二次（立刻）：冷却期 → 不重启，只是等待
    await ensureXiangqiReady(deps)
    expect(restarted).toBe(1) // 没再重启
  })

  test("gate 集成：相期不可用且重启失败 → 拒绝发送（fail-closed）", async () => {
    const r = await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi" } },
      {
        askCard: askCardOf("deliver-approve"),
        isXiangqiConnected: () => false,
        restartXiangqi: async () => {}, // 重启后仍连不上
        reconnectWaitMs: 300,
      },
    )
    expect(r.allowed).toBe(false)
    expect(r.reason).toContain("自动重启")
  })

  test("gate 集成：相期重启后连上 → 审核正常放行", async () => {
    let connected = false
    const r = await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi" } },
      {
        askCard: askCardOf("deliver-approve"),
        isXiangqiConnected: () => connected,
        restartXiangqi: async () => { connected = true },
        reconnectWaitMs: 500,
      },
    )
    expect(r.allowed).toBe(true)
    expect(r.review).toBe("approved")
  })
})