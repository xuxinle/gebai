import { readdir, access, stat } from "node:fs/promises"
import { join } from "node:path"
import type { SubAgentDef } from "./types"
import type { ToolRegistry } from "./registry"
import type { SubAgentInfo } from "@gebai/sdk"
import { parseSubAgentMd } from "./sub-agent-md"

export interface SubAgentManagerOptions {
  registry: ToolRegistry
  preloadOverride?: string[]
  bundledNames?: string[]
}

/** 源码目录扫描结果缓存（进程级）：与目录签名（`discoveredSigCache`）配套——签名未变直接复用首次扫描的
 *  定义（def 为纯数据 + 工具函数引用，跨实例共享安全；测试中每个用例新建 SubAgentManager 再 discover
 *  时跳过重复的目录扫描/动态 import，只做本实例的注册与预载）；签名变化（新增/修改/删除子Agent 文件）
 *  即失效重新扫描（DESIGN「子Agent 热加载」）——self_optimize 生成新子Agent 后当会话可用，无需重启。 */
let discoveredDefsCache: SubAgentDef[] | null = null
let discoveredSigCache: string | null = null

/** 计算子Agent 目录签名：递归收集 .ts/.md 文件（排除 .test.ts）的 路径:mtimeMs 排序拼接——
 *  任何新增/修改/删除都改变签名；目录不存在（dist/二进制 bundled 形态）返回 null（bundle 注册表不可变）。
 *  成本约一次目录遍历（~30 次 stat，可忽略），供每次 load/run 前的热加载检查。 */
async function subagentsDirSignature(dir: string): Promise<string | null> {
  const parts: string[] = []
  const walk = async (d: string, prefix: string, depth: number): Promise<void> => {
    if (depth > 2) return
    let entries
    try {
      entries = await readdir(d, { withFileTypes: true })
    } catch {
      return
    }
    for (const e of entries) {
      if (e.isDirectory()) await walk(join(d, e.name), `${prefix}${e.name}/`, depth + 1)
      else if (e.isFile() && (e.name.endsWith(".ts") || e.name.endsWith(".md")) && !e.name.endsWith(".test.ts")) {
        const st = await stat(join(d, e.name)).catch(() => null)
        if (st) parts.push(`${prefix}${e.name}:${st.mtimeMs}`)
      }
    }
  }
  await walk(dir, "", 0)
  return parts.length ? parts.sort().join("|") : null
}

export class SubAgentManager {
  private defs = new Map<string, SubAgentDef>()
  private loaded = new Set<string>()
  private registry: ToolRegistry
  private preloadOverride?: string[]
  private bundledNames: Set<string>
  /** 运行期显式移除的子Agent 名（如 GEBAI_CRON_ENABLED=false 时 unregister cron）：
   *  热加载重扫/缓存水合后仍保持移除（重扫会重新发现其文件，不过滤会「复活」）。 */
  private removedDefs = new Set<string>()

  constructor(opts: SubAgentManagerOptions) {
    this.registry = opts.registry
    this.preloadOverride = opts.preloadOverride
    this.bundledNames = new Set(opts.bundledNames || [])
  }

