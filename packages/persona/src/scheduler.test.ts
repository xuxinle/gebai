/**
 * 调度器用例：两层队列的不变量。
 *
 * 三条硬不变量（破坏任意一条都会导致真实故障）：
 * 1. 同 sessionId 同时最多一个 execute 在途（引擎约束：并发 run 会抛错）
 * 2. 同时运行的 session 数 ≤ maxConcurrent（资源约束）
 * 3. 事件 FIFO 且不丢（人的话不能被静默丢弃）
 */
import { describe, expect, test } from "bun:test"
import { PersonaScheduler } from "./scheduler"
import type { PersonaEvent } from "./event"

function ev(id: string, at = Date.now()): PersonaEvent {
  return {
    id,
    kind: "message",
    personaId: "p1",
    channel: "feishu",
    context: { source: "s", summary: "m" },
    body: id,
    receivedAt: at,
  }
}

/** 可控执行器：手动 resolve 每个任务，观察并发与串行。 */
function makeHarness(maxConcurrent = 2) {
  const running: Array<{ id: string; sessionId: string; done: boolean; resolve: () => void; reject: (e: Error) => void }> = []
  /** 同一会话的并发计数（不变量 1 的判据）。 */
  const perSession = new Map<string, number>()
  let maxPerSession = 0
  let maxGlobal = 0
  const starts: string[] = []
  const scheduler = new PersonaScheduler({
    personaId: "p1",
    maxConcurrent,
    execute: (item) => {
      starts.push(item.events.map((e) => e.id).join(","))
      const cur = (perSession.get(item.sessionId) ?? 0) + 1
      perSession.set(item.sessionId, cur)
      maxPerSession = Math.max(maxPerSession, cur)
      // 活跃数 = running 里尚未完成的 + 本次（本次尚未 push）
      maxGlobal = Math.max(maxGlobal, running.filter((x) => !x.done).length + 1)
      return new Promise<void>((resolve, reject) => {
        const entry = {
          id: item.events.map((e) => e.id).join(","),
          sessionId: item.sessionId,
          done: false,
          resolve: () => {
            entry.done = true
            perSession.set(item.sessionId, (perSession.get(item.sessionId) ?? 1) - 1)
            resolve()
          },
          reject: (e: Error) => {
            entry.done = true
            perSession.set(item.sessionId, (perSession.get(item.sessionId) ?? 1) - 1)
            reject(e)
          },
        }
        running.push(entry)
      })
    },
  })
  const settle = async () => {
    // 让 microtask 排空（launch 是 async IIFE）
    for (let i = 0; i < 8; i++) await Promise.resolve()
  }
  return {
    scheduler,
    running,
    starts,
    settle,
    get maxPerSession() {
      return maxPerSession
    },
    get maxGlobal() {
      return maxGlobal
    },
    /** 完成当前在跑的第一个。 */
    async finishFirst() {
      const item = running.shift()
      if (!item) throw new Error("no running task")
      item.resolve()
      await settle()
    },
  }
}

