/**
 * 输入与输出解耦：`listen` / `send` 两个方向各自独立。
 *
 * 这是「渠道分输入输出两类」在人格侧的落点。要钉住的性质：
 *
 * 1. **收 A 群、发 B 群**：两个方向各写各的，互不影响
 * 2. **收 A 实例、发 B 实例**：真正的跨渠道解耦（用甲机器人收、乙机器人发）
 * 3. **方向写错要报错**：`listen` 引用出站实例 / `send` 引用入站实例
 * 4. **旧扁平写法仍生效**：`channels`/`chats` 同时作用于两个方向（不破坏已有部署）
 */
import { describe, expect, test } from "bun:test"
import { applyPersonaEnv } from "./env-config"
import type { PersonaDef } from "./persona"

const DEF: PersonaDef = {
  id: "xiaoshuang",
  name: "小霜",
  identity: "你是小霜",
  role: "运维",
  drivers: [{ id: "support", focus: "答疑", subscriptions: ["message"] }],
}

/** 实例 → 允许的群；另一个映射给方向。 */
const CHATS = new Map<string, readonly string[]>([
  ["feishu_in", ["oc_in", "oc_also_in"]],
  ["feishu_out", ["oc_out", "oc_also_out"]],
])
const DIRS = new Map<string, "in" | "out">([
  ["feishu_in", "in"],
  ["feishu_out", "out"],
])

const run = (items: unknown[], dirs: ReadonlyMap<string, "in" | "out"> = DIRS) =>
  applyPersonaEnv(
    new Map([[DEF.id, DEF]]),
    { GEBAI_PERSONA_ENABLE: "xiaoshuang", GEBAI_PERSONA_XIAOSHUANG: JSON.stringify(items) },
    CHATS,
    dirs,
  )

