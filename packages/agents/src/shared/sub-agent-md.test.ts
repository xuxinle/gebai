import { describe, expect, test } from "bun:test"
import { mdSubAgentDef, parseSubAgentMd } from "./sub-agent-md"

describe("parseSubAgentMd", () => {
  test("parses frontmatter description and body as system prompt", () => {
    const md = `---
description: 组合 Agent：编排多个子 Agent
---
你是编排专家。正文第一段。`
    const r = parseSubAgentMd("composer", md)
    expect(r.description).toBe("组合 Agent：编排多个子 Agent")
    expect(r.systemPrompt).toBe("你是编排专家。正文第一段。")
  })

  test("falls back to first body line when no frontmatter", () => {
    const md = `# 报告专家\n\n负责撰写技术报告。`
    const r = parseSubAgentMd("reporter", md)
    expect(r.description).toBe("报告专家")
    expect(r.systemPrompt).toBe("# 报告专家\n\n负责撰写技术报告。")
  })

  test("falls back to name for empty body", () => {
    const r = parseSubAgentMd("ghost", "   ")
    expect(r.description).toBe("ghost")
    expect(r.systemPrompt).toBe("")
  })

  test("truncates long first-line description to 120 chars", () => {
    const long = "x".repeat(200)
    const r = parseSubAgentMd("longy", long)
    expect(r.description).toBe(long.slice(0, 120))
  })

  test("frontmatter without description falls back to body first line", () => {
    const md = "---\nno description here\n---\n正文"
    const r = parseSubAgentMd("weird", md)
    expect(r.description).toBe("正文")
    expect(r.systemPrompt).toBe("正文")
  })

  test("handles CRLF line endings", () => {
    const md = "---\r\ndescription: CRLF 描述\r\n---\r\n正文第一段\r\n第二段"
    const r = parseSubAgentMd("crlf", md)
    expect(r.description).toBe("CRLF 描述")
    expect(r.systemPrompt).toBe("正文第一段\r\n第二段")
  })

  test("unclosed frontmatter marker is treated as body", () => {
    const md = "---\n这不是 frontmatter\n正文"
    const r = parseSubAgentMd("open", md)
    expect(r.description).toBe("---")
    expect(r.systemPrompt).toBe("---\n这不是 frontmatter\n正文")
  })

  test("parses frontmatter dependencies (逗号分隔，装载时自动连带装载)", () => {
    const md = "---\ndescription: 站点逆向组合\ndependencies: playwright, code\n---\n你是组合专家。"
    const r = parseSubAgentMd("revcombo", md)
    expect(r.dependencies).toEqual(["playwright", "code"])
    expect(r.description).toBe("站点逆向组合")
  })

  test("dependencies 过滤非法条目（命名规则 [a-z0-9_]+）；未声明为 undefined", () => {
    const md = "---\ndependencies: playwright, Bad-Name, ,9x\n---\n正文"
    const r = parseSubAgentMd("pick", md)
    expect(r.dependencies).toEqual(["playwright", "9x"])
    expect(parseSubAgentMd("none", "正文").dependencies).toBeUndefined()
  })

  test("parses frontmatter preload（true/false；非严格值与未声明为 undefined）", () => {
    expect(parseSubAgentMd("a", "---\npreload: true\n---\n正文").preload).toBe(true)
    expect(parseSubAgentMd("a", "---\npreload: false\n---\n正文").preload).toBe(false)
    expect(parseSubAgentMd("a", "---\npreload: yes\n---\n正文").preload).toBeUndefined()
    expect(parseSubAgentMd("a", "正文").preload).toBeUndefined()
  })

  test("parses frontmatter env_vars 列表（name 须以 {name 大写}_ 前缀，description 取后续缩进行）", () => {
    const md = [
      "---",
      "description: 组合助手",
      "env_vars:",
      "  - name: COMBO_HELPER_TOKEN",
      "    description: 访问令牌（敏感）",
      "  - name: COMBO_HELPER_BASE_URL",
      "    description: 服务地址",
      "  - name: WRONG_PREFIX_KEY",
      "    description: 前缀不符应忽略",
      "---",
      "正文",
    ].join("\n")
    const r = parseSubAgentMd("combo_helper", md)
    expect(r.envVars).toEqual([
      { name: "COMBO_HELPER_TOKEN", description: "访问令牌（敏感）" },
      { name: "COMBO_HELPER_BASE_URL", description: "服务地址" },
    ])
    expect(parseSubAgentMd("none", "正文").envVars).toBeUndefined()
  })
})

describe("mdSubAgentDef", () => {
  test("builds SubAgentDef from plain md (no frontmatter)", () => {
    const def = mdSubAgentDef("reporter", "# 报告专家\n\n负责撰写技术报告。")
    expect(def.name).toBe("reporter")
    expect(def.description).toBe("报告专家")
    expect(def.systemPrompt).toBe("# 报告专家\n\n负责撰写技术报告。")
    expect(def.dependencies).toBeUndefined()
    expect(def.preload).toBeUndefined()
    expect(def.envVars).toBeUndefined()
  })

  test("carries frontmatter fields (dependencies/preload/env_vars)", () => {
    const md = `---
description: 组合 Agent
dependencies: playwright, code
preload: true
env_vars:
  - name: COMPOSER_TOKEN
    description: 访问令牌
---
正文`
    const def = mdSubAgentDef("composer", md)
    expect(def.description).toBe("组合 Agent")
    expect(def.dependencies).toEqual(["playwright", "code"])
    expect(def.preload).toBe(true)
    expect(def.envVars).toEqual([{ name: "COMPOSER_TOKEN", description: "访问令牌" }])
  })
})
