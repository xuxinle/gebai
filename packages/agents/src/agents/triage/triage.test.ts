import { afterAll, describe, expect, test } from "bun:test"
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { def, description, name, preload, requiresApproval, systemPrompt, tools } from "./triage"
import { TRIAGE_PARAM_NAMES } from "../../core/triage"

const dirs: string[] = []
function tempHome(): string {
  const d = mkdtempSync(join(tmpdir(), "triage-agent-"))
  dirs.push(d)
  return d
}
afterAll(() => {
  for (const d of dirs) rmSync(d, { recursive: true, force: true })
})

/** 只提供 view 工具需要的字段（其余从简）。 */
function ctxOf(home: string): ToolContext {
  return { user: "tester", home, workdir: home, sessionId: "s1", env: {}, sandboxed: false } as unknown as ToolContext
}

/** L1 stub 端点（工具层的 L1 调用无法注入 fetchImpl，只能起真端点）。 */
function startL1Stub(): { port: number; stop: () => void } {
  const server = Bun.serve({
    port: 0,
    async fetch(req) {
      if (!req.url.endsWith("/v1/chat/completions")) return new Response("not found", { status: 404 })
      const body = (await req.json()) as { messages?: Array<{ content?: string }> }
      const text = (body.messages ?? []).map((m) => m.content ?? "").join("\n")
      const args = text.includes("【条目】weak")
        ? { result: { label: "未知" }, confidence: 0.2, reason: "信息不足", evidence_index: [] }
        : { result: { label: "OOM" }, confidence: 0.95, reason: "日志含 OutOfMemory", evidence_index: ["CUDA out of memory"] }
      return Response.json({
        choices: [
          {
            message: {
              role: "assistant",
              content: "",
              tool_calls: [{ id: "c1", type: "function", function: { name: "submit_result", arguments: JSON.stringify(args) } }],
            },
            finish_reason: "tool_calls",
          },
        ],
        usage: { prompt_tokens: 30, completion_tokens: 12 },
      })
    },
  })
  return { port: server.port ?? 0, stop: () => server.stop(true) }
}

/** 子会话服务替身：可控制 wait 返回几次 running，以及最终是否产出结论。 */
function subSessionsStub(opts: { runningTimes?: number; output?: string; finalStatus?: string }) {
  const state = { waitCalls: 0, started: 0 }
  const svc = {
    async start(specs: Array<Record<string, unknown>>) {
      state.started += specs.length
      return specs.map((_s, i) => ({ runId: `s${i}`, status: "running" }))
    },
    async wait(_runId: string) {
      state.waitCalls++
      const remain = (opts.runningTimes ?? 0) - (state.waitCalls - 1)
      return { status: remain > 0 ? "running" : (opts.finalStatus ?? "done") }
    },
    result(_runId: string) {
      return opts.output ? { output: opts.output } : undefined
    },
  }
  return { svc, state }
}

describe("triage_run：L2 子会话等待策略", () => {
  const l1Schema = { type: "object", properties: { label: { type: "string" } }, required: ["label"] }

  test("wait 先返回 running 时继续等到终态，不把正要交付的结论当成失败", async () => {
    const home = tempHome()
    const l1 = startL1Stub()
    try {
      // 子会话超时后的「快速结束」宽限内仍报 running —— 必须继续等
      const { svc, state } = subSessionsStub({
        runningTimes: 2,
        output: JSON.stringify({
          results: [
            {
              id: "weak",
              result: { label: "网络问题" },
              confidence: 0.9,
              reason: "查到连接超时",
              evidence_chain: [{ tool: "grep", finding: "timeout" }],
              revised: true,
              action: "修正",
            },
          ],
        }),
      })
      const ctx = {
        ...ctxOf(home),
        subSessions: svc,
      } as unknown as ToolContext
      const r = await tools.run.execute(
        {
          items: [{ id: "weak", features: "只有一行报错" }],
          l1_target: `http://127.0.0.1:${l1.port}`,
          label_enum: ["OOM", "网络问题", "未知"],
          schema: l1Schema,
          job_id: "wait-loop",
        },
        ctx,
      )
      expect(state.waitCalls).toBeGreaterThanOrEqual(3) // 循环等待（前两次 running）
      expect(r.output).toContain("[L2]")
      expect((r.data as Record<string, unknown>).reviewed).toBe(1)
      expect((r.data as Record<string, unknown>).revised).toBe(1)
    } finally {
      l1.stop()
    }
  })

  test("快速结束阶段已产出结论时采用它（不死等终态）", async () => {
    const home = tempHome()
    const l1 = startL1Stub()
    try {
      const { svc, state } = subSessionsStub({
        runningTimes: 99, // 永不 done
        output: JSON.stringify({ results: [{ id: "weak", result: { label: "未知" }, confidence: 0.4, reason: "证据不足", action: "证据不足" }] }),
      })
      const r = await tools.run.execute(
        {
          items: [{ id: "weak", features: "只有一行报错" }],
          l1_target: `http://127.0.0.1:${l1.port}`,
          label_enum: ["未知"],
          schema: l1Schema,
          job_id: "finish-grace",
          l2_timeout_ms: 2000, // 小预算 → 等待窗口 = 2s + 宽限 + 30s，测试秒级完成
        },
        { ...ctxOf(home), subSessions: svc } as unknown as ToolContext,
      )
      expect(state.waitCalls).toBeGreaterThanOrEqual(1)
      expect((r.data as Record<string, unknown>).reviewed).toBe(1)
      expect(r.output).toContain("证据不足")
    } finally {
      l1.stop()
    }
  })
})

