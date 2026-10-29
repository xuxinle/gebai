/**
 * 三段式订阅（channel:chatId:kind）用例：人格按群收窄的匹配语义。
 */
import { describe, expect, test } from "bun:test"
import { coordsOf, matchesAny, matchesSubscription, validateSubscription } from "./subscription"
import type { PersonaEvent } from "./event"

function ev(chatId: string | undefined): PersonaEvent {
  return {
    id: "m1",
    kind: "message",
    personaId: "",
    channel: "feishu",
    context: { source: "群", summary: "s", refs: chatId ? { chatId, messageId: "x" } : {} },
    body: "b",
    receivedAt: Date.now(),
  }
}

describe("三段式订阅（channel:chatId:kind）", () => {
  test("命中：feishu:oc_abc:message 匹配同群消息", () => {
    expect(matchesSubscription("feishu:oc_abc:message", coordsOf(ev("oc_abc")))).toBe(true)
  })

  test("不命中：群 id 不符", () => {
    expect(matchesSubscription("feishu:oc_abc:message", coordsOf(ev("oc_xyz")))).toBe(false)
  })

  test("不命中：事件无 chatId", () => {
    expect(matchesSubscription("feishu:oc_abc:message", coordsOf(ev(undefined)))).toBe(false)
  })

  test("段序无关：message:oc_abc:feishu 同样命中", () => {
    expect(matchesSubscription("message:oc_abc:feishu", coordsOf(ev("oc_abc")))).toBe(true)
    expect(matchesSubscription("oc_abc:feishu:message", coordsOf(ev("oc_abc")))).toBe(true)
    expect(matchesSubscription("feishu:message:oc_abc", coordsOf(ev("oc_abc")))).toBe(true)
  })

  test("三段式不吃普通两段订阅的群（顺序无关仍保持）", () => {
    // 两段式不受 chatId 影响
    expect(matchesSubscription("feishu:message", coordsOf(ev("oc_abc")))).toBe(true)
    expect(matchesSubscription("feishu:message", coordsOf(ev(undefined)))).toBe(true)
    expect(matchesSubscription("message:feishu", coordsOf(ev("oc_9")))).toBe(true)
  })

  test("matchesAny：一条群订阅、其余无关订阅混用", () => {
    const subs = ["schedule", "feishu:oc_abc:message"]
    expect(matchesAny(subs, coordsOf(ev("oc_abc")))).toBe(true)
    expect(matchesAny(subs, coordsOf(ev("oc_other")))).toBe(false)
  })

  test("校验：三段合法、四段拒绝", () => {
    expect(validateSubscription("feishu:oc_abc:message")).toBeNull()
    expect(validateSubscription("message:oc_abc:feishu")).toBeNull()
    expect(validateSubscription("a:b:c:d")).toContain("段数过多")
  })

  test("* 与 message 不受群过滤影响（set CHATS 换掉的就是这两类）", () => {
    expect(matchesSubscription("*", coordsOf(ev("oc_abc")))).toBe(true)
    expect(matchesSubscription("message", coordsOf(ev("oc_abc")))).toBe(true)
    expect(matchesSubscription("feishu", coordsOf(ev("oc_abc")))).toBe(true)
  })
})