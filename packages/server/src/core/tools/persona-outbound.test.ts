/**
 * 人格出站约束用例（core/tools/persona-outbound.ts）。
 *
 * 覆盖判定链：普通会话放行 / 人格未配置放行 / 人格已配置按群收窄（命中放行、越界拒绝）/
 * 非飞书目标不过滤。
 */
import { describe, expect, test } from "bun:test"
import { feishuChatIdOf, personOutboundDecision, personaChatWhitelist, personaOutboundBlock } from "./persona-outbound"

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

const envWith = (personaId: string, chats: string) => ({ [`GEBAI_PERSONA_${personaId.toUpperCase()}_CHATS`]: chats } as Record<string, string>)

describe("feishuChatIdOf", () => {
  test("feishu:oc_xxx → oc_xxx", () => {
    expect(feishuChatIdOf("feishu:oc_abc")).toBe("oc_abc")
  })
  test("非 feishu 渠道 → null", () => {
    expect(feishuChatIdOf("jira:PROJ")).toBeNull()
    expect(feishuChatIdOf("local:memory")).toBeNull()
  })
  test("缺地址 → null", () => {
    expect(feishuChatIdOf("feishu:")).toBeNull()
  })
})

describe("personaChatWhitelist", () => {
  test("未配置 → null（不限群）", () => {
    expect(personaChatWhitelist("finops_auditor", {})).toBeNull()
  })
  test("配置解析：逗号/空格分隔", () => {
    const s = personaChatWhitelist("finops_auditor", envWith("finops_auditor", "oc_a, oc_b"))
    expect(s).not.toBeNull()
    expect(s!.has("oc_a")).toBe(true)
    expect(s!.has("oc_b")).toBe(true)
  })
  test("id 大写归一：小写人格 id 也读同变量", () => {
    expect(personaChatWhitelist("xiaoshuang", { GEBAI_PERSONA_XIAOSHUANG_CHATS: "oc_z" })).not.toBeNull()
  })
})

describe("personOutboundDecision 判定链", () => {
  test("非人格会话 → 放行（不过滤）", async () => {
    const d = await personOutboundDecision(ctxWithPersona(null) as never, "feishu:oc_x", envWith("finops_auditor", "oc_a"))
    expect(d.allowed).toBe(true)
  })

  test("人格未配置 CHATS → 放行", async () => {
    const d = await personOutboundDecision(ctxWithPersona("finops_auditor") as never, "feishu:oc_x", {})
    expect(d.allowed).toBe(true)
    expect(d.personaId).toBe("finops_auditor")
  })

  test("已配置 + 目标在名单内 → 放行（restricted 标记）", async () => {
    const d = await personOutboundDecision(ctxWithPersona("finops_auditor") as never, "feishu:oc_a", envWith("finops_auditor", "oc_a,oc_b"))
    expect(d.allowed).toBe(true)
    expect(d.restricted).toBe(true)
  })

  test("已配置 + 目标越界 → 拒绝（带原因）", async () => {
    const d = await personOutboundDecision(ctxWithPersona("finops_auditor") as never, "feishu:oc_evil", envWith("finops_auditor", "oc_a"))
    expect(d.allowed).toBe(false)
    expect(d.reason).toContain("oc_evil")
    expect(d.reason).toContain("finops_auditor")
  })

  test("非 feishu 目标 → 放行（人格约束只针对飞书群）", async () => {
    const d = await personOutboundDecision(ctxWithPersona("finops_auditor") as never, "jira:PROJ", envWith("finops_auditor", "oc_a"))
    expect(d.allowed).toBe(true)
  })

  test("personaOutboundBlock：越界返回文案，命中返回 null", async () => {
    const block1 = await personaOutboundBlock(ctxWithPersona("finops_auditor") as never, "feishu:oc_evil", envWith("finops_auditor", "oc_a"))
    expect(block1).toContain("oc_evil")
    expect(block1).toContain("finops_auditor")
    const block2 = await personaOutboundBlock(ctxWithPersona("finops_auditor") as never, "feishu:oc_a", envWith("finops_auditor", "oc_a"))
    expect(block2).toBeNull()
  })
})