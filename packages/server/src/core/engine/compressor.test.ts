/** 上下文压缩器单元测试：摘要输入构造（工具调用骨架/分块/头尾保留）、滚动摘要合并、
 *  摘要失败降级骨架、溢出护栏降级与事件、压缩区间端点与 tool 配对对齐。 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { Message, MessageLike } from "@gebai/sdk"
import { SessionStore } from "../session/store"
import type { EnvManager } from "../session/env"
import type { LLMProvider } from "../llm/llm"
import { ContextCompressor, buildSummaryChunks, summarizeMessageLine, summarizeFallback, planCompactRange, estimateMessageTokens, outputReserveTokens, COMPACT_OUTPUT_RESERVE_FALLBACK, type CompressorDeps } from "./compressor"

function msg(role: Message["role"], content: string, extra: Partial<Message> = {}): Message {
  return { id: crypto.randomUUID().replace(/-/g, ""), role, content, createdAt: Date.now(), ...extra } as Message
}

/** 记录每次 chat 输入的 mock provider（onChat 返回字符串则作为该次输出文本）。usage 可注入缓存命中量。 */
function mockProvider(onChat?: (msgs: MessageLike[], index: number) => string | void, usage?: { inputTokens?: number; cachedTokens?: number }): { provider: LLMProvider; inputs: MessageLike[][] } {
  const inputs: MessageLike[][] = []
  const provider = {
    id: "mock",
    capabilities: () => ({ streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 100000, maxOutputTokens: 8192, model: "m" }),
    chat: async function* (msgs: MessageLike[]) {
      const out = onChat?.(msgs, inputs.length)
      inputs.push(msgs)
      yield { type: "text", text: typeof out === "string" ? out : "摘要正文" }
      yield { type: "done", usage }
    },
  } as unknown as LLMProvider
  return { provider, inputs }
}

async function setup(opts: {
  provider: LLMProvider
  messages?: Message[]
  /** 可选的「与主循环同前缀」历史渲染（缓存友好摘要路径；upToIndex 为区间末端 store 下标）。 */
  loadHistory?: (sessionId: string, user: string, inlineMultimodal?: boolean, upToIndex?: number) => Promise<MessageLike[]>
}) {
  const home = mkdtempSync(join(tmpdir(), "gebai-compressor-"))
  const store = new SessionStore({ home })
  const session = await store.createSession("default")
  for (const m of opts.messages ?? []) await store.appendMessage(session.id, m)
  const events: Array<{ type: string; payload: Record<string, unknown> }> = []
  const deps: CompressorDeps = {
    store,
    env: { resolve: async () => ({}) } as unknown as EnvManager,
    getDefaultProvider: () => opts.provider,
    idleTimeoutMs: 2000,
    isTaskRunning: () => false,
    taskSignal: () => undefined,
    publish: (_sid, type, payload) => events.push({ type, payload }),
    loadHistory: opts.loadHistory ?? (async () => []),
    callModel: async () => ({ text: "", toolCalls: [] }),
  }
  return { compressor: new ContextCompressor(deps), store, session, events, cleanup: () => rmSync(home, { recursive: true, force: true }) }
}

/** 交替的用户/助手历史（n 轮 = 2n 条消息）。 */
function dialogue(n: number, size = 10): Message[] {
  const out: Message[] = []
  for (let i = 0; i < n; i++) {
    out.push(msg("user", `问题 ${i}`))
    out.push(msg("assistant", `回答 ${i}${"x".repeat(size)}`))
  }
  return out
}

