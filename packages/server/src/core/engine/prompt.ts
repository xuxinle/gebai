/** 系统提示词构建（自 engine.ts 拆分）：总Agent 主提示词（buildSystemPrompt）与子Agent 提示词段落
 *  （buildAgentSection）、动态描述（agentDescription）与项目注记。engine 注入 PromptDeps（项目解析、
 *  AGENTS.md 读取等引擎方法）委托调用。 */
import type { PresetProject, SubAgentDef } from "../base/types"
import { SELF_PROJECT_NAME } from "../tools/projects"
import type { ServerConfig } from "../base/config"
import type { Sandbox } from "../security/sandbox"
import type { SubAgentManager } from "../agents/subagents"
import { sessionPath } from "../base/paths"

export interface PromptDeps {
  config: ServerConfig
  sandbox: Sandbox
  subAgents: SubAgentManager
  /** 通道环境注记（任务态读取）。 */
  channelNote: (sessionId: string) => string | undefined
  /** 子Agent 项目根解析（{AGENT}_PROJECT env / def.projectRoot 兜底）。 */
  resolveSubAgentProject: (user: string, env: Record<string, string>, name: string) => string | undefined
  /** 子Agent 预置项目清单（{AGENT}_PROJECTS env 解析）。 */
  presetProjectsFor: (user: string, env: Record<string, string>, agentName: string) => PresetProject[]
  /** 读取项目根 AGENTS.md（无项目根/文件缺失返回空串）。 */
  loadProjectAgentsMd: (projectRoot: string | undefined) => Promise<string>
  /** 会话工作目录段（独立 system 消息）：buildSystemPrompt 不再包含它（见 PROMPT_SECTION_ORDER 注释）；
   *  由 loadHistory 置于主提示词之后、装载提示词之前（与 SystemPrompt 同拼接顺序 = 逐字节同前缀）。 */
  workspaceSection?: (sessionId: string, user: string) => string
}

/** 全局提示词段落键（启动期裁剪口径，`GEBAI_PROMPT_ENABLE`/`GEBAI_PROMPT_DISABLE` 与领域档案 `prompt.*` 取值）：
 *  persona=身份与智体概念模型、workspace=会话工作目录与沙箱注记、channel=通道环境注记、safe_mode=安全模式注记、
 *  orchestration=脚本编排指引、batching=并行工具调用指引、planning=重大任务计划审批、artifact_naming=产物命名纪律、
 *  agent_routing=子Agent 用法路由、parallel_sessions=并行多路推进、project_bindings=子Agent 项目绑定、
 *  builtin_projects=内置项目、subagent_catalog=可选子Agent 清单。 */
export const PROMPT_SECTION_KEYS = [
  "persona",
  "workspace",
  "channel",
  "safe_mode",
  "orchestration",
  "batching",
  "planning",
  "artifact_naming",
  "agent_routing",
  "parallel_sessions",
  "project_bindings",
  "builtin_projects",
  "subagent_catalog",
] as const

export type PromptSectionKey = (typeof PROMPT_SECTION_KEYS)[number]

/** 段落渲染顺序（**前缀缓存契约**，见 DESIGN「前缀缓存稳定性」）：按变更频率分层，使所有会话的最长公共
 *  前缀最大化——服务端前缀缓存（OpenAI 自动前缀缓存 / DeepSeek 上下文缓存 / Anthropic 等同机制）按请求
 *  前缀逐字节匹配，任一字节变化即从该点起全部失配，故「最不常变的内容排最前」。
 *  分层：① 启动常量（进程生命周期内恒定，跨会话跨用户共享）；② per-user（歌白家目录路径，同用户多会话恒定）；
 *  ③ 任务级易变（通道注记、`{AGENT}_PROJECT` 项目绑定）；④ 会话级易变（子Agent 未装载清单——`agent_load`
 *  装载即改写）。
 *  **subagent_catalog 必须保持在最后**：它是唯一「同一会话内必然变化」的段落，排最后使装载只截断末尾；
 *  前移会让其后所有段落的正文一起失配，工具 schema 与整个历史前缀随之重算。
 *  workspace（会话工作目录）不并入主提示词——其正文含会话 ID、每会话必然唯一，由 workspaceSection 独立成
 *  紧随其后的第二条 system 消息，同用户多会话因此共享整段主提示词。 */
