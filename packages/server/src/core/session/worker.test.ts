/**
 * 派活链路：`SessionWorkerSpawner`（宿主实现）+ `worker_*` 工具（模型面）。
 *
 * 这组用例要钉住的是**几条真实会出事的语义**：
 *
 * | 语义 | 破坏后果 |
 * |---|---|
 * | worker 用独立会话、跑完即删 | 侧栏被「派活」会话淹没；不删则一个活跃人格一天几十个 |
 * | 跑完无文本结论 → failed 而不是 done | driver 会以为“查完了，没发现问题”而漏报 |
 * | 异步派活立刻拿到 runId | worker_list/worker_cancel 在结论回来前找不到这个活 |
 * | 叫停后不改回 done | 模型已经决定不要这个结果了，反过来把它标成成功是谎报 |
 * | verify 必填 | 回来的结论无法直接引用（“我看了一下，大概是……”） |
 * | 只允许停本会话派出的活 | A 人格能掐断 B 人格的活（人格之间没有隶属关系） |
 */
import { describe, expect, test } from "bun:test"
import type { SessionData } from "./store"
import type { Message } from "@gebai/sdk"
import type { WorkerOutcome, WorkerSpawnInput } from "@gebai/persona"
import { finalAnswerOf, SessionWorkerSpawner, workerNote, workerPrompt } from "./worker-spawner"
import { createWorkerTaskBook, createWorkerTools } from "./worker-tools"

function session(id: string, messages: Message[] = []): SessionData {
  return { id, name: "w", userId: "admin", messages, todos: [], createdAt: 0, updatedAt: 0 }
}

/** store 替身：记录创建/删除，供断言「跑完即删」。 */
function fakeStore(opts: { messages?: Message[]; loadThrows?: boolean } = {}) {
  const created: string[] = []
  const deleted: string[] = []
  let n = 0
  return {
    created,
    deleted,
    createSession: async (_user: string, _name?: string) => {
      const id = `ws${++n}`
      created.push(id)
      return session(id)
    },
    load: async (id: string) => {
      if (opts.loadThrows) throw new Error("读不到")
      return session(id, opts.messages ?? [])
    },
    delete: async (id: string) => {
      deleted.push(id)
    },
  }
}

/** 引擎替身：可控「跑多久 / 抛什么 / 是否被取消」。 */
function fakeEngine(opts: { onRun?: (sessionId: string) => Promise<void> | void; throwOnRun?: Error } = {}) {
  const runs: Array<{ sessionId: string; prompt: string; opts?: Record<string, unknown> }> = []
  const cancelled: string[] = []
  return {
    runs,
    cancelled,
    engine: {
      run: async (sessionId: string, _user: string, prompt: string, o?: Record<string, unknown>) => {
        runs.push({ sessionId, prompt, opts: o })
        if (opts.throwOnRun) throw opts.throwOnRun
        if (opts.onRun) await opts.onRun(sessionId)
      },
      cancel: (sessionId: string) => void cancelled.push(sessionId),
      isRunning: () => false,
    },
  }
}

const IDENTITY = { id: "xiaoshuang", name: "小霜", identity: "你是小霜，值班工程师。", role: "运维" }

describe("workerPrompt：派活的提示词要写清什么", () => {
  const base: WorkerSpawnInput = {
    personaId: "xiaoshuang",
    user: "admin",
    identity: IDENTITY,
    task: "查一下 tsdb 写入延迟为什么涨了",
    verify: "给出具体数值与可复现命令",
    driverSessionId: "d1",
    signal: new AbortController().signal,
  }

  test("含派出者身份、任务、验收标准（三者缺一 worker 就会自己猜）", () => {
    const p = workerPrompt(base)
    expect(p).toContain("小霜")
    expect(p).toContain("运维")
    expect(p).toContain("tsdb 写入延迟")
    expect(p).toContain("可复现命令")
  })

  test("含交付要求：给结论不给过程叙述（结论要原样交给 driver）", () => {
    const p = workerPrompt(base)
    expect(p).toContain("原样交给派出者")
    expect(p).toContain("不要把没做成写成做成了")
  })

  test("不给 context 时不出现空标题（空段落只会占 token）", () => {
    expect(workerPrompt(base)).not.toContain("已知背景")
    expect(workerPrompt({ ...base, context: "已经确认不是磁盘满" })).toContain("已经确认不是磁盘满")
  })
})

