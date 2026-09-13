/**
 * 构建时生成子 Agent bundle 注册表（`src/core/subagents.bundle.generated.ts`）。
 *
 * 背景：`discover()` 在 dev 模式下运行时扫描 `@gebai/agents` 的 `src/agents/`（子代理定义域）并动态 import；
 * 但 dist/bun --compile 产物中源码目录与动态 import 路径均不可用。
 * 本脚本在构建前把所有子 Agent 定义（含其导入的 .md 提示词）以静态 import
 * 聚合成注册表，随 bundle 一起内联进产物，实现子 Agent「打包进二进制」。
 *
 * 构建期裁剪/预加载指定（环境变量，二进制形态无法改源码，须在构建时定死）：
 * - `GEBAI_BUILD_SUBAGENTS`：逗号分隔的包含清单（缺省 = 全部打包）——按需产出精简二进制；
 * - `GEBAI_BUILD_PRELOAD`：逗号分隔的预加载清单——烘焙为 def.preload=true（启动即装载，
 *   与运行时 GEBAI_PRELOAD_SUB_AGENTS 覆盖语义一致：运行时配置仍优先）。
 * 两清单中的未知名字直接报错退出（防构建产物静默缺失）。
 *
 * 该文件为生成产物，已 gitignore，勿手改。
 */
import { readdir } from "node:fs/promises"
import { basename, join } from "node:path"
import { readFileSync } from "node:fs"
import { writeFileIfChanged } from "./write-if-changed"
import { AGENTS_SRC, agentsSrcPath } from "./agents-paths"
import { pathToFileURL } from "node:url"
import { parseSubAgentMd } from "@gebai/agents"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/server
const srcDir = join(AGENTS_SRC, "agents") // @gebai/agents 子代理定义域（基建在 src/core/，物理分域即排除）
const customDir = join(root, "..", "..", "custom", "agents") // 二开子代理域（仓库根 custom/：packages/server → 上两级即仓库根；随文件夹整体迁移，缺失零条目）
const outFile = join(root, "src", "core", "subagents.bundle.generated.ts")

/** 逗号分隔环境变量 → 名单（空值 = 未指定）。 */
const nameList = (v: string | undefined): string[] =>
  (v ?? "")
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean)

const includeNames = nameList(process.env.GEBAI_BUILD_SUBAGENTS)
const preloadNames = nameList(process.env.GEBAI_BUILD_PRELOAD)

/** 定义文件判定：内容须导出 `def`（辅助文件如 desktop_tools.ts 不收录）。 */
function isDefFile(p: string): boolean {
  try {
    return /export\s+const\s+def\b/.test(readFileSync(p, "utf8"))
  } catch {
    return false
  }
}

const entries = await readdir(srcDir, { withFileTypes: true }).catch(() => [] as Awaited<ReturnType<typeof readdir>>)
/** 双域扫描（内置 srcDir 先扫 → 二开 customDir 后扫）：同名后写覆盖（二开胜出，与 dev 发现同语义）；custom 域不存在零条目零告警。 */
/** 子Agent 命名规则（DESIGN：仅限小写字母/数字/下划线）；不合规条目跳过并告警，防生成非法标识符。 */
function validName(name: string): boolean {
  if (/^[a-z0-9_]+$/.test(name)) return true
  console.warn(`[build-subagents] 跳过不合规子Agent 名: ${name}（须匹配 [a-z0-9_]+）`)
  return false
}
/** bundle 条目：定义导入路径（ts 静态 import / md 内联 def 两形态）、preload 烘焙信息与构建期
 *  导入验证结果（badAgents：模块顶层抛错/缺 def 导出/def 非法——剔除出静态 import 烘焙进
 *  bundledErrors，运行时水合进 loadErrors；单代理失败不阻断构建、不连带其他代理）。 */
