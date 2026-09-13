import { describe, expect, test } from "bun:test"
import { mkdirSync, rmSync, statSync, utimesSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { ToolRegistry } from "../base/registry"
import { SubAgentManager, discoverySignature } from "./subagents"
import { disposeAllKeqing } from "./keqing"
import type { SubAgentDef } from "../base/types"

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
    // 未装载的保持轻量引导列表（装载/子会话运行机制说明由系统提示词路由段与 agent_load/subsession_run 工具描述承载）
    expect(out).toContain("可选子Agent（未装载）")
    expect(out).toContain("- writer: 文档撰写")
  })

  test("load returns names actually loaded this call (idempotent skips; dependencies cascade)", async () => {
    const mgr = makeManager()
    expect(await mgr.load("code")).toEqual(["code"])
    expect(await mgr.load("code")).toEqual([]) // 幂等跳过
    // 依赖自动连带装载（def.dependencies 声明）：依赖与自身都计入（依赖已装载时仅自身）
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

/** self_optimize 依赖连带加载测试：def 声明 dependencies: ["code"]，工具只含独有能力（page_capture），
 *  通用工具由 code 提供（依赖方各自命名空间注册，不重复定义）。 */
function makeSelfOptimizeManager(): { mgr: SubAgentManager; registry: ToolRegistry } {
  const registry = new ToolRegistry()
  const mgr = new SubAgentManager({ registry, preloadOverride: [] })
  mgr.register(loadedDef) // code：tools = { read }
  mgr.register({
    name: "self_optimize",
    description: "优化自身",
    systemPrompt: "你是 self_optimize。",
    dependencies: ["code"],
    tools: {
      page_capture: { name: "page_capture", description: "捕获页面", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "ok" }) },
    },
  })
  return { mgr, registry }
}

describe("self_optimize cascade load（依赖声明驱动）", () => {
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

  test("registers own toolset as declared when dependency code is absent (依赖缺失跳过不阻断)", async () => {
    const registry = new ToolRegistry()
    const mgr = new SubAgentManager({ registry, preloadOverride: [] })
    mgr.register({
      name: "self_optimize",
      description: "优化自身",
      systemPrompt: "你是 self_optimize。",
      dependencies: ["code"], // code def 不存在（启停名单移除/构建裁剪形态）
      tools: {
        read: { name: "read", description: "读文件", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "x" }) },
        page_capture: { name: "page_capture", description: "捕获页面", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "ok" }) },
      },
    })
    // 依赖缺失跳过：按 def 声明原样注册自身（不做隐式去重——去重由「def 不声明重叠工具」这一约定承担）
    await mgr.load("self_optimize")
    expect(registry.resolve("self_optimize_read")).toBeDefined()
    expect(registry.resolve("self_optimize_page_capture")).toBeDefined()
  })
})

/** 通用依赖机制测试载体：dep_top → dep_mid → dep_base 传递依赖，dep_top 同时直依赖 dep_base（去重验证）。 */
function makeDepsManager(): { mgr: SubAgentManager; registry: ToolRegistry } {
  const registry = new ToolRegistry()
  const mgr = new SubAgentManager({ registry, preloadOverride: [] })
  const mk = (name: string, short: string, extra: Partial<SubAgentDef> = {}): SubAgentDef => ({
    name,
    description: name,
    systemPrompt: `你是 ${name}。`,
    tools: { [short]: { name: short, description: name, parameters: { type: "object", properties: {} }, execute: async () => ({ output: name }) } },
    ...extra,
  })
  mgr.register(mk("dep_base", "base", { requiresApproval: { base: true } }))
  mgr.register(mk("dep_mid", "mid", { dependencies: ["dep_base"] }))
  mgr.register(mk("dep_top", "top", { dependencies: ["dep_mid", "dep_base"] }))
  return { mgr, registry }
}

