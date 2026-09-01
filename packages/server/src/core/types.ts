import type { AgentEvent, AttachmentRef, ContentBlock, FileEntry, ToolSchema } from "@gebai/sdk"

export interface ToolResult {
  output: string
  /** 结构化输出（DESIGN「工具双输出」）：供 flow 数据流编排引用/映射（`{{步骤id.data.xxx}}`），不进模型上下文（模型只见 output 文本）。
   *  与 output 相互独立——output 面向模型分析，data 面向编排消费；无结构化语义的工具可省略。 */
  data?: unknown
  blocks?: ContentBlock[]
  truncated?: boolean
  filePath?: string
  /** agent_run（执行新会话）工具返回：新会话 run 完整存档（扩展字段落盘到工具调用记录，历史回放渲染用）。 */
  sessionRun?: import("@gebai/sdk").SessionRunArchive
}

/** 预置项目（来自 {AGENT_NAME_UPPER}_PROJECTS 环境变量的 JSON 数组项）。 */
export interface PresetProject {
  name: string
  path: string
  description?: string
}

/**
 * ask 选项询问分支的选项：纯文本（`string`）或复杂选项（`{ title, description? }`，UI 按标题+说明展示，提交值取 title）。
 */
export type ChoiceOption = string | { title: string; description?: string }

/**
 * ask 选择结果：
 * - `{ kind: "option"; value }`：单选（点选选项或输入的自定义文本）
 * - `{ kind: "multi"; values }`：多选（用户勾选的选项集合，可能含自定义文本）
 * - `{ kind: "refuse" }`：用户拒绝回答
 * - `null`：超时未作答
 */
export type ChoiceResult = { kind: "option"; value: string } | { kind: "multi"; values: string[] } | { kind: "refuse" } | null

