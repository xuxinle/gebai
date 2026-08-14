import { describe, expect, test } from "bun:test"
import { parseSubAgentMd } from "./sub-agent-md"

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
})