describe("triage_run：mode=async（通用后台任务）", () => {
  const l1Schema = { type: "object", properties: { label: { type: "string" } }, required: ["label"] }

  /** 后台任务服务替身：真实跑任务体并记录启动参数（工具侧只依赖 start）。 */
  function bgJobsStub() {
    const started: Array<{ kind: string; name: string; ref?: Record<string, string> }> = []
    const state: {
      outcome?: { status: string; summary?: string; error?: string }
      signal?: AbortSignal
      done?: Promise<void>
    } = {}
    const svc = {
      start(opts: {
        kind: string
        name: string
        ref?: Record<string, string>
        run: (task: { signal: AbortSignal; onProgress?: (p: { phase: string; done?: number; total?: number; detail?: string }) => void }) => Promise<string | undefined>
      }) {
        started.push({ kind: opts.kind, name: opts.name, ...(opts.ref ? { ref: opts.ref } : {}) })
        const controller = new AbortController()
        const progress: Array<{ phase: string }> = []
        state.signal = controller.signal
        state.done = opts.run({ signal: controller.signal, onProgress: (p) => progress.push(p) }).then(
          (summary) => void (state.outcome = { status: "done", summary }),
          (e) => void (state.outcome = { status: "failed", error: String((e as Error).message) }),
        )
        return { id: "jtest0001" }
      },
      outcome: () => state.outcome,
      signal: () => state.signal,
      waitDone: async () => void (await state.done),
    }
    return { svc, started }
  }

  test("无后台任务服务 → 可操作报错（提示改用 sync 或 REST）", async () => {
    const home = tempHome()
    const r = await tools.run.execute(
      { items: [{ id: "a", features: "f" }], mode: "async", escalate: false },
      ctxOf(home),
    )
    expect(r.output).toContain("ctx.bgJobs 不可用")
    expect(r.output).toContain("mode=sync")
  })

  test("异步：立即返回 jobId 与产物目录，后台跑完落盘结论（与同步同一份执行）", async () => {
    const home = tempHome()
    const l1 = startL1Stub()
    try {
      const { svc, started } = bgJobsStub()
      const ctx = { ...ctxOf(home), bgJobs: svc } as unknown as ToolContext
      const r = await tools.run.execute(
        {
          items: [{ id: "good", features: "命令 exit 1" }],
          target: `http://127.0.0.1:${l1.port}`,
          label_enum: ["OOM", "未知"],
          schema: l1Schema,
          job_id: "async-tool",
          escalate: false,
          mode: "async",
        },
        ctx,
      )
      // 立即返回：带后台任务 id、产物目录与 bg_task 管理指引
      expect(r.output).toContain("研判已后台启动")
      expect(r.output).toContain("jtest0001")
      expect(r.output).toContain("bg_task action=status")
      expect(r.output).toContain("triage_view")
      const d = r.data as Record<string, unknown>
      expect(d.bg_job_id).toBe("jtest0001")
      expect(d.state).toBe("running")
      expect(d.job_id).toBe("async-tool")
      expect(started[0].kind).toBe("triage")
      expect(started[0].ref?.job_id).toBe("async-tool")
      // 后台真实跑完并落盘（结论与同步路径同一份）
      await svc.waitDone()
      expect(svc.outcome()?.status).toBe("done")
      expect(svc.outcome()?.summary).toContain("小模型定案")
      const back = await tools.view.execute({ job_id: "async-tool", action: "results" }, ctx)
      expect(back.output).toContain("good")
    } finally {
      l1.stop()
    }
  })

  test("异步：中止信号传到管线，已跑完的部分结果仍落盘", async () => {
    const home = tempHome()
    const l1 = startL1Stub()
    try {
      const { svc } = bgJobsStub()
      const ctx = { ...ctxOf(home), bgJobs: svc } as unknown as ToolContext
      await tools.run.execute(
        {
          items: [
            { id: "good", features: "命令 exit 1" },
            { id: "second", features: "另一条" },
          ],
          target: `http://127.0.0.1:${l1.port}`,
          label_enum: ["OOM", "未知"],
          schema: l1Schema,
          job_id: "async-stop",
          escalate: false,
          mode: "async",
        },
        ctx,
      )
      svc.signal()?.dispatchEvent?.(new Event("abort"))
      await svc.waitDone()
      // 产物目录存在（哪怕中途中止，item 副本与已完成的 L1 记录都在）
      const back = await tools.view.execute({ job_id: "async-stop" }, ctx)
      expect(back.output).toContain("async-stop")
    } finally {
      l1.stop()
    }
  })

  test("兜底端点覆盖经子会话自定义环境变量生效（与 REST 通道能力一致）", async () => {
    const home = tempHome()
    const l1 = startL1Stub()
    try {
      const specs: Array<Record<string, unknown>> = []
      const subSessions = {
        async start(list: Array<Record<string, unknown>>) {
          specs.push(...list)
          return list.map((_s, i) => ({ runId: `s${i}`, status: "done" }))
        },
        async wait() {
          return { status: "done" }
        },
        result() {
          return { output: JSON.stringify({ results: [{ id: "weak", result: { label: "未知" }, confidence: 0.3, action: "证据不足" }] }) }
        },
      }
      const r = await tools.run.execute(
        {
          items: [{ id: "weak", features: "只有一行报错" }],
          target: `http://127.0.0.1:${l1.port}`,
          label_enum: ["未知"],
          schema: l1Schema,
          job_id: "env-override",
          l2_api_base: "http://127.0.0.1:19999",
          l2_api_key: "sk-test",
          l2_model: "big-model",
        },
        { ...ctxOf(home), subSessions } as unknown as ToolContext,
      )
      expect(r.output).not.toContain("不支持")
      const spec = specs.find((s) => s.merge === "summary")!
      expect(spec.env_mode).toBe("inherit")
      expect(spec.env).toEqual({ GEBAI_LLM_MODEL: "big-model", GEBAI_LLM_API_BASE: "http://127.0.0.1:19999", GEBAI_LLM_API_KEY: "sk-test" })
    } finally {
      l1.stop()
    }
  })
})