export type ToolContext = {
  user: string
  /** 发起任务用户的角色（admin/user；公共资源权限判定用，如公共 mini-tool 仅管理员可写）。 */
  userRole?: string
  /** 认证模式（local=本地模式单用户 / server=服务模式多用户隔离；公共资源权限判定用）。 */
  authMode?: "local" | "server"
  sessionId: string
  workdir: string
  /** 会话工作区绝对路径（引擎恒定注入，不随项目绑定变化——workdir 在新会话绑定项目根时是项目根，
   *  保留项目名 tmp（DESIGN「项目机制」）恒指本路径）。可选：测试桩未注入时回退 workdir。 */
  sessionWorkdir?: string
  /** 子Agent 项目绑定根（{AGENT_NAME_UPPER}_PROJECT 解析结果，未绑定为空）：受限模式下未传 project 时允许在绑定根内操作。 */
  boundProjectRoot?: string
  /** GEBAI_HOME（截断文件/产物落盘基准，避免依赖进程 cwd）。 */
  home: string
  env: Record<string, string>
  sandboxed: boolean
  /** 当前任务交互模式（引擎按任务注入）：show 等合并型工具按分支校验通道能力（如 HTML 预览仅 realtime）。
   *  可选：测试桩/无引擎环境不注入时不做分支门控（保持全通道行为）。 */
  interactionMode?: InteractionMode
  resolvePath: (p: string) => string
  readFile: (p: string) => Promise<string>
  /** 读取二进制文件原始字节（vision 等工具用，路径同样经 resolvePath/沙箱约束）。 */
  readBinaryFile: (p: string) => Promise<Uint8Array>
  writeFile: (p: string, content: string) => Promise<void>
  /** 写入二进制文件原始字节（draw 后端渲染 PNG 落盘等，路径同样经 resolvePath/沙箱约束）。 */
  writeBinaryFile?: (p: string, data: Uint8Array) => Promise<void>
  /** 后端渲染图表源码为 PNG 字节（show 图表分支 render=backend 时用；format 缺省 plantuml；未注入时该模式不可用）。 */
  renderDiagram?: (code: string, opts?: { format?: import("@gebai/sdk").DiagramFormat; background?: string; maxWidth?: number; maxHeight?: number }) => Promise<Uint8Array>
  listFiles: (p?: string) => Promise<FileEntry[]>
  /** 列出单层目录内容（ls 工具用）。 */
  listDir: (p: string) => Promise<FileEntry[]>
  /** 删除文件/目录（file 工具 delete 动作用）。 */
  deleteFile: (p: string) => Promise<void>
  /** 移动/重命名文件（file 工具 move/rename 动作用）。 */
  moveFile: (from: string, to: string) => Promise<void>
  runCommand: (cmd: string, opts?: { shell?: string; workdir?: string; env?: Record<string, string>; timeoutMs?: number; input?: string; signal?: AbortSignal }) => Promise<{ stdout: string; stderr: string; code: number }>
  /** sh 异步后台任务服务（引擎按会话注入，会话 tmp/sh-tasks/ 落盘）：sh async:true 启动、bg_task 统一管理（查询/等待/终止）。
   *  可选：测试桩/无引擎环境不注入时 sh async 返回不可用说明。 */
  shTasks?: import("./sh-tasks").ShTaskService
  uploadAttachment: (ref: AttachmentRef) => Promise<string>
  publish: (type: string, payload: Record<string, unknown>) => void
  /** 任务取消信号（引擎按任务注入）：长时工具（js 脚本子进程等）监听中止并终止子进程。可选：测试桩不注入时不响应取消。 */
  signal?: AbortSignal
  /** 会话上下文注入（js 脚本工具用）：当前会话最近消息快照（文本抽取、条数/长度截断）。
   *  可选：测试桩/无引擎环境不注入时脚本侧 ctx.messages 为空数组。 */
  recentMessages?: () => Array<{ role: string; name?: string; content: string }>
  /** 运行时工具定义（js 脚本 defineTool 用）：校验并注册会话级动态工具（引擎注入——主会话进会话覆盖层并
   *  随会话落盘、新会话执行进本次运行注册表）。可选：未注入（测试桩/无引擎环境）时 defineTool 返回不可用错误。 */
  defineDynamicTool?: (def: DynamicToolDef) => Promise<void>
  /** 安全模式（GEBAI_SAFE_MODE=true 启动时加载）：flow 等工具内直接执行工具的工具需按同规则拦截。 */
  safeMode?: boolean
  /** js RPC 桥标记（js-tool 分发层注入，仅 js-tool 内部使用）：本 ctx 正在 js/动态工具子进程桥内执行工具——
   *  js 与动态工具的 execute 见标记即拒，封死 js→flow→js 交替递归与桥内失控子进程。 */
  fromJsBridge?: boolean
  /** 会话级已读文件追踪（防误覆盖/防陈旧覆盖，引擎按会话注入，分支运行 fork 独立快照）：
   *  read/write/edit/patch 成功后登记已读绝对路径与内容指纹（BOM 无关），write/edit/patch 写前据两项拦截——
   *  「已存在但本会话未读过」防盲写；「读过但内容自登记后被改动」（并行分支/主线/脚本命令/外部编辑）防陈旧覆盖。
   *  可选：测试桩/无引擎环境不注入时相关守卫自动放行（不改变行为）。 */
  fileGuard?: {
    /** 登记已读与读取时内容指纹（写入成功后同登记——写后内容即已掌握）；content 缺省仅登记已读（不参与陈旧比对）。 */
    markRead(absPath: string, content?: string): void
    hasRead(absPath: string): boolean
    /** 内容自本会话最后登记以来是否被改动（未登记或无指纹返回 false）：写入前以当前内容比对，命中即陈旧。 */
    staleSinceRead(absPath: string, currentContent: string): boolean
  }
  /** 写范围守卫（子Agent 声明、引擎按「会话已装载/新会话预加载的子Agent」注入）：文件写类工具
   *  （write/edit/patch/file（rename/move/delete））写入前以**解析后的绝对路径**调用，
   *  返回非空字符串 = 拒绝写入（作为工具结果返回引导模型调整，不抛错）。可选：未注入时不限制。 */
  writeGuard?: (absPaths: string[]) => string | null | Promise<string | null>
  /** 预置项目注册表（{AGENT_NAME_UPPER}_PROJECTS 环境变量解析，子Agent 运行环境注入）。 */
  projects: PresetProject[]
  /** 按预置项目名解析项目根（绝对路径，已按沙箱规则约束）；未知项目名抛错。 */
  resolveProjectPath: (name: string) => string
  getTodos: () => Promise<import("@gebai/sdk").TodoItem[]>
  setTodos: (todos: import("@gebai/sdk").TodoItem[]) => Promise<void>
  registry: {
    schemas(enabledOnly?: boolean): Array<{ name: string; description: string; parameters: Record<string, unknown> }>
    resolve(name: string): { name: string; tool: Tool } | undefined
    getAgentNames(): string[]
  }
  listSubAgentDefs: () => Array<{ name: string; description: string; preload: boolean; loaded: boolean; tools?: string[] }>
  /** 装载子Agent 能力模块（agent_load 工具）：其工具并入当前工具集、能力描述注入系统提示词；不创建新上下文、无独立执行（DESIGN「装载 vs 新会话执行」）。 */
  loadSubAgent: (name: string) => Promise<void>
  /** 执行新会话（agent_run 工具）：派生临时新会话、预加载指定子Agent 列表（完整系统提示词+工具）后阻塞执行任务，
   *  返回最终输出文本与完整执行存档（存档作为工具调用记录扩展字段落盘）。opts.inheritGlobalTools
   *  （默认 true）与 opts.inheritGlobalPrompt（默认 true）分别控制全局工具注册与总Agent 全局系统提示词
   *  注入——两者默认一致，新会话与主会话同构（DESIGN「新会话执行的上下文隔离」）。 */
  runNewSession: (agents: string[], input: string, opts?: { inheritGlobalTools?: boolean; inheritGlobalPrompt?: boolean }) => Promise<{ output: string; archive: import("@gebai/sdk").SessionRunArchive }>
  /** agent_run 异步后台运行服务（agent_run async:true 启动、bg_task 统一管理；引擎按会话注入）。
   *  可选：测试桩/无引擎环境不注入时 agent_run async 返回不可用说明。 */
  sessionRuns?: import("./session-runs").SessionRunService
  /** 会话分支运行服务（branch_run 工具，DESIGN「会话分支运行与合并」；引擎仅主循环注入——分支内/新会话
   *  执行内不可再分支）。可选：未注入（子Agent 运行环境/测试桩）时 branch_run 返回不可用说明。 */
  branchRuns?: import("./branch-runs").BranchRunService
  /** 分支与主干双向同步（branch_sync 工具，DESIGN「会话分支运行与合并」互相感知，分支唯一同步工具）：
   *  传 content = 交出阶段性成果立即合入主上下文（主线与其他并行分支下一轮可见，分支继续执行）；
   *  不传 = 纯拉取。均返回主干自 fork/上次同步以来的增量（主线输入/回复、其他分支合入全文、主线工具摘要）。
   *  可选：仅分支运行上下文注入——未注入时 branch_sync 返回不可用说明。 */
  branchSync?: (content?: string) => Promise<string>
  /** 向用户提出选择并阻塞等待选择结果（ask 选项询问分支用）；multi=true 多选；超时返回 null。 */
  waitForChoice: (prompt: string, options: ChoiceOption[], multi?: boolean) => Promise<ChoiceResult>
  /**
   * 请求用户设置环境变量并阻塞等待（ask 填值分支用）：发布 event.env.request 给前端弹窗填值，
   * 用户提交后值写入任务 env（本次任务后续工具读取立即生效）返回 true；拒绝/超时返回 false。
   */
  waitForEnv: (name: string, description?: string, secret?: boolean) => Promise<boolean>
  /**
   * 前端渲染图表并等待渲染结果（show 图表分支用）：发布 event.draw.render（含源码与图表语言 format）给前端，
   * 前端渲染成功后回传结果本方法才返回；渲染错误回传 error；超时（5 秒）返回 null。
   */
  waitForDraw: (render: { code: string; name?: string; format?: import("@gebai/sdk").DiagramFormat }) => Promise<{ ok: boolean; error?: string } | null>
  /**
   * 请求前端捕获当前页面并等待结果（page_capture 工具用）：发布 event.capture.request 给前端，
   * 前端回传渲染后 DOM html 与截图 base64（imageBase64）后返回；前端离线/超时（30 秒）返回 null。
   * fullPage=true 时前端截整页（高度上限见常量参考），缺省截视口。
   */
  waitForCapture: (opts?: { fullPage?: boolean; delayMs?: number }) => Promise<{ html: string; imageBase64?: string; error?: string } | null>
  /**
   * 退出极简模式（full_mode 工具用）：清除会话极简标记（会话内存 env + 当前任务工具白名单）并解锁
   * 全部工具（本任务后续轮次 schema 立即全量下发），发布 event.session.minimal 通知前端同步开关。
   * 可选：未注入（测试桩/无引擎环境）时 full_mode 返回不可用说明。
   */
  exitMinimalMode?: () => void | Promise<void>
  /**
   * 定时任务（cron_* 工具用，按当前用户绑定——用户级资源与会话解耦；服务端未启用定时任务能力时为空）。
   */
  cron?: {
    add: (input: import("./cron").CronCreateInput, originSessionId?: string) => Promise<import("./cron").CronTask>
    list: () => Promise<import("./cron").CronTask[]>
    remove: (id: string) => Promise<boolean>
    update: (id: string, patch: import("./cron").CronUpdateInput) => Promise<import("./cron").CronTask | null>
    trigger: (id: string) => Promise<import("./cron").CronTask | null>
  }
}

