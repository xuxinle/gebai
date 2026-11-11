/**
 * 「一个渠道可以被多个驱动监听」—— 这是入站链路的核心性质，删掉事件源后必须仍然成立。
 *
 * 链路：渠道事件 → `PersonaCatalog.subscribers()` 找出命中的 (人格, 驱动) →
 * `PersonaRuntime` 为**每个驱动各投递一次**（幂等键是 `事件id + 驱动key`，
 * 所以同一驱动不会重复收，不同驱动各收一次）。
 *
 * 为什么必须单测钉住：这条性质一旦退化成“一个事件只给第一个命中的驱动”，
 * 表现是「某个分身永远不干活」——而它不报错、计数也正常，只是从不响应。
 */
import { describe, expect, test } from "bun:test"
import { PersonaCatalog } from "./catalog"
import type { PersonaDef } from "./persona"
import type { PersonaEvent } from "./event"

/** 两个驱动都订阅同一个渠道（feishu），第三个驱动只订 schedule。 */
const DEF: PersonaDef = {
  id: "xiaoshuang",
  name: "小霜",
  identity: "你是小霜",
  role: "运维",
  drivers: [
    { id: "support", focus: "答疑", subscriptions: ["message"] },
    { id: "alerts", focus: "盯告警", subscriptions: ["message"] },
    { id: "daily", focus: "写日报", subscriptions: ["schedule"] },
  ],
}

const EVENT = {
  kind: "message" as const,
  channel: "feishu_main",
  context: { summary: "群里有人提问", body: "为什么一直 pending", refs: { chatId: "oc_x" } },
} as unknown as Pick<PersonaEvent, "kind" | "channel" | "context">

describe("一个渠道 → 多个驱动", () => {
  test("同一渠道事件命中多个驱动（driverIds 是数组，不是单个）", () => {
    const cat = new PersonaCatalog({ personas: new Map([[DEF.id, DEF]]) })
    const subs = cat.subscribers(EVENT)
    console.log(`    实测: ${JSON.stringify(subs)}`)
    expect(subs).toHaveLength(1)
    // 两个订阅 message 的驱动都应命中；订阅 schedule 的不该被误算
    expect(subs[0]!.driverIds.sort()).toEqual(["alerts", "support"])
  })

  test("订阅面不同的驱动互不干扰（schedule 驱动不吃渠道消息）", () => {
    const cat = new PersonaCatalog({ personas: new Map([[DEF.id, DEF]]) })
    expect(cat.subscribers(EVENT)[0]!.driverIds).not.toContain("daily")
    const sched = { kind: "schedule" as const, channel: "schedule", context: { summary: "到点", body: "写日报" } } as unknown as Pick<
      PersonaEvent,
      "kind" | "channel" | "context"
    >
    expect(cat.subscribers(sched)[0]!.driverIds).toEqual(["daily"])
  })

  test("渠道收窄（chats）对**该人格的所有驱动**同时生效", () => {
    // 模拟 applyPersonaEnv 的收窄结果：逐群订阅的前缀是**实例名**（feishu_main），
    // 不是类型名（feishu）——那个前缀就是事件的 channel 字段。
    const narrowed: PersonaDef = {
      ...DEF,
      drivers: (DEF.drivers ?? []).map((d) =>
        d.subscriptions.includes("message") ? { ...d, subscriptions: ["feishu_main:oc_a:message"] } : d,
      ),
    }
    const cat = new PersonaCatalog({ personas: new Map([[narrowed.id, narrowed]]) })
    // 命中白名单群
    expect(
      cat.subscribers({
        kind: "message",
        channel: "feishu_main",
        context: { summary: "", body: "", refs: { chatId: "oc_a" } },
      } as unknown as Pick<PersonaEvent, "kind" | "channel" | "context">)[0]!.driverIds.sort(),
    ).toEqual(["alerts", "support"])
    // 白名单外的群：两个驱动都不命中（收窄是人格级的，不该只收窄一个）
    expect(
      cat.subscribers({
        kind: "message",
        channel: "feishu_main",
        context: { summary: "", body: "", refs: { chatId: "oc_other" } },
      } as unknown as Pick<PersonaEvent, "kind" | "channel" | "context">),
    ).toEqual([])
  })

  test("不同渠道的事件互不串（feishu_main 与 feishu_ops 各算各的）", () => {
    const perChannel: PersonaDef = {
      ...DEF,
      drivers: [
        { id: "support", focus: "支持群", subscriptions: ["feishu_main:message"] },
        { id: "ops", focus: "运维群", subscriptions: ["feishu_ops:message"] },
      ],
    }
    const cat = new PersonaCatalog({ personas: new Map([[perChannel.id, perChannel]]) })
    const hit = (channel: string) =>
      cat.subscribers({
        kind: "message",
        channel,
        context: { summary: "", body: "", refs: { chatId: "oc_x" } },
      } as unknown as Pick<PersonaEvent, "kind" | "channel" | "context">)[0]?.driverIds ?? []
    console.log(`    实测: main=${hit("feishu_main")} ops=${hit("feishu_ops")}`)
    expect(hit("feishu_main")).toEqual(["support"])
    expect(hit("feishu_ops")).toEqual(["ops"])
  })
})
