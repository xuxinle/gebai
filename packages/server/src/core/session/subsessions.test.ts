import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { AgentEvent, LLMCapabilities, MessageLike, TodoItem } from "@gebai/sdk"
import type { LLMChunk, LLMProvider, ChatOptions } from "../llm/llm"
import { parseModelRoutes, resolveModelRouteProvider } from "../llm/llm"
import { AgentEngine } from "../engine/engine"
import { SessionStore } from "./store"
import { ToolRegistry } from "../base/registry"
import { createGlobalTools } from "../tools"
import { Sandbox } from "../security/sandbox"
import { EnvManager } from "./env"
import { EventBus } from "../base/event-bus"
import { SubAgentManager } from "../agents/subagents"
import { loadConfig } from "../base/config"
import { SubSessionRegistry, normalizeSubSessionSpecs, SUBSESSION_MAX_CONCURRENT, SUBSESSION_KEEP, type SubSessionHandle, type SubSessionSpec } from "./subsessions"

/**
 * 子会话运行（DESIGN「子会话运行」）测试：
 * - 规格规范化：单任务（input+agents）与多任务（subsessions）二选一、缺省命名/默认值/非法形态
 * - 注册表单元：start/wait/cancel/list、视角可见性（父子进程树）、并发上限、fork 快照切片、终态修剪
 * - 引擎集成（脚本化 ScriptProvider）：
 *   隔离上下文（spawn）与继承上下文（fork）两种形态、报告自动合入父会话、摘要合入、异步后台运行与
 *   bg_task 控制、同步不注入合并工具、待办运行内隔离、空 agents（不加载子Agent）
 */

const sleep = (ms: number) => new Promise<void>((res) => setTimeout(res, ms))

/** 规格规范化：两形态互斥、缺省值、缺省命名、批内唯一、上限。 */
describe("normalizeSubSessionSpecs", () => {
  test("单任务形态：input 必填，agents 缺省空（不加载子Agent），缺省值与开关默认", () => {
    const specs = normalizeSubSessionSpecs({ input: "干活", agents: ["code", " code ", ""] })
    expect(specs).toHaveLength(1)
    expect(specs[0]).toMatchObject({
      name: "s1",
      input: "干活",
      agents: ["code"],
      inheritContext: false,
      merge: "full",
      async: false,
      inheritGlobalTools: true,
      inheritGlobalPrompt: true,
    })
    // 缺省 agents → 空数组（不加载任何子Agent）
    expect(normalizeSubSessionSpecs({ input: "x" })[0].agents).toEqual([])
  })

  test("多任务形态：缺省名 s1..sN、per-item agents/model，调用级开关盖章到各子会话", () => {
    const specs = normalizeSubSessionSpecs({
      subsessions: [{ input: "a" }, { name: "路二", input: "b", agents: ["code"], model: "fast" }],
      inherit_context: true,
      async: true,
      merge: "summary",
      inherit_global_tools: false,
      inherit_global_prompt: false,
    })
    expect(specs.map((s) => s.name)).toEqual(["s1", "路二"])
    expect(specs[1]).toMatchObject({ agents: ["code"], model: "fast", inheritContext: true, merge: "summary", async: true, inheritGlobalTools: false, inheritGlobalPrompt: false })
  })

  test("非法形态抛错：两形态同给/空数组/缺 input/重名/非法名/超批上限", () => {
    expect(() => normalizeSubSessionSpecs({ input: "x", subsessions: [{ input: "y" }] })).toThrow(/二选一/)
    expect(() => normalizeSubSessionSpecs({ subsessions: [] })).toThrow()
    expect(() => normalizeSubSessionSpecs({})).toThrow()
    expect(() => normalizeSubSessionSpecs({ subsessions: [{ name: "a" }] })).toThrow(/input 必填/)
    expect(() => normalizeSubSessionSpecs({ subsessions: [{ name: "a", input: "x" }, { name: "a", input: "y" }] })).toThrow(/重复/)
    expect(() => normalizeSubSessionSpecs({ subsessions: [{ name: "坏 名", input: "x" }] })).toThrow(/name 非法/)
    expect(() => normalizeSubSessionSpecs({ subsessions: Array.from({ length: 9 }, (_, i) => ({ name: `n${i}`, input: "x" })) })).toThrow(/数量超限/)
  })
})