/** 引擎交互模式（DESIGN「交互模式」）：none=无交互（如 REST/HTTP 调用，单次请求无往返，需审批工具自动通过）、
 *  multi_turn=多轮交互（如飞书对话，多轮往返但无前端页面，仅关键操作询问用户）、realtime=实时交互（Web 前端）。
 *  工具按最低可用模式声明（Tool.interaction，缺省 none 即全模式可用）；高于当前模式声明的工具被自动禁用。 */
export type InteractionMode = "none" | "multi_turn" | "realtime"

/** 输出方式（请求层配置，DESIGN「交互模式与输出方式」）：final_only=仅最终响应（不推送文本增量/推理流）、
 *  streaming=流式输出（推送 event.message.delta/reasoning，默认）。结构化事件（工具调用/审批/最终 done）两种方式均推送。 */
export type OutputMode = "final_only" | "streaming"

export interface Tool {
  name: string
  description: string
  parameters: ToolSchema
  /** 是否需审批：布尔静态声明，或函数按调用参数动态判定（flow 等编排工具据此实现「内部任一工具需审批则整体审批」，
   *  引擎在审批点解析两种形态；函数异常按需审批处理）。 */
  requiresApproval?: boolean | ((args: Record<string, unknown>, ctx: ToolContext) => boolean | Promise<boolean>)
  /** 结构化输出（ToolResult.data）的 JSON Schema：经 tool_schemas 工具批量暴露给模型，供编排前理解输出结构。 */
  outputSchema?: ToolSchema
  /** 最低可用交互模式（缺省 none=全模式可用）：realtime=仅实时前端（如 page_capture）、
   *  multi_turn=至少多轮交互（飞书已有按钮/后端渲染适配）。当前模式低于声明时工具被禁用（schema 过滤 + 执行阻止）。
   *  合并型工具（如 show）不做工具级门控，按 ctx.interactionMode 在分支内校验通道能力。 */
  interaction?: Exclude<InteractionMode, "none">
  /** 卡片展示元数据（注册时声明，前端按声明渲染卡片，不硬编码工具名）：
   *  titleParams：参数名列表，其值直接拼入卡片标题（简单工具的关键参数，如 write 的 path；单参数仅显示值、多参数 key=value，
   *    长值智能截断——URL 保留头部、路径型保留尾部，截断时悬浮见全文；标题参数不在参数区重复展示）；
   *  args：参数展示模式——缺省自适应（扁平标量参数渲染为键值行，嵌套结构 JSON 高亮）/ "json"（强制完整 JSON 高亮）/
   *    "kv"（强制键值行，嵌套值紧凑 JSON）/ "none"（不展示参数区）/ "code"（codeField 参数渲染为代码块，其余参数键值行/JSON 附注）/
   *    "edits"（codeField 数组参数的 {oldString,newString} 项渲染为旧(红)/新(绿)对比块，如 edit 的 edits）/
   *    "block"（结果直出内容块，调用不显示通用卡片，如 show/diff）；超长参数区自动折叠；
   *  codeField/codeLang：args="code"/"edits" 时对应的代码/修改数组字段名与语言（如 sh 的 command/bash、edit 的 edits）；
   *  file：路径参数名（如 read/write/edit/patch 的 path）——声明本工具卡为文件卡（产物块为文件内容卡），
   *    前端「文件展示方式=弹窗查看」时其产物 file 块收敛为文件链接（点击弹窗查看，块路径为解析后真实路径——
   *    会话 tmp/ 逻辑或项目绝对路径，经 files/preview 取数）；参数区与输出不受影响。 */
  card?: {
    titleParams?: string[]
    args?: "json" | "kv" | "none" | "code" | "edits" | "block"
    codeField?: string
    codeLang?: string
    file?: string
  }
  /** 运行时定义工具标记（js 脚本 defineTool 注册）：js/动态工具子进程内禁止再调用（防递归嵌套子进程），
   *  RPC 分发层按本标记拦截。内置/子Agent 工具无此标记。 */
  runtimeDefined?: boolean
  /** 安全模式可用性自主声明（子Agent 工具用；全局风险工具不使用本字段——它们内置降级为只读/限范围形态）：
   *  true = 作者判定安全模式下可提供（即使短名风险如 xxx_sh，须自行保证实现只读或在体内按 ctx.safeMode 校验）；
   *  false = 作者判定安全模式下不提供（即使名字无风险命中，如内部会写文件/外发请求的工具）；
   *  未声明 = 按短名风险规则默认（isRiskyToolName 命中则安全模式下不注册）。 */
  safeMode?: boolean
  execute: (args: Record<string, unknown>, ctx: ToolContext) => Promise<ToolResult>
}

