/**
 * 热加载局限提示的可达性测试。
 *
 * 背景（真实踩坑）：改完子Agent 定义立即在**运行中的进程**里自测，新工具会报
 * 「未知工具: X（可用工具: …旧清单…）」——只列旧工具，不说真实原因是
 * 「该子Agent 的辅助模块在本进程运行期间被改过，而辅助模块无法在进程内重载（仅入口带 `?t` 绕缓存）」，
 * 于是很容易被误读成「文件没写对 / 注册失败」，然后反复改文件排查。
 *
 * 判定早已存在（`SubAgentManager.hotReloadWarnings()`），但此前没有任何消费方——
 * 本测试钉住**三条**出口：① 未知工具报错；② `agent_load` 成功返回；
 * ③ `subsession_run` 成功返回（子Agent 开发循环里**最常用的验证入口**）。
 *
 * 为什么必须有 ③（实测再次踩到，2026-10-04）：改完子Agent 的 `.md` 系统提示词，
 * 用 `subsession_run` 验证（隔离上下文、看它是否收到新提示词）——子会话**如实回答“提示词里
 * 没有这一节”**，且工具返回**一切正常、无任何提示**。这里的失败是**静默的**：
 * 工具成功、子会话也成功，只是服务的是旧版辅助模块（`.md` 被 import 后进程内无法失效）。
 * 于是改动者只能得出“改动写错了”的结论——真实原因只需重启服务。
 * 对比 `agent_load`：那条路径本来就带提示，而更常用的验证入口反而没有。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { loadConfig } from "../base/config"
import { SessionStore } from "../session/store"
import { ToolRegistry } from "../base/registry"
import { createGlobalTools } from "../tools"
import { Sandbox } from "../security/sandbox"
import { EnvManager } from "../session/env"
import { EventBus } from "../base/event-bus"
import { agentLoadTool, subSessionRunTool } from "../tools/agent"
import type { SubSessionRecord } from "../session/subsessions"
import { SubAgentManager } from "../agents/subagents"
import type { ToolContext } from "@gebai/sdk"
import type { LLMProvider, LLMChunk, ChatOptions } from "../llm/llm"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"
import { AgentEngine } from "./engine"

const RELOAD_NOTE =
  "辅助模块在进程运行期间被修改过，但辅助模块无法在进程内重载（仅入口文件带 ?t 可绕模块缓存）——当前可能是「新入口 + 旧辅助」的混合版本；请重启服务后再使用本子Agent"

/** 子会话记录桩（子会话工具的状态行会读 envKeys/rounds 等字段；只填谓词用到的）。 */
function makeRecord(): SubSessionRecord {
  return {
    runId: "s1",
    sessionId: "sess",
    name: "s1",
    input: "x",
    agents: [],
    envMode: "inherit" as const,
    envKeys: [],
    inheritContext: false,
    async: false,
    depth: 1,
    status: "done" as const,
    rounds: 1,
    toolCalls: 0,
    merged: false,
    startedAt: 0,
  } as unknown as SubSessionRecord
}

/** 真实管理器 + 注入的探针子Agent：热加载提示路径需要一个已注册的 `probe`（前缀匹配与工具清单都用它），
 *  而 `refreshIfChanged` 等其余方法走真实实现（`run` 生命周期要用）。 */
async function stubSubAgents(
  registry: ToolRegistry,
  notes: Array<[string, string]>,
): Promise<SubAgentManager> {
  const manager = new SubAgentManager({ registry, preloadOverride: [] })
  await manager.discover()
  manager.register({
    name: "probe",
    description: "探针子Agent",
    systemPrompt: "probe",
    tools: { do: { name: "probe_do", description: "探针工具", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "ok" }) } },
  })
  if (notes.length) {
    // 只有这条读法被替身（真实实现靠扫描时比对辅助模块签名，单测里不必构造磁盘布局）
    ;(manager as unknown as { hotReloadWarnings: () => Array<[string, string]> }).hotReloadWarnings = () => notes
  }
  return manager
}

class CallerProvider implements LLMProvider {
  readonly id = "fake"
  calls = 0
  constructor(private readonly toolName: string) {}
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 100000 }
  }
  async *chat(_msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.calls++
    if (this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-1", name: this.toolName, arguments: {} } }
      yield { type: "done" }
      return
    }
    yield { type: "text", text: "done" }
    yield { type: "done" }
  }
}

