/**
 * 会话丢失自愈：引擎报「会话不存在」时换锚重跑，而不是把事件当毒事件丢掉。
 *
 * 真实形态（按发生频率）：用户删掉了分身常驻会话 / GC 归档了它 / 会话目录被外部
 * 搬走（chat.json 丢了）。两层缓存（运行时的 `sessionCache` 与解析器的 `memo`）
 * 都还记着那个 id，于是**每一条**事件都在死 id 上死一遍，重试 3 次后整批丢弃——
 * 表面症状是「这个分身从此哑了」，日志里只有几条 warn。
 *
 * 本文件钉住五件容易做坏的事：
 * 1. 换锚后**同一批事件**真的在新会话上跑完（且不计失败）
 * 2. 待跑队列里的事件也改投新 id（否则下一轮还在死 id 上打）
 * 3. 换不到锚时回到旧语义（重试→有界放弃），不静默吞事件
 * 4. 「解出同一个 id」「重建了但仍报不存在」都不会无限自愈
 * 5. 没配 `resolveSession` 时行为与改动前一致
 */
import { describe, expect, test } from "bun:test"
import { PersonaScheduler } from "./scheduler"
import { SessionLostError } from "./session-lost"
import type { PersonaEvent } from "./event"

function ev(id: string): PersonaEvent {
  return { id, kind: "message", personaId: "p1", channel: "feishu", context: { source: "s", summary: "m" }, body: id, receivedAt: Date.now() }
}

interface HarnessOpts {
  maxConcurrent?: number
  /** 这些会话 id 上执行时报会话丢失。 */
  dead?: Set<string>
  /** 死会话 → 新会话（缺省 undefined = 换不到锚）。 */
  freshFor?: (dead: string) => string | null
  /** 配 false 时不提供 resolveSession（旧行为）。 */
  reanchor?: boolean
  /** 每次换锚都生成新 id（用来钉「重建了但仍死」的有界性）。 */
  alwaysFresh?: boolean
}

function makeHarness(opts: HarnessOpts = {}) {
  const calls: Array<{ sessionId: string; ids: string }> = []
  const resolveCalls: string[] = []
  const gates: Array<() => void> = []
  const dead = opts.dead ?? new Set<string>()
  let seq = 0

  const scheduler = new PersonaScheduler({
    personaId: "p1",
    maxConcurrent: opts.maxConcurrent ?? 1,
    execute: ({ sessionId, events }) => {
      calls.push({ sessionId, ids: events.map((e) => e.id).join(",") })
      if (dead.has(sessionId)) return Promise.reject(new SessionLostError(sessionId, { reason: "测试注入" }))
      return new Promise<void>((r) => gates.push(r))
    },
    ...(opts.reanchor === false
      ? {}
      : {
          resolveSession: async (deadId: string) => {
            resolveCalls.push(deadId)
            if (opts.alwaysFresh) {
              const fresh = `fresh-${++seq}`
              dead.add(fresh) // 新会话同样报丢失：模拟「重建了但仍然不存在」
              return fresh
            }
            return opts.freshFor ? opts.freshFor(deadId) : "fresh-session"
          },
        }),
  })
  const settle = async () => {
    for (let i = 0; i < 20; i++) await Promise.resolve()
  }
  const release = async () => {
    gates.shift()?.()
    await settle()
  }
  return { scheduler, calls, resolveCalls, settle, release }
}

