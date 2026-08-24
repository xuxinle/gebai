import { describe, expect, test } from "bun:test"
import { def, name, tools } from "./tutor_secondary"

describe("tutor_secondary sub-agent", () => {
  test("def 结构与中学段配置", () => {
    expect(def.name).toBe("tutor_secondary")
    expect(name).toBe("tutor_secondary")
    expect(Object.keys(tools).sort()).toEqual([
      "demo",
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
    // 三种身份（学生/家长/教师）与主动拓展引导落地在提示词
    expect(def.systemPrompt).toContain("身份适配")
    expect(def.systemPrompt).toContain("主动拓展引导")
    expect(def.systemPrompt).toContain("陪学顾问")
    expect(def.systemPrompt).toContain("教学助手")
    expect(def.description).toContain("三种身份")
    // 教学法落地：必然作图审题法 / 解后双归纳（分析路径+解法方法）/ 变式与原题比对
    expect(def.systemPrompt).toContain("作图审题")
    expect(def.systemPrompt).toContain("必然作图审题法")
    expect(def.systemPrompt).toContain("先纠图再解题")
    expect(def.systemPrompt).toContain("解后归纳")
    expect(def.systemPrompt).toContain("分析路径归纳")
    expect(def.systemPrompt).toContain("解法方法归纳")
    expect(def.systemPrompt).toContain("变式必与原题比对")
    expect(def.description).toContain("作图审题法")
  })
})
