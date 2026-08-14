import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { loadConfig } from "./config"
import { SessionStore, estimateCharsTokens, estimateCtxTokens } from "./store"
import { ToolRegistry } from "./registry"
import { createGlobalTools } from "./tools"
import { Sandbox } from "./sandbox"
import { EnvManager } from "./env"
import { EventBus } from "./event-bus"
import { SubAgentManager } from "./subagents"
import { AgentEngine } from "./engine"
import type { LLMProvider, LLMChunk, ChatOptions } from "./llm"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"

class HardenProvider implements LLMProvider {
  readonly id = "fake"
  calls = 0
  seen: MessageLike[][] = []
  /** 每轮行为：{ mode: "text" | "tool" | "hang" | "badargs", tool?, argsError? }，按 calls 序号。 */
  script: Array<{ mode: "text" | "tool" | "hang" | "badargs"; tool?: string; text?: string }> = []
  maxCtx = 100000
  usageInput = 0
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: true, multimodal: true, maxContextTokens: this.maxCtx }
  }
  async *chat(msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.calls++
    this.seen.push(msgs)
    const step = this.script[Math.min(this.calls - 1, this.script.length - 1)] ?? { mode: "text" as const }
    if (step.mode === "hang") {
      yield { type: "text", text: "partial output" }
      // 此后不再产出：模拟接口假死（挂起直到读空闲超时中止）
      await new Promise(() => {})
    }
    if (step.mode === "tool") {
      // offset 随轮次变化：read 参数签名不同（重复检测不会中断连续读同一文件）
      yield { type: "tool_call", toolCall: { id: `tc-${this.calls}`, name: step.tool ?? "read", arguments: { path: "tmp/x.txt", offset: this.calls } } }
      yield { type: "done", usage: { inputTokens: this.usageInput } }
      return
    }
    if (step.mode === "badargs") {
      yield { type: "tool_call", toolCall: { id: `tc-bad-${this.calls}`, name: "read", arguments: {} }, toolArgsError: "{path: 非法JSON" }
      yield { type: "done" }
      return
    }
    yield { type: "text", text: step.text ?? `final ${this.calls}` }
    yield { type: "done", usage: { inputTokens: this.usageInput } }
  }
}

async function setupEngine(provider: HardenProvider, opts: Partial<ConstructorParameters<typeof AgentEngine>[0]> = {}) {
  const home = mkdtempSync(join(tmpdir(), "gebai-harden-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  const config = loadConfig({ gebaiHome: home, auth: "local", sandbox: "off", preloadSubAgents: [], binaryMode: false })
  const store = new SessionStore({ home })
  const registry = new ToolRegistry()
  for (const tool of Object.values(createGlobalTools())) registry.register(tool)
  const sandbox = new Sandbox({ home, enabled: false })
  const env = new EnvManager(home, store)
  const events = new EventBus()
  const subAgents = new SubAgentManager({ registry, preloadOverride: [] })
  await subAgents.discover()
  const engine = new AgentEngine({ provider, registry, store, env, sandbox, events, config, subAgents, retryBackoffMs: 5, ...opts })
  return { home, store, engine, events, registry, subAgents }
}

describe("CJK token 估算", () => {
  test("CJK 字符按 1 token、ASCII 按 4 字符 1 token（中文场景不再低估 2~4 倍）", () => {
    expect(estimateCharsTokens("你好世界")).toBe(4)
    expect(estimateCharsTokens("abcdefgh")).toBe(2)
    expect(estimateCharsTokens("中文mixed1234")).toBeGreaterThan(4)
    const msgs = [{ role: "user", content: "你好世界，这是一个中文测试。" }]
    expect(estimateCtxTokens(msgs as never)).toBeGreaterThan(8)
  })
})

describe("LLM 流式读空闲超时", () => {
  test("接口假死（有产出后挂起）被中止：任务以错误收尾而非无限挂起", async () => {
    const provider = new HardenProvider()
    provider.script = [{ mode: "hang" }]
    const { home, store, engine } = await setupEngine(provider, { llmIdleTimeoutMs: 60 })
    const session = await store.createSession("default", "t")
    const events: string[] = []
    const unsub = engine["opts"].events.subscribe((e) => {
      if (e.sessionId === session.id) events.push(e.type)
    })
    const t0 = Date.now()
    await engine.run(session.id, "default", "hi")
    unsub()
    expect(events).toContain("event.task.error")
    expect(Date.now() - t0).toBeLessThan(5000) // 中止而非挂起
    rmSync(home, { recursive: true, force: true })
  })
})

describe("工具参数 JSON 畸形回传", () => {
  test("接口聚合的参数非法 JSON：工具不执行，回传原始片段让模型修正后正常完成", async () => {
    const provider = new HardenProvider()
    provider.script = [{ mode: "badargs" }, { mode: "text", text: "corrected" }]
    const { home, store, engine } = await setupEngine(provider)
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "hi")
    const loaded = await store.load(session.id)
    const tools = loaded!.messages.filter((m) => m.role === "tool")
    expect(tools.some((m) => m.content.includes("参数 JSON 解析失败"))).toBe(true)
    // read 工具未实际执行（无成功执行记录）；第二轮模型被引导后正常收尾
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "corrected")).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })
})

