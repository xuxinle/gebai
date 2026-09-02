/** 子Agent/后台任务类全局工具（agent_load/agent_run/branch_run/branch_sync/bg_task）——自 core/tools.ts 按域拆分。
 *  branch_sync 不进全局表：仅在 branch_run 分支运行上下文注册（引擎注入）。 */
import type { Tool } from "../base/types"
import { normalizeBranchSpecs, type BranchRunRecord } from "../session/branch-runs"
import { shTaskStatus, type ShTaskRecord } from "../exec/sh-tasks"
import type { SessionRunRecord } from "../session/session-runs"
import { truncate, TRUNCATE_THRESHOLD } from "../support/truncate"
import { schema, type GlobalToolEntry } from "./shared"

/** bg_task 命令任务（sh async:true）输出尾部默认/上限（字符）：后台任务输出可能持续增长，status/wait 仅取尾部。 */
const SH_TASK_TAIL_DEFAULT = 4000
const SH_TASK_TAIL_MAX = 20000
/** bg_task wait 默认等待秒数（上限对齐脚本超时上限 540，保证不晚于引擎 9 分钟兜底；命令任务与子Agent 运行同口径）。 */
const SH_TASK_WAIT_DEFAULT_S = 60
const SH_TASK_WAIT_MAX_S = 540

function shTaskTailChars(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n) || n <= 0) return SH_TASK_TAIL_DEFAULT
  return Math.min(Math.floor(n), SH_TASK_TAIL_MAX)
}

function shTaskWaitMs(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n) || n <= 0) return SH_TASK_WAIT_DEFAULT_S * 1000
  return Math.min(n, SH_TASK_WAIT_MAX_S) * 1000
}

function shTaskElapsed(r: { startedAt: number; endedAt?: number }, now = Date.now()): number {
  return Math.round(((r.endedAt ?? now) - r.startedAt) / 1000)
}

