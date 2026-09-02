/** 全局工具聚合器与 barrel（原单文件 core/tools.ts 目录化拆分后的唯一聚合点）。
 *
 * 零注册扩展模型（与子 Agent 同款，DESIGN「全局工具」）：
 * - dev 形态：模块初始化时运行时扫描本目录（`core/tools/*.ts`），凡导出 `export const globalTools`
 *   的文件自动收集——**新增全局工具 = 丢一个新文件**（重启进程生效；子 Agent 才有热加载，self_optimize
 *   只写子 Agent 目录）。目录内文件按文件名排序聚合，重名在 createAllGlobalTools 抛错。
 * - dist / bun --compile 形态：源码目录不可扫，回退构建期生成的静态注册表 `tools/bundle.generated.ts`
 *   （`scripts/build-tools.ts` 生成，静态 import 随 bundle 内联）。
 * - 兼容 barrel：原 core/tools.ts 的全部导出（工具定义、截断/遍历/fetch 防护等 util、常量）经
 *   `export *` 原路径再导出，既有导入方（engine/index/ws/子 Agent/测试）零改动。
 *   **分层规则：core/ 内部模块禁止 import 本 barrel**（会与 TLA 扫描构成环）——utils 直引
 *   `core/truncate.ts` 等叶子模块，工具定义直引 `tools/{family}.ts`。
 */
import type { Dirent } from "node:fs"
import { readdir, readFile } from "node:fs/promises"
import { join } from "node:path"
import type { Tool } from "../base/types"
import { projectAware } from "./projects"
import { EXCLUDED_GLOBAL_TOOLS } from "../tools-excluded.generated"
import type { GlobalToolEntry } from "./shared"

export type ToolSet = Record<string, Tool>

// ---- barrel 再导出（保持原 `core/tools` 导入路径兼容）----
export * from "../support/truncate"
export * from "../support/walk"
export * from "../security/fetch-guard"
export * from "../support/artifacts"
export * from "../support/plan"
export * from "../support/exec-opts"
export * from "./shared"
// 家族文件具名再导出（原 core/tools.ts 兼容面）：不使用 export *——各文件均导出注册表 globalTools，
// 星号再导出会重名冲突；新文件新增的工具请从所在文件直接 import（不经 barrel）。
export { readTool, writeTool, lsTool, fileTool, grepTool, globTool, editTool, diffTool, patchTool } from "./fs"
export { shTool, pyTool, resolvePythonCmd, _resetPythonCmdCache } from "./exec"
export { showTool, fetchUrlTool, SHOW_MAX_BYTES, SHOW_TEXT_DIRECT_BYTES, SHOW_TEXT_MAX_CHARS } from "./show"
export {
  gitTool,
  pageCaptureTool,
  readFeedbackTool,
  systemInfoTool,
  envDetectTool,
  makePreviewServerTool,
  PAGE_CAPTURE_HTML_LIMIT,
  type PreviewServerEntry,
} from "./extras"
export { makeTodoTool, askTool, fullModeTool } from "./interact"
export { agentListTool, agentLoadTool, agentRunTool, branchRunTool, branchSyncTool, bgTaskTool } from "./agent"
export { makeFlowTool, toolSchemasTool } from "./flow"

