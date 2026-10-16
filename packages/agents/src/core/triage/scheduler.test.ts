/**
 * 全局公平闸门测试：把「并发上限真的生效」「小任务不被大任务饿死」两件事固定为断言。
 *
 * 这里的测试全部用「不真等」的方式验证——闸门本身不做任何计时（等待只是排队），
 * 所以测试靠手动 release 驱动，毫秒级完成、无 flake。
 */
import { describe, expect, test } from "bun:test"
import { FairGate, FairGateHub, FAIR_GATE_ABORTED } from "./scheduler"

/** 记录授予顺序的辅助：每次 acquire 成功后把 owner 推进 log。 */
async function acquireLogged(gate: FairGate, owner: string, log: string[]): Promise<() => void> {
  const release = await gate.acquire({ owner })
  log.push(owner)
  return release
}

const tick = (): Promise<void> => new Promise((r) => setTimeout(r, 0))

describe("FairGate：上限与排队", () => {
  test("容量未满时立即授予（快路径），满了才排队", async () => {
    const gate = new FairGate(2)
    const r1 = await gate.acquire({ owner: "a" })
    const r2 = await gate.acquire({ owner: "a" })
    expect(gate.stats().inflight).toBe(2)

    let granted = false
    const pending = gate.acquire({ owner: "b" }).then((r) => {
      granted = true
      return r
    })
    await tick()
    expect(granted).toBe(false) // 满了 → 排队等，不占算力
    expect(gate.stats().waiting).toBe(1)

    r1()
    const r3 = await pending
    expect(granted).toBe(true)
    expect(gate.stats().inflight).toBe(2)
    r2()
    r3()
    expect(gate.stats().inflight).toBe(0)
  })

  test("在途数永不超过上限（放槽幂等，不会把额度算漏或算重）", async () => {
    const gate = new FairGate(3)
    const releases = await Promise.all([0, 1, 2].map(() => gate.acquire({ owner: "a" })))
    expect(gate.stats().inflight).toBe(3)
    releases[0]()
    releases[0]() // 重复释放应幂等
    expect(gate.stats().inflight).toBe(2)
    releases[1]()
    releases[2]()
    expect(gate.stats().inflight).toBe(0)
    expect(gate.stats().peak_inflight).toBe(3)
  })

  test("等待中中止：抛中止错、出队，且不泄漏槽位", async () => {
    const gate = new FairGate(1)
    const held = await gate.acquire({ owner: "a" })
    const ac = new AbortController()
    const pending = gate.acquire({ owner: "b", signal: ac.signal })
    ac.abort()
    await expect(pending).rejects.toThrow(FAIR_GATE_ABORTED)
    expect(gate.stats().waiting).toBe(0)
    held()
    expect(gate.stats().inflight).toBe(0)
  })

  test("已中止的信号：不进队直接拒绝", async () => {
    const gate = new FairGate(1)
    const ac = new AbortController()
    ac.abort()
    await expect(gate.acquire({ owner: "a", signal: ac.signal })).rejects.toThrow(FAIR_GATE_ABORTED)
  })
})

describe("FairGate：公平轮转（小任务不被大任务饿死）", () => {
  test("大任务排了一串，小任务仍能在下一轮拿到槽位（上限 1）", async () => {
    const gate = new FairGate(1)
    const held = await gate.acquire({ owner: "holder" })

    const log: string[] = []
    const big: Array<Promise<void>> = []
    for (let i = 0; i < 6; i++) big.push(acquireLogged(gate, "big", log).then((r) => r()))
    // 小任务在**队尾**（大任务先占了队列，小任务最后才到）
    const small = acquireLogged(gate, "small", log).then((r) => r())

    held()
    await tick()
    // 第 1 个授予可以是 big；但 small 必须在第 2 个授予就拿到（而不是等到 6 个 big 全部跑完）
    const smallIndex = log.indexOf("small")
    expect(smallIndex).toBeGreaterThanOrEqual(0)
    expect(smallIndex).toBeLessThanOrEqual(1)
    await Promise.all([...big, small])
  })

  test("多 owner 争抢：一圈之内每个 owner 都能拿到（轮转）", async () => {
    const gate = new FairGate(1)
    const held = await gate.acquire({ owner: "holder" })
    const log: string[] = []
    const all: Array<Promise<void>> = []
    // 每个 owner 都排 3 条，顺序刻意交错成大任务在前
    for (let i = 0; i < 3; i++) {
      for (const owner of ["a", "b", "c"]) all.push(acquireLogged(gate, owner, log).then((r) => r()))
    }
    held()
    await tick()
    // 前 3 个授予必须是 3 个不同 owner（轮转的最小保证）
    expect(new Set(log.slice(0, 3)).size).toBe(3)
    await Promise.all(all)
    expect(log.length).toBe(9)
  })

  test("上限调大立即唤醒等待者（任务挂/卸时重算上限）", async () => {
    const gate = new FairGate(1)
    const held = await gate.acquire({ owner: "a" })
    let granted = false
    const pending = gate.acquire({ owner: "b" }).then((r) => {
      granted = true
      return r
    })
    await tick()
    expect(granted).toBe(false)
    gate.setLimit(2)
    await pending
    expect(granted).toBe(true)
    held()
    expect(gate.stats().peak_inflight).toBe(2)
  })
})

describe("FairGateHub：同端点共享一个闸门", () => {
  test("同一 key 返回同一实例；上限取成员并发度的最大值（不相加）", () => {
    const hub = new FairGateHub()
    const g1 = hub.attach("http://x", "jobA", 8)
    expect(g1.limit).toBe(8)
    const g2 = hub.attach("http://x", "jobB", 8)
    expect(g2).toBe(g1) // 共享：两个任务一起受 8 个在途额度约束
    expect(g2.limit).toBe(8) // 不是 16——并发加到网关上只会排队，不会提吞吐
    expect(hub.attach("http://y", "jobC", 2).limit).toBe(2) // 不同端点各自一份
  })

  test("env 显式上限优先于成员推导", () => {
    const hub = new FairGateHub()
    const g = hub.attach("http://x", "jobA", 8, 16)
    expect(g.limit).toBe(16)
    hub.attach("http://x", "jobB", 2)
    expect(hub.limitOf("http://x")).toBe(16)
  })

  test("任务卸载后上限按剩余成员重算；最后一个卸载不报错", () => {
    const hub = new FairGateHub()
    hub.attach("http://x", "jobA", 8)
    hub.attach("http://x", "jobB", 2)
    expect(hub.limitOf("http://x")).toBe(8)
    hub.detach("http://x", "jobA")
    expect(hub.limitOf("http://x")).toBe(2) // 大任务走了，小任务恢复独享
    hub.detach("http://x", "jobB")
    expect(hub.limitOf("http://x")).toBe(1)
    hub.detach("http://x", "jobB") // 幂等
  })
})