/** 命令任务状态行（bg_task status/wait/stop 单任务 + list 每行共用）。 */
function shTaskLine(r: ShTaskRecord, label?: string): string {
  const status = shTaskStatus(r)
  const head = `${label ?? ""}taskId ${r.id} [${status}] ${shTaskElapsed(r)}s`
  if (status === "running") return `${head} — ${r.command}`
  const exit = r.exitCode === undefined ? "（退出码未知）" : `（exit ${r.exitCode}）`
  const suffix = r.timedOut ? " [生命周期超时已终止]" : r.killed ? " [已手动终止]" : r.lost ? " [进程已结束，服务可能重启过]" : r.spawnError ? ` [启动失败: ${r.spawnError.slice(0, 200)}]` : ""
  return `${head}${exit}${suffix} — ${r.command}`
}
export const agentListTool: Tool = {
  name: "agent_list",
  description: "列出可用子Agent（名称、描述、是否已装载）。工具名以已注册的工具集为准，不在此列出。",
  parameters: schema({}),
  outputSchema: schema({
    agents: {
      type: "array",
      items: schema({ name: { type: "string" }, description: { type: "string" }, loaded: { type: "boolean", description: "是否已装载" } }, ["name", "description", "loaded"]),
    },
  }, ["agents"]),
  async execute(_args, ctx) {
    const defs = ctx.listSubAgentDefs()
    if (!defs.length) return { output: "无可用子Agent。", data: { agents: [] } }
    return {
      output: defs
        .map((d) => `- ${d.name}${d.loaded ? " [已装载]" : " [未装载]"}: ${d.description}`)
        .join("\n"),
      data: { agents: defs.map((d) => ({ name: d.name, description: d.description, loaded: d.loaded })) },
    }
  },
}
export const agentLoadTool: Tool = {
  name: "agent_load",
  description: "装载指定子Agent 能力模块（类比 import 子模块）：其工具立即并入当前工具集（以 {agent}_ 前缀调用，schema 直接可见）、完整系统提示词注入当前上下文。不创建新上下文、无独立执行——装载后直接调用其工具，全程在当前会话内完成；重复装载幂等跳过。",
  card: { titleParams: ["name"], args: "none" },
  parameters: schema({ name: { type: "string" } }, ["name"]),
  async execute(args, ctx) {
    const name = String(args.name)
    await ctx.loadSubAgent(name)
    // 装载反馈不枚举工具清单：{agent}_* 工具 schema 已注册进工具集（下一轮请求即全量下发），再列一遍是冗余
    return {
      output: `子Agent ${name} 已装载：独有工具（如有）以 ${name}_ 前缀并入当前工具集（schema 直接可见）、完整系统提示词已注入上下文，直接调用其工具即可。`,
      data: { loaded: name },
    }
  },
}
export const agentRunTool: Tool = {
  name: "agent_run",
  description: "执行新会话：派生临时新会话（独立上下文，与主会话完全隔离），预加载指定子Agent 列表（可多个，其完整系统提示词与独有工具进入新会话）并执行任务，只返回最终结果文本；执行过程全程存档供历史回放。默认与主会话同构——全局工具（read/write/sh/grep 等，同名同参）与总Agent 全局系统提示词一并继承，子Agent 只需提供独有能力（如 code 的 search_symbols/analyze/git）。async:true 时后台异步执行——立即返回 runId 不阻塞（适合长任务），期间可处理其他任务；之后用 bg_task（action=status/wait/stop/list）查询进度、等待结果或主动终止。",
  card: { titleParams: ["agents"] },
  parameters: schema(
    {
      agents: { type: "array", items: { type: "string" }, description: "预加载进新会话的子Agent 名称列表（一个或多个，如 [\"code\", \"playwright\"]）" },
      input: { type: "string", description: "任务指令（新会话的初始消息）" },
      async: { type: "boolean", description: "可选：true 后台异步执行——立即返回 runId 不等待完成（适合长任务，期间可处理其他任务）；后续用 bg_task（action=status/wait/stop/list）查询进度、等待结果或终止" },
      inherit_global_tools: { type: "boolean", description: "是否继承全局工具进新会话（默认 true——新会话与主会话同构的完整工具面；false = 仅预加载子Agent 的工具，依赖全局文件工具的子Agent（如 code）将无法读写文件，慎用）" },
      inherit_global_prompt: { type: "boolean", description: "是否注入总Agent 全局系统提示词进新会话（默认 true——与全局工具继承一致，新会话与主会话行为约定同构；false = 仅子Agent 提示词，上下文最省）" },
    },
    ["agents", "input"],
  ),
  async execute(args, ctx) {
    const agents = Array.isArray(args.agents) ? args.agents.map(String) : []
    if (!agents.length) return { output: "参数 agents 必须为非空子Agent 名称列表。" }
    const input = String(args.input)
    const opts = {
      inheritGlobalTools: args.inherit_global_tools !== false,
      inheritGlobalPrompt: args.inherit_global_prompt !== false,
    }
    // 异步后台执行（DESIGN「新会话执行的异步运行」）：立即返回 runId，bg_task 管理进度/结果/终止
    if (args.async === true) {
      if (!ctx.sessionRuns) return { output: "当前环境不支持异步子Agent 运行（sessionRuns 服务未注入）。" }
      const rec = await ctx.sessionRuns.start(agents, input, opts)
      return {
        output: `[后台子Agent 运行已启动] runId: ${rec.runId}\n子Agent: ${rec.agents.join(", ")}\n任务: ${input.slice(0, 500)}${input.length > 500 ? "…" : ""}\n（后台执行不阻塞会话——执行过程实时推送到前端；之后用 bg_task action=status id=${rec.runId} 查询进度，action=wait 等待完成并取回结果，action=stop 主动终止）`,
        data: { runId: rec.runId, agents: rec.agents },
      }
    }
    const result = await ctx.runNewSession(agents, input, opts)
    // 最终返回超长时截断（与其余工具一致）；新会话完整存档原样挂到调用记录（截断只影响主上下文可见的结果文本）
    const safe = !result.output || result.output.length <= TRUNCATE_THRESHOLD ? { output: result.output } : await truncate(result.output, `session_${agents[0]}`, ctx)
    return { output: safe.output, sessionRun: result.archive }
  },
}
function agentTaskElapsed(r: { startedAt: number; endedAt?: number }, now = Date.now()): number {
  return Math.round(((r.endedAt ?? now) - r.startedAt) / 1000)
}

