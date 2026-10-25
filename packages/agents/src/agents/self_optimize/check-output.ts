/** run_tests 各检查项输出的体积收敛：准入判定看 exit code，明细只在失败时需要。
 *
 *  通过的检查项（test/typecheck/lint）原始输出里绝大部分是逐条 `(pass)` 用例明细、turbo 任务流水
 *  与构建脚本日志——它们对「是否可提交」无信息量，却会整段进上下文（一次三件套可达上万字符）。 */
import { cleanScriptOutput } from "./clean-output"

/** 用例级行（`(pass) …` / `(skipped) …` / `(todo) …`）。 */
const TEST_CASE_LINE = /^\((pass|skipped|skip|todo)\) /

/** 构建/编排流水行：任务调度、缓存命中、脚本自述与产物摘要。 */
const BUILD_NOISE = [
  /^@?[\w@/.-]+:(build|typecheck|lint|test): /, // turbo 前缀任务输出（含 `$ 命令` 与脚本自述）
  /^\s*• (Packages in scope|Running |Remote caching)/,
  /^cache (hit|miss)/,
  /^\[build-[\w-]+\] /,
  /^\[[\w-]+\] (bundled|copied|embedded) /,
  /^\s*\S+\.(js|css|wasm)\s+[\d.]+\s*(KB|MB|B)\s+\(entry point\)/,
  /^⊘ /,
]

/** 通过的检查输出摘要：用例计数 + 失败/跳过明细 + 其余非流水行。 */
export function summarizePassedCheck(text: string): string {
  const keep: string[] = []
  let passCount = 0
  for (const raw of cleanScriptOutput(text).split("\n")) {
    const t = raw.trim()
    if (!t) continue
    const m = TEST_CASE_LINE.exec(t)
    if (m) {
      if (m[1] === "pass") passCount++
      else keep.push(t) // 跳过/待办逐条保留（量小且需人看）
      continue
    }
    if (BUILD_NOISE.some((re) => re.test(t))) continue
    keep.push(t)
  }
  const body = (passCount ? [`（通过 ${passCount} 个用例，明细略）`, ...keep] : keep).join("\n")
  return body || "（无输出）"
}

/** 失败项保留的尾部行数（错误与堆栈在最后）。 */
export const CHECK_FAIL_TAIL_LINES = 120

/** 失败的检查输出：保留尾部，前置注明省略了多少行。 */
export function tailForFailure(text: string): string {
  const lines = cleanScriptOutput(text).split("\n").filter((l) => l.trim())
  if (lines.length <= CHECK_FAIL_TAIL_LINES) return lines.join("\n")
  const kept = lines.slice(-CHECK_FAIL_TAIL_LINES)
  return `…（前 ${lines.length - kept.length} 行省略，失败尾部如下）\n${kept.join("\n")}`
}
