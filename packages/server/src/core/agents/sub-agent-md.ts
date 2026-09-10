/**
 * 纯提示词子 Agent（简化定义）的 md 解析：`sub-agents/{name}/{name}.md` 单独存在（无同名 ts）时，
 * 直接由 md 构成 SubAgentDef——零 TS 代码的简单/组合式子 Agent。
 *
 * 可选 frontmatter（YAML 风格，识别 description/dependencies/preload/env_vars）：
 * ```md
 * ---
 * description: 一句话能力描述（缺省取正文首行）
 * dependencies: playwright, code（依赖的子Agent 名，逗号分隔——装载时自动连带装载）
 * preload: true（启动即装载）
 * env_vars:（可配置环境变量声明，须以 {NAME 大写}_ 前缀）
 *   - name: MY_AGENT_TOKEN
 *     description: 访问令牌
 * ---
 * 系统提示词正文
 * ```
 */
export interface ParsedSubAgentMd {
  description: string
  systemPrompt: string
  /** 依赖的子Agent 名单（frontmatter `dependencies: a, b`；未声明为 undefined）。 */
  dependencies?: string[]
  /** 是否预加载（frontmatter `preload: true`；未声明为 undefined）。 */
  preload?: boolean
  /** 可配置环境变量声明（frontmatter `env_vars:` 列表；未声明为 undefined）。 */
  envVars?: Array<{ name: string; description: string }>
}

export function parseSubAgentMd(name: string, md: string): ParsedSubAgentMd {
  let description = ""
  let dependencies: string[] | undefined
  let preload: boolean | undefined
  let envVars: Array<{ name: string; description: string }> | undefined
  let body = md
  const fm = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/.exec(md)
  if (fm) {
    for (const [i, line] of fm[1].split(/\r?\n/).entries()) {
      const m = /^description\s*:\s*(.+)$/.exec(line)
      if (m) description = m[1].trim()
      const d = /^dependencies\s*:\s*(.+)$/.exec(line)
      if (d) {
        const names = d[1].split(",").map((s) => s.trim()).filter((s) => /^[a-z0-9_]+$/.test(s))
        if (names.length) dependencies = names
      }
      const p = /^preload\s*:\s*(true|false)$/.exec(line)
      if (p) preload = p[1] === "true"
      // env_vars 列表项：`- name: VAR` 行 + 后续缩进 `description: ...` 行（变量名须以 {name 大写}_ 前缀）
      const ev = /^\s*-\s*name\s*:\s*([A-Z][A-Z0-9_]*)\s*$/.exec(line)
      if (ev) {
        const rest = fm[1].split(/\r?\n/).slice(i + 1)
        const descLine = rest.find((l) => /^\s+description\s*:\s*(.+)$/.test(l))
        const desc = descLine ? /^description\s*:\s*(.+)$/.exec(descLine.trim())?.[1]?.trim() ?? "" : ""
        const prefix = `${name.toUpperCase().replace(/[^A-Z0-9]/g, "_")}_`
        if (ev[1].startsWith(prefix)) {
          envVars ??= []
          envVars.push({ name: ev[1], description: desc })
        }
      }
    }
    body = md.slice(fm[0].length)
  }
  if (!description) {
    const first = body.trim().split(/\r?\n/).find((l) => l.trim())
    description = (first ? first.replace(/^#+\s*/, "").trim() : name).slice(0, 120)
  }
  return { description: description || name, systemPrompt: body.trim(), dependencies, preload, envVars }
}
