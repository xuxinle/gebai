import { describe, expect, test } from "bun:test"
import { def, name, tools } from "./tutor_secondary"

describe("tutor_secondary sub-agent", () => {
  test("def 结构与中学段配置", () => {
    expect(def.name).toBe("tutor_secondary")
    expect(name).toBe("tutor_secondary")
    expect(Object.keys(tools).sort()).toEqual([
      "knowledge",
      "mistake_add",
      "mistake_list",
      "mistake_remove",
      "mistake_review",
      "profile",
    ])
    expect(def.preload).toBe(false)
    expect(def.requiresApproval).toEqual({ mistake_remove: true })
    expect(def.systemPrompt).toContain("初中与高中")
    // 提示词内工具引用统一用 tutor_secondary_ 命名空间（无残留旧 tutor_ 前缀）
    expect(def.systemPrompt).toContain("tutor_secondary_knowledge")
    expect(def.systemPrompt).not.toMatch(/(?<!_secondary)_mistake_add/)
    // 路由描述含小学边界指引
    expect(def.description).toContain("tutor_primary")
  })
})
