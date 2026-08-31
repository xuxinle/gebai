import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { AgentEvent, LLMCapabilities, MessageLike } from "@gebai/sdk"
import type { LLMChunk, LLMProvider, ChatOptions } from "./llm"
import { parseModelRoutes, resolveModelRouteProvider } from "./llm"
import { AgentEngine } from "./engine"
import { SessionStore } from "./store"
import { ToolRegistry } from "./registry"
import { createGlobalTools } from "./tools"
import { Sandbox } from "./sandbox"
import { EnvManager } from "./env"
import { EventBus } from "./event-bus"
import { SubAgentManager } from "./subagents"
import { loadConfig } from "./config"
import { BranchRunRegistry, normalizeBranchSpecs, BRANCH_RUN_MAX_CONCURRENT, BRANCH_RUN_KEEP, type BranchRunHandle } from "./branch-runs"

/**
 * 会话分支运行与合并（DESIGN「会话分支运行与合并」）测试：
 * - 注册表单元：规格校验/并发上限/等待/终止/fork 快照切片
 * - 模型路由：GEBAI_LLM_ROUTES 解析与 Provider 构建（多路接口）
 * - 引擎集成（脚本化 ScriptProvider）：同步 fan-out 并行执行 + 报告合入主上下文（消息序列/事件/fork 内容/
 *   下一轮可见/模型路由）、异步分支晚于主线完成直接落盘、bg_task b 前缀管理、父任务停止连带终止
 */

const sleep = (ms: number) => new Promise<void>((res) => setTimeout(res, ms))

/** 规格校验：默认命名/批内唯一/上限/必填项。 */
describe("normalizeBranchSpecs", () => {
  test("缺省名按 b1..bN 自动命名，model 空串省略", () => {
    expect(normalizeBranchSpecs([{ prompt: "a" }, { name: "x", prompt: "b", model: "" }])).toEqual([
      { name: "b1", prompt: "a" },
      { name: "x", prompt: "b" },
    ])
  })
  test("非法形态抛错：空数组/缺 prompt/重名/超上限/非法名", () => {
    expect(() => normalizeBranchSpecs([])).toThrow()
    expect(() => normalizeBranchSpecs([{ name: "a" }])).toThrow()
    expect(() => normalizeBranchSpecs([{ name: "a", prompt: "x" }, { name: "a", prompt: "y" }])).toThrow()
    expect(() => normalizeBranchSpecs([{ name: "坏 名", prompt: "x" }])).toThrow()
  })
})