describe("workerNote：worker 不对外发言", () => {
  test("明确禁止 act 与再派活（否则群里同一个问题会被答两遍）", () => {
    const note = workerNote({
      personaId: "p",
      user: "admin",
      identity: IDENTITY,
      task: "t",
      driverSessionId: "d1",
      signal: new AbortController().signal,
    })
    expect(note).toContain("不要对外发消息")
    expect(note).toContain("收到两遍")
    expect(note).toContain("不要派活")
  })
})

describe("finalAnswerOf：取最终结论", () => {
  test("取最后一条有文本的 assistant 消息（跳过工具调用中间轮）", () => {
    const s = session("s1", [
      { id: "1", role: "user", content: "做点事", createdAt: 1 } as Message,
      { id: "2", role: "assistant", content: "", createdAt: 2 } as Message,
      { id: "3", role: "tool", content: "结果", createdAt: 3 } as Message,
      { id: "4", role: "assistant", content: "结论：延迟 320ms，见 a.ts:12", createdAt: 4 } as Message,
    ])
    expect(finalAnswerOf(s)).toBe("结论：延迟 320ms，见 a.ts:12")
  })

  test("只有空白的 assistant 消息不算结论", () => {
    const s = session("s1", [{ id: "1", role: "assistant", content: "   \n ", createdAt: 1 } as Message])
    expect(finalAnswerOf(s)).toBe("")
  })

  test("块形态内容里抽 text 块", () => {
    const s = session("s1", [
      { id: "1", role: "assistant", content: [{ type: "text", text: "块里的结论" }], createdAt: 1 } as unknown as Message,
    ])
    expect(finalAnswerOf(s)).toBe("块里的结论")
  })

  test("会话为 null → 空串（读不到不等于有结论）", () => {
    expect(finalAnswerOf(null)).toBe("")
  })
})

describe("SessionWorkerSpawner：生命周期", () => {
  test("正常跑完：结论回来、会话被删（跑完即走）", async () => {
    const store = fakeStore({ messages: [{ id: "1", role: "assistant", content: "查清了：延迟来自写入放大", createdAt: 1 } as Message] })
    const { engine } = fakeEngine()
    const s = new SessionWorkerSpawner({ engine, store })
    const out = await s.spawn({
      personaId: "xiaoshuang",
      user: "admin",
      identity: IDENTITY,
      task: "查延迟",
      verify: "给数值",
      driverSessionId: "d1",
      signal: new AbortController().signal,
    })
    expect(out.status).toBe("done")
    expect(out.report).toContain("写入放大")
    expect(store.created.length).toBe(1)
    expect(store.deleted).toEqual(store.created) // 结论已交出，会话不留
  })

  test("引擎返回但没有任何文本结论 → failed（不能当成“没问题”）", async () => {
    const store = fakeStore({ messages: [] })
    const { engine } = fakeEngine()
    const out = await new SessionWorkerSpawner({ engine, store }).spawn({
      personaId: "p",
      user: "admin",
      identity: IDENTITY,
      task: "t",
      driverSessionId: "d1",
      signal: new AbortController().signal,
    })
    expect(out.status).toBe("failed")
    expect(out.error).toContain("没有给出任何文本结论")
  })

  test("引擎抛错 → failed 且带原因；会话仍然被清理", async () => {
    const store = fakeStore()
    const { engine } = fakeEngine({ throwOnRun: new Error("模型接口 500") })
    const out = await new SessionWorkerSpawner({ engine, store }).spawn({
      personaId: "p",
      user: "admin",
      identity: IDENTITY,
      task: "t",
      driverSessionId: "d1",
      signal: new AbortController().signal,
    })
    expect(out.status).toBe("failed")
    expect(out.error).toContain("500")
    expect(store.deleted.length).toBe(1)
  })

  test("派活时已被取消 → cancelled，且**不建会话**（省一次创建与删除）", async () => {
    const store = fakeStore()
    const { engine } = fakeEngine()
    const ctrl = new AbortController()
    ctrl.abort()
    const out = await new SessionWorkerSpawner({ engine, store }).spawn({
      personaId: "p",
      user: "admin",
      identity: IDENTITY,
      task: "t",
      driverSessionId: "d1",
      signal: ctrl.signal,
    })
    expect(out.status).toBe("cancelled")
    expect(store.created.length).toBe(0)
  })

  test("调用方预分配的 runId 被采用（异步派活靠它先记账）", async () => {
    const store = fakeStore({ messages: [{ id: "1", role: "assistant", content: "结论", createdAt: 1 } as Message] })
    const { engine } = fakeEngine()
    const out = await new SessionWorkerSpawner({ engine, store }).spawn({
      personaId: "p",
      user: "admin",
      identity: IDENTITY,
      task: "t",
      driverSessionId: "d1",
      runId: "worker_fixed_1",
      signal: new AbortController().signal,
    })
    expect(out.runId).toBe("worker_fixed_1")
  })

  test("worker 以 interactionMode=none 运行（无交互通道：ask/show 对它没意义）", async () => {
    const store = fakeStore({ messages: [{ id: "1", role: "assistant", content: "结论", createdAt: 1 } as Message] })
    const { engine, runs } = fakeEngine()
    await new SessionWorkerSpawner({ engine, store }).spawn({
      personaId: "p",
      user: "admin",
      identity: IDENTITY,
      task: "t",
      driverSessionId: "d1",
      signal: new AbortController().signal,
    })
    expect(runs[0]!.opts?.interactionMode).toBe("none")
    // 通道注记与提示词都进了引擎（worker 靠这两段知道自己是谁派来的、不许对外发言）
    expect(String(runs[0]!.opts?.channelNote)).toContain("不要对外发消息")
    expect(runs[0]!.prompt).toContain("查")
  })
})