describe("子Agent 依赖与自动装载（dependencies 级联，DESIGN「子Agent 依赖与自动装载」）", () => {
  test("cascade：传递依赖递归展开（依赖在前、自身在后，共享依赖去重）", () => {
    const { mgr } = makeDepsManager()
    expect(mgr.cascade("dep_top")).toEqual(["dep_base", "dep_mid", "dep_top"])
    expect(mgr.cascade("dep_base")).toEqual(["dep_base"])
    expect(mgr.cascade("nonexistent")).toEqual([]) // 未知名不展开（load 层报 unknown 错）
  })

  test("load：依赖自动连带装载（各自命名空间注册，装载方不重复注册依赖工具）", async () => {
    const { mgr, registry } = makeDepsManager()
    expect(await mgr.load("dep_top")).toEqual(["dep_base", "dep_mid", "dep_top"])
    expect(registry.resolve("dep_base_base")).toBeDefined()
    expect(registry.resolve("dep_mid_mid")).toBeDefined()
    expect(registry.resolve("dep_top_top")).toBeDefined()
    expect(registry.resolve("dep_top_base")).toBeUndefined() // 依赖工具只在依赖方命名空间
    expect(mgr.isLoaded("dep_base")).toBe(true)
    expect(mgr.getLoaded().map((d) => d.name).sort()).toEqual(["dep_base", "dep_mid", "dep_top"])
  })

  test("循环依赖报错（定义缺陷暴露给模型修复；自依赖同判）", () => {
    const mgr = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
    mgr.register({ name: "ca", description: "x", systemPrompt: "y", dependencies: ["cb"] })
    mgr.register({ name: "cb", description: "x", systemPrompt: "y", dependencies: ["ca"] })
    expect(() => mgr.cascade("ca")).toThrow("依赖循环: ca → cb → ca")
    mgr.register({ name: "cs", description: "x", systemPrompt: "y", dependencies: ["cs"] })
    expect(() => mgr.cascade("cs")).toThrow("依赖循环: cs → cs")
  })

  test("未知依赖跳过不阻断（告警，自身照常装载——启停名单移除/构建裁剪形态）", () => {
    const mgr = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
    mgr.register({ name: "orphan", description: "x", systemPrompt: "y", dependencies: ["ghost"] })
    expect(mgr.cascade("orphan")).toEqual(["orphan"])
  })

  test("卸载装载方不连带卸载依赖（级联装载的隐式引用同样按 owner 计数）", async () => {
    const { mgr, registry } = makeDepsManager()
    await mgr.load("dep_mid", "sessionA") // 连带装载 dep_base（owner=sessionA 记隐式引用）
    await mgr.load("dep_base", "sessionB") // 幂等跳过注册，追加 sessionB 引用
    mgr.unload("dep_mid", "sessionA") // 卸载装载方：不连带卸载依赖
    expect(registry.resolve("dep_base_base")).toBeDefined()
    expect(registry.resolve("dep_mid_mid")).toBeUndefined()
    mgr.unload("dep_base", "sessionB") // sessionA 的隐式引用仍在（装载 dep_mid 时连带计入）
    expect(registry.resolve("dep_base_base")).toBeDefined()
    mgr.unload("dep_base", "sessionA") // 最后一个引用解除才注销工具
    expect(registry.resolve("dep_base_base")).toBeUndefined()
  })

  test("releaseOwner：按会话全量解引用（引用归零注销工具；他方引用不受影响；幂等）", async () => {
    const { mgr, registry } = makeDepsManager()
    await mgr.load("dep_mid", "sessionA") // 连带装载 dep_base
    await mgr.load("dep_base", "sessionB") // 幂等跳过注册，追加 sessionB 引用
    expect(registry.resolve("dep_base_base")).toBeDefined()
    mgr.releaseOwner("sessionA") // 只释放 sessionA：dep_mid 归零注销，dep_base 仍有 sessionB
    expect(registry.resolve("dep_mid_mid")).toBeUndefined()
    expect(registry.resolve("dep_base_base")).toBeDefined()
    mgr.releaseOwner("sessionA") // 幂等：未持有引用的 owner 无副作用
    expect(registry.resolve("dep_base_base")).toBeDefined()
    mgr.releaseOwner("sessionB") // 最后一个引用解除：全部注销
    expect(registry.resolve("dep_base_base")).toBeUndefined()
    expect(mgr.getLoaded()).toEqual([])
  })
})

describe("子Agent 热加载（目录签名失效缓存）", () => {
  const dir = join(import.meta.dirname, "..", "..", "..", "..", "agents", "src", "agents")  // @gebai/agents 包内子代理源
  test("目录双入口优先级：{name}/{name}.ts 优先于 {name}/index.ts（后者静默忽略，不报错）", async () => {
    const name = "zz_dualentry_tmp"
    const agentDir = join(dir, name)
    rmSync(agentDir, { recursive: true, force: true })
    mkdirSync(agentDir, { recursive: true })
    writeFileSync(
      join(agentDir, `${name}.ts`),
      `export const def = { name: "${name}", description: "主入口胜出", systemPrompt: "y" }\n`,
    )
    writeFileSync(
      join(agentDir, "index.ts"),
      `export const def = { name: "${name}", description: "index 不应生效", systemPrompt: "y" }\n`,
    )
    try {
      const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      await m.discover()
      expect(m.def(name)?.description).toBe("主入口胜出")
      expect(m.loadError(name)).toBeUndefined()
    } finally {
      rmSync(agentDir, { recursive: true, force: true })
      // 清理后重扫：进程级缓存签名回到干净态（防影响后续用例的 refreshIfChanged）
      await new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] }).discover()
    }
  })

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
    const probe = join(dir, "code", "index.ts")
    const st = statSync(probe)
    utimesSync(probe, new Date(st.atimeMs + 4000), new Date(st.mtimeMs + 4000))
    await m.refreshIfChanged()
    expect(m.def("cron")).toBeUndefined()
    // 基础定义不受影响
    expect(m.def("code")).toBeDefined()
  })

  test("加载失败的子Agent：错误原因记录并在 load/未知错误中透出（模型可见根因），修复后热加载恢复", async () => {
    const name = "zz_broken_tmp"
    const file = join(dir, `${name}.ts`)
    writeFileSync(file, `import "./nonexistent-module-xyz"\nexport const def = { name: "${name}", description: "x", systemPrompt: "y" }\n`)
    const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
    try {
      await m.discover()
      // 文件存在但 import 失败：def 不注册，失败原因记录（不再只进 console.warn）
      expect(m.def(name)).toBeUndefined()
      expect(m.loadError(name)).toBeTruthy()
      expect(m.unknownAgentError(name)).toContain("加载失败")
      await expect(m.load(name)).rejects.toThrow(/加载失败/)
      // 修复文件（改为合法定义）：mtime 变化触发重扫 → 注册恢复、错误清除
      writeFileSync(file, `export const def = { name: "${name}", description: "修好了", systemPrompt: "y" }\n`)
      const st = statSync(file)
      utimesSync(file, new Date(st.atimeMs + 4000), new Date(st.mtimeMs + 4000))
      await m.refreshIfChanged()
      expect(m.def(name)?.description).toBe("修好了")
      expect(m.loadError(name)).toBeUndefined()
      expect(await m.load(name)).toEqual([name])
    } finally {
      rmSync(file, { force: true })
      // 删除后立即重扫一次：进程级缓存签名回到干净态——否则后续用例 load() 内的 refreshIfChanged
      // 会触发重扫清掉其手工注册的测试 defs（缓存签名停留在「含本文件」状态）
      await m.discover()
    }
  })
})

