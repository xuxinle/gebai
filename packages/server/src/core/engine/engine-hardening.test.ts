import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { loadConfig } from "../base/config"
import { SessionStore, estimateCharsTokens, estimateCtxTokens } from "../session/store"
import { ToolRegistry } from "../base/registry"
import { createGlobalTools } from "../tools"
import { Sandbox } from "../security/sandbox"
import { EnvManager } from "../session/env"
import { EventBus } from "../base/event-bus"
import { SubAgentManager } from "../agents/subagents"
import { AgentEngine } from "./engine"
import { sessionPath } from "../base/paths"
import type { LLMProvider, LLMChunk, ChatOptions } from "../llm/llm"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"

class HardenProvider implements LLMProvider {
  readonly id = "fake"
  calls = 0
  seen: MessageLike[][] = []
  /** 每次模型调用的工具 schema 清单（name+parameters）：装载可见性/全局工具继承断言用。 */
  toolSchemas: Array<Array<{ name: string; parameters: Record<string, unknown> }>> = []
  /** 每轮行为：{ mode: "text" | "tool" | "hang" | "badargs" | "trunc", tool?, args?, text?, raw?, stopReason? }，按 calls 序号。 */
  script: Array<{ mode: "text" | "tool" | "hang" | "badargs" | "trunc"; tool?: string; args?: Record<string, unknown>; text?: string; raw?: string; stopReason?: string }> = []
  maxCtx = 100000
  usageInput = 0
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: true, multimodal: true, maxContextTokens: this.maxCtx }
  }
  async *chat(msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.calls++
    this.seen.push(msgs)
    this.toolSchemas.push(((_opts?.tools ?? []) as Array<{ name: string; parameters: Record<string, unknown> }>).map((t) => ({ name: t.name, parameters: t.parameters as Record<string, unknown> })))
    const step = this.script[Math.min(this.calls - 1, this.script.length - 1)] ?? { mode: "text" as const }
    if (step.mode === "hang") {
      yield { type: "text", text: "partial output" }
      // 此后不再产出：模拟接口假死（挂起直到读空闲超时中止）
      await new Promise(() => {})
    }
    if (step.mode === "tool") {
      // offset 随轮次变化：read 参数签名不同（重复检测不会中断连续读同一文件）
      yield { type: "tool_call", toolCall: { id: `tc-${this.calls}`, name: step.tool ?? "read", arguments: step.args ?? { path: "tmp/x.txt", offset: this.calls } } }
      yield { type: "done", usage: { inputTokens: this.usageInput } }
      return
    }
    if (step.mode === "badargs") {
      yield { type: "tool_call", toolCall: { id: `tc-bad-${this.calls}`, name: "read", arguments: {} }, toolArgsError: "{path: 非法JSON" }
      yield { type: "done" }
      return
    }
    if (step.mode === "trunc") {
      // 输出上限截断：参数 JSON 未生成完（raw 为截断前缀），finish_reason=length
      yield { type: "tool_call", toolCall: { id: `tc-trunc-${this.calls}`, name: step.tool ?? "write", arguments: {}, ...(step.raw ? { raw: step.raw } : {}) }, toolArgsError: "{path: 截断" }
      yield { type: "done", ...(step.stopReason ? { stopReason: step.stopReason } : {}) }
      return
    }
    yield { type: "text", text: step.text ?? `final ${this.calls}` }
    yield { type: "done", usage: { inputTokens: this.usageInput } }
  }
}

