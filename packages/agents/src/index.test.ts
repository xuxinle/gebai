/**
 * 包入口清单一致性守护：allAgents（手工汇总）与目录扫描发现结果（dev 发现逻辑同款规则）
 * 必须一致——新增子代理忘改 index.ts 时在此失败，防清单静默漂移。
 * 扫描规则与 server 侧 subagents.ts discover() 同源（排除清单 import 自本包 shared/scan）。
 */
import { describe, expect, test } from "bun:test"
import { readdirSync, readFileSync, statSync } from "node:fs"
import { join } from "node:path"
import { allAgents } from "./index"
import { NON_AGENT_DIRS, NON_AGENT_FILES } from "./shared/scan"

const srcDir = import.meta.dirname

/** 目录扫描发现子Agent 名（与 SubAgentManager.discover 同规则：单文件 {name}.ts / 目录双入口 / 纯 md）。 */
function scanAgentNames(): Set<string> {
  const names = new Set<string>()
  for (const e of readdirSync(srcDir, { withFileTypes: true })) {
    if (e.isFile() && e.name.endsWith(".ts") && !e.name.endsWith(".test.ts")) {
      if (NON_AGENT_FILES.has(e.name)) continue
      const base = e.name.slice(0, -3)
      if (/^[a-z0-9_]+$/.test(base) && isDefFile(join(srcDir, e.name))) names.add(base)
    } else if (e.isDirectory()) {
      if (!/^[a-z0-9_]+$/.test(e.name) || NON_AGENT_DIRS.has(e.name)) continue
      const tsEntry = join(srcDir, e.name, `${e.name}.ts`)
      const indexEntry = join(srcDir, e.name, "index.ts")
      const mdEntry = join(srcDir, e.name, `${e.name}.md`)
      if (isDefFile(tsEntry) || isDefFile(indexEntry) || statSync(mdEntry, { throwIfNoEntry: false })) names.add(e.name)
    }
  }
  return names
}

/** 定义文件判定：内容须导出 `def`（与 build-subagents.ts isDefFile 同规则）。 */
function isDefFile(p: string): boolean {
  try {
    return /export\s+const\s+def\b/.test(readFileSync(p, "utf8"))
  } catch {
    return false
  }
}

describe("包入口清单一致性（allAgents vs 目录扫描）", () => {
  test("allAgents 与扫描发现的子Agent 名集合完全一致", () => {
    const scanned = scanAgentNames()
    const declared = new Set(allAgents.map((a) => a.name))
    expect([...declared].sort()).toEqual([...scanned].sort())
  })

  test("allAgents 各项 def.name 与清单项一致（合并视图不串名）", () => {
    for (const a of allAgents) expect(a.def.name).toBe(a.name)
  })
})