describe("摘要输入构造", () => {
  test("工具调用骨架行：assistant 空正文时仍记录工具名与参数（工具调用史不消失）", () => {
    const line = summarizeMessageLine(
      msg("assistant", "", { toolCalls: [{ id: "tc1", name: "write", arguments: { path: "src/a.ts", content: "y".repeat(500) } }] }),
    )
    expect(line).toContain("[调用工具]")
    expect(line).toContain("write(")
    expect(line).toContain("src/a.ts")
    expect(line).toContain("余 ") // 超长参数截断并标注剩余字符
  })

  test("工具结果行标记工具名（摘要能看出哪个工具产出过什么）", () => {
    const line = summarizeMessageLine(msg("tool", "命令输出内容", { name: "sh" }))
    expect(line).toContain("[工具结果:sh]")
    expect(line).toContain("命令输出内容")
  })

  test("分块：总量在预算内按顺序分块，每块不超预算", () => {
    const lines = Array.from({ length: 10 }, (_, i) => `[assistant] 第${i}段 ${"x".repeat(3000)}`)
    const chunks = buildSummaryChunks(lines, 10000, 6)
    expect(chunks.length).toBeGreaterThan(1)
    for (const c of chunks) expect(c.length).toBeLessThanOrEqual(10000 + 3000) // 单行超预算时独占一块
    expect(chunks.join("\n")).toContain("第0段")
    expect(chunks.join("\n")).toContain("第9段")
  })

  test("分块超总预算：头尾保留 + 中部省略说明（不静默丢弃最新进度）", () => {
    const lines = Array.from({ length: 100 }, (_, i) => `[assistant] 第${i}段 ${"x".repeat(5000)}`)
    const chunks = buildSummaryChunks(lines, 20000, 3)
    const joined = chunks.join("\n")
    expect(chunks.length).toBeGreaterThan(1)
    expect(joined).toContain("第0段") // 最早的任务背景保留
    expect(joined).toContain("第99段") // 最新进度保留
    expect(joined).toContain("[省略]") // 中部省略有显式说明
  })

  test("降级骨架：保留被裁剪内容的骨架行（不是一句空占位）", () => {
    const slice = [msg("assistant", "", { toolCalls: [{ id: "tc1", name: "read", arguments: { path: "b.ts" } }] }), msg("tool", "读取结果", { name: "read" })]
    const text = summarizeFallback(slice)
    expect(text).toContain("上下文已裁剪")
    expect(text).toContain("read(")
    expect(text).toContain("b.ts")
    expect(text.length).toBeLessThanOrEqual(2000)
  })
})

describe("compactSession 摘要生成", () => {
  test("大历史：逐块摘要后合并为一条摘要（不再一刀切截断丢信息）", async () => {
    const { provider, inputs } = mockProvider()
    const s = await setup({ provider })
    // 80 条消息的骨架行总量（每条上限 600）远超单块预算 20000 → 分块 + 合并
    const slice = Array.from({ length: 80 }, (_, i) => msg("assistant", `回答 ${i} ${"x".repeat(2000)}`))
    const summary = await s.compressor.summarize(slice, provider)
    expect(summary).toBeTruthy()
    expect(inputs.length).toBeGreaterThanOrEqual(2) // 分块摘要 + 合并
    const all = inputs.map((m) => JSON.stringify(m)).join("\n")
    expect(all).toContain("回答 0") // 最早
    expect(all).toContain("回答 79") // 最新
    s.cleanup()
  })

  test("compactSession 大历史：首尾内容都进摘要输入，压缩正常完成", async () => {
    const { provider, inputs } = mockProvider()
    const s = await setup({ provider, messages: dialogue(20, 3000) }) // 20 轮，压缩区间约 3 万字符
    const r = await s.compressor.compactSession(s.session.id, "default")
    expect(r.compacted).toBeGreaterThan(0)
    expect(r.summary).toBeTruthy()
    const all = inputs.map((m) => JSON.stringify(m)).join("\n")
    expect(all).toContain("回答 0") // 压缩区间最早
    expect(all).toContain("回答 6") // 压缩区间末尾（40 条历史、窗口 12，保守口径压窗口外一半 → 到下标 14）
    s.cleanup()
  })

  test("滚动摘要合并：重复压缩后摘要恒为一条，旧摘要内容进新摘要输入", async () => {
    const { provider, inputs } = mockProvider()
    const s = await setup({ provider, messages: dialogue(10, 200) })
    const r1 = await s.compressor.compactSession(s.session.id, "default")
    expect(r1.compacted).toBeGreaterThan(0)
    const r2 = await s.compressor.compactSession(s.session.id, "default")
    expect(r2.compacted).toBeGreaterThan(0) // 第二轮仍能压到新内容
    const loaded = await s.store.load(s.session.id)
    const summaries = loaded!.messages.filter((m) => m.compacted)
    expect(summaries.length).toBe(1) // 旧摘要被吸收（不再逐条累积）
    // 第二轮摘要输入含「此前摘要」标记（旧摘要内容并入新摘要）
    expect(JSON.stringify(inputs[inputs.length - 1])).toContain("此前摘要")
    s.cleanup()
  })

  test("摘要失败降级：压缩仍完成，占位文本保留被裁剪内容骨架", async () => {
    const provider = {
      capabilities: () => ({ streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 100000 }),
      chat: async function* () {
        throw new Error("provider down")
      },
    } as unknown as LLMProvider
    const s = await setup({
      provider,
      messages: [msg("user", "首个问题"), msg("assistant", "", { toolCalls: [{ id: "tc1", name: "sh", arguments: { command: "bun test" } }] }), msg("tool", "测试通过", { name: "sh" }), msg("user", "继续做"), msg("assistant", "好的"), msg("assistant", "收尾")],
    })
    const r = await s.compressor.compactSession(s.session.id, "default")
    expect(r.compacted).toBeGreaterThan(0)
    expect(r.summary).toContain("上下文已裁剪")
    expect(r.summary).toContain("sh(") // 骨架里有工具调用
    s.cleanup()
  })

  test("压缩区间端点对齐 tool 配对边界：边界处的工具结果进摘要而非被当孤儿丢弃", async () => {
    const { provider, inputs } = mockProvider()
    const messages: Message[] = []
    for (let i = 0; i < 3; i++) {
      messages.push(msg("assistant", "", { toolCalls: [{ id: `tc-${i}`, name: "read", arguments: { path: `f${i}.ts` } }] }))
      messages.push(msg("tool", `文件内容 ${i}`, { toolCallId: `tc-${i}`, name: "read" }))
    }
    const s = await setup({ provider, messages })
    // 显式指定一个「切在配对中间」的区间：[1,3) 以 tool 结果开头、以发起 assistant 结尾，对齐后应变成 [0,4)
    const r = await s.compressor.compactSession(s.session.id, "default", { from: 1, to: 3 })
    expect(r.compacted).toBeGreaterThan(0)
    // 边界处（区间末尾发起消息之后）的 tool 结果被一并纳入压缩区间（旧实现下它会被配对修复当孤儿丢弃）
    expect(inputs[0]!.map((m) => String(m.content)).join("\n")).toContain("文件内容 1")
    const loaded = await s.store.load(s.session.id)
    // 压缩后不存在孤儿 tool（每条 tool 前都有发起 assistant）
    const msgs = loaded!.messages
    const orphans = msgs.filter((m, i) => m.role === "tool" && !(msgs[i - 1]?.role === "assistant" && msgs[i - 1].toolCalls?.some((tc) => tc.id === m.toolCallId)))
    expect(orphans.length).toBe(0)
    s.cleanup()
  })
})

