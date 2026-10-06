import { describe, expect, test } from "bun:test"

describe("ai_native_eng（知识型子Agent）", () => {
  test("def 导出完整，且无工具（知识型，不接项目）", async () => {
    const mod = await import("./ai_native_eng")
    expect(mod.name).toBe("ai_native_eng")
    expect(mod.def.name).toBe(mod.name)
    expect(mod.def.systemPrompt.length).toBeGreaterThan(1000)
    // 知识型子Agent：不提供任何工具（它只回答"怎么做"，不动手）
    expect(mod.def.tools ?? {}).toEqual({})
    expect(mod.def.preload).toBe(false)
  })

  test("说明它的定位与边界（不接项目/不写代码）", async () => {
    const mod = await import("./ai_native_eng")
    expect(mod.description).toContain("不接项目")
    expect(mod.description).toContain("不写代码")
    // 与相邻子Agent 的分工要写清，否则会被误用
    expect(mod.description).toContain("code")
    expect(mod.description).toContain("shuangtian")
  })

  test("提示词含七节方法论骨架（缺一节即知识不完整）", async () => {
    const prompt = (await import("./ai_native_eng")).def.systemPrompt
    // ① 形态差异 ② 无头闭环 ③ 协同自进化 ④ 观感类改动 ⑤ 智能体纪律 ⑥ 跨平台 ⑦ 汇报纪律
    expect(prompt).toContain("AI 原生")
    expect(prompt).toContain("无头闭环")
    expect(prompt).toContain("协同自进化")
    expect(prompt).toContain("观感类改动")
    expect(prompt).toContain("记忆")
    expect(prompt).toContain("跨平台")
    expect(prompt).toContain("汇报纪律")
    // 边界诚实性：必须说明样本是 n=1、数字不可迁移
    expect(prompt).toContain("边界")
    expect(prompt).toContain("不可迁移")
  })

  test("每条约束都带实测代价（否则会退化成空话）", async () => {
    const prompt = (await import("./ai_native_eng")).def.systemPrompt
    // 三个最关键的事故引用
    expect(prompt).toContain("四次返工")
    expect(prompt).toContain("20 类")
    expect(prompt).toContain("--text-preset")
    // 判据层面的两条硬约束必须在内（可迁移性最强的两条）
    expect(prompt).toContain("正交量")
    expect(prompt).toContain("参照")
  })
})