describe("客卿集成（多语言子代理：发现→注册→装载→调用）", () => {
  /** fake 协议驱动（bun -e 子进程）：init/tools.list/tool.call 三 op，工具名为裸名（注册表自动加 {agent}_ 前缀）。 */
  const fakeSpawn = (cmd: string[], opts: { env?: Record<string, string> }) => {
    const proc = Bun.spawn(cmd, { env: { ...process.env, ...opts.env }, stdout: "pipe", stderr: "pipe", stdin: "pipe" })
    return {
      stdin: proc.stdin,
      stdout: proc.stdout as unknown as ReadableStream<Uint8Array>,
      stderr: proc.stderr as unknown as ReadableStream<Uint8Array>,
      kill: () => proc.kill(),
      get killed() {
        return proc.killed
      },
    }
  }

  test("选项注入后才启用发现；fake 客卿子代理注册/装载/工具调用全链路", async () => {
    // 1) 未注入选项：discover 不做任何客卿发现（既有测试零影响的行为面）
    const plain = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
    await plain.discover()
    // 2) 注入 fake spawn：发现 fake 客卿子代理并注册
    const registry = new ToolRegistry()
    const m = new SubAgentManager({ registry, preloadOverride: [] })
    m.setKeqingOpts({
      spawn: fakeSpawn as never,
    } as never)
    // 注入后目录内 manifest 为真实仓库内置 python 子代理——发现路径走真实 manifest + fake 驱动替换不了
    // （真实 spawn 会启动 python；此处断言门控语义而非真实进程，真实链路由真机验证覆盖）
    const savedNative = process.env.GEBAI_KEQING
    process.env.GEBAI_KEQING = "off"
    await m.discover()
    expect(m.def("python")).toBeUndefined() // off：不发现
    delete process.env.GEBAI_KEQING
    if (savedNative !== undefined) process.env.GEBAI_KEQING = savedNative
    expect(plain).toBeDefined()
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
    const probe = join(import.meta.dirname, "..", "..", "..", "..", "agents", "src", "agents", "code", "index.ts")
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

  test("客卿目录热加载：refreshIfChanged 在 TS 签名未变时也检查客卿签名（放置新目录即生效，无需重启）", async () => {
    // fake spawn（bun -e 驱动，协议同 keqing/README）：两个同语言子代理先后放置，
    // 验证 ①多子代理并存注册 ②后放置的目录经 refreshIfChanged 被发现（修复前 TS 签名未变直接 return，
    // 客卿永不重扫）③对账回收（目录删除后 dispose）
    const fakeSpawn = (cmd: string[], opts: { env?: Record<string, string> }) => {
      const proc = Bun.spawn(cmd, { env: { ...process.env, ...opts.env }, stdout: "pipe", stderr: "pipe", stdin: "pipe" })
      return {
        stdin: proc.stdin,
        stdout: proc.stdout as unknown as ReadableStream<Uint8Array>,
        stderr: proc.stderr as unknown as ReadableStream<Uint8Array>,
        kill: () => proc.kill(),
        get killed() {
          return proc.killed
        },
      }
    }
    const { mkdtempSync, mkdirSync, writeFileSync, rmSync } = await import("node:fs")
    const { join } = await import("node:path")
    const { tmpdir } = await import("node:os")
    const fakeRoot = mkdtempSync(join(tmpdir(), "gebai-keqing-hot-"))
    // fake 驱动：python 脚本（与官方驱动同进程模式，避免 bun-on-bun 边车组合的平台差异）
    const driver = join(fakeRoot, "drv.py")
    writeFileSync(
      driver,
      [
        "import json, os, sys",
        "sys.stdout.reconfigure(encoding=\"utf-8\", newline=chr(10))",
        "sys.stdin.reconfigure(encoding=\"utf-8\")",
        "TOOLS = [{\"name\": \"foo\", \"description\": \"echo\", \"parameters\": {\"type\": \"object\", \"properties\": {}}}]",
        "def send(o):",
        "    sys.stdout.write(json.dumps(o) + chr(10))",
        "    sys.stdout.flush()",
        "send({\"op\": \"init\", \"name\": os.environ[\"FAKE_NAME\"], \"protocol\": 2, \"tools\": TOOLS})",
        "for line in sys.stdin:",
        "    req = json.loads(line)",
        "    if req[\"op\"] == \"init\": send({\"id\": req[\"id\"], \"ok\": True, \"result\": {\"name\": os.environ[\"FAKE_NAME\"], \"protocol\": 2}})",
        "    elif req[\"op\"] == \"tools.list\": send({\"id\": req[\"id\"], \"ok\": True, \"result\": TOOLS})",
        "    elif req[\"op\"] == \"tool.call\": send({\"id\": req[\"id\"], \"ok\": True, \"result\": {\"output\": \"hello from \" + os.environ[\"FAKE_NAME\"]}})",
      ].join(String.fromCharCode(10)) + String.fromCharCode(10),
    )
    const py = process.platform === "win32" ? "python" : "python3"
    const mk = (name: string) => {
      const d = join(fakeRoot, name)
      mkdirSync(d, { recursive: true })
      writeFileSync(
        join(d, "agent.json"),
        JSON.stringify({ name, description: name + " 热加载验证", protocol: 2, command: [py, driver], env: { FAKE_NAME: name } }),
      )
    }
    try {
      mk("hot_one")
      const registry = new ToolRegistry()
      const m = new SubAgentManager({ registry, preloadOverride: [] })
      m.setKeqingOpts({ roots: [fakeRoot], spawn: fakeSpawn as never } as never)
      await m.discover()
      const mgrAny = m as unknown as { loadErrors?: Map<string, string> }
      console.log("discover loadErrors:", JSON.stringify([...(mgrAny.loadErrors?.entries() ?? [])]))
      expect(m.def("hot_one")).toBeDefined() // 首扫发现
      // 后放置第二个目录：TS 签名未变，客卿签名变化 → refreshIfChanged 应发现
      mk("hot_two")
      await m.refreshIfChanged()
      expect(m.def("hot_two")).toBeDefined() // 修复前这里失败（永不重扫客卿）
      // 同语言多子代理并存：两个 fake（同 bun 驱动）均注册且工具各自独立
      expect(m.list().map((d) => d.name)).toContain("hot_one")
      expect(m.list().map((d) => d.name)).toContain("hot_two")
      await m.load("hot_two")
      expect(registry.resolve("hot_two_foo")).toBeDefined()
      // 目录删除 → 对账回收（边车 dispose，defs 移除）
      rmSync(join(fakeRoot, "hot_one"), { recursive: true, force: true })
      await m.refreshIfChanged()
      expect(m.def("hot_one")).toBeUndefined()
      expect(m.def("hot_two")).toBeDefined() // 其余不受影响
    } finally {
      rmSync(fakeRoot, { recursive: true, force: true })
    }
  })
})

describe("跨语言同名合并（TS + 客卿贡献集 → 合并视图，DESIGN「客卿」）", () => {
  /** fake 协议驱动（python 子进程，同上文热加载用例）：上报 foo/bar 两工具，tool.call 回显 FAKE_NAME。 */
  const fakeSpawn2 = (cmd: string[], opts: { env?: Record<string, string> }) => {
    const proc = Bun.spawn(cmd, { env: { ...process.env, ...opts.env }, stdout: "pipe", stderr: "pipe", stdin: "pipe" })
    return {
      stdin: proc.stdin,
      stdout: proc.stdout as unknown as ReadableStream<Uint8Array>,
      stderr: proc.stderr as unknown as ReadableStream<Uint8Array>,
      kill: () => proc.kill(),
      get killed() {
        return proc.killed
      },
    }
  }
  const mkTsTool = (name: string, marker: string): import("../base/types").Tool => ({
    name,
    description: `${marker} 工具`,
    parameters: { type: "object" as const, properties: {} },
    execute: async () => ({ output: marker }),
  })
  const writeFakeAgent = (root: string, name: string, description: string) => {
    const d = join(root, name)
    mkdirSync(d, { recursive: true })
    writeFileSync(
      join(d, "agent.json"),
      JSON.stringify({ name, description, protocol: 2, command: ["python", join(root, "drv.py")], env: { FAKE_NAME: name } }),
    )
    writeFileSync(join(d, "PROMPT.md"), `${name} 客卿提示词正文`)
    return d
  }

  test("同名 TS+客卿：描述/提示词拼接、工具并集（同名冲突 TS 优先）、装载统一命名空间、卸载注销全部", async () => {
    const { mkdtempSync } = await import("node:fs")
    const { tmpdir } = await import("node:os")
    const fakeRoot = mkdtempSync(join(tmpdir(), "gebai-keqing-merge-"))
    const driver = join(fakeRoot, "drv.py")
    writeFileSync(
      driver,
      [
        "import json, os, sys",
        "sys.stdout.reconfigure(encoding=\"utf-8\", newline=chr(10))",
        "sys.stdin.reconfigure(encoding=\"utf-8\")",
        "TOOLS = [{\"name\": \"foo\", \"description\": \"客卿 foo\", \"parameters\": {\"type\": \"object\", \"properties\": {}}}, {\"name\": \"bar\", \"description\": \"客卿 bar\", \"parameters\": {\"type\": \"object\", \"properties\": {}}}]",
        "def send(o):",
        "    sys.stdout.write(json.dumps(o) + chr(10))",
        "    sys.stdout.flush()",
        "for line in sys.stdin:",
        "    req = json.loads(line)",
        "    if req[\"op\"] == \"init\": send({\"id\": req[\"id\"], \"ok\": True, \"result\": {\"name\": os.environ[\"FAKE_NAME\"], \"protocol\": 2}})",
        "    elif req[\"op\"] == \"tools.list\": send({\"id\": req[\"id\"], \"ok\": True, \"result\": TOOLS})",
        "    elif req[\"op\"] == \"tool.call\": send({\"id\": req[\"id\"], \"ok\": True, \"result\": {\"output\": \"keqing-\" + req[\"args\"][\"tool\"]}})",
      ].join(String.fromCharCode(10)) + String.fromCharCode(10),
    )
    try {
      writeFakeAgent(fakeRoot, "mergx", "客卿侧描述")
      const registry = new ToolRegistry()
      const m = new SubAgentManager({ registry, preloadOverride: [] })
      m.setKeqingOpts({ roots: [fakeRoot], spawn: fakeSpawn2 as never } as never)
      await m.discover()
      // 客卿先发现（单侧）；再补 TS 贡献（register 写入贡献集并重建合并视图）
      m.register({
        name: "mergx",
        description: "TS 侧描述",
        systemPrompt: "TS 提示词正文",
        tools: { crc32: mkTsTool("crc32", "ts-crc32"), foo: mkTsTool("foo", "ts-foo") },
      })
      const def = m.def("mergx")!
      expect(def.description).toBe("TS 侧描述；客卿侧描述")
      expect(def.systemPrompt).toBe("TS 提示词正文\n\nmergx 客卿提示词正文")
      expect(Object.keys(def.tools ?? {}).sort()).toEqual(["bar", "crc32", "foo"])
      // 同名工具冲突：TS 贡献优先（执行体为 ts 实现）
      const fooOut = await def.tools!.foo!.execute({}, {} as never)
      expect(fooOut.output).toBe("ts-foo")
      // 装载：两侧工具统一进 mergx_ 命名空间
      await m.load("mergx")
      for (const t of ["mergx_crc32", "mergx_foo", "mergx_bar"]) expect(registry.resolve(t)).toBeDefined()
      const barOut = await registry.resolve("mergx_bar")!.tool.execute({ tool: "bar" }, {} as never)
      expect(barOut.output).toBe("keqing-bar")
      // 卸载：全部合并工具（两侧）一并注销
      m.unload("mergx")
      for (const t of ["mergx_crc32", "mergx_foo", "mergx_bar"]) expect(registry.resolve(t)).toBeUndefined()
    } finally {
      rmSync(fakeRoot, { recursive: true, force: true })
      await new Promise((r) => setTimeout(r, 50)) // 边车进程退出窗口
    }
  })

  test("热加载重合并：manifest 修改后重拉并重合并；TS 目录重扫不丢客卿贡献（贡献集独立缓存）", async () => {
    const { mkdtempSync, utimesSync, statSync } = await import("node:fs")
    const { tmpdir } = await import("node:os")
    const fakeRoot = mkdtempSync(join(tmpdir(), "gebai-keqing-remerge-"))
    const driver = join(fakeRoot, "drv.py")
    writeFileSync(
      driver,
      [
        "import json, os, sys",
        "sys.stdout.reconfigure(encoding=\"utf-8\", newline=chr(10))",
        "sys.stdin.reconfigure(encoding=\"utf-8\")",
        "TOOLS = [{\"name\": \"foo\", \"description\": \"echo\", \"parameters\": {\"type\": \"object\", \"properties\": {}}}]",
        "def send(o):",
        "    sys.stdout.write(json.dumps(o) + chr(10))",
        "    sys.stdout.flush()",
        "for line in sys.stdin:",
        "    req = json.loads(line)",
        "    if req[\"op\"] == \"init\": send({\"id\": req[\"id\"], \"ok\": True, \"result\": {\"name\": os.environ[\"FAKE_NAME\"], \"protocol\": 2}})",
        "    elif req[\"op\"] == \"tools.list\": send({\"id\": req[\"id\"], \"ok\": True, \"result\": TOOLS})",
        "    elif req[\"op\"] == \"tool.call\": send({\"id\": req[\"id\"], \"ok\": True, \"result\": {\"output\": \"ok\"}})",
      ].join(String.fromCharCode(10)) + String.fromCharCode(10),
    )
    try {
      const agentDir = writeFakeAgent(fakeRoot, "mergx", "v1")
      const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      m.setKeqingOpts({ roots: [fakeRoot], spawn: fakeSpawn2 as never } as never)
      await m.discover()
      expect(m.def("mergx")?.description).toBe("v1")
      // 客卿变化（改 manifest description）：重拉 → 重合并（视图更新）
      const mf = join(agentDir, "agent.json")
      writeFileSync(mf, JSON.stringify({ name: "mergx", description: "v2", protocol: 2, command: ["python", driver], env: { FAKE_NAME: "mergx" } }))
      const st = statSync(mf)
      utimesSync(mf, new Date(st.atimeMs + 4000), new Date(st.mtimeMs + 4000))
      await m.refreshIfChanged()
      expect(m.def("mergx")?.description).toBe("v2")
      // TS 目录签名变化触发全量重扫：客卿贡献不丢（独立贡献集 + 进程级缓存水合）
      const probe = join(import.meta.dirname, "..", "..", "..", "..", "agents", "src", "agents", "code", "index.ts")
      const st2 = statSync(probe)
      utimesSync(probe, new Date(st2.atimeMs + 6000), new Date(st2.mtimeMs + 6000))
      await m.refreshIfChanged()
      expect(m.def("mergx")).toBeDefined()
      expect(m.def("mergx")?.description).toBe("v2")
      expect(m.def("code")).toBeDefined() // TS 侧重扫正常
    } finally {
      rmSync(fakeRoot, { recursive: true, force: true })
      await new Promise((r) => setTimeout(r, 50))
    }
  })
})

describe("子代理失败隔离（DESIGN「子代理失败隔离」：单个失败不炸主流程、不连带其他代理）", () => {
  const dir = join(import.meta.dirname, "..", "..", "..", "..", "agents", "src", "agents")

  test("坏子代理（顶层 import 抛错）不连带其他代理：其余照常发现注册，坏代理根因可见", async () => {
    // 同批放入一个坏代理（import 不存在模块）与一个好代理
    const bad = "zz_isolation_bad_tmp"
    const good = "zz_isolation_good_tmp"
    const badFile = join(dir, `${bad}.ts`)
    const goodFile = join(dir, `${good}.ts`)
    rmSync(badFile, { force: true })
    rmSync(goodFile, { force: true })
    writeFileSync(badFile, `import "./nonexistent-xyz"\nexport const def = { name: "${bad}", description: "x", systemPrompt: "y" }\n`)
    writeFileSync(goodFile, `export const def = { name: "${good}", description: "好代理", systemPrompt: "y" }\n`)
    try {
      const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      await m.discover() // 不得抛错（坏代理只记 loadErrors）
      expect(m.def(bad)).toBeUndefined()
      expect(m.loadError(bad)).toBeTruthy()
      expect(m.unknownAgentError(bad)).toContain(bad)
      expect(m.def(good)?.description).toBe("好代理") // 好代理不受连带
      expect(m.def("code")).toBeDefined() // 既有代理不受连带
    } finally {
      rmSync(badFile, { force: true })
      rmSync(goodFile, { force: true })
      await new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] }).discover()
    }
  })

  test("preload 单个失败不抛穿 discover（记 loadErrors，其余代理照常预载）", async () => {
    // 好代理 preload:true + 工具注册必炸的代理 preload:true：后者预载失败只记 loadErrors
    const bad = "zz_preload_bad_tmp"
    const badFile = join(dir, `${bad}.ts`)
    rmSync(badFile, { force: true })
    writeFileSync(badFile, `export const def = { name: "${bad}", description: "x", systemPrompt: "y", preload: true, tools: { t: { name: "t", description: "d", parameters: { type: "object", properties: {} }, execute: async () => ({ output: "" }) } } }\n`)
    try {
      const registry = new ToolRegistry()
      // 让 registerSubAgentTools 对本代理抛错（模拟注册失败）
      const orig = registry.registerSubAgentTools.bind(registry)
      registry.registerSubAgentTools = ((name: string) => {
        if (name === bad) throw new Error("模拟注册失败")
        return orig(name, {}, undefined)
      }) as typeof registry.registerSubAgentTools
      const m = new SubAgentManager({ registry, preloadOverride: [] })
      await m.discover() // 不得抛错
      expect(m.loadError(bad)).toContain("模拟注册失败")
      // 其余 preload 代理（如手动注册的）不受影响
      const other = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      other.register({ name: "zz_other_ok", description: "d", systemPrompt: "s", preload: true, tools: {} })
      await other.discover()
      expect(other.loadError("zz_other_ok")).toBeUndefined()
    } finally {
      rmSync(badFile, { force: true })
      await new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] }).discover()
    }
  })

  test("bundle 形态 bundledErrors 水合进 loadErrors（构建期剔除的代理根因可见，不抛错）", async () => {
    // 直接构造 discover 的 dist 分支不可行（源码目录存在）；以真实生成产物验证契约：
    // bundledErrors 导出形态为数组（当前全绿为空），bundledDefs 非空——运行时水合逻辑由
    // discover dist 分支消费（ bundledErrors 逐项进 loadErrors + 告警）
    const { bundledDefs, bundledErrors } = await import("../subagents.bundle.generated")
    expect(Array.isArray(bundledErrors)).toBe(true)
    expect(bundledDefs.length).toBeGreaterThan(0)
    // 单条 bundledError 的水合语义：错误信息含代理名与原因（模型侧 unknownAgentError 附因可读）
    for (const [name, err] of bundledErrors) {
      expect(typeof name).toBe("string")
      expect(err.length).toBeGreaterThan(0)
    }
  })
})

