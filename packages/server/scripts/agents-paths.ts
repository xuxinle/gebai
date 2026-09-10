/**
 * agents 包源码路径单一事实源（构建脚本专用）。
 *
 * 背景：agents 包历经两次搬家（`packages/server/src/agents` → `packages/agents/src` →
 * 物理分域 `src/agents` 子代理定义 + `src/core` 依赖组件基建）。各 `build-*.ts` 里手写的
 * `join(root, "..", "agents", "src", …)` 有 6 处未跟进分域，构建链直接 ENOENT
 * （analyzer/browser/cv 内嵌产物生成 + dist 驱动的复制）——`bun run typecheck`/`build` 全线失败。
 * 路径集中到本文件：下次布局调整只改这里，其余脚本无需再逐处手写。
 */
import { existsSync } from "node:fs"
import { join } from "node:path"

/** `packages/` 目录（本文件位于 packages/server/scripts/）。 */
const packagesDir = join(import.meta.dirname, "..", "..")

/** agents 包 `src/` 目录。 */
export const AGENTS_SRC = join(packagesDir, "agents", "src")
/** agents 包依赖组件基建目录（`src/core/`：analyzer、browser、cv 等）。 */
export const AGENTS_CORE = join(AGENTS_SRC, "core")

/** 拼接 agents 包 `src/` 内路径：`agentsSrcPath("core", "cv", "cv-driver.mjs")`。 */
export function agentsSrcPath(...segments: string[]): string {
  return join(AGENTS_SRC, ...segments)
}

// 布局守卫：agents 包再搬家时在此显式失败（而不是在某个内嵌产物脚本里报一句无上下文的 ENOENT）
if (!existsSync(AGENTS_SRC)) {
  throw new Error(`[agents-paths] agents 包源码目录不存在：${AGENTS_SRC}——agents 包布局变更后请同步本文件`)
}
