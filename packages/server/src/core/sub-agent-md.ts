/**
 * 纯提示词子 Agent（简化定义）的 md 解析：`sub-agents/{name}/{name}.md` 单独存在（无同名 ts）时，
 * 直接由 md 构成 SubAgentDef——零 TS 代码的简单/组合式子 Agent。
 *
 * 可选 frontmatter（YAML 风格，识别 description 与 dependencies）：
 * ```md
 * ---
 * description: 一句话能力描述（缺省取正文首行）
 * dependencies: playwright, code（依赖的子Agent 名，逗号分隔——装载时自动连带装载）
 * ---
 * 系统提示词正文
 * ```
 */
export interface ParsedSubAgentMd {
  description: string
  systemPrompt: string
  /** 依赖的子Agent 名单（frontmatter `dependencies: a, b`；未声明为 undefined）。 */
  dependencies?: string[]
}

export function parseSubAgentMd(name: string, md: string): ParsedSubAgentMd {
  let description = ""
  let dependencies: string[] | undefined
  let body = md
  const fm = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/.exec(md)
  if (fm) {
    for (const line of fm[1].split(/\r?\n/)) {
      const m = /^description\s*:\s*(.+)$/.exec(line)
      if (m) description = m[1].trim()
      const d = /^dependencies\s*:\s*(.+)$/.exec(line)
      if (d) {
        const names = d[1].split(",").map((s) => s.trim()).filter((s) => /^[a-z0-9_]+$/.test(s))
        if (names.length) dependencies = names
      }
    }
    body = md.slice(fm[0].length)
  }
  if (!description) {
    const first = body.trim().split(/\r?\n/).find((l) => l.trim())
    description = (first ? first.replace(/^#+\s*/, "").trim() : name).slice(0, 120)
  }
  return { description: description || name, systemPrompt: body.trim(), dependencies }
}
