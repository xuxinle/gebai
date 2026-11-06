/**
 * WS 多队列调度（ws-lanes.ts）单测。
 *
 * 守四件事，每件都对应一个真实踩过或极易踩到的坑：
 * ① **同 lane 严格保序**：同一条终端的按键顺序、同一会话的「先切后读」不能乱；
 * ② **跨 lane 真并发**：慢的 prompt 启动不得堵住终端按键与会话快照（原单链的核心缺陷）；
 * ③ **auth 屏障**：登录后立刻发的请求必须看到已登录身份（当初引入单链就是为了它，拆队不能丢）；
 * ④ **队列深度护栏**：某 lane 积压超限时拒绝新消息而不是无限堆内存，且不误伤其他 lane。
 */
import { describe, expect, test } from "bun:test"
import { ALL_LANES, LANE_MAX_DEPTH, laneOf, WsLanes } from "./ws-lanes"

/** 可控制完成时机的 promise。 */
function deferred(): { promise: Promise<void>; resolve: () => void } {
  let resolve!: () => void
  const promise = new Promise<void>((r) => (resolve = r))
  return { promise, resolve }
}

const tick = (ms = 0): Promise<void> => new Promise((r) => setTimeout(r, ms))

describe("消息类型 → lane 映射", () => {
  test("关键域各自成队（乱序会造成真实后果的那几类）", () => {
    expect(laneOf("auth.login")).toBe("auth")
    expect(laneOf("auth.logout")).toBe("auth")
    expect(laneOf("session.prompt")).toBe("prompt")
    expect(laneOf("session.cancel")).toBe("prompt")
    expect(laneOf("term.input")).toBe("term")
    expect(laneOf("lsp.change")).toBe("lsp")
    expect(laneOf("session.get")).toBe("session")
    expect(laneOf("approval.decide")).toBe("interaction")
    expect(laneOf("user.create")).toBe("admin")
  })

  test("终端与 LSP 不在同一队（否则打字与补全互相阻塞）", () => {
    expect(laneOf("term.input")).not.toBe(laneOf("lsp.change"))
    // 也不在会话队（会话读盘慢，不能压住按键）
    expect(laneOf("term.input")).not.toBe(laneOf("session.get"))
  })

  test("快照/重放/ping 与未知类型落 default（仍保序，不阻塞其余域）", () => {
    expect(laneOf("state.snapshot")).toBe("default")
    expect(laneOf("sync.request")).toBe("default")
    expect(laneOf("ping")).toBe("default")
    expect(laneOf("future.unknown_type")).toBe("default")
  })

  test("映射表里的 lane 全部在 ALL_LANES 内（防笔误写出一个永远不会被调度的队名）", () => {
    const types = [
      "auth.login", "session.get", "session.prompt", "term.input", "lsp.change",
      "approval.decide", "user.list", "state.snapshot",
    ]
    for (const t of types) expect(ALL_LANES).toContain(laneOf(t))
  })
})

describe("同 lane 保序", () => {
  test("同 lane 按提交顺序执行（即使前一条 await 很久）", async () => {
    const lanes = new WsLanes()
    const order: string[] = []
    const gate = deferred()
    lanes.submit("term.input", async () => {
      order.push("a-start")
      await gate.promise
      order.push("a-end")
    })
    lanes.submit("term.input", () => void order.push("b"))
    lanes.submit("term.input", () => void order.push("c"))
    await tick(10)
    expect(order).toEqual(["a-start"]) // 后面两条在排队
    gate.resolve()
    await lanes.waitAll()
    expect(order).toEqual(["a-start", "a-end", "b", "c"])
  })

  test("同一会话的「先切后读」保持顺序", async () => {
    const lanes = new WsLanes()
    const seq: string[] = []
    const gate = deferred()
    lanes.submit("session.switch", async () => {
      await gate.promise
      seq.push("switch")
    })
    lanes.submit("session.get", () => void seq.push("get"))
    gate.resolve()
    await lanes.waitAll()
    expect(seq).toEqual(["switch", "get"])
  })
})

describe("跨 lane 并发", () => {
  test("慢的 prompt 不阻塞终端按键（原单链的核心缺陷）", async () => {
    const lanes = new WsLanes()
    const gate = deferred()
    const started: string[] = []
    lanes.submit("session.prompt", async () => {
      started.push("prompt")
      await gate.promise
    })
    // 终端按键应立刻开始执行，不等 prompt 完成
    lanes.submit("term.input", () => void started.push("term"))
    await tick(10)
    expect(started).toEqual(["prompt", "term"])
    gate.resolve()
    await lanes.waitAll()
  })

  test("慢会话读盘不阻塞 LSP 编辑", async () => {
    const lanes = new WsLanes()
    const gate = deferred()
    const done: string[] = []
    lanes.submit("session.get", async () => {
      await gate.promise
      done.push("session")
    })
    lanes.submit("lsp.change", () => void done.push("lsp"))
    await tick(10)
    expect(done).toEqual(["lsp"]) // LSP 已经完成，会话还卡着
    gate.resolve()
    await lanes.waitAll()
  })

  test("三个不同 lane 可同时进行", async () => {
    const lanes = new WsLanes()
    const gates = [deferred(), deferred(), deferred()]
    const active = new Set<string>()
    const types = ["term.input", "lsp.change", "approval.decide"]
    types.forEach((t, i) => {
      lanes.submit(t, async () => {
        active.add(t)
        await gates[i].promise
        active.delete(t)
      })
    })
    await tick(10)
    expect(active.size).toBe(3) // 三条同时在跑
    gates.forEach((g) => g.resolve())
    await lanes.waitAll()
  })
})

