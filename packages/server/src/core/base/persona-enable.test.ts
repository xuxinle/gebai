/**
 * 「有配置就启用」：拟人与渠道都不再有独立开关。
 *
 * 这套用例咬住的是**判定的边界**，因为这里判错的两个方向代价都很高：
 * - 该启用没启用 → 配了却不干活（而界面上看不到“开关”这个东西，更难查）
 * - 不该启用却启用 → 只改了一个行为参数，就凭空开起人来（会真的以用户名义发言）
 *
 * 所以重点验证：**公共行为参数不算配置**。
 */
import { describe, expect, test } from "bun:test"
import { hasPersonaConfig } from "./config"

describe("hasPersonaConfig：配了就启用", () => {
  test("没有任何配置 → false", () => {
    expect(hasPersonaConfig({})).toBe(false)
  })

  test("有某个人格的配置 → true（这就是“启用”）", () => {
    expect(hasPersonaConfig({ GEBAI_PERSONA_XIAOSHUANG: '[{"name":"support"}]' })).toBe(true)
  })

  test("有一份空数组的人格配置 → 仍算配了（明确写了 GEBAI_PERSONA_<ID>）", () => {
    // 这是刻意选择：写了空数组的人是在说“这个人格我配了但没加分身”，
    // 不该被当成“没配置”——否则面板会一直说未启用。
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
