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
import { sessionPath } from "./paths"
import type { LLMProvider, LLMChunk, ChatOptions } from "./llm"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"

class HardenProvider implements LLMProvider {
  readonly id = "fake"
  calls = 0
  seen: MessageLike[][] = []
  /** 每次模型调用的工具 schema 清单（name+parameters）：装载可见性/双胞胎合并断言用。 */
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

async function setupEngine(provider: HardenProvider, opts: Partial<ConstructorParameters<typeof AgentEngine>[0]> = {}) {
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

describe("装载工具会话可见性与双胞胎合并", () => {
  test("装载 code 的会话：同名工具合并为全局名（带 project 参数），独有工具前缀可见；未装载会话不见 code_* 且目录仍列出 code", async () => {
    const provider = new HardenProvider()
    provider.script = [{ mode: "text", text: "done" }]
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    const b = await store.createSession("default", "b")
    await engine.loadAgentToSession(a.id, "default", "code")
    await engine.run(a.id, "default", "hi")
    // A：双胞胎合并——read 是全局名但 schema 为 code 版（带 project 参数）；code_read 不再单出；独有工具前缀可见
    const sA = provider.toolSchemas[0]
    const readA = sA.find((t) => t.name === "read")!
    expect(readA).toBeTruthy()
    expect((readA.parameters as { properties: Record<string, unknown> }).properties.project).toBeTruthy()
    expect(sA.some((t) => t.name === "code_read")).toBe(false)
    expect(sA.some((t) => t.name === "code_write")).toBe(false)
    expect(sA.some((t) => t.name === "code_git")).toBe(true)
    // A 的系统提示词目录不含 code（本会话已装载，完整提示词在会话记录里）
    const sysA = String(provider.seen[0][0].content)
    expect(sysA).not.toContain("- code:")
    // B：未装载——无任何 code_* schema；read 无 project 参数；目录仍列出 code 供装载（跨会话不泄漏）
    await engine.run(b.id, "default", "hi")
    const sB = provider.toolSchemas[provider.toolSchemas.length - 1]
    expect(sB.some((t) => t.name.startsWith("code_"))).toBe(false)
    const readB = sB.find((t) => t.name === "read")!
    expect((readB.parameters as { properties: Record<string, unknown> }).properties.project).toBeUndefined()
    const sysB = String(provider.seen[provider.seen.length - 1][0].content)
    expect(sysB).toContain("- code:")
    rmSync(home, { recursive: true, force: true })
  })

  test("双胞胎别名兼容：装载会话内以 code_read 前缀名调用仍可执行（历史消息/子Agent 提示词引用不破）", async () => {
    const provider = new HardenProvider()
    provider.script = [
      { mode: "tool", tool: "code_read", args: { path: "hello.txt" } },
      { mode: "text", text: "done" },
    ]
    const { home, store, engine } = await setupEngine(provider)
    const a = await store.createSession("default", "a")
    await engine.loadAgentToSession(a.id, "default", "code")
    const ws = join(sessionPath(home, "default", a.id), "tmp")
    mkdirSync(ws, { recursive: true })
    writeFileSync(join(ws, "hello.txt"), "hello gebai\n")
    await engine.run(a.id, "default", "hi")
    const msgs = (await store.load(a.id, "default"))!.messages
    const toolMsg = msgs.find((m) => m.role === "tool")
    expect(toolMsg).toBeTruthy()
    expect(toolMsg!.content).toContain("hello gebai")
    expect(toolMsg!.content).not.toContain("未知工具")
    rmSync(home, { recursive: true, force: true })
  })

  test("未装载会话调用 code_read：路由自愈按会话装载后经别名执行", async () => {
    const provider = new HardenProvider()
    provider.script = [
      { mode: "tool", tool: "code_read", args: { path: "hello.txt" } },
      { mode: "text", text: "done" },
    ]
    const { home, store, engine } = await setupEngine(provider)
    const b = await store.createSession("default", "b")
    const ws = join(sessionPath(home, "default", b.id), "tmp")
    mkdirSync(ws, { recursive: true })
    writeFileSync(join(ws, "hello.txt"), "self-heal\n")
    await engine.run(b.id, "default", "hi")
    const msgs = (await store.load(b.id, "default"))!.messages
    const toolMsg = msgs.find((m) => m.role === "tool")
    expect(toolMsg).toBeTruthy()
    expect(toolMsg!.content).toContain("self-heal")
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