/** 注册表单元：start/wait/cancel/list、视角（进程树）可见性、并发上限、fork 快照切片、终态修剪。 */
describe("SubSessionRegistry", () => {
  const specOf = (name: string, input = "p", inheritContext = true): SubSessionSpec => ({
    name,
    input,
    agents: [],
    inheritContext,
    merge: "full",
    async: false,
    inheritGlobalTools: true,
    inheritGlobalPrompt: true,
  })

  function makeRegistry(store = new Map<string, SubSessionHandle>(), extra: { ownerRunId?: string; onDone?: (h: SubSessionHandle) => void } = {}) {
    const reg = new SubSessionRegistry({
      sessionId: "s1",
      store,
      depth: 0,
      forkSource: [{ role: "system", content: "sys" }, { role: "user", content: "hi" }],
      validate: (spec) => spec.agents,
      ...extra,
      runner: (spec, signal) =>
        new Promise((resolve, reject) => {
          if (spec.name.startsWith("hang")) {
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
                archive: { runId: spec.runId, agents: spec.agents, input: spec.input, output: `out-${spec.name}`, messages: [{ role: "user", content: spec.input }, { role: "assistant", content: `out-${spec.name}` }] },
              }),
            10,
          )
        }),
    })
    return { reg, store }
  }

  test("start 返回 s 前缀 id 与派生元信息，wait 等到 done，list 按会话过滤", async () => {
    const { reg, store } = makeRegistry()
    store.set("szzzzzzzz", { runId: "szzzzzzzz", sessionId: "other", name: "x", input: "p", agents: [], inheritContext: false, async: false, merge: "full", inheritGlobalTools: true, inheritGlobalPrompt: true, depth: 1, startedAt: 0, status: "running", controller: new AbortController(), done: Promise.resolve() })
    const recs = await reg.start([specOf("a"), specOf("b", "q", false)])
    expect(recs).toHaveLength(2)
    expect(recs[0].runId.startsWith("s")).toBe(true)
    expect(recs[0]).toMatchObject({ status: "running", inheritContext: true, depth: 1, merged: false })
    expect(recs[1].inheritContext).toBe(false)
    const done = await reg.wait(recs[0].runId, 2000)
    expect(done?.status).toBe("done")
    expect(done?.output).toBe("out-a")
    expect(reg.list().length).toBe(2)
    expect(reg.get("szzzzzzzz")).toBeUndefined()
  })

  test("继承形态完成经 onDone 合入回调（隔离形态不回调）；运行中 result 不可取", async () => {
    const merged: string[] = []
    const { reg } = makeRegistry(new Map(), { onDone: (h) => void merged.push(h.name) })
    const [forkRec] = await reg.start([specOf("fork")])
    expect(reg.result(forkRec.runId)).toBeUndefined()
    await reg.wait(forkRec.runId, 2000)
    const [spawnRec] = await reg.start([specOf("spawn", "p", false)])
    await reg.wait(spawnRec.runId, 2000)
    expect(merged).toEqual(["fork"])
    expect(reg.result(spawnRec.runId)?.output).toBe("out-spawn")
  })

  test("视角可见性（进程树）：子会话视角只见自己的直接子会话", async () => {
    const store = new Map<string, SubSessionHandle>()
    const root = makeRegistry(store).reg
    const [child] = await root.start([specOf("child")])
    const childView = new SubSessionRegistry({
      sessionId: "s1",
      store,
      depth: 1,
      ownerRunId: child.runId,
      validate: (spec) => spec.agents,
      runner: async (spec) => ({ output: "ok", archive: { runId: spec.runId, agents: [], input: spec.input, output: "ok", messages: [] } }),
    })
    const [grand] = await childView.start([specOf("grand")])
    await root.wait(child.runId, 2000)
    await root.wait(grand.runId, 2000)
    // 主任务视角见全部（含孙代）；子会话视角只见自己的直接子会话
    expect(root.list().map((r) => r.name).sort()).toEqual(["child", "grand"])
    expect(childView.list().map((r) => r.name)).toEqual(["grand"])
    expect(childView.get(child.runId)).toBeUndefined()
    // 孙代 parentRunId 指向派生它的子会话
    expect(root.get(grand.runId)?.parentRunId).toBe(child.runId)
  })

  test("并发上限按会话全树合计；cancel 终止运行中运行", async () => {
    const { reg } = makeRegistry()
    await reg.start([specOf("hang")])
    await expect(reg.start(Array.from({ length: SUBSESSION_MAX_CONCURRENT }, (_, i) => specOf(`n${i}`)))).rejects.toThrow(/并发子会话超限/)
    const [rec] = await reg.start([specOf("hang2")])
    const cancelled = await reg.cancel(rec.runId)
    expect(cancelled?.status).toBe("cancelled")
  })

  test("父任务取消信号连带终止子会话；失败运行落 failed", async () => {
    const parent = new AbortController()
    const { reg } = makeRegistry()
    const reg2 = new SubSessionRegistry({
      sessionId: "s1",
      store: new Map(),
      depth: 0,
      parentSignal: parent.signal,
      validate: (spec) => spec.agents,
      runner: (spec, signal) =>
        new Promise((_resolve, reject) => {
          if (spec.name === "fail") return reject(new Error("boom"))
          signal.addEventListener("abort", () => reject(new Error("cancelled")), { once: true })
        }),
    })
    const [hang] = await reg2.start([specOf("hang")])
    const [fail] = await reg2.start([specOf("fail")])
    await reg2.wait(fail.runId, 2000)
    expect(reg2.get(fail.runId)?.status).toBe("failed")
    expect(reg2.get(fail.runId)?.error).toContain("boom")
    parent.abort(new Error("用户停止"))
    await reg2.wait(hang.runId, 2000)
    expect(reg2.get(hang.runId)?.status).toBe("cancelled")
    void reg
  })

  test("终态保留修剪：超出 SUBSESSION_KEEP 淘汰最旧，运行中不淘汰", async () => {
    const store = new Map<string, SubSessionHandle>()
    const { reg } = makeRegistry(store)
    const finished: string[] = []
    for (let i = 0; i < SUBSESSION_KEEP + 2; i++) {
      const [rec] = await reg.start([specOf(`r${i}`)])
      await reg.wait(rec.runId, 2000)
      finished.push(rec.runId)
    }
    expect(reg.get(finished[0])).toBeUndefined()
    expect(reg.get(finished[1])).toBeUndefined()
    expect(reg.get(finished[2])).toBeDefined()
    expect(reg.list().length).toBe(SUBSESSION_KEEP)
  })

  test("fork 快照切片：继承形态 runner 收到构造时消息副本，隔离形态收到空数组", async () => {
    const live: MessageLike[] = [{ role: "system", content: "sys" }]
    const seen: MessageLike[][] = []
    const reg = new SubSessionRegistry({
      sessionId: "s1",
      store: new Map(),
      depth: 0,
      forkSource: live,
      validate: (spec) => spec.agents,
      runner: async (spec, _signal, fork) => {
        seen.push(fork)
        return { output: "ok", archive: { runId: spec.runId, agents: [], input: spec.input, output: "ok", messages: [] } }
      },
    })
    await reg.start([specOf("fork"), specOf("spawn", "p", false)])
    live.push({ role: "user", content: "after-fork" })
    await sleep(30)
    expect(seen[0]).toEqual([{ role: "system", content: "sys" }])
    expect(seen[1]).toEqual([])
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
    const base = { model: "base-model", apiBase: "https://base.example/v1", apiKey: "k", apiKind: "openai" as const, maxContextTokens: 128000, multimodal: false }
    const env = { GEBAI_LLM_ROUTES: JSON.stringify({ fast: { model: "gpt-4o-mini", api_base: "https://a.example/v1" } }) }
    expect(resolveModelRouteProvider(base, env, "fast")).toBeDefined()
    expect(resolveModelRouteProvider(base, env, "other-model")).toBeDefined()
    expect(resolveModelRouteProvider(base, env, "")).toBeUndefined()
  })
})