export const PROMPT_SECTION_ORDER: readonly PromptSectionKey[] = [
  // ① 启动常量
  "persona",
  "safe_mode",
  "orchestration",
  "batching",
  "planning",
  "artifact_naming",
  "agent_routing",
  "parallel_sessions",
  // ② per-user
  "builtin_projects",
  // ③ 任务级
  "channel",
  "project_bindings",
  // ④ 会话级（必须最后）
  "subagent_catalog",
  // 不并入主提示词（独立 system 消息）
  "workspace",
]

/** 跨会话稳定前缀段（PROMPT_SECTION_ORDER 的 ①② 层）：同一用户的所有会话逐字节相同。
 *  守卫测试据此断言最长公共前缀下限，防止易变段被插到常量段之前（领域补充提示词紧随其后，同属稳定前缀）。 */
export const PROMPT_STABLE_PREFIX_KEYS: readonly PromptSectionKey[] = PROMPT_SECTION_ORDER.slice(
  0,
  PROMPT_SECTION_ORDER.indexOf("channel"),
)

/** 段落键判定（`GEBAI_PROMPT_*` 与领域档案清单的未知名校验用）。 */
export function isPromptSectionKey(v: string): v is PromptSectionKey {
  return (PROMPT_SECTION_KEYS as readonly string[]).includes(v)
}

/** 段落裁剪判定（`GEBAI_PROMPT_ENABLE` 白名单 / `GEBAI_PROMPT_DISABLE` 黑名单，先白后黑）：启动级配置，
 *  对所有用户/会话生效（领域专用模式的能力面收敛，见 DESIGN「启动裁剪与领域专用模式」）。 */
export function promptSectionEnabled(config: Pick<ServerConfig, "promptEnable" | "promptDisable">, key: PromptSectionKey): boolean {
  const enable = config.promptEnable ?? []
  if (enable.length && !enable.includes(key)) return false
  return !(config.promptDisable ?? []).includes(key)
}

/** 全局提示词段落（键 + 正文）：正文为空串表示当前形态不适用（如未启用安全模式），渲染时按空行丢弃。 */
interface PromptSection {
  key: PromptSectionKey
  text: string
}