  async discover(): Promise<void> {
    const dir = join(import.meta.dirname, "..", "sub-agents")
    const sig = await subagentsDirSignature(dir)
    // 命中缓存（签名未变，或 bundled 形态注册表不可变）：直接复用扫描结果
    if (discoveredDefsCache && sig === discoveredSigCache) {
      this.defs.clear()
      for (const def of discoveredDefsCache) this.defs.set(def.name, def)
      for (const n of this.removedDefs) this.defs.delete(n)
      await this.preload()
      return
    }
    if (sig === null) {
      // dist/二进制模式：源码目录不存在，回退到构建时生成的 bundle 注册表（不可变，写入缓存）
      this.defs.clear()
      try {
        const { bundledDefs } = await import("./subagents.bundle.generated")
        for (const def of bundledDefs) this.defs.set(def.name, def)
      } catch (err) {
        // 必抛错，绝不静默降级为空列表——启动「成功」但没有任何子Agent 比启动失败更难排查；
        // 加载失败常见根因是子Agent 模块的模块作用域副作用（如第三方包解析，见 DESIGN「打包闭环」铁律）
        throw new Error(
          `[subagents] bundle 注册表缺失或加载失败（构建时先运行 scripts/build-subagents.ts）: ${err instanceof Error ? err.message : err}`,
        )
      }
      for (const n of this.removedDefs) this.defs.delete(n)
      discoveredDefsCache = [...this.defs.values()]
      discoveredSigCache = null
      await this.preload()
      return
    }
    // 全量扫描（首次或目录签名变化——热加载）：重扫前清空（删除的文件不再保留旧定义）
    this.defs.clear()
    const entries = await readdir(dir, { withFileTypes: true })
    for (const e of entries) {
      if (e.isFile() && e.name.endsWith(".ts") && !e.name.endsWith(".test.ts")) {
        const base = e.name.slice(0, -3)
        if (!/^[a-z0-9_]+$/.test(base)) continue // 命名规则校验（DESIGN：子Agent 名 [a-z0-9_]+）
        try {
          // mtime 查询参数绕过模块缓存：修改过的 TS 文件重新 import 拿到新代码（Bun 按完整 specifier 缓存模块）
          const mtime = (await stat(join(dir, e.name)).catch(() => null))?.mtimeMs ?? 0
          const mod = await import(`../sub-agents/${base}?t=${mtime}`)
          const def = mod.def as SubAgentDef | undefined
          if (def) this.defs.set(def.name, def)
          else console.warn(`[subagents] ${base}.ts 未导出 def，已跳过`)
        } catch (err) {
          console.warn(`[subagents] 加载 ${base}.ts 失败: ${(err as Error).message}`)
        }
      } else if (e.isDirectory()) {
        // 目录形式：{dir}/{dir}.ts 为定义入口；系统提示词可拆 {dir}.md 由入口文件导入并修饰。
        // 无同名 ts（或不导出 def）时支持纯提示词简化定义：{dir}/{dir}.md 单独存在即构成子Agent（零 TS）。
        const base = e.name
        if (!/^[a-z0-9_]+$/.test(base)) continue // 命名规则校验
        const tsEntry = join(dir, base, `${base}.ts`)
        if (await access(tsEntry).then(() => true, () => false)) {
          try {
            const mtime = (await stat(tsEntry).catch(() => null))?.mtimeMs ?? 0
            const mod = await import(`../sub-agents/${base}/${base}?t=${mtime}`)
            const def = mod.def as SubAgentDef | undefined
            if (def) this.defs.set(def.name, def)
            else await this.loadMdOnly(base, dir) // ts 存在但不导出 def（纯辅助目录）→ 回退 md，与 bundle 行为一致
          } catch (err) {
            console.warn(`[subagents] 加载 ${base}/${base}.ts 失败: ${(err as Error).message}`)
          }
        } else {
          await this.loadMdOnly(base, dir)
        }
      }
    }
    for (const n of this.removedDefs) this.defs.delete(n)
    discoveredDefsCache = [...this.defs.values()]
    discoveredSigCache = sig
    await this.preload()
  }

  /** 热加载检查：目录签名变化时重新扫描（幂等、未变化时零成本目录遍历）。装载/新任务前调用——
   *  已装载会话沿用旧定义（工具注册与注入的提示词保持稳定），新定义对未装载与新会话生效。
   *  仅校验既有扫描结果（进程内从未 discover 过时不主动发起首次扫描——生产由启动 discover 负责，
   *  测试桩手工 register 的管理器不因 load 意外扫入真实子Agent）。 */
  async refreshIfChanged(): Promise<void> {
    if (!discoveredDefsCache) return
    const dir = join(import.meta.dirname, "..", "sub-agents")
    const sig = await subagentsDirSignature(dir)
    if (sig === null || sig === discoveredSigCache) return
    await this.discover()
  }

  /** 纯提示词简化定义：{dir}/{dir}.md 单独构成子Agent（零 TS，可选 frontmatter description）。 */
  private async loadMdOnly(base: string, dir: string): Promise<void> {
    try {
      const md = await Bun.file(join(dir, base, `${base}.md`)).text()
      const { description, systemPrompt } = parseSubAgentMd(base, md)
      this.defs.set(base, { name: base, description, systemPrompt })
    } catch (err) {
      console.warn(`[subagents] 加载 ${base}/${base}.md 失败: ${(err as Error).message}`)
    }
  }

  private async preload(): Promise<void> {
    const targets = this.preloadOverride?.length ? this.preloadOverride : []
    for (const def of this.defs.values()) {
      const shouldPreload = targets.length ? targets.includes(def.name) : !!def.preload
      if (shouldPreload) await this.load(def.name)
    }
  }

