import { describe, expect, test } from "bun:test"
import { mkdtempSync, readdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { FeedbackInfo } from "@gebai/sdk"
import { feedbackContext, readFeedback, writeFeedback } from "./feedback"

/** 最小会话存储替身：load 按预设表返回（可抛错模拟读取异常）。 */
function fakeStore(sessions: Record<string, unknown> = {}, opts: { fail?: boolean } = {}) {
  return {
    async load(sessionId: string) {
      if (opts.fail) throw new Error("boom")
      return sessions[sessionId] ?? null
    },
  }
}

describe("feedback 上下文关联（feedbackContext：按会话记录补 model/subAgent，尽力关联不阻断）", () => {
  const fb = { sessionId: "s1", messageId: "m1" }

  test("消息携带 model + 会话装载子Agent → 两者均关联", async () => {
    const store = fakeStore({ s1: { messages: [{ id: "m1", role: "assistant", model: "glm-5" }], loadedSubAgents: ["self_optimize", "code"] } })
    expect(await feedbackContext(store as never, "default", fb)).toEqual({ model: "glm-5", subAgent: "self_optimize,code" })
  })

  test("消息无 model / 无装载名单 → 空对象（不带 undefined 字段）", async () => {
    const store = fakeStore({ s1: { messages: [{ id: "m1", role: "assistant" }] } })
    expect(await feedbackContext(store as never, "default", fb)).toEqual({})
  })

  test("会话不存在 / 消息未找到 / 读取异常 → 空对象（反馈仍可写入）", async () => {
    expect(await feedbackContext(fakeStore() as never, "default", fb)).toEqual({})
    const store = fakeStore({ s1: { messages: [{ id: "other", role: "assistant", model: "x" }] } })
    expect(await feedbackContext(store as never, "default", fb)).toEqual({})
    expect(await feedbackContext(fakeStore({}, { fail: true }) as never, "default", fb)).toEqual({})
  })
})

describe("feedback 写入与读取（writeFeedback/readFeedback：ctx 关联合并落盘）", () => {
  test("writeFeedback 合并 ctx（model/subAgent）并落盘分片目录，readFeedback 读回", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-feedback-"))
    try {
      const id = await writeFeedback(home, "alice", { sessionId: "s1", messageId: "m1", type: "thumbs_down", label: "错误", text: "路径算错了" }, { model: "glm-5", subAgent: "code" })
      const list = await readFeedback(home, "alice")
      expect(list).toHaveLength(1)
      expect(list[0]).toMatchObject({ id, userId: "alice", type: "thumbs_down", label: "错误", text: "路径算错了", model: "glm-5", subAgent: "code" } satisfies Partial<FeedbackInfo>)
      // 无 ctx：不带关联字段（不会写入 undefined 键）
      const id2 = await writeFeedback(home, "alice", { sessionId: "s1", messageId: "m2", type: "thumbs_up" })
      const list2 = (await readFeedback(home, "alice")).find((f) => f.id === id2)!
      expect(list2.model).toBeUndefined()
      expect(list2.subAgent).toBeUndefined()
      // 分片目录形态：users/{user}/feedback/日期/{2位}/{2位}/*.json（两条反馈随机分片，递归计数）
      const day = new Date().toISOString().slice(0, 10)
      const countJson = (dir: string): string[] => readdirSync(dir, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? countJson(join(dir, e.name)) : e.name.endsWith(".json") ? [join(dir, e.name)] : []))
      const files = countJson(join(home, "users", "alice", "feedback", day))
      expect(files).toHaveLength(2)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