/** 子Agent 运行状态行（bg_task status/wait/stop 单任务 + list 每行共用；进度含轮次/工具调用/最近活动）。 */
function agentTaskLine(r: SessionRunRecord): string {
  const head = `runId ${r.runId} [${r.status}] ${agentTaskElapsed(r)}s — ${r.agents.join(", ")}`
  if (r.status === "running") {
    const progress = `已 ${r.rounds} 轮回复、${r.toolCalls} 次工具调用${r.last ? `，最近: ${r.last}` : ""}`
    return `${head}（${progress}）`
  }
  const suffix = r.status === "cancelled" ? "（已主动终止）" : r.status === "failed" ? `（失败: ${r.error ?? "未知原因"}）` : "（已完成）"
  return `${head} ${suffix}`
}

/** 分支耗时（秒）。 */
function branchTaskElapsed(r: { startedAt: number; endedAt?: number }, now = Date.now()): number {
  return Math.round(((r.endedAt ?? now) - r.startedAt) / 1000)
}

/** 分支运行状态行（bg_task status/wait/stop + list 共用；进度含轮次/工具调用/最近活动）。 */
function branchTaskLine(r: BranchRunRecord): string {
  const head = `branchId ${r.branchId}「${r.name}」 [${r.status}] ${branchTaskElapsed(r)}s${r.model ? ` · ${r.model}` : ""}`
  if (r.status === "running") {
    const progress = `已 ${r.rounds} 轮回复、${r.toolCalls} 次工具调用${r.last ? `，最近: ${r.last}` : ""}`
    return `${head}（${progress}）`
  }
  const suffix = r.status === "done" ? (r.merged ? "（已完成并合入主上下文）" : "（已完成）") : r.status === "cancelled" ? "（已终止，未合入）" : `（失败: ${r.error ?? "未知原因"}）`
  return `${head} ${suffix}`
}
/** 会话分支运行（DESIGN「会话分支运行与合并」，git 式并发）：从主会话**当前上下文** fork 出多个分支并行执行——
 *  各分支独立 LLM 循环、共享主线上下文快照与工具面，可按模型路由（model 参数，GEBAI_LLM_ROUTES 多路接口）
 *  走不同端点并行；分支最终报告完成后**自动合入主上下文**（主线下轮即见），可不断分支合并推进任务。
 *  与 agent_run 的区别：agent_run 派生隔离新会话（子Agent 提示词，不继承主会话历史）；分支 fork 主上下文
 *  （同一历史/系统提示词/工具面）——适合同一任务的并行多路探索与执行，摆脱单轮串行的模型服务速度限制。 */
