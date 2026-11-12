/**
 * 人格出站治理用例（core/tools/persona-outbound-review.ts）。
 *
 * 判定链：非飞书目标 / 非 send-reply 放行 → 普通会话放行 → 人格 as_user=false 拒绝 →
 * 审核关（JSON 里 reviewOutbound=false）放行 → 相期卡缺失拒绝 → 卡未送达拒绝 → 超时拒绝 → 驳回拒绝 → 批准放行。
 *
 * ## 为何要清掉进程里的 GEBAI_PERSONA_<ID>
 *
 * `gate` 在调用方未注入 `deps.env` 时现读 `process.env`，而 bun 会自动加载仓库根的
 * `.env`——那是**真实部署配置**（可能写着 `reviewOutbound:false`）。不清掉的话，
 * 这些用例的结果取决于本机 .env 内容：在配了人格的机器上全部变成“审核已关”。
 * 测试必须只看自己写的输入。
 */
import { describe, expect, test, beforeEach, afterAll } from "bun:test"
import {
  outboundReviewEnabled,
  personaOutboundGate,
  isAuthFailure,
  notifyAuthNeeded,
  ensureXiangqiReady,
  _resetXiangqiRecoveryForTest,
  asPlainCodeBlock,
} from "./persona-outbound-review"

/** 宿主的 `GEBAI_PERSONA_*`（来自 `.env`）：用例期间摘掉，事后原样还回。 */
const savedPersonaEnv = Object.fromEntries(
  Object.entries(process.env).filter(([k]) => k.startsWith("GEBAI_PERSONA_")),
) as Record<string, string>
const isolatePersonaEnv = (): void => {
  for (const k of Object.keys(process.env)) if (k.startsWith("GEBAI_PERSONA_")) delete process.env[k]
}
beforeEach(isolatePersonaEnv)
afterAll(() => {
  Object.assign(process.env, savedPersonaEnv)
})

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

describe("outboundReviewEnabled：从 GEBAI_PERSONA_<ID> 数组里读", () => {
  const envOf = (item: Record<string, unknown>) => ({ GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([item]) })

  test("缺省 true（审核默认开，安全的那一侧）", () => {
    expect(outboundReviewEnabled("xiaoshuang", {})).toBe(true)
  })
  test("显式 false → 关", () => {
    expect(outboundReviewEnabled("xiaoshuang", envOf({ reviewOutbound: false }))).toBe(false)
  })
  test("显式 true → 开（与缺省同效，但明确写出来）", () => {
    expect(outboundReviewEnabled("xiaoshuang", envOf({ reviewOutbound: true }))).toBe(true)
  })
  test("分身级优先于人格级（给了 driverId 时）", () => {
    const env = {
      GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([
        { reviewOutbound: true },
        { name: "support", reviewOutbound: false },
      ]),
    }
    expect(outboundReviewEnabled("xiaoshuang", env, "support")).toBe(false)
    expect(outboundReviewEnabled("xiaoshuang", env)).toBe(true)
  })
  test("拿不到 driverId 时：任一分身写了 false 就生效（不默默变回开）", () => {
    const env = { GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([{ name: "support", reviewOutbound: false }]) }
    expect(outboundReviewEnabled("xiaoshuang", env)).toBe(false)
  })
  test("无人格归属 → true（不是人格发言，走不到审核卡这一步）", () => {
    expect(outboundReviewEnabled(null, {})).toBe(true)
  })
  test("旧变量已不再生效（已删）", () => {
    expect(outboundReviewEnabled("xiaoshuang", { GEBAI_PERSONA_OUTBOUND_REVIEW: "false" })).toBe(true)
  })
})