export function buildSystemPrompt(deps: PromptDeps, sessionId: string, user: string, env: Record<string, string>): string {
  const sections: PromptSection[] = [
    // 智能与智体概念模型（DESIGN「定位」）：行为化措辞（状态落盘、调用担责），非装饰性身份说明
    { key: "persona", text: `你是歌白智能体（GEBAI Agent）：目标是融合旧世界IT的所有技术，打造新世界智能的躯体——以极致动态扩展的能力把任意语言、任意进程的外部技术收编为工具。你是智体：智能（模型）负责思考、无状态、可替换，记忆与责任都长在智体——需跨轮次/跨会话保留的结论与状态写入文件或会话记录；你的每次工具调用都是智体的行为，经审批执行、留痕可审计` },
    // 安全模式注记（启动级常量）：紧接身份行——能力边界与身份同为「此次运行不变」的事实，且不含易变路径
    { key: "safe_mode", text: deps.config.safeMode
      ? `安全模式已启用（风险能力降级而非禁用）：sh 仅允许只读命令白名单（cat/grep/find/git 读类等，输出重定向限定用户目录）；py/js 为只读运行时（写文件/子进程/网络屏蔽，仅保留文件读取）；write/edit/patch/file 限定用户目录内；任务调度（task_*）不可用。`
      : "" },
    { key: "orchestration", text: `复杂/多步操作优先用 js 脚本编排一次执行，避免大量单步工具调用浪费往返与词元（脚本内工具像内置函数一样直接 await 调用；编排前可用 tool_schemas 查询工具输出结构）；纯系统操作用 sh/py 脚本。` },
    { key: "batching", text: `同一次回复返回的多个工具调用会并行执行（互不等待）：互不依赖的操作放进同批调用可显著加速；有先后依赖/需严格串行的操作不要同批发出——用 js 脚本按序编排或拆分多轮；对同一文件的写改尤其必须串行（并行修改会相互覆盖）。` },
    { key: "planning", text: `重大任务（多步骤/有风险/不可逆/用户需要把关）先用 ask 的计划审批分支（title+steps）制定计划并等待用户批准后再执行（被拒绝则按修改意见修订重新提交）；简单任务无需计划审批，直接用 todo 跟踪即可。` },
    // 产物命名纪律（结构化兵底在 show/reel：内容寻址与 -vN 唯一化；此处让模型主动起可区分的名）
    { key: "artifact_naming", text: `产物命名：同一用途的每次产出起**可区分的新名**（带目的或版本，如 qa/s2-frame190.png、promo-v3-final.mp4），不要在同一个名字上反复重写——对话里的产物是按**路径引用**的，同名覆盖会让历史消息里的产物变成新内容（历史不可回看）。` },
    { key: "agent_routing", text: `任务类型路由（子Agent 两种用法语义不同：默认 agent_load 装载——其工具并入当前工具集、全程在当前上下文完成，不创建独立执行；仅当需要干净上下文（结果隔离）、防止上下文膨胀（中间过程多/输出大）或长任务并行时，才用 subsession_run 派生子会话。按下方「可选子Agent」清单选用——描述即触发场景；纯文本问答（无需工具）时直接回答，不装载子Agent。）` },
    { key: "parallel_sessions", text: `同一任务的并行多路推进（多方案对比、多文件并行修改、多角度调研等互不依赖的线）用 subsession_run 的 inherit_context:true（fork 父会话上下文）——可用 subsessions 数组一次派生多个（每个可单独传 model 走不同模型接口并行更快），报告完成即自动合入父会话；长耗子会话传 async:true 后台执行，可用 subsession_merge 随时合入阶段性成果。` },
    // 内置项目（歌白自身）：与文件工作台的项目列表同源——工作台能看到的项目，模型可按名寻址
    { key: "builtin_projects", text: builtinProjects(deps, user)
      .map((p) => `内置项目「${p.name}」：${p.path}（project 参数可按名寻址）`)
      .join("") },
    { key: "channel", text: deps.channelNote(sessionId) ?? "" },
    // 项目绑定声明：装载模式下总Agent 直接使用子Agent 工具时按名操作绑定项目；
    // 未装载清单描述动态体现预置项目（方便总Agent 按项目名关联任务，完整清单注记仍只注入子Agent 提示词）
    { key: "project_bindings", text: subAgentProjectNote(deps, user, env) },
    // 会话级过滤（DESIGN「装载工具会话可见性」）：目录按「对本会话可见」判定未装载——其他会话装载过
    // 不代表本会话已装载（防跨会话泄漏：A 装载后 B 的目录仍应列出该子Agent 供 B 装载）
    { key: "subagent_catalog", text: deps.subAgents.systemPromptInjection((d) => agentDescription(deps, { name: d.name, description: d.description, tools: Object.keys(d.tools ?? {}) }, user, env), sessionId) },
  ]
  const byKey = new Map(sections.map((s) => [s.key, s]))
  const text = (key: PromptSectionKey): string => {
    const s = byKey.get(key)
    return s && promptSectionEnabled(deps.config, key) ? s.text : ""
  }
  // 稳定组（跨会话共享前缀）→ 领域补充提示词（启动级配置，不含会话变量）→ 易变组（任务级/会话级）
  const volatileKeys = PROMPT_SECTION_ORDER.filter((key) => key !== "workspace" && !PROMPT_STABLE_PREFIX_KEYS.includes(key))
  const extra = deps.config.promptExtra?.trim() ?? ""
  return [...PROMPT_STABLE_PREFIX_KEYS.map(text), extra, ...volatileKeys.map(text)].filter(Boolean).join("\n")
}

