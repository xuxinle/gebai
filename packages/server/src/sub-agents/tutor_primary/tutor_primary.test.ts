import { describe, expect, test } from "bun:test"
import { def, name, tools } from "./tutor_primary"

describe("tutor_primary sub-agent", () => {
  test("def 结构与小学段配置", () => {
    expect(def.name).toBe("tutor_primary")
    expect(name).toBe("tutor_primary")
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
    expect(def.systemPrompt).toContain("小学（1-6 年级）")
    // 小学化教学方法落地在提示词（具象化/画图/读题先行/防挫败）
    expect(def.systemPrompt).toContain("一切从具体开始")
    expect(def.systemPrompt).toContain("读题先行")
    expect(def.systemPrompt).toContain("连续错 2 次立即降难度")
    // 提示词内工具引用统一用 tutor_primary_ 命名空间
    expect(def.systemPrompt).toContain("tutor_primary_knowledge")
    expect(def.systemPrompt).not.toContain("tutor_secondary_")
    // 路由描述含中学边界指引
    expect(def.description).toContain("tutor_secondary")
    // 三种身份（学生/家长/教师）与主动拓展引导落地在提示词
    expect(def.systemPrompt).toContain("身份适配")
    expect(def.systemPrompt).toContain("主动拓展引导")
    expect(def.systemPrompt).toContain("陪学顾问")
    expect(def.systemPrompt).toContain("教学助手")
    expect(def.description).toContain("三种身份")
  })
})