  /** 装载者引用表（agent → owner 集合）：全局装载记 GLOBAL_OWNER，会话级装载记会话 id。
   *  卸载按 owner 解引用——还有其他装载者时只解除本方引用、工具注册保留（一个会话卸载
   *  不得砍掉其他会话正在使用的工具：注册表是全局的，装载状态却按会话建模）。 */
  private static readonly GLOBAL_OWNER = "*global*"
  private ownersByAgent = new Map<string, Map<string, true>>()

  /** 装载子Agent 能力模块（agent_load 工具 / WS sub_agent.load / 预加载的统一入口，幂等）：
   *  模块语义（DESIGN「装载 vs 新会话执行」）——工具并入当前工具集（{agent}_ 命名空间注册）、完整系统提示词由调用方写入会话记录，
   *  不创建新上下文、无独立执行；与新会话执行（agent_run，派生临时新会话执行）是两种不同概念。
   *  owner：装载者（会话 id / agent_run 共享标记 / 缺省全局），unload 按其解引用。
   *  返回本次实际装载的名字列表（幂等跳过的不计入；self_optimize 连带装载 code 时两者都计入）。 */
  async load(name: string, owner: string = SubAgentManager.GLOBAL_OWNER): Promise<string[]> {
    // 热加载检查（目录签名变化即重扫）：agent_load/路由自愈/agent_run 预加载前拿到最新定义
    // （如 self_optimize 刚生成的子Agent 文件）；签名未变时零成本（一次目录遍历）
    await this.refreshIfChanged()
    const def = this.defs.get(name)
    if (!def) throw new Error(`unknown sub-agent: ${name}`)
    const track = (n: string) => {
      let owners = this.ownersByAgent.get(n)
      if (!owners) {
        owners = new Map()
        this.ownersByAgent.set(n, owners)
      }
      owners.set(owner, true)
    }
    if (this.loaded.has(name)) {
      track(name) // 已注册（他方装载）：幂等跳过注册，但记入本装载者引用
      return []
    }
    // self_optimize 复用 code 的通用能力（其 def 只声明 code 没有的独有工具，提示词亦不复刻 code 工作流）：
    // 装载 self_optimize 时连带装载 code——文件/分析类工具直接用 code_* 命名空间，无需重复注册。
    // load 幂等（loaded 去重）：code 已装载则跳过，不重复注册工具。
    if (name === "self_optimize" && this.defs.has("code")) {
      const loadedNow = await this.load("code", owner)
      this.registry.registerSubAgentTools(name, def.tools ?? {}, def.requiresApproval)
      this.loaded.add(name)
      track(name)
      return [...loadedNow, name]
    }
    this.registry.registerSubAgentTools(name, def.tools ?? {}, def.requiresApproval)
    this.loaded.add(name)
    track(name)
    return [name]
  }

  /** 卸载（解引用）：仅当无其他装载者时才注销工具注册（owner 缺省为全局卸载）。 */
  unload(name: string, owner: string = SubAgentManager.GLOBAL_OWNER): void {
    const owners = this.ownersByAgent.get(name)
    if (owners && owners.size > 0) {
      owners.delete(owner)
      if (owners.size > 0) return // 其他装载者仍在用：保留工具注册
      this.ownersByAgent.delete(name)
    }
    this.registry.unregisterAgent(name)
    this.loaded.delete(name)
  }

  def(name: string): SubAgentDef | undefined {
    return this.defs.get(name)
  }

  /** 全部子Agent 定义（含 envVars 声明；环境变量目录等消费方）。 */
  allDefs(): SubAgentDef[] {
    return [...this.defs.values()]
  }

  /** 动态注册子Agent 定义（测试/运行期扩展用；重名覆盖）。 */
  register(def: SubAgentDef): void {
    this.defs.set(def.name, def)
  }

  /** 撤销子Agent 定义（能力开关关闭时隐藏，如 GEBAI_CRON_ENABLED=false 移除 cron）：未装载直接删除定义；
   *  已装载则先注销其工具（注册表残留工具不清理会让模型可见但引擎不可用）；热加载重扫后仍保持移除。 */
  unregister(name: string): void {
    if (this.loaded.has(name)) this.unload(name)
    this.defs.delete(name)
    this.removedDefs.add(name)
  }