describe("triage 子代理契约", () => {
  test("标识与装载姿态", () => {
    expect(name).toBe("triage")
    expect(def.name).toBe("triage")
    expect(preload).toBe(false)
    expect(def.preload).toBe(false)
    expect(description).toContain("置信度")
    expect(systemPrompt.length).toBeGreaterThan(200)
    expect(def.systemPrompt).toBe(systemPrompt)
  })

  test("工具集不多不少：run + view", () => {
    expect(Object.keys(tools).sort()).toEqual(["run", "view"])
    expect(Object.keys(def.tools ?? {}).sort()).toEqual(["run", "view"])
  })

  test("免审批：两者都不改变服务状态", () => {
    expect(requiresApproval).toEqual({})
    expect(def.requiresApproval).toEqual({})
    expect(tools.run.safeMode).toBe(false) // 会发请求/派生会话
    expect(tools.view.safeMode).toBe(true) // 只读产物目录
  })

  test("run 的参数契约 = 共享参数表的规范名全集（与接口调用一致）", () => {
    const props = (tools.run.parameters.properties ?? {}) as Record<string, unknown>
    expect(Object.keys(props).sort()).toEqual([...TRIAGE_PARAM_NAMES].sort())
    // 用户可见的五个核心参数必须在内（且名字就是规范名）
    for (const k of ["items", "target", "schema", "threshold", "escalate"]) expect(props[k]).toBeDefined()
  })

  test("view 对不存在的任务给可读提示而不抛异常", async () => {
    const home = tempHome()
    const r = await tools.view.execute({ job_id: "nope" }, ctxOf(home))
    expect(r.output).toContain("未找到任务")
  })

  test("view 读得出任务汇总与逐条结果", async () => {
    const home = tempHome()
    const dir = join(home, "users", "tester", "triage", "t1")
    mkdirSync(dir, { recursive: true })
    writeFileSync(
      join(dir, "job.json"),
      JSON.stringify({
        job_id: "t1",
        state: "done",
        summary: { total: 2, adopted: 1, escalated: 1, reviewed: 1, pending_review: 0, failed: 0, revised: 1, notes: ["分流：1 条小模型采纳"] },
      }),
      "utf-8",
    )
    writeFileSync(
      join(dir, "results.jsonl"),
      [
        JSON.stringify({ id: "a", layer: "L1", ok: true, result: { label: "代码退出" }, confidence: 0.92 }),
        JSON.stringify({ id: "b", layer: "L2", ok: true, result: { label: "网络" }, confidence: 0.88, revised: true, action: "修正" }),
      ].join("\n") + "\n",
      "utf-8",
    )

    const status = await tools.view.execute({ job_id: "t1" }, ctxOf(home))
    expect(status.output).toContain("小模型定案 1")
    expect(status.output).toContain("修正 1")
    expect(status.output).toContain("分流：1 条小模型采纳")

    const results = await tools.view.execute({ job_id: "t1", action: "results" }, ctxOf(home))
    expect(results.output).toContain("[L1]")
    expect(results.output).toContain("代码退出")
    expect(results.output).toContain("（修正初判）")

    const failed = await tools.view.execute({ job_id: "t1", action: "failed" }, ctxOf(home))
    expect(failed.output).toContain("0 条中展示前 0 条")
  })
})
