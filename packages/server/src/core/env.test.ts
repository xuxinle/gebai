import { describe, expect, test } from "bun:test"
import { applyEmbeddedEnvDefaults } from "./env"

describe("applyEmbeddedEnvDefaults（构建期内置模型配置默认值）", () => {
  test("仅填充未设置或空串的键，已设置的键不覆盖", () => {
    const target: Record<string, string | undefined> = {
      GEBAI_LLM_MODEL: "my-own-model", // 运行时显式设置：保留
      GEBAI_LLM_API_BASE: "", // 空串视为未设置：填充
      // GEBAI_LLM_API_KEY 未出现：填充
      UNRELATED: "keep",
    }
    const applied = applyEmbeddedEnvDefaults(
      { GEBAI_LLM_MODEL: "deepseek-chat", GEBAI_LLM_API_BASE: "https://api.example.com", GEBAI_LLM_API_KEY: "sk-x" },
      target,
    )
    expect(applied.sort()).toEqual(["GEBAI_LLM_API_BASE", "GEBAI_LLM_API_KEY"])
    expect(target.GEBAI_LLM_MODEL).toBe("my-own-model")
    expect(target.GEBAI_LLM_API_BASE).toBe("https://api.example.com")
    expect(target.GEBAI_LLM_API_KEY).toBe("sk-x")
    expect(target.UNRELATED).toBe("keep")
  })
  test("空内置/空键安全（无操作）", () => {
    const target: Record<string, string | undefined> = { A: "1" }
    expect(applyEmbeddedEnvDefaults({}, target)).toEqual([])
    expect(applyEmbeddedEnvDefaults({ "": "x", B: undefined as unknown as string }, target)).toEqual([])
    expect(target).toEqual({ A: "1" })
  })
})