// ---- 目录扫描（dev）→ bundle 回退（dist/--compile），模块初始化一次成形 ----
let allEntries: GlobalToolEntry[] | null = null
const scanErrors: string[] = []
{
  const dir = import.meta.dirname
  let entries: Dirent[] | null = null
  try {
    entries = await readdir(dir, { withFileTypes: true })
  } catch {
    /* dist/binary 形态目录不存在 → bundle 回退 */
  }
  if (entries) {
    const collected: GlobalToolEntry[] = []
    for (const e of [...entries].sort((a, b) => a.name.localeCompare(b.name))) {
      if (!e.isFile() || !e.name.endsWith(".ts") || e.name.endsWith(".test.ts")) continue
      if (e.name === "index.ts" || e.name === "shared.ts" || e.name === "bundle.generated.ts") continue
      // 注册文件判定：内容含 `export const globalTools`（纯辅助文件跳过，同 build-subagents 的 def 判定）
      const text = await readFile(join(dir, e.name), "utf8").catch(() => "")
      if (!/export\s+const\s+globalTools\b/.test(text)) continue
      try {
        const mod = (await import(`./${e.name.slice(0, -3)}`)) as { globalTools?: GlobalToolEntry[] }
        if (Array.isArray(mod.globalTools)) collected.push(...mod.globalTools)
        else scanErrors.push(`${e.name}: 缺 globalTools 导出`)
      } catch (err) {
        scanErrors.push(`${e.name}: ${err instanceof Error ? err.message : err}`)
      }
    }
    if (collected.length) allEntries = collected
    else if (scanErrors.length) {
      // 目录存在但一个注册文件都没成功：多为工具文件语法/依赖错误，宁可启动失败也不静默空表
      throw new Error(`[tools] 全局工具扫描全部失败:\n${scanErrors.join("\n")}`)
    }
  }
  if (!allEntries) {
    const { bundledToolEntries } = await import("./bundle.generated")
    allEntries = bundledToolEntries
  }
  for (const err of scanErrors) console.warn(`[tools] 工具文件加载失败（已跳过）: ${err}`)
}

// ---- 构建期排除名单（GEBAI_BUILD_EXCLUDE_TOOLS → scripts/build-tools.ts 生成并烘焙，静态导入：
//  运行时读文件在 bun --compile 单文件形态不可行；生成文件提交默认空名单，裁剪构建后为脏属预期） ----
const excludedGlobalTools = new Set<string>(EXCLUDED_GLOBAL_TOOLS)

/** 测试注入：覆写构建期排除名单（测试环境生成文件为默认空名单，无法验证过滤路径）。 */
export function _setExcludedGlobalToolsForTest(names: string[]): void {
  excludedGlobalTools.clear()
  for (const n of names) excludedGlobalTools.add(n)
}

/** 工具是否被构建期排除：index.ts 注册（含 vision）、engine agent_run 新会话内建编排工具（flow/tool_schemas/js）
 *  注入共用——排除 = 不注册不暴露（schema 不可见、调用报未知工具）。 */
export function isGlobalToolExcluded(name: string): boolean {
  return excludedGlobalTools.has(name)
}

/** 全量全局工具表（不经构建期排除过滤）：构建脚本校验清单用（`scripts/build-tools.ts` 须对全量名单校验，
 *  否则连续两次不同排除清单的构建会误拒上次被排除的名字）。
 *  文件/路径类工具统一经 projectAware 包装（DESIGN「项目机制」）：默认会话相对路径，project 参数
 *  （预置项目名/项目根路径/保留名 tmp）切换解析基准——code/explore 等编码类子Agent 不再重复定义文件工具，
 *  装载与新会话执行直接复用全局同名工具。projectAware 由聚合器按条目 `project` 声明统一施加。 */
export function createAllGlobalTools(): Record<string, Tool> {
  if (!allEntries) throw new Error("[tools] 全局工具注册表未初始化")
  const out: Record<string, Tool> = {}
  for (const e of allEntries) {
    if (e.name in out) throw new Error(`全局工具重名: ${e.name}（tools/ 目录多文件注册同名工具）`)
    const tool = typeof e.tool === "function" ? e.tool() : e.tool
    out[e.name] = e.project === "workdir" ? projectAware(tool, { workdir: true }) : e.project ? projectAware(tool) : tool
  }
  return out
}

/** 全局工具表（构建期排除过滤后）：被 GEBAI_BUILD_EXCLUDE_TOOLS 排除的工具不注册不暴露。 */
export function createGlobalTools(): Record<string, Tool> {
  const all = createAllGlobalTools()
  return Object.fromEntries(Object.entries(all).filter(([name]) => !excludedGlobalTools.has(name)))
}