describe("PersonaScheduler：会话丢失自愈", () => {
  test("引擎报会话不存在 → 换锚到新会话重跑同一批事件（不计失败、不丢事件）", async () => {
    const dead = new Set(["dead-1"])
    const h = makeHarness({ dead, freshFor: () => "fresh-1" })
    h.scheduler.enqueue({ event: ev("e1"), sessionId: "dead-1", user: "u", threadKey: "t" })
    await h.settle()

    // 第一次打在死会话上（报丢失），换锚后同一件事件健康跑一轮
    expect(h.calls.map((c) => c.sessionId)).toEqual(["dead-1", "fresh-1"])
    expect(h.calls[1]!.ids).toBe("e1")
    expect(h.resolveCalls).toEqual(["dead-1"])

    await h.release()
    const st = h.scheduler.state()
    expect(st.queued).toBe(0)
    expect(st.sessions).toEqual([])
  })

  test("待跑队列里的事件随换锚改投新会话（下一轮不再打旧 id）", async () => {
    const dead = new Set(["dead-q"])
    const h = makeHarness({ maxConcurrent: 1, dead, freshFor: () => "fresh-q" })
    // 屏障会话占满唯一额度 → dead-q 的两件只能在队列里等着
    h.scheduler.enqueue({ event: ev("blocker"), sessionId: "blocker", user: "u", threadKey: "t" })
    await h.settle()
    h.scheduler.enqueue({ event: ev("e1"), sessionId: "dead-q", user: "u", threadKey: "t" })
    h.scheduler.enqueue({ event: ev("e2"), sessionId: "dead-q", user: "u", threadKey: "t" })
    expect(h.scheduler.state().queued).toBe(2)

    await h.release() // blocker 结束 → dead-q 拿到额度开跑（首轮批量带走 e1,e2）
    // 死会话上失败一次，换锚后同一批在新会话上跑
    expect(h.calls.map((c) => c.sessionId)).toEqual(["blocker", "dead-q", "fresh-q"])
    expect(h.calls[2]!.ids).toBe("e1,e2")
    await h.release()
    expect(h.scheduler.state().queued).toBe(0)
  })

  test("换不到锚（宿主也认不出）→ 回到旧语义：有界重试后放弃，不静默吞也不死循环", async () => {
    const dead = new Set(["dead-x"])
    const h = makeHarness({ dead, freshFor: () => null })
    h.scheduler.enqueue({ event: ev("e1"), sessionId: "dead-x", user: "u", threadKey: "t" })
    await h.settle()
    // 每次尝试都做了换锚努力，但都没换成 → 按原重试上限（3 次）放弃
    expect(h.resolveCalls.length).toBeGreaterThan(0)
    expect(h.calls).toHaveLength(3)
    expect(h.calls.every((c) => c.sessionId === "dead-x")).toBe(true)
    expect(h.scheduler.state().queued).toBe(0)
  })

  test("解出同一个 id（解析器没校验存活）→ 视为换锚失败，不重跑同一轮", async () => {
    const dead = new Set(["dead-same"])
    const h = makeHarness({ dead, freshFor: (d) => d })
    h.scheduler.enqueue({ event: ev("e1"), sessionId: "dead-same", user: "u", threadKey: "t" })
    await h.settle()
    // 每轮只执行一次（没有「换锚到同一 id 再抡一次」），换锚尝试有界
    expect(h.resolveCalls.length).toBeLessThanOrEqual(3)
    expect(h.calls.every((c) => c.sessionId === "dead-same")).toBe(true)
  })

  test("「重建了但依旧报不存在」有换锚次数上限（不死循环）", async () => {
    const h = makeHarness({ alwaysFresh: true, dead: new Set(["dead-loop"]) })
    h.scheduler.enqueue({ event: ev("e1"), sessionId: "dead-loop", user: "u", threadKey: "t" })
    await h.settle()
    // 单次运行最多 3 次换锚，加原重试上限（3 次尝试）→ 执行次数有界
    expect(h.calls.length).toBeLessThanOrEqual(16)
    expect(h.calls.length).toBeGreaterThan(1)
    expect(h.scheduler.state().queued).toBe(0)
  })

  test("没配 resolveSession → 行为与改动前一致（只在原会话上重试）", async () => {
    const dead = new Set(["s1"])
    const h = makeHarness({ dead, reanchor: false })
    h.scheduler.enqueue({ event: ev("e1"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    expect(h.calls).toHaveLength(3)
    expect(h.calls.every((c) => c.sessionId === "s1")).toBe(true)
    expect(h.resolveCalls).toEqual([])
  })
})
