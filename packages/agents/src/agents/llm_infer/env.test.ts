/**
 * 环境变量读取（`GEBAI_LLM_INFER_*`）测试。
 *
 * 要钉住的是：变量只认 `GEBAI_LLM_INFER_` 前缀；空串/空白视作未配置；
 * envNum 非法值当未配置；env 为 undefined 不炸。
 */
import { describe, expect, test } from "bun:test"
import { envNum, envVar, ENV_PREFIX } from "./env"

describe("envVar：GEBAI_LLM_INFER_ 前缀单读", () => {
  test("配置了 → 读到值（保留原文，含前后空白）", () => {
    const env = { [`${ENV_PREFIX}TARGETS`]: "[]" }
    expect(envVar(env, "TARGETS")).toBe("[]")
  })

  test("空串/空白/非字符串 → 视作未配置", () => {
    expect(envVar({ [`${ENV_PREFIX}PORT`]: "" }, "PORT")).toBeUndefined()
    expect(envVar({ [`${ENV_PREFIX}PORT`]: "   " }, "PORT")).toBeUndefined()
    expect(envVar({}, "PORT")).toBeUndefined()
    expect(envVar(undefined, "PORT")).toBeUndefined()
  })

  test("envNum：取数成功；非法值当未配置", () => {
    expect(envNum({ [`${ENV_PREFIX}TIMEOUT_MS`]: "1234" }, "TIMEOUT_MS")).toBe(1234)
    expect(envNum({ [`${ENV_PREFIX}TIMEOUT_MS`]: "abc" }, "TIMEOUT_MS")).toBeUndefined()
    expect(envNum({}, "TIMEOUT_MS")).toBeUndefined()
  })
})

// ── 与真实读取点联动：变量名要在「路径/端口/目标」这些真被读到的地方生效 ──────────

describe("envVar 与真实读取点联动", () => {
  test("GEBAI_LLM_INFER_HOME 定位子项目根（paths.inferHome）", async () => {
    const { inferHome } = await import("./paths")
    expect(inferHome({ [`${ENV_PREFIX}HOME`]: "/tmp/home-a" })).toBe("/tmp/home-a")
  })

  test("GEBAI_LLM_INFER_PORT 决定缺省端口（paths.defaultPort）", async () => {
    const { defaultPort } = await import("./paths")
    expect(defaultPort({ [`${ENV_PREFIX}PORT`]: "18081" })).toBe(18081)
    expect(defaultPort({})).toBe(8080)
  })

  test("GEBAI_LLM_INFER_DEFAULT_TARGET 改缺省推理目标（providers.resolveTarget）", async () => {
    const { resolveTarget } = await import("./providers")
    const ctx = { env: { [`${ENV_PREFIX}DEFAULT_TARGET`]: "http://192.168.1.20:8080", [`${ENV_PREFIX}HOME`]: "/tmp/home-b" } }
    const t = resolveTarget(undefined, ctx)
    expect("error" in t).toBe(false)
    expect((t as { baseUrl: string }).baseUrl).toBe("http://192.168.1.20:8080")
  })
})
