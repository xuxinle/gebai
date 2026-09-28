/**
 * 镜像构建裁剪计划器测试：档案解析与结构校验、CLI 覆盖合并、环境文件与报告输出。
 * 计划器是镜像裁剪的唯一真相源，字段拼错/静默忽略都会让镜像能力面与预期不符，故逐字段覆盖。
 */
import { describe, expect, test } from "bun:test"
import { applyOverride, parseProfile, planToEnv, renderReport, type BuildPlan } from "./build-image-plan"

const ALL_ASSETS_ON = { web_ui: true, cv: true, d2: true, analyzer: true, browser: true, ripgrep: true } as const

/** 取 env 文件里某键的值（去掉 shell 引号）。 */
function envValue(env: string, key: string): string | undefined {
  const line = env.split("\n").find((l) => l.startsWith(`${key}=`))
  if (!line) return undefined
  return line.slice(key.length + 1).replace(/^"|"$/g, "")
}

describe("裁剪档案解析", () => {
  test("缺省全开：资产、vendor 全量、系统组默认开（chromium 默认关）", () => {
    const plan = parseProfile({ name: "full" }, "测试")
    expect(plan.name).toBe("full")
    expect(plan.assets).toEqual(ALL_ASSETS_ON)
    expect(plan.vendor).toEqual(["monaco", "plantuml", "mermaid", "echarts", "d2js", "xterm", "tree_sitter"])
    expect(plan.system).toEqual({ git: true, python: true, bubblewrap: true, fonts: true, tzdata: true, procps: true, chromium: false })
    expect(plan.subAgents).toEqual({})
    expect(plan.tools).toEqual({})
  })

  test("档案逐段覆盖：子Agent / 工具 / 资产 / vendor / 系统", () => {
    const plan = parseProfile(
      {
        name: "code",
        description: "编码场景",
        sub_agents: { enable: ["code", "explore"], preload: ["code"] },
        tools: { disable: ["show"] },
        assets: { cv: false, d2: false },
        web: { vendor: ["monaco", "xterm"] },
        system: { python: false, chromium: true },
        prompt: { disable: ["batching"] },
      },
      "测试",
    )
    expect(plan.description).toBe("编码场景")
    expect(plan.subAgents).toEqual({ enable: ["code", "explore"], disable: undefined, preload: ["code"] })
    expect(plan.tools).toEqual({ disable: ["show"] })
    expect(plan.assets.cv).toBe(false)
    expect(plan.assets.d2).toBe(false)
    expect(plan.assets.web_ui).toBe(true)
    expect(plan.vendor).toEqual(["monaco", "xterm"])
    expect(plan.system.python).toBe(false)
    expect(plan.system.chromium).toBe(true)
  })

  test("未知字段 / 类型不符 / 未知组 / 白名单黑名单并存：一律报错（不静默降级）", () => {
    expect(() => parseProfile({ name: "x", unknown_section: {} }, "测试")).toThrow(/未知字段/)
    expect(() => parseProfile({ description: "无名字" }, "测试")).toThrow(/缺少 name/)
    expect(() => parseProfile({ name: "x", assets: { d2: "no" } }, "测试")).toThrow(/assets\.d2 必须是布尔值/)
    expect(() => parseProfile({ name: "x", assets: { chalk: false } }, "测试")).toThrow(/未知字段/)
    expect(() => parseProfile({ name: "x", web: { vendor: ["monaco", "chalk"] } }, "测试")).toThrow(/未知组/)
    expect(() => parseProfile({ name: "x", system: { gitt: true } }, "测试")).toThrow(/未知字段/)
    expect(() => parseProfile({ name: "x", sub_agents: { enable: ["code"], disable: ["wps"] } }, "测试")).toThrow(/互斥/)
    expect(() => parseProfile({ name: "x", sub_agents: 1 }, "测试")).toThrow(/必须是对象/)
  })

  test("tools.enable 属运行期语义：构建期忽略但不报错（同一份档案可两用）", () => {
    const plan = parseProfile({ name: "x", tools: { enable: ["read"], disable: ["show"] } }, "测试")
    expect(plan.tools.disable).toEqual(["show"])
  })
})