describe("agent_run 加固", () => {
  test("预加载子Agent 数量上限（>5 拒绝）与去重", async () => {
    const provider = new HardenProvider()
    const { home, store, engine, subAgents } = await setupEngine(provider)
    for (const n of ["dummy_a", "dummy_b", "dummy_c", "dummy_d", "dummy_e", "dummy_f"]) {
      subAgents.register({ name: n, description: n, systemPrompt: n })
    }
    const session = await store.createSession("default", "t")
    const runNewSession = (engine as unknown as { runNewSession: (sid: string, user: string, env: Record<string, string>, agents: string[], input: string, signal: AbortSignal) => Promise<unknown> }).runNewSession.bind(engine) as unknown as (sid: string, user: string, env: Record<string, string>, agents: string[], input: string, signal: AbortSignal) => Promise<{ output: string; archive: { agents: string[] } }>
    // 6 个不同名：超上限拒绝
    await expect(runNewSession(session.id, "default", {}, ["dummy_a", "dummy_b", "dummy_c", "dummy_d", "dummy_e", "dummy_f"], "x", new AbortController().signal)).rejects.toThrow(/数量超限/)
    // 重复名去重后 3 个：正常执行完成（去重生效，不因重复名报错）
    const result = await runNewSession(session.id, "default", {}, ["dummy_a", "dummy_a", "dummy_b", "dummy_b", "dummy_c", "dummy_c"], "x", new AbortController().signal)
    expect(result.archive.agents).toEqual(["dummy_a", "dummy_b", "dummy_c"])
    rmSync(home, { recursive: true, force: true })
  })
})

describe("溢出硬护栏", () => {
  test("全部用户消息无压缩空间时：最旧用户消息裁剪为占位，最新输入不被裁", async () => {
    const provider = new HardenProvider()
    provider.maxCtx = 800 // 极小窗口强制触发压缩+护栏
    const { home, store, engine } = await setupEngine(provider)
    const session = await store.createSession("default", "t")
    // 预置多条历史用户消息（无 assistant/tool → 无可压缩内容；>500 字符才可被护栏裁剪）
    for (let i = 0; i < 5; i++) {
      await store.appendMessage(session.id, { id: `old-${i}`, role: "user", content: `历史用户消息${i}`.repeat(100), createdAt: Date.now() })
    }
    await engine.run(session.id, "default", `最新输入`.repeat(50))
    const loaded = await store.load(session.id)
    const users = loaded!.messages.filter((m) => m.role === "user")
    // 最旧用户消息被裁剪为占位
    expect(users[0].content).toContain("历史消息已裁剪")
    // 最新输入（本次任务）原样保留
    expect(users[users.length - 1].content.startsWith("最新输入")).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })
})