describe("personaOutboundGate 判定链", () => {
  test("非飞书目标放行（jira 等）", async () => {
    const r = await personaOutboundGate(ctxWithPersona("xiaoshuang") as never, { target: "jira:PROJ", verb: "submit", args: {} })
    expect(r.allowed).toBe(true)
  })

  test("**实例名不是 feishu 也要治理**（feishu_out / ops_bot 都算飞书）", async () => {
    const types = new Map([
      ["feishu_out", "feishu"],
      ["ops_bot", "feishu"],
    ])
    for (const inst of ["feishu_out", "ops_bot"]) {
      const r = await personaOutboundGate(
        { ...ctxWithPersona("xiaoshuang"), channels: { instanceTypes: () => types } } as never,
        { target: `${inst}:oc_x`, verb: "send", args: { as_user: false, text: "hi" } },
      )
      console.log(`    实测 ${inst}: ${JSON.stringify(r)}`)
      expect(r.allowed).toBe(false)
      expect(r.reason).toContain("机器人身份")
    }
  })

  test("按类型判定不会扩大化：jira 实例仍不被飞书治理拦", async () => {
    const types = new Map([["jira_main", "jira"]])
    const r = await personaOutboundGate(
      { ...ctxWithPersona("xiaoshuang"), channels: { instanceTypes: () => types } } as never,
      { target: "jira_main:PROJ", verb: "submit", args: { as_user: false } },
    )
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

  test("审核关（JSON 里 reviewOutbound=false）→ 放行（review=disabled）", async () => {
 const r = await personaOutboundGate(
 ctxWithPersona("xiaoshuang") as never,
 { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi" } },
 {
 askCard: askCardOf("deliver-reject"),
 env: { GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([{ reviewOutbound: false }]) },
 },
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

  /**
   * 纯文本 vs markdown 的观感一致性。
   *
   * 审核卡用 MarkdownView 渲染，而被审内容发到飞书是**纯文本**。
   * 直接拼会让 `##`、`*`、`-` 在卡片上被当标题/列表解释，预览与实际发出不一致——
   * 而审核卡的全部意义就是让人看到「将会发出什么」。
   */
  test("纯文本正文包进围栏代码块（markdown 不解析它）", async () => {
    const ask = askCardOf("deliver-approve")
    const plain = "## 一、判定与处置\n* 训练频次检查\n1. 第一条\n不要 **随便** 改"
    await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: plain } },
      { askCard: ask },
    )
    const sent = ask.calls[0] as { body: string }
    // 原文逐字在内（断言内容不丢）
    expect(sent.body).toContain("## 一、判定与处置")
    expect(sent.body).toContain("不要 **随便** 改")
    // 且被围栏包住：原文紧跟在 ```text 之后、``` 之前
    expect(sent.body).toContain(`\`\`\`text\n${plain}\n\`\`\``)
  })

  test("正文自带反引号围栏时升级围栏长度（防提前关闭）", () => {
    const withFence = "示例：\n```js\nconst a = 1\n```"
    const wrapped = asPlainCodeBlock(withFence)
    // 内容里最长连续反引号是 3 个 → 外层用 4 个
    expect(wrapped.startsWith("````text\n")).toBe(true)
    expect(wrapped.endsWith("\n````")).toBe(true)
    // 原文整段逐字在内（不被自己在内部的 ``` 截断）
    expect(wrapped).toContain(withFence)
  })
})

/**
 * 卡片渲染契约：审核卡的头尾说明是 markdown（需要渲染），只有**消息正文**是代码块。
 * 反向锚住：不要把“目标”也包进代码块——那会让卡片看起来像一坨终端输出。
 */
describe("审核卡正文形态", () => {
  test("目标与缘由保持 markdown 强调，正文才是代码块", async () => {
    const ask = askCardOf("deliver-approve")
    await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi", why: "回复他" } },
      { askCard: ask },
    )
    const body = (ask.calls[0] as { body: string }).body
    expect(body).toContain("**目标**：")
    expect(body).toContain("**缘由**：")
    // 正文在代码块里，不在 markdown 强词里
    expect(body).toContain("```text\nhi\n```")
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
describe("让位窗口（C）：只拦主动插话，不拦回复", () => {
  /** 一个“用户刚说过话”的窗口。 */
  function windowWith(chatId: string, ms = 60_000) {
    const w = new (require("../session/human-speech-window").HumanSpeechWindow)({ windowMs: ms })
    w.note(chatId)
    return w
  }

  /**
   * **真实事故回归**：上一版只要“用户刚说过话”就拒，导致每次正常问答都被拦——
   * 用户 20:25 问「多久元旦」，人格处理完想回复时落在 180 秒窗口内被拒，
   * 用户 20:31 才收到回复（延迟 6 分钟）。
   */
  test("引用回复（reply + reply_to）**必须放行**——那是在答他刚问的那句", async () => {
    const w = windowWith("oc_x")
    const r = await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "reply", args: { as_user: true, text: "还有 55 天", reply_to: "om_1" } },
      { askCard: askCardOf("deliver-approve"), humanSpeech: w },
    )
    expect(r.allowed).toBe(true)
  })

  test("带 root 的发送（回话题里那条消息）同样放行", async () => {
    const w = windowWith("oc_x")
    const r = await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "还有 55 天", root: "om_1" } },
      { askCard: askCardOf("deliver-approve"), humanSpeech: w },
    )
    expect(r.allowed).toBe(true)
  })

  test("裸 send（无 reply_to/root）= 主动插话 → 窗口内被拦", async () => {
    const w = windowWith("oc_x")
    const r = await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "顺便说一句" } },
      { askCard: askCardOf("deliver-approve"), humanSpeech: w },
    )
    expect(r.allowed).toBe(false)
    expect(r.reason).toContain("主动")
    // 理由里要给出可行出路，否则模型会以为彻底发不出去
    expect(r.reason).toContain("reply")
  })

  test("窗口内但 explicit:true → 放行", async () => {
    const w = windowWith("oc_x")
    const r = await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi", explicit: true } },
      { askCard: askCardOf("deliver-approve"), humanSpeech: w },
    )
    expect(r.allowed).toBe(true)
  })

  test("别的群说话不影响本群", async () => {
    const w = windowWith("oc_other")
    const r = await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi" } },
      { askCard: askCardOf("deliver-approve"), humanSpeech: w },
    )
    expect(r.allowed).toBe(true)
  })

  test("未注入让位窗口 → 旧行为（不能因装配缺失把发送全堵死）", async () => {
    const r = await personaOutboundGate(
      ctxWithPersona("xiaoshuang") as never,
      { target: "feishu:oc_x", verb: "send", args: { as_user: true, text: "hi" } },
      { askCard: askCardOf("deliver-approve") },
    )
    expect(r.allowed).toBe(true)
  })
})
