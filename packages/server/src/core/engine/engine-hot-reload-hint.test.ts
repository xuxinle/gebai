/**
 * 热加载局限提示的可达性测试。
 *
 * 背景（真实踩坑）：改完子Agent 定义立即在**运行中的进程**里自测，新工具会报
 * 「未知工具: X（可用工具: …旧清单…）」——只列旧工具，不说真实原因是
 * 「该子Agent 的辅助模块在本进程运行期间被改过，而辅助模块无法在进程内重载（仅入口带 `?t` 绕缓存）」，
 * 于是很容易被误读成「文件没写对 / 注册失败」，然后反复改文件排查。
 *
 * 判定早已存在（`SubAgentManager.hotReloadWarnings()`），但此前没有任何消费方——
 * 本测试钉住两条出口：① 未知工具报错；② `agent_load` 成功返回。
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
import { agentLoadTool } from "../tools/agent"
import { SubAgentManager } from "../agents/subagents"
import type { ToolContext } from "@gebai/sdk"
import type { LLMProvider, LLMChunk, ChatOptions } from "../llm/llm"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"
import { AgentEngine } from "./engine"

const RELOAD_NOTE =
  "辅助模块在进程运行期间被修改过，但辅助模块无法在进程内重载（仅入口文件带 ?t 可绕模块缓存）——当前可能是「新入口 + 旧辅助」的混合版本；请重启服务后再使用本子Agent"

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
})
