import { describe, expect, test } from "bun:test"
import { mkdirSync, rmSync, statSync, utimesSync, writeFileSync } from "node:fs"
import { join } from "node:path"
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
    // 未装载的保持轻量引导列表（装载/新会话执行机制说明由系统提示词路由段与 agent_load/agent_run 工具描述承载）
    expect(out).toContain("可选子Agent（未装载）")
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

/** self_optimize 连带加载测试：def 只声明独有工具（page_capture），通用工具由 code 提供。 */
function makeSelfOptimizeManager(): { mgr: SubAgentManager; registry: ToolRegistry } {
  const registry = new ToolRegistry()
  const mgr = new SubAgentManager({ registry, preloadOverride: [] })
  mgr.register(loadedDef) // code：tools = { read }
  mgr.register({
    name: "self_optimize",
    description: "优化自身",
    systemPrompt: "你是 self_optimize。",
    tools: {
      page_capture: { name: "page_capture", description: "捕获页面", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "ok" }) },
    },
  })
  return { mgr, registry }
}

describe("self_optimize cascade load", () => {
  test("loading self_optimize auto-loads code (通用能力复用：工具与提示词不重复注册)", async () => {
    const { mgr, registry } = makeSelfOptimizeManager()
    await mgr.load("self_optimize")
    // 连带加载 code：code 工具完整注册
    expect(mgr.isLoaded("code")).toBe(true)
    expect(mgr.isLoaded("self_optimize")).toBe(true)
    expect(registry.resolve("code_read")).toBeDefined()
    // self_optimize 只注册自己的独有工具（def 不声明通用工具——复用 code_* 命名空间）
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

  test("self_optimize registers own toolset as declared when code is absent", async () => {
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
    // code 不存在时无连带：按 def 声明原样注册（不做隐式去重——去重由「def 不声明重叠工具」这一约定承担）
    await mgr.load("self_optimize")
    expect(registry.resolve("self_optimize_read")).toBeDefined()
    expect(registry.resolve("self_optimize_page_capture")).toBeDefined()
  })
})

describe("子Agent 热加载（目录签名失效缓存）", () => {
  const dir = join(import.meta.dirname, "..", "sub-agents")
  test("新增/删除 md 子Agent 目录即时生效（无需重启进程）", async () => {
    const name = "zz_hotreload_tmp"
    const agentDir = join(dir, name)
    rmSync(agentDir, { recursive: true, force: true })
    mkdirSync(agentDir, { recursive: true })
    writeFileSync(join(agentDir, `${name}.md`), "---\ndescription: 热加载临时子Agent\n---\n你是热加载临时助手。")
    try {
      // 新增目录：签名变化 → 重新扫描 → 新子Agent 可见
      const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      await m.discover()
      expect(m.def(name)?.description).toBe("热加载临时子Agent")
      expect(m.list().some((d) => d.name === name)).toBe(true)
      // 删除目录：签名变化 → 新实例 discover 重扫后不再可见；refreshIfChanged 幂等（未变化零操作）
      rmSync(agentDir, { recursive: true, force: true })
      const m2 = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      await m2.discover()
      expect(m2.def(name)).toBeUndefined()
      await m2.refreshIfChanged()
      expect(m2.def(name)).toBeUndefined()
    } finally {
      rmSync(agentDir, { recursive: true, force: true })
    }
  })

  test("unregister 的子Agent 在重扫/缓存水合后保持移除（cron 开关语义不因热加载复活）", async () => {
    const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
    await m.discover()
    if (!m.def("cron")) return test.skip("cron 未打包", () => {})
    m.unregister("cron")
    expect(m.def("cron")).toBeUndefined()
    // 触发重扫（touch code.ts 改变目录签名），removedDefs 过滤使其保持移除
    const probe = join(dir, "code.ts")
    const st = statSync(probe)
    utimesSync(probe, new Date(st.atimeMs + 4000), new Date(st.mtimeMs + 4000))
    await m.refreshIfChanged()
    expect(m.def("cron")).toBeUndefined()
    // 基础定义不受影响
    expect(m.def("code")).toBeDefined()
  })
})

describe("子Agent 启停名单（applyEnableDisable：GEBAI_SUB_AGENTS_ENABLE 白名单 / GEBAI_SUB_AGENTS_DISABLE 黑名单）", () => {
  test("enable 白名单：仅保留名单内（未列出的 unregister——已装载的连带卸载工具，目录同步隐藏）", async () => {
    const registry = new ToolRegistry()
    const mgr = new SubAgentManager({ registry, preloadOverride: [] })
    mgr.register(loadedDef)
    mgr.register(unloadedDef)
    await mgr.load("code")
    expect(registry.resolve("code_read")).toBeDefined()
    mgr.applyEnableDisable(["writer"], [])
    expect(mgr.def("code")).toBeUndefined()
    expect(mgr.def("writer")).toBeDefined()
    expect(mgr.list().map((d) => d.name)).toEqual(["writer"])
    expect(registry.resolve("code_read")).toBeUndefined() // unregister 连带卸载工具注册
  })

  test("disable 黑名单移除名单内；与 enable 同时配置先白后黑（黑名单最终生效）", () => {
    const mgr = makeManager()
    mgr.applyEnableDisable([], ["writer"])
    expect(mgr.def("writer")).toBeUndefined()
    expect(mgr.def("code")).toBeDefined()
    const mgr2 = makeManager()
    mgr2.applyEnableDisable(["code", "writer"], ["code"])
    expect(mgr2.def("code")).toBeUndefined()
    expect(mgr2.def("writer")).toBeDefined()
  })

  test("名单未知名告警忽略不阻断（防拼写错误静默失效，白名单语义按名单精确匹配）", () => {
    const mgr = makeManager()
    expect(() => mgr.applyEnableDisable(["code", "ghost"], ["phantom"])).not.toThrow()
    expect(mgr.def("code")).toBeDefined()
    // 白名单不含 writer：被移除是名单语义而非报错；ghost/phantom 不存在仅告警
    expect(mgr.def("writer")).toBeUndefined()
  })

  test("热加载重扫后启停名单效果保持（removedDefs 防复活，与 cron 开关同机制）", async () => {
    const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
    await m.discover()
    if (!m.def("code")) return test.skip("code 未打包", () => {})
    m.applyEnableDisable([], ["code"])
    expect(m.def("code")).toBeUndefined()
    // 触发重扫（touch cron 目录内文件改变签名）后移除保持
    const probe = join(import.meta.dirname, "..", "sub-agents", "code.ts")
    const st = statSync(probe)
    utimesSync(probe, new Date(st.atimeMs + 5000), new Date(st.mtimeMs + 5000))
    await m.refreshIfChanged()
    expect(m.def("code")).toBeUndefined()
    expect(m.def("cron")).toBeDefined()
  })
})

describe("装载工具会话可见性（visibleTo / 目录会话过滤）", () => {
  test("会话级装载不扩散：其他会话不可见，目录对未装载会话仍列出该子Agent", async () => {
    const mgr = makeManager()
    await mgr.load("code", "s1")
    expect(mgr.visibleTo("code", "s1")).toBe(true)
    expect(mgr.visibleTo("code", "s2")).toBe(false)
    expect(mgr.visibleTo("writer", "s1")).toBe(false) // 从未装载
    // 目录：s1 的提示词不含 code（会话记录承载），s2 的仍列出 code 供装载（跨会话不泄漏）
    expect(mgr.systemPromptInjection(undefined, "s1")).not.toContain("- code:")
    expect(mgr.systemPromptInjection(undefined, "s2")).toContain("- code:")
  })

  test("全局装载（admin/启动预载）对所有会话可见；会话卸载解引用不砍全局", async () => {
    const mgr = makeManager()
    await mgr.load("code", "s1")
    await mgr.load("code") // 全局装载（GLOBAL_OWNER）
    expect(mgr.visibleTo("code", "s2")).toBe(true)
    expect(mgr.systemPromptInjection(undefined, "s2")).not.toContain("- code:")
    // 会话 s1 卸载：全局引用仍在，工具注册保留（其他会话仍可见）
    mgr.unload("code", "s1")
    expect(mgr.visibleTo("code", "s1")).toBe(true)
    expect(mgr.visibleTo("code", "s2")).toBe(true)
    // 全局卸载后全部不可见
    mgr.unload("code")
    expect(mgr.visibleTo("code", "s1")).toBe(false)
    expect(mgr.visibleTo("code", "s2")).toBe(false)
  })

  test("无第二参数时保持进程级旧语义（未装载即列出）", async () => {
    const mgr = makeManager()
    await mgr.load("code", "s1")
    // 不传会话过滤：按进程装载状态（兼容旧调用方/测试桩）
    expect(mgr.systemPromptInjection()).not.toContain("- code:")
  })
})