/** 会话工作目录段落（独立 system 消息，紧跟在主提示词之后）：正文含会话 ID、每会话必然唯一，
 *  并入主提示词会让所有会话在第 200 字节内分叉。独立成条使主提示词对同一用户的所有会话逐字节相同。
 *  与历史消息同拼接顺序（紧随主提示词），故压缩的「缓存友好前缀请求」逐字节同前缀不变。 */
export function buildWorkspaceSection(deps: PromptDeps, sessionId: string, user: string): string {
  const workdir = sessionPath(deps.config.gebaiHome, user, sessionId)
  const sandboxNote = deps.sandbox.enforcedFor(user)
    ? `（文件读写限定在此目录内，禁止越界）`
    : `（本地模式：不限制文件目录，可访问本机任意路径）`
  return `当前会话工作目录: ${workdir}/tmp（所有文件工具的相对路径以此为基准，tmp/ 前缀可省略；操作项目文件用文件工具的 project 参数——项目名或项目根路径，路径即相对所选项目根解析）${sandboxNote}`
}

/** 项目绑定注入总Agent 系统提示词（{AGENT_NAME_UPPER}_PROJECT 环境变量，DESIGN「项目内置」；
 *  SubAgentDef.projectRoot 兜底（环境变量未配置时的默认项目根，如 self_optimize 脚本调试模式自动
 *  推导歌白仓库根）同规则注入）；预置项目说明与受限模式说明（{AGENT_NAME_UPPER}_PROJECTS /
 *  CODE_RESTRICT_PROJECTS）属 code 子Agent 行为约束，只注入子Agent 系统提示词（subsession_run 隔离子会话时），
 *  不注入总Agent 系统提示词。 */
export function subAgentProjectNote(deps: PromptDeps, user: string, env: Record<string, string>): string {
  const lines: string[] = []
  for (const d of deps.subAgents.list()) {
    const root = deps.resolveSubAgentProject(user, env, d.name)
    if (!root) continue
    // 仅声明绑定与根路径（subsession_run 隔离子会话加载该子Agent 时以其为项目根；装载模式下路径基准仍是会话目录，
    // 访问项目请用预置项目 project 参数或绝对路径，不宣称工作目录已切换）
    lines.push(`${d.name} 子Agent 项目绑定：${root}（subsession_run 隔离子会话加载该子Agent 时以其为项目根；装载模式下路径基准为会话目录，访问项目用 project 参数或绝对路径）`)
  }
  return lines.length ? `\n\n${lines.join("\n")}` : ""
}

/** 内置项目：歌白自身（家目录——dev/源码形态下即歌白仓库根）与预置项目同清单（本地模式恒可寻址；
 *  服务模式与 `{AGENT}_PROJECTS` 同规则不生效）。用户配置同名项目时以其配置为准。 */
export function builtinProjects(deps: PromptDeps, user: string): PresetProject[] {
  if (deps.sandbox.enforcedFor(user)) return []
  return [{ name: SELF_PROJECT_NAME, path: deps.config.gebaiHome, description: "歌白自身（家目录：源码、用户数据与资源）" }]
}

/** 并入内置项目（同名已被占用时保持原清单——配置优先）。 */
export function withBuiltinProjects(deps: PromptDeps, user: string, list: PresetProject[]): PresetProject[] {
  const builtin = builtinProjects(deps, user)
  if (!builtin.length || list.some((p) => p.name === SELF_PROJECT_NAME)) return list
  return [...list, ...builtin]
}

/** 汇总所有已注册子Agent 的预置项目注册表（{AGENT_NAME_UPPER}_PROJECTS）：装载模式下总Agent 直接使用子Agent 工具时 project 参数路由用；同名去重（首个生效）；末尾附内置项目。 */
export function allPresetProjects(deps: PromptDeps, user: string, env: Record<string, string>): PresetProject[] {
  const out: PresetProject[] = []
  const seen = new Set<string>()
  for (const d of deps.subAgents.list()) {
    for (const p of deps.presetProjectsFor(user, env, d.name)) {
      if (seen.has(p.name)) continue
      seen.add(p.name)
      out.push(p)
    }
  }
  return withBuiltinProjects(deps, user, out)
}