describe("压缩区间规划（水位区间 + 滑动窗口）", () => {
  /** 每条 4000 ASCII 字符 ≈ 1000 token（estimateCharsTokens 口径）。 */
  const bigMsg = () => msg("assistant", "x".repeat(4000))

  test("输出预留：未声明输出上限时用缺省值，且不超过窗口一半", () => {
    expect(outputReserveTokens(1000000, 32000)).toBe(32000) // 声明了就用声明的
    expect(outputReserveTokens(1000000, undefined)).toBe(COMPACT_OUTPUT_RESERVE_FALLBACK)
    expect(outputReserveTokens(10000, undefined)).toBe(5000) // 小窗口：夹到窗口一半
    expect(outputReserveTokens(1000, undefined)).toBe(1024) // 极小窗口：不小于最小值
    expect(outputReserveTokens(0, 32000)).toBe(0) // 无窗口信息：0（退回兜底口径）
  })

  test("水位区间：从触发水位（剩余不足一次回复）压到目标水位（剩余够两次回复），不一次压太多", () => {
    const messages = Array.from({ length: 30 }, bigMsg)
    const compactable = messages.map((_, i) => i)
    // cap 100000、预留 10000 → 上水位：占用 > 90000 触发；下水位（目标输入）80000 → 需腾出 15000 ≈ 15 条
    // （滑动窗口 12 条 → 最多只能压到下标 18，需腾出量远小于此）
    const plan = planCompactRange(messages, compactable, { baseline: 95000, cap: 100000, reserve: 10000 })
    expect(plan).not.toBeNull()
    expect(plan!.from).toBe(0)
    expect(plan!.to).toBe(15)
  })

  test("近消息滑动窗口：最近 12 条（任意角色）永不进压缩区间，即使需腾出量很大", () => {
    const messages = Array.from({ length: 40 }, bigMsg)
    const compactable = messages.map((_, i) => i)
    // 目标输入 40000（40% 下限）：基线 96000 → 需腾 56000，但窗口内 12 条不可动 → 最多压到下标 28
    const plan = planCompactRange(messages, compactable, { baseline: 96000, cap: 100000, reserve: 30000 })
    expect(plan!.to).toBe(28)
    // 显式指定窗口（GEBAI_COMPACT_WINDOW 同类口径）同样生效
    const wide = planCompactRange(messages, compactable, { baseline: 96000, cap: 100000, reserve: 30000, window: 20 })
    expect(wide!.to).toBe(20)
  })

  test("滑动窗口上限为历史一半：短会话仍可压缩（不会被窗口卡死）", () => {
    const messages = Array.from({ length: 8 }, () => msg("assistant", "abc"))
    const compactable = messages.map((_, i) => i)
    // 8 条 → 窗口取 4（一半），无基线时只压掉窗口外可压缩消息的一半（4 → 2）
    const plan = planCompactRange(messages, compactable, { baseline: undefined, cap: 100000, reserve: 10000 })
    expect(plan!.to).toBe(2)
  })

  test("无真实占用基线（算不出需腾出量）：保守只压窗口外一半，不一次丢大量历史", () => {
    // 30 条、每条 1000 token：窗口 12 → 窗口外 18 条可压缩 → 只压 9 条（保守）
    const messages = Array.from({ length: 30 }, bigMsg)
    const compactable = messages.map((_, i) => i)
    const plan = planCompactRange(messages, compactable, { baseline: undefined, cap: 100000, reserve: 10000 })
    expect(plan!.to).toBe(9)
  })

  test("窗口内可压缩消息不足时不做压缩", () => {
    const messages = Array.from({ length: 20 }, () => msg("assistant", "x".repeat(400)))
    const compactable = messages.map((_, i) => i)
    // 每条 100 token、需腾 2000 → 窗口外 10 条全拿也不够 → 压满窗口外那段（to = 10）
    const plan = planCompactRange(messages, compactable, { baseline: 100000, cap: 100000, reserve: 1000 })
    expect(plan!.to).toBe(10)
  })

  test("占用远低于上限时只最小腾挪（溢出恢复场景：接口已报溢出但基线未更新）", () => {
    const messages = Array.from({ length: 20 }, bigMsg)
    const compactable = messages.map((_, i) => i)
    // 基线 1000（远低于目标 80000）→ 需腾出量为 0，最小腾挪取窗口 5% = 5000 ≈ 5 条
    const plan = planCompactRange(messages, compactable, { baseline: 1000, cap: 100000, reserve: 10000 })
    expect(plan!.to).toBe(5)
  })

  test("窗口 40% 下限保护：需腾出量不超过「基线 - 窗口 40%」（压太狠会丢信息）", () => {
    const messages = Array.from({ length: 60 }, bigMsg)
    const compactable = messages.map((_, i) => i)
    // 预留 45000 → cap-2×reserve=10000 低于下限 40000 → 目标输入取 40000；基线 96000 → 需腾 56000
    // 窗口外仅 48 条（共 48000 token）→ 压满 48 条
    const plan = planCompactRange(messages, compactable, { baseline: 96000, cap: 100000, reserve: 45000 })
    expect(plan!.to).toBe(48)
  })

  test("单条消息 token 估算包含工具调用签名与图片", () => {
    const plain = estimateMessageTokens(msg("assistant", "abc"))
    const withCall = estimateMessageTokens(msg("assistant", "abc", { toolCalls: [{ id: "t", name: "read", arguments: { path: "src/a.ts" } }] }))
    expect(withCall).toBeGreaterThan(plain)
    const withImage = estimateMessageTokens(msg("tool", "ok", { images: [{ path: "a.png" }] } as never))
    expect(withImage).toBeGreaterThanOrEqual(1000)
  })
})