async function setupEngine(provider: LLMProvider, withNote: boolean) {
  const home = mkdtempSync(join(tmpdir(), "gebai-hotreload-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  const config = loadConfig({ gebaiHome: home, auth: "local", sandbox: "off", preloadSubAgents: [], binaryMode: false })
  const store = new SessionStore({ home })
  const registry = new ToolRegistry()
  for (const tool of Object.values(createGlobalTools())) registry.register(tool)
  const sandbox = new Sandbox({ home, enabled: false })
  const env = new EnvManager(store)
  const events = new EventBus()
  const subAgents = await stubSubAgents(registry, withNote ? [["probe", RELOAD_NOTE]] : [])
  const engine = new AgentEngine({ provider, registry, store, env, sandbox, events, config, subAgents, retryBackoffMs: 5 })
  return { home, store, engine }
}

describe("热加载局限提示", () => {
  test("未知工具：命中已改辅助模块的子Agent 时，附「需重启」提示而非只列旧工具", async () => {
    const provider = new CallerProvider("probe_missing")
    const { home, store, engine } = await setupEngine(provider, true)
    const session = await store.createSession("default", "a")
    await engine.run(session.id, "default", "hi")
    const toolMsg = (await store.load(session.id, "default"))!.messages.find((m) => m.role === "tool")
    expect(toolMsg).toBeTruthy()
    expect(toolMsg!.content).toContain("未知工具")
    expect(toolMsg!.content).toContain("probe_do") // 恢复面：可用工具清单仍在
    expect(toolMsg!.content).toContain("重启服务") // 真实原因：辅助模块改动需重启
    rmSync(home, { recursive: true, force: true })
  })

  test("未知工具：无热加载改动时不附提示（避免噪声与误判）", async () => {
    const provider = new CallerProvider("probe_missing")
    const { home, store, engine } = await setupEngine(provider, false)
    const session = await store.createSession("default", "a")
    await engine.run(session.id, "default", "hi")
    const toolMsg = (await store.load(session.id, "default"))!.messages.find((m) => m.role === "tool")
    expect(toolMsg!.content).toContain("未知工具")
    expect(toolMsg!.content).not.toContain("重启服务")
    rmSync(home, { recursive: true, force: true })
  })

  test("agent_load 成功返回：附同一提示（装载时就能知道本次可能服务旧辅助模块）", async () => {
    const ctx = {
      loadSubAgent: async () => {},
      subAgentHotReloadNote: (name: string) => (name === "probe" ? RELOAD_NOTE : null),
    } as unknown as ToolContext
    const loaded = await agentLoadTool.execute({ name: "probe" }, ctx)
    expect(loaded.output).toContain("已装载")
    expect(loaded.output).toContain("重启服务")
    expect((loaded.data as { hotReloadNote?: string }).hotReloadNote).toBe(RELOAD_NOTE)
  })

  test("agent_load：宿主未提供该能力时不报错、不加噪声（可选契约）", async () => {
    const ctx = { loadSubAgent: async () => {} } as unknown as ToolContext
    const loaded = await agentLoadTool.execute({ name: "probe" }, ctx)
    expect(loaded.output).toContain("已装载")
    expect(loaded.output).not.toContain("重启服务")
    expect((loaded.data as { hotReloadNote?: string }).hotReloadNote).toBeUndefined()
  })

  test("subsession_run 成功返回：装载名单里有已改辅助模块的子Agent 时附同一提示", async () => {
    // 隔离形态（inherit_context=false）同步运行：单任务形态，agents 指向已改辅助模块的 probe。
    const started = [makeRecord()]
    const ctx = {
      subAgentHotReloadNote: (name: string) => (name === "probe" ? RELOAD_NOTE : null),
      subSessions: {
        start: async (specs: Array<{ agents: string[] }>) => started.map((r, i) => ({ ...r, agents: specs[i]?.agents ?? [] })),
        wait: async () => started[0],
        result: () => ({ output: "子会话结论" }),
      },
    } as unknown as ToolContext
    const res = await subSessionRunTool.execute({ input: "看你的提示词里有没有那一节", agents: ["probe"] }, ctx)
    expect(res.output).toContain("子会话执行完成")
    // 真实原因必须出现：否则改动者会把「子会话说没收到新提示词」误读成改动写错了
    expect(res.output).toContain("重启服务")
    expect((res.data as { hotReloadNotes?: Record<string, string> }).hotReloadNotes?.probe).toBe(RELOAD_NOTE)
  })

  test("subsession_run：无热加载改动时不附提示（避免噪声与误判）", async () => {
    const started = [makeRecord()]
    const ctx = {
      subAgentHotReloadNote: () => null,
      subSessions: {
        start: async () => started,
        wait: async () => started[0],
        result: () => ({ output: "子会话结论" }),
      },
    } as unknown as ToolContext
    const res = await subSessionRunTool.execute({ input: "x", agents: ["probe"] }, ctx)
    expect(res.output).not.toContain("重启服务")
    expect((res.data as { hotReloadNotes?: Record<string, string> }).hotReloadNotes).toBeUndefined()
  })

  test("subsession_run：宿主未注入该能力时沉默降级（可选契约，不报错）", async () => {
    const started = [makeRecord()]
    const ctx = {
      subSessions: {
        start: async () => started,
        wait: async () => started[0],
        result: () => ({ output: "ok" }),
      },
    } as unknown as ToolContext
    const res = await subSessionRunTool.execute({ input: "x", agents: ["probe"] }, ctx)
    expect(res.output).toContain("子会话执行完成")
    expect(res.output).not.toContain("重启服务")
  })
})
