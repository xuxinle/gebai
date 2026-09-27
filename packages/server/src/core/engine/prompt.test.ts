import { describe, expect, test } from "bun:test"
import { buildSystemPrompt, isPromptSectionKey, PROMPT_SECTION_KEYS, promptSectionEnabled, type PromptDeps } from "./prompt"
import type { ServerConfig } from "../base/config"
import { sessionPath } from "../base/paths"

/** 会话 id 必须是 32 位十六进制（sessionPath 校验）。 */
const SID = "0123456789abcdef0123456789abcdef"
const HOME = "/tmp/gebai-home"

/** 最小 PromptDeps 桩：全局提示词构建只读 config/sandbox/subAgents/channelNote。 */
function makeDeps(overrides: Partial<ServerConfig> = {}): PromptDeps {
  return {
    config: { gebaiHome: HOME, safeMode: false, ...overrides } as ServerConfig,
    sandbox: { enforcedFor: () => false } as unknown as PromptDeps["sandbox"],
    subAgents: { list: () => [], systemPromptInjection: () => "\n\n可选子Agent（未装载）:\n- code: 代码" } as unknown as PromptDeps["subAgents"],
    channelNote: () => undefined,
    resolveSubAgentProject: () => undefined,
    presetProjectsFor: () => [],
    loadProjectAgentsMd: async () => "",
  }
}

const build = (overrides: Partial<ServerConfig> = {}, env: Record<string, string> = {}) => buildSystemPrompt(makeDeps(overrides), SID, "admin", env)

describe("全局提示词段落键（启动裁剪口径）", () => {
  test("段落键判定与白/黑名单（先白后黑）", () => {
    expect(PROMPT_SECTION_KEYS).toContain("persona")
    expect(isPromptSectionKey("persona")).toBe(true)
    expect(isPromptSectionKey("persona_x")).toBe(false)
    expect(promptSectionEnabled({}, "persona")).toBe(true)
    expect(promptSectionEnabled({ promptDisable: ["persona"] }, "persona")).toBe(false)
    expect(promptSectionEnabled({ promptEnable: ["workspace"] }, "persona")).toBe(false)
    // 先白后黑：白名单含、黑名单也含 → 移除
    expect(promptSectionEnabled({ promptEnable: ["persona", "workspace"], promptDisable: ["persona"] }, "persona")).toBe(false)
  })
})

describe("buildSystemPrompt 段落裁剪与领域补充", () => {
  test("默认注入全段落（未配置裁剪时行为不变）", () => {
    const out = build()
    expect(out).toContain("你是歌白智能体（GEBAI Agent）")
    expect(out).toContain(`当前会话工作目录: ${sessionPath(HOME, "admin", SID)}/tmp`)
    expect(out).toContain("复杂/多步操作优先用 js 脚本编排")
    expect(out).toContain("重大任务（多步骤/有风险/不可逆/用户需要把关）")
    expect(out).toContain("产物命名：同一用途的每次产出起")
    expect(out).toContain("可选子Agent（未装载）")
    // 未启用安全模式：该段落正文为空串，渲染时丢弃（不留空行）
    expect(out).not.toContain("安全模式已启用")
  })

  test("黑名单移除指定段落，其余保留", () => {
    const out = build({ promptDisable: ["orchestration", "batching", "artifact_naming", "subagent_catalog"] })
    expect(out).toContain("你是歌白智能体（GEBAI Agent）")
    expect(out).toContain("重大任务（多步骤/有风险/不可逆/用户需要把关）")
    expect(out).not.toContain("复杂/多步操作优先用 js 脚本编排")
    expect(out).not.toContain("同一次回复返回的多个工具调用会并行执行")
    expect(out).not.toContain("产物命名：同一用途的每次产出起")
    expect(out).not.toContain("可选子Agent（未装载）")
  })

  test("白名单仅注入名单内段落（领域专用模式的极简提示词）", () => {
    const out = build({ promptEnable: ["persona", "workspace"] })
    expect(out).toContain("你是歌白智能体（GEBAI Agent）")
    expect(out).toContain("当前会话工作目录:")
    expect(out).not.toContain("任务类型路由")
    expect(out).not.toContain("可选子Agent（未装载）")
  })

  test("安全模式注记属独立段落（GEBAI_PROMPT_DISABLE 可裁，未启用时本就为空）", () => {
    expect(build({ safeMode: true })).toContain("安全模式已启用")
    expect(build({ safeMode: true, promptDisable: ["safe_mode"] })).not.toContain("安全模式已启用")
  })

  test("领域补充提示词（GEBAI_PROMPT_EXTRA）追加在裁剪后的提示词末尾", () => {
    const out = build({ promptEnable: ["persona"], promptExtra: "领域约束：只处理订单相关请求。" })
    expect(out.endsWith("领域约束：只处理订单相关请求。")).toBe(true)
    expect(out).not.toContain("任务类型路由")
  })
})