  /** 按启停名单收敛子Agent 集（GEBAI_SUB_AGENTS_ENABLE 白名单 / GEBAI_SUB_AGENTS_DISABLE 黑名单，
   *  启动 discover 后调用一次）：enable 非空 = 白名单（未列出的全部 unregister）；disable = 黑名单；
   *  两者同时配置先白后黑（黑名单最终生效）。unregister 含工具注销、已预载卸载与热加载防复活
   *  （removedDefs），agent_list/系统提示词注入/agent_run 校验随之完全不可见；名单中的未知名告警忽略
   *  （防拼写错误静默失效，不阻断启动——与选择性打包不同，运行态名单以实际发现的子Agent 为准）。 */
  applyEnableDisable(enable: string[] = [], disable: string[] = []): void {
    const preloaded = [...this.loaded] // 过滤前已预载的名单（def.preload 与 GEBAI_PRELOAD_SUB_AGENTS），用于移除告警
    if (enable.length) {
      const whitelist = new Set(enable)
      for (const name of [...this.defs.keys()]) {
        if (!whitelist.has(name)) this.unregister(name)
      }
      for (const name of whitelist) {
        if (!this.defs.has(name)) console.warn(`[subagents] GEBAI_SUB_AGENTS_ENABLE 中的子Agent 不存在: ${name}`)
      }
    }
    for (const name of disable) {
      if (!this.defs.has(name)) {
        console.warn(`[subagents] GEBAI_SUB_AGENTS_DISABLE 中的子Agent 不存在: ${name}`)
        continue
      }
      this.unregister(name)
    }
    for (const name of preloaded) {
      if (!this.defs.has(name)) console.warn(`[subagents] 预载的子Agent ${name} 已被启停名单移除（不再预载）`)
    }
  }

  isLoaded(name: string): boolean {
    return this.loaded.has(name)
  }

  /** 子Agent 是否对某会话可见（装载工具会话可见性，DESIGN「装载工具会话可见性」）：
   *  该会话装载过、或经全局装载（GLOBAL_OWNER：启动预载/admin 全局装载——设计上对所有会话生效）。
   *  其他会话的装载不扩散——共享注册表里的 {agent}_* 工具对未装载会话不可见（防跨会话泄漏）。 */
  visibleTo(name: string, owner: string): boolean {
    if (!this.loaded.has(name)) return false
    const owners = this.ownersByAgent.get(name)
    if (!owners) return false
    return owners.has(owner) || owners.has(SubAgentManager.GLOBAL_OWNER)
  }

  getLoaded(): SubAgentDef[] {
    return [...this.loaded].map((n) => this.defs.get(n)!).filter(Boolean)
  }

  list(): SubAgentInfo[] {
    return [...this.defs.values()].map((d) => ({
      name: d.name,
      description: d.description,
      tools: Object.keys(d.tools ?? {}),
      preload: !!d.preload,
      loaded: this.loaded.has(d.name),
      bundled: this.bundledNames.size === 0 || this.bundledNames.has(d.name),
    }))
  }

  /** 未装载子Agent 轻量引导注入总Agent 系统提示词。
   *  已装载子Agent 的完整系统提示词不在此注入——装载时已作为 system 消息写入会话记录（chat.json 持久化，
   *  loadHistory 透传进模型上下文），此处再注入会双份占用上下文；未装载的仅注入轻量列表（名称 + 描述），
   *  引导模型 agent_load 装载（工具注册进会话、提示词写入会话记录）或 agent_run 执行新会话。
   *  不展开工具列表——工具名已注册进工具集（schema 全名）。
   *  describe：可选描述覆写（engine 用于在描述中动态体现预置项目清单，方便按项目名关联任务）。
   *  visibleToOwner：可选会话过滤（会话 id）——按「对该会话可见」判定未装载（其他会话装载过的不算本会话已装载，
   *  防跨会话泄漏：A 装载后 B 的目录仍应列出该子Agent 供 B 装载）；缺省按进程装载状态过滤（兼容旧语义）。 */
  systemPromptInjection(describe?: (d: SubAgentDef) => string, visibleToOwner?: string): string {
    const lines: string[] = []
    const unloaded = [...this.defs.values()].filter((d) => (visibleToOwner ? !this.visibleTo(d.name, visibleToOwner) : !this.loaded.has(d.name)))
    if (unloaded.length) {
      lines.push("可选子Agent（未装载）:")
      for (const d of unloaded) {
        lines.push(`- ${d.name}: ${describe ? describe(d) : d.description}`)
      }
    }
    return lines.length ? `\n\n${lines.join("\n")}` : ""
  }
}
