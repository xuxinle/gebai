import { describe, expect, test } from "bun:test"

/**
 * 话题群闭环：入站识别话题（root_id → refs.threadId）→ 会话按话题分流 → 出站带 root 回到话题。
 * 这里钉 channels 侧的字段贯通；会话分流在 persona 包 session-key.test.ts。
 */

// extractMessage 未导出；字段贯通的端到端断言在 feishu-input.test.ts（入站 refs）。
// 这里钉 channels 公共契约：send 的 root 参数（出站回话题的通路）。
const { channel } = await import("./channels/feishu")

describe("feishu 渠道：话题消息闭环契约", () => {
  test("send 声明 root 参数（出站回话题的通路必须存在）", async () => {
    const send = (channel.verbs as Record<string, { args?: Record<string, unknown> }>).send
    expect(send.args && "root" in send.args).toBe(true)
  })

  test("send 的 root 参数描述指向 refs.messageId（话题回源 = reply 触发消息，飞书自动落回原话题）", async () => {
    const send = (channel.verbs as Record<string, { args?: Record<string, { description?: string }> }>).send
    expect(send.args?.root?.description ?? "").toContain("messageId")
  })
})
