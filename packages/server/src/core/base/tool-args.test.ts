import { describe, expect, test } from "bun:test"
import { tolerantToolName, normalizeToolArgs } from "./tool-args"
import type { Tool } from "./types"

function toolWithProps(properties: Record<string, unknown>, required: string[] = []): Tool {
  return { name: "t", description: "", parameters: { type: "object", properties, required }, execute: async () => ({ output: "" }) }
}

describe("tolerantToolName（工具名容错归一）", () => {
  test("分隔符（./-/:）与驼峰归一蛇形", () => {
    expect(tolerantToolName("agent.run")).toBe("agent_run")
    expect(tolerantToolName("agent-run")).toBe("agent_run")
    expect(tolerantToolName("agent:run")).toBe("agent_run")
    expect(tolerantToolName("agentRun")).toBe("agent_run")
    expect(tolerantToolName("AgentRun")).toBe("agent_run")
    expect(tolerantToolName("codeRead")).toBe("code_read")
  })
  test("连续大写缩写拆点（HTTPFetch→http_fetch）；已合规蛇形恒等", () => {
    expect(tolerantToolName("HTTPFetch")).toBe("http_fetch")
    expect(tolerantToolName("agent_run")).toBe("agent_run")
    expect(tolerantToolName("read")).toBe("read")
    expect(tolerantToolName("bg_task")).toBe("bg_task")
  })
})

describe("normalizeToolArgs（参数键指纹归一，schema 驱动递归）", () => {
  const editLike = toolWithProps(
    {
      path: { type: "string" },
      edits: {
        type: "array",
        items: { type: "object", properties: { old_string: { type: "string" }, new_string: { type: "string" }, replace_all: { type: "boolean" } }, required: ["old_string", "new_string"] },
      },
      dry_run: { type: "boolean" },
    },
    ["path"],
  )

  test("驼峰/连字符/大小写键归一到 schema 蛇形键（含嵌套数组元素）", () => {
    const out = normalizeToolArgs(editLike, {
      path: "a.txt",
      dryRun: true,
      "dry-run2": 1 as never,
      edits: [{ oldString: "a", newString: "b", replaceAll: true }, { OLD_STRING: "c", new_string: "d" }],
    })
    expect(out.dry_run).toBe(true)
    // schema 外的键原样保留（任意透传参数不受影响）
    expect(out["dry-run2"]).toBe(1)
    const edits = out.edits as Array<Record<string, unknown>>
    expect(edits[0]).toEqual({ old_string: "a", new_string: "b", replace_all: true })
    expect(edits[1]).toEqual({ old_string: "c", new_string: "d" })
  })

  test("精确键优先；无 schema 可依（无 properties）时原样返回", () => {
    const out = normalizeToolArgs(editLike, { path: "x", dry_run: false })
    expect(out).toEqual({ path: "x", dry_run: false })
    const bare = { name: "t", description: "", parameters: { type: "object" }, execute: async () => ({ output: "" }) } as unknown as Tool
    expect(normalizeToolArgs(bare, { anyKey: 1, oldString: 2 })).toEqual({ anyKey: 1, oldString: 2 })
  })

  test("指纹冲突（两 schema 键同指纹）只认精确匹配，不做容错改写", () => {
    const t = toolWithProps({ full_text: { type: "string" }, fulltext: { type: "string" } })
    const out = normalizeToolArgs(t, { fullText: "x", full_text: "y" })
    // fullText 指纹歧义（full_text/fulltext 均匹配）→ 保持原键；精确键不动
    expect(out.full_text).toBe("y")
    expect(out.fullText).toBe("x")
    expect("fulltext" in out).toBe(false)
  })

  test("嵌套对象按子 schema 递归；非对象/数组值不动", () => {
    const t = toolWithProps({ style: { type: "object", properties: { base_font: { type: "string" }, base_size: { type: "number" } } } })
    const out = normalizeToolArgs(t, { style: { baseFont: "serif", baseSize: 12, keepMe: true } })
    expect(out.style).toEqual({ base_font: "serif", base_size: 12, keepMe: true })
  })
})