describe("PersonaScheduler：会话级串行", () => {
  /**
   * 会话级串行 = 同一会话**绝不同时跑两轮**。
   *
   * 与旧版预期的区别：e2/e3 现在会被**合并进下一轮**（整编成会话的直接结果），
   * 而不是分成两次独立运行——这正是在「能合就合」与「不并发」之间取的平衡。
   */
  test("同一会话绝不并发；后续事件合并进下一轮", async () => {
    const h = makeHarness(4)
    h.scheduler.enqueue({ event: ev("e1"), sessionId: "s1", user: "u", threadKey: "t" })
    h.scheduler.enqueue({ event: ev("e2"), sessionId: "s1", user: "u", threadKey: "t" })
    h.scheduler.enqueue({ event: ev("e3"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    expect(h.running.map((r) => r.id)).toEqual(["e1"]) // 首轮只带第一件
    expect(h.maxPerSession).toBe(1)
    await h.finishFirst()
    // e2+e3 被合并进同一轮（一次把两件事讲清楚，比分两次跑便宜）
    expect(h.running.map((r) => r.id)).toEqual(["e2,e3"])
    await h.finishFirst()
    expect(h.running).toHaveLength(0)
    expect(h.maxPerSession).toBe(1) // 全程从未并发
  })

  test("FIFO：同会话内的事件按入队顺序进轮次", async () => {
    const h = makeHarness(1)
    h.scheduler.enqueue({ event: ev("e1"), sessionId: "s1", user: "u", threadKey: "t" })
    h.scheduler.enqueue({ event: ev("e2"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    await h.finishFirst()
    await h.finishFirst()
    expect(h.starts).toEqual(["e1", "e2"])
  })

  /**
   * 跨会话公平性：额度不足时取**队首入队最早**的会话，而不是 Map 迭代序。
   *
   * 构造要点（两个坑都是反向验证发现的）：
   * 1. `enqueue` 末尾会 pump，**额度空闲时事件立即开跑**——必须先用屏障会话占满额度。
   * 2. 会话 Map 插入序天然与队首入队时间单调一致，普通的「依次入队三个会话」
   *    区分不了两种实现（变异存活）。需要让两个会话**都在队列里**且先后入队。
   *
   * 注意：**不能让某会话在跑时再追入事件**——那些会进插话信箱（不经队列），
   * 那就测不到队列排序了。所以本用例全程用不同会话且都不在运行。
   */
  test("FIFO：跨会话按队首入队时间取最早（非 Map 迭代序）", async () => {
    const h = makeHarness(1)
    // 屏障占满唯一额度
    h.scheduler.enqueue({ event: ev("blocker"), sessionId: "sx", user: "u", threadKey: "tx" })
    await h.settle()
    expect(h.starts).toEqual(["blocker"])
    // 三个不同会话排队（都跑不了，因为额度被 blocker 占着）
    h.scheduler.enqueue({ event: ev("a1"), sessionId: "sA", user: "u", threadKey: "ta" })
    h.scheduler.enqueue({ event: ev("b1"), sessionId: "sB", user: "u", threadKey: "tb" })
    h.scheduler.enqueue({ event: ev("c1"), sessionId: "sC", user: "u", threadKey: "tc" })
    await h.settle()
    expect(h.scheduler.state().queued).toBe(3)
    // 逐个释放额度：应按入队时间 a1 → b1 → c1
    await h.finishFirst()
    expect(h.starts).toEqual(["blocker", "a1"])
    await h.finishFirst()
    expect(h.starts).toEqual(["blocker", "a1", "b1"])
    await h.finishFirst()
    expect(h.starts).toEqual(["blocker", "a1", "b1", "c1"])
  })

  /**
   * FIFO 的排序键必须是**入队序号**，不能是事件时间（`receivedAt`）。
   *
   * 这条用例存在的理由：单靠「依次入队三个会话」区分不了两种实现——那时入队顺序与
   * 事件时间天然同向（都单调递增），换成时间戳排序照样通过。必须让两者**相反**：
   *
   * > 一条**迟到重投的旧事件**（`receivedAt` 很早）晚于新事件入队，
   * > 它绝不能因为「事件时间早」而插队。
   *
   * 真实形态：飞书 ACK 前断连把旧消息重推回来、webhook 按自身策略重试、
   * 调度器重启重新触发同一时刻的任务。若按事件时间排序，新消息会被旧消息无限散死。
   */
  test("FIFO：迟到重投的旧事件不插队（排序键是入队序号，不是事件时间）", async () => {
    const h = makeHarness(1)
    // 屏障占满唯一额度
    h.scheduler.enqueue({ event: ev("blocker"), sessionId: "sx", user: "u", threadKey: "tx" })
    await h.settle()
    expect(h.starts).toEqual(["blocker"])

    const now = Date.now()
    // 新事件先入队（刚发生）
    h.scheduler.enqueue({ event: ev("fresh", now), sessionId: "sA", user: "u", threadKey: "ta" })
    // 旧事件后入队，但它的 receivedAt 很早（= 迟到重推）
    h.scheduler.enqueue({ event: ev("stale-redelivery", now - 60_000), sessionId: "sB", user: "u", threadKey: "tb" })
    await h.settle()
    expect(h.scheduler.state().queued).toBe(2)

    await h.finishFirst()
    // 先入队的 fresh 先跑——而不是因为 stale 的事件时间更早而插队
    expect(h.starts).toEqual(["blocker", "fresh"])
    await h.finishFirst()
    expect(h.starts).toEqual(["blocker", "fresh", "stale-redelivery"])
  })

  /**
   * 排序键也不能是**入队时刻的毫秒值**（`enqueuedAt`）——系统时钟会被回拨。
   *
   * 真实触发面：NTP 校时、虚拟机/容器迁移、手动改系统时间。时钟回拨后，
   * 后入队的事件 `enqueuedAt` 反而更小 → 它会插到先入队者前面，先到的会话被无限散死。
   *
   * 这就是为什么要用**单调递增序号**而不是任何时间戳：序号不受系统时钟影响。
   * （实测：这个变异体在时钟正常时与 seq 行为等价，只有回拨场景能暴露。）
   */
  test("FIFO：系统时钟回拨后仍按入队序（序号不受时钟影响）", async () => {
    const h = makeHarness(1)
    const origNow = Date.now
    try {
      h.scheduler.enqueue({ event: ev("blocker"), sessionId: "sx", user: "u", threadKey: "tx" })
      await h.settle()

      let clock = 1_000_000
      Date.now = () => clock
      // 先入队 first
      h.scheduler.enqueue({ event: ev("first", clock), sessionId: "sA", user: "u", threadKey: "ta" })
      // ★ 时钟回拨 1 小时，再入队 second
      clock = 1_000_000 - 3_600_000
      h.scheduler.enqueue({ event: ev("second", clock), sessionId: "sB", user: "u", threadKey: "tb" })
      await h.settle()
      expect(h.scheduler.state().queued).toBe(2)

      await h.finishFirst()
      // first 先入队 → first 先跑；若按 enqueuedAt 排序，second 会因时钟回拨而插队
      expect(h.starts).toEqual(["blocker", "first"])
      await h.finishFirst()
      expect(h.starts).toEqual(["blocker", "first", "second"])
    } finally {
      Date.now = origNow
    }
  })
})

describe("PersonaScheduler：人格级并发额度", () => {
  test("不同会话可并行，但不超过 maxConcurrent", async () => {
    const h = makeHarness(2)
    h.scheduler.enqueue({ event: ev("e1"), sessionId: "s1", user: "u", threadKey: "t1" })
    h.scheduler.enqueue({ event: ev("e2"), sessionId: "s2", user: "u", threadKey: "t2" })
    h.scheduler.enqueue({ event: ev("e3"), sessionId: "s3", user: "u", threadKey: "t3" })
    await h.settle()
    expect(h.running.map((r) => r.id).sort()).toEqual(["e1", "e2"]) // 只有 2 个在跑
    expect(h.scheduler.state().queued).toBe(1)
    await h.finishFirst()
    // e1 完成后额度释放、e3 补位；e2 从未完成，仍在跑
    expect(h.running.map((r) => r.id).sort()).toEqual(["e2", "e3"])
    expect(h.maxGlobal).toBeLessThanOrEqual(2)
  })

  test("一个会话卡住不影响其它会话（这正是分层的意义）", async () => {
    const h = makeHarness(3)
    h.scheduler.enqueue({ event: ev("stuck"), sessionId: "s1", user: "u", threadKey: "t1" })
    h.scheduler.enqueue({ event: ev("free1"), sessionId: "s2", user: "u", threadKey: "t2" })
    h.scheduler.enqueue({ event: ev("free2"), sessionId: "s3", user: "u", threadKey: "t3" })
    await h.settle()
    // s1 永不完成，但 s2/s3 都跑起来了
    expect(h.running.map((r) => r.id).sort()).toEqual(["free1", "free2", "stuck"])
    expect(h.maxPerSession).toBe(1)
  })
})

describe("PersonaScheduler：可观测与取消", () => {
  test("state 报出在跑/等待/统计", async () => {
    const h = makeHarness(2)
    h.scheduler.enqueue({ event: ev("e1"), sessionId: "s1", user: "u", threadKey: "t1" })
    h.scheduler.enqueue({ event: ev("e2"), sessionId: "s1", user: "u", threadKey: "t1" })
    await h.settle()
    const st = h.scheduler.state()
    expect(st.personaId).toBe("p1")
    expect(st.runningSessions).toBe(1)
    expect(st.queued).toBe(0) // e2 进了插话信箱，不在待跑队列
    expect(st.sessions).toHaveLength(1)
    expect(st.sessions[0].running).toBe(true)
    // e2 在会话运行期间到达 → 进**插话信箱**（等下一轮吸收），不是排队
    expect(st.sessions[0].waiting).toBe(0)
    expect(st.sessions[0].pendingInterjections).toBe(1)
    expect(st.sessions[0].interjections).toBe(1)
    expect(st.sessions[0].runningEventId).toBe("e1")
  })

  test("cancelRunning 中止在跑任务（signal aborted）", async () => {
    let aborted = false
    const scheduler = new PersonaScheduler({
      personaId: "p1",
      maxConcurrent: 2,
      execute: ({ signal }) =>
        new Promise<void>((resolve) => {
          signal.addEventListener("abort", () => {
            aborted = true
            resolve()
          })
        }),
    })
    scheduler.enqueue({ event: ev("e1"), sessionId: "s1", user: "u", threadKey: "t" })
    for (let i = 0; i < 8; i++) await Promise.resolve()
    expect(scheduler.cancelRunning("s1")).toBe(true)
    expect(aborted).toBe(true)
    // abort 后 execute 的 finally 要等 microtask 才清掉 running 记录
    for (let i = 0; i < 8; i++) await Promise.resolve()
    expect(scheduler.cancelRunning("s1")).toBe(false) // 已完成，无可取消
  })

  test("cancelWaiting 清空待跑但不动在跑", async () => {
    const h = makeHarness(1)
    h.scheduler.enqueue({ event: ev("e1"), sessionId: "s1", user: "u", threadKey: "t" })
    h.scheduler.enqueue({ event: ev("e2"), sessionId: "s1", user: "u", threadKey: "t" })
    h.scheduler.enqueue({ event: ev("e3"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    // e2/e3 在 e1 运行期间到达 → 进插话信箱（不在待跑队列）→ 无待跑可取消
    expect(h.scheduler.cancelWaiting("s1")).toBe(0)
    expect(h.running.map((r) => r.id)).toEqual(["e1"]) // 在跑的不受影响
    // 取消后全局排队计数必须归零（否则会持续占着额度，后续溢出判定失准）
    expect(h.scheduler.state().queued).toBe(0)
    await h.finishFirst()
    // 注意：cancelWaiting 只清**队列**；信箱里的 e2/e3 会被下一轮吸收（插入语义的必然）。
    // 要彻底停住应该用 cancelRunning——这里如实断言信箱内容仍会被处理。
    expect(h.running.map((r) => r.id)).toEqual(["e2,e3"])
  })

  test("cancelWaiting 对不存在的会话返回 0（不抛错）", () => {
    const h = makeHarness(1)
    expect(h.scheduler.cancelWaiting("nobody")).toBe(0)
  })
})

describe("PersonaScheduler：失败隔离与停止", () => {
  test("单个事件失败不影响后续事件继续跑", async () => {
    const h = makeHarness(1)
    h.scheduler.enqueue({ event: ev("bad"), sessionId: "s1", user: "u", threadKey: "t" })
    h.scheduler.enqueue({ event: ev("good"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    const item = h.running.shift()!
    item.reject(new Error("boom"))
    await h.settle()
    expect(h.running.map((r) => r.id)).toEqual(["good"]) // 后续照跑
    expect(h.scheduler.state().sessions[0].failed).toBe(1)
  })

  /**
   * 失败后必须继续泵下一个：异常路径上的 pump 是最容易漏的一行
   * （`finally` 里漏掉 → 一个事件出错后整个会话再也跑不动，而且不报错）。
   * 用**跨会话**验证：失败的会话释放额度后，另一个会话必须被拉起。
   */
  test("事件失败后额度必须释放给其它会话（异常路径也 pump）", async () => {
    const h = makeHarness(1)
    h.scheduler.enqueue({ event: ev("boom"), sessionId: "s1", user: "u", threadKey: "t1" })
    h.scheduler.enqueue({ event: ev("other"), sessionId: "s2", user: "u", threadKey: "t2" })
    await h.settle()
    expect(h.starts).toEqual(["boom"])
    const item = h.running.shift()!
    item.reject(new Error("boom"))
    await h.settle()
    expect(h.starts).toEqual(["boom", "other"]) // 失败也把额度让出来
  })

  test("stop 后不再接受新事件；start 可恢复", async () => {
    const h = makeHarness(1)
    h.scheduler.stop()
    expect(h.scheduler.enqueue({ event: ev("e1"), sessionId: "s1", user: "u", threadKey: "t" })).toBe(-1)
    h.scheduler.start()
    expect(h.scheduler.enqueue({ event: ev("e2"), sessionId: "s1", user: "u", threadKey: "t" })).toBe(0)
  })

  /**
   * stop 必须中止在跑任务：否则停止人格后，长任务还握着引擎的会话继续跑，
   * 而人格已「下线」——用户看到“停了但还在动”。
   */
  test("stop 中止在跑任务（signal aborted）并清空状态", async () => {
    let abortedCount = 0
    const scheduler = new PersonaScheduler({
      personaId: "p1",
      maxConcurrent: 3,
      execute: ({ signal }) =>
        new Promise<void>((resolve) => {
          signal.addEventListener("abort", () => {
            abortedCount++
            resolve()
          })
        }),
    })
    scheduler.enqueue({ event: ev("a"), sessionId: "s1", user: "u", threadKey: "t1" })
    scheduler.enqueue({ event: ev("b"), sessionId: "s2", user: "u", threadKey: "t2" })
    scheduler.enqueue({ event: ev("c"), sessionId: "s3", user: "u", threadKey: "t3" })
    for (let i = 0; i < 8; i++) await Promise.resolve()
    expect(scheduler.state().runningSessions).toBe(3)
    scheduler.stop()
    expect(abortedCount).toBe(3) // 全部被中止
    for (let i = 0; i < 8; i++) await Promise.resolve()
    expect(scheduler.state().runningSessions).toBe(0)
    expect(scheduler.state().queued).toBe(0)
  })

  test("同事件 id 在同一会话不重复排队（含正在跑的那条）", async () => {
    const h = makeHarness(4)
    h.scheduler.enqueue({ event: ev("dup"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    // 第一个已在跑，第二次入队同 id 应被忽略
    h.scheduler.enqueue({ event: ev("dup"), sessionId: "s1", user: "u", threadKey: "t" })
    await h.settle()
    expect(h.running).toHaveLength(1)
    await h.finishFirst()
    expect(h.running).toHaveLength(0)
  })

  test("非法 maxConcurrent 抛错", () => {
    expect(() => new PersonaScheduler({ personaId: "p", maxConcurrent: 0, execute: async () => {} })).toThrow()
  })

  /**
   * 队列上限：溢出时丢最旧、新事件接位（同步验证**被淘汰者确实不会再运行**）。
   *
   * 为何要验证「不再运行」：只看 `queued` 计数无法区分「真的被移除」与
   * 「只是减了计数但仍留在队列里」——后者会让陈旧事件在被回收前又被执行（副作用已过期）。
   * 实测：反向验证时把 `cancelled = true` 改成空操作，仅靠计数断言无法发现。
   */
  test("待跑队列达上限时丢弃最旧（且被淘汰者不会再运行）", async () => {
    const started: string[] = []
    /** 逐个放行：验证哪些事件真的被执行了。 */
    const gates: Array<() => void> = []
    const scheduler = new PersonaScheduler({
      personaId: "p1",
      maxConcurrent: 1,
      queueLimit: 3,
      execute: ({ events }) => {
        started.push(events.map((e) => e.id).join(","))
        return new Promise<void>((r) => gates.push(r))
      },
    })
    const settle = async () => {
      for (let i = 0; i < 8; i++) await Promise.resolve()
    }
    const release = async () => {
      gates.shift()?.()
      await settle()
    }
    scheduler.enqueue({ event: ev("blocker"), sessionId: "s0", user: "u", threadKey: "t0" })
    await settle()
    expect(started).toEqual(["blocker"])
    // 额度被 blocker 占着 → 接下来四条全在**队列**里（不同会话，避免走插话信箱）
    scheduler.enqueue({ event: ev("e1"), sessionId: "s1", user: "u", threadKey: "t1" })
    scheduler.enqueue({ event: ev("e2"), sessionId: "s2", user: "u", threadKey: "t2" })
    scheduler.enqueue({ event: ev("e3"), sessionId: "s3", user: "u", threadKey: "t3" })
    expect(scheduler.state().queued).toBe(3)
    // 满额（上限 3）：e4 入队 → 淘汰全局最旧的 e1，e4 接位
    expect(scheduler.enqueue({ event: ev("e4"), sessionId: "s4", user: "u", threadKey: "t4" })).toBe(0)
    expect(scheduler.state().queued).toBe(3)
    await release() // blocker 结束
    await release()
    await release()
    await release()
    // 被淘汰者（e1）绝不运行；其余按 FIFO 顺序跑完
    expect(started).toEqual(["blocker", "e2", "e3", "e4"])
    expect(started).not.toContain("e1")
  })
})