/** 预置项目清单注记（子Agent 提示词开头动态追加：名称/说明/路径，供模型按名使用 project 参数）。 */
export function buildPresetNote(_agentName: string, projectRoot: string | undefined, presetProjects: PresetProject[]): string {
  if (!presetProjects.length) return ""
  return `\n预置项目（全局文件工具用 project 参数指定项目名，路径参数相对所选项目根解析；未传 project 时相对路径以${projectRoot ? "项目根" : "会话工作目录"}为基准）:\n${presetProjects
    .map((p) => `- ${p.name}${p.description ? `: ${p.description}` : ""}（${p.path}）`)
    .join("\n")}`
}

/** 子Agent 对外描述（动态）：静态 description + 预置项目摘要（{AGENT}_PROJECTS 名称: 说明（路径））+
 *  装载后工具摘要（短名，超出 10 个截断）——未装载清单（总Agent 提示词）与 agent_list 展示用，
 *  模型在装载前即可按项目名/工具能力关联任务与代码位置（路由匹配面）。
 *  工具调用前缀与「通用工具仍用全局名」的说明由清单表头统一承载（systemPromptInjection），
 *  逐条重复会让清单体积随子Agent 数量线性膨胀。 */
export function agentDescription(deps: PromptDeps, d: { name: string; description: string; tools?: string[] }, user: string, env: Record<string, string>): string {
  const projects = deps.presetProjectsFor(user, env, d.name)
  const parts = [d.description]
  if (projects.length) parts.push(`预置项目：${projects.map((p) => `${p.name}${p.description ? `: ${p.description}` : ""}（${p.path}）`).join("、")}`)
  const tools = d.tools ?? []
  if (tools.length) {
    // 只列前 3 个工具名 + 总数：能力面已由静态 description 承载，摘要是路由补充信号（全列会让清单体积
    // 随子Agent 数量线性膨胀；完整清单装载后即得，或经 agent_list 查）
    parts.push(`工具 ${tools.length} 个`)
  }
  return parts.join(" ")
}

/** 子Agent 提示词段落（职责分隔头 + 项目注记 + 静态提示词 + 项目 AGENTS.md）：runSubSession 预加载拼接
 *  与运行中装载（路由自愈）共用。动态环境注记（项目根/预置项目清单/受限模式）置于职责分隔头之后、
 *  静态提示词之前——配置信息前置，模型开工先读环境（目标项目与 project 参数取值），再读工作流。 */
export async function buildAgentSection(deps: PromptDeps, def: SubAgentDef, user: string, env: Record<string, string>, sessionId: string): Promise<string> {
  // 项目内置（特定项目绑定）：会话环境变量 {AGENT_NAME_UPPER}_PROJECT（如 CODE_PROJECT）指定子Agent 的项目根
  const projectRoot = deps.resolveSubAgentProject(user, env, def.name)
  const presetProjects = withBuiltinProjects(deps, user, deps.presetProjectsFor(user, env, def.name))
  const workNote = projectRoot ? `\n项目根: ${projectRoot}` : `\n工作目录: ${sessionPath(deps.config.gebaiHome, user, sessionId)}/tmp`
  const presetNote = buildPresetNote(def.name, projectRoot, presetProjects)
  const restrictNote = env.CODE_RESTRICT_PROJECTS === "true"
    ? `\n受限模式（CODE_RESTRICT_PROJECTS=true）：仅允许操作预配置项目（${def.name} 的 ${def.name.toUpperCase()}_PROJECTS 清单，或 ${def.name.toUpperCase()}_PROJECT 绑定根），文件工具必须携带 project 参数，自由路径（path）不可用。`
    : ""
  return `### ${def.name}（${def.description}）\n${workNote}${presetNote}${restrictNote}${def.systemPrompt}${await deps.loadProjectAgentsMd(projectRoot)}`
}