/**
 * 脚本化 Provider：按 (消息, 调用序, 调用选项) 响应 chunk 数组——子会话按任务指令分流、
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

async function setupSub(
  respond: (msgs: MessageLike[], callIdx: number, opts?: ChatOptions) => LLMChunk[] | Promise<LLMChunk[]>,
  opts: { resolveModelProvider?: boolean } = {},
): Promise<Harness> {
  const home = mkdtempSync(join(tmpdir(), "gebai-sub-"))
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
const hasUserText = (msgs: MessageLike[], text: string): boolean => msgs.some((m) => m.role === "user" && typeof m.content === "string" && m.content === text)
const runIdIn = (text: string): string | undefined => text.match(/s[0-9a-f]{8}/)?.[0]

/** 子会话上下文判定（子会话系统提示词含 fork 附注/隔离开场白；父会话/压缩调用不含）。 */
const isSubChat = (msgs: MessageLike[]): boolean => {
  const sys = String(msgs[0]?.content ?? "")
  return sys.includes("【并行子会话】") || sys.includes("你正在一个子会话中执行任务")
}
/** 父会话轮次计数：父会话 chat 串行（阻塞在工具调用），子会话并发不共用计数。 */
function makeParentRound(): (msgs: MessageLike[]) => number {
  let n = 0
  return (msgs) => {
    if (isSubChat(msgs)) return 0
    const sys = String(msgs[0]?.content ?? "")
    if (sys.includes("子会话报告压缩器")) return 0
    return ++n
  }
}

