/**
 * 出站治理必须按**渠道类型**判定，不能按实例名字串判定。
 *
 * 起因是一个真实部署：出站实例叫 `feishu_out`（入站 `feishu_in`），人格治理链路
 * 全部写死 `target.startsWith("feishu:")`，于是整套出站治理对这份配置**整段跳过**——
 * 禁机器人回退、出站审核卡、群白名单一个都没生效，而日志里一个字都没有
 * （看起来完全像“人格就是没配审核”）。
 *
 * 钉住三件事：
 * 1. `feishu_out:oc_x` 与 `feishu:oc_x` 的治理结论**必须一致**
 * 2. 其它类型（`jira` / `local`）仍不经过飞书治理（不能扩大化）
 * 3. 实例名与类型名同形时（未装配注册表、测试桩）行为与旧写法一致
 */
import { describe, expect, test } from "bun:test"
import {
  channelTypeOf,
  feishuChatIdOf,
  isFeishuTarget,
  personOutboundDecision,
  personaChatWhitelist,
} from "./persona-outbound"

/** 实例名 → 类型名（真实形态：注册表给出；这里手工造同样的表）。 */
const TYPES = new Map<string, string>([
  ["feishu_in", "feishu"],
  ["feishu_out", "feishu"],
  ["ops_bot", "feishu"],
  ["jira_main", "jira"],
  ["local", "local"],
])

function ctxWith(channels?: { instanceTypes(): Map<string, string> }, personaId: string | null = "xiaoshuang") {
  return {
    sessionId: "s1",
    user: "admin",
    ...(channels ? { channels } : {}),
    sessions: {
      store: {
        load: async () =>
          personaId
            ? ({ id: "s1", persona: { personaId, driverId: "support", boundAt: 1 } } as never)
            : ({ id: "s1" } as never),
      },
    },
  }
}

const registryOf = (types: Map<string, string>) => ({ instanceTypes: () => types })

/** 用户机器的真实形态：收 feishu_in、发 feishu_out，两个方向都只准一个群。 */
const ENV = {
  GEBAI_PERSONA_ENABLE: "xiaoshuang",
  GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([
    {
      name: "support",
      listen: { channels: ["feishu_in"], chats: ["oc_in"] },
      send: { channels: ["feishu_out"], chats: ["oc_out"] },
    },
  ]),
} as Record<string, string>

describe("channelTypeOf / isFeishuTarget：按实例名查类型", () => {
  test("实例名 → 类型名", () => {
    expect(channelTypeOf("feishu_out", TYPES)).toBe("feishu")
    expect(channelTypeOf("feishu_in", TYPES)).toBe("feishu")
    expect(channelTypeOf("ops_bot", TYPES)).toBe("feishu")
    expect(channelTypeOf("jira_main", TYPES)).toBe("jira")
  })

  test("大小写归一（target 里写大写也认出类型）", () => {
    expect(channelTypeOf("FeiShu_Out", TYPES)).toBe("feishu")
  })

  test("未登记 → 回落成名字本身（类型名与实例名同形的老部署行为不变）", () => {
    expect(channelTypeOf("feishu", new Map())).toBe("feishu")
    expect(channelTypeOf("jira", undefined)).toBe("jira")
    expect(channelTypeOf("", TYPES)).toBe("")
  })

  test("isFeishuTarget：feishu_out / ops_bot 都算飞书，jira / local 不算", () => {
    expect(isFeishuTarget("feishu_out:oc_out", TYPES)).toBe(true)
    expect(isFeishuTarget("ops_bot:oc_out", TYPES)).toBe(true)
    expect(isFeishuTarget("jira_main:PROJ", TYPES)).toBe(false)
    expect(isFeishuTarget("local:memory", TYPES)).toBe(false)
    expect(isFeishuTarget("没有冒号", TYPES)).toBe(false)
  })
})

