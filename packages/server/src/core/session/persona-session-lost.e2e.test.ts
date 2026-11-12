/**
 * 端到端：会话被删后，人格事件必须能自动恢复（真实 store / resolver / runtime / runner）。
 *
 * 这条链路有四层各持一份 sessionId，任何一层不清都会让自愈空转：
 *
 * ```
 * PersonaRuntime.sessionCache（TTL 30min）→ PersonaSessionResolver.memo（每次校验存活）
 *        ↓                                          ↓
 *   PersonaScheduler（按 sessionId 建桶）  →  EnginePersonaRunner（→ engine.run）
 * ```
 *
 * 本用例复刻的正是线上那条现象：**分身常驻会话被删（或 GC 归档 / chat.json 丢失）后，
 * 事件持续投给死 id，重试三次后丢弃**。判据不是「日志里有 warn」，而是
 * **事件最终在新会话上跑完**——这才是用户看到的「它还活着」。
 */
import { describe, expect, test } from "bun:test"
import { mkdtemp, rm } from "node:fs/promises"
import { existsSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { PersonaRuntime } from "@gebai/persona"
import type { PersonaEvent, PersonaIdentity, PersonaLookup } from "@gebai/persona"
import { SessionStore } from "./store"
import { PersonaSessionResolver } from "./persona-sessions"
import { EnginePersonaRunner } from "./persona-runner"

const IDENT: PersonaIdentity = { id: "xiaoshuang", name: "小霜", identity: "你是小霜", role: "应答支持群" }

function ev(id: string): PersonaEvent {
  return {
    id,
    kind: "message",
    personaId: "",
    channel: "feishu",
    context: { source: "飞书群「支持群」", summary: "有人提问", refs: { chatId: "oc_support", messageId: id } },
    body: "任务为什么一直 pending",
    receivedAt: Date.now(),
  }
}

async function makeWorld() {
  const home = await mkdtemp(join(tmpdir(), "gebai-persona-lost-"))
  const store = new SessionStore({ home })
  const resolver = new PersonaSessionResolver({ store })
  resolver.nameOf = () => "小霜"

  /** 每次引擎轮次的 (sessionId, 事件 id)。 */
  const rounds: Array<{ sessionId: string; ids: string }> = []

  /**
   * 伪引擎：与 `AgentEngine.run` 的准备阶段同口径——会话不存在就直接抛
   * `会话不存在: <id>`（engine.ts 的 `if (!session) throw ...`）。
   * 不模拟完整 LLM 循环：本用例要证的是「会话解析与自愈」，不是模型行为。
   */
  const engine = {
    isRunning: () => false,
    cancel: () => {},
    run: async (sessionId: string, user: string, _prompt: string, _opts?: unknown) => {
      rounds.push({ sessionId, ids: "?" })
      const session = await store.load(sessionId, user)
      if (!session) throw new Error(`会话不存在: ${sessionId}`)
      await store.appendMessage(sessionId, { id: crypto.randomUUID(), role: "user", content: "（本轮输入）", createdAt: Date.now() }, user)
    },
  }
  const runner = new EnginePersonaRunner({ engine })

  const lookup: PersonaLookup = {
    get: (id) => (id === "xiaoshuang" ? IDENT : null),
    subscribers: () => [{ personaId: "xiaoshuang", user: "admin", driverIds: ["support"] }],
    drivers: () => [{ id: "support", focus: "答支持群的问题", subscriptions: ["*"] }],
    maxConcurrent: () => 1,
  }
  const runtime = new PersonaRuntime({ lookup, runner, sessions: resolver, log: { info: () => {}, warn: () => {} } })
  runtime.start()

  const settle = async () => {
    for (let i = 0; i < 8; i++) await new Promise<void>((r) => setTimeout(r, 8))
  }
  return { home, store, resolver, runtime, rounds, settle }
}

describe("人格会话丢失后的自动恢复（端到端，真实 store）", () => {
  test("会话被删 → 换锚重建并跑完事件（不是重试三次后丢弃）", async () => {
    const w = await makeWorld()
    try {
      // 第一条事件：正常建立常驻会话并跑完
      await w.runtime.dispatch(ev("e1"))
      await w.settle()
      const first = w.rounds.at(-1)!.sessionId
      expect(await w.store.load(first, "admin")).toBeTruthy()

      // 用户删掉了这个常驻会话（真实入口：面板删除 / GC 归档）
      await w.store.delete(first, "admin")
      expect(await w.store.load(first, "admin")).toBeNull()

      // 第二条事件：运行时的解析缓存（TTL 内）还记着死 id —— 必须自愈
      await w.runtime.dispatch(ev("e2"))
      await w.settle()

      const second = w.rounds.at(-1)!.sessionId
      expect(second).not.toBe(first)
      // 新会话真的落盘了，事件跑在它上面
      expect(await w.store.load(second, "admin")).toBeTruthy()
      // 死会话没有被「复活」成目录
      expect(existsSync(join(w.home, "users", "admin", "sessions", first.slice(0, 2), first.slice(2, 4), first))).toBe(false)

      // 第三条事件：缓存已指向新会话，不再触发换锚
      await w.runtime.dispatch(ev("e3"))
      await w.settle()
      expect(w.rounds.at(-1)!.sessionId).toBe(second)
    } finally {
      await rm(w.home, { recursive: true, force: true })
    }
  })

  test("GC 归档（目录整体搬走）同样恢复，且不把已归档会话重建回来", async () => {
    const w = await makeWorld()
    try {
      await w.runtime.dispatch(ev("e1"))
      await w.settle()
      const archived = w.rounds.at(-1)!.sessionId

      // 模拟 GC 归档：目录整体搬到 trash/（真实语义）+ 置移除标记（save 拒绝复活）
      const dir = join(w.home, "users", "admin", "sessions", archived.slice(0, 2), archived.slice(2, 4), archived)
      await rm(dir, { recursive: true, force: true })
      w.store.markRemoved(archived, "archived")
      expect(await w.store.load(archived, "admin")).toBeNull()

      await w.runtime.dispatch(ev("e2"))
      await w.settle()
      const fresh = w.rounds.at(-1)!.sessionId
      expect(fresh).not.toBe(archived)
      expect(await w.store.load(fresh, "admin")).toBeTruthy()
    } finally {
      await rm(w.home, { recursive: true, force: true })
    }
  })

  test("绑定被删后重解出的是**新会话**而不是同一个死 id（否则自愈空转）", async () => {
    const w = await makeWorld()
    try {
      const a = await w.resolver.resolveDriver({ personaId: "xiaoshuang", driverId: "support", user: "admin", event: ev("e1") })
      await w.store.delete(a.sessionId, "admin")
      const b = await w.resolver.resolveDriver({ personaId: "xiaoshuang", driverId: "support", user: "admin", event: ev("e2") })
      expect(b.sessionId).not.toBe(a.sessionId)
      expect(b.created).toBe(true)
    } finally {
      await rm(w.home, { recursive: true, force: true })
    }
  })
})