export const branchRunTool: Tool = {
  name: "branch_run",
  description: "会话分支运行（git 式并发）：从主会话当前上下文 fork 出多个并行分支，各分支带独立任务指令同时执行（独立模型循环，同一上下文快照与工具面），分支最终报告完成后自动合入主上下文——像 git 一样不停分支合并，多分支并行摆脱单轮串行的模型服务速度限制。与 agent_run 不同：分支继承主会话全部历史与系统提示词（fork 而非隔离），适合同一任务的并行多路探索/执行（如多方案对比、多文件并行修改、多角度调研）；agent_run 适合委派独立子任务给子Agent。branches 为分支清单（每项 name 可选默认 b1..bN、prompt 必填、model 可选——GEBAI_LLM_ROUTES 配置的路由名或字面模型名，多分支走不同模型/端点并行更快）。merge 可选合入粒度（默认 full 全文合入；summary 摘要合入——长报告压成结论要点进主线、全文留过程存档，分支多/报告长时保上下文预算）。默认阻塞等全部分支完成（结果已合入，随后的合并消息可见全文）；async:true 后台执行——立即返回 branchId，完成自动合入，用 bg_task（action=status/wait/stop/list，id b 开头）管理。",
  parameters: schema(
    {
      branches: {
        type: "array",
        description: "分支清单（1-8 项）：每项 { name?: 分支名（默认 b1..bN，批内唯一）, prompt: 分支任务指令（必填，各分支专注一方面）, model?: 模型路由名或模型名（GEBAI_LLM_ROUTES 配置的路由走独立端点，多路并行） }",
        items: {
          type: "object",
          properties: {
            name: { type: "string" },
            prompt: { type: "string" },
            model: { type: "string" },
          },
        },
      },
      async: { type: "boolean", description: "可选：true 后台并行执行——立即返回 branchId 不等待（主线继续其他工作，分支完成自动合入主上下文并推送前端）；用 bg_task（id b 开头）查询/等待/终止" },
      merge: { type: "string", enum: ["full", "summary"], description: "可选：报告合入粒度，默认 full 全文合入；summary 摘要合入——超阈值的长报告经模型压缩为「结论+关键发现+产物清单+建议」进主线上下文（报告全文保留在过程存档，bg_task wait 可取回），分支多/报告长时用，保主线上下文预算" },
    },
    ["branches"],
  ),
  outputSchema: schema(
    {
      branches: {
        type: "array",
        description: "分支执行结果（同步模式为终态，异步模式为启动态）",
        items: schema(
          {
            branchId: { type: "string" },
            name: { type: "string" },
            status: { type: "string", description: "running/done/failed/cancelled" },
            rounds: { type: "integer" },
            toolCalls: { type: "integer" },
            merged: { type: "boolean", description: "报告是否已合入主上下文" },
          },
          ["branchId", "name", "status"],
        ),
      },
    },
    [],
  ),
  async execute(args, ctx) {
    if (!ctx.branchRuns) return { output: "当前环境不支持会话分支运行（branchRuns 服务未注入——分支内/新会话执行内不可再分支，仅主会话主循环可用）。" }
    // 合入粒度（调用级，统一盖章到各分支规格）：非法值按 full 处理（缺省语义）
    const merge: "full" | "summary" = args.merge === "summary" ? "summary" : "full"
    const specs = normalizeBranchSpecs(args.branches).map((s) => ({ ...s, merge }))
    // fork 点 = 当前上下文：注册表构造时绑定本上下文 live messages（buildContext 注入），
    // start 同步切片快照——工具层无 live messages 访问权
    const started = await ctx.branchRuns.start(specs)
    if (args.async === true) {
      const lines = started.map((r) => `- 「${r.name}」 branchId: ${r.branchId}${r.model ? `（${r.model}）` : ""}`)
      return {
        output: `[分支已后台启动] 共 ${started.length} 个分支并行执行，完成即自动合入主上下文（过程实时推送到前端）:\n${lines.join("\n")}\n（主线可继续其他工作；用 bg_task action=status id=${started[0].branchId} 查询进度、action=wait 等待报告、action=stop 终止，action=list 列全部后台任务。）`,
        data: { branches: started.map((r) => ({ branchId: r.branchId, name: r.name, status: r.status, rounds: r.rounds, toolCalls: r.toolCalls, merged: r.merged })) },
      }
    }
    // 同步 fan-out/fan-in：等待全部分支终态（每分支完成即经引擎合并队列自动合入主上下文——
    // 排空点在本轮工具结果之后，随后模型调用即见全部合并消息，本结果只给概要不重复全文）
    const recs: BranchRunRecord[] = []
    for (const r of started) recs.push((await ctx.branchRuns.wait(r.branchId, shTaskWaitMs(540))) ?? r)
    const lines = recs.map((r) => `- ${branchTaskLine(r)}`)
    const failed = recs.filter((r) => r.status !== "done")
    return {
      output: `[分支并行执行完成] 共 ${recs.length} 个分支${failed.length ? `（${failed.length} 个未正常完成，详情见各状态行）` : "，全部成功"}。各分支完整报告已作为合并消息追加进主上下文（随后的 assistant 消息，过程存档可回放）:\n${lines.join("\n")}`,
      data: { branches: recs.map((r) => ({ branchId: r.branchId, name: r.name, status: r.status, rounds: r.rounds, toolCalls: r.toolCalls, merged: r.merged })) },
    }
  },
}
/** 分支与主干双向同步（DESIGN「会话分支运行与合并」互相感知）：**仅在 branch_run 分支运行上下文注册**（引擎
 *  runBranch 装配分支注册表时注入，主会话/新会话执行不可见）——分支的唯一同步工具，传 content 即交出、
 *  不传即拉取，均返回主干增量。ctx.branchSync 由引擎按分支身份绑定（合入 + 增量快照一体）。 */
