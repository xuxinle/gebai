/**
 * 环境变量双读（`GEBAI_LLM_INFER_*` 新名 + `LOCAL_INFER_*` 旧名）测试。
 *
 * 要钉住的是两件事：
 *   ① **旧名必须继续生效**——这些变量是部署面配置（.env / docker / 任务级 env 都是持久化的），
 *      只认新名会让存量部署静默失效（服务照常起，档位/目标/目录全回缺省，极难排查）；
 *   ② **新名优先**——两套都配时以新名为准，且旧名的存在要被如实报告（不能悄悄用旧值）。
 */
import { describe, expect, test } from "bun:test"
import { envDeprecationLines, envNum, envVar, hasEnvVar, resetEnvDeprecations, ENV_NEW_PREFIX, ENV_OLD_PREFIX } from "./env"

describe("envVar：新名优先、旧名兼容", () => {
  test("只配新名 → 读到新值，不报弃用", () => {
    resetEnvDeprecations()
    const env = { [`${ENV_NEW_PREFIX}TARGETS`]: "new" }
    expect(envVar(env, "TARGETS")).toBe("new")
    expect(envDeprecationLines(env)).toEqual([])
  })

  test("只配旧名 → **仍然生效**，并给出改名提示", () => {
    resetEnvDeprecations()
    const env = { [`${ENV_OLD_PREFIX}TARGETS`]: "old" }
    expect(envVar(env, "TARGETS")).toBe("old")
    const lines = envDeprecationLines(env)
    expect(lines.length).toBe(1)
    expect(lines[0]).toContain("LOCAL_INFER_TARGETS")
    expect(lines[0]).toContain("GEBAI_LLM_INFER_TARGETS")
  })

  test("两套都配 → 新名优先（旧名不参与）", () => {
    resetEnvDeprecations()
    const env = { [`${ENV_NEW_PREFIX}PORT`]: "9090", [`ENV_OLD`]: "1", [`${ENV_OLD_PREFIX}PORT`]: "8080" }
    expect(envVar(env, "PORT")).toBe("9090")
  })

  test("空串/空白视作未配置（新名空串不应把旧名挡掉）", () => {
    resetEnvDeprecations()
    const env = { [`${ENV_NEW_PREFIX}PORT`]: "  ", [`${ENV_OLD_PREFIX}PORT`]: "8080" }
    expect(envVar(env, "PORT")).toBe("8080")
  })

  test("都没配 → undefined；env 为 undefined 不炸", () => {
    resetEnvDeprecations()
    expect(envVar({}, "PORT")).toBeUndefined()
    expect(envVar(undefined, "PORT")).toBeUndefined()
  })

  test("envNum：新旧都能取数，非法值当未配置", () => {
    resetEnvDeprecations()
    expect(envNum({ [`${ENV_NEW_PREFIX}TIMEOUT_MS`]: "1234" }, "TIMEOUT_MS")).toBe(1234)
    expect(envNum({ [`${ENV_OLD_PREFIX}TIMEOUT_MS`]: "5678" }, "TIMEOUT_MS")).toBe(5678)
    expect(envNum({ [`${ENV_NEW_PREFIX}TIMEOUT_MS`]: "abc" }, "TIMEOUT_MS")).toBeUndefined()
  })

  test("hasEnvVar：新旧任一即视为已配置", () => {
    resetEnvDeprecations()
    expect(hasEnvVar({ [`${ENV_OLD_PREFIX}HOME`]: "/x" }, "HOME")).toBe(true)
    expect(hasEnvVar({}, "HOME")).toBe(false)
  })

  test("多个旧名一起用时，提示行去重且逐项列出（便于一次改完）", () => {
    resetEnvDeprecations()
    const env = { [`${ENV_OLD_PREFIX}PORT`]: "1", [`${ENV_OLD_PREFIX}TARGETS`]: "[]" }
    envVar(env, "PORT")
    envVar(env, "TARGETS")
    const lines = envDeprecationLines(env)
    expect(lines.length).toBe(1)
    expect(lines[0]).toContain("LOCAL_INFER_PORT")
    expect(lines[0]).toContain("LOCAL_INFER_TARGETS")
  })
})

// ── 与真实读取点联动：改名的关键在于「路径/端口/档位」这些真被读到的地方 ──────────

describe("envVar 与真实读取点联动", () => {
  test("LOCAL_INFER_HOME 旧名仍能定位子项目根（paths.inferHome）", async () => {
    const { inferHome } = await import("./paths")
    resetEnvDeprecations()
    expect(inferHome({ [`${ENV_OLD_PREFIX}HOME`]: "/tmp/old-home" })).toBe("/tmp/old-home")
    expect(inferHome({ [`${ENV_NEW_PREFIX}HOME`]: "/tmp/new-home" })).toBe("/tmp/new-home")
  })

  test("LOCAL_INFER_PORT 旧名仍决定缺省端口（paths.defaultPort）", async () => {
    const { defaultPort } = await import("./paths")
    resetEnvDeprecations()
    expect(defaultPort({ [`${ENV_OLD_PREFIX}PORT`]: "18080" })).toBe(18080)
    expect(defaultPort({ [`${ENV_NEW_PREFIX}PORT`]: "18081" })).toBe(18081)
    expect(defaultPort({})).toBe(8080)
  })
})
