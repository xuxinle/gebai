/**
 * 生成产物「可分发」守卫（回归防线：新克隆环境启动失败）。
 *
 * 背景（真实事故）：`boot/compose.ts` 静态导入 `core/session/env-embedded.generated`（构建期内置模型
 * 配置默认值源码，默认空对象入库，策略同 `tools-excluded.generated.ts`）。一次仓库瘦身把「构建产物
 * 一律不入库」的兜底 gitignore 规则顺手覆盖到它，文件被 `git rm --cached` 移出跟踪——本地（文件还在）
 * 一切正常，**任何新克隆/新机器 `bun run dev` 直接 Cannot find module 启动即崩**，且只在别人机器上
 * 暴露（最坏的一类问题：提交者环境永远看不见）。
 *
 * 守卫规则：`src/**` 中以**静态 import/export**（含 side-effect import）引入的、名字含 `generated`
 * 的模块，必须是「文件存在 + 已被 git 跟踪」。
 * - 静态导入在模块加载期解析，缺文件 = 进程起不来（无 catch 回退空间），故必须随仓库分发；
 * - 动态 `import()`（本仓一律配 try/catch 降级或构建链接管）不在断言范围——构建脚本会在 `bun build`
 *   前生成它们，运行时缺失已有降级路径；
 * - 非 git 工作树（tarball/沙箱解包）环境自动跳过，不误报。
 */
import { describe, expect, test } from "bun:test"
import { spawnSync } from "node:child_process"
import { existsSync, readFileSync, readdirSync, statSync } from "node:fs"
import { dirname, join, resolve, sep } from "node:path"

/** 仓库根：本文件位于 packages/server/src/core/ → 上溯四级。 */
const repoRoot = resolve(import.meta.dirname, "../../../..")
/** 扫描域：服务端与 agents 包的源码（子代理代码在构建期内联进产物，同样受静态导入约束）。 */
const SCAN_ROOTS = ["packages/server/src", "packages/agents/src"]

/** 递归收集 .ts/.tsx 文件。 */
function collectSourceFiles(dir: string): string[] {
  const out: string[] = []
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, entry.name)
    if (entry.isDirectory()) {
      if (entry.name === "node_modules") continue
      out.push(...collectSourceFiles(p))
    } else if (/\.tsx?$/.test(entry.name)) {
      out.push(p)
    }
  }
  return out
}

/** 提取单文件中静态导入的模块说明符（跳过动态 `import(...)`）：行首 import/export 语句 + side-effect import。 */
function staticImportSpecifiers(file: string): string[] {
  const specs: string[] = []
  for (const raw of readFileSync(file, "utf8").split(/\r?\n/)) {
    if (raw.includes("import(")) continue // 动态导入：不在本守卫范围
    const m = raw.match(/^\s*(?:import|export)\b[^"']*?["']([^"']+)["']/)
    if (m) specs.push(m[1])
  }
  return specs
}

/** 相对说明符解析到实际文件（补 .ts/.tsx 扩展名）。 */
function resolveSpecifier(fromFile: string, spec: string): string | null {
  const base = resolve(dirname(fromFile), spec)
  for (const candidate of [base, `${base}.ts`, `${base}.tsx`]) {
    if (existsSync(candidate) && statSync(candidate).isFile()) return candidate
  }
  return null
}

/** git 命令（-C 仓库根）；返回退出码与 stdout。 */
function git(args: string[]): { code: number; stdout: string } {
  const r = spawnSync("git", ["-C", repoRoot, ...args], { encoding: "utf8" })
  return { code: r.status ?? 1, stdout: (r.stdout ?? "").trim() }
}

const rel = (p: string) => p.slice(repoRoot.length + 1).split(sep).join("/")

describe("生成产物可分发守卫（静态导入的 generated 模块必须已入库）", () => {
  test("静态导入的 generated 模块：文件存在且被 git 跟踪", () => {
    const sourceDirs = SCAN_ROOTS.map((d) => join(repoRoot, d))
    if (!sourceDirs.every((d) => existsSync(d))) return // 非本仓目录结构（打包解包）：跳过
    if (git(["rev-parse", "--is-inside-work-tree"]).code !== 0) {
      console.warn("[generated-artifacts] 非 git 工作树，跳过守卫")
      return
    }

    const problems: string[] = []
    for (const dir of sourceDirs) {
      for (const file of collectSourceFiles(dir)) {
        for (const spec of staticImportSpecifiers(file)) {
          if (!spec.startsWith(".") || !spec.includes("generated")) continue // 只看相对路径的生成产物
          const target = resolveSpecifier(file, spec)
          if (!target) {
            problems.push(`${rel(file)} 静态导入 "${spec}"，但文件不存在（模块加载期即崩）`)
            continue
          }
          // 被 git 跟踪 = 随仓库分发到任何新克隆/新机器（本地存在但未入库正是本守卫要拦的事故态）
          if (git(["ls-files", "--error-unmatch", "--", rel(target)]).code !== 0) {
            const ignored = git(["check-ignore", "-q", "--", rel(target)]).code === 0
            problems.push(
              `${rel(target)} 未纳入版本控制（被 ${rel(file)} 静态导入）${ignored ? "，且被 .gitignore 忽略" : ""}` +
                "——新克隆环境启动必失败；请将其空态入库（参考 tools-excluded.generated.ts 策略）",
            )
          }
        }
      }
    }

    expect(problems).toEqual([])
  })
})
