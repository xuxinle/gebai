/** 会话输入队列（服务端权威）的纯逻辑测试：入队/编辑/撤回/置顶/派发/上限/归属隔离。
 *
 *  这些行为替代了原先「前端本地队列 + 前端 drain」的全部语义（刷新不丢、多端一致、
 *  后台会话照常推进），所以闭合条件（幂等 drain、派发中重入、派发失败继续下一条）
 *  必须钉死——它们缺一个就是「排队输入永远不执行」或「同一条执行两次」。 */
import { describe, expect, test } from "bun:test"
import { MAX_QUEUED_PER_SESSION, SessionInputQueue, type QueuedInput } from "./input-queue"

const item = (sessionId: string, text: string, extra: Partial<QueuedInput> = {}): QueuedInput => ({
  id: `${sessionId}-${text}`,
  sessionId,
  userId: "u1",
  text,
  attachments: [],
  messageId: `m-${text}`,
  createdAt: Date.now(),
  ...extra,
})

/** 装配一个可观测的队列：running 由外部开关控制，派发记进 ran 数组。 */
function setup(opts: { fail?: boolean } = {}) {
  const q = new SessionInputQueue()
  const ran: QueuedInput[] = []
  const notes: string[] = []
  let running = false
  q.attach({
    isRunning: () => running,
    runner: async (it) => {
      ran.push(it)
      await Promise.resolve()
      if (opts.fail) throw new Error("boom")
    },
    notify: (sid) => notes.push(sid),
  })
  return { q, ran, notes, setRunning: (v: boolean) => { running = v } }
}

const tick = () => new Promise((r) => setTimeout(r, 0))

describe("入队与出队", () => {
  test("空闲时入队立即派发（覆盖「运行恰在入队前结束」的竞态窗口）", async () => {
    const { q, ran } = setup()
    q.enqueue(item("s1", "第一条"))
    await tick()
    expect(ran.map((x) => x.text)).toEqual(["第一条"])
    expect(q.size("s1")).toBe(0)
  })

  test("运行中入队不派发；任务收尾后按序逐条执行", async () => {
    const { q, ran, setRunning } = setup()
    setRunning(true)
    q.enqueue(item("s1", "A"))
    q.enqueue(item("s1", "B"))
    await tick()
    expect(ran).toHaveLength(0)
    expect(q.list("s1").map((x) => x.text)).toEqual(["A", "B"])
    // 任务收尾：队列派发队首，派发完成后接着下一条
    setRunning(false)
    q.drain("s1")
    await tick()
    await tick()
    expect(ran.map((x) => x.text)).toEqual(["A", "B"])
    expect(q.size("s1")).toBe(0)
  })

  test("入队返回队列位置；队首为 1", () => {
    const { q, setRunning } = setup()
    setRunning(true)
    expect(q.enqueue(item("s1", "A")).ok && (q.enqueue(item("s1", "B")) as { position: number }).position).toBe(2)
  })

  test("中断插入（front）进队首", async () => {
    const { q, ran, setRunning } = setup()
    setRunning(true)
    q.enqueue(item("s1", "A"))
    q.enqueue(item("s1", "B"))
    q.enqueue(item("s1", "X"), { front: true })
    setRunning(false)
    q.drain("s1")
    await tick()
    await tick()
    await tick()
    expect(ran.map((x) => x.text)).toEqual(["X", "A", "B"])
  })

  test("单会话排队上限：超限拒绝且不入队", () => {
    const { q, setRunning } = setup()
    setRunning(true)
    for (let i = 0; i < MAX_QUEUED_PER_SESSION; i++) expect(q.enqueue(item("s1", `t${i}`)).ok).toBe(true)
    const over = q.enqueue(item("s1", "溢出"))
    expect(over.ok).toBe(false)
    expect(q.size("s1")).toBe(MAX_QUEUED_PER_SESSION)
  })

  test("空内容拒绝入队", () => {
    const { q } = setup()
    expect(q.enqueue(item("s1", "   ")).ok).toBe(false)
  })
})