describe("任务中途工具结果回收", () => {
  test("真实 usage 逼近窗口上限时最早的工具结果回收为归档占位", async () => {
    const provider = new HardenProvider()
    provider.maxCtx = 10000
    provider.usageInput = 9500 // 超过 0.9 × 10000
    // 10 轮 read（结果 >800 字符）→ 保留最近 8 条，最早的被回收
    provider.script = Array.from({ length: 10 }, (): HardenProvider["script"][number] => ({ mode: "tool", tool: "read" }))
    provider.script.push({ mode: "text", text: "done" })
    const { home, store, engine } = await setupEngine(provider, { authMode: "local" })
    const session = await store.createSession("default", "t")
    const tmp = store.getTmpDir(session.id, "default")
    mkdirSync(tmp, { recursive: true })
    writeFileSync(join(tmp, "x.txt"), "长内容".repeat(600))
    await engine.run(session.id, "default", "hi")
    const loaded = await store.load(session.id)
    const toolMsgs = loaded!.messages.filter((m) => m.role === "tool")
    // 最早的超长结果被回收为占位
    expect(toolMsgs.some((m) => m.content.includes("已归档回收"))).toBe(true)
    // 最近的结果未被回收（近期操作上下文保留）
    const unreclaimed = toolMsgs.filter((m) => !m.content.includes("已归档回收"))
    expect(unreclaimed.length).toBe(9)
    rmSync(home, { recursive: true, force: true })
  })
})

describe("子Agent 卸载清理持久化痕迹", () => {
  test("unloadAgentFromSession 移除提示词消息与装载记录，工具注销", async () => {
    const provider = new HardenProvider()
    const { home, store, engine } = await setupEngine(provider)
    const session = await store.createSession("default", "t")
    await engine.loadAgentToSession(session.id, "default", "desktop")
    let loaded = await store.load(session.id)
    expect(loaded!.messages.some((m) => m.loadedAgent === "desktop")).toBe(true)
    expect(loaded!.loadedSubAgents).toContain("desktop")
    await engine.unloadAgentFromSession(session.id, "default", "desktop")
    loaded = await store.load(session.id)
    expect(loaded!.messages.some((m) => m.loadedAgent === "desktop")).toBe(false)
    expect(loaded!.loadedSubAgents).toBeUndefined()
    rmSync(home, { recursive: true, force: true })
  })
})

describe("历史图片内联窗口", () => {
  test("超过 3 组的历史图片消息降级为文本说明，仅最近 3 组内联", async () => {
    const provider = new HardenProvider()
    provider.maxCtx = 1000000
    const { home, store, engine } = await setupEngine(provider)
    const session = await store.createSession("default", "t")
    // 真实 PNG 字节写入会话 tmp（附件内联需要可读文件）
    const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==", "base64")
    const tmp = store.getTmpDir(session.id, "default")
    mkdirSync(tmp, { recursive: true })
    writeFileSync(join(tmp, "pic.png"), png)
    for (let i = 0; i < 5; i++) {
      await store.appendMessage(session.id, {
        id: `img-${i}`,
        role: "user",
        content: `看图${i}`,
        attachments: [{ name: "pic.png", mime: "image/png", path: "tmp/pic.png", size: png.length }],
        createdAt: Date.now(),
      })
    }
    await engine.run(session.id, "default", "再看一张", { attachments: [{ name: "pic.png", mime: "image/png", path: "tmp/pic.png" }] })
    // 模型最后收到的消息（第 1 轮无工具调用即结束）：统计 image 块数量
    const msgs = provider.seen[0]
    let imageBlocks = 0
    for (const m of msgs) {
      if (Array.isArray(m.content)) {
        imageBlocks += m.content.filter((b) => b.type === "image").length
      }
    }
    // 5 条历史 + 1 条新输入 = 6 组图片，仅最近 3 组内联
    expect(imageBlocks).toBe(3)
    rmSync(home, { recursive: true, force: true })
  })
})
