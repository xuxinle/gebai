/**
 * 工具审批名单：**默认全部不审批，只有名单里的工具弹窗问人**。
 *
 * 这套规则是安全相关的：判错的方向有两种，都不报错——
 * - **该弹窗却没弹**（名单没命中）：一条 `rm -rf` 静默执行了
 * - **不该弹却弹了**（名单过宽）：人格在无人值守场景里被一张卡卡死
 *
 * 所以这里把匹配语义穷举钉住，尤其是“通配只认尾部”这个刻意的限制。
 */
import { describe, expect, test } from "bun:test"
import { askTools, matchesAskTool, needsApproval, parseAskToolList, unknownAskTools } from "./approval-tools"

describe("名单解析", () => {
  test("逗号 / 空白 / 混合分隔", () => {
    expect(parseAskToolList("sh,py")).toEqual(["sh", "py"])
    expect(parseAskToolList("sh py")).toEqual(["sh", "py"])
    expect(parseAskToolList(" sh , py ,file.delete ")).toEqual(["sh", "py", "file.delete"])
  })

  test("空 / 空白 → 空数组（= 谁都不问，即缺省行为）", () => {
    expect(parseAskToolList(undefined)).toEqual([])
    expect(parseAskToolList("")).toEqual([])
    expect(parseAskToolList("   ")).toEqual([])
    expect(parseAskToolList(", ,")).toEqual([])
  })

  test("去重保留首次出现顺序（重复写不该有新含义）", () => {
    expect(parseAskToolList("sh,py,sh")).toEqual(["sh", "py"])
  })

  test("askTools 从环境变量读", () => {
    expect(askTools({ GEBAI_PERSONA_APPROVAL_ASK_TOOLS: "sh, file.delete" })).toEqual(["sh", "file.delete"])
    expect(askTools({})).toEqual([])
  })
})

describe("单个匹配", () => {
  test("精确名", () => {
    expect(matchesAskTool("sh", "sh")).toBe(true)
    expect(matchesAskTool("sh", "py")).toBe(false)
    // 工具名是标识符：大小写敏感（`SH` 不该命中 `sh`——那是拼写错误，该暴露）
    expect(matchesAskTool("SH", "sh")).toBe(false)
  })

  test("带点的名字（file.delete）按字面匹配，`*` 不是正则", () => {
    expect(matchesAskTool("file.delete", "file.delete")).toBe(true)
    // 若误当正则会命中 fileXdelete；这里必须是 false
    expect(matchesAskTool("file.delete", "fileXdelete")).toBe(false)
  })

  test("尾部通配：`code_*` 命中同前缀，不命中别的前缀", () => {
    expect(matchesAskTool("code_*", "code_read")).toBe(true)
    expect(matchesAskTool("code_*", "code_sh")).toBe(true)
    expect(matchesAskTool("code_*", "codex_read")).toBe(false)
    expect(matchesAskTool("code_*", "sh")).toBe(false)
  })

  test("`*` 命中一切（要“什么都问”就写它）", () => {
    for (const n of ["sh", "py", "file.delete", "code_read", "anything"]) {
      expect(matchesAskTool("*", n)).toBe(true)
    }
  })

  test("刻意不支持中间通配（难以预测的规则比不管更糟）", () => {
    // `code_*_tool` 不按通配解释 → 只能字面命中
    expect(matchesAskTool("code_*_tool", "code_read_tool")).toBe(false)
    expect(matchesAskTool("code_*_tool", "code_*_tool")).toBe(true)
  })

  test("空条目 / 空工具名", () => {
    expect(matchesAskTool("", "sh")).toBe(false)
    expect(matchesAskTool("sh", "")).toBe(false)
  })
})

describe("任一命中即审批", () => {
  test("空名单 → 谁都不问（缺省）", () => {
    expect(needsApproval([], "sh")).toBe(false)
    expect(needsApproval([], "file.delete")).toBe(false)
  })

  test("名单里只有 sh → 别的工具不问", () => {
    expect(needsApproval(["sh"], "sh")).toBe(true)
    expect(needsApproval(["sh"], "py")).toBe(false)
  })

  test("混合精确名与通配", () => {
    const list = ["sh", "code_*"]
    expect(needsApproval(list, "sh")).toBe(true)
    expect(needsApproval(list, "code_sh")).toBe(true)
    expect(needsApproval(list, "py")).toBe(false)
  })
})

describe("拼写检查（启动期告警用）", () => {
  test("精确名里不存在的工具会被报出来", () => {
    const known = new Set(["sh", "py", "file.delete"])
    expect(unknownAskTools(["sh", "shell", "delete"], known)).toEqual(["shell", "delete"])
  })

  test("通配条目跳过检查（子 Agent 可能还没装载）", () => {
    const known = new Set(["sh"])
    expect(unknownAskTools(["code_*", "*"], known)).toEqual([])
  })

  test("全部认识 → 空", () => {
    expect(unknownAskTools(["sh"], new Set(["sh"]))).toEqual([])
  })
})
