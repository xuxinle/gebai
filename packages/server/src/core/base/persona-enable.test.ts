/**
 * 「人格必须显式启用」：`GEBAI_PERSONA_ENABLE` 未配置 = 一个都不启用。
 *
 * 这套用例咬住的是**判定的边界**，因为这里判错的两个方向代价都很高：
 * - 不该启用却启用 → 放个定义文件就凭空开起人来（会真的以用户名义发言）
 * - 该装配却没装配 → 面板/日志分不清“没定义”“没启用”“装配坏了”
 *
 * 所以重点验证：**装配（建组件、开 REST 面）与启用（谁干活）是两件事**。
 */
import { describe, expect, test } from "bun:test"
import { hasPersonaConfig } from "./config"

describe("hasPersonaConfig：要不要装配拟人运行时", () => {
  test("没有任何配置 → false", () => {
    expect(hasPersonaConfig({})).toBe(false)
  })

  test("有某个人格的配置 → true（装出运行时，但谁干活另由 ENABLE 定）", () => {
    expect(hasPersonaConfig({ GEBAI_PERSONA_XIAOSHUANG: '[{"name":"support"}]' })).toBe(true)
  })

  test("人格定义文件放进去不等于启用（hasPersonaConfig 只回答“装不装运行时”）", () => {
    // 真正的“谁在干活”由 applyPersonaEnv 判（空名单 = 全禁用），见 env-config.test.ts
    expect(hasPersonaConfig({ GEBAI_PERSONA_XIAOSHUANG: "[]" })).toBe(true)
  })

  test("启用人格白名单 → true", () => {
    expect(hasPersonaConfig({ GEBAI_PERSONA_ENABLE: "xiaoshuang" })).toBe(true)
  })

  test("**只有公共行为参数 → false**（改审批名单/行为参数不该开起人格）", () => {
    const sharedOnly = {
      GEBAI_PERSONA_APPROVAL_ASK_TOOLS: "sh,py",
      GEBAI_PERSONA_YIELD_SECONDS: "180",
      GEBAI_PERSONA_REPLY_TO_OWNER: "true",
      GEBAI_PERSONA_SESSION_ROUTING: "driver",
    }
    console.log(`    实测: ${hasPersonaConfig(sharedOnly)}`)
    expect(hasPersonaConfig(sharedOnly)).toBe(false)
  })

  test("公共参数 + 一个人格配置 → true（人格配置才是开关）", () => {
    expect(
      hasPersonaConfig({ GEBAI_PERSONA_APPROVAL_ASK_TOOLS: "sh", GEBAI_PERSONA_XIAOSHUANG: "[{}]" }),
    ).toBe(true)
  })

  test("空值的 GEBAI_PERSONA_<ID> 不算配置（等号后面什么都没有）", () => {
    expect(hasPersonaConfig({ GEBAI_PERSONA_XIAOSHUANG: "   " })).toBe(false)
  })

  test("大小写与其它前缀不干扰", () => {
    expect(hasPersonaConfig({ GEBAI_PERSONALITY_X: "[{}]" })).toBe(false)
  })
})