const defs: Array<{ name: string; importPath: string; baseDir: string; inline?: string; dir?: boolean }> = []
const seen = new Set<string>()
/** 单域扫描收集：目录内条目进 defs；同名已存在时移除旧条目后写胜出（custom 覆盖内置）。 */
function collectDomain(entryBase: string, entries: Array<{ name: string; isDirectory(): boolean; isFile(): boolean }>, isCustom: boolean): void {
  const domain = isCustom ? "custom" : "builtin"
  const importBase = isCustom ? "../../../../custom/agents" : "../../../agents/src/agents"
  for (const e of entries) {
    const base = e.name
    if (e.isDirectory()) {
      if (!validName(base)) continue
      if (seen.has(base)) {
        console.log(`[build-subagents:${domain}] ${base} 同名，${domain} 版本覆盖`)
        const i = defs.findIndex((d) => d.name === base)
        if (i >= 0) defs.splice(i, 1)
      }
      seen.add(base)
      const tsEntry = join(entryBase, base, `${base}.ts`)
      const indexEntry = join(entryBase, base, "index.ts")
      if (isDefFile(tsEntry)) {
        defs.push({ name: base, dir: true, baseDir: entryBase, importPath: `${importBase}/${base}/${base}` })
      } else if (isDefFile(indexEntry)) {
        // 平铺文件迁移形态：{name}/index.ts（code/hsh/self_optimize 等无同名入口的目录）
        defs.push({ name: base, dir: true, baseDir: entryBase, importPath: `${importBase}/${base}/index` })
      } else {
      // 纯提示词简化定义：{dir}.md 单独存在，内联为 def 对象（description/systemPrompt/dependencies/preload/env_vars 转义嵌入）
      try {
        const md = readFileSync(join(entryBase, base, `${base}.md`), "utf8")
        const { description, systemPrompt, dependencies, preload, envVars } = parseSubAgentMd(base, md)
        const extra =
          `${dependencies?.length ? `, dependencies: ${JSON.stringify(dependencies)}` : ""}` +
          `${preload != null ? `, preload: ${preload}` : ""}` +
          `${envVars?.length ? `, envVars: ${JSON.stringify(envVars)}` : ""}`
        defs.push({
          name: base,
          dir: true,
          baseDir: entryBase,
          importPath: `inline:${base}`,
          inline: `{ name: ${JSON.stringify(base)}, description: ${JSON.stringify(description)}, systemPrompt: ${JSON.stringify(systemPrompt)}${extra} }`,
        })
      } catch {
        console.warn(`[build-subagents:${domain}] 跳过 ${base}：{${base}.md} 缺失或不可读`)
      }
    }
  } else if (e.isFile() && e.name.endsWith(".ts") && !e.name.endsWith(".test.ts")) {
    const name = e.name.slice(0, -3)
    if (!validName(name)) continue
    if (seen.has(name)) {
      console.log(`[build-subagents:${domain}] ${name} 同名，${domain} 版本覆盖`)
      const i = defs.findIndex((d) => d.name === name)
      if (i >= 0) defs.splice(i, 1)
    }
    seen.add(name)
    if (isDefFile(join(entryBase, e.name))) defs.push({ name, dir: false, baseDir: entryBase, importPath: `${importBase}/${name}` })
  }
  }
}
collectDomain(srcDir, entries, false)
const customEntries = await readdir(customDir, { withFileTypes: true }).catch(() => null)
if (customEntries) collectDomain(customDir, customEntries, true)
else console.log("[build-subagents] custom/ 二开域不存在，仅打包内置子代理")
defs.sort((a, b) => a.name.localeCompare(b.name))

// 清单校验：未知名字直接失败（构建产物静默缺失比构建失败更难排查），并列出可用名单辅助修正
const known = new Set(defs.map((d) => d.name))
const unknown = [...includeNames, ...preloadNames].filter((n) => !known.has(n))
if (unknown.length) {
  console.error(`[build-subagents] 未知子Agent 名: ${unknown.join(", ")}（可用: ${[...known].join(", ")}）`)
  process.exit(1)
}
const preload = new Set(preloadNames)
const included = includeNames.length ? defs.filter((d) => includeNames.includes(d.name)) : defs

/** 构建期逐代理导入验证（DESIGN「子代理失败隔离」）：TS 定义真 import 一遍——模块顶层抛错/
 *  缺 def 导出/def 非法的代理剔除出静态 import（否则运行时顶层静态 import 任一模块失败会炸
 *  整个注册表，全部子代理不可用），原因烘焙进 bundledErrors（运行时水合进 loadErrors，模型
 *  可见根因）。单代理失败不阻断构建（打印告警），不连带其他代理。 */
