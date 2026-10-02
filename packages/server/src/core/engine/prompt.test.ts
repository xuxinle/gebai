import { describe, expect, test } from "bun:test"
import { buildSystemPrompt, buildWorkspaceSection, isPromptSectionKey, PROMPT_SECTION_KEYS, PROMPT_SECTION_ORDER, PROMPT_STABLE_PREFIX_KEYS, promptSectionEnabled, type PromptDeps } from "./prompt"
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
const buildFor = (sid: string, user = "admin") => buildSystemPrompt(makeDeps(), sid, user, {})
const workdir = (sid: string, user = "admin") => buildWorkspaceSection(makeDeps(), sid, user)

/** 最长公共前缀长度（前缀缓存按逐字节匹配，此为「跨会话共享前缀」的直接度量）。 */
function commonPrefixLen(a: string, b: string): number {
  const n = Math.min(a.length, b.length)
  let i = 0
  while (i < n && a[i] === b[i]) i++
  return i
}

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
    // 会话工作目录不在主提示词内（独立 system 消息，见 PROMPT_SECTION_ORDER）
    expect(out).not.toContain("当前会话工作目录")
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
    // workspace 段由独立入口渲染（白名单命中也走该入口，不进主提示词）
    expect(workdir(SID)).toContain("当前会话工作目录:")
    expect(out).not.toContain("当前会话工作目录")
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

  test("工作目录段（buildWorkspaceSection）：路径与沙箱注记，与主提示词分离", () => {
    expect(workdir(SID)).toContain(`当前会话工作目录: ${sessionPath(HOME, "admin", SID)}/tmp`)
    expect(workdir(SID)).toContain("本地模式：不限制文件目录")
    // 沙箱启用（服务端部署）时注记切换
    const sandboxed = buildWorkspaceSection({ ...makeDeps(), sandbox: { enforcedFor: () => true } as unknown as PromptDeps["sandbox"] }, SID, "admin")
    expect(sandboxed).toContain("文件读写限定在此目录内")
    // 会话不同 → 工作目录段不同（这正是它不能并入主提示词的原因）
    expect(workdir(SID)).not.toBe(workdir("fedcba9876543210fedcba9876543210"))
  })
})

/** 前缀缓存稳定性契约（DESIGN「前缀缓存稳定性」）：段落顺序按变更频率分层，主提示词的最长公共前缀
 *  是跨会话/跨用户共享的缓存区。这些断言把顺序锁死——任何人把易变段插到常量段之前，测试当场失败。 */
describe("前缀缓存稳定性（段落分层顺序）", () => {
  test("subagent_catalog 是最后一段（装载只截断末尾，不动其余段）", () => {
    const order = PROMPT_SECTION_ORDER.filter((k) => k !== "workspace")
    expect(order[order.length - 1]).toBe("subagent_catalog")
  })

  test("稳定前缀段（启动常量 + per-user）不含任务级/会话级段落", () => {
    expect(PROMPT_STABLE_PREFIX_KEYS).toContain("persona")
    expect(PROMPT_STABLE_PREFIX_KEYS).toContain("orchestration")
    expect(PROMPT_STABLE_PREFIX_KEYS).toContain("builtin_projects")
    for (const k of ["channel", "project_bindings", "subagent_catalog", "workspace"]) {
      expect(PROMPT_STABLE_PREFIX_KEYS).not.toContain(k)
    }
  })

  test("不同会话的主提示词逐字节相同（共享前缀 = 全段）", () => {
    const a = buildFor(SID)
    const b = buildFor("fedcba9876543210fedcba9876543210")
    expect(commonPrefixLen(a, b)).toBe(Math.min(a.length, b.length))
    // 跨用户同样稳定（本地模式下 builtin_projects 是安装级路径，不含用户名）
    const c = buildFor("fedcba9876543210fedcba9876543210", "other")
    expect(a).toBe(c)
  })

  test("未装载清单变化只影响末尾（装载子Agent 时前缀不被重写）", () => {
    const before = buildSystemPrompt(makeDeps(), SID, "admin", {})
    // 模拟本会话装载 code：该子Agent 从「未装载」清单消失
    const after = buildSystemPrompt(
      { ...makeDeps(), subAgents: { list: () => [], systemPromptInjection: () => "" } as unknown as PromptDeps["subAgents"] },
      SID,
      "admin",
      {},
    )
    expect(before.startsWith(after)).toBe(true) // after 是 before 的前缀
  })

  test("任务级易变段（通道注记）位于稳定前缀之后", () => {
    const base = buildSystemPrompt(makeDeps(), SID, "admin", {})
    const withChannel = buildSystemPrompt({ ...makeDeps(), channelNote: () => "当前对话经飞书机器人通道进行。" }, SID, "admin", {})
    // 旧实现：workspace 紧跟 persona，共享前缀仅约 181 字符（几乎等于零）。
    // 断言阈值取稳定组规模的量级下限（提示词正文会随措辞调整而伸缩，不锁具体字数；
    // 缓存是否有效看的是「稳定组 + tools 段」联合长度，tools 段（约 8.5k token）也逐字节一致）：
    // 稳定组必须远大于 persona 段（~171 字符），否则说明有易变段被插到了前面。
    expect(commonPrefixLen(base, withChannel)).toBeGreaterThanOrEqual(900)
    expect(withChannel).toContain("当前对话经飞书机器人通道进行。")
  })
})
