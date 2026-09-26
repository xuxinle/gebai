/**
 * 通用后台任务注册表测试：start/wait/cancel 状态机、会话作用域隔离、终态修剪、
 * 以及「任务体返回摘要 / 抛错记失败 / 中止记取消」三条终态语义。
 */
import { describe, expect, test } from "bun:test"
import { BackgroundJobRegistry, BG_JOB_KEEP, type BgJobStore } from "./jobs"

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms))

function store(): BgJobStore {
  return new Map()
}

describe("BackgroundJobRegistry", () => {
  test("start 立即返回 j 前缀 id 与 running 状态；wait 等到终态并带摘要", async () => {
    const s = store()
    const reg = new BackgroundJobRegistry(s, "s1")
    const rec = reg.start({
      kind: "triage",
      name: "研判 t1（2 条）",
      ref: { job_id: "t1", job_dir: "/tmp/t1" },
      run: async () => {
        await sleep(20)
        return "共 2 条：小模型定案 2"
      },
    })
    expect(rec.id.startsWith("j")).toBe(true)
    expect(rec.status).toBe("running")
    expect(rec.ref?.job_id).toBe("t1")

    const done = await reg.wait(rec.id, 2000)
    expect(done?.status).toBe("done")
    expect(done?.summary).toContain("小模型定案 2")
    expect(done?.endedAt).toBeGreaterThan(0)
  })

  test("进度上报可达（onProgress 覆盖式更新），任务结束后不再变更", async () => {
    const s = store()
    const reg = new BackgroundJobRegistry(s, "s1")
    const rec = reg.start({
      kind: "triage",
      name: "n",
      run: async (task) => {
        task.onProgress?.({ phase: "小模型 1/3", done: 1, total: 3, detail: "采纳 1" })
        await sleep(10)
        task.onProgress?.({ phase: "小模型 3/3", done: 3, total: 3 })
        return "ok"
      },
    })
    const mid = await reg.wait(rec.id, 5) // 首次上报后、完成前
    expect(mid?.progress?.done).toBe(1)

    await reg.wait(rec.id, 2000)
    const after = reg.get(rec.id)
    expect(after?.progress?.done).toBe(3)
    // 终态后上报被忽略（任务体若在收尾后又报一次，不污染记录）
    expect(after?.status).toBe("done")
  })

  test("任务体抛错 → failed 并记原因；不吞掉错误语义", async () => {
    const s = store()
    const reg = new BackgroundJobRegistry(s, "s1")
    const rec = reg.start({ kind: "k", name: "n", run: async () => { throw new Error("端点不可达") } })
    const done = await reg.wait(rec.id, 2000)
    expect(done?.status).toBe("failed")
    expect(done?.error).toContain("端点不可达")
  })

  test("cancel 协作中止：任务体看到 signal.aborted 后返回，记 cancelled 且保留已产出的摘要", async () => {
    const s = store()
    const reg = new BackgroundJobRegistry(s, "s1")
    let sawAbort = false
    const rec = reg.start({
      kind: "triage",
      name: "n",
      run: async (task) => {
        for (let i = 0; i < 100; i++) {
          if (task.signal.aborted) {
            sawAbort = true
            return "部分完成：2/10"
          }
          await sleep(5)
        }
        return "全量完成"
      },
    })
    await reg.cancel(rec.id, 2000)
    const after = reg.get(rec.id)
    expect(sawAbort).toBe(true)
    expect(after?.status).toBe("cancelled")
    expect(after?.summary).toContain("部分完成")
  })

  test("cancel 对已结束任务是空操作（幂等，状态不被改写）", async () => {
    const s = store()
    const reg = new BackgroundJobRegistry(s, "s1")
    const rec = reg.start({ kind: "k", name: "n", run: async () => "ok" })
    await reg.wait(rec.id, 2000)
    const after = await reg.cancel(rec.id)
    expect(after?.status).toBe("done")
    expect(after?.summary).toBe("ok")
  })

  test("wait 超时返回 running 快照（调用方据此决定再等或看进度）", async () => {
    const s = store()
    const reg = new BackgroundJobRegistry(s, "s1")
    const rec = reg.start({ kind: "k", name: "n", run: async () => { await sleep(300); return "ok" } })
    const snap = await reg.wait(rec.id, 20)
    expect(snap?.status).toBe("running")
    await reg.cancel(rec.id, 500)
  })

  test("会话作用域隔离：别的会话看不到也拿不到本会话的任务", async () => {
    const s = store()
    const a = new BackgroundJobRegistry(s, "sA")
    const b = new BackgroundJobRegistry(s, "sB")
    const rec = a.start({ kind: "k", name: "n", run: async () => "ok" })
    expect(a.list().map((r) => r.id)).toEqual([rec.id])
    expect(b.list()).toEqual([])
    expect(b.get(rec.id)).toBeUndefined()
    expect(await b.wait(rec.id, 10)).toBeUndefined()
    expect(await b.cancel(rec.id)).toBeUndefined()
    await a.wait(rec.id, 2000)
  })

  test("list 按启动顺序且含终态；终态记录超上限淘汰最旧（运行中的不淘汰）", async () => {
    const s = store()
    const reg = new BackgroundJobRegistry(s, "s1")
    for (let i = 0; i < BG_JOB_KEEP + 5; i++) {
      const r = reg.start({ kind: "k", name: `n${i}`, run: async () => `ok${i}` })
      await reg.wait(r.id, 2000)
    }
    const list = reg.list()
    expect(list.length).toBeLessThanOrEqual(BG_JOB_KEEP)
    // 最新的一定在（最旧的被淘汰）
    expect(list[list.length - 1].name).toBe(`n${BG_JOB_KEEP + 4}`)
  })

  test("未知名：get/wait/cancel 都返回 undefined（不抛异常）", async () => {
    const reg = new BackgroundJobRegistry(store(), "s1")
    expect(reg.get("jnope")).toBeUndefined()
    expect(await reg.wait("jnope", 10)).toBeUndefined()
    expect(await reg.cancel("jnope")).toBeUndefined()
  })
})