const badAgents: Array<[string, string]> = []
/** 入口文件解析：目录形态 {name}/{name}.ts 优先，回退 {name}/index.ts；平铺 {name}.ts。 */
function resolveEntry(baseDir: string, d: { name: string; dir?: boolean }): string {
  if (!d.dir) return join(baseDir, `${d.name}.ts`)
  return isDefFile(join(baseDir, d.name, `${d.name}.ts`)) ? join(baseDir, d.name, `${d.name}.ts`) : join(baseDir, d.name, "index.ts")
}
for (const d of included) {
  if (d.inline) continue // md 内联定义无模块导入风险（本脚本解析即验证）
  // 域内入口解析：每个条目自带 baseDir（内置 srcDir / 二开 customDir）
  const entryFile = resolveEntry(d.baseDir, d)
  try {
    const mod: unknown = await import(pathToFileURL(entryFile).href)
    const def = (mod as { def?: unknown }).def
    if (def == null || typeof def !== "object" || !("name" in def) || (def as { name?: unknown }).name !== d.name) {
      const reason = def == null ? "模块未导出 def" : `def.name 不一致（${String((def as { name?: unknown }).name)} ≠ ${d.name}）`
      badAgents.push([d.name, reason])
      console.warn(`[build-subagents] 子Agent ${d.name} 验证失败，已剔除出 bundle: ${reason}`)
    }
  } catch (err) {
    const reason = `模块导入失败: ${err instanceof Error ? err.message : String(err)}`
    badAgents.push([d.name, reason])
    console.warn(`[build-subagents] 子Agent ${d.name} 导入抛错，已剔除出 bundle（其余代理不受影响）: ${reason}`)
  }
}
const bad = new Map(badAgents)
const good = included.filter((d) => !bad.has(d.name))

/** def 依赖名单读取：TS 定义动态 import 读 def.dependencies（与运行时 discover 同通道，模块按装载
 *  语义设计、import 零副作用）；纯 md 定义用 frontmatter 解析结果。import 失败告警按无依赖处理
 *  （运行时装载侧另有缺失跳过与告警兜底）。读取一律走条目自带 baseDir（内置/二开域各自解析）。 */
async function defDependencies(d: { name: string; dir?: boolean; baseDir: string }): Promise<string[]> {
  try {
    const tsPath = d.dir ? join(d.baseDir, d.name, `${d.name}.ts`) : join(d.baseDir, `${d.name}.ts`)
    if (isDefFile(tsPath)) {
      const mod = await import(pathToFileURL(tsPath).href)
      const deps = (mod.def as { dependencies?: string[] } | undefined)?.dependencies
      return deps ?? []
    }
    const md = readFileSync(join(d.baseDir, d.name, `${d.name}.md`), "utf8")
    return parseSubAgentMd(d.name, md).dependencies ?? []
  } catch (err) {
    console.warn(`[build-subagents] 读取 ${d.name} 依赖失败（按无依赖处理）: ${err instanceof Error ? err.message : err}`)
    return []
  }
}

// 依赖闭包展开（仅包含清单形态）：include reverse_site 自动带上其依赖 playwright——运行时依赖
// 自动装载（DESIGN「子Agent 依赖与自动装载」）要求依赖方在产物中存在，裁剪清单漏列依赖会产出
// 能力残缺的二进制；依赖指向不存在的子Agent 名（拼写错误）直接构建失败
if (includeNames.length) {
  const byName = new Map(defs.map((d) => [d.name, d]))
  const includedNames = new Set(included.map((d) => d.name))
  const queue = [...included]
  while (queue.length) {
    const d = queue.shift()!
    for (const dep of await defDependencies(d)) {
      if (!known.has(dep)) {
        console.error(`[build-subagents] ${d.name} 依赖的子Agent ${dep} 不存在（可用: ${[...known].join(", ")}）`)
        process.exit(1)
      }
      if (!includedNames.has(dep)) {
        includedNames.add(dep)
        const depDef = byName.get(dep)!
        included.push(depDef)
        queue.push(depDef)
      }
    }
  }
}