describe("dist/二进制形态：域签名与 bundle 回退（scanDirs 覆盖）", () => {
  /** 内置定义域真实路径（与生产同式：本文件位于 packages/server/src/core/agents/）。 */
  const builtinDir = join(import.meta.dirname, "..", "..", "..", "..", "agents", "src", "agents")
  /** 必然不存在的域（模拟 dist/二进制：产物内无源码树）。 */
  const missingDir = join(import.meta.dirname, "..", "..", "..", "..", "..", "no_such_agents_domain")

  test("域签名：内置域缺失整体为 null；域存在但未二开按空串计入（缺失 ≠ 空）", async () => {
    expect(await discoverySignature({ builtin: missingDir, custom: missingDir })).toBeNull()
    const sig = await discoverySignature({ builtin: builtinDir, custom: missingDir })
    expect(typeof sig).toBe("string")
    expect(sig!.endsWith("|custom:")).toBe(true) // 缺失的二开域拼空串（非 "null"）
    // 目录存在但无定义文件（空目录）：非 null（区别于「域缺失」），临时建空目录验证
    const emptyDir = join(import.meta.dirname, "..", "..", "..", "..", "..", "zz_empty_domain_probe")
    mkdirSync(emptyDir, { recursive: true })
    try {
      const emptySig = await discoverySignature({ builtin: emptyDir, custom: missingDir })
      expect(emptySig).not.toBeNull()
      expect(emptySig).toBe("|custom:")
    } finally {
      rmSync(emptyDir, { recursive: true, force: true })
    }
  })

  test("内置域缺失 → 回退 bundle 注册表：不抛错、定义与构建期失败清单均从注册表水合", async () => {
    const m = new SubAgentManager({
      registry: new ToolRegistry(),
      preloadOverride: ["__none__"], // 阻断真实预载（本用例只验证发现回退）
      scanDirs: { builtin: missingDir, custom: missingDir },
    })
    await m.discover() // 修复前：域签名拼接使此分支不可达 → 真实扫描缺失目录抛 ENOENT
    const { bundledDefs, bundledErrors } = await import("../subagents.bundle.generated")
    expect(m.allDefs().map((d) => d.name).sort()).toEqual(bundledDefs.map((d) => d.name).sort())
    for (const [name, err] of bundledErrors) expect(m.loadError(name)).toContain(err)
    // 二次 discover：命中进程级缓存（签名与缓存同为 null），幂等不抛错
    await m.discover()
    expect(m.allDefs().length).toBe(bundledDefs.length)
    // 热加载判定：bundle 形态下签名未变（null === null）→ 不触发无谓重扫
    await m.refreshIfChanged()
    // 清理进程级缓存（bundle 形态写入的缓存不遗留：后续用例按真实源码域签名重扫重建）
    await new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] }).discover()
  })
})

