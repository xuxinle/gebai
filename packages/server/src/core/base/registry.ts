import type { Tool, ToolSet } from "./types"
import { isRiskyToolName } from "../security/safety"

export interface RegisteredTool {
  name: string
  tool: Tool
  agent?: string
  enabled: boolean
}

export interface ToolRegistryOptions {
  /** 安全模式（GEBAI_SAFE_MODE）：子Agent 工具按 Tool.safeMode 自主声明过滤——true 强制可提供 /
   *  false 强制不提供 / 未声明按短名风险规则默认（isRiskyToolName）。全局工具不过滤（风险工具内置降级）。 */
  safeMode?: boolean
}

/** 启动级启停策略（`GEBAI_TOOL_ENABLE`/`GEBAI_TOOL_DISABLE` 与领域档案）：白名单非空仅启用名单内、
 *  黑名单移除名单内（先白后黑）；两项均支持 `{agent}_*` 通配（整包启停某子Agent）。 */
export interface ToolPolicy {
  enable?: string[]
  disable?: string[]
}

function normalize(name: string): string {
  return name.replace(/[-.:]/g, "_")
}

export class ToolRegistry {
  private tools = new Map<string, RegisteredTool>()
  private agents = new Map<string, { tools: string[] }>()
  private safeMode: boolean
  /** 启动级启停策略：**注册时判定**（而非注册后一次性快照）——后注册的工具（子Agent 装载/热加载/
   *  custom 与客卿后到的定义）同样受约束，能力面不因注册时序回弹。 */
  private policy: Required<ToolPolicy> = { enable: [], disable: [] }

  constructor(opts: ToolRegistryOptions = {}) {
    this.safeMode = !!opts.safeMode
  }

  register(tool: Tool, agent?: string): void {
    const name = agent ? `${agent}_${tool.name}` : tool.name
    const key = normalize(name)
    if (this.tools.has(key)) throw new Error(`duplicate tool name: ${key}`)
    // 安全模式子Agent 工具过滤（DESIGN「安全模式」）：自主声明覆盖默认短名风险规则，不注册即不可见不可调
    if (agent && this.safeMode) {
      const allowed = tool.safeMode === undefined ? !isRiskyToolName(key) : tool.safeMode === true
      if (!allowed) return
    }
    if (agent) {
      // 注册期碰撞检查（DESIGN「命名约束」）：子Agent 名不得互为前缀，
      // 否则 `{agent}_` 前缀解析产生歧义；全局工具名不得以任何 `{agent}_` 开头
      for (const existing of this.agents.keys()) {
        if (existing === agent) continue
        if (existing.startsWith(`${agent}_`) || agent.startsWith(`${existing}_`)) {
          throw new Error(`sub-agent name prefix collision: "${existing}" vs "${agent}"`)
        }
      }
      const agentPrefix = `${agent}_`
      for (const rt of this.tools.values()) {
        if (!rt.agent && rt.name.startsWith(agentPrefix)) {
          throw new Error(`global tool name collides with sub-agent namespace: ${rt.name}`)
        }
      }
    } else {
      for (const a of this.agents.keys()) {
        if (key.startsWith(`${a}_`)) {
          throw new Error(`global tool name collides with sub-agent namespace: ${key}`)
        }
      }
    }
    this.tools.set(key, { name: key, tool, agent, enabled: this.policyAllows(key, agent) })
    if (agent) {
      const a = this.agents.get(agent) || { tools: [] }
      a.tools.push(key)
      this.agents.set(agent, a)
    }
  }

  registerSubAgentTools(name: string, tools: ToolSet, requiresApproval?: Record<string, boolean>): void {
    for (const [toolName, tool] of Object.entries(tools)) {
      const prefixed: Tool = {
        ...tool,
        requiresApproval: requiresApproval?.[toolName] ?? tool.requiresApproval,
      }
      this.register(prefixed, name)
    }
  }

  unregisterAgent(agent: string): void {
    const a = this.agents.get(agent)
    if (!a) return
    for (const key of a.tools) this.tools.delete(key)
    this.agents.delete(agent)
  }

  getAgentNames(): string[] {
    return [...this.agents.keys()]
  }

  setEnabled(name: string, enabled: boolean): void {
    const key = normalize(name)
    const rt = this.tools.get(key)
    if (rt) rt.enabled = enabled
  }

  /**
   * 设置启动级启停策略（启动装配期调用一次）：立即作用于已注册工具，并作为后续注册的判定基线——
   * 于是子Agent 在运行期装载/热加载进来的 `{agent}_*` 工具也遵守同一份白/黑名单。
   * 与运行时开关（`setEnabled`/REST `PATCH /api/v1/tools`）叠加：后者改的是当前注册项，不受策略阻止。
   */
  setPolicy(policy: ToolPolicy): void {
    this.policy = { enable: [...(policy.enable ?? [])], disable: [...(policy.disable ?? [])] }
    for (const rt of this.tools.values()) rt.enabled = this.policyAllows(rt.name, rt.agent)
  }

  /** 策略判定（先白后黑；`{agent}_*` 通配按子Agent 命名空间整包匹配）。 */
  private policyAllows(name: string, agent?: string): boolean {
    const hit = (pattern: string): boolean => {
      const p = normalize(pattern)
      if (p.endsWith("*")) return agent === p.slice(0, -1).replace(/_$/, "")
      return p === name
    }
    if (this.policy.enable.length && !this.policy.enable.some(hit)) return false
    return !this.policy.disable.some(hit)
  }

  /** 一次性声明白/黑名单（`setPolicy` 的同义入口，保留既有调用方语义）。 */
  enableSet(enable?: string[], disable?: string[]): void {
    this.setPolicy({ enable, disable })
  }

  resolve(input: string): RegisteredTool | undefined {
    const name = normalize(input)
    const exact = this.tools.get(name)
    if (exact && exact.enabled) return exact
    const agents = [...this.agents.keys()].sort((a, b) => b.length - a.length)
    for (const agent of agents) {
      if (name.startsWith(`${agent}_`)) {
        const rt = this.tools.get(name)
        if (rt && rt.enabled && rt.agent === agent) return rt
      }
    }
    return undefined
  }

  list(enabledOnly = true): RegisteredTool[] {
    const out: RegisteredTool[] = []
    for (const rt of this.tools.values()) {
      if (enabledOnly && !rt.enabled) continue
      out.push(rt)
    }
    return out.sort((a, b) => a.name.localeCompare(b.name))
  }

  schemas(enabledOnly = true): Array<{ name: string; description: string; parameters: Record<string, unknown> }> {
    return this.list(enabledOnly).map((rt) => ({
      name: rt.name,
      description: rt.tool.description,
      parameters: rt.tool.parameters as unknown as Record<string, unknown>,
    }))
  }
}