describe("subsession_run 集成（继承上下文 fork）", () => {
  test("同步 fan-out：并行执行 + 报告自动合入父上下文 + 下一轮可见 + 模型路由", async () => {
    const parentRound = makeParentRound()
    const h = await setupSub(
      (msgs) => {
        const round = parentRound(msgs)
        const last = lastUserText(msgs)
        if (round === 1) {
          return [
            {
              type: "tool_call",
              toolCall: {
                id: "tc-sr",
                name: "subsession_run",
                arguments: {
                  subsessions: [
                    { name: "左路", input: "调研方案A" },
                    { name: "右路", input: "调研方案B", model: "fast" },
                  ],
                  inherit_context: true,
                },
              },
            } as LLMChunk,
            { type: "done" } as LLMChunk,
          ]
        }
        if (isSubChat(msgs) && (last === "调研方案A" || last === "调研方案B")) {
          // 子会话响应：延迟制造并发窗口，验证两子会话同时在途（摆脱单轮串行）
          return (async () => {
            await sleep(80)
            return [{ type: "text", text: `报告:${last}` }, { type: "done" }] as LLMChunk[]
          })()
        }
        return [{ type: "text", text: "父会话总结完成" }, { type: "done" }] as LLMChunk[]
      },
      { resolveModelProvider: true },
    )
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "做两个方案的调研")
      // 并行性：两子会话同时在途（maxInFlight ≥ 2）
      expect(h.provider.maxInFlight).toBeGreaterThanOrEqual(2)
      // 合入消息：tool 结果之后两条合并消息（顺序：tool → merge×2 → 最终回复）
      const msgs = (await h.store.load(session.id, "default"))!.messages
      const toolIdx = msgs.findIndex((m) => m.role === "tool" && m.name === "subsession_run")
      expect(toolIdx).toBeGreaterThan(0)
      expect(msgs[toolIdx + 1].subSessionMerged?.name).toBe("左路")
      expect(msgs[toolIdx + 2].subSessionMerged?.name).toBe("右路")
      expect(msgs[toolIdx + 1].content).toContain("【子会话「左路」已合并】")
      expect(msgs[toolIdx + 1].content).toContain("报告:调研方案A")
      expect(msgs[toolIdx + 1].subSessionArchive?.subsession?.name).toBe("左路")
      expect(msgs[toolIdx + 2].subSessionArchive?.subsession?.model).toBe("fast")
      // 落盘即 user + engineNote: "subsession"（避开思考类模型的尾 assistant 约束，前端渲染为通知条）
      expect(msgs[toolIdx + 1].role).toBe("user")
      expect(msgs[toolIdx + 1].engineNote).toBe("subsession")
      expect(msgs[toolIdx + 3].role).toBe("assistant")
      expect(msgs[toolIdx + 3].content).toBe("父会话总结完成")
      // 工具结果为概要（不重复全文）
      expect(msgs[toolIdx].content).toContain("子会话执行完成")
      // 下一轮父会话可见合入内容（最终回复那次调用包含合并消息）
      const finalChat = h.provider.seenChats[h.provider.seenChats.length - 1]
      const joined = JSON.stringify(finalChat)
      expect(joined).toContain("报告:调研方案A")
      expect(joined).toContain("报告:调研方案B")
      // fork 内容：子会话上下文含父会话上下文（用户输入）+ 任务指令 + 悬空 toolCall 合成补齐 + 子会话系统附注
      const subChat = h.provider.seenChats.find((m) => isSubChat(m) && hasUserText(m, "调研方案A"))!
      expect(JSON.stringify(subChat[0])).toContain("并行子会话")
      expect(lastUserText(subChat)).toBe("调研方案A")
      expect(subChat.find((m) => m.role === "tool" && m.toolCallId === "tc-sr")).toBeDefined()
      expect(hasUserText(subChat, "做两个方案的调研")).toBe(true)
      // 模型路由（多路接口）：右路经 resolveModelProvider 解析
      expect(h.routeNames).toEqual(["fast"])
      // 事件：子会话容器 start（带子会话名）与合并事件
      expect(h.events.some((e) => e.type === "event.subsession.start" && e.payload.subsession === "左路")).toBe(true)
      expect(h.events.filter((e) => e.type === "event.subsession.merged")).toHaveLength(2)
      // 工具面：与父会话同构（全局工具 + 编排可见）；同步运行不注入合并工具
      const subIdx = h.provider.seenChats.findIndex((m) => isSubChat(m) && hasUserText(m, "调研方案A"))
      expect(h.provider.seenTools[subIdx]).toContain("read")
      expect(h.provider.seenTools[subIdx]).toContain("subsession_run")
      expect(h.provider.seenTools[subIdx]).not.toContain("subsession_merge")
    } finally {
      h.cleanup()
    }
  })

  test("merge=summary 摘要合入：长报告压成要点进父上下文（全文留过程存档），短报告低于阈值原文合入", async () => {
    const parentRound = makeParentRound()
    const longReport = `调研结论开始。${"细节内容占位。".repeat(300)}`
    expect(longReport.length).toBeGreaterThan(1500)
    const h = await setupSub(async (msgs) => {
      const sys = msgs[0]?.role === "system" ? String(msgs[0].content) : ""
      if (sys.includes("子会话报告压缩器")) return [{ type: "text", text: "要点：方案A可行，产物 a.ts；建议采纳A。" }, { type: "done" }] as LLMChunk[]
      if (parentRound(msgs) === 1) {
        return [
          {
            type: "tool_call",
            toolCall: {
              id: "tc-ms",
              name: "subsession_run",
              arguments: {
                subsessions: [
                  { name: "摘要路", input: "长报告任务" },
                  { name: "短路", input: "短报告任务" },
                ],
                inherit_context: true,
                merge: "summary",
              },
            },
          } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      const last = lastUserText(msgs)
      if (isSubChat(msgs) && last === "长报告任务") return [{ type: "text", text: longReport }, { type: "done" }] as LLMChunk[]
      if (isSubChat(msgs) && last === "短报告任务") return [{ type: "text", text: "短报告：直接结论。" }, { type: "done" }] as LLMChunk[]
      return [{ type: "text", text: "父会话总结完成" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "两个子会话")
      const msgs = (await h.store.load(session.id, "default"))!.messages
      // 长报告：摘要合入（头行带标记、正文为要点、不含长文细节），全文保留在过程存档
      const mergedLong = msgs.find((m) => m.subSessionMerged?.name === "摘要路")
      expect(mergedLong).toBeDefined()
      expect(mergedLong!.content).toContain("已合并（摘要合入）")
      expect(mergedLong!.content).toContain("要点：方案A可行")
      expect(mergedLong!.content).not.toContain("细节内容占位")
      expect(mergedLong!.content).toContain("报告全文见子会话过程存档")
      expect(mergedLong!.subSessionArchive?.output).toContain("细节内容占位")
      // 短报告：低于摘要阈值原文合入（无摘要标记、不触发摘要调用）
      const mergedShort = msgs.find((m) => m.subSessionMerged?.name === "短路")
      expect(mergedShort!.content).toContain("短报告：直接结论。")
      expect(mergedShort!.content).not.toContain("摘要合入")
      // 父会话最终回复那轮上下文：只见摘要要点，不见长文全文（上下文预算保护生效）
      const finalChat = h.provider.seenChats[h.provider.seenChats.length - 1]
      const joined = JSON.stringify(finalChat)
      expect(joined).toContain("要点：方案A可行")
      expect(joined).not.toContain("细节内容占位")
      // 摘要调用恰好一次（仅长报告触发）
      expect(h.provider.seenChats.filter((m) => String(m[0]?.content ?? "").includes("子会话报告压缩器")).length).toBe(1)
    } finally {
      h.cleanup()
    }
  })

  test("父会话停止连带终止子会话（不合入）", async () => {
    const parentRound = makeParentRound()
    const h = await setupSub((msgs, _call, opts) => {
      if (parentRound(msgs) === 1) {
        return [
          { type: "tool_call", toolCall: { id: "tc-hb", name: "subsession_run", arguments: { input: "慢任务", inherit_context: true, async: true } } } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      if (isSubChat(msgs) && lastUserText(msgs) === "慢任务") {
        // 挂起至 abort：取消信号沿进程树传播（真实 Provider 的 fetch abort 语义）
        return new Promise<LLMChunk[]>((_resolve, reject) => {
          opts?.signal?.addEventListener("abort", () => reject(new Error("aborted")), { once: true })
        })
      }
      return [{ type: "text", text: "父会话收尾" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      const running = h.engine.run(session.id, "default", "启动慢子会话")
      await sleep(100) // 子会话进入挂起 chat
      h.engine.cancel(session.id)
      await running
      await sleep(100)
      const msgs = (await h.store.load(session.id, "default"))!.messages
      expect(msgs.some((m) => m.subSessionMerged)).toBe(false) // 终止的子会话不合入
      expect(h.provider.calls).toBeGreaterThan(0)
    } finally {
      h.cleanup()
    }
  })
})

describe("subsession_run 集成（隔离上下文 spawn）", () => {
  test("同步隔离运行：结果作为工具结果返回 + 过程存档挂调用记录 + 不继承父上下文", async () => {
    const parentRound = makeParentRound()
    const h = await setupSub((msgs) => {
      if (isSubChat(msgs)) return [{ type: "text", text: "子会话结论：目录里有 3 个文件。" }, { type: "done" }] as LLMChunk[]
      if (parentRound(msgs) === 1) {
        return [
          { type: "tool_call", toolCall: { id: "tc-i1", name: "subsession_run", arguments: { input: "看看目录", agents: ["code"] } } } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      return [{ type: "text", text: "父会话收到结论" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "委托子会话")
      const msgs = (await h.store.load(session.id, "default"))!.messages
      const callMsg = msgs.find((m) => m.role === "tool" && m.name === "subsession_run")
      expect(callMsg?.content).toContain("子会话结论：目录里有 3 个文件。")
      expect(callMsg?.subSessionArchive?.output).toContain("目录里有 3 个文件")
      // 隔离形态：无自动合入消息
      expect(msgs.some((m) => m.subSessionMerged)).toBe(false)
      // 子会话上下文：不含父会话用户输入（隔离），但注入全局工具与子Agent 独有工具
      const subChat = h.provider.seenChats.find((m) => isSubChat(m))!
      expect(hasUserText(subChat, "委托子会话")).toBe(false)
      expect(lastUserText(subChat)).toBe("看看目录")
      expect(String(subChat[0].content)).toContain("执行过程不进入父会话上下文")
      const subIdx = h.provider.seenChats.findIndex((m) => isSubChat(m))
      expect(h.provider.seenTools[subIdx]).toContain("read")
      expect(h.provider.seenTools[subIdx]).toContain("code_git")
      expect(h.provider.seenTools[subIdx]).not.toContain("subsession_merge")
    } finally {
      h.cleanup()
    }
  })

  test("空 agents（不加载任何子Agent）：通用子会话可运行，工具面只有全局工具与编排", async () => {
    const parentRound = makeParentRound()
    const h = await setupSub((msgs) => {
      if (isSubChat(msgs)) return [{ type: "text", text: "通用子会话完成" }, { type: "done" }] as LLMChunk[]
      if (parentRound(msgs) === 1) {
        return [
          { type: "tool_call", toolCall: { id: "tc-e1", name: "subsession_run", arguments: { input: "独立任务" } } } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      return [{ type: "text", text: "父会话收尾" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "跑个通用子会话")
      const subChat = h.provider.seenChats.find((m) => isSubChat(m))!
      expect(String(subChat[0].content)).toContain("未预加载子Agent")
      const subIdx = h.provider.seenChats.findIndex((m) => isSubChat(m))
      expect(h.provider.seenTools[subIdx]).toContain("read")
      expect(h.provider.seenTools[subIdx]).toContain("subsession_run")
      expect(h.provider.seenTools[subIdx].some((t) => t.startsWith("code_"))).toBe(false)
      const msgs = (await h.store.load(session.id, "default"))!.messages
      expect(msgs.find((m) => m.role === "tool" && m.name === "subsession_run")?.content).toContain("通用子会话完成")
    } finally {
      h.cleanup()
    }
  })

  test("异步隔离运行：父会话不阻塞，bg_task list/status/wait 可控（s 前缀）", async () => {
    const parentRound = makeParentRound()
    const h = await setupSub(async (msgs) => {
      if (isSubChat(msgs)) {
        await sleep(60)
        return [{ type: "text", text: "后台子会话完成" }, { type: "done" }] as LLMChunk[]
      }
      const round = parentRound(msgs)
      if (round === 1) {
        return [
          { type: "tool_call", toolCall: { id: "tc-a1", name: "subsession_run", arguments: { input: "后台任务", async: true } } } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      if (round === 2) {
        // 父会话未被阻塞：立刻查询后台子会话进度（list）
        return [{ type: "tool_call", toolCall: { id: "tc-b1", name: "bg_task", arguments: { action: "list" } } }, { type: "done" }] as LLMChunk[]
      }
      if (round === 3) {
        const listText = [...msgs].reverse().find((m) => m.role === "tool" && m.name === "bg_task")?.content
        const id = runIdIn(String(listText ?? ""))
        return [{ type: "tool_call", toolCall: { id: "tc-b2", name: "bg_task", arguments: { action: "wait", id, timeout: 5 } } }, { type: "done" }] as LLMChunk[]
      }
      return [{ type: "text", text: "父会话收尾" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "启动后台")
      const msgs = (await h.store.load(session.id, "default"))!.messages
      const listTool = msgs.find((m) => m.role === "tool" && m.name === "bg_task" && String(m.content).includes("子会话"))
      expect(listTool).toBeDefined()
      expect(String(listTool!.content)).toMatch(/runId s[0-9a-f]{8}/)
      const waitTool = [...msgs].reverse().find((m) => m.role === "tool" && m.name === "bg_task")
      expect(String(waitTool!.content)).toContain("后台子会话完成")
      // 异步隔离形态：结果经 bg_task 取回（不自动合入父上下文）
      expect(msgs.some((m) => m.subSessionMerged)).toBe(false)
    } finally {
      h.cleanup()
    }
  })
})

describe("subsession_run 待办隔离与异步合入", () => {
  test("待办运行内隔离：子会话读写自己的清单，不落盘、不回流、不继承父会话待办", async () => {
    const parentRound = makeParentRound()
    const h = await setupSub((msgs) => {
      if (isSubChat(msgs)) {
        const todoDone = msgs.some((m) => m.role === "tool" && String(m.name ?? "") === "todo")
        if (!todoDone) return [{ type: "tool_call", toolCall: { id: "tc-t1", name: "todo", arguments: { entries: [{ op: "add", title: "子任务A", priority: "high" }] } } }, { type: "done" }] as LLMChunk[]
        return [{ type: "text", text: "子会话做完了一段" }, { type: "done" }] as LLMChunk[]
      }
      if (parentRound(msgs) === 1) {
        return [
          { type: "tool_call", toolCall: { id: "tc-s1", name: "subsession_run", arguments: { input: "子任务" } } } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      return [{ type: "text", text: "父会话收尾" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.store.setTodos(session.id, [{ id: "t1", title: "父任务", status: "completed", priority: "medium" }])
      await h.engine.run(session.id, "default", "派个子会话")
      // 子会话首轮上下文不含父会话待办；其 todo 结果只见自己的清单
      const subChats = h.provider.seenChats.filter((m) => isSubChat(m))
      expect(JSON.stringify(subChats[0])).not.toContain("父任务")
      expect(JSON.stringify(subChats[1])).toContain("子任务A")
      expect(JSON.stringify(subChats[1])).not.toContain("父任务")
      // 父会话待办清单不被子会话改动（不回流、不落盘）
      const todos = await h.store.getTodos(session.id, "default")
      expect(todos.map((t: TodoItem) => t.title)).toEqual(["父任务"])
      // 待办事件带子会话标记（前端不覆盖父会话面板）
      const ev = h.events.find((e) => e.type === "event.todo.update")
      expect(ev?.payload.subSession).toBe(true)
      expect(ev?.payload.subSessionId).toBeTruthy()
    } finally {
      h.cleanup()
    }
  })

  test("异步 fork：子会话主动合入阶段性成果（subsession_merge）+ 兄弟子会话感知", async () => {
    let h!: Harness
    const state: Record<string, number> = {}
    const waitEvent = async (pred: (e: AgentEvent) => boolean) => {
      for (let i = 0; i < 200; i++) {
        if (h.events.some(pred)) return
        await sleep(20)
      }
      throw new Error("等待事件超时")
    }
    const parentRound = makeParentRound()
    h = await setupSub(async (msgs) => {
      if (parentRound(msgs) === 1) {
        return [
          {
            type: "tool_call",
            toolCall: {
              id: "tc-sr",
              name: "subsession_run",
              arguments: { subsessions: [{ name: "左路", input: "调研方案A" }, { name: "右路", input: "调研方案B" }], inherit_context: true, async: true },
            },
          } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      if (isSubChat(msgs) && hasUserText(msgs, "调研方案A")) {
        state.A = (state.A ?? 0) + 1
        if (state.A === 1) {
          // 左路第一轮：主动合入阶段性成果（异步运行注入 subsession_merge），随后继续
          return [{ type: "tool_call", toolCall: { id: "tc-am", name: "subsession_merge", arguments: { content: "A的中期发现：方案A可行且成本低" } } }, { type: "done" }] as LLMChunk[]
        }
        return [{ type: "text", text: "左路最终报告" }, { type: "done" }] as LLMChunk[]
      }
      if (isSubChat(msgs) && hasUserText(msgs, "调研方案B")) {
        state.B = (state.B ?? 0) + 1
        if (state.B === 1) {
          // 右路第一轮：等左路阶段性合入事件后消耗一轮工具调用，通知在下一轮轮首注入
          await waitEvent((e) => e.type === "event.subsession.merged" && String(e.payload.text ?? "").includes("阶段性合入"))
          return [{ type: "tool_call", toolCall: { id: "tc-b1", name: "todo", arguments: { entries: [] } } }, { type: "done" }] as LLMChunk[]
        }
        return [{ type: "text", text: "右路最终报告" }, { type: "done" }] as LLMChunk[]
      }
      // 父会话：等阶段性合入出现后收尾（合并消息经工具批处理边界排空进入父上下文）
      await waitEvent((e) => e.type === "event.subsession.merged" && String(e.payload.text ?? "").includes("阶段性合入"))
      return [{ type: "text", text: "父会话总结完成" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "做两个方案的调研")
      await waitEvent((e) => e.type === "event.subsession.merged" && String(e.payload.text ?? "").includes("右路"))
      await sleep(150)
      const msgs = (await h.store.load(session.id, "default"))!.messages
      const interim = msgs.find((m) => m.content.includes("阶段性合入"))
      expect(interim).toBeDefined()
      expect(interim?.content).toContain("A的中期发现：方案A可行且成本低")
      expect(interim?.subSessionMerged?.name).toBe("左路")
      expect(interim?.subSessionArchive).toBeUndefined() // 子会话仍在执行，阶段性合入不带活引用存档
      expect(interim?.role).toBe("user")
      expect(interim?.engineNote).toBe("subsession")
      // 左路合入后继续运行并给出最终报告
      expect(msgs.some((m) => m.subSessionMerged?.name === "左路" && m.content.includes("左路最终报告"))).toBe(true)
      expect(msgs.some((m) => m.subSessionMerged?.name === "右路" && m.content.includes("右路最终报告"))).toBe(true)
      // 合入进父上下文与落盘同形（user + engineNote）
      expect(h.provider.seenChats.some((c) => !isSubChat(c) && c.some((x) => typeof x.content === "string" && x.content.includes("【子会话「左路」")))).toBe(true)
      // 右路第二轮感知左路阶段性合入（通知注入其上下文）
      const bChats = h.provider.seenChats.filter((m) => isSubChat(m) && hasUserText(m, "调研方案B"))
      expect(bChats.length).toBeGreaterThanOrEqual(2)
      expect(JSON.stringify(bChats[1])).toContain("【子会话感知】子会话「左路」阶段性合入父会话")
      expect(JSON.stringify(bChats[1])).toContain("A的中期发现")
      // 左路自身不收到自己的合入通知（exceptRunId）
      const aChats = h.provider.seenChats.filter((m) => isSubChat(m) && hasUserText(m, "调研方案A"))
      expect(aChats.every((c) => !c.some((x) => x.role === "user" && typeof x.content === "string" && x.content.startsWith("【子会话感知】")))).toBe(true)
      // 异步运行注入合并工具
      const aIdx = h.provider.seenChats.findIndex((m) => isSubChat(m) && hasUserText(m, "调研方案A"))
      expect(h.provider.seenTools[aIdx]).toContain("subsession_merge")
    } finally {
      h.cleanup()
    }
  })

  test("subsession_merge 拉取：增量同步兄弟子会话合入全文，且增量去重；非子会话上下文不可用", async () => {
    let h!: Harness
    const state: Record<string, number> = {}
    const waitEvent = async (pred: (e: AgentEvent) => boolean) => {
      for (let i = 0; i < 200; i++) {
        if (h.events.some(pred)) return
        await sleep(20)
      }
      throw new Error("等待事件超时")
    }
    const parentRound = makeParentRound()
    h = await setupSub(async (msgs) => {
      if (parentRound(msgs) === 1) {
        return [
          {
            type: "tool_call",
            toolCall: {
              id: "tc-sr",
              name: "subsession_run",
              arguments: { subsessions: [{ name: "生产者", input: "生产任务" }, { name: "消费者", input: "消费任务" }], inherit_context: true, async: true },
            },
          } as LLMChunk,
          { type: "done" } as LLMChunk,
        ]
      }
      if (isSubChat(msgs) && hasUserText(msgs, "生产任务")) {
        state.P = (state.P ?? 0) + 1
        if (state.P === 1) {
          return [{ type: "tool_call", toolCall: { id: "tc-pm", name: "subsession_merge", arguments: { content: "生产者的详细发现：模块X存在循环依赖，重构路径为 A→B→C 三步" } } }, { type: "done" }] as LLMChunk[]
        }
        return [{ type: "text", text: "生产者完成" }, { type: "done" }] as LLMChunk[]
      }
      if (isSubChat(msgs) && hasUserText(msgs, "消费任务")) {
        state.C = (state.C ?? 0) + 1
        if (state.C === 1) {
          await waitEvent((e) => e.type === "event.subsession.merged" && String(e.payload.text ?? "").includes("阶段性合入"))
          return [{ type: "tool_call", toolCall: { id: "tc-c1", name: "todo", arguments: { entries: [] } } }, { type: "done" }] as LLMChunk[]
        }
        if (state.C === 2) return [{ type: "tool_call", toolCall: { id: "tc-cs", name: "subsession_merge", arguments: {} } }, { type: "done" }] as LLMChunk[]
        if (state.C === 3) return [{ type: "tool_call", toolCall: { id: "tc-cs2", name: "subsession_merge", arguments: {} } }, { type: "done" }] as LLMChunk[]
        return [{ type: "text", text: "消费者完成" }, { type: "done" }] as LLMChunk[]
      }
      return [{ type: "text", text: "父会话总结完成" }, { type: "done" }] as LLMChunk[]
    })
    try {
      const session = await h.store.createSession("default", "t")
      await h.engine.run(session.id, "default", "两子会话协作")
      await waitEvent((e) => e.type === "event.subsession.merged" && String(e.payload.text ?? "").includes("消费者"))
      const cChats = h.provider.seenChats.filter((m) => isSubChat(m) && hasUserText(m, "消费任务"))
      expect(cChats.length).toBeGreaterThanOrEqual(4)
      const syncRound = JSON.stringify(cChats[2])
      expect(syncRound).toContain("subsession_merge")
      expect(syncRound).toContain("【合并·生产者】")
      expect(syncRound).toContain("模块X存在循环依赖")
      // 二次拉取：增量为空（去重不重复出现）
      const secondSyncRound = JSON.stringify(cChats[3])
      expect(secondSyncRound).toContain("暂无新消息")
      expect(secondSyncRound.split("模块X存在循环依赖").length - 1).toBe(2) // 通知一次 + 首次同步一次
      // 非子会话上下文不可用
      const { subSessionMergeTool } = await import("../tools")
      const r = await subSessionMergeTool.execute({ content: "x" }, {
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
        waitForChoice: async () => null,
        waitForEnv: async () => false,
        waitForDraw: async () => null,
        waitForCapture: async () => null,
      })
      expect(r.output).toContain("仅在异步子会话运行内可用")
    } finally {
      h.cleanup()
    }
  })
})
