import { describe, expect, test } from "bun:test"
import { filterEnvToCatalog, type EnvCatalogGroup } from "./env-local"

const GROUPS: EnvCatalogGroup[] = [
  {
    group: "global",
    label: "全局",
    vars: [
      { name: "GEBAI_LLM_MODEL", description: "主模型" },
      { name: "GEBAI_LLM_API_KEY", description: "密钥" },
    ],
  },
  {
    group: "code",
    label: "code",
    vars: [{ name: "CODE_PROJECT", description: "项目根" }],
  },
]

describe("filterEnvToCatalog（只保留目录内变量）", () => {
  test("目录内变量保留，自定义变量丢弃", () => {
    const out = filterEnvToCatalog({ GEBAI_LLM_MODEL: "m1", CODE_PROJECT: "/p", MY_CUSTOM: "x" }, GROUPS)
    expect(out).toEqual({ GEBAI_LLM_MODEL: "m1", CODE_PROJECT: "/p" })
    expect("MY_CUSTOM" in out).toBe(false)
  })

  test("空目录/空输入安全", () => {
    expect(filterEnvToCatalog({}, GROUPS)).toEqual({})
    expect(filterEnvToCatalog({ A: "1" }, [])).toEqual({})
  })

  test("值原样保留（含空字符串丢弃由调用方负责）", () => {
    const out = filterEnvToCatalog({ GEBAI_LLM_API_KEY: "" }, GROUPS)
    expect(out).toEqual({ GEBAI_LLM_API_KEY: "" })
  })
})