export type ToolSet = Record<string, Tool>

/** 子Agent 可配置环境变量项（前端环境变量面板白名单来源，变量名须以 `{AGENT_NAME_UPPER}_` 前缀）。 */
export interface EnvCatalogVar {
  name: string
  /** 变量作用说明（前端 tip 展示）。 */
  description: string
}

export interface SubAgentDef {
  name: string
  description: string
  systemPrompt: string
  /** 依赖的其他子Agent 名单（依赖自动装载）：装载/预加载/agent_run 预执行本子Agent 时自动连带
   *  装载全部依赖（工具按各自 {agent}_ 命名空间注册、提示词一并注入，不重复定义）——子Agent 间
   *  复用能力走依赖声明而非把依赖方的工具展开进自己的 def。依赖缺失（被启停名单移除/构建裁剪）时
   *  跳过不阻断；循环依赖在装载时报错（cascade 展开检测）。如 reverse_site 依赖 playwright、
   *  self_optimize 依赖 code。 */
  dependencies?: string[]
  /** 子Agent 自有工具（注册为 {agent}_{tool}）。可省略：纯提示词子 Agent（简单/组合式），
   *  省略时子 Agent 运行环境自动注入编排工具（agent_list/agent_load/agent_run）。 */
  tools?: ToolSet
  requiresApproval?: Record<string, boolean>
  preload?: boolean
  /** 子Agent 可配置环境变量声明（`{AGENT_NAME_UPPER}_*` 前缀）；汇总进环境变量目录（前端面板白名单）。 */
  envVars?: EnvCatalogVar[]
  /** 默认项目根兜底解析（`{AGENT_NAME_UPPER}_PROJECT` 环境变量未配置时生效）：返回绝对路径即视为项目
   *  绑定（提示词注入「项目根」注记、agent_run 新会话以其为工作目录、加载项目 AGENTS.md——与显式
   *  绑定同语义，沙箱模式同规则拒绝）。如 self_optimize 在脚本调试模式下按模块路径自动推导歌白仓库根。 */
  projectRoot?: (env: Record<string, string>) => string | undefined
  /** 写范围守卫声明：会话装载本子Agent（或新会话预加载）后，本政策注入 ToolContext.writeGuard——
   *  文件写类工具写入前以解析后的绝对路径调用，返回非空字符串 = 拒绝写入。用于 self_optimize 的
   *  「核心引擎源码默认只读」代码级强制（工具与提示词复用 code，写范围政策独立声明）。 */
  writeGuard?: (env: Record<string, string>, absPaths: string[]) => string | null
}