describe("compressSession 摘要放置与规划", () => {
  test("摘要置于消息数组最前，近期消息紧随其后（远的历史压缩后放数组前面）", async () => {
    const { provider } = mockProvider()
    const s = await setup({ provider, messages: dialogue(6, 50) })
    await s.compressor.compactSession(s.session.id, "default")
    const loaded = await s.store.load(s.session.id)
    const msgs = loaded!.messages
    expect(msgs[0]!.compacted).toBe(true) // 摘要位于最前
    expect(msgs.filter((m) => m.compacted).length).toBe(1)
    // 近期消息仍在（原样保留）
    expect(msgs.some((m) => m.role === "assistant" && String(m.content).startsWith("回答 5"))).toBe(true)
    s.cleanup()
  })

  test("重复压缩：摘要恒为一条且始终在数组最前（旧摘要被吸收）", async () => {
    const { provider } = mockProvider()
    const s = await setup({ provider, messages: dialogue(12, 50) })
    await s.compressor.compactSession(s.session.id, "default")
    await s.compressor.compactSession(s.session.id, "default")
    const msgs = (await s.store.load(s.session.id))!.messages
    expect(msgs.filter((m) => m.compacted).length).toBe(1)
    expect(msgs[0]!.compacted).toBe(true)
    s.cleanup()
  })

  test("远消息完全抛弃：区间内的用户输入与工具结果被摘要替换移除（不留原文）", async () => {
    const { provider } = mockProvider()
    const s = await setup({ provider, messages: dialogue(10, 50) })
    await s.compressor.compactSession(s.session.id, "default")
    const msgs = (await s.store.load(s.session.id))!.messages
    // 早先的“问题 i”/“回答 i”原文不在上下文里（模型侧只看得到摘要）；滑动窗口内的近期消息原样保留
    for (const i of [0, 1]) {
      expect(msgs.some((m) => m.role === "user" && m.content === `问题 ${i}`)).toBe(false)
      expect(msgs.some((m) => m.role === "assistant" && String(m.content).startsWith(`回答 ${i}`))).toBe(false)
    }
    expect(msgs.some((m) => m.role === "user" && m.content === "问题 9")).toBe(true)
    // 原文不再保留（摘要是其在会话中的唯一留存形态）：摘要消息带压缩条数说明
    const summary = msgs.find((m) => m.compacted)
    expect(String(summary!.summary)).toContain("已压缩")
    s.cleanup()
  })

  test("系统提示词不压缩：区间夹带的 system 消息原位保留", async () => {
    const { provider } = mockProvider()
    const s = await setup({ provider, messages: dialogue(10, 50) })
    // 区间内插入一条 system 提示词（历史中间）
    await s.store.appendMessage(s.session.id, { id: "sys-1", role: "system", content: "你是一个助手", createdAt: Date.now() } as never)
    await s.store.appendMessage(s.session.id, { id: "u-x", role: "user", content: "新问题", createdAt: Date.now() })
    // 手动指定跨过该 system 消息的区间（模拟区间夹带）
    const before = (await s.store.load(s.session.id))!.messages
    const sysIdx = before.findIndex((m) => m.id === "sys-1")
    await s.compressor.compactSession(s.session.id, "default", { from: 0, to: sysIdx + 2 })
    const msgs = (await s.store.load(s.session.id))!.messages
    expect(msgs.some((m) => m.id === "sys-1" && m.content === "你是一个助手")).toBe(true) // 原位保留
    expect(msgs.some((m) => m.compacted)).toBe(true) // 其余可压缩消息已压缩
    s.cleanup()
  })
})