describe("auth 屏障", () => {
  test("登录完成前，其他 lane 的消息不开始（保证「刚登录就发的请求」看到已登录身份）", async () => {
    const lanes = new WsLanes()
    const gate = deferred()
    const order: string[] = []
    lanes.submit("auth.login", async () => {
      order.push("login-start")
      await gate.promise
      order.push("login-end")
    })
    lanes.submit("session.get", () => void order.push("session.get"))
    lanes.submit("term.input", () => void order.push("term"))
    await tick(10)
    expect(order).toEqual(["login-start"]) // 其余都等在屏障后面
    gate.resolve()
    await lanes.waitAll()
    expect(order[0]).toBe("login-start")
    expect(order[1]).toBe("login-end")
    expect(order.slice(2).sort()).toEqual(["session.get", "term"])
  })

  test("登录之后才提交的消息同样等屏障（屏障不是一次性的时间窗，而是状态）", async () => {
    const lanes = new WsLanes()
    const gate = deferred()
    const order: string[] = []
    lanes.submit("auth.login", async () => {
      await gate.promise
      order.push("login")
    })
    await tick(5)
    // 这时才提交的请求也要等登录落地
    lanes.submit("session.list", () => void order.push("list"))
    await tick(5)
    expect(order).toEqual([])
    gate.resolve()
    await lanes.waitAll()
    expect(order).toEqual(["login", "list"])
  })

  test("两次登录之间保持先后（auth 自己的 lane 保序）", async () => {
    const lanes = new WsLanes()
    const order: string[] = []
    const gate = deferred()
    lanes.submit("auth.login", async () => {
      await gate.promise
      order.push("login1")
    })
    lanes.submit("auth.logout", () => void order.push("logout"))
    lanes.submit("auth.login", () => void order.push("login2"))
    gate.resolve()
    await lanes.waitAll()
    expect(order).toEqual(["login1", "logout", "login2"])
  })
})

describe("队列深度护栏", () => {
  test("同一 lane 积压超限时拒绝新消息（回 queue_full），不无限堆内存", async () => {
    const lanes = new WsLanes(4) // 小阈值便于测试
    const gate = deferred()
    let rejected = 0
    for (let i = 0; i < 4; i++) lanes.submit("term.input", async () => void (await gate.promise))
    // 第 5 条应被拒
    lanes.submit("term.input", () => {}, () => rejected++)
    expect(rejected).toBe(1)
    expect(lanes.rejectedOf("term")).toBe(1)
    expect(lanes.depthOf("term")).toBe(4)
    gate.resolve()
    await lanes.waitAll()
    expect(lanes.depthOf("term")).toBe(0)
  })

  test("某 lane 被限流不影响其他 lane（慢的只挡自己）", async () => {
    const lanes = new WsLanes(2)
    const gate = deferred()
    for (let i = 0; i < 2; i++) lanes.submit("lsp.change", async () => void (await gate.promise))
    let rejected = 0
    lanes.submit("lsp.change", () => {}, () => rejected++)
    expect(rejected).toBe(1)
    // 终端 lane 完全不受影响
    let ran = false
    lanes.submit("term.input", () => void (ran = true))
    await tick(5)
    expect(ran).toBe(true)
    gate.resolve()
    await lanes.waitAll()
  })

  test("默认阈值足够宽（真实输入速率远低于它）", () => {
    expect(LANE_MAX_DEPTH).toBeGreaterThanOrEqual(32)
  })
})

describe("waitAll 与异常兜底", () => {
  test("处理函数抛错不炸整条连接（其余消息照常执行）", async () => {
    const lanes = new WsLanes()
    const done: string[] = []
    lanes.submit("term.input", () => {
      throw new Error("boom")
    })
    lanes.submit("term.input", () => void done.push("after"))
    await lanes.waitAll()
    expect(done).toEqual(["after"])
  })

  test("waitAll 等到所有 lane 排空（含跨 lane 并行）", async () => {
    const lanes = new WsLanes()
    const done: string[] = []
    lanes.submit("term.input", async () => {
      await tick(10)
      done.push("term")
    })
    lanes.submit("lsp.change", async () => {
      await tick(5)
      done.push("lsp")
    })
    lanes.submit("session.get", () => void done.push("session"))
    await lanes.waitAll()
    expect(done.sort()).toEqual(["lsp", "session", "term"])
    expect(lanes.totalDepth()).toBe(0)
  })
})
