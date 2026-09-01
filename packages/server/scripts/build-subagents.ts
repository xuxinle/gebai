/**
 * 构建时生成子 Agent bundle 注册表（`src/core/subagents.bundle.generated.ts`）。
 *
 * 背景：`discover()` 在 dev 模式下运行时扫描 `src/sub-agents/` 并动态 import；
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
import { join } from "node:path"
import { writeFileSync, readFileSync } from "node:fs"
import { pathToFileURL } from "node:url"
import { parseSubAgentMd } from "../src/core/sub-agent-md"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/server
const srcDir = join(root, "src", "sub-agents")
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

const entries = await readdir(srcDir, { withFileTypes: true })
/** 子Agent 命名规则（DESIGN：仅限小写字母/数字/下划线）；不合规条目跳过并告警，防生成非法标识符。 */
function validName(name: string): boolean {
  if (/^[a-z0-9_]+$/.test(name)) return true
  console.warn(`[build-subagents] 跳过不合规子Agent 名: ${name}（须匹配 [a-z0-9_]+）`)
  return false
}
/** bundle 条目：静态 import 行（ts 定义）或内联 def 对象行（纯 md 定义）。 */
const defs: Array<{ name: string; line: string; dir: boolean }> = []
const seen = new Set<string>()
for (const e of entries) {
  const base = e.name
  if (e.isDirectory()) {
    if (seen.has(base) || !validName(base)) continue
    seen.add(base)
    const tsEntry = join(srcDir, base, `${base}.ts`)
    if (isDefFile(tsEntry)) {
      defs.push({ name: base, dir: true, line: `import { def as subAgent_${base} } from "../sub-agents/${base}/${base}"` })
    } else {
      // 纯提示词简化定义：{dir}.md 单独存在，内联为 def 对象（description/systemPrompt/dependencies 转义嵌入）
      try {
        const md = readFileSync(join(srcDir, base, `${base}.md`), "utf8")
        const { description, systemPrompt, dependencies } = parseSubAgentMd(base, md)
        defs.push({
          name: base,
          dir: true,
          line: `const subAgent_${base}: SubAgentDef = { name: ${JSON.stringify(base)}, description: ${JSON.stringify(description)}, systemPrompt: ${JSON.stringify(systemPrompt)}${dependencies?.length ? `, dependencies: ${JSON.stringify(dependencies)}` : ""} }`,
        })
      } catch {
        console.warn(`[build-subagents] 跳过 ${base}：{${base}.md} 缺失或不可读`)
      }
    }
  } else if (e.isFile() && e.name.endsWith(".ts") && !e.name.endsWith(".test.ts")) {
    const name = e.name.slice(0, -3)
    if (seen.has(name) || !validName(name)) continue
    seen.add(name)
    if (isDefFile(join(srcDir, e.name))) defs.push({ name, dir: false, line: `import { def as subAgent_${name} } from "../sub-agents/${name}"` })
  }
}
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

/** def 依赖名单读取：TS 定义动态 import 读 def.dependencies（与运行时 discover 同通道，模块按装载
 *  语义设计、import 零副作用）；纯 md 定义用 frontmatter 解析结果。import 失败告警按无依赖处理
 *  （运行时装载侧另有缺失跳过与告警兜底）。 */
async function defDependencies(d: { name: string; dir: boolean }): Promise<string[]> {
  try {
    const tsPath = d.dir ? join(srcDir, d.name, `${d.name}.ts`) : join(srcDir, `${d.name}.ts`)
    if (isDefFile(tsPath)) {
      const mod = await import(pathToFileURL(tsPath).href)
      const deps = (mod.def as { dependencies?: string[] } | undefined)?.dependencies
      return deps ?? []
    }
    const md = readFileSync(join(srcDir, d.name, `${d.name}.md`), "utf8")
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
  'import type { SubAgentDef } from "./types"',
  ...included.map((d) => d.line),
  "",
  // 预加载清单烘焙：展开补 preload=true（运行时 GEBAI_PRELOAD_SUB_AGENTS 覆盖仍优先）
  `export const bundledDefs: SubAgentDef[] = [${included.map((d) => (preload.has(d.name) ? `{ ...subAgent_${d.name}, preload: true }` : `subAgent_${d.name}`)).join(", ")}]`,
  "",
]
writeFileSync(outFile, lines.join("\n"))
console.log(
  `[build-subagents] bundled ${included.length}/${defs.length} sub-agents` +
    (preloadNames.length ? ` (preload: ${preloadNames.join(", ")})` : "") +
    ` -> ${outFile}`,
)

/**
 * 子Agent 运行时资源复制：playwright 子Agent 的 node 桥接驱动（driver.mjs）不能被
 * bun build 内联（需保持独立文件供 node 子进程运行），构建时复制到 dist/ 与产物同目录。
 * 幂等：typecheck 等场景下 dist/ 不存在也会创建（产物目录已 gitignore）。
 */
const driverSrc = join(srcDir, "playwright", "driver.mjs")
const distDir = join(root, "dist")
try {
  const { copyFile, mkdir } = await import("node:fs/promises")
  await mkdir(distDir, { recursive: true })
  await copyFile(driverSrc, join(distDir, "driver.mjs"))
  console.log(`[build-subagents] copied playwright driver -> ${join(distDir, "driver.mjs")}`)
} catch (err) {
  console.warn(`[build-subagents] playwright driver 复制失败（playwright 子Agent 在 dist 模式将不可用）: ${err instanceof Error ? err.message : err}`)
}