export const branchSyncTool: Tool = {
  name: "branch_sync",
  description: "分支与主干双向同步（分支内唯一协作工具，两种用法）：①交出——传 content 把阶段性成果（重要结论/产物清单/对其他分支有用的发现）立即合入主干并广播其他并行分支（各自下一轮可见），本分支继续执行不受影响、可多次调用，分支完成时的最终报告仍会自动合入；②拉取——不传 content 返回主干自你 fork（或上次同步）以来的全部新消息（主线用户输入与回复、其他分支合入的完整内容、主线工具结果摘要）。两种用法均返回主干增量——合入后立刻看到主线与其他分支动态；合入通知（【分支感知】/【主线进展】）只是摘要，需要完整内容（如依赖兄弟分支详细发现做决策）时用②拉取，每次只返回上次同步之后的新内容（增量式）。仅 branch_run 分支运行内可用。",
  parameters: schema(
    { content: { type: "string", description: "可选：阶段性成果全文（传入即合入主干并广播其他分支——写清结论与对协作方有用的信息）；省略 = 纯拉取主干增量" } },
    [],
  ),
  async execute(args, ctx) {
    if (!ctx.branchSync) return { output: "当前上下文不支持分支同步（branch_sync 仅在 branch_run 分支运行内可用）。" }
    const content = typeof args.content === "string" ? args.content.trim() : ""
    const delta = await ctx.branchSync(content || undefined)
    return content
      ? { output: `已合入主干：主线与其他并行分支下一轮可见（本分支继续执行）。\n主干自 fork/上次同步以来的新进展:\n${delta}` }
      : { output: `主干新进展（自 fork/上次同步以来的增量）:\n${delta}` }
  },
}
/** 后台异步任务统一管理（DESIGN「sh 异步后台任务」「新会话执行的异步运行」「会话分支运行与合并」）：三类任务
 *  同构管理面，按 id 前缀自动识别——命令任务（sh async:true 启动，id 形如 tXXXXXXXX：status/wait 附输出尾部，
 *  stop 杀进程树，磁盘落盘跨重启可见（lost 判定））、子Agent 运行（agent_run async:true 启动，id 形如
 *  rXXXXXXXX：status/wait 附进度/最终结果与完整存档，stop 协作中止，进程内随服务重启中断）与分支运行
 *  （branch_run async:true 启动，id 形如 bXXXXXXXX：完成报告自动合入主上下文，status/wait/stop 同构管理）。
 *  action=status 立即返回状态 / wait 阻塞等待完成（timeout 秒内未完成返回当前状态可再次 wait）/
 *  stop 终止 / list 列出本会话全部后台任务（三类合并）。管理动作免审批。 */
