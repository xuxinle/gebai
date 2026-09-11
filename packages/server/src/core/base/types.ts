/**
 * 引擎侧类型入口：契约类型（Tool/ToolContext/SubAgentDef 等）以 @gebai/sdk 为单一来源，
 * 本文件 re-export 保持引擎内部既有引用路径稳定（迁移期兼容）；server 专有类型（ToolCallRecord/
 * SessionData/EventSink + 引擎服务注入字段）保留于此。
 * ToolContext 中引用引擎服务（shTasks/sessionRuns/branchRuns/renderDiagram/waitForCapture）的
 * 可选字段为引擎增强面——在契约基础类型上交叉扩展（EngineToolContext），子代理只见契约字段。
 */
import type { Tool as ContractTool, ToolContext as ContractToolContext, DynamicToolDef as ContractDynamicToolDef } from "@gebai/sdk"

export type {
  ToolResult,
  ToolResultImage,
  PresetProject,
  ChoiceOption,
  ChoicePlan,
  ChoiceResult,
  InteractionMode,
  OutputMode,
  ToolSet,
  EnvCatalogVar,
  SubAgentDef,
} from "@gebai/sdk"

/** 引擎增强的 ToolContext：在契约基础上注入引擎服务（sh 异步任务/新会话执行存档/分支运行/后端图表渲染）。
 *  全部可选——子代理与测试桩只依赖契约字段即可运行。 */
export type ToolContext = ContractToolContext & {
  /** sh 异步后台任务服务（引擎按会话注入，会话 tmp/sh-tasks/ 落盘）。 */
  shTasks?: import("../exec/sh-tasks").ShTaskService
  /** agent_run 异步后台运行服务（agent_run async:true 启动、bg_task 统一管理）。 */
  sessionRuns?: import("../session/session-runs").SessionRunService
  /** 会话分支运行服务（branch_run 工具；引擎仅主循环注入）。 */
  branchRuns?: import("../session/branch-runs").BranchRunService
  /** 后端渲染图表源码为 PNG 字节（show 图表分支 render=backend 时用）。 */
  renderDiagram?: (code: string, opts?: { format?: import("@gebai/sdk").DiagramFormat; background?: string; maxWidth?: number; maxHeight?: number }) => Promise<Uint8Array>
  /** 运行时工具定义注册（js 脚本 defineTool 用）。 */
  defineDynamicTool?: (def: ContractDynamicToolDef) => Promise<void>
}

/** 会话级运行时定义工具（js defineTool）的持久化形态（契约类型同形，引擎侧引用别名）。 */
export type DynamicToolDef = ContractDynamicToolDef

/** 引擎侧 Tool：契约 Tool 的 execute/requiresApproval 换用引擎增强 ToolContext（shTasks/sessionRuns 等服务
 *  注入字段，全部可选）。增强字段均为可选——契约工具（execute 接收契约 ctx）经函数参数逆变仍可
 *  赋值给本类型，子代理按契约实现、引擎侧实现按本类型，两者在同一注册表共存。 */
export interface Tool extends Omit<ContractTool, "execute" | "requiresApproval"> {
  requiresApproval?: boolean | ((args: Record<string, unknown>, ctx: ToolContext) => boolean | Promise<boolean>)
  execute: (args: Record<string, unknown>, ctx: ToolContext) => Promise<import("@gebai/sdk").ToolResult>
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
  publish(event: import("@gebai/sdk").AgentEvent): void
}
