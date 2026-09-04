/** 系统提示词构建（自 engine.ts 拆分）：总Agent 主提示词（buildSystemPrompt）与子Agent 提示词段落
 *  （buildAgentSection）、动态描述（agentDescription）与项目注记。engine 注入 PromptDeps（项目解析、
 *  AGENTS.md 读取等引擎方法）委托调用。 */
import type { PresetProject, SubAgentDef } from "../base/types"
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
}

export function buildSystemPrompt(deps: PromptDeps, sessionId: string, user: string, env: Record<string, string>): string {
  const workdir = sessionPath(deps.config.gebaiHome, user, sessionId)
  const channelNote = deps.channelNote(sessionId)
  const sandboxNote = deps.sandbox.enforcedFor(user)
    ? `（文件读写限定在此目录内，禁止越界）`
    : `（本地模式：不限制文件目录，可访问本机任意路径）`
  // 极简模式提示词极简化（DESIGN「极简模式」）：编排/任务路由/子Agent 清单等说明对应工具均已裁剪，
  // 注入纯属浪费上下文——仅保留身份、路径基准与极简工具说明（full_mode 切换完整模式后原地升级为完整提示词）
  if (env.GEBAI_MINIMAL_MODE === "true") {
    return [
      `你是歌白智能体（GEBAI Agent）：极致动态扩展能力的智能体`,
      `当前会话工作目录: ${workdir}/tmp（sh 命令与 edit 等文件工具的相对路径以此为基准，tmp/ 前缀可省略）${sandboxNote}`,
      `当前会话处于极简模式：仅启用 sh、edit 与 full_mode 三个工具（其余工具均不可用）。查看/读取文件请用 sh 执行命令（cat、ls、find 等），修改文件请用 edit 工具；若任务确需其他工具能力，调用 full_mode 工具（需用户批准）切换到完整模式，批准后全部工具与完整说明立即生效。`,
      ...(deps.config.safeMode ? [`安全模式已启用：sh 仅允许只读命令白名单、edit 限定用户目录内修改。`] : []),
      ...(channelNote ? [channelNote] : []),
    ].join("\n")
  }
  const safeModeNote = deps.config.safeMode
    ? `安全模式已启用（风险能力降级而非禁用）：sh 仅允许只读命令白名单（cat/grep/find/git 读类等，输出重定向限定用户目录）；py/js 为只读运行时（写文件/子进程/网络屏蔽，仅保留文件读取）；write/edit/patch/file 限定用户目录内；定时任务调度（cron_*）不可用。`
    : ""
  const parts = [
    // 智能与智体概念模型（DESIGN「定位」）：行为化措辞（状态落盘、调用担责），非装饰性身份说明；
    // 极简模式按提示词极简化原则不注入
    `你是歌白智能体（GEBAI Agent）：极致动态扩展能力的智能体。你是智体：智能（模型）负责思考、无状态、可替换，记忆与责任都长在智体——需跨轮次/跨会话保留的结论与状态写入文件或会话记录；你的每次工具调用都是智体的行为，经审批执行、留痕可审计`,
    `当前会话工作目录: ${workdir}/tmp（所有文件工具的相对路径以此为基准，tmp/ 前缀可省略；操作项目文件用文件工具的 project 参数——项目名或项目根路径，路径即相对所选项目根解析）${sandboxNote}`,
    ...(channelNote ? [channelNote] : []),
    ...(safeModeNote ? [safeModeNote] : []),
    `复杂/多步操作优先用 js 脚本编排一次执行，避免大量单步工具调用浪费往返与词元（脚本内工具像内置函数一样直接 await 调用、可用变量/分支/循环/错误处理表达任意流程，编排前可用 tool_schemas 查询工具输出结构，语法见 js 工具描述）；纯系统操作用 sh/py 脚本。`,
    `同一次回复返回的多个工具调用会并行执行（互不等待）：互不依赖的操作放进同批调用可显著加速（多文件读取/多路查询/独立子任务等尽量同批发出）；有先后依赖、需严格串行的操作不要同批发出——用 js 脚本按序编排（await 前一步结果再决定下一步），或拆分到多轮逐步执行；对同一文件的写/改尤其必须串行编排（并行修改会相互覆盖）。`,
    `重大任务（多步骤/有风险/不可逆/用户需要把关）先用 ask 的计划审批分支（title+steps）制定计划并等待用户批准后再执行（被拒绝则按修改意见修订重新提交）；简单任务无需计划审批，直接用 todo 跟踪即可。`,
    `任务类型路由（子Agent 两种用法语义不同：默认 agent_load 装载——其工具并入当前工具集，装载后直接调用、全程在当前上下文完成，不创建独立执行；仅当需要干净上下文（结果隔离、不污染主上下文）、防止上下文膨胀（中间过程多、输出大）或长任务并行时，才用 agent_run 执行新会话——派生临时新会话，预加载一个或多个子Agent（完整系统提示词与工具）后执行，只返回最终结果，长任务传 async:true 后台执行、bg_task 回头查进度/收结果/终止；拿不准时先判断任务类型再选。按任务类型从下方「可选子Agent」清单选用——每个子Agent 的描述即其触发场景，匹配任务类型即装载或执行新会话；纯文本问答（无需工具）时直接回答，不装载子Agent。）`,
    `同一任务的并行多路推进（多方案对比、多文件并行修改、多角度调研等多条互不依赖的线）用 branch_run 会话分支运行——从主上下文 fork 多分支同时执行（各分支掌握主线全部背景与工具，可各自传 model 走不同模型接口并行更快），分支最终报告自动合入主上下文；长耗时分支传 async:true 后台执行（bg_task 管理），可不断分支合并像 git 一样推进——并行多线是摆脱单轮串行等待、加速大体量任务的主要手段。`,
    // 项目绑定声明：装载模式下总Agent 直接使用子Agent 工具时按名操作绑定项目；
    // 未装载清单描述动态体现预置项目（方便总Agent 按项目名关联任务，完整清单注记仍只注入子Agent 提示词）
    subAgentProjectNote(deps, user, env),
    // 会话级过滤（DESIGN「装载工具会话可见性」）：目录按「对本会话可见」判定未装载——其他会话装载过
    // 不代表本会话已装载（防跨会话泄漏：A 装载后 B 的目录仍应列出该子Agent 供 B 装载）
    deps.subAgents.systemPromptInjection((d) => agentDescription(deps, { name: d.name, description: d.description, tools: Object.keys(d.tools ?? {}) }, user, env), sessionId),
  ]
  return parts.filter(Boolean).join("\n")
}

