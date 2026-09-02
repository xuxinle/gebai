import { describe, expect, test } from "bun:test"
import { SessionRunRegistry, SESSION_RUN_MAX_CONCURRENT, SESSION_RUN_KEEP, type SessionRunHandle, type SessionRunRunner } from "./session-runs"
import type { SessionRunArchive } from "@gebai/sdk"

/** 构造共享 store + 指定 runner 的注册表视图（validate 缺省原样返回）。 */
function makeRegistry(opts: { store?: Map<string, SessionRunHandle>; runner: SessionRunRunner; validate?: (agents: string[]) => string[]; parentSignal?: AbortSignal; sessionId?: string }) {
  return new SessionRunRegistry({
    sessionId: opts.sessionId ?? "s1",
    store: opts.store ?? new Map(),
    runner: opts.runner,
    validate: opts.validate ?? ((agents) => agents),
    parentSignal: opts.parentSignal,
  })
}

const archiveOf = (runId: string): SessionRunArchive => ({ runId, agents: ["code"], input: "job", output: "", messages: [{ role: "user", content: "job" }] })

describe("SessionRunRegistry（agent_run 异步后台运行）", () => {
  test("start 立即返回 running 记录；validate 抛错（未知子Agent 等）直接失败且不留句柄", async () => {
    const store = new Map<string, SessionRunHandle>()
    let started = 0
    const reg = makeRegistry({
      store,
      runner: async (_agents, _input, _signal) => {
        started++
        await new Promise((r) => setTimeout(r, 10))
        return { output: "ok", archive: archiveOf("x") }
      },
      validate: (agents) => {
        if (agents.includes("ghost")) throw new Error("未知子Agent: ghost")
        return agents
      },
    })
    const rec = await reg.start(["code"], "job")
    expect(rec.status).toBe("running")
    expect(rec.runId).toMatch(/^r[0-9a-f]+$/)
    expect(started).toBe(1)
    await expect(reg.start(["ghost"], "job")).rejects.toThrow("未知子Agent: ghost")
    expect(store.size).toBe(1) // 失败启动不登记
    await reg.wait(rec.runId, 1000)
    expect(reg.get(rec.runId)?.status).toBe("done")
  })

  test("进度快照随存档实时推导（rounds/toolCalls/last）；终态 result 返回最终输出与存档", async () => {
    let archive: SessionRunArchive | undefined
    let release: () => void = () => {}
    const gate = new Promise<void>((r) => (release = r))
    const reg = makeRegistry({
      runner: async (_agents, _input, _signal, opts) => {
        archive = archiveOf("x")
        opts?.onArchive?.(archive)
        await gate
        archive.output = "final text"
        return { output: "final text", archive }
      },
    })
    const rec = await reg.start(["code"], "job")
    // 执行中：存档推进 → 快照实时反映轮次/工具调用/最近活动
    archive!.messages.push({ role: "assistant", content: "thinking about it" })
    archive!.messages.push({ role: "tool", content: "已写入 x（10 字符）" })
    let snap = reg.get(rec.runId)!
    expect(snap.rounds).toBe(1)
    expect(snap.toolCalls).toBe(1)
    expect(snap.last).toContain("已写入 x")
    expect(snap.status).toBe("running")
    // 运行中 result 不可取
    expect(reg.result(rec.runId)).toBeUndefined()
    release()
    const done = await reg.wait(rec.runId, 1000)
    expect(done!.status).toBe("done")
    expect(done!.output).toBe("final text")
    const res = reg.result(rec.runId)!
    expect(res.output).toBe("final text")
    expect(res.archive.messages).toHaveLength(3)
  })

  test("cancel 主动终止：中止信号传播进执行循环，状态落定 cancelled（存档保留）", async () => {
    let aborted = false
    const reg = makeRegistry({
      runner: async (_agents, _input, signal, opts) => {
        opts?.onArchive?.(archiveOf("x"))
        await new Promise<void>((_, reject) => {
          signal.addEventListener("abort", () => {
            aborted = true
            reject(signal.reason instanceof Error ? signal.reason : new Error("cancelled"))
          }, { once: true })
        })
        return { output: "", archive: archiveOf("x") }
      },
    })
    const rec = await reg.start(["code"], "job")
    const cancelled = await reg.cancel(rec.runId)
    expect(aborted).toBe(true)
    expect(cancelled!.status).toBe("cancelled")
    expect(cancelled!.error).toContain("bg_task stop")
    expect(reg.result(rec.runId)?.archive).toBeTruthy()
    // 已结束再 cancel：原样返回快照（幂等）
    expect((await reg.cancel(rec.runId))!.status).toBe("cancelled")
  })

  test("父任务取消信号传播：发起任务停止连带终止后台运行", async () => {
    const parent = new AbortController()
    let sawAbort = false
    const reg = makeRegistry({
      parentSignal: parent.signal,
      runner: async (_agents, _input, signal, opts) => {
        opts?.onArchive?.(archiveOf("x"))
        await new Promise<void>((_, reject) => {
          signal.addEventListener("abort", () => {
            sawAbort = true
            reject(signal.reason ?? new Error("cancelled"))
          }, { once: true })
        })
        return { output: "", archive: archiveOf("x") }
      },
    })
    const rec = await reg.start(["code"], "job")
    parent.abort(new Error("用户停止"))
    await reg.wait(rec.runId, 1000)
    expect(sawAbort).toBe(true)
    expect(reg.get(rec.runId)!.status).toBe("cancelled")
  })

  test("并发上限与会话隔离：超限拒绝新运行；其他会话的运行不可见", async () => {
    const store = new Map<string, SessionRunHandle>()
    let release: () => void = () => {}
    const gate = new Promise<void>((r) => (release = r))
    const gatedRunner: SessionRunRunner = async (_agents, _input, _signal, opts) => {
      opts?.onArchive?.(archiveOf("x"))
      await gate
      return { output: "ok", archive: archiveOf("x") }
    }
    const reg = makeRegistry({ store, runner: gatedRunner })
    const other = makeRegistry({ store, runner: gatedRunner, sessionId: "s2" })
    for (let i = 0; i < SESSION_RUN_MAX_CONCURRENT; i++) await reg.start(["code"], `job ${i}`)
    await expect(reg.start(["code"], "one more")).rejects.toThrow("并发后台运行超限")
    // 会话隔离：s2 视图看不到 s1 的运行
    expect(other.list()).toHaveLength(0)
    expect(reg.list()).toHaveLength(SESSION_RUN_MAX_CONCURRENT)
    release()
  })

  test("终态记录修剪：超出保留上限淘汰最旧，运行中不淘汰", async () => {
    const store = new Map<string, SessionRunHandle>()
    const reg = makeRegistry({
      store,
      runner: async () => ({ output: "ok", archive: archiveOf("x") }),
    })
    for (let i = 0; i < SESSION_RUN_KEEP + 5; i++) {
      const rec = await reg.start(["code"], `job ${i}`)
      await reg.wait(rec.runId, 1000)
    }
    expect(reg.list().length).toBe(SESSION_RUN_KEEP)
  })

  test("wait 超时返回运行中快照；未知 runId 返回 undefined", async () => {
    let release: () => void = () => {}
    const gate = new Promise<void>((r) => (release = r))
    const reg = makeRegistry({
      runner: async (_a, _i, _s, opts) => {
        opts?.onArchive?.(archiveOf("x"))
        await gate
        return { output: "ok", archive: archiveOf("x") }
      },
    })
    const rec = await reg.start(["code"], "job")
    const timedOut = await reg.wait(rec.runId, 30)
    expect(timedOut!.status).toBe("running")
    expect(reg.wait("r-nope", 30)).resolves.toBeUndefined()
    expect(reg.get("r-nope")).toBeUndefined()
    release()
  })


})