export interface ToolCallRecord {
  id: string
  name: string
  arguments: Record<string, unknown>
  approvalRequired: boolean
  status: "pending" | "approved" | "rejected" | "running" | "done" | "error"
  result?: string
  error?: string
  retries: number
}

/** 会话级运行时定义工具（js defineTool）的持久化形态：execute 源码序列化保存，
 *  随会话 chat.json 落盘、重启恢复；新会话执行内定义的工具不落盘（仅本次运行注册表）。 */
export interface DynamicToolDef {
  name: string
  description: string
  parameters: Record<string, unknown>
  source: string
  requiresApproval?: boolean
}

export interface SessionData {
  id: string
  name: string
  userId: string
  messages: Array<import("@gebai/sdk").Message>
  todos: import("@gebai/sdk").TodoItem[]
  createdAt: number
  updatedAt: number
  /** 会话已装载子Agent 名单（chat.json 持久化）：恢复历史会话时据此重新注册工具；
   *  未定义 = 新会话/旧格式，首次运行按启动预载名单（GEBAI_PRELOAD_SUB_AGENTS）初始化。 */
  loadedSubAgents?: string[]
  /** 上下文 token 估算（chars/4）：任务结束时持久化，会话列表展示用（单位 k）。
   *  有 usage 真值时 = 最近一次调用的真实 input tokens + 未发送增量估算。 */
  ctxTokens?: number
  /** 最近一次模型调用的真实 input tokens（服务端 usage 真值，含 system 提示词与工具 schema）：
   *  跨 run 上下文压缩判定基线；未定义 = 无真值（老会话/接口不返回 usage/压缩后锚点失效），走估算兜底。 */
  ctxInputTokens?: number
  /** 建立 ctxInputTokens 基线那次调用已覆盖的历史消息条数（loadHistory 坐标）：下次 run 以
   *  history.slice(ctxAtMessage) 估算基线之后的增量（下一次真实调用会用真值接管并重建基线）。 */
  ctxAtMessage?: number
  /** 会话级运行时定义工具清单（js defineTool 注册，chat.json 持久化、重启恢复）：序列化定义。 */
  dynamicTools?: DynamicToolDef[]
}

export interface EventSink {
  publish(event: AgentEvent): void
}
