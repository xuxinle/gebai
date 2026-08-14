/**
 * 纯提示词子 Agent（简化定义）的 md 解析：`sub-agents/{name}/{name}.md` 单独存在（无同名 ts）时，
 * 直接由 md 构成 SubAgentDef——零 TS 代码的简单/组合式子 Agent。
 *
 * 可选 frontmatter（YAML 风格，仅识别 description）：
 * ```md
 * ---
 * description: 一句话能力描述（缺省取正文首行）
 * ---
 * 系统提示词正文
 * ```
 */
export interface ParsedSubAgentMd {
  description: string
  systemPrompt: string
}

export function parseSubAgentMd(name: string, md: string): ParsedSubAgentMd {
  let description = ""
  let body = md
  const fm = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/.exec(md)
  if (fm) {
    for (const line of fm[1].split(/\r?\n/)) {
      const m = /^description\s*:\s*(.+)$/.exec(line)
      if (m) description = m[1].trim()
    }
    body = md.slice(fm[0].length)
  }
  if (!description) {
    const first = body.trim().split(/\r?\n/).find((l) => l.trim())
    description = (first ? first.replace(/^#+\s*/, "").trim() : name).slice(0, 120)
  }
  return { description: description || name, systemPrompt: body.trim() }
}