async function setupEngine(provider: LLMProvider, opts: Partial<ConstructorParameters<typeof AgentEngine>[0]> = {}) {
  const home = mkdtempSync(join(tmpdir(), "gebai-harden-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  const config = loadConfig({ gebaiHome: home, auth: "local", sandbox: "off", preloadSubAgents: [], binaryMode: false })
  const store = new SessionStore({ home })
  const registry = new ToolRegistry()
  for (const tool of Object.values(createGlobalTools())) registry.register(tool)
  const sandbox = new Sandbox({ home, enabled: false })
  const env = new EnvManager(store)
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

describe("大文件生成截断防护", () => {
  test("write 参数被输出上限截断：抢救落盘已生成部分，回传 append 续写引导", async () => {
    const provider = new HardenProvider()
    // content 字符串中途截断（JSON 转义形态的换行，前缀可抢救）
    const raw = '{"path":"big.txt","content":"' + "line\\n".repeat(10) + "partial"
    provider.script = [{ mode: "trunc", raw, stopReason: "length" }, { mode: "text", text: "done" }]
    const { home, store, engine } = await setupEngine(provider)
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "hi")
    // 已生成部分先落盘（失败轮次转化为进度），完整内容 = 解码后的前缀
    const saved = await Bun.file(join(sessionPath(home, "default", session.id), "tmp", "big.txt")).text()
    expect(saved).toBe("line\n".repeat(10) + "partial")
    // 工具结果引导 append 续写（不引导重新整体输出）
    const salvagedLen = "line\n".repeat(10).length + "partial".length
    const loaded = await store.load(session.id)
    const tools = loaded!.messages.filter((m) => m.role === "tool")
    expect(tools.some((m) => m.content.includes("append"))).toBe(true)
    expect(tools.some((m) => m.content.includes(`前 ${salvagedLen} 字符`))).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })

  test("非 write 工具截断：无抢救，文案区分截断引导（拆小操作/分段写入），任务正常收尾", async () => {
    const provider = new HardenProvider()
    provider.script = [{ mode: "trunc", tool: "read", stopReason: "max_tokens" }, { mode: "text", text: "ok" }]
    const { home, store, engine } = await setupEngine(provider)
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "hi")
    const loaded = await store.load(session.id)
    const tools = loaded!.messages.filter((m) => m.role === "tool")
    expect(tools.some((m) => m.content.includes("输出上限"))).toBe(true)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "ok")).toBe(true)
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
  test("全部用户消息无压缩空间时：接口拒绝 → 硬护栏裁剪最旧用户消息为占位，最新输入不被裁", async () => {
    const provider = new HardenProvider()
    provider.maxCtx = 800 // 极小窗口
    // 接口以上下文长度错误拒绝（真实大小信号）→ 压缩（无 assistant/tool 可压缩）→ 硬护栏降级
    let calls = 0
    const origChat = provider.chat.bind(provider)
    provider.chat = async function* (msgs: MessageLike[], opts?: ChatOptions) {
      calls++
      if (calls <= 3) {
        throw new Error("模型接口错误（HTTP 400）: This model's maximum context length is 800 tokens. However, your messages resulted in 9000 tokens.")
      }
      yield* origChat(msgs, opts)
    }
    const { home, store, engine } = await setupEngine(provider)
    const session = await store.createSession("default", "t")
    // 预置多条历史用户消息（无 assistant/tool → 无可压缩内容；>500 字符才可被护栏裁剪）
    for (let i = 0; i < 5; i++) {
      await store.appendMessage(session.id, { id: `old-${i}`, role: "user", content: `历史用户消息${i}`.repeat(100), createdAt: Date.now() })
    }
    await engine.run(session.id, "default", `最新输入`.repeat(50))
    const loaded = await store.load(session.id)
    const users = loaded!.messages.filter((m) => m.role === "user")
    // 最旧用户消息被裁剪为占位（原文仍在会话存储中）
    expect(users[0].content).toContain("历史消息已裁剪")
    // 最新输入（本次任务）原样保留
    expect(users[users.length - 1].content.startsWith("最新输入")).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })
})

describe("任务中途自动压缩（真实 usage 驱动）", () => {
  test("真实 input tokens 超过窗口阈值时压缩最早历史，任务继续完成", async () => {
    // 每轮调用返回相同的真实 usage（8500 > 0.8 × 10000）：压缩判定只用模型服务返回的大小
    let calls = 0
    const provider = {
      id: "fake-midrun",
      capabilities: () => ({ streaming: true, toolCalling: true, multimodal: true, maxContextTokens: 10000 }),
      async *chat(_msgs: MessageLike[], _o?: ChatOptions): AsyncIterable<LLMChunk> {
        calls++
        if (calls <= 6) {
          // offset 随轮次变化（避免重复检测中断连续读同一文件）
          yield { type: "tool_call", toolCall: { id: `tc-${calls}`, name: "read", arguments: { path: "tmp/x.txt", offset: calls } } }
          yield { type: "done", usage: { inputTokens: 8500 } }
          return
        }
        yield { type: "text", text: "done" }
        yield { type: "done", usage: { inputTokens: 8500 } }
      },
    } as unknown as HardenProvider
    const { home, store, engine } = await setupEngine(provider, { authMode: "local" })
    const session = await store.createSession("default", "t")
    const tmp = store.getTmpDir(session.id, "default")
    mkdirSync(tmp, { recursive: true })
    writeFileSync(join(tmp, "x.txt"), "长内容\n".repeat(600))
    await engine.run(session.id, "default", "hi")
    const loaded = await store.load(session.id)
    // 中途压缩发生：最早历史被摘要替换（真实 usage 触发，压缩后基线锚点失效清除）
    expect(loaded!.messages.some((m) => m.compacted)).toBe(true)
    // 压缩后消息列表重建，任务继续执行：后续轮次的工具结果保留、最终回复收尾
    expect(loaded!.messages.filter((m) => m.role === "tool" && m.content.includes("长内容")).length).toBeGreaterThanOrEqual(1)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "done")).toBe(true)
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

describe("收尾验证提醒（改代码未跑测试的任务结束注入一次提醒）", () => {
  test("改了代码文件但未跑测试：任务结束注入一次验证提醒并续跑一轮（模型说明后收尾）", async () => {
    const provider = new HardenProvider()
    provider.script = [
      { mode: "tool", tool: "write", args: { path: "src/a.ts", content: "const x = 1\n" } },
      { mode: "text", text: "改完了" },
      { mode: "text", text: "好的，该改动不涉及行为" },
    ]
    const { home, store, engine } = await setupEngine(provider)
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "hi")
    const msgs = (await store.load(session.id, "default"))!.messages
    const nudge = msgs.find((m) => m.role === "user" && m.content.includes("【验证提醒】"))
    expect(nudge).toBeDefined()
    expect(nudge!.content).toContain("src/a.ts")
    expect(provider.calls).toBe(3) // 提醒额外触发一轮模型调用
    rmSync(home, { recursive: true, force: true })
  })

  test("已运行测试命令（sh 白名单免审形态）或仅改非代码文件：不触发提醒", async () => {
    // 用假 sh 工具替代真实执行：只需 command 文本命中验证判定（bun test），不真正跑命令
    const fakeRegistry = new ToolRegistry()
    for (const [n, t] of Object.entries(createGlobalTools())) if (n !== "sh") fakeRegistry.register(t)
    fakeRegistry.register({ name: "sh", description: "fake sh", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "ok" }) })
    const provider = new HardenProvider()
    provider.script = [
      { mode: "tool", tool: "write", args: { path: "src/a.ts", content: "const x = 1\n" } },
      { mode: "tool", tool: "sh", args: { command: "bun test src/a.test.ts", approval: false } },
      { mode: "text", text: "完成并已验证" },
    ]
    const { home, store, engine } = await setupEngine(provider, { registry: fakeRegistry })
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "hi")
    const msgs1 = (await store.load(session.id, "default"))!.messages
    expect(msgs1.some((m) => m.role === "user" && m.content.includes("【验证提醒】"))).toBe(false)
    // 仅文档文件（report.md）：不计入代码文件，不触发
    const provider2 = new HardenProvider()
    provider2.script = [
      { mode: "tool", tool: "write", args: { path: "report.md", content: "说明文档" } },
      { mode: "text", text: "完成" },
    ]
    const s2 = await store.createSession("default", "t2")
    await engine.run(s2.id, "default", "hi2")
    const msgs2 = (await store.load(s2.id, "default"))!.messages
    expect(msgs2.some((m) => m.role === "user" && m.content.includes("【验证提醒】"))).toBe(false)
    rmSync(home, { recursive: true, force: true })
  })
})