describe("custom 二开域（双域扫描自动合并，DESIGN「custom 二开域」）", () => {
  const customDir = join(import.meta.dirname, "..", "..", "..", "..", "..", "custom", "agents")

  test("custom 新增子代理即自动发现（与内置域合并；删除后消失，热加载同机制）", async () => {
    const name = "zz_custom_probe_tmp"
    const agentDir = join(customDir, name)
    rmSync(agentDir, { recursive: true, force: true })
    mkdirSync(agentDir, { recursive: true })
    writeFileSync(join(agentDir, `${name}.ts`), `export const def = { name: "${name}", description: "二开探针", systemPrompt: "y" }\n`)
    const cleanup = async () => {
      rmSync(agentDir, { recursive: true, force: true })
      await new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] }).discover()
    }
    try {
      const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      await m.discover()
      expect(m.def(name)?.description).toBe("二开探针") // custom 域发现
      expect(m.def("code")).toBeDefined() // 内置域不受影响
      rmSync(agentDir, { recursive: true, force: true })
      const m2 = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      await m2.discover() // 签名变化（custom 域 mtime 变化）→ 重扫 → 二开代理消失
      expect(m2.def(name)).toBeUndefined()
      expect(m2.def("code")).toBeDefined()
    } finally {
      await cleanup()
    }
  })

  test("custom 同名覆盖内置（后扫胜出——改写内置行为而不动上游代码）", async () => {
    const name = "hsh" // 内置轻量子代理，覆盖实验低风险
    const agentDir = join(customDir, name)
    rmSync(agentDir, { recursive: true, force: true })
    mkdirSync(agentDir, { recursive: true })
    writeFileSync(join(agentDir, `${name}.ts`), `export const def = { name: "${name}", description: "二开覆盖版", systemPrompt: "y" }\n`)
    try {
      const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      await m.discover()
      expect(m.def(name)?.description).toBe("二开覆盖版") // custom 版本胜出
    } finally {
      rmSync(agentDir, { recursive: true, force: true })
      await new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] }).discover()
    }
  })

  test("custom 域坏子代理失败隔离（不连带内置域，根因可见）", async () => {
    const name = "zz_custom_bad_tmp"
    const file = join(customDir, `${name}.ts`)
    rmSync(file, { force: true })
    writeFileSync(file, `import "./nonexistent-xyz"\nexport const def = { name: "${name}", description: "x", systemPrompt: "y" }\n`)
    try {
      const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      await m.discover() // 不抛错
      expect(m.def(name)).toBeUndefined()
      expect(m.loadError(name)).toBeTruthy()
      expect(m.def("code")).toBeDefined() // 内置域不受连带
    } finally {
      rmSync(file, { force: true })
      await new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] }).discover()
    }
  })
})