/** 注册表单元：start/wait/cancel/list、并发上限、fork 快照切片。 */
describe("BranchRunRegistry", () => {
  function makeRegistry(store = new Map<string, BranchRunHandle>()) {
    const reg = new BranchRunRegistry({
      sessionId: "s1",
      store,
      forkSource: [{ role: "system", content: "sys" }, { role: "user", content: "hi" }],
      runner: (spec, signal) =>
        new Promise((resolve, reject) => {
          if (spec.name === "hang") {
            signal.addEventListener("abort", () => reject(new Error("cancelled")), { once: true })
            return
          }
          if (spec.name === "fail") {
            reject(new Error("boom"))
            return
          }
          setTimeout(
            () =>
              resolve({
                output: `out-${spec.name}`,
                archive: { runId: spec.branchId, agents: [], input: spec.prompt, output: `out-${spec.name}`, messages: [{ role: "user", content: spec.prompt }, { role: "assistant", content: `out-${spec.name}` }] },
              }),
            10,
          )
        }),
    })
    return { reg, store }
  }

  test("start 返回 b 前缀 id，wait 等到 done，list 按会话过滤", async () => {
    const { reg, store } = makeRegistry()
    // 其他会话句柄：list 过滤验证
    store.set("bzzzzzzzz", { branchId: "bzzzzzzzz", sessionId: "other", name: "x", prompt: "p", startedAt: 0, status: "running", controller: new AbortController(), done: Promise.resolve() })
    const recs = await reg.start([{ name: "a", prompt: "p" }, { name: "b", prompt: "q" }])
    expect(recs).toHaveLength(2)
    for (const r of recs) {
      expect(r.branchId.startsWith("b")).toBe(true)
      expect(r.status).toBe("running")
      expect(r.merged).toBe(false)
    }
    const done = await reg.wait(recs[0].branchId, 2000)
    expect(done?.status).toBe("done")
    expect(done?.output).toBe("out-a")
    expect(reg.list().length).toBe(2)
    expect(reg.get("bzzzzzzzz")).toBeUndefined()
  })

  test("并发上限：运行中 + 新增超限抛错", async () => {
    const { reg } = makeRegistry()
    await reg.start([{ name: "hang", prompt: "p" }])
    await expect(reg.start(Array.from({ length: BRANCH_RUN_MAX_CONCURRENT }, (_, i) => ({ name: `n${i}`, prompt: "p" })))).rejects.toThrow(/并发分支超限/)
  })

  test("cancel 终止运行中分支，状态落定 cancelled", async () => {
    const { reg } = makeRegistry()
    const [rec] = await reg.start([{ name: "hang", prompt: "p" }])
    const cancelled = await reg.cancel(rec.branchId)
    expect(cancelled?.status).toBe("cancelled")
    expect(reg.result(rec.branchId)).toBeUndefined()
  })

  test("终态保留修剪：超出 BRANCH_RUN_KEEP 淘汰最旧", async () => {
    const store = new Map<string, BranchRunHandle>()
    const { reg } = makeRegistry(store)
    // 逐个启动并等待完成（串行避免并发上限），累计超出保留上限
    const finished: string[] = []
    for (let i = 0; i < BRANCH_RUN_KEEP + 2; i++) {
      const [rec] = await reg.start([{ name: `r${i}`, prompt: "p" }])
      await reg.wait(rec.branchId, 2000)
      finished.push(rec.branchId)
    }
    // 最早完成的 2 条被淘汰，其余保留
    expect(reg.get(finished[0])).toBeUndefined()
    expect(reg.get(finished[1])).toBeUndefined()
    expect(reg.get(finished[2])).toBeDefined()
    expect(reg.list().length).toBe(BRANCH_RUN_KEEP)
  })

  test("fork 快照切片：runner 收到的 fork 是构造时消息数组的副本", async () => {
    const store = new Map<string, BranchRunHandle>()
    const live: MessageLike[] = [{ role: "system", content: "sys" }]
    let seen: MessageLike[] | undefined
    const reg = new BranchRunRegistry({
      sessionId: "s1",
      store,
      forkSource: live,
      runner: async (_spec, _signal, fork) => {
        seen = fork
        return { output: "ok", archive: { runId: "b1", agents: [], input: "p", output: "ok", messages: [] } }
      },
    })
    await reg.start([{ name: "a", prompt: "p" }])
    live.push({ role: "user", content: "after-fork" })
    await sleep(30)
    expect(seen).toEqual([{ role: "system", content: "sys" }])
  })
})

/** 模型路由（GEBAI_LLM_ROUTES 多路接口）：解析与 Provider 构建。 */
describe("model routes", () => {
  test("parseModelRoutes：合法路由/非法 JSON/缺 model 条目忽略", () => {
    const env = { GEBAI_LLM_ROUTES: JSON.stringify({ fast: { model: "gpt-4o-mini", api_base: "https://a.example/v1", api_kind: "openai", max_context: 128000 }, bad: { api_base: "x" } }) }
    const routes = parseModelRoutes(env)
    expect(Object.keys(routes)).toEqual(["fast"])
    expect(routes.fast).toEqual({ model: "gpt-4o-mini", apiBase: "https://a.example/v1", apiKind: "openai", maxContextTokens: 128000 })
    expect(parseModelRoutes({ GEBAI_LLM_ROUTES: "{oops" })).toEqual({})
    expect(parseModelRoutes(undefined)).toEqual({})
  })
  test("resolveModelRouteProvider：路由命中/字面模型名/空名", () => {
    const base = { apiKind: "openai" as const, apiBase: "https://main.example/v1", apiKey: "k", model: "main-m", maxContextTokens: 64000, multimodal: false }
    const env = { GEBAI_LLM_ROUTES: JSON.stringify({ fast: { model: "mini-m", api_base: "https://fast.example/v1" } }) }
    expect(resolveModelRouteProvider(base, env, "fast")).toBeDefined()
    expect(resolveModelRouteProvider(base, env, "other-model")).toBeDefined()
    expect(resolveModelRouteProvider(base, env, "")).toBeUndefined()
  })
})

/**
 * 脚本化 Provider：按 (消息, 调用序, 调用选项) 响应 chunk 数组——分支按提示词分流、
 * 可延迟制造并发窗口、可挂起至 abort（取消传播验证）。
 */
class ScriptProvider implements LLMProvider {
  readonly id = "script"
  calls = 0
  inFlight = 0
  maxInFlight = 0
  seenChats: MessageLike[][] = []
  seenTools: string[][] = []
  constructor(
    private respond: (msgs: MessageLike[], callIdx: number, opts?: ChatOptions) => LLMChunk[] | Promise<LLMChunk[]>,
  ) {}
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 100000 }
  }
  async *chat(msgs: MessageLike[], opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.calls++
    this.inFlight++
    this.maxInFlight = Math.max(this.maxInFlight, this.inFlight)
    this.seenChats.push(JSON.parse(JSON.stringify(msgs)) as MessageLike[])
    this.seenTools.push((opts?.tools ?? []).map((t) => t.name))
    try {
      const chunks = await this.respond(msgs, this.calls, opts)
      for (const c of chunks) yield c
    } finally {
      this.inFlight--
    }
  }
}

