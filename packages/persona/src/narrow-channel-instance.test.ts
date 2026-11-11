/**
 * 收窄订阅的前缀必须是**渠道实例名**，不是类型名。
 *
 * 这是移除事件源后暴露出来的一个真 bug：`narrowSubscriptions` 曾经硬编码
 * `feishu:${chat}:message`。而渠道实例化之后，事件的 `channel` 字段是**实例名**
 * （`feishu_main`），于是收窄后的订阅一条也命不中——
 * 表现是「配了 chats 之后它彻底不说话了」，且零报错、计数全零。
 *
 * 这个文件同时钉住两件事：
 * 1. 一个渠道可以被**多个驱动**监听（driverIds 是数组）
 * 2. 收窄时按群**所属实例**分组生成前缀（一个分身监多个实例的群也能同时生效）
 */
import { describe, expect, test } from "bun:test"
import { applyPersonaEnv } from "./env-config"
import type { PersonaDef } from "./persona"

const DEF: PersonaDef = {
  id: "xiaoshuang",
  name: "小霜",
  identity: "你是小霜",
  role: "运维",
  drivers: [
    { id: "support", focus: "答疑", subscriptions: ["message"] },
    { id: "alerts", focus: "盯告警", subscriptions: ["message"] },
  ],
}

/** 两个渠道实例各自的群白名单。 */
const CHANNEL_CHATS = new Map<string, readonly string[]>([
  ["feishu_main", ["oc_main"]],
  ["feishu_ops", ["oc_ops"]],
])

const envOf = (items: unknown[]) => ({ GEBAI_PERSONA_XIAOSHUANG: JSON.stringify(items) })

describe("收窄前缀 = 实例名（不是类型名）", () => {
  test("chats 里的群 → 订阅前缀是该群所属实例名", () => {
    const { personas } = applyPersonaEnv(
      new Map([[DEF.id, DEF]]),
      envOf([{ chats: ["oc_main"], channels: ["feishu_main"] }]),
      CHANNEL_CHATS,
    )
    const subs = personas.get("xiaoshuang")!.drivers![0]!.subscriptions
    console.log(`    实测: ${JSON.stringify(subs)}`)
    expect(subs).toEqual(["feishu_main:oc_main:message"])
    // 关键回归点：绝不能出现类型名前缀
    expect(subs.some((s) => s.startsWith("feishu:oc_"))).toBe(false)
  })

  test("一个分身监两个实例的群 → 各自用自己的前缀", () => {
    const { personas } = applyPersonaEnv(
      new Map([[DEF.id, DEF]]),
      envOf([{ chats: ["oc_main", "oc_ops"], channels: ["feishu_main", "feishu_ops"] }]),
      CHANNEL_CHATS,
    )
    const subs = personas.get("xiaoshuang")!.drivers![0]!.subscriptions
    console.log(`    实测: ${JSON.stringify(subs)}`)
    expect(subs.sort()).toEqual(["feishu_main:oc_main:message", "feishu_ops:oc_ops:message"])
  })

  test("没写 channels 时按实例白名单反查前缀（合法形态，不得回落成类型名）", () => {
    const { personas } = applyPersonaEnv(
      new Map([[DEF.id, DEF]]),
      envOf([{ chats: ["oc_ops"] }]),
      CHANNEL_CHATS,
    )
    expect(personas.get("xiaoshuang")!.drivers![0]!.subscriptions).toEqual(["feishu_ops:oc_ops:message"])
  })

  test("收窄对**所有**订阅了 message 的驱动同时生效", () => {
    const { personas } = applyPersonaEnv(
      new Map([[DEF.id, DEF]]),
      envOf([{ chats: ["oc_main"], channels: ["feishu_main"] }]),
      CHANNEL_CHATS,
    )
    const ds = personas.get("xiaoshuang")!.drivers!
    console.log(`    实测: support=${JSON.stringify(ds[0]!.subscriptions)} alerts=${JSON.stringify(ds[1]!.subscriptions)}`)
    // 两个分身都订阅 message，都该被收窄（不是只改第一个）
    expect(ds[0]!.subscriptions).toEqual(["feishu_main:oc_main:message"])
    expect(ds[1]!.subscriptions).toEqual(["feishu_main:oc_main:message"])
  })
})