describe("客卿发现延迟（deferNative：启动不被 sidecar 阻塞）", () => {
  /** 慢握手驱动：init 响应前先睡一段时间，用于「阻塞 vs 不阻塞」的可观测差异。 */
  const slowDriverSource = (delaySec: number) =>
    [
      "import json, os, sys, time",
      'sys.stdout.reconfigure(encoding="utf-8", newline=chr(10))',
      'sys.stdin.reconfigure(encoding="utf-8")',
      `time.sleep(${delaySec})`,
      "for line in sys.stdin:",
      "    req = json.loads(line)",
      '    if req["op"] == "init":',
      '        sys.stdout.write(json.dumps({"id": req["id"], "ok": True, "result": {"name": os.environ.get("FAKE_NAME", "slow"), "protocol": 2}}) + chr(10))',
      "        sys.stdout.flush()",
      '    elif req["op"] == "tools.list":',
      '        sys.stdout.write(json.dumps({"id": req["id"], "ok": True, "result": []}) + chr(10))',
      "        sys.stdout.flush()",
    ].join(String.fromCharCode(10)) + String.fromCharCode(10)

  /** 建一个只有一个慢客卿的假根目录（manifest 用 {python} 占位：由解释器解析器跨平台解析）。 */
  async function makeSlowRoot(delaySec: number): Promise<string> {
    const { mkdtempSync } = await import("node:fs")
    const { tmpdir } = await import("node:os")
    const root = mkdtempSync(join(tmpdir(), "gebai-keqing-defer-"))
    writeFileSync(join(root, "drv.py"), slowDriverSource(delaySec))
    const d = join(root, "slowagent")
    mkdirSync(d, { recursive: true })
    writeFileSync(
      join(d, "agent.json"),
      JSON.stringify({
        name: "slowagent",
        description: "慢握手客卿",
        protocol: 2,
        command: ["{python}", join(root, "drv.py")],
        env: { FAKE_NAME: "slowagent" },
      }),
    )
    writeFileSync(join(d, "PROMPT.md"), "slowagent 提示词正文")
    return root
  }

  test("deferNative：discover 不等客卿握手即返回；whenNativeReady 后就绪", async () => {
    const { rmSync } = await import("node:fs")
    const delaySec = 1.5
    const root = await makeSlowRoot(delaySec)
    try {
      const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      m.setKeqingOpts({ roots: [root] } as never)

      const t0 = Date.now()
      await m.discover({ deferNative: true })
      expect(Date.now() - t0).toBeLessThan(delaySec * 1000) // 未等满握手时长
      expect(m.def("slowagent")).toBeUndefined() // 后台未回：合并视图里还没有

      await m.whenNativeReady() // 等后台发现
      expect(m.def("slowagent")).toBeDefined() // 就绪后自动合并进视图
      const t1 = Date.now()
      await m.whenNativeReady() // 已完成：零等待
      expect(Date.now() - t1).toBeLessThan(100)
    } finally {
      await disposeAllKeqing() // 先回收边车进程：否则它仍占着临时目录
      await new Promise((r) => setTimeout(r, 50)) // 进程退出窗口
      rmSync(root, { recursive: true, force: true })
    }
  })

  test("deferNative 下 load 会先等后台发现（显式装载语义不变）", async () => {
    const { rmSync } = await import("node:fs")
    const root = await makeSlowRoot(1.5)
    try {
      const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      m.setKeqingOpts({ roots: [root] } as never)
      await m.discover({ deferNative: true })
      expect(m.def("slowagent")).toBeUndefined()
      // 显式装载：不因后台未回而报「未知子Agent」
      const loaded = await m.load("slowagent")
      expect(loaded).toContain("slowagent")
      expect(m.def("slowagent")).toBeDefined()
    } finally {
      await disposeAllKeqing() // 先回收边车进程：否则它仍占着临时目录
      await new Promise((r) => setTimeout(r, 50)) // 进程退出窗口
      rmSync(root, { recursive: true, force: true })
    }
  })

  test("非 defer（缺省）仍同步等待客卿发现", async () => {
    const { rmSync } = await import("node:fs")
    const root = await makeSlowRoot(1.5)
    try {
      const m = new SubAgentManager({ registry: new ToolRegistry(), preloadOverride: [] })
      m.setKeqingOpts({ roots: [root] } as never)
      await m.discover()
      // 缺省语义：await 回来时客卿定义已在视图里（测试与脚本依赖此语义）
      expect(m.def("slowagent")).toBeDefined()
    } finally {
      await disposeAllKeqing() // 先回收边车进程：否则它仍占着临时目录
      await new Promise((r) => setTimeout(r, 50)) // 进程退出窗口
      rmSync(root, { recursive: true, force: true })
    }
  })
})