describe("编辑 / 撤回 / 置顶", () => {
  test("编辑替换内容；空内容不许", () => {
    const { q, setRunning } = setup()
    setRunning(true)
    q.enqueue(item("s1", "原文"))
    const id = q.list("s1")[0].id
    expect(q.edit("s1", id, "改后")).toBe(true)
    expect(q.list("s1")[0].text).toBe("改后")
    expect(q.edit("s1", id, "  ")).toBe(false)
    expect(q.list("s1")[0].text).toBe("改后")
  })

  test("撤回移除条目；不存在返回 false", () => {
    const { q, setRunning } = setup()
    setRunning(true)
    q.enqueue(item("s1", "A"))
    const id = q.list("s1")[0].id
    expect(q.remove("s1", id)).toBe(true)
    expect(q.size("s1")).toBe(0)
    expect(q.remove("s1", id)).toBe(false)
  })

  test("置顶把条目移到队首（运行中不派发，等收尾）", async () => {
    const { q, ran, setRunning } = setup()
    setRunning(true)
    q.enqueue(item("s1", "A"))
    q.enqueue(item("s1", "B"))
    const bId = q.list("s1")[1].id
    expect(q.promote("s1", bId)).toBe(true)
    expect(q.list("s1").map((x) => x.text)).toEqual(["B", "A"])
    expect(ran).toHaveLength(0)
    setRunning(false)
    q.drain("s1")
    await tick()
    await tick()
    expect(ran.map((x) => x.text)).toEqual(["B", "A"])
  })

  test("归属校验：非 owner 不可见也不可操作", () => {
    const { q, setRunning } = setup()
    setRunning(true)
    q.enqueue(item("s1", "A", { userId: "u1" }))
    const id = q.list("s1")[0].id
    expect(q.list("s1", "u2")).toEqual([])
    expect(q.edit("s1", id, "x", "u2")).toBe(false)
    expect(q.remove("s1", id, "u2")).toBe(false)
    expect(q.promote("s1", id, "u2")).toBe(false)
  })

  test("快照只含指定用户的会话，且不带内部字段 userId", () => {
    const { q, setRunning } = setup()
    setRunning(true)
    q.enqueue(item("s1", "A", { userId: "u1" }))
    q.enqueue(item("s2", "B", { userId: "u2" }))
    const snap = q.snapshotPublic("u1")
    expect(Object.keys(snap)).toEqual(["s1"])
    expect(snap.s1[0].text).toBe("A")
    expect("userId" in snap.s1[0]).toBe(false)
  })
})

describe("派发健壮性", () => {
  test("派发中重入不双发；派发结束后继续下一条", async () => {
    const { q, ran } = setup()
    q.enqueue(item("s1", "A"))
    q.enqueue(item("s1", "B"))
    // 派发进行中（A 的 runner 未 settle）：再次 drain 不得重复取 A/B
    q.drain("s1")
    q.drain("s1")
    await tick()
    await tick()
    expect(ran.map((x) => x.text)).toEqual(["A", "B"])
  })

  test("派发失败仍继续下一条（失败即出队，不卡死后续）", async () => {
    const { q, ran } = setup({ fail: true })
    q.enqueue(item("s1", "A"))
    q.enqueue(item("s1", "B"))
    await tick()
    await tick()
    expect(ran.map((x) => x.text)).toEqual(["A", "B"])
    expect(q.size("s1")).toBe(0)
  })

  test("未装配 runner 时只记不跑（不吞条目）", async () => {
    const q = new SessionInputQueue()
    q.enqueue(item("s1", "A"))
    await tick()
    expect(q.size("s1")).toBe(1)
  })

  test("forget 释放队列与归属；forgetUser 清该用户全部会话", () => {
    const { q, setRunning } = setup()
    setRunning(true)
    q.enqueue(item("s1", "A", { userId: "u1" }))
    q.enqueue(item("s2", "B", { userId: "u1" }))
    q.forget("s1")
    expect(q.size("s1")).toBe(0)
    q.forgetUser("u1")
    expect(q.size("s2")).toBe(0)
  })
})