describe("装载工具会话可见性与全局工具复用", () => {
  test("装载 code 的会话：独有工具前缀可见、重复工具不再注册（全局名直接用）；未装载会话不见 code_* 且目录仍列出 code", async () => {
    const provider = new HardenProvider()
    provider.script = [{ mode: "text", text: "done" }]
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    const b = await store.createSession("default", "b")
    await engine.loadAgentToSession(a.id, "default", "code")
    await engine.run(a.id, "default", "hi")
    // A：独有工具前缀可见；code 不再定义 read/write 等重复工具（code_read/code_write 不存在）
    const sA = provider.toolSchemas[0]
    const readA = sA.find((t) => t.name === "read")!
    expect(readA).toBeTruthy()
    // 全局文件工具自带 project 参数（默认会话相对路径，项目相对须指定项目名/路径）
    expect((readA.parameters as { properties: Record<string, unknown> }).properties.project).toBeTruthy()
    expect(sA.some((t) => t.name === "code_read")).toBe(false)
    expect(sA.some((t) => t.name === "code_write")).toBe(false)
    expect(sA.some((t) => t.name === "code_git")).toBe(true)
    expect(sA.some((t) => t.name === "code_search_symbols")).toBe(true)
    // A 的系统提示词目录不含 code（本会话已装载，完整提示词在会话记录里）
    const sysA = String(provider.seen[0][0].content)
    expect(sysA).not.toContain("- code:")
    // B：未装载——无任何 code_* schema；全局 read 同样带 project 参数；目录仍列出 code 供装载（跨会话不泄漏）
    await engine.run(b.id, "default", "hi")
    const sB = provider.toolSchemas[provider.toolSchemas.length - 1]
    expect(sB.some((t) => t.name.startsWith("code_"))).toBe(false)
    const readB = sB.find((t) => t.name === "read")!
    expect((readB.parameters as { properties: Record<string, unknown> }).properties.project).toBeTruthy()
    const sysB = String(provider.seen[provider.seen.length - 1][0].content)
    expect(sysB).toContain("- code:")
    rmSync(home, { recursive: true, force: true })
  })

  test("code_read 前缀名已废除：装载会话内调用报未知工具并列出 code 的可用工具（自愈装载后仍不可解析）", async () => {
    const provider = new HardenProvider()
    provider.script = [
      { mode: "tool", tool: "code_read", args: { path: "hello.txt" } },
      { mode: "text", text: "done" },
    ]
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    await engine.loadAgentToSession(a.id, "default", "code")
    await engine.run(a.id, "default", "hi")
    const msgs = (await store.load(a.id, "default"))!.messages
    const toolMsg = msgs.find((m) => m.role === "tool")
    expect(toolMsg).toBeTruthy()
    expect(toolMsg!.content).toContain("未知工具")
    // 恢复面：错误信息附 code 的可用工具全名清单（独有工具），引导改用全局 read
    expect(toolMsg!.content).toContain("code_search_symbols")
    rmSync(home, { recursive: true, force: true })
  })

  test("agent_run 全局工具继承：默认继承（新会话可直接用全局 write），inherit_global_tools=false 时不继承", async () => {
    const provider = new HardenProvider()
    provider.script = [
      // 主会话：agent_run 预加载 code（默认继承全局工具）
      { mode: "tool", tool: "agent_run", args: { agents: ["code"], input: "write a file" } },
      // 新会话：直接调用全局 write（继承形态）后收尾
      { mode: "tool", tool: "write", args: { path: "inherited.txt", content: "from child" } },
      { mode: "text", text: "child done" },
      { mode: "text", text: "main done" },
    ]
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    await engine.run(a.id, "default", "hi")
    // 新会话内全局 write 已执行（写入会话工作区——agent_run 新会话与主会话共用 sessionId 工作区）
    const ws = join(sessionPath(home, "default", a.id), "tmp")
    expect(await Bun.file(join(ws, "inherited.txt")).text()).toBe("from child")
    // 新会话 schema：全局工具齐全 + code 独有工具前缀（第二次模型调用的 schema 清单）
    const childSchemas = provider.toolSchemas[1]
    expect(childSchemas.some((t) => t.name === "write")).toBe(true)
    expect(childSchemas.some((t) => t.name === "grep")).toBe(true)
    expect(childSchemas.some((t) => t.name === "code_search_symbols")).toBe(true)
    // 关闭继承：新会话仅剩子Agent 独有工具 + 内建编排，全局 write 不可见（调用报未知工具）
    const provider2 = new HardenProvider()
    provider2.script = [
      { mode: "tool", tool: "agent_run", args: { agents: ["code"], input: "write a file", inherit_global_tools: false } },
      { mode: "tool", tool: "write", args: { path: "no-inherit.txt", content: "x" } },
      { mode: "text", text: "child done" },
      { mode: "text", text: "main done" },
    ]
    const { home: home2, store: store2, engine: engine2 } = await setupEngine(provider2)
    const b = await store2.createSession("default", "b")
    await engine2.run(b.id, "default", "hi")
    const ws2 = join(sessionPath(home2, "default", b.id), "tmp")
    expect(await Bun.file(join(ws2, "no-inherit.txt")).exists()).toBe(false)
    const childSchemas2 = provider2.toolSchemas[1]
    expect(childSchemas2.some((t) => t.name === "write")).toBe(false)
    expect(childSchemas2.some((t) => t.name === "code_search_symbols")).toBe(true)
    // 未继承时全局 write 调用报未知工具（模型在下一轮收尾）
    const msgs2 = (await store2.load(b.id, "default"))!.messages
    const callMsg = msgs2.find((m) => m.role === "tool" && m.name === "agent_run" && m.sessionRun)
    expect(callMsg!.sessionRun!.messages.some((m) => m.role === "tool" && m.content.includes("未知工具"))).toBe(true)
    rmSync(home, { recursive: true, force: true })
    rmSync(home2, { recursive: true, force: true })
  })

  test("agent_run 全局提示词注入：默认注入（与全局工具继承一致，新会话与主会话同构），inherit_global_prompt=false 时仅子Agent 段", async () => {
    const provider = new HardenProvider()
    provider.script = [
      { mode: "tool", tool: "agent_run", args: { agents: ["code"], input: "do" } },
      { mode: "text", text: "child done" },
      { mode: "text", text: "main done" },
    ]
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    await engine.run(a.id, "default", "hi")
    const sysDefault = String(provider.seen[1][0].content)
    // 默认（inherit_global_prompt 缺省）：总Agent 提示词单源复用 buildSystemPrompt，位于子Agent 段之前
    expect(sysDefault).toContain("已预加载子Agent")
    expect(sysDefault).toContain("总Agent 全局系统提示词")
    expect(sysDefault).toContain("任务类型路由")
    expect(sysDefault.indexOf("总Agent 全局系统提示词")).toBeLessThan(sysDefault.indexOf("### code"))

    const provider2 = new HardenProvider()
    provider2.script = [
      { mode: "tool", tool: "agent_run", args: { agents: ["code"], input: "do", inherit_global_prompt: false } },
      { mode: "text", text: "child done" },
      { mode: "text", text: "main done" },
    ]
    const { home: home2, store: store2, engine: engine2 } = await setupEngine(provider2)
    const b = await store2.createSession("default", "b")
    await engine2.run(b.id, "default", "hi")
    const sysOff = String(provider2.seen[1][0].content)
    // 显式关闭：仅子Agent 提示词（上下文最省）
    expect(sysOff).toContain("已预加载子Agent")
    expect(sysOff).not.toContain("总Agent 全局系统提示词")
    expect(sysOff).not.toContain("任务类型路由")
    rmSync(home, { recursive: true, force: true })
    rmSync(home2, { recursive: true, force: true })
  })

  test("agent_run 绑定项目根的新会话：search_symbols 扫描项目文件树（listFiles 随 resolveBase 切换），而非仅会话 tmp", async () => {
    const projectRoot = mkdtempSync(join(tmpdir(), "gebai-harden-proj-"))
    writeFileSync(join(projectRoot, "app.ts"), "export function findMeInProject() {\n  return 1\n}\n")
    const provider = new HardenProvider()
    provider.script = [
      { mode: "tool", tool: "agent_run", args: { agents: ["code"], input: "search" } },
      { mode: "tool", tool: "code_search_symbols", args: { symbol: "findMeInProject" } },
      { mode: "text", text: "child done" },
      { mode: "text", text: "main done" },
    ]
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    await engine.run(a.id, "default", "hi", { envOverride: { CODE_PROJECT: projectRoot } })
    const msgs = (await store.load(a.id, "default"))!.messages
    const callMsg = msgs.find((m) => m.role === "tool" && m.name === "agent_run" && m.sessionRun)!
    const sym = callMsg.sessionRun!.messages.find((m) => m.role === "tool" && m.content.includes("findMeInProject"))
    expect(sym).toBeTruthy()
    expect(sym!.content).toContain("app.ts")
    rmSync(projectRoot, { recursive: true, force: true })
    rmSync(home, { recursive: true, force: true })
  })

  test("预置项目保留名 tmp 在任务期被拒绝（前端注入 env 的兜底校验）", async () => {
    const provider = new HardenProvider()
    provider.script = [{ mode: "text", text: "done" }]
    const { home, store, engine, events } = await setupEngine(provider)
    const session = await store.createSession("default", "t")
    const errs: string[] = []
    const unsub = events.subscribe((e) => {
      if (e.sessionId === session.id && e.type === "event.task.error") errs.push(String((e.payload as { error?: string }).error ?? ""))
    })
    await engine.run(session.id, "default", "hi", { envOverride: { CODE_PROJECTS: '[{"name":"tmp","path":"/srv/app"}]' } })
    unsub()
    expect(errs.some((m) => m.includes("保留名"))).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })
})