describe("CLI 覆盖（优先于档案）", () => {
  const plan = (): BuildPlan => parseProfile({ name: "x", assets: { d2: false }, web: { vendor: ["monaco"] } }, "测试")

  test("资产 / vendor / 系统组字段级覆盖", () => {
    const p = plan()
    applyOverride(p, "assets.d2=1")
    applyOverride(p, "web.vendor=monaco,mermaid")
    applyOverride(p, "system.python=off")
    expect(p.assets.d2).toBe(true)
    expect(p.vendor).toEqual(["monaco", "mermaid"])
    expect(p.system.python).toBe(false)
  })

  test("子Agent 名单覆盖：设置 enable 会清掉 disable（互斥在覆盖后仍成立）", () => {
    const p = parseProfile({ name: "x", sub_agents: { disable: ["wps"] } }, "测试")
    applyOverride(p, "sub_agents.enable=code")
    expect(p.subAgents.enable).toEqual(["code"])
    expect(p.subAgents.disable).toBeUndefined()
  })

  test("未知键 / 未知资产 / 非法布尔一律报错", () => {
    const p = plan()
    expect(() => applyOverride(p, "assets.chalk=0")).toThrow(/未知资产/)
    expect(() => applyOverride(p, "unknown=1")).toThrow(/未知键/)
    expect(() => applyOverride(p, "assets.d2=maybe")).toThrow(/需要布尔值/)
    expect(() => applyOverride(p, "assets.d2")).toThrow(/键=值/)
  })
})

describe("构建计划输出", () => {
  test("env：资产开关、清单、系统包按裁剪结果落成 shell 变量", () => {
    const plan = parseProfile(
      {
        name: "minimal",
        sub_agents: { enable: ["task"] },
        tools: { disable: ["show", "fetch_url"] },
        assets: { web_ui: false, cv: false, d2: false, analyzer: false, browser: false, ripgrep: false },
        web: { vendor: [] },
        system: { git: false, python: false, bubblewrap: false, fonts: false, procps: false, tzdata: true },
      },
      "测试",
    )
    const env = planToEnv(plan, "docker/profiles/minimal.json")
    expect(env).toContain("GEBAI_BUILD_WEB_UI=0")
    expect(env).toContain("GEBAI_BUILD_CV=0")
    expect(env).toContain("GEBAI_BUILD_RG=0")
    expect(env).toContain("GEBAI_BUILD_SUBAGENTS=task")
    expect(env).toContain("GEBAI_BUILD_EXCLUDE_SUBAGENTS=")
    expect(env).toContain("GEBAI_BUILD_EXCLUDE_TOOLS=show,fetch_url")
    expect(env).toContain("GEBAI_WEB_VENDOR=")
    expect(envValue(env, "PLAN_SYSTEM_PACKAGES")).toBe("ca-certificates curl tini tzdata")
    expect(envValue(env, "PLAN_WITH_BROWSER")).toBe("0")
  })

  test("env：系统组全开 + chromium 时列出全部包并置位浏览器开关", () => {
    const plan = parseProfile({ name: "full", system: { chromium: true } }, "测试")
    const env = planToEnv(plan, "")
    expect(envValue(env, "PLAN_SYSTEM_PACKAGES")).toBe(
      "ca-certificates curl tini git python3 python3-venv python3-pip bubblewrap fonts-noto-cjk fontconfig tzdata procps",
    )
    expect(envValue(env, "PLAN_WITH_BROWSER")).toBe("1")
    expect(env).toContain("GEBAI_BUILD_WEB_UI=1")
    expect(envValue(env, "GEBAI_WEB_VENDOR")).toBe("monaco,plantuml,mermaid,echarts,d2js,xterm,tree_sitter")
  })

  test("报告：如实列出被裁掉的能力面", () => {
    const plan = parseProfile({ name: "minimal", description: "最小面", assets: { web_ui: false }, web: { vendor: [] } }, "测试")
    const report = renderReport(plan, "docker/profiles/minimal.json")
    expect(report).toContain("镜像构建裁剪计划：minimal（最小面）")
    expect(report).toContain("web_ui=off")
    expect(report).toContain("无（跳过全部前端引擎）")
    expect(report).toContain("chromium: off")
  })
})
