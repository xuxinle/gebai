/**
 * 事件构造与幂等键推导用例：幂等键错 = 重复副作用；线程键错 = 上下文断裂。
 */
import { describe, expect, test } from "bun:test"
import { buildEvent, eventIdFor, hash32, renderEventContext } from "./events"
import { deriveThreadKey } from "./event"

describe("eventIdFor：每源幂等键", () => {
  test("message 优先用渠道消息 id（不随正文变化）", () => {
    const a = eventIdFor({ kind: "message", personaId: "p", channel: "feishu", body: "hi", source: "s", summary: "m", refs: { messageId: "om_1" } })
    const b = eventIdFor({ kind: "message", personaId: "p", channel: "feishu", body: "hi 改了正文", source: "s", summary: "m", refs: { messageId: "om_1" } })
    expect(a).toBe("msg:om_1")
    expect(a).toBe(b) // 正文变化不影响幂等键（同一条消息的编辑不该重复处理）
  })

  test("message 无消息 id 时退回内容哈希（同内容同键，防重复）", () => {
    const mk = (body: string) => eventIdFor({ kind: "message", personaId: "p", channel: "feishu", body, source: "s", summary: "m", refs: { chatId: "oc_1", senderId: "ou_1" } })
    expect(mk("你好")).toBe(mk("你好"))
    expect(mk("你好")).not.toBe(mk("再见"))
  })

  test("schedule 用 taskId@触发时刻（同一时刻只触发一次）", () => {
    const id = eventIdFor({ kind: "schedule", personaId: "p", channel: "schedule", body: "巡检", source: "s", summary: "m", refs: { taskId: "t1" }, at: 1700000000000 })
    expect(id).toBe("task:t1@1700000000000")
    const sameAt = eventIdFor({ kind: "schedule", personaId: "p", channel: "schedule", body: "巡检", source: "s", summary: "m", refs: { taskId: "t1" }, at: 1700000000000 })
    expect(id).toBe(sameAt)
    const nextAt = eventIdFor({ kind: "schedule", personaId: "p", channel: "schedule", body: "巡检", source: "s", summary: "m", refs: { taskId: "t1" }, at: 1700000060000 })
    expect(nextAt).not.toBe(id)
  })

  test("webhook 优先用投递 id，缺失退回内容哈希", () => {
    const withId = eventIdFor({ kind: "webhook", personaId: "p", channel: "hook", body: "{}", source: "s", summary: "m", refs: { extra: { deliveryId: "d1" } } })
    expect(withId).toBe("hook:d1")
  })

  test("显式 id 覆盖推导", () => {
    const id = eventIdFor({ kind: "message", personaId: "p", channel: "feishu", body: "hi", source: "s", summary: "m", id: "explicit" })
    expect(id).toBe("explicit")
  })
})

describe("deriveThreadKey：路由键（决定上下文连续性）", () => {
  test("同一群的连续消息落同一线程（不含 messageId）", () => {
    const base = { channel: "feishu", kind: "message" as const, context: { source: "s", summary: "m", refs: { chatId: "oc_1" } } }
    expect(deriveThreadKey(base)).toBe("feishu:oc_1")
    // messageId 不同不影响线程键
    const withMsg = { ...base, context: { ...base.context, refs: { chatId: "oc_1", messageId: "om_9" } } }
    expect(deriveThreadKey(withMsg)).toBe("feishu:oc_1")
  })

  test("不同群落不同线程", () => {
    const a = deriveThreadKey({ channel: "feishu", kind: "message", context: { source: "s", summary: "m", refs: { chatId: "oc_a" } } })
    const b = deriveThreadKey({ channel: "feishu", kind: "message", context: { source: "s", summary: "m", refs: { chatId: "oc_b" } } })
    expect(a).not.toBe(b)
  })

  test("定时任务与自省进 anchor（长期记忆会话），不按时间分线程", () => {
    expect(deriveThreadKey({ channel: "schedule", kind: "schedule", context: { source: "s", summary: "m" } })).toBe("anchor")
    expect(deriveThreadKey({ channel: "idle", kind: "idle", context: { source: "s", summary: "m" } })).toBe("anchor")
  })

  test("显式 threadKey 优先（/new 后的新线程）", () => {
    const k = deriveThreadKey({ channel: "feishu", kind: "message", threadKey: "feishu:oc_1#2", context: { source: "s", summary: "m", refs: { chatId: "oc_1" } } })
    expect(k).toBe("feishu:oc_1#2")
  })

  test("无 chatId 的非定时源退回 channel", () => {
    expect(deriveThreadKey({ channel: "webhook", kind: "webhook", context: { source: "s", summary: "m" } })).toBe("webhook")
  })
})

describe("renderEventContext：出站纪律必须显式声明", () => {
  const e = buildEvent({
    kind: "message",
    personaId: "p",
    channel: "feishu",
    body: "帮我看看",
    source: "飞书群「支持群」",
    summary: "张三问任务为什么失败",
    refs: { chatId: "oc_1", messageId: "om_1", senderName: "张三", senderId: "ou_1" },
  })

  test("含来源/发起者/消息 id/摘要", () => {
    const text = renderEventContext(e)
    expect(text).toContain("飞书群「支持群」")
    expect(text).toContain("张三")
    expect(text).toContain("om_1")
    expect(text).toContain("oc_1")
    expect(text).toContain("张三问任务为什么失败")
  })

  test("明确声明回复不会自动发送（防「人格沉默」的首要防线）", () => {
    const text = renderEventContext(e)
    expect(text).toContain("不会被自动发送")
    expect(text).toContain("必须调用发消息工具")
  })

  test("无出站通道时如实说明", () => {
    const text = renderEventContext(e, { canReply: false })
    expect(text).toContain("无可用出站通道")
    expect(text).not.toContain("必须调用发消息工具")
  })
})

describe("hash32", () => {
  test("稳定且定长 8 位", () => {
    expect(hash32("abc")).toBe(hash32("abc"))
    expect(hash32("abc")).not.toBe(hash32("abd"))
    expect(hash32("abc")).toHaveLength(8)
  })
})
