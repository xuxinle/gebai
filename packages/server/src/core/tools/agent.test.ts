/**
 * bg_task 工具的通用后台任务分支（id 前缀 `j`）测试：三类任务同构管理面中的第三类。
 *
 * 关注点：状态/进度呈现、wait 等到终态取摘要、stop 协作中止、finish 明确拒绝（它不是模型会话）、
 * list 把三类合并列出、以及未知名与缺服务的可读报错。
 */
import { describe, expect, test } from "bun:test"
import type { ToolContext } from "../base/types"
import { BackgroundJobRegistry, type BgJobStore } from "../session/jobs"
import { bgTaskTool } from "./agent"

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms))

function ctxWithJobs(store: BgJobStore, sessionId = "s1"): ToolContext {
  return { user: "u", sessionId, home: "/tmp", workdir: "/tmp", env: {}, sandboxed: false, bgJobs: new BackgroundJobRegistry(store, sessionId) } as unknown as ToolContext
}

describe("bg_task：通用后台任务（j 前缀）", () => {
  test("status：报状态与进度（阶段/已完成/总数/说明）与产物引用", async () => {
    const store: BgJobStore = new Map()
    const ctx = ctxWithJobs(store)
    const rec = ctx.bgJobs!.start({
      kind: "triage",
      name: "研判 t1（3 条）",
      ref: { job_id: "t1", job_dir: "/tmp/t1" },
      run: async (task) => {
        task.onProgress?.({ phase: "小模型 1/3", done: 1, total: 3, detail: "采纳 1、待审 0、失败 0" })
        await sleep(150)
        return "共 3 条：小模型定案 3"
      },
    })
    await sleep(20)
    const r = await bgTaskTool.execute({ action: "status", id: rec.id }, ctx)
    expect(r.output).toContain("jobId")
    expect(r.output).toContain("[running]")
    expect(r.output).toContain("小模型 1/3")
    expect(r.output).toContain("采纳 1")
    expect(r.output).toContain("/tmp/t1")
    const d = r.data as Record<string, unknown>
    expect(d.kind).toBe("job")
    expect(d.ref).toEqual({ job_id: "t1", job_dir: "/tmp/t1" })
    await ctx.bgJobs!.wait(rec.id, 2000)
  })

  test("wait：等到终态给出摘要；done 后 status 也带摘要", async () => {
    const store: BgJobStore = new Map()
    const ctx = ctxWithJobs(store)
    const rec = ctx.bgJobs!.start({ kind: "triage", name: "n", run: async () => { await sleep(20); return "共 2 条：兜底定案 1" } })
    const w = await bgTaskTool.execute({ action: "wait", id: rec.id, timeout: 5 }, ctx)
    expect(w.output).toContain("[done]")
    expect(w.output).toContain("兜底定案 1")
    const s = await bgTaskTool.execute({ action: "status", id: rec.id }, ctx)
    expect(s.output).toContain("兜底定案 1")
  })

  test("wait 超时：返回 running 与「可再次 wait」提示", async () => {
    const store: BgJobStore = new Map()
    const ctx = ctxWithJobs(store)
    const rec = ctx.bgJobs!.start({ kind: "triage", name: "n", run: async () => { await sleep(3000); return "ok" } })
    const w = await bgTaskTool.execute({ action: "wait", id: rec.id, timeout: 1 }, ctx)
    expect(w.output).toContain("等待超时仍在运行")
    expect((w.data as Record<string, unknown>).status).toBe("running")
    await ctx.bgJobs!.cancel(rec.id, 1000)
  })

  test("stop：协作中止，提示已落盘结果保留在产物目录", async () => {
    const store: BgJobStore = new Map()
    const ctx = ctxWithJobs(store)
    const rec = ctx.bgJobs!.start({
      kind: "triage",
      name: "n",
      ref: { job_id: "j1", job_dir: "/tmp/j1" },
      run: async (task) => {
        for (let i = 0; i < 200; i++) {
          if (task.signal.aborted) return "部分完成：2/10"
          await sleep(5)
        }
        return "全量完成"
      },
    })
    const r = await bgTaskTool.execute({ action: "stop", id: rec.id }, ctx)
    expect(r.output).toContain("[cancelled]")
    expect(r.output).toContain("/tmp/j1")
    expect(r.output).toContain("部分完成")
  })

  test("finish：明确拒绝并指向 stop（后台任务不是模型会话，没有可收敛的对话）", async () => {
    const store: BgJobStore = new Map()
    const ctx = ctxWithJobs(store)
    const rec = ctx.bgJobs!.start({ kind: "triage", name: "n", run: async () => { await sleep(50); return "ok" } })
    const r = await bgTaskTool.execute({ action: "finish", id: rec.id }, ctx)
    expect(r.output).toContain("不支持快速结束")
    expect(r.output).toContain("action=stop")
    await ctx.bgJobs!.cancel(rec.id, 1000)
  })

  test("list：三类任务同一清单（t / s / j 前缀）", async () => {
    const store: BgJobStore = new Map()
    const bgJobs = new BackgroundJobRegistry(store, "s1")
    const rec = bgJobs.start({ kind: "triage", name: "研判", run: async () => { await sleep(30); return "ok" } })
    const ctx = {
      user: "u",
      sessionId: "s1",
      home: "/tmp",
      workdir: "/tmp",
      env: {},
      sandboxed: false,
      bgJobs,
      shTasks: { list: async () => [{ id: "t1", command: "echo hi", startedAt: 1, pid: 1, cwd: "/tmp", maxMs: 1 }], refresh: async () => undefined, wait: async () => undefined, kill: async () => undefined, readLog: async () => "" },
      subSessions: { list: () => [{ runId: "s1a", sessionId: "s1", name: "子会话", input: "x", agents: [], envMode: "inherit", envKeys: [], inheritContext: false, async: true, depth: 1, startedAt: 2, status: "running", finishing: false, rounds: 0, toolCalls: 0, merged: false }], get: () => undefined, wait: async () => undefined, cancel: async () => undefined, finish: () => undefined, result: () => undefined, start: async () => [] },
    } as unknown as ToolContext
    const r = await bgTaskTool.execute({ action: "list" }, ctx)
    expect(r.output).toContain("t1")
    expect(r.output).toContain("s1a")
    expect(r.output).toContain(rec.id)
    const kinds = (r.data as { tasks: Array<{ kind: string }> }).tasks.map((t) => t.kind).sort()
    expect(kinds).toEqual(["job", "sh", "subsession"])
    await bgJobs.cancel(rec.id, 1000)
  })

  test("未知名与缺服务：都给可操作报错（不抛异常）", async () => {
    const store: BgJobStore = new Map()
    const ctx = ctxWithJobs(store)
    const miss = await bgTaskTool.execute({ action: "status", id: "jnope" }, ctx)
    expect(miss.output).toContain("未找到后台任务")

    const noSvc = { ...ctx, bgJobs: undefined } as unknown as ToolContext
    const r = await bgTaskTool.execute({ action: "status", id: "jabc12345" }, noSvc)
    expect(r.output).toContain("不支持通用后台任务")
  })

  test("会话作用域：本会话看不到别的会话（作用域）启的同类任务", async () => {
    const store: BgJobStore = new Map()
    const other = new BackgroundJobRegistry(store, "api:u")
    const rec = other.start({ kind: "triage", name: "REST 批次", run: async () => { await sleep(50); return "ok" } })
    const ctx = ctxWithJobs(store, "s1")
    const r = await bgTaskTool.execute({ action: "status", id: rec.id }, ctx)
    expect(r.output).toContain("未找到后台任务")
    await other.cancel(rec.id, 1000)
  })
})