describe("feishuChatIdOf：实例名不影响取群 id", () => {
  test("feishu_out:oc_x 与 feishu:oc_x 取到同一个群 id", () => {
    expect(feishuChatIdOf("feishu_out:oc_x", TYPES)).toBe("oc_x")
    expect(feishuChatIdOf("feishu:oc_x", new Map())).toBe("oc_x")
    expect(feishuChatIdOf("ops_bot:oc_x", TYPES)).toBe("oc_x")
  })

  test("非飞书类型 → null（治理不越界到别的渠道）", () => {
    expect(feishuChatIdOf("jira_main:PROJ", TYPES)).toBeNull()
    expect(feishuChatIdOf("local:memory", TYPES)).toBeNull()
  })
})

describe("personaChatWhitelist：send.chats 必须计入", () => {
  test("listen/send 分开写 → 两个方向的群都在名单里", () => {
    const w = personaChatWhitelist("xiaoshuang", ENV)
    console.log(`    实测: ${[...(w ?? [])].join(",")}`)
    expect(w).not.toBeNull()
    expect(w!.has("oc_in")).toBe(true)
    expect(w!.has("oc_out")).toBe(true)
  })

  test("只写 send.chats → 白名单非空（不是“未配置 = 不限群”）", () => {
    const w = personaChatWhitelist("xiaoshuang", {
      GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([{ name: "support", send: { chats: ["oc_only_send"] } }]),
    })
    console.log(`    实测: ${JSON.stringify([...(w ?? [])])}`)
    expect(w).toEqual(new Set(["oc_only_send"]))
  })

  test("只写 listen.chats → 也计入（与入站同源）", () => {
    const w = personaChatWhitelist("xiaoshuang", {
      GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([{ name: "support", listen: { chats: ["oc_only_listen"] } }]),
    })
    expect(w).toEqual(new Set(["oc_only_listen"]))
  })

  test("完全没写 chats → null（未配置 = 不限群，保持既有语义）", () => {
    const w = personaChatWhitelist("xiaoshuang", {
      GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([{ name: "support", sessionMap: "per_thread" }]),
    })
    expect(w).toBeNull()
  })
})

describe("personOutboundDecision：实例名不再让治理失效", () => {
  test("越界群 + 实例名 feishu_out → 拒绝（旧实现会放行）", async () => {
    const d = await personOutboundDecision(
      ctxWith(registryOf(TYPES)) as never,
      "feishu_out:oc_evil",
      ENV,
    )
    console.log(`    实测: ${JSON.stringify(d)}`)
    expect(d.allowed).toBe(false)
    expect(d.reason).toContain("oc_evil")
  })

  test("名单内群 + 实例名 feishu_out → 放行（并标记 restricted）", async () => {
    const d = await personOutboundDecision(ctxWith(registryOf(TYPES)) as never, "feishu_out:oc_out", ENV)
    expect(d.allowed).toBe(true)
    expect(d.restricted).toBe(true)
  })

  test("同一配置换回实例名 feishu → 结论一致（两种命名等价）", async () => {
    const withType = await personOutboundDecision(ctxWith(registryOf(TYPES)) as never, "feishu_out:oc_evil", ENV)
    const withLegacy = await personOutboundDecision(ctxWith(registryOf(TYPES)) as never, "feishu:oc_evil", ENV)
    expect(withType.allowed).toBe(withLegacy.allowed)
  })

  test("非飞书类型不受人格群约束（jira / local 走各自白名单）", async () => {
    const d1 = await personOutboundDecision(ctxWith(registryOf(TYPES)) as never, "jira_main:ANY_PROJ", ENV)
    const d2 = await personOutboundDecision(ctxWith(registryOf(TYPES)) as never, "local:memory", ENV)
    expect(d1.allowed).toBe(true)
    expect(d2.allowed).toBe(true)
  })

  test("未装配注册表 → 按前缀名兜底（老部署/测试桩行为不变）", async () => {
    const ok = await personOutboundDecision(ctxWith() as never, "feishu:oc_out", ENV)
    const blocked = await personOutboundDecision(ctxWith() as never, "feishu:oc_evil", ENV)
    expect(ok.allowed).toBe(true)
    expect(blocked.allowed).toBe(false)
  })
})
