import { describe, expect, test } from "bun:test"
import { getEnvCatalog } from "./env-catalog"
import { def as codeDef } from "../sub-agents/code"
import { def as selfOptimizeDef } from "../sub-agents/self_optimize"
import { def as feishuDocsDef } from "../sub-agents/feishu_docs/feishu_docs"

const defs = [codeDef, selfOptimizeDef, feishuDocsDef]

describe("环境变量目录（前端配置白名单）", () => {
  test("分组：全局 + 各子Agent（envVars 由子Agent 导出汇总）", () => {
    const groups = getEnvCatalog(defs)
    const names = groups.map((g) => g.group)
    expect(names).toContain("global")
    expect(names).toContain("code")
    expect(names).toContain("self_optimize")
    expect(names).toContain("feishu_docs")
  })

  test("未声明 envVars 的子Agent 不产生分组", () => {
    const groups = getEnvCatalog([{ name: "desktop", description: "d", systemPrompt: "p" }])
    const names = groups.map((g) => g.group)
    expect(names).toEqual(["global"])
  })

  test("全局组含模型相关配置（LLM 与视觉）", () => {
    const global = getEnvCatalog(defs).find((g) => g.group === "global")!
    const varNames = global.vars.map((v) => v.name)
    for (const n of ["GEBAI_LLM_MODEL", "GEBAI_LLM_API_BASE", "GEBAI_LLM_API_KEY", "GEBAI_LLM_API_KIND", "GEBAI_LLM_MAX_CONTEXT", "GEBAI_LLM_EXTRA_PARAMS", "GEBAI_LLM_MULTIMODAL", "GEBAI_VISION_MODEL", "GEBAI_VISION_API_BASE", "GEBAI_VISION_API_KEY", "GEBAI_VISION_API_KIND"]) {
      expect(varNames).toContain(n)
    }
  })

  test("不含启动级/安全敏感特殊变量", () => {
    const all = getEnvCatalog(defs).flatMap((g) => g.vars.map((v) => v.name))
    for (const n of ["GEBAI_MODE", "GEBAI_HOST", "GEBAI_PORT", "GEBAI_ADMIN_PASSWORD_HASH", "GEBAI_SAFE_MODE", "GEBAI_SANDBOX", "GEBAI_BASE_PATH", "GEBAI_CORS_ORIGINS", "GEBAI_SERVICE_API_KEY"]) {
      expect(all).not.toContain(n)
    }
  })

  test("GEBAI_APPROVAL_SKIP 对所有用户可见（会话级审批跳过开放给用户本人，目录不再按角色过滤）", () => {
    const global = getEnvCatalog(defs).find((g) => g.group === "global")!
    expect(global.vars.map((v) => v.name)).toContain("GEBAI_APPROVAL_SKIP")
    // 子Agent 组不受影响
    const feishu = getEnvCatalog(defs).find((g) => g.group === "feishu_docs")!
    expect(feishu.vars.map((v) => v.name)).toContain("FEISHU_DOCS_APP_ID")
  })

  test("变量名合法且全局无重复；子Agent 变量以 {AGENT}_ 前缀开头", () => {
    const groups = getEnvCatalog(defs)
    const seen = new Set<string>()
    for (const g of groups) {
      for (const v of g.vars) {
        expect(v.name).toMatch(/^[A-Za-z_][A-Za-z0-9_]*$/)
        expect(seen.has(v.name)).toBe(false)
        seen.add(v.name)
        if (g.group !== "global") {
          expect(v.name.startsWith(`${g.group.toUpperCase()}_`)).toBe(true)
        }
        expect(v.description.length).toBeGreaterThan(0) // tip 说明非空
      }
    }
  })
})
