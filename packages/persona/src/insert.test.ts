/**
 * 插入机制用例：**运行中的会话接纳新事件**。
 *
 * 这是"会话要能被新事件插入"的核心验收。四条必须守住：
 *
 * 1. **插入生效**：运行期间到达的事件在**下一轮**就被处理，不是等下次
 * 2. **合并省成本**：同一轮吸收多条插话（一次讲清三件事，比跑三遍便宜）
 * 3. **轮次上限兜底**：插话洪水时封顶，**剩余事件重新入队不丢**
 * 4. **失败不丢**：运行异常时残留插话同样重新入队
 *
 * 第 3、4 条是"插话绝不退化成丢事件"的保证——丢事件比不插话严重得多。
 */
import { describe, expect, test } from "bun:test"
import { PersonaScheduler } from "./scheduler"
import { InterjectionMailbox } from "./mailbox"
import { renderBatch, replyHint, summarizeBatch } from "./render"
import type { PersonaEvent } from "./event"

function ev(id: string, at = Date.now()): PersonaEvent {
  return {
    id,
    kind: "message",
    personaId: "",
    channel: "feishu",
    context: { source: "飞书群「支持群」", summary: `事件 ${id}`, refs: { chatId: "oc_1", messageId: id } },
    body: `正文 ${id}`,
    receivedAt: at,
  }
}

/** 可控执行器：每轮挂起，手动放行。 */
function makeHarness(opts: { maxRounds?: number } = {}) {
  /** 每轮记录：{ round, ids } */
  const rounds: Array<{ round: number; ids: string[] }> = []
  const pending: Array<{ ids: string[]; resolve: () => void; reject: (e: Error) => void }> = []
  const started = new Map<string, () => void>()

  const scheduler = new PersonaScheduler({
    personaId: "p1",
    maxConcurrent: 1,
    ...(opts.maxRounds !== undefined ? { maxRounds: opts.maxRounds } : {}),
    execute: ({ events, round }) => {
      rounds.push({ round, ids: events.map((e) => e.id) })
      return new Promise<void>((resolve, reject) => pending.push({ ids: events.map((e) => e.id), resolve, reject }))
    },
  })
  const settle = async () => {
    for (let i = 0; i < 16; i++) await Promise.resolve()
  }
  /** 完成当前这一轮（触发轮末吸收）。 */
  const finishRound = async () => {
    const item = pending.shift()
    if (!item) throw new Error("没有在跑的轮次")
    item.resolve()
    await settle()
  }
  const failRound = async (msg = "boom") => {
    const item = pending.shift()
    if (!item) throw new Error("没有在跑的轮次")
    item.reject(new Error(msg))
    await settle()
  }
  return { scheduler, rounds, pending, settle, finishRound, failRound, started }
}

describe("mailbox：插话信箱", () => {
  test("FIFO 且 drain 清空", () => {
    const mb = new InterjectionMailbox()
    mb.push(ev("a"))
    mb.push(ev("b"))
    expect(mb.size()).toBe(2)
    expect(mb.drain().map((e) => e.id)).toEqual(["a", "b"])
    expect(mb.size()).toBe(0)
  })

  test("同 id 不重复入箱", () => {
    const mb = new InterjectionMailbox()
    mb.push(ev("a"))
    mb.push(ev("a"))
    expect(mb.size()).toBe(1)
  })

  test("hasPending / stats", () => {
    const mb = new InterjectionMailbox()
    expect(mb.hasPending()).toBe(false)
    mb.push(ev("x"))
    expect(mb.hasPending()).toBe(true)
    expect(mb.stats()).toMatchObject({ pending: 1, pushed: 1, drained: 0 })
    mb.drain()
    expect(mb.stats()).toMatchObject({ pending: 0, pushed: 1, drained: 1 })
  })
})