describe("createWorkerTools：模型面", () => {
  function setup(opts: { owner?: boolean; identityMsg?: Message[]; spawnOutcome?: Partial<WorkerOutcome>; gate?: boolean } = {}) {
    const store = fakeStore({ messages: opts.identityMsg ?? [{ id: "1", role: "assistant", content: "结论", createdAt: 1 } as Message] })
    // gate=true：引擎挂在跑（不在本用例内完成）——用来验证「结论回来前的账本状态」
    // （不挂住的话 worker 几十毫秒就跑完了，叫停测不到 running 态）
    let release: (() => void) | null = null
    const hung = new Promise<void>((r) => {
      release = r
    })
    const { engine } = fakeEngine(opts.gate ? { onRun: () => hung } : {})
    const spawner = new SessionWorkerSpawner({ engine, store })
    const book = createWorkerTaskBook()
    const reports: Array<{ runId: string; personaId: string; driverId: string }> = []
    const tools = createWorkerTools({
      spawner,
      book,
      store: {
        load: async (id: string) =>
          opts.owner === false
            ? session(id)
            : ({ ...session(id), persona: { personaId: "xiaoshuang", driverId: "support", boundAt: 1 } } as SessionData),
      },
      identity: (id) => (id === "xiaoshuang" ? IDENTITY : null),
      report: async (record) => void reports.push({ runId: record.runId, personaId: record.personaId, driverId: record.driverId }),
    })
    return { tools, book, reports, store, release: () => release?.() }
  }

  const ctx = (sessionId = "s1") => ({ sessionId, user: "admin" }) as never

  test("普通会话：派活被拒且不建会话（能力只在人格会话）", async () => {
    const { tools, store } = setup({ owner: false })
    const dispatch = tools.find((t) => t.name === "worker_dispatch")!
    const r = await dispatch.execute({ task: "查", verify: "给数值" }, ctx())
    expect(r.output).toContain("不属于任何人格")
    expect(store.created.length).toBe(0)
  })

  test("缺 verify → 拒绝并说明为什么（不是形式要求）", async () => {
    const { tools } = setup()
    const dispatch = tools.find((t) => t.name === "worker_dispatch")!
    const r = await dispatch.execute({ task: "查一下" }, ctx())
    expect(r.output).toContain("verify")
    expect(r.output).toContain("什么算查清了")
  })

  test("异步派活：立刻返回 runId，且账本里马上查得到（可 worker_list / worker_cancel）", async () => {
    const { tools, book } = setup()
    const dispatch = tools.find((t) => t.name === "worker_dispatch")!
    const r = await dispatch.execute({ task: "查延迟", verify: "给数值", label: "查延迟" }, ctx())
    expect(r.output).toContain("异步执行中")
    const runId = (r.data as { runId: string }).runId
    expect(runId).toStartWith("worker_")
    expect(book.get(runId)?.status).toBe("running") // 结论回来前也在账本里
  })

  test("同步模式（wait=true）：等回结论并直接给出", async () => {
    const { tools } = setup()
    const dispatch = tools.find((t) => t.name === "worker_dispatch")!
    const r = await dispatch.execute({ task: "查延迟", verify: "给数值", wait: true }, ctx())
    expect(r.output).toContain("已等回结论")
    expect(r.output).toContain("结论")
  })

  test("worker_list：只列本会话派出的活", async () => {
    const { tools } = setup()
    const dispatch = tools.find((t) => t.name === "worker_dispatch")!
    const list = tools.find((t) => t.name === "worker_list")!
    await dispatch.execute({ task: "活A", verify: "v", wait: true }, ctx("s1"))
    const r1 = await list.execute({}, ctx("s1"))
    const r2 = await list.execute({}, ctx("s2"))
    expect(r1.output).toContain("活A")
    expect(r2.output).toContain("还没有派过活")
  })

  test("worker_cancel：跨会话派出的活不可叫停（人格之间没有隶属关系）", async () => {
    const { tools } = setup({ gate: true })
    const dispatch = tools.find((t) => t.name === "worker_dispatch")!
    const cancel = tools.find((t) => t.name === "worker_cancel")!
    const r = await dispatch.execute({ task: "查延迟", verify: "v", label: "x" }, ctx("s1"))
    const runId = (r.data as { runId: string }).runId
    const foreign = await cancel.execute({ run_id: runId }, ctx("s2"))
    expect(foreign.output).toContain("本会话没有")
    const own = await cancel.execute({ run_id: runId }, ctx("s1"))
    expect(own.output).toContain("已叫停")
  })

  test("已结束的活：叫停返回「无需叫停」而不是谎称叫停成功", async () => {
    const { tools } = setup()
    const dispatch = tools.find((t) => t.name === "worker_dispatch")!
    const cancel = tools.find((t) => t.name === "worker_cancel")!
    const r = await dispatch.execute({ task: "查延迟", verify: "v", wait: true }, ctx("s1"))
    const runId = (r.data as { runId: string }).runId
    const after = await cancel.execute({ run_id: runId }, ctx("s1"))
    expect(after.output).toContain("无需叫停")
  })

  test("叫停后结论到达：账本保留 cancelled（不改回 done）", async () => {
    const { tools, book, release } = setup({ gate: true })
    const dispatch = tools.find((t) => t.name === "worker_dispatch")!
    const cancel = tools.find((t) => t.name === "worker_cancel")!
    const r = await dispatch.execute({ task: "查延迟", verify: "v" }, ctx())
    const runId = (r.data as { runId: string }).runId
    await cancel.execute({ run_id: runId }, ctx())
    release() // 让 worker 跑完（模拟“叫停后它仍然正常收了尾”）
    await new Promise((r) => setTimeout(r, 30))
    expect(book.get(runId)?.status).toBe("cancelled")
  })

  test("回报事件带正确的 persona/driver（定向回派活的那个分身）", async () => {
    const { tools, reports } = setup()
    const dispatch = tools.find((t) => t.name === "worker_dispatch")!
    const r = await dispatch.execute({ task: "查延迟", verify: "v", wait: true }, ctx())
    const runId = (r.data as { runId: string }).runId
    expect(reports).toEqual([{ runId, personaId: "xiaoshuang", driverId: "support" }])
  })
})

describe("createWorkerTaskBook：有界", () => {
  test("超过 keep 条后淘汰最旧的（防长运行进程无界增长）", () => {
    const book = createWorkerTaskBook({ keep: 3 })
    const base = { personaId: "p", driverId: "d", driverSessionId: "s", user: "admin", task: "t", startedAt: 1, status: "done" as const }
    for (let i = 0; i < 5; i++) book.add({ ...base, runId: `r${i}`, startedAt: i })
    expect(book.get("r0")).toBeUndefined()
    expect(book.get("r1")).toBeUndefined()
    expect(book.get("r4")).toBeDefined()
    expect(book.all().length).toBe(3)
  })
})