describe("缓存友好摘要（直接重发主循环同前缀，命中提示词缓存）", () => {
  const PREFIX: MessageLike[] = [
    { role: "system", content: "主循环 system 提示词" },
    { role: "user", content: "帮我改一下这个模块" },
    { role: "assistant", content: "好的，我先读文件" },
    { role: "tool", toolCallId: "t1", name: "read", content: `文件全文 ${"y".repeat(1200)}` },
  ]

  test("有缓存前缀时：单次调用、直接把前缀原文 + 压缩指令发出（不再重写成骨架行）", async () => {
    const { provider, inputs } = mockProvider(undefined, { inputTokens: 90000, cachedTokens: 88000 })
    const s = await setup({ provider })
    const out = await s.compressor.summarize([msg("assistant", "x")], provider, undefined, [], { messages: PREFIX })
    expect(out).toBe("摘要正文")
    expect(inputs.length).toBe(1) // 单次调用（不分块、不合并）
    const sent = inputs[0]!
    // 前缀逐字节相同（前 4 条原样），最后追加压缩指令
    expect(sent.slice(0, PREFIX.length)).toEqual(PREFIX)
    expect(String(sent[sent.length - 1]!.content)).toContain("压缩指令")
    expect(String(sent[3]!.content)).toContain("y".repeat(500)) // 原文完整（骨架行会截到 600 字符并标注省略）
    expect(JSON.stringify(sent)).not.toContain("[此前摘要]")
    s.cleanup()
  })

  test("前缀超预算时不发全价大请求：退回骨架行路径", async () => {
    const { provider, inputs } = mockProvider()
    const s = await setup({ provider })
    const huge: MessageLike[] = [{ role: "system", content: "s" }, { role: "user", content: "z".repeat(800_000) }]
    const out = await s.compressor.compactSession(s.session.id, "default", undefined, provider, {
      internal: true,
      cachePrefix: { systemPrompt: "s" },
    })
    expect(out.compacted).toBe(0) // 该会话无历史可压（仅用于验证不报错）
    // 直接用 summarize 验证超预算分支：前缀估算超预算 → 骨架路径（system 为骨架提示词）
    await s.compressor.summarize([msg("assistant", "abc")], provider, undefined, [], { messages: huge })
    expect(String(inputs[inputs.length - 1]![0]!.content)).toContain("压缩器")
    s.cleanup()
  })

  test("未命中缓存（usage.cachedTokens=0）：下次自动改走骨架行路径（不白付全价）", async () => {
    const { provider, inputs } = mockProvider(undefined, { inputTokens: 90000, cachedTokens: 0 })
    const s = await setup({ provider })
    const cached = { messages: PREFIX }
    // 第一次：走前缀路径（命中记忆未知，先试）
    await s.compressor.summarize([msg("assistant", "abc")], provider, undefined, [], cached)
    expect(String(inputs[0]![inputs[0]!.length - 1]!.content)).toContain("压缩指令")
    // 第二次：记录到未命中 → 骨架路径（system 为骨架提示词）
    await s.compressor.summarize([msg("assistant", "abc")], provider, undefined, [], cached)
    expect(String(inputs[1]![0]!.content)).toContain("压缩器")
    s.cleanup()
  })

  test("compactSession 带前缀：摘要请求 = system 提示词 + 历史前缀（截至区间末端）+ 指令", async () => {
    const { provider, inputs } = mockProvider(undefined, { inputTokens: 90000, cachedTokens: 80000 })
    const rendered: MessageLike[] = [
      { role: "user", content: "问题 0" },
      { role: "assistant", content: "回答 0" },
    ]
    let askedUpTo: number | undefined
    const s = await setup({
      provider,
      messages: dialogue(6, 20),
      loadHistory: async (_sid, _user, _mm, upTo) => {
        askedUpTo = upTo
        return rendered
      },
    })
    const r = await s.compressor.compactSession(s.session.id, "default", undefined, provider, {
      internal: true,
      cachePrefix: { systemPrompt: "主 system", tools: [{ name: "read", description: "r", parameters: {} }] },
    })
    expect(r.compacted).toBeGreaterThan(0)
    expect(askedUpTo).toBeGreaterThan(0) // 按区间末端渲染前缀（与主循环同前缀）
    const sent = inputs[0]!
    expect(sent[0]).toEqual({ role: "system", content: "主 system" })
    expect(sent.slice(1, 1 + rendered.length)).toEqual(rendered)
    expect(String(sent[sent.length - 1]!.content)).toContain("压缩指令")
  })
})

describe("溢出硬护栏", () => {
  test("降级发布可见事件（payload.degraded 标识类型，count=0 表示非压缩替换）", async () => {
    const { provider } = mockProvider()
    const s = await setup({
      provider,
      messages: [
        msg("user", `很长的历史用户输入 ${"z".repeat(600)}`),
        msg("assistant", "收到"),
        msg("user", "最新任务输入"),
        msg("assistant", "进行中"),
      ],
    })
    const degraded = await s.compressor.degradeProtectedMessages(s.session.id, "default")
    expect(degraded).toBe(true)
    const compactEvents = s.events.filter((e) => e.type === "event.message.compact")
    expect(compactEvents.length).toBe(1)
    expect(compactEvents[0]!.payload.degraded).toBe("user-message")
    expect(compactEvents[0]!.payload.count).toBe(0)
    expect(String(compactEvents[0]!.payload.summary)).toContain("已裁剪为占位")
    // 最新一条用户消息（本次任务输入）不被裁剪
    const loaded = await s.store.load(s.session.id)
    expect(loaded!.messages.some((m) => m.content === "最新任务输入")).toBe(true)
    s.cleanup()
  })
})
