import { describe, expect, test } from "bun:test"
import { ToolRegistry } from "./registry"
import { SubAgentManager } from "./subagents"
import type { SubAgentDef } from "./types"

const loadedDef: SubAgentDef = {
  name: "code",
  description: "代码分析/修改/创建项目",
  systemPrompt: "你是 code。",
  tools: { read: { name: "read", description: "读文件", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "x" }) } },
  preload: true,
}

const unloadedDef: SubAgentDef = {
  name: "writer",
  description: "文档撰写",
  systemPrompt: "你是 writer。",
  tools: {},
}

function makeManager(): SubAgentManager {
  const registry = new ToolRegistry()
  const mgr = new SubAgentManager({ registry, preloadOverride: [] })
  mgr.register(loadedDef)
  mgr.register(unloadedDef)
  return mgr
}

describe("SubAgentManager systemPromptInjection", () => {
  test("loaded agents removed from lightweight list (full prompt lives in session records)", async () => {
    const mgr = makeManager()
    await mgr.load("code")
    const out = mgr.systemPromptInjection()
    // 已装载的完整提示词由会话记录承载（loadedAgent system 消息），不再注入总层提示词
    expect(out).not.toContain("已装载子Agent 模块")
    expect(out).not.toContain("你是 code。")
    expect(out).not.toContain("code:")
    // 未装载的保持轻量引导列表
    expect(out).toContain("可选子Agent（未装载：先用 agent_load 装载后直接调用其工具——工具注册进当前会话、完整系统提示词写入会话记录；或不经装载直接 agent_run 执行新会话）")
    expect(out).toContain("- writer: 文档撰写")
  })

  test("load returns names actually loaded this call (idempotent skips; self_optimize cascades code)", async () => {
    const mgr = makeManager()
    expect(await mgr.load("code")).toEqual(["code"])
    expect(await mgr.load("code")).toEqual([]) // 幂等跳过
    // self_optimize 连带装载 code：两者都计入（code 已装载时仅 self_optimize）
    const { mgr: mgr2 } = makeSelfOptimizeManager()
    expect(await mgr2.load("self_optimize")).toEqual(["code", "self_optimize"])
    expect(await mgr2.load("self_optimize")).toEqual([])
  })

  test("empty when nothing registered", () => {
    const mgr = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
    expect(mgr.systemPromptInjection()).toBe("")
  })

  test("describe override renders dynamic description (preset projects surfaced)", () => {
    const mgr = makeManager()
    const out = mgr.systemPromptInjection((d) => (d.name === "code" ? `${d.description} 预置项目：train: 训练系统（/srv/train）` : d.description))
    expect(out).toContain("- code: 代码分析/修改/创建项目 预置项目：train: 训练系统（/srv/train）")
    expect(out).toContain("- writer: 文档撰写")
  })
})

/** self_optimize 连带加载测试：与 code 重叠的工具（read）+ 独有工具（page_capture）。 */
function makeSelfOptimizeManager(): { mgr: SubAgentManager; registry: ToolRegistry } {
  const registry = new ToolRegistry()
  const mgr = new SubAgentManager({ registry, preloadOverride: [] })
  mgr.register(loadedDef) // code：tools = { read }
  mgr.register({
    name: "self_optimize",
    description: "优化自身",
    systemPrompt: "你是 self_optimize。",
    tools: {
      read: { name: "read", description: "读文件", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "x" }) },
      page_capture: { name: "page_capture", description: "捕获页面", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "ok" }) },
    },
  })
  return { mgr, registry }
}

describe("self_optimize cascade load", () => {
  test("loading self_optimize auto-loads code and skips overlapping tool registration", async () => {
    const { mgr, registry } = makeSelfOptimizeManager()
    await mgr.load("self_optimize")
    // 连带加载 code
    expect(mgr.isLoaded("code")).toBe(true)
    expect(mgr.isLoaded("self_optimize")).toBe(true)
    // code 工具完整注册
    expect(registry.resolve("code_read")).toBeDefined()
    // self_optimize 仅注册独有工具，重叠的 read 跳过（避免双命名空间冗余）
    expect(registry.resolve("self_optimize_page_capture")).toBeDefined()
    expect(registry.resolve("self_optimize_read")).toBeUndefined()
  })

  test("repeated load is idempotent (no duplicate tool registration)", async () => {
    const { mgr, registry } = makeSelfOptimizeManager()
    await mgr.load("self_optimize")
    await mgr.load("self_optimize") // 幂等：不抛 duplicate tool name
    await mgr.load("code") // code 已连带加载，同样幂等
    expect(registry.resolve("code_read")).toBeDefined()
    expect(registry.resolve("self_optimize_page_capture")).toBeDefined()
  })

  test("self_optimize registers full toolset when code is absent", async () => {
    const registry = new ToolRegistry()
    const mgr = new SubAgentManager({ registry, preloadOverride: [] })
    mgr.register({
      name: "self_optimize",
      description: "优化自身",
      systemPrompt: "你是 self_optimize。",
      tools: {
        read: { name: "read", description: "读文件", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "x" }) },
        page_capture: { name: "page_capture", description: "捕获页面", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "ok" }) },
      },
    })
    await mgr.load("self_optimize")
    expect(registry.resolve("self_optimize_read")).toBeDefined()
    expect(registry.resolve("self_optimize_page_capture")).toBeDefined()
  })
})
