/**
 * 子代理同名定义跨语言合并（纯函数）：TS 侧与 客卿（多语言）侧可各自贡献同名子代理的
 * 一部分——description/systemPrompt 非空项依次拼接、工具集合并（同名工具确定性保留前者并
 * 告警）、dependencies/envVars 并集、preload 取或、projectRoot/writeGuard 取首个非空。
 * 约定「只在一处定义、其他地方留空」：某侧 description 省略/留空、systemPrompt 文件缺失
 * 即表示该侧不贡献此字段（客卿 manifest 放宽为此设计服务），合并层兜底生成描述。
 */
import type { EnvCatalogVar, SubAgentDef, ToolSet } from "../base/types"

/** 描述兜底：全部贡献的 description 均空时生成（保持 agent_list/系统提示词注入恒有可读描述）。 */
export function fallbackDescription(name: string): string {
  return `子代理 ${name}`
}

/** 文本拼接：跳过空白项，连接符拼接；全空返回空串。 */
function joinText(parts: string[], sep: string): string {
  const v = parts.map((p) => p.trim()).filter(Boolean)
  return v.join(sep)
}

/**
 * 合并同名子代理定义（贡献集至少 1 项；顺序即确定性优先级——TS 贡献在前、客卿 在后，
 * 同名工具/函数字段冲突时前者胜出并 console.warn）。
 */
export function mergeSubAgentDefs(name: string, defs: SubAgentDef[]): SubAgentDef {
  if (!defs.length) throw new Error(`mergeSubAgentDefs: 贡献集为空（${name}）`)
  for (const d of defs) {
    if (d.name !== name) throw new Error(`mergeSubAgentDefs: 贡献项 name 不一致（期望 ${name}，实际 ${d.name}）`)
  }
  const description = joinText(defs.map((d) => d.description), "；") || fallbackDescription(name)
  const systemPrompt = joinText(defs.map((d) => d.systemPrompt), "\n\n")
  const merged: SubAgentDef = {
    name,
    description,
    systemPrompt: systemPrompt || `你是子代理 ${name}。${description}`,
  }
  // 工具集合并：同名工具确定性保留靠前者（TS 优先）并告警（静默丢弃会掩盖贡献意图）
  let tools: ToolSet | undefined
  for (const d of defs) {
    if (!d.tools) continue
    if (!tools) tools = { ...d.tools }
    else {
      for (const [toolName, tool] of Object.entries(d.tools)) {
        if (tools[toolName]) console.warn(`[subagents] 子代理 ${name} 的工具 ${toolName} 多处定义，保留靠前贡献（后者丢弃）`)
        else tools[toolName] = tool
      }
    }
  }
  if (tools) merged.tools = tools
  const deps = [...new Set(defs.flatMap((d) => d.dependencies ?? []))]
  if (deps.length) merged.dependencies = deps
  // envVars 同名取首个（与工具冲突同优先级规则），不同名并集
  const envVars = new Map<string, EnvCatalogVar>()
  for (const v of defs.flatMap((d) => d.envVars ?? [])) if (!envVars.has(v.name)) envVars.set(v.name, v)
  if (envVars.size) merged.envVars = [...envVars.values()]
  const ra = defs.map((d) => d.requiresApproval).filter((r): r is Record<string, boolean> => !!r)
  if (ra.length) merged.requiresApproval = Object.assign({}, ...ra)
  if (defs.some((d) => d.preload)) merged.preload = true
  const projectRoot = defs.map((d) => d.projectRoot).find((f): f is NonNullable<SubAgentDef["projectRoot"]> => !!f)
  if (projectRoot) merged.projectRoot = projectRoot
  const writeGuard = defs.map((d) => d.writeGuard).find((f): f is NonNullable<SubAgentDef["writeGuard"]> => !!f)
  if (writeGuard) merged.writeGuard = writeGuard
  return merged
}