describe("listen / send 解耦", () => {
  test("收 A 群、发 B 群（同一实例内也能分离）", () => {
    const same = new Map([["feishu", ["oc_a", "oc_b"]]])
    const dirs = new Map<string, "in" | "out">([["feishu", "out"]]) // 不校验方向
    const { personas } = applyPersonaEnv(
      new Map([[DEF.id, DEF]]),
      {
        GEBAI_PERSONA_ENABLE: "xiaoshuang",
        GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([
          { listen: { channels: ["feishu"], chats: ["oc_a"] }, send: { channels: ["feishu"], chats: ["oc_b"] } },
        ]),
      },
      same,
      dirs,
    )
    const d = personas.get("xiaoshuang")!.drivers![0]!
    console.log(`    实测: 订阅=${JSON.stringify(d.subscriptions)} 输出=${JSON.stringify(d.outputs)}`)
    // 收的只有 oc_a
    expect(d.subscriptions).toEqual(["feishu:oc_a:message"])
    // 发的只有 oc_b —— 解耦的核心断言
    expect(d.outputs).toEqual([{ channel: "feishu", address: "oc_b", label: "feishu", default: true }])
  })

  test("收 A 实例、发 B 实例（跨渠道解耦）", () => {
    const { personas, report } = run([
      {
        listen: { channels: ["feishu_in"], chats: ["oc_in"] },
        send: { channels: ["feishu_out"], chats: ["oc_out"] },
      },
    ])
    const d = personas.get("xiaoshuang")!.drivers![0]!
    console.log(`    实测: 订阅=${JSON.stringify(d.subscriptions)} 输出=${JSON.stringify(d.outputs)}`)
    console.log(`    errors=${JSON.stringify(report.errors)}`)
    expect(d.subscriptions).toEqual(["feishu_in:oc_in:message"])
    expect(d.outputs).toEqual([{ channel: "feishu_out", address: "oc_out", label: "feishu_out", default: true }])
    expect(report.errors).toEqual([])
  })

  test("只写 listen → 只收这几个群，没有出站目标", () => {
    const { personas } = run([{ listen: { channels: ["feishu_in"], chats: ["oc_in"] } }])
    const d = personas.get("xiaoshuang")!.drivers![0]!
    expect(d.subscriptions).toEqual(["feishu_in:oc_in:message"])
    expect(d.outputs).toBeUndefined()
  })

  test("只写 send → 收不受限（按代码定义），只发这几个群", () => {
    const { personas } = run([{ send: { channels: ["feishu_out"], chats: ["oc_out"] } }])
    const d = personas.get("xiaoshuang")!.drivers![0]!
    console.log(`    实测: 订阅=${JSON.stringify(d.subscriptions)} 输出=${JSON.stringify(d.outputs)}`)
    // 订阅保持代码定义里的原样（message）——没有被无端收窄
    expect(d.subscriptions).toEqual(["message"])
    expect(d.outputs).toEqual([{ channel: "feishu_out", address: "oc_out", label: "feishu_out", default: true }])
  })

  test("方向写错：listen 引用出站实例 → 报错（它收不到）", () => {
    const { report } = run([{ listen: { channels: ["feishu_out"], chats: ["oc_out"] } }])
    console.log(`    实测: ${JSON.stringify(report.errors)}`)
    expect(report.errors.some((e) => e.includes("listen 引用了出站实例"))).toBe(true)
  })

  test("方向写错：send 引用入站实例 → 报错（它发不出）", () => {
    const { report } = run([{ send: { channels: ["feishu_in"], chats: ["oc_in"] } }])
    console.log(`    实测: ${JSON.stringify(report.errors)}`)
    expect(report.errors.some((e) => e.includes("send 引用了入站实例"))).toBe(true)
  })

  test("旧扁平写法仍生效：channels/chats 同时作用于两个方向", () => {
    const same = new Map([["feishu", ["oc_a"]]])
    const { personas, report } = applyPersonaEnv(
      new Map([[DEF.id, DEF]]),
      { GEBAI_PERSONA_ENABLE: "xiaoshuang", GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([{ channels: ["feishu"], chats: ["oc_a"] }]) },
      same,
      new Map(),
    )
    const d = personas.get("xiaoshuang")!.drivers![0]!
    console.log(`    实测: 订阅=${JSON.stringify(d.subscriptions)} 输出=${JSON.stringify(d.outputs)}`)
    expect(d.subscriptions).toEqual(["feishu:oc_a:message"])
    expect(d.outputs?.[0]?.address).toBe("oc_a")
    expect(report.errors).toEqual([])
  })

  test("listen/send 优先于旧扁平字段（混用时更具体的赢）", () => {
    const same = new Map([["feishu", ["oc_a", "oc_b"]]])
    const { personas } = applyPersonaEnv(
      new Map([[DEF.id, DEF]]),
      {
        GEBAI_PERSONA_ENABLE: "xiaoshuang",
        GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([
          { channels: ["feishu"], chats: ["oc_a"], send: { chats: ["oc_b"] } },
        ]),
      },
      same,
      new Map(),
    )
    const d = personas.get("xiaoshuang")!.drivers![0]!
    console.log(`    实测: 订阅=${JSON.stringify(d.subscriptions)} 输出=${JSON.stringify(d.outputs)}`)
    // listen 没写 → 回落到旧 chats（oc_a）；send 写了 → oc_b
    expect(d.subscriptions).toEqual(["feishu:oc_a:message"])
    expect(d.outputs?.[0]?.address).toBe("oc_b")
  })
})

describe("listen/send 的结构校验", () => {
  test("listen 不是对象 → 明确报错", () => {
    const { report } = run([{ listen: ["feishu_in"] }])
    expect(report.errors.some((e) => e.includes("listen 必须是对象"))).toBe(true)
  })

  test("listen 里的未知键 → 报错（拼错的键不能静默）", () => {
    const { report } = run([{ listen: { channel: ["feishu_in"] } }])
    console.log(`    实测: ${JSON.stringify(report.errors)}`)
    expect(report.errors.some((e) => e.includes("含未知键「channel」"))).toBe(true)
  })
})
