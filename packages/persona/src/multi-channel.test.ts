/**
 * 「一个驱动可以监听多个渠道」—— 与「一个渠道可被多个驱动监听」是**两个方向**，
 * 都必须成立，否则入站路由的总表达能力就受限。
 *
 * 允许的形态（驱动侧 `subscriptions` 是数组，按「任一命中即关注」）：
 * - `["feishu_main:message", "feishu_ops:message"]`  两个实例都听
 * - `["message"]`                                    任何渠道的消息都听
 * - `["feishu_main:oc_a:message", "feishu_ops:oc_b:message"]`  精确到群
 *
 * 这条一旦退化（例如某处把订阅当成「只能有一个 channel」），表现是
 * 「配了第二个渠道它就不响应了」——不报错，只是那一半事件永远没人接。
 */
import { describe, expect, test } from "bun:test"
import { PersonaCatalog } from "./catalog"
import type { PersonaDef } from "./persona"
import type { PersonaEvent } from "./event"

const mk = (subscriptions: string[]): PersonaDef => ({
  id: "xiaoshuang",
  name: "小霜",
  identity: "你是小霜",
  role: "运维",
  drivers: [{ id: "support", focus: "答疑", subscriptions }],
})

const ev = (channel: string, chatId = "oc_x", kind: "message" | "schedule" = "message") =>
  ({ kind, channel, context: { summary: "", body: "", refs: { chatId } } }) as unknown as Pick<
    PersonaEvent,
    "kind" | "channel" | "context"
  >

const hitsOf = (subs: string[], e: Pick<PersonaEvent, "kind" | "channel" | "context">): string[] => {
  const cat = new PersonaCatalog({ personas: new Map([["xiaoshuang", mk(subs)]]) })
  return cat.subscribers(e)[0]?.driverIds ?? []
}

describe("一个驱动监听多个渠道", () => {
  test("订阅两个渠道实例 → 两个实例的事件都命中", () => {
    const subs = ["feishu_main:message", "feishu_ops:message"]
    console.log(`    实测: main=${hitsOf(subs, ev("feishu_main"))} ops=${hitsOf(subs, ev("feishu_ops"))}`)
    expect(hitsOf(subs, ev("feishu_main"))).toEqual(["support"])
    expect(hitsOf(subs, ev("feishu_ops"))).toEqual(["support"])
  })

  test("只订阅一个实例 → 另一个实例的事件不命中（不能误收）", () => {
    const subs = ["feishu_main:message"]
    expect(hitsOf(subs, ev("feishu_main"))).toEqual(["support"])
    expect(hitsOf(subs, ev("feishu_ops"))).toEqual([])
  })

  test("广播式订阅（message）→ 任意渠道的消息都命中", () => {
    const subs = ["message"]
    for (const ch of ["feishu_main", "feishu_ops", "local"]) {
      expect(hitsOf(subs, ev(ch))).toEqual(["support"])
    }
    // 但不该吃非消息类事件
    expect(hitsOf(subs, ev("schedule", "", "schedule"))).toEqual([])
  })

  test("精确到群：两个实例各自指定不同群，互不串", () => {
    const subs = ["feishu_main:oc_a:message", "feishu_ops:oc_b:message"]
    expect(hitsOf(subs, ev("feishu_main", "oc_a"))).toEqual(["support"])
    expect(hitsOf(subs, ev("feishu_ops", "oc_b"))).toEqual(["support"])
    // 交叉：哪个都不该命中
    expect(hitsOf(subs, ev("feishu_main", "oc_b"))).toEqual([])
    expect(hitsOf(subs, ev("feishu_ops", "oc_a"))).toEqual([])
  })

  test("跨渠道消息（不同 chatId 同 id 形态）各自独立命中", () => {
    const subs = ["feishu_main:oc_same:message", "feishu_ops:oc_same:message"]
    expect(hitsOf(subs, ev("feishu_main", "oc_same"))).toEqual(["support"])
    expect(hitsOf(subs, ev("feishu_ops", "oc_same"))).toEqual(["support"])
  })
})

describe("两个方向同时成立：多渠道 × 多驱动", () => {
  test("2 个渠道 × 2 个驱动，每个 (渠道,事件) 命中该渠道下的全部驱动", () => {
    const def: PersonaDef = {
      id: "xiaoshuang",
      name: "小霜",
      identity: "你是小霜",
      role: "运维",
      drivers: [
        { id: "support", focus: "答疑", subscriptions: ["feishu_main:message", "feishu_ops:message"] },
        { id: "alerts", focus: "盯告警", subscriptions: ["feishu_ops:message"] },
      ],
    }
    const cat = new PersonaCatalog({ personas: new Map([[def.id, def]]) })
    const hit = (ch: string) => cat.subscribers(ev(ch))[0]?.driverIds.sort() ?? []
    console.log(`    实测: main=${hit("feishu_main")} ops=${hit("feishu_ops")}`)
    // main 只有 support 订；ops 两个都订
    expect(hit("feishu_main")).toEqual(["support"])
    expect(hit("feishu_ops")).toEqual(["alerts", "support"])
  })
})