describe("插入：运行中的新事件在下一轮被处理", () => {
  test("首轮跑着时来了 B → 下一轮处理 B（不等下次运行）", async () => {
    const h = makeHarness()
    h.scheduler.enqueue({ event: ev("A"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    expect(h.rounds).toEqual([{ round: 1, ids: ["A"] }])

    // A 还在跑时插入 B
    h.scheduler.enqueue({ event: ev("B"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    expect(h.rounds).toHaveLength(1) // B 没有单独起运行，进信箱等轮末

    await h.finishRound() // A 结束 → 轮末吸收 B → 立即起第 2 轮
    expect(h.rounds).toEqual([
      { round: 1, ids: ["A"] },
      { round: 2, ids: ["B"] },
    ])
    await h.finishRound()
  })

  test("一轮内吸收多条插话（合并省成本）", async () => {
    const h = makeHarness()
    h.scheduler.enqueue({ event: ev("A"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    h.scheduler.enqueue({ event: ev("B"), sessionId: "s1", user: "u", threadKey: "t" })
    h.scheduler.enqueue({ event: ev("C"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()

    await h.finishRound()
    // B+C 合并进同一轮（一次讲清两件事，而不是跑两遍各自带全套上下文）
    expect(h.rounds).toEqual([
      { round: 1, ids: ["A"] },
      { round: 2, ids: ["B", "C"] },
    ])
    await h.finishRound()
  })

  test("无插话时运行正常结束（不会死循环）", async () => {
    const h = makeHarness()
    h.scheduler.enqueue({ event: ev("A"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    await h.finishRound()
    expect(h.rounds).toHaveLength(1)
    expect(h.scheduler.isRunning("s1")).toBe(false)
  })

  test("状态里能看到插话计数", async () => {
    const h = makeHarness()
    h.scheduler.enqueue({ event: ev("A"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    h.scheduler.enqueue({ event: ev("B"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    const st = h.scheduler.state().sessions.find((s) => s.sessionId === "s1")!
    expect(st.interjections).toBe(1)
    expect(st.pendingInterjections).toBe(1)
    expect(st.round).toBe(1)
    await h.finishRound()
    await h.finishRound()
  })
})

describe("插入：轮次上限兜底（绝不丢事件）", () => {
  /** 插话洪水时封顶，剩余重新入队——这是"丢事件"与"无限跑"之间的正确取舍。 */
  test("达到轮次上限 → 收尾并把剩余插话重新入队", async () => {
    const h = makeHarness({ maxRounds: 2 })
    h.scheduler.enqueue({ event: ev("A"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()

    // 第 1 轮跑着时插 B（会被第 2 轮吸收）
    h.scheduler.enqueue({ event: ev("B"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    await h.finishRound() // 第 1 轮结束 → 起第 2 轮（带 B）
    expect(h.rounds).toEqual([
      { round: 1, ids: ["A"] },
      { round: 2, ids: ["B"] },
    ])

    // 第 2 轮跑着时插 C —— 已达上限，C 不能进第 3 轮
    h.scheduler.enqueue({ event: ev("C"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    await h.finishRound() // 第 2 轮结束 → 达上限，C 重新入队

    // 关键：C **没有被丢弃**。重新入队后 finally 里的 pump 会立即把它拉起，
    // 所以此处它已不在队列（queued=0），而是已经在下一轮运行里了。
    const st = h.scheduler.state()
    expect(st.queued).toBe(0)
    await h.settle()
    // C 作为新一次运行的第 1 轮执行（事件未丢）
    expect(h.rounds.at(-1)).toEqual({ round: 1, ids: ["C"] })
    await h.finishRound()
  })
})

describe("插入：失败时残留插话不丢", () => {
  test("本轮抛错 → 信箱残留重新入队", async () => {
    const h = makeHarness()
    h.scheduler.enqueue({ event: ev("A"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    h.scheduler.enqueue({ event: ev("B"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()

    await h.failRound("模型接口挂了")
    // B 残留在信箱 → 应被重新入队（不是消失）
    const st = h.scheduler.state()
    expect(st.sessions.find((s) => s.sessionId === "s1")?.failed).toBe(1)
    // 重新入队后会被 subsequent pump 拉起
    await h.settle()
    expect(h.rounds.some((r) => r.ids.includes("B"))).toBe(true)
  })
})

describe("renderBatch：整编成会话", () => {
  test("单事件直通（零回归保证）", () => {
    const e = ev("x")
    expect(renderBatch([e])).toBe(e.body)
  })

  test("多事件编号并保留来源", () => {
    const out = renderBatch([ev("a"), ev("b")])
    expect(out).toContain("2 件事")
    expect(out).toContain("①")
    expect(out).toContain("②")
    expect(out).toContain("飞书群")
  })

  test("多事件附出站回法（告诉模型回到哪）", () => {
    const out = renderBatch([ev("a"), ev("b")])
    expect(out).toContain("send_message")
    expect(out).toContain("oc_1")
  })

  test("worker 回报无 chatId → 不出回法提示", () => {
    const w: PersonaEvent = {
      id: "worker:1",
      kind: "message",
      personaId: "p",
      channel: "worker",
      context: { source: "工作会话已完成", summary: "回报" },
      body: "结论：磁盘满了",
      receivedAt: Date.now(),
    }
    expect(replyHint(w)).toBeNull()
    expect(renderBatch([ev("a"), w])).not.toContain('reply_to="undefined"')
  })

  test("超长正文截断并标注", () => {
    const long = ev("a")
    long.body = "x".repeat(5000)
    const out = renderBatch([long, ev("b")], { maxBodyChars: 100 })
    expect(out).toContain("已截断")
  })

  test("summarizeBatch 汇总", () => {
    expect(summarizeBatch([ev("a")])).toBe("事件 a")
    expect(summarizeBatch([ev("a"), ev("b")])).toContain("2 件事")
  })
})