/** 项目绑定注入总Agent 系统提示词（{AGENT_NAME_UPPER}_PROJECT 环境变量，DESIGN「项目内置」；
 *  SubAgentDef.projectRoot 兜底（环境变量未配置时的默认项目根，如 self_optimize 脚本调试模式自动
 *  推导歌白仓库根）同规则注入）；预置项目说明与受限模式说明（{AGENT_NAME_UPPER}_PROJECTS /
 *  CODE_RESTRICT_PROJECTS）属 code 子Agent 行为约束，只注入子Agent 系统提示词（agent_run 执行新会话时），
 *  不注入总Agent 系统提示词。 */
export function subAgentProjectNote(deps: PromptDeps, user: string, env: Record<string, string>): string {
  const lines: string[] = []
  for (const d of deps.subAgents.list()) {
    const root = deps.resolveSubAgentProject(user, env, d.name)
    if (!root) continue
    // 仅声明绑定与根路径（agent_run 新会话执行该子Agent 时以其为项目根；装载模式下路径基准仍是会话目录，
    // 访问项目请用预置项目 project 参数或绝对路径，不宣称工作目录已切换）
    lines.push(`${d.name} 子Agent 项目绑定：${root}（agent_run 新会话执行该子Agent 时以其为项目根；装载模式下路径基准为会话目录，访问项目用 project 参数或绝对路径）`)
  }
  return lines.length ? `\n\n${lines.join("\n")}` : ""
}

/** 汇总所有已注册子Agent 的预置项目注册表（{AGENT_NAME_UPPER}_PROJECTS）：装载模式下总Agent 直接使用子Agent 工具时 project 参数路由用；同名去重（首个生效）。 */
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
  return out
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
 *  模型在装载前即可按项目名/工具能力关联任务与代码位置（路由匹配面）。 */
export function agentDescription(deps: PromptDeps, d: { name: string; description: string; tools?: string[] }, user: string, env: Record<string, string>): string {
  const projects = deps.presetProjectsFor(user, env, d.name)
  const parts = [d.description]
  if (projects.length) parts.push(`预置项目：${projects.map((p) => `${p.name}${p.description ? `: ${p.description}` : ""}（${p.path}）`).join("、")}`)
  const tools = d.tools ?? []
  if (tools.length) {
    parts.push(`装载后工具：${tools.slice(0, 10).join("、")}${tools.length > 10 ? ` 等 ${tools.length} 个` : ""}（以 ${d.name}_ 前缀调用；文件读写查询等通用工具为全局工具，直接用全局名）`)
  }
  return parts.join(" ")
}

/** 子Agent 提示词段落（职责分隔头 + 项目注记 + 静态提示词 + 项目 AGENTS.md）：runNewSession 预加载拼接
 *  与运行中装载（路由自愈）共用。动态环境注记（项目根/预置项目清单/受限模式）置于职责分隔头之后、
 *  静态提示词之前——配置信息前置，模型开工先读环境（目标项目与 project 参数取值），再读工作流。 */
export async function buildAgentSection(deps: PromptDeps, def: SubAgentDef, user: string, env: Record<string, string>, sessionId: string): Promise<string> {
  // 项目内置（特定项目绑定）：会话环境变量 {AGENT_NAME_UPPER}_PROJECT（如 CODE_PROJECT）指定子Agent 的项目根
  const projectRoot = deps.resolveSubAgentProject(user, env, def.name)
  const presetProjects = deps.presetProjectsFor(user, env, def.name)
  const workNote = projectRoot ? `\n项目根: ${projectRoot}` : `\n工作目录: ${sessionPath(deps.config.gebaiHome, user, sessionId)}/tmp`
  const presetNote = buildPresetNote(def.name, projectRoot, presetProjects)
  const restrictNote = env.CODE_RESTRICT_PROJECTS === "true"
    ? `\n受限模式（CODE_RESTRICT_PROJECTS=true）：仅允许操作预配置项目（${def.name} 的 ${def.name.toUpperCase()}_PROJECTS 清单，或 ${def.name.toUpperCase()}_PROJECT 绑定根），文件工具必须携带 project 参数，自由路径（path）不可用。`
    : ""
  return `### ${def.name}（${def.description}）\n${workNote}${presetNote}${restrictNote}${def.systemPrompt}${await deps.loadProjectAgentsMd(projectRoot)}`
}
