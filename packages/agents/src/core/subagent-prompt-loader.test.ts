import { describe, expect, test } from "bun:test"
import { readdirSync, readFileSync, statSync } from "node:fs"
import { join } from "node:path"

/**
 * 全仓防回归：子Agent 的系统提示词（`{name}/{name}.md`）必须以**原始 Markdown** 载入——
 * 不得带 HTML 标签（Bun 默认对 .md 走 html loader 会包 `<p>`，把提示词变成 HTML 串）。
 *
 * 两道保险同时断言：
 * 1. 定义文件用 `with { type: "text" }` 显式导入（跨包/跨构建形态一致，见 styles/index.ts 注释）；
 * 2. 本包 bunfig.toml 声明 `".md" = "text"`（覆盖直跑场景）。
 * 另外实测断言：真正 import 进来的 def.systemPrompt 不含块级 HTML 标签。
 */

const AGENTS_SRC = join(process.cwd(), "src", "agents")
const DEF_FILES = (() => {
  const out: string[] = []
  for (const e of readdirSync(AGENTS_SRC, { withFileTypes: true })) {
    if (!e.isDirectory()) continue
    const ts = join(AGENTS_SRC, e.name, `${e.name}.ts`)
    const idx = join(AGENTS_SRC, e.name, "index.ts")
    for (const p of [ts, idx]) {
      try {
        if (statSync(p).isFile() && /export\s+const\s+def\b/.test(readFileSync(p, "utf8"))) out.push(p)
      } catch {
        /* 不存在则跳过 */
      }
    }
  }
  return out
})()

describe("子Agent 提示词载入形态", () => {
  test("定义文件存在且以 with { type: \"text\" } 导入提示词", () => {
    expect(DEF_FILES.length).toBeGreaterThan(10)
    const offenders: string[] = []
    for (const p of DEF_FILES) {
      const src = readFileSync(p, "utf8")
      // 匹配 `import x from "./y.md"`（同一行内必须有 with { type: "text" }）
      const re = /^import\s+\w+\s+from\s+"\.\/[\w-]+\.md"([^\n]*)$/gm
      let m: RegExpExecArray | null
      while ((m = re.exec(src))) {
        if (!/with\s*\{\s*type:\s*"text"\s*\}/.test(m[1])) offenders.push(`${p}: ${m[0].trim()}`)
      }
    }
    expect(offenders).toEqual([])
  })

  test("bunfig.toml 声明 md 走 text loader（直跑场景的第二道保险）", () => {
    const cfg = readFileSync(join(process.cwd(), "bunfig.toml"), "utf8")
    expect(cfg).toContain("[loader]")
    expect(cfg).toMatch(/"?\.md"?\s*=\s*"text"/)
  })

  test("各子Agent 的 systemPrompt 不含块级 HTML 标签（实测载入结果）", async () => {
    const checked: string[] = []
    const bad: string[] = []
    for (const p of DEF_FILES) {
      const mod = (await import(p)) as { def?: { name?: string; systemPrompt?: string } }
      const name = mod.def?.name
      const prompt = String(mod.def?.systemPrompt ?? "")
      if (!name || !prompt) continue
      checked.push(name)
      if (/<\/?(p|ul|ol|li|h[1-6]|blockquote|pre|code)\b[^>]*>/.test(prompt)) bad.push(name)
      // 提示词不应以 HTML 实体形态出现（Markdown 原文里这些字符是字面量）
      if (/&lt;(p|li|ul|h[1-6])&gt;/.test(prompt)) bad.push(`${name}(实体)`)
    }
    expect(checked.length).toBeGreaterThan(10)
    expect(bad).toEqual([])
  })
})