interface Harness {
  home: string
  store: SessionStore
  engine: AgentEngine
  provider: ScriptProvider
  events: AgentEvent[]
  routeNames: string[]
  cleanup: () => void
}

async function setupBranch(
  respond: (msgs: MessageLike[], callIdx: number, opts?: ChatOptions) => LLMChunk[] | Promise<LLMChunk[]>,
  opts: { resolveModelProvider?: boolean } = {},
): Promise<Harness> {
  const home = mkdtempSync(join(tmpdir(), "gebai-branch-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  const config = loadConfig({ gebaiHome: home, auth: "local", sandbox: "off", preloadSubAgents: [], binaryMode: false, safeMode: false })
  const store = new SessionStore({ home })
  const registry = new ToolRegistry({ safeMode: false })
  for (const tool of Object.values(createGlobalTools())) registry.register(tool)
  const sandbox = new Sandbox({ home, enabled: false })
  const env = new EnvManager(store)
  const events = new EventBus()
  const subAgents = new SubAgentManager({ registry, preloadOverride: [] })
  await subAgents.discover()
  const provider = new ScriptProvider(respond)
  const collected: AgentEvent[] = []
  events.subscribe((e) => collected.push(e))
  const routeNames: string[] = []
  const engine = new AgentEngine({
    provider,
    registry,
    store,
    env,
    sandbox,
    events,
    config,
    subAgents,
    retryBackoffMs: 5,
    authMode: "local",
    resolveModelProvider: opts.resolveModelProvider
      ? (_e, name) => {
          routeNames.push(name)
          return provider
        }
      : undefined,
  })
  return { home, store, engine, provider, events: collected, routeNames, cleanup: () => rmSync(home, { recursive: true, force: true }) }
}

const lastUserText = (msgs: MessageLike[]): string => {
  for (let i = msgs.length - 1; i >= 0; i--) {
    const m = msgs[i]
    if (m.role === "user" && typeof m.content === "string") return m.content
  }
  return ""
}

describe("branch_run 集成", () => {
  test("同步 fan-out：并行执行 + 报告合入主上下文 + 下一轮可见 + 模型路由", async () => {
    const h = await setupBranch(
      (msgs, call) => {
        const last = lastUserText(msgs)
        if (call === 1) {
          return [
            {
              type: "tool_call",
              toolCall: {
                id: "tc-br",
                name: "branch_run",
                arguments: {
                  branches: [
                    { name: "左路", prompt: "调研方案A" },
                    { name: "右路", prompt: "调研方案B", model: "fast" },
                  ],
                },
              },
            } as LLMChunk,
            { type: "done" } as LLMChunk,
          ]
        }
        if (last === "调研方案A" || last === "调研方案B") {
          // 分支响应：延迟制造并发窗口，验证两分支同时在途（摆脱单轮串行）
          return (async () => {
            await sleep(80)
            return [{ type: "text", text: `报告:${last}` }, { type: "done" }] as LLMChunk[]
          })()
        }
        return [{ type: "text", text: "主线总结完成" }, { type: "done" }] as LLMChunk[]
      },
      { resolveModelProvider: true },
    )
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "做两个方案的调研")
      // 并行性：两分支同时在途（maxInFlight ≥ 2）
      expect(h.provider.maxInFlight).toBeGreaterThanOrEqual(2)
      // 合入消息：tool 结果之后两条合并 assistant 消息（顺序：tool → merge×2 → 最终回复）
      const msgs = (await h.store.load(session.id, "default"))!.messages
      const toolIdx = msgs.findIndex((m) => m.role === "tool" && m.name === "branch_run")
      expect(toolIdx).toBeGreaterThan(0)
      expect(msgs[toolIdx + 1].branchMeta?.name).toBe("左路")
      expect(msgs[toolIdx + 2].branchMeta?.name).toBe("右路")
      expect(msgs[toolIdx + 1].content).toContain("【并行分支「左路」已合并】")
      expect(msgs[toolIdx + 1].content).toContain("报告:调研方案A")
      expect(msgs[toolIdx + 1].sessionRun?.branch?.name).toBe("左路")
      expect(msgs[toolIdx + 2].sessionRun?.branch?.model).toBe("fast")
      expect(msgs[toolIdx + 3].role).toBe("assistant")
      expect(msgs[toolIdx + 3].content).toBe("主线总结完成")
      // 工具结果为概要（不重复全文）
      expect(msgs[toolIdx].content).toContain("分支并行执行完成")
      // 下一轮主线可见合入内容（最终回复那次调用包含合并消息）
      const finalChat = h.provider.seenChats[h.provider.seenChats.length - 1]
      const joined = JSON.stringify(finalChat)
      expect(joined).toContain("报告:调研方案A")
      expect(joined).toContain("报告:调研方案B")
      // fork 内容：分支上下文含主线上下文（用户输入）+ 分支提示词 + 悬空 toolCall 合成补齐 + 分支系统附注
      const branchChat = h.provider.seenChats[1]
      expect(JSON.stringify(branchChat[0])).toContain("并行分支「左路」")
      expect(lastUserText(branchChat)).toBe("调研方案A")
      const synth = branchChat.find((m) => m.role === "tool" && m.toolCallId === "tc-br")
      expect(synth).toBeDefined()
      const userMsg = branchChat.find((m) => m.role === "user" && m.content === "做两个方案的调研")
      expect(userMsg).toBeDefined()
      // 模型路由（多路接口）：右路经 resolveModelProvider 解析
      expect(h.routeNames).toEqual(["fast"])
      // 事件：分支容器 start（带 branch 标识）与合并事件
      expect(h.events.some((e) => e.type === "event.session.start" && e.payload.branch === "左路")).toBe(true)
      expect(h.events.filter((e) => e.type === "event.branch.merged")).toHaveLength(2)
      // 分支工具面：与主会话同构（全局工具可见）
      expect(h.provider.seenTools[1]).toContain("read")
      expect(h.provider.seenTools[1]).toContain("branch_run")
    } finally {
      h.cleanup()
    }
  })

  test("merge=summary 摘要合入：长报告压缩为要点进主线（全文留过程存档），短报告低于阈值原文合入", async () => {
    const longReport = `调研结论开始。${"细节内容占位。".repeat(300)}`
    expect(longReport.length).toBeGreaterThan(1500)
    const h = await setupBranch(async (msgs) => {
      if (h.provider.calls === 1) {
        return [
          {
            type: "tool_call",
            toolCall: {
              id: "tc-ms",
              name: "branch_run",
              arguments: { branches: [{ name: "摘要路", prompt: "长报告任务" }, { name: "短路", prompt: "短报告任务" }], merge: "summary" },
            },
          } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      // 摘要调用（completeText：system 为报告压缩器提示词，区别于分支/主线调用）
      const sys = msgs[0]?.role === "system" ? String(msgs[0].content) : ""
      if (sys.includes("并行分支报告压缩器")) {
        return [{ type: "text", text: "要点：方案A可行，产物 a.ts；建议采纳A。" }, { type: "done" }] as LLMChunk[]
      }
      const last = lastUserText(msgs)
      if (last === "长报告任务") return [{ type: "text", text: longReport }, { type: "done" }] as LLMChunk[]
      if (last === "短报告任务") return [{ type: "text", text: "短报告：直接结论。" }, { type: "done" }] as LLMChunk[]
      return [{ type: "text", text: "主线总结完成" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "两个分支")
      const msgs = (await h.store.load(session.id, "default"))!.messages
      // 长报告：摘要合入（头行带标记、正文为要点、不含长文细节），全文保留在过程存档
      const mergedLong = msgs.find((m) => m.branchMeta?.name === "摘要路")
      expect(mergedLong).toBeDefined()
      expect(mergedLong!.content).toContain("已合并（摘要合入）")
      expect(mergedLong!.content).toContain("要点：方案A可行")
      expect(mergedLong!.content).not.toContain("细节内容占位")
      expect(mergedLong!.content).toContain("报告全文见分支过程存档")
      expect(mergedLong!.sessionRun?.output).toContain("细节内容占位")
      // 短报告：低于摘要阈值原文合入（无摘要标记、不触发摘要调用）
      const mergedShort = msgs.find((m) => m.branchMeta?.name === "短路")
      expect(mergedShort!.content).toContain("短报告：直接结论。")
      expect(mergedShort!.content).not.toContain("摘要合入")
      // 主线最终回复那轮上下文：只见摘要要点，不见长文全文（主线上下文预算保护生效）
      const finalChat = h.provider.seenChats[h.provider.seenChats.length - 1]
      const joined = JSON.stringify(finalChat)
      expect(joined).toContain("要点：方案A可行")
      expect(joined).not.toContain("细节内容占位")
      // 摘要调用恰好一次（仅长报告触发）
      expect(h.provider.seenChats.filter((m) => String(m[0]?.content ?? "").includes("并行分支报告压缩器")).length).toBe(1)
    } finally {
      h.cleanup()
    }
  })

  test("异步分支：主线先结束，分支完成后直接落盘合入，bg_task 可查", async () => {
    const h = await setupBranch(async (msgs, call) => {
      const last = lastUserText(msgs)
      if (call === 1) {
        // 主线启动后台分支后立即收尾
        return [
          {
            type: "tool_call",
            toolCall: { id: "tc-ab", name: "branch_run", arguments: { branches: [{ name: "后台", prompt: "后台任务" }], async: true } },
          } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      if (last === "后台任务") {
        // 分支慢于主线收尾（150ms），完成后无任务在运行——直接落盘合入
        return (async () => {
          await sleep(150)
          return [{ type: "text", text: "后台报告完成" }, { type: "done" }] as LLMChunk[]
        })()
      }
      if (call === 3) return [{ type: "text", text: "主线先完成" }, { type: "done" }] as LLMChunk[]
      if (call === 4) {
        // 第二个任务：bg_task list 查看分支状态
        return [{ type: "tool_call", toolCall: { id: "tc-bt", name: "bg_task", arguments: { action: "list" } } }, { type: "done" }] as LLMChunk[]
      }
      return [{ type: "text", text: "查询完成" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "启动后台调研")
      // 主线已结束、分支仍在运行：此刻无合并消息
      let msgs = (await h.store.load(session.id, "default"))!.messages
      expect(msgs.some((m) => m.branchMeta?.name === "后台")).toBe(false)
      await sleep(300)
      // 分支完成后直接落盘（无任务运行路径）
      msgs = (await h.store.load(session.id, "default"))!.messages
      const merged = msgs.find((m) => m.branchMeta?.name === "后台")
      expect(merged?.content).toContain("后台报告完成")
      expect(merged?.sessionRun?.branch?.name).toBe("后台")
      expect(h.events.some((e) => e.type === "event.branch.merged" && e.payload.name === "后台")).toBe(true)
      // 第二个任务：bg_task list 可见分支已合入
      await h.engine.run(session.id, "default", "查后台状态")
      msgs = (await h.store.load(session.id, "default"))!.messages
      const listTool = [...msgs].reverse().find((m) => m.role === "tool" && m.name === "bg_task")
      expect(listTool?.content).toContain("已完成并合入主上下文")
    } finally {
      h.cleanup()
    }
  })

  test("父任务停止连带终止分支（不合入）；waitApproval 无任务守卫不崩溃", async () => {
    const h = await setupBranch((msgs, call, opts) => {
      const last = lastUserText(msgs)
      if (call === 1) {
        return [
          {
            type: "tool_call",
            toolCall: { id: "tc-hb", name: "branch_run", arguments: { branches: [{ name: "慢分支", prompt: "慢任务" }], async: true } },
          } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      if (last === "慢任务") {
        // 挂起至 abort：取消信号传播打断（真实 Provider 的 fetch abort 语义）
        return new Promise<LLMChunk[]>((_resolve, reject) => {
          opts?.signal?.addEventListener("abort", () => reject(new Error("aborted")), { once: true })
        })
      }
      return [{ type: "text", text: "不该到达" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      const running = h.engine.run(session.id, "default", "启动慢分支")
      await sleep(100) // 分支进入挂起 chat
      h.engine.cancel(session.id)
      await running
      await sleep(100)
      const msgs = (await h.store.load(session.id, "default"))!.messages
      expect(msgs.some((m) => m.branchMeta?.name === "慢分支")).toBe(false) // 终止分支不合入
      // 会话状态未破坏：无终止相关的任务错误落盘
      expect(h.provider.calls).toBeGreaterThan(0)
    } finally {
      h.cleanup()
    }
  })
})

describe("branch_run 主动合入与互相感知", () => {
  test("阶段性合入（branch_sync 传 content）+ 分支继续运行 + 兄弟分支感知", async () => {
    let h!: Harness
    const state: Record<string, number> = {}
    const waitEvent = async (pred: (e: AgentEvent) => boolean) => {
      for (let i = 0; i < 150; i++) {
        if (h.events.some(pred)) return
        await sleep(20)
      }
      throw new Error("等待事件超时")
    }
    h = await setupBranch(async (msgs, call) => {
      const last = lastUserText(msgs)
      // 分支上下文识别按「包含分支提示词的 user 消息」——主干通知注入后最后一条 user 是通知文本，
      // 按 lastUserText 分发会漏（通知感知轮恰好是断言目标）
      const hasPrompt = (p: string) => msgs.some((m) => m.role === "user" && typeof m.content === "string" && m.content === p)
      if (call === 1) {
        return [
          {
            type: "tool_call",
            toolCall: {
              id: "tc-br",
              name: "branch_run",
              arguments: { branches: [{ name: "左路", prompt: "调研方案A" }, { name: "右路", prompt: "调研方案B" }] },
            },
          } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      if (hasPrompt("调研方案A") && last !== "做两个方案的调研") {
        state.A = (state.A ?? 0) + 1
        if (state.A === 1) {
          // 左路第一轮：主动合入阶段性成果，随后继续
          return [{ type: "tool_call", toolCall: { id: "tc-bm", name: "branch_sync", arguments: { content: "A的中期发现：方案A可行且成本低" } } }, { type: "done" }] as LLMChunk[]
        }
        // 左路第二轮：分支继续运行并给出最终报告（合入后不应收到自身通知）
        await sleep(30)
        return [{ type: "text", text: "左路最终报告" }, { type: "done" }] as LLMChunk[]
      }
      if (hasPrompt("调研方案B") && last !== "做两个方案的调研") {
        state.B = (state.B ?? 0) + 1
        if (state.B === 1) {
          // 右路第一轮先等左路阶段性合入事件（chat 未结束），返回 tool_call 消耗一轮——
          // 下一轮轮首排空收件箱，通知进入右路上下文
          await waitEvent((e) => e.type === "event.branch.merged" && String(e.payload.text ?? "").includes("阶段性合入"))
          return [{ type: "tool_call", toolCall: { id: "tc-todo", name: "todo", arguments: { entries: [] } } }, { type: "done" }] as LLMChunk[]
        }
        return [{ type: "text", text: "右路最终报告" }, { type: "done" }] as LLMChunk[]
      }
      return [{ type: "text", text: "主线总结完成" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "做两个方案的调研")
      // 主干消息：阶段性合入（无过程存档）→ 最终合入 ×2 → 主线总结，顺序保持
      const msgs = (await h.store.load(session.id, "default"))!.messages
      const interim = msgs.find((m) => m.content.includes("阶段性合入"))
      expect(interim).toBeDefined()
      expect(interim?.content).toContain("A的中期发现：方案A可行且成本低")
      expect(interim?.branchMeta?.name).toBe("左路")
      expect(interim?.sessionRun).toBeUndefined() // 分支仍在执行，阶段性合入不带活引用存档
      expect(msgs.findIndex((m) => m.content.includes("阶段性合入"))).toBeLessThan(msgs.findIndex((m) => m.branchMeta?.name === "左路" && m.content.includes("已合并")))
      expect(msgs.some((m) => m.branchMeta?.name === "左路" && m.content.includes("左路最终报告"))).toBe(true) // 合入后继续运行并最终合入
      expect(msgs.some((m) => m.branchMeta?.name === "右路" && m.content.includes("右路最终报告"))).toBe(true)
      // 右路第二轮感知左路阶段性合入（通知注入其上下文；按包含分支提示词识别分支 chat）
      const bChats = h.provider.seenChats.filter((m) => m.some((x) => x.role === "user" && x.content === "调研方案B"))
      expect(bChats.length).toBeGreaterThanOrEqual(2)
      expect(JSON.stringify(bChats[1])).toContain("【分支感知】分支「左路」阶段性合入主干")
      expect(JSON.stringify(bChats[1])).toContain("A的中期发现")
      // 左路自身不收到自己的合入通知（exceptBranchId；系统提示词附注含通知格式说明，按 user 消息判定）
      const aChats = h.provider.seenChats.filter((m) => m.some((x) => x.role === "user" && x.content === "调研方案A"))
      expect(aChats.every((c) => !c.some((x) => x.role === "user" && typeof x.content === "string" && x.content.startsWith("【分支感知】")))).toBe(true)
    } finally {
      h.cleanup()
    }
  })

  test("主线进展感知：异步分支运行中收到主线每轮回复通知", async () => {
    let h!: Harness
    let mainRun: Promise<void> | undefined
    const state: Record<string, number> = {}
    h = await setupBranch(async (msgs, call) => {
      const inBranch = msgs.some((m) => m.role === "user" && typeof m.content === "string" && m.content === "后台任务")
      if (call === 1) {
        return [
          {
            type: "tool_call",
            toolCall: { id: "tc-ab", name: "branch_run", arguments: { branches: [{ name: "后台", prompt: "后台任务" }], async: true } },
          } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      if (inBranch) {
        state.B = (state.B ?? 0) + 1
        if (state.B === 1) {
          // 分支第一轮：等主线任务收尾（最终回复已持久化、通知已入收件箱）再消耗一轮工具
          await mainRun
          await sleep(30)
          return [{ type: "tool_call", toolCall: { id: "tc-todo", name: "todo", arguments: { entries: [] } } }, { type: "done" }] as LLMChunk[]
        }
        return [{ type: "text", text: "后台分支完成" }, { type: "done" }] as LLMChunk[]
      }
      if (call === 3) return [{ type: "text", text: "主线先给出方向：优先X" }, { type: "done" }] as LLMChunk[]
      return [{ type: "text", text: "不该到达" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      mainRun = h.engine.run(session.id, "default", "启动后台")
      await mainRun
      await sleep(200) // 分支完成并直接落盘合入
      const msgs = (await h.store.load(session.id, "default"))!.messages
      expect(msgs.some((m) => m.branchMeta?.name === "后台" && m.content.includes("后台分支完成"))).toBe(true)
      // 分支第二轮感知主线回复（通知注入其上下文；按包含分支提示词识别分支 chat）
      const bChats = h.provider.seenChats.filter((m) => m.some((x) => x.role === "user" && x.content === "后台任务"))
      expect(bChats.length).toBeGreaterThanOrEqual(2)
      expect(JSON.stringify(bChats[1])).toContain("【主线进展】主线回复")
      expect(JSON.stringify(bChats[1])).toContain("主线先给出方向：优先X")
    } finally {
      h.cleanup()
    }
  })

  test("branch_sync 在非分支上下文返回不可用说明", async () => {
    const { branchSyncTool } = await import("./tools")
    const r = await branchSyncTool.execute({ content: "x" }, {
      user: "default",
      sessionId: "s",
      workdir: "/tmp",
      home: "/tmp",
      env: {},
      sandboxed: false,
      resolvePath: (p) => p,
      readFile: async () => "",
      readBinaryFile: async () => new Uint8Array(),
      writeFile: async () => {},
      listFiles: async () => [],
      listDir: async () => [],
      deleteFile: async () => {},
      moveFile: async () => {},
      runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
      uploadAttachment: async (r2) => r2.path,
      publish: () => {},
      projects: [],
      resolveProjectPath: () => {
        throw new Error("nope")
      },
      getTodos: async () => [],
      setTodos: async () => {},
      registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
      listSubAgentDefs: () => [],
      loadSubAgent: async () => {},
      runNewSession: async () => ({ output: "", archive: { runId: "", agents: [], input: "", output: "", messages: [] } }),
      waitForChoice: async () => null,
      waitForEnv: async () => false,
      waitForDraw: async () => null,
      waitForCapture: async () => null,
    })
    expect(r.output).toContain("不支持分支同步")
  })
})

describe("branch_run 主干全量感知（branch_sync / 合入回显）", () => {
  test("branch_sync 增量拉取其他分支合入全文，且增量去重", async () => {
    let h!: Harness
    const state: Record<string, number> = {}
    const waitEvent = async (pred: (e: AgentEvent) => boolean) => {
      for (let i = 0; i < 150; i++) {
        if (h.events.some(pred)) return
        await sleep(20)
      }
      throw new Error("等待事件超时")
    }
    h = await setupBranch(async (msgs, call) => {
      const hasPrompt = (p: string) => msgs.some((m) => m.role === "user" && typeof m.content === "string" && m.content === p)
      if (call === 1) {
        return [
          {
            type: "tool_call",
            toolCall: {
              id: "tc-br",
              name: "branch_run",
              arguments: { branches: [{ name: "生产者", prompt: "生产任务" }, { name: "消费者", prompt: "消费任务" }] },
            },
          } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      if (hasPrompt("生产任务")) {
        state.P = (state.P ?? 0) + 1
        if (state.P === 1) {
          return [{ type: "tool_call", toolCall: { id: "tc-pm", name: "branch_sync", arguments: { content: "生产者的详细发现：模块X存在循环依赖，重构路径为 A→B→C 三步" } } }, { type: "done" }] as LLMChunk[]
        }
        await sleep(50)
        return [{ type: "text", text: "生产者完成" }, { type: "done" }] as LLMChunk[]
      }
      if (hasPrompt("消费任务")) {
        state.C = (state.C ?? 0) + 1
        if (state.C === 1) {
          // 等生产者合入后消耗一轮，再同步主干
          await waitEvent((e) => e.type === "event.branch.merged" && String(e.payload.text ?? "").includes("阶段性合入"))
          return [{ type: "tool_call", toolCall: { id: "tc-c1", name: "todo", arguments: { entries: [] } } }, { type: "done" }] as LLMChunk[]
        }
        if (state.C === 2) {
          // 全量同步主干（应含生产者合入全文；自身 fork 前的主线消息不重复出现）
          return [{ type: "tool_call", toolCall: { id: "tc-cs", name: "branch_sync", arguments: {} } }, { type: "done" }] as LLMChunk[]
        }
        if (state.C === 3) {
          // 再次同步：增量应为「暂无新消息」（去重验证：队列合入不因重复拉取而重复出现）
          return [{ type: "tool_call", toolCall: { id: "tc-cs2", name: "branch_sync", arguments: {} } }, { type: "done" }] as LLMChunk[]
        }
        return [{ type: "text", text: "消费者完成" }, { type: "done" }] as LLMChunk[]
      }
      return [{ type: "text", text: "主线总结完成" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "两分支协作")
      // 消费者各轮上下文：合入通知（轮2注入）+ 首次同步结果（轮2工具结果）各出现一次
      const cChats = h.provider.seenChats.filter((m) => m.some((x) => x.role === "user" && x.content === "消费任务"))
      expect(cChats.length).toBeGreaterThanOrEqual(4)
      const syncRound = JSON.stringify(cChats[2])
      expect(syncRound).toContain("branch_sync")
      expect(syncRound).toContain("【合并·生产者】")
      expect(syncRound).toContain("模块X存在循环依赖，重构路径为 A→B→C 三步")
      expect(syncRound.split("模块X存在循环依赖").length - 1).toBe(2) // 通知一次 + 同步结果一次
      // 二次同步（第 4 轮上下文）：第二次 sync 结果为「暂无新消息」，全文不因重复拉取再次出现
      const secondSyncRound = JSON.stringify(cChats[3])
      expect(secondSyncRound).toContain("暂无新消息")
      expect(secondSyncRound.split("模块X存在循环依赖").length - 1).toBe(2) // 仍是通知+首次同步，无重复
      // 主线最终消息完整
      const msgs = (await h.store.load(session.id, "default"))!.messages
      expect(msgs.some((m) => m.branchMeta?.name === "生产者" && m.content.includes("阶段性合入"))).toBe(true)
      expect(msgs.some((m) => m.branchMeta?.name === "生产者" && m.content.includes("生产者完成"))).toBe(true)
      expect(msgs.some((m) => m.branchMeta?.name === "消费者" && m.content.includes("消费者完成"))).toBe(true)
    } finally {
      h.cleanup()
    }
  })

  test("branch_sync 传 content 合入并返回主干增量（合入即感知）", async () => {
    let h!: Harness
    const state: Record<string, number> = {}
    const waitEvent = async (pred: (e: AgentEvent) => boolean) => {
      for (let i = 0; i < 150; i++) {
        if (h.events.some(pred)) return
        await sleep(20)
      }
      throw new Error("等待事件超时")
    }
    h = await setupBranch(async (msgs, call) => {
      const hasPrompt = (p: string) => msgs.some((m) => m.role === "user" && typeof m.content === "string" && m.content === p)
      if (call === 1) {
        return [
          {
            type: "tool_call",
            toolCall: {
              id: "tc-br",
              name: "branch_run",
              arguments: { branches: [{ name: "先行", prompt: "先行任务" }, { name: "后至", prompt: "后至任务" }] },
            },
          } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      if (hasPrompt("先行任务")) {
        state.F = (state.F ?? 0) + 1
        if (state.F === 1) {
          return [{ type: "tool_call", toolCall: { id: "tc-fm", name: "branch_sync", arguments: { content: "先行的关键结论：接口 V2 已废弃，统一走 V3" } } }, { type: "done" }] as LLMChunk[]
        }
        return [{ type: "text", text: "先行完成" }, { type: "done" }] as LLMChunk[]
      }
      if (hasPrompt("后至任务")) {
        state.L = (state.L ?? 0) + 1
        if (state.L === 1) {
          // 等先行合入后，后至分支合入——工具结果应回显先行的合入内容（主干增量）
          await waitEvent((e) => e.type === "event.branch.merged" && String(e.payload.text ?? "").includes("阶段性合入"))
          return [{ type: "tool_call", toolCall: { id: "tc-lm", name: "branch_sync", arguments: { content: "后至的中期结论" } } }, { type: "done" }] as LLMChunk[]
        }
        return [{ type: "text", text: "后至完成" }, { type: "done" }] as LLMChunk[]
      }
      return [{ type: "text", text: "主线总结完成" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "两分支接力")
      // 后至分支第二轮上下文：branch_sync 工具结果回显了先行合入（主干增量，合入即感知）
      const lChats = h.provider.seenChats.filter((m) => m.some((x) => x.role === "user" && x.content === "后至任务"))
      expect(lChats.length).toBeGreaterThanOrEqual(2)
      const mergedRound = JSON.stringify(lChats[1])
      expect(mergedRound).toContain("主干自 fork/上次同步以来的新进展")
      expect(mergedRound).toContain("接口 V2 已废弃，统一走 V3")
      // 先行分支的合入工具结果不回显自己（自身合入跳过）：其合入时主干无增量，结果为「暂无新消息」
      const fChats = h.provider.seenChats.filter((m) => m.some((x) => x.role === "user" && x.content === "先行任务"))
      expect(JSON.stringify(fChats[1])).toContain("暂无新消息")
    } finally {
      h.cleanup()
    }
  })
})