const lines = [
  "// AUTO-GENERATED by scripts/build-subagents.ts — do not edit.",
  'import type { SubAgentDef } from "./base/types"',
  // 仅经验证可导入的代理进静态 import（构建期已真 import 一遍，bad 剔除并烘焙 bundledErrors）
  ...good.filter((d) => !d.inline).map((d) => `import { def as subAgent_${d.name} } from "${d.importPath}"`),
  ...good.filter((d) => d.inline).map((d) => `const subAgent_${d.name}: SubAgentDef = ${d.inline}`),
  "",
  // 预加载清单烘焙：展开补 preload=true（运行时 GEBAI_PRELOAD_SUB_AGENTS 覆盖仍优先）
  `export const bundledDefs: SubAgentDef[] = [${good.map((d) => (preload.has(d.name) ? `{ ...subAgent_${d.name}, preload: true }` : `subAgent_${d.name}`)).join(", ")}]`,
  "",
  // 构建期验证失败清单（name → 原因）：运行时水合进 loadErrors（agent_load/subsession_run 未知名错误附因）
  `export const bundledErrors: Array<[string, string]> = ${JSON.stringify(badAgents)}`,
  "",
]
writeFileIfChanged(outFile, lines.join("\n"))
console.log(
  `[build-subagents] bundled ${good.length}/${defs.length} sub-agents` +
    (badAgents.length ? ` (失败剔除: ${badAgents.map(([n]) => n).join(", ")})` : "") +
    (preloadNames.length ? ` (preload: ${preloadNames.join(", ")})` : "") +
    ` -> ${outFile}`,
)

/**
 * 运行时资源复制：浏览器桥接驱动（core/browser/driver.mjs，playwright/reverse_site 子Agent
 * 与透明浏览器代理共用）与 CV GPU sidecar 驱动（core/cv/cv-driver.mjs，检测重模型的原生推理
 * 子进程）不能被 bun build 内联（需保持独立文件供 node 子进程运行），构建时复制到 dist/
 * 与产物同目录；客卿源（keqing/ 整树：manifest + 任意语言驱动脚本 + 提示词）
 * 同理不能内联，整树复制到 dist/keqing/。幂等：typecheck 等场景下 dist/ 不存在也会创建
 * （产物目录已 gitignore）。
 */
const distDir = join(root, "dist")
try {
  const { copyFile, mkdir, cp } = await import("node:fs/promises")
  await mkdir(distDir, { recursive: true })
  await copyFile(agentsSrcPath("core", "browser", "driver.mjs"), join(distDir, "driver.mjs"))
  await copyFile(agentsSrcPath("core", "cv", "cv-driver.mjs"), join(distDir, "cv-driver.mjs"))
  // 客卿源（仓库根 keqing/，按语言分目录）→ dist/keqing/（独立部署的
  // dist 树发现兕底）；过滤运行时数据（venv/__pycache__）与编译产物（driver*.exe/objs）——
  // 只带源码与 manifest，可执行体由目标机构建引导按需生成；driver 跨平台形态：Windows
  // driver.exe / Linux 与 macOS 无后缀 driver（含中间产物 driver.obj/pdb 等）
  const nativeFilter = (src: string) => {
    const base = basename(src)
    // 运行时/构建资产目录：venv/__pycache__/objs（Python、C++）、target（cargo）与 go 构建缓存
    return !(
      base === "venv" ||
      base === "__pycache__" ||
      base === "objs" ||
      base === "target" ||
      /^driver(\.(exe|pdb|obj|o|d|out|bin|so|dylib))?$/.test(base)
    )
  }
  await cp(join(root, "..", "..", "keqing"), join(distDir, "keqing"), { recursive: true, filter: nativeFilter })
  console.log(`[build-subagents] copied browser driver + cv sidecar driver + keqing -> ${distDir}`)
} catch (err) {
  console.warn(`[build-subagents] 驱动复制失败（dist 模式下对应能力将不可用）: ${err instanceof Error ? err.message : err}`)
}
