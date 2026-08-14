import { describe, expect, test, beforeEach } from "bun:test"
import { mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { UserJournal, ConnectionState } from "./ws-state"
import type { AgentEvent } from "@gebai/sdk"

function ev(type: string, sessionId: string, payload: Record<string, unknown> = {}): AgentEvent {
  return { type, sessionId, payload, timestamp: Date.now() }
}

describe("UserJournal", () => {
  let j: UserJournal
  beforeEach(() => {
    j = new UserJournal()
  })

  test("appends entries with increasing seq and injects sessionId into payload", () => {
    const e1 = j.append(ev("event.message.delta", "s1", { text: "a" }))
    const e2 = j.append(ev("event.tool.call", "s1", { name: "x" }))
    expect(e1.seq).toBe(1)
    expect(e2.seq).toBe(2)
    expect(e1.payload.sessionId).toBe("s1")
    expect(j.lastSeq()).toBe(2)
  })

  test("replay returns events after lastSeq", () => {
    j.append(ev("event.message.delta", "s1", { text: "a" }))
    j.append(ev("event.message.delta", "s1", { text: "b" }))
    j.append(ev("event.task.done", "s1"))
    const r = j.replay(1)
    expect(r).not.toBeNull()
    expect(r!.entries.map((e) => e.seq)).toEqual([2, 3])
    expect(r!.lastSeq).toBe(3)
    // 无遗漏：重放为空
    expect(j.replay(3)!.entries).toEqual([])
  })

  test("replay returns null when buffer overflowed (gap)", () => {
    const small = new UserJournal(3)
    small.append(ev("event.message.delta", "s1", { text: "1" }))
    small.append(ev("event.message.delta", "s1", { text: "2" }))
    small.append(ev("event.message.delta", "s1", { text: "3" }))
    small.append(ev("event.task.done", "s1")) // seq 1 被挤出
    expect(small.replay(0)).toBeNull() // 客户端只到 seq0：缺 seq1
    expect(small.replay(1)).not.toBeNull() // 从 seq2 起可完整重放
    expect(small.replay(1)!.entries.map((e) => e.seq)).toEqual([2, 3, 4])
  })

  test("fresh journal replay from 0 is empty", () => {
    const r = j.replay(0)
    expect(r).not.toBeNull()
    expect(r!.entries).toEqual([])
    expect(j.lastSeq()).toBe(0)
  })
})

describe("ConnectionState", () => {
  test("persists currentSessionId per user across instances", () => {
    const file = join(mkdtempSync(join(tmpdir(), "gebai-conn-")), "conn-state.json")
    try {
      const c1 = new ConnectionState(file)
      c1.setCurrent("u1", "sess-1")
      c1.setCurrent("u2", "sess-2")
      c1.flush()
      const c2 = new ConnectionState(file)
      expect(c2.getCurrent("u1")).toBe("sess-1")
      expect(c2.getCurrent("u2")).toBe("sess-2")
      expect(c2.getCurrent("u3")).toBeUndefined()
      // 清空（登出/会话删除）
      c2.setCurrent("u1", undefined)
      expect(c2.getCurrent("u1")).toBeUndefined()
      c2.flush()
      const c3 = new ConnectionState(file)
      expect(c3.getCurrent("u1")).toBeUndefined()
      expect(c3.getCurrent("u2")).toBe("sess-2")
    } finally {
      rmSync(join(file, ".."), { recursive: true, force: true })
    }
  })

  test("missing/corrupt file loads empty state", () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-conn-"))
    try {
      const missing = new ConnectionState(join(dir, "nope.json"))
      expect(missing.getCurrent("any")).toBeUndefined()
      const corruptPath = join(dir, "corrupt.json")
      writeFileSync(corruptPath, "{not-json")
      const corrupt = new ConnectionState(corruptPath)
      expect(corrupt.getCurrent("any")).toBeUndefined()
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })
})
