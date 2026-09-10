#!/usr/bin/env bun
/**
 * custom/ 二开域类型检查：tsc -p custom/tsconfig.json（paths 已映射 @gebai/sdk / @gebai/sdk/node /
 * @gebai/agents 到上游源码）。根 typecheck 链与 `bun run typecheck:custom` 调用。
 * 空态跳过：custom/ 整体不存在，或 agents/ 与 core/ 下无任何 .ts 文件（出厂空骨架——
 * 仅 .gitkeep 占位；tsc 无输入报 TS18003，跳过而非报错，未二开环境零负担）。
 */
import { existsSync, readdirSync } from "node:fs"
import { join } from "node:path"

const root = join(import.meta.dirname, "..")
const customRoot = join(root, "custom")
if (!existsSync(join(customRoot, "tsconfig.json"))) {
  console.log("[typecheck:custom] custom/ 不存在，跳过")
  process.exit(0)
}
/** 目录树内是否存在 .ts 文件（.gitkeep 占位不算——空骨架跳过）。 */
const hasTs = (d: string): boolean => {
  try {
    return readdirSync(d, { withFileTypes: true }).some(
      (e) => (e.isFile() && e.name.endsWith(".ts")) || (e.isDirectory() && hasTs(join(d, e.name))),
    )
  } catch {
    return false
  }
}
if (!hasTs(customRoot)) {
  console.log("[typecheck:custom] custom/ 无 .ts 文件（空骨架），跳过")
  process.exit(0)
}
const proc = Bun.spawn(["bunx", "tsc", "-p", "custom/tsconfig.json", "--noEmit"], { cwd: root, stdout: "inherit", stderr: "inherit" })
const code = await proc.exited
process.exit(code)