describe("agent_run 异步后台运行（async:true + bg_task）", () => {
  /** 主/子会话双形态假模型：系统提示词含「临时新会话」判定子会话；主会话按 calls 序号走
   *  agent_run(async) → bg_task（id 从上一轮 agent_run 工具结果文本提取）→ 收尾。 */
  class AsyncRunProvider implements LLMProvider {
    readonly id = "fake"
    calls = 0
    childCalls = 0
    seen: MessageLike[][] = []
    /** 子会话首个模型调用挂起（直到取消信号），模拟长任务。 */
    hangChild = false
    /** 主会话第二个模型调用挂起（直到取消信号）——保持发起任务存活，测停止传播。 */
    hangMain = false
    /** 子会话挂起调用收到中止信号（父任务停止传播的观测点）。 */
    childAborted = false
    /** 主会话第二轮 bg_task 动作（wait/stop）。 */
    waitAction: "wait" | "stop" = "wait"
    capabilities(): LLMCapabilities {
      return { streaming: true, toolCalling: true, multimodal: true, maxContextTokens: 100000 }
    }
    async *chat(msgs: MessageLike[], opts?: ChatOptions): AsyncIterable<LLMChunk> {
      this.seen.push(msgs)
      // 子会话系统提示词以固定句式开头（主会话提示词的子Agent 目录也可能含「临时新会话」字样，不能 includes）
      const isChild = String(msgs[0]?.content ?? "").startsWith("你正在一个临时新会话中执行任务")
      if (isChild) {
        this.childCalls++
        if (this.childCalls === 1 && this.hangChild) {
          yield { type: "text", text: "child starting" }
          await new Promise<never>((_, reject) => {
            const t = setTimeout(() => reject(new Error("hang-release")), 10000)
            opts?.signal?.addEventListener("abort", () => {
              clearTimeout(t)
              this.childAborted = true
              reject(opts.signal!.reason instanceof Error ? opts.signal!.reason : new Error("cancelled"))
            }, { once: true })
          })
        }
        if (this.childCalls === 1) {
          yield { type: "tool_call", toolCall: { id: `child-tc-${this.childCalls}`, name: "write", arguments: { path: "async-proof.txt", content: "child was here" } } }
          yield { type: "done" }
          return
        }
        yield { type: "text", text: "child finished" }
        yield { type: "done" }
        return
      }
      this.calls++
      if (this.calls === 1) {
        yield { type: "tool_call", toolCall: { id: "tc-start", name: "agent_run", arguments: { agents: ["code"], input: "long job", async: true } } }
        yield { type: "done" }
        return
      }
      if (this.calls === 2) {
        if (this.hangMain) {
          await new Promise<never>((_, reject) => {
            const t = setTimeout(() => reject(new Error("hang-release")), 10000)
            opts?.signal?.addEventListener("abort", () => {
              clearTimeout(t)
              reject(opts.signal!.reason instanceof Error ? opts.signal!.reason : new Error("cancelled"))
            }, { once: true })
          })
        }
        const toolMsgs = msgs.filter((m) => m.role === "tool")
        const runId = String(toolMsgs[toolMsgs.length - 1]?.content ?? "").match(/runId: (r[0-9a-f]+)/)?.[1] ?? "r-none"
        yield { type: "tool_call", toolCall: { id: `tc-manage-${this.calls}`, name: "bg_task", arguments: { action: this.waitAction, id: runId, timeout: 20 } } }
        yield { type: "done" }
        return
      }
      yield { type: "text", text: "main done" }
      yield { type: "done" }
    }
  }

  /** 轮询等待条件成立（后台运行跨任务异步推进，无法单点 await）。 */
  async function waitFor(cond: () => boolean, timeoutMs = 3000): Promise<void> {
    const deadline = Date.now() + timeoutMs
    while (!cond()) {
      if (Date.now() >= deadline) throw new Error("waitFor 超时")
      await new Promise((r) => setTimeout(r, 25))
    }
  }

  test("async:true 立即返回 runId 不阻塞；bg_task wait 取回最终结果与完整存档（回放扩展字段）", async () => {
    const provider = new AsyncRunProvider()
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    await engine.run(a.id, "default", "hi")
    // 子会话在新会话共享工作区完成 write（后台运行真实执行）
    const ws = join(sessionPath(home, "default", a.id), "tmp")
    expect(await Bun.file(join(ws, "async-proof.txt")).text()).toBe("child was here")
    const msgs = (await store.load(a.id, "default"))!.messages
    // agent_run 异步启动记录（立即返回，不携带存档）
    const startMsg = msgs.find((m) => m.role === "tool" && m.name === "agent_run")
    expect(startMsg!.content).toContain("后台子Agent 运行已启动")
    expect(startMsg!.content).toMatch(/runId: r[0-9a-f]+/)
    expect(startMsg!.sessionRun).toBeUndefined()
    // bg_task wait 终态记录：取回最终结果 + 完整存档（历史回放扩展字段）
    const waitMsg = msgs.find((m) => m.role === "tool" && m.name === "bg_task")!
    expect(waitMsg.content).toContain("后台运行")
    expect(waitMsg.content).toContain("child finished")
    expect(waitMsg.sessionRun).toBeTruthy()
    expect(waitMsg.sessionRun!.output).toBe("child finished")
    expect(waitMsg.sessionRun!.messages.some((m) => m.role === "tool" && m.content.includes("async-proof.txt"))).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })

  test("bg_task stop 主动终止运行中的后台任务：状态落定 cancelled、存档保留供回放", async () => {
    const provider = new AsyncRunProvider()
    provider.hangChild = true
    provider.waitAction = "stop"
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    await engine.run(a.id, "default", "hi")
    const msgs = (await store.load(a.id, "default"))!.messages
    const cancelMsg = msgs.find((m) => m.role === "tool" && m.name === "bg_task")!
    expect(cancelMsg.content).toContain("已终止")
    // 取消发生在子会话首个模型调用期间（存档仅含初始输入），存档仍随终止结果回传（过程保留语义）
    expect(cancelMsg.sessionRun).toBeTruthy()
    expect(cancelMsg.sessionRun!.input).toBe("long job")
    expect(cancelMsg.sessionRun!.messages[0].role).toBe("user")
    // write 未执行（任务被拦截在模型调用阶段）
    const ws = join(sessionPath(home, "default", a.id), "tmp")
    expect(await Bun.file(join(ws, "async-proof.txt")).exists()).toBe(false)
    rmSync(home, { recursive: true, force: true })
  })

  test("用户停止发起任务连带终止后台运行（父任务取消信号传播）", async () => {
    const provider = new AsyncRunProvider()
    provider.hangChild = true
    provider.hangMain = true // 发起任务在启动后台运行后持续存活（第二个模型调用挂起）
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    const runPromise = engine.run(a.id, "default", "hi")
    // 后台运行已启动（子会话首个模型调用挂起中）
    await waitFor(() => provider.childCalls >= 1)
    engine.cancel(a.id) // 用户停止
    await runPromise
    // 父任务取消信号传播：后台运行连带终止（子会话挂起的模型调用收到中止）
    await waitFor(() => provider.childAborted)
    rmSync(home, { recursive: true, force: true })
  })

  test("异步运行与主任务并行流式：session 快照不覆盖主任务在途快照，clearStream 只清本 run", async () => {
    // 主任务挂起存活（hangMain）+ 后台运行执行中 = 真正并行形态；快照隔离语义用引擎私有方法直调验证
    const provider = new AsyncRunProvider()
    provider.hangMain = true
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    const runPromise = engine.run(a.id, "default", "hi")
    // 后台运行已启动（子会话首轮模型调用完成、其 session 快照可能已建）后，注入主任务流式快照
    await waitFor(() => provider.childCalls >= 1)
    const internals = engine as unknown as {
      noteStream: (sid: string, patch: { messageId?: string; text?: string; reasoning?: string; session?: boolean; sessionRunId?: string }) => void
      clearStream: (sid: string, sessionRunId?: string) => void
    }
    internals.noteStream(a.id, { messageId: "main-msg", text: "主任务流式文本" })
    // 后台运行的 session delta 不覆盖主任务快照（并行保护）
    internals.noteStream(a.id, { messageId: "bg-msg", text: "后台运行文本", session: true, sessionRunId: "r-bg" })
    internals.noteStream(a.id, { messageId: "main-msg", text: "继续" })
    const snap = engine.attachSnapshot(a.id)
    expect(snap!.stream!.text).toBe("主任务流式文本继续")
    expect(snap!.stream!.session).toBeFalsy()
    // 后台运行的轮末清空不误清主任务快照（只清属于该 run 的 session 快照）
    internals.clearStream(a.id, "r-bg")
    expect(engine.attachSnapshot(a.id)!.stream).toBeTruthy()
    // 主任务自己的清空语义不变
    internals.clearStream(a.id)
    expect(engine.attachSnapshot(a.id)!.stream).toBeUndefined()
    engine.cancel(a.id)
    await runPromise
    rmSync(home, { recursive: true, force: true })
  })

  test("forgetSession 清理异步运行句柄：运行中的被终止、全部句柄移除（无泄漏）", async () => {
    const provider = new AsyncRunProvider()
    provider.hangChild = true
    provider.hangMain = true
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    const runPromise = engine.run(a.id, "default", "hi")
    await waitFor(() => provider.childCalls >= 1)
    const storeMap = (engine as unknown as { sessionRunStore: Map<string, { sessionId: string; status: string }> }).sessionRunStore
    expect(storeMap.size).toBeGreaterThan(0)
    expect([...storeMap.values()].every((h) => h.sessionId === a.id)).toBe(true)
    engine.forgetSession(a.id)
    expect(storeMap.size).toBe(0)
    // 运行中的后台运行被终止（子会话挂起的模型调用收到中止）
    await waitFor(() => provider.childAborted)
    engine.cancel(a.id)
    await runPromise
    rmSync(home, { recursive: true, force: true })
  })
})