export const bgTaskTool: Tool = {
  name: "bg_task",
  description: "统一管理后台异步任务（按 id 前缀自动识别三类，无需指定类型）：命令任务（sh async:true 启动，taskId 形如 tXXXXXXXX）、子Agent 运行（agent_run async:true 启动，runId 形如 rXXXXXXXX）与分支运行（branch_run async:true 启动，branchId 形如 bXXXXXXXX——完成报告自动合入主上下文，无需取回动作）。action=status 立即返回状态——命令任务附输出尾部（stdout+stderr 合并日志，完整日志 tmp/sh-tasks/{id}.log），运行附进度（已执行轮次/工具调用/最近活动，已结束含最终结果），分支附进度与合入状态；action=wait 阻塞等待完成并取回结果（运行完成时附完整存档供回放；分支报告已自动合入，wait 仅确认终态与存档；timeout 秒内未完成返回当前状态，适合「先做别的再回头等结果」）；action=stop 终止（命令任务杀进程树、运行/分支协作中止，已执行过程保留在存档）；action=list 列出本会话全部后台任务。",
  card: { titleParams: ["action", "id"] },
  parameters: schema(
    {
      action: { type: "string", enum: ["status", "wait", "stop", "list"], description: "操作（必填）" },
      id: { type: "string", description: "任务 id——命令任务 taskId（t 开头）、运行 runId（r 开头）或分支 branchId（b 开头），action=list 可省略" },
      timeout: { type: "number", description: "wait 操作等待秒数（默认 60，上限 540）" },
      tail: { type: "number", description: "命令任务返回输出尾部字符数（默认 4000，上限 20000）" },
    },
    ["action"],
  ),
  outputSchema: schema(
    {
      id: { type: "string", description: "任务 id（list 为空）" },
      kind: { type: "string", description: "sh=命令任务 / agent=子Agent 运行 / branch=分支运行" },
      status: { type: "string", description: "命令任务：running/done/failed/killed/timed_out/lost；运行：running/done/failed/cancelled；分支：running/done/failed/cancelled" },
      exitCode: { type: "integer", description: "命令任务退出码（未知为 null）" },
      rounds: { type: "integer", description: "运行/分支：已执行模型回复轮次" },
      toolCalls: { type: "integer", description: "运行/分支：已执行工具调用次数" },
      merged: { type: "boolean", description: "分支：报告是否已合入主上下文" },
      output: { type: "string", description: "命令任务输出尾部 / 运行最终结果文本 / 分支最终报告（done 时）" },
      tasks: { type: "array", description: "list 的任务概要（三类合并，按启动顺序）", items: schema({ id: { type: "string" }, kind: { type: "string", description: "sh/agent/branch" }, status: { type: "string" }, detail: { type: "string", description: "命令、子Agent 名单或分支名" } }, ["id", "kind", "status"]) },
    },
    [],
  ),
  async execute(args, ctx) {
    const action = String(args.action ?? "status")
    if (action === "list") {
      const shList = ctx.shTasks ? await ctx.shTasks.list() : []
      const runList = ctx.sessionRuns ? ctx.sessionRuns.list() : []
      const branchList = ctx.branchRuns ? ctx.branchRuns.list() : []
      const merged = [
        ...shList.map((r) => ({ startedAt: r.startedAt, line: shTaskLine(r), data: { id: r.id, kind: "sh" as const, status: shTaskStatus(r), detail: r.command } })),
        ...runList.map((r) => ({ startedAt: r.startedAt, line: agentTaskLine(r), data: { id: r.runId, kind: "agent" as const, status: r.status, detail: r.agents.join("+") } })),
        ...branchList.map((r) => ({ startedAt: r.startedAt, line: branchTaskLine(r), data: { id: r.branchId, kind: "branch" as const, status: r.status, detail: r.name } })),
      ].sort((a, b) => a.startedAt - b.startedAt)
      if (!merged.length) return { output: "本会话暂无后台任务（用 sh async:true、agent_run async:true 或 branch_run async:true 启动）。", data: { tasks: [] } }
      return {
        output: `本会话后台任务（${merged.length} 个，按启动顺序——t 开头为命令任务、r 开头为子Agent 运行、b 开头为分支运行）:\n${merged.map((t) => t.line).join("\n")}`,
        data: { tasks: merged.map((t) => t.data) },
      }
    }
    const id = String(args.id ?? "")
    if (!id) return { output: "缺少任务 id（status/wait/stop 需要传启动时返回的 taskId/runId；列清单用 action=list）。" }

    // 命令任务分支（id 前缀 t）：状态/输出尾部/进程树终止，磁盘落盘跨重启可见
    if (id.startsWith("t")) {
      if (!ctx.shTasks) return { output: "当前环境不支持命令后台任务（shTasks 服务未注入）。" }
      const tail = shTaskTailChars(args.tail)
      let rec = action === "wait" ? await ctx.shTasks.wait(id, shTaskWaitMs(args.timeout)) : action === "stop" ? await ctx.shTasks.kill(id) : await ctx.shTasks.refresh(id)
      if (!rec) return { output: `未找到命令后台任务: ${id}（taskId 以 sh async:true 的返回为准；查现有任务用 action=list）。` }
      if (action === "wait" && !rec.endedAt) {
        // 等待超时仍在运行：返回当前状态与已有输出，模型可再次 wait 或继续其他工作
        const out = await ctx.shTasks.readLog(id, tail)
        const text = `${shTaskLine(rec, "")}\n（等待超时仍在运行；可再次 wait、用 status 查询，或 stop 终止）\n已产出输出（尾部 ${Math.min(out.length, tail)} 字符）:\n${out || "（暂无输出）"}`
        return { ...(await truncate(text, "bg_task", ctx)), data: { id, kind: "sh", status: shTaskStatus(rec), exitCode: null, output: out } }
      }
      const out = await ctx.shTasks.readLog(id, tail)
      const text = `${shTaskLine(rec)}${out ? `\n输出（尾部 ${Math.min(out.length, tail)} 字符，完整日志 tmp/sh-tasks/${id}.log）:\n${out}` : "\n（无输出）"}`
      return { ...(await truncate(text, "bg_task", ctx)), data: { id, kind: "sh", status: shTaskStatus(rec), exitCode: rec.exitCode ?? null, output: out } }
    }

    // 子Agent 运行分支（id 前缀 r）：进度/最终结果与完整存档/协作中止，进程内随服务重启中断
    if (id.startsWith("r")) {
      if (!ctx.sessionRuns) return { output: "当前环境不支持异步子Agent 运行（sessionRuns 服务未注入）。" }
      const runs = ctx.sessionRuns
      if (action === "stop") {
        const rec = await runs.cancel(id)
        if (!rec) return { output: `未找到子Agent 后台运行: ${id}（runId 以 agent_run async:true 的返回为准；查现有运行用 action=list）。` }
        if (rec.status === "running") {
          return { output: `${agentTaskLine(rec)}\n（终止指令已下达，执行循环仍在收尾——稍后用 action=status 确认。）`, data: { id, kind: "agent", status: rec.status } }
        }
        const res = runs.result(id)
        const text = `子Agent 后台运行 ${id} 已终止（终止前 ${rec.rounds} 轮回复、${rec.toolCalls} 次工具调用，过程保留在存档可回放）。`
        return { output: text, sessionRun: res?.archive, data: { id, kind: "agent", status: rec.status, rounds: rec.rounds, toolCalls: rec.toolCalls } }
      }
      if (action === "wait") {
        const rec = await runs.wait(id, shTaskWaitMs(args.timeout))
        if (!rec) return { output: `未找到子Agent 后台运行: ${id}（runId 以 agent_run async:true 的返回为准；查现有运行用 action=list）。` }
        if (rec.status === "running") {
          // 等待超时仍在运行：返回当前进度，模型可再次 wait、用 status 查询或 stop 终止
          return {
            output: `${agentTaskLine(rec)}\n（等待超时仍在运行；可再次 wait、用 status 查询进度，或 stop 终止。）`,
            data: { id, kind: "agent", status: rec.status, rounds: rec.rounds, toolCalls: rec.toolCalls },
          }
        }
        const res = runs.result(id)
        const text = rec.status === "done"
          ? `子Agent 后台运行 ${id} 已完成（${agentTaskElapsed(rec)}s，${rec.rounds} 轮回复、${rec.toolCalls} 次工具调用）。\n最终结果:\n${rec.output || "（无输出文本）"}`
          : `子Agent 后台运行 ${id} ${rec.status === "cancelled" ? "已被终止" : "执行失败"}（${agentTaskElapsed(rec)}s，终止/失败前 ${rec.rounds} 轮回复、${rec.toolCalls} 次工具调用）${rec.error ? `: ${rec.error}` : ""}。`
        return { ...(await truncate(text, "bg_task", ctx)), sessionRun: res?.archive, data: { id, kind: "agent", status: rec.status, output: rec.output } }
      }
      // status
      const rec = runs.get(id)
      if (!rec) return { output: `未找到子Agent 后台运行: ${id}（runId 以 agent_run async:true 的返回为准；查现有运行用 action=list）。` }
      const text = rec.status === "done"
        ? `${agentTaskLine(rec)}\n最终结果:\n${rec.output || "（无输出文本）"}`
        : rec.status === "running"
          ? `${agentTaskLine(rec)}\n（执行中——可 wait 等待完成、status 跟踪进度或 stop 终止。）`
          : agentTaskLine(rec)
      return { ...(await truncate(text, "bg_task", ctx)), data: { id, kind: "agent", status: rec.status, rounds: rec.rounds, toolCalls: rec.toolCalls, output: rec.output } }
    }

    // 分支运行分支（id 前缀 b）：进度/合入状态/最终报告与完整存档/协作中止——报告完成即自动合入主上下文
    if (id.startsWith("b")) {
      if (!ctx.branchRuns) return { output: "当前环境不支持分支后台运行（branchRuns 服务未注入）。" }
      const branches = ctx.branchRuns
      if (action === "stop") {
        const rec = await branches.cancel(id)
        if (!rec) return { output: `未找到分支后台运行: ${id}（branchId 以 branch_run async:true 的返回为准；查现有分支用 action=list）。` }
        if (rec.status === "running") {
          return { output: `${branchTaskLine(rec)}\n（终止指令已下达，执行循环仍在收尾——稍后用 action=status 确认。）`, data: { id, kind: "branch", status: rec.status } }
        }
        const res = branches.result(id)
        const text = `分支后台运行 ${id}「${rec.name}」已终止（终止前 ${rec.rounds} 轮回复、${rec.toolCalls} 次工具调用，过程保留在存档可回放；终止分支不合入主上下文）。`
        return { output: text, sessionRun: res?.archive, data: { id, kind: "branch", status: rec.status, rounds: rec.rounds, toolCalls: rec.toolCalls, merged: rec.merged } }
      }
      if (action === "wait") {
        const rec = await branches.wait(id, shTaskWaitMs(args.timeout))
        if (!rec) return { output: `未找到分支后台运行: ${id}（branchId 以 branch_run async:true 的返回为准；查现有分支用 action=list）。` }
        if (rec.status === "running") {
          // 等待超时仍在运行：返回当前进度，模型可再次 wait、用 status 查询或 stop 终止
          return {
            output: `${branchTaskLine(rec)}\n（等待超时仍在运行；可再次 wait、用 status 查询进度，或 stop 终止。）`,
            data: { id, kind: "branch", status: rec.status, rounds: rec.rounds, toolCalls: rec.toolCalls, merged: rec.merged },
          }
        }
        const res = branches.result(id)
        const text = rec.status === "done"
          ? `分支后台运行 ${id}「${rec.name}」已完成（${branchTaskElapsed(rec)}s，${rec.rounds} 轮回复、${rec.toolCalls} 次工具调用），报告已自动合入主上下文（合并消息在主线上下文中可见）。\n最终报告:\n${rec.output || "（无输出文本）"}`
          : `分支后台运行 ${id}「${rec.name}」${rec.status === "cancelled" ? "已被终止" : "执行失败"}（${branchTaskElapsed(rec)}s，终止/失败前 ${rec.rounds} 轮回复、${rec.toolCalls} 次工具调用）${rec.error ? `: ${rec.error}` : ""}。`
        return { ...(await truncate(text, "bg_task", ctx)), sessionRun: res?.archive, data: { id, kind: "branch", status: rec.status, output: rec.output, merged: rec.merged } }
      }
      // status
      const rec = branches.get(id)
      if (!rec) return { output: `未找到分支后台运行: ${id}（branchId 以 branch_run async:true 的返回为准；查现有分支用 action=list）。` }
      const text = rec.status === "done"
        ? `${branchTaskLine(rec)}\n最终报告（已自动合入主上下文）:\n${rec.output || "（无输出文本）"}`
        : rec.status === "running"
          ? `${branchTaskLine(rec)}\n（执行中——完成自动合入主上下文；可 wait 等待、status 跟踪进度或 stop 终止。）`
          : branchTaskLine(rec)
      return { ...(await truncate(text, "bg_task", ctx)), data: { id, kind: "branch", status: rec.status, rounds: rec.rounds, toolCalls: rec.toolCalls, output: rec.output, merged: rec.merged } }
    }

    return { output: `未找到后台任务: ${id}（命令任务 taskId 以 sh async:true 返回为准（t 开头）、子Agent 运行 runId 以 agent_run async:true 返回为准（r 开头）、分支 branchId 以 branch_run async:true 返回为准（b 开头）；查现有任务用 action=list）。` }
  },
}

export const globalTools: GlobalToolEntry[] = [
  { name: "agent_load", tool: agentLoadTool },
  { name: "agent_run", tool: agentRunTool },
  { name: "branch_run", tool: branchRunTool },
  { name: "bg_task", tool: bgTaskTool },
]
