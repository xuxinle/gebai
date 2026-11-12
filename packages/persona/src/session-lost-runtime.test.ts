/**
 * 运行时层的会话丢失自愈：`sessionCache`（TTL 30 分钟）里的死 id 必须能被救回来。
 *
 * 为什么单测这一层：调度器的换锚调用的是运行时提供的 `resolveSession`，而真正
 * 「记住死 id」的是运行时的 `sessionCache`。若那里不清，换锚会拿回同一个死 id，
 * 自愈退化成「换到同一个 id → 再抡一次」。
 *
 * 本文件钉住三件事：
 * 1. 引擎报「会话不存在」→ 运行时重解一次（清缓存），后续事件直接用新 id
 * 2. 重解出同一个 id（解析器没校验存活）→ 不当新会话用、不死循环
 * 3. 解析失败 → 视为换不到锚（交回调度器按失败处理，不静默吞）
 */
import { describe, expect, test } from "bun:test"
import { PersonaRuntime } from "./runtime"
import { SessionLostError } from "./session-lost"
import type { PersonaEvent } from "./event"
import type { PersonaIdentity, PersonaLookup, PersonaRunner, PersonaRunInput, SessionResolver } from "./ports"

const IDENT: PersonaIdentity = { id: "xiaoshuang", name: "小霜", identity: "你是小霜", role: "应答支持群" }

function ev(id: string): PersonaEvent {
  return { id, kind: "message", personaId: "", channel: "feishu", context: { source: "飞书群", summary: "有人提问", refs: { chatId: "oc_1", messageId: id } }, body: "帮我看看", receivedAt: Date.now() }
}

interface HarnessOpts {
  /** 引擎侧报丢失时抛什么（缺省中文文案，与真实引擎一致）。 */
  engineError?: (sessionId: string) => unknown
  /** 解析器行为：返回什么 id（缺省每次给新的）。 */
  resolveId?: (call: number) => string
  /** 解析器抛错。 */
  resolveThrows?: boolean
}

function makeHarness(opts: HarnessOpts = {}) {
  /** 每次 run 收到的 (sessionId, 事件 id)。 */
  const runs: Array<{ sessionId: string; ids: string }> = []
  const resolveCalls: string[] = []
  /** 引擎里「已不存在」的会话（测试开始时就是死的：模拟缓存里抱着的历史死 id）。 */
  const dead = new Set<string>(["sess-1"])
  let seq = 0

  const lookup: PersonaLookup = {
    get: (id) => (id === "xiaoshuang" ? IDENT : null),
    subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["support"] }],
    drivers: () => [{ id: "support", focus: "支持", subscriptions: ["*"] }],
    maxConcurrent: () => 1,
  }
  const runner: PersonaRunner = {
    isRunning: () => false,
    run: async (input: PersonaRunInput) => {
      runs.push({ sessionId: input.sessionId, ids: input.events.map((e) => e.id).join(",") })
      if (dead.has(input.sessionId)) {
        throw opts.engineError ? opts.engineError(input.sessionId) : new Error(`会话不存在: ${input.sessionId}`)
      }
    },
  }
  const sessions: SessionResolver = {
    resolveDriver: async () => {
      resolveCalls.push("call")
      if (opts.resolveThrows) throw new Error("磁盘不可读")
      const id = opts.resolveId ? opts.resolveId(resolveCalls.length) : `sess-${++seq}`
      return { sessionId: id, created: true, name: "小霜·support" }
    },
  }
  const runtime = new PersonaRuntime({ lookup, runner, sessions })
  const settle = async () => {
    // 调度器是 async IIFE 驱动、且路径里有多次 await（解析→入队→launch→execute 重跑），
    // 纯 microtask 排空不够，给几个宏任务回合。
    for (let i = 0; i < 5; i++) await new Promise<void>((r) => setTimeout(r, 5))
  }
  return { runtime, runs, resolveCalls, settle }
}

describe("PersonaRuntime：会话丢失自愈（换锚）", () => {
  test("引擎报会话不存在 → 重解出新会话并重跑同一批事件，后续事件直接用新 id", async () => {
    const h = makeHarness({ resolveId: (n) => `sess-${n}` })
    h.runtime.start()
    await h.runtime.dispatch(ev("e1"))
    await h.settle()

    // 第一次在 sess-1 上死（引擎报不存在）→ 重解出 sess-2 并重跑
    expect(h.runs.map((r) => r.sessionId)).toEqual(["sess-1", "sess-2"])
    expect(h.runs[1]!.ids).toBe("e1")
    expect(h.resolveCalls).toHaveLength(2)

    // 第二条事件：缓存已是新 id（不再重解、不再打死 id）
    await h.runtime.dispatch(ev("e2"))
    await h.settle()
    expect(h.runs[2]!.sessionId).toBe("sess-2")
    expect(h.resolveCalls).toHaveLength(2)
  })

  test("重解出同一个 id（解析器没校验存活）→ 不把同一个 id 当新会话（不自愈成死循环）", async () => {
    const h = makeHarness({ resolveId: () => "always-same" })
    h.runtime.start()
    await h.runtime.dispatch(ev("e1"))
    await h.settle()
    // 换锚拿到同一个 id → 视为换不到锚；事件按失败处理（有界重试），
    // 关键：不会无限重跑（次数远小于任意大）
    expect(h.runs.length).toBeLessThanOrEqual(4)
    expect(h.runs.every((r) => r.sessionId === "always-same")).toBe(true)
  })

  test("重解失败（存储不可读）→ 视为换不到锚，事件回到重试路径（不静默吞）", async () => {
    const h = makeHarness({ resolveThrows: true })
    h.runtime.start()
    const r = await h.runtime.dispatch(ev("e1"))
    // 首次投递就解析失败：如实报 unavailable（源会重投），不是 accepted
    expect(r.status).toBe("unavailable")
    expect(JSON.stringify(r)).toContain("会话解析失败")
    expect(h.runs).toHaveLength(0)
  })

  test("SessionLostError 原样上抛（调度器据此走换锚，不当毒事件）", async () => {
    const h = makeHarness({ engineError: (sid) => new SessionLostError(sid) })
    h.runtime.start()
    await h.runtime.dispatch(ev("e1"))
    await h.settle()
    expect(h.runs.map((r) => r.sessionId)).toEqual(["sess-1", "sess-2"])
  })
})
