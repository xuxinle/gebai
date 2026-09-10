#!/usr/bin/env bun
/**
 * custom/ 二开域类型检查：tsc -p custom/tsconfig.json（paths 已映射 @gebai/sdk / @gebai/sdk/node /
 * @gebai/agents 到上游源码）。根 `bun run typecheck:custom` 调用；custom/ 整体不存在时静默跳过
 * （未二开环境零负担）。
 */
import { existsSync } from "node:fs"
import { join } from "node:path"

const root = join(import.meta.dirname, "..")
if (!existsSync(join(root, "custom", "tsconfig.json"))) {
  console.log("[typecheck:custom] custom/ 不存在，跳过")
  process.exit(0)
}
const proc = Bun.spawn(["bunx", "tsc", "-p", "custom/tsconfig.json", "--noEmit"], { cwd: root, stdout: "inherit", stderr: "inherit" })
const code = await proc.exited
process.exit(code)
