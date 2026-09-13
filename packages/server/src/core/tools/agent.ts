/** 子会话/后台任务类全局工具（agent_list/agent_load/subsession_run/subsession_merge/bg_task）——自 core/tools.ts 按域拆分。
 *  subsession_merge 不进全局表：仅在异步子会话运行上下文注册（引擎注入 ctx.subSessionMerge）。 */
import type { Tool } from "../base/types"
import {
  normalizeSubSessionSpecs,
  SUBSESSION_MAX_PER_CALL,
  type SubSessionRecord,
  type SubSessionSpec,
} from "../session/subsessions"
import { shTaskStatus, type ShTaskRecord } from "../exec/sh-tasks"
import { truncate, TRUNCATE_THRESHOLD } from "../support/truncate"
import { schema, type GlobalToolEntry } from "./shared"

/** bg_task 命令任务（sh async:true）输出尾部默认/上限（字符）：后台任务输出可能持续增长，status/wait 仅取尾部。 */
const SH_TASK_TAIL_DEFAULT = 4000
const SH_TASK_TAIL_MAX = 20000
/** bg_task wait 等待秒数（默认与上限相同，均为 1 分钟；命令任务/子会话运行同口径）：阻塞等待是「回头取结果」的
 *  便捷动作，上限压到 1 分钟强制按进度轮询——等满上限而不看过程是无收益的空耗。超时返回当前状态与进度，
 *  需要继续等再次 wait 即可。 */
const SH_TASK_WAIT_DEFAULT_S = 60
const SH_TASK_WAIT_MAX_S = 60
/** 同步 subsession_run 的 fan-in 等待上限（秒）：不带 async 时语义就是「阻塞等全部子会话完成」，
 *  不随 bg_task wait 上限收紧（后台等待才需要短轮询，同步等待本身就是设计意图）。 */
const SUBSESSION_SYNC_WAIT_S = 540

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

/** 耗时（秒）。 */
function elapsed(r: { startedAt: number; endedAt?: number }, now = Date.now()): number {
  return Math.round(((r.endedAt ?? now) - r.startedAt) / 1000)
}

function shTaskLine(r: ShTaskRecord, label?: string): string {
  const status = shTaskStatus(r)
  const head = `${label ?? ""}taskId ${r.id} [${status}] ${elapsed(r)}s`
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

/** 子会话状态行（subsession_run 结果 / bg_task status/wait/stop/list 共用；进度含轮次/工具调用/最近活动）。 */
function subSessionLine(r: SubSessionRecord): string {
  const flavor = r.inheritContext ? "继承上下文" : "隔离上下文"
  const head = `runId ${r.runId}「${r.name}」 [${r.status}] ${elapsed(r)}s — ${flavor}${r.agents.length ? ` · 子Agent ${r.agents.join("+")}` : ""}${r.model ? ` · ${r.model}` : ""}`
  if (r.status === "running") {
    const progress = `已 ${r.rounds} 轮回复、${r.toolCalls} 次工具调用${r.last ? `，最近: ${r.last}` : ""}`
    return `${head}（${progress}）`
  }
  const suffix = r.status === "done"
    ? r.inheritContext ? (r.merged ? "（已完成，报告已合入父会话）" : "（已完成）") : "（已完成）"
    : r.status === "cancelled" ? "（已终止）" : `（失败: ${r.error ?? "未知原因"}）`
  return `${head} ${suffix}`
}

/** 子会话运行（DESIGN「子会话运行」）：单任务形态 `input`（+ `agents`）或多任务并发形态 `subsessions`，
 *  一套父子会话模型统一「隔离子会话（spawn）」与「继承子会话（fork）」——`inherit_context` 决定 fork（继承父上下文）还是
 *  隔离新上下文，`async` 决定同步阻塞还是后台运行（同步不注入合并工具，异步注入 subsession_merge）。 */
export const subSessionRunTool: Tool = {
  name: "subsession_run",
  description:
    "子会话运行（父子会话模型，一个入口覆盖「执行新会话」与「并行分支」两种用法）：派生一个或多个子会话执行任务，" +
    "每个子会话是独立 LLM 循环（独立上下文与工具面、可各自指定模型路由走不同接口并行）。\n" +
    "① **是否继承父会话上下文**（`inherit_context`，默认 false）——false=派生**隔离新上下文**（子Agent 提示词 + 全局工具，适合委派独立子任务、防父上下文膨胀）；true=从**当前上下文 fork**（父消息历史 + 系统提示词 + 工具面快照，适合同一任务的并行多路探索/执行，各子会话都掌握父会话全部背景）。\n" +
    "② **是否加载子Agent**（`agents`，可省略/为空 = 不加载任何子Agent）——只提供该子Agent 的独有工具与系统提示词，执行语义与装载一致。\n" +
    "③ **结果交付**——继承上下文形态：子会话最终报告**自动合入父会话**（合并消息在本次工具结果之后进入上下文，过程存档可回放）；隔离形态：最终结果作为本次工具结果返回（异步则用 bg_task 取回）。\n" +
    "④ **同步 / 异步**（`async`，默认 false）——false=阻塞等全部子会话完成（父会话被占住；不注入合并工具）；true=立即返回 runId 后台执行（父会话继续其他工作，子会话注入 `subsession_merge` 可随时把阶段性成果合入父会话并感知父会话进展）。\n" +
    "⑤ **父会话控制**（异步）——`bg_task`（id 以 s 开头）status 查进度 / wait 等完成取结果 / stop 终止 / list 列全部。\n" +
    `单任务用 \`input\`（可选 \`agents\`/\`model\`）；多任务并发用 \`subsessions\` 数组（每项 { name?, input, agents?, model? }，最多 ${SUBSESSION_MAX_PER_CALL} 个，与 input 二选一）。`,
  card: { titleParams: ["subsessions"], args: "none" },
  parameters: schema(
    {
      input: { type: "string", description: "单任务形态：子会话任务指令（与 subsessions 二选一）" },
      agents: { type: "array", items: { type: "string" }, description: "单任务形态：预加载进子会话的子Agent 名单（省略/空 = 不加载任何子Agent）" },
      model: { type: "string", description: "单任务形态：模型路由名或字面模型名（GEBAI_LLM_ROUTES 配置的命名路由走独立端点；缺省沿用任务级模型）" },
      subsessions: {
        type: "array",
        description: `多任务并发形态（1-${SUBSESSION_MAX_PER_CALL} 项，与 input 二选一）：每项 { name?: 子会话名（缺省 s1..sN，批内唯一）, input: 任务指令（必填）, agents?: 预加载子Agent 名单, model?: 模型路由 }`,
        items: {
          type: "object",
          properties: {
            name: { type: "string" },
            input: { type: "string" },
            agents: { type: "array", items: { type: "string" } },
            model: { type: "string" },
          },
        },
      },
      inherit_context: { type: "boolean", description: "是否继承父会话上下文（默认 false）：true=从当前上下文 fork（父消息历史 + 系统提示词 + 工具面快照）；false=隔离新上下文（子Agent 提示词 + 全局工具）" },
      async: { type: "boolean", description: "默认 false（阻塞等全部子会话完成）；true=后台运行——立即返回 runId，父会话继续其他工作，子会话可用 subsession_merge 主动合入阶段性成果，经 bg_task（id 以 s 开头）查询/等待/终止" },
      merge: { type: "string", enum: ["full", "summary"], description: "继承上下文形态的报告合入粒度：默认 full 全文合入；summary 摘要合入——超长报告经模型压成「结论+关键发现+产物清单+建议」进父会话上下文（全文保留在过程存档）" },
      inherit_global_tools: { type: "boolean", description: "隔离形态：是否继承全局工具（默认 true——read/write/grep/sh 等与父会话同名同参；false = 仅子Agent 工具与内建编排）" },
      inherit_global_prompt: { type: "boolean", description: "隔离形态：是否注入总Agent 全局系统提示词（默认 true；false = 仅子Agent 提示词，上下文最省）" },
    },
    [],
  ),
  outputSchema: schema(
    {
      subsessions: {
        type: "array",
        description: "子会话执行结果（同步模式为终态，异步模式为启动态）",
        items: schema(
          {
            runId: { type: "string" },
            name: { type: "string" },
            status: { type: "string", description: "running/done/failed/cancelled" },
            inheritContext: { type: "boolean" },
            rounds: { type: "integer" },
            toolCalls: { type: "integer" },
            merged: { type: "boolean", description: "报告是否已合入父会话（继承上下文形态）" },
            output: { type: "string", description: "隔离形态的最终结果文本（done 时）" },
          },
          ["runId", "name", "status"],
        ),
      },
    },
    [],
  ),
  async execute(args, ctx) {
    if (!ctx.subSessions) return { output: "当前环境不支持子会话运行（subSessions 服务未注入——嵌套深度已达上限，DESIGN「子会话运行」）。" }
    // 参数与环境问题（形态二选一/数量超限/并发超限/未知名）向模型回传可修正的引导文本，
    // 不让工具调用以异常中断本轮（弱模型可自愈）
    let started: SubSessionRecord[]
    let specs: SubSessionSpec[]
    try {
      specs = normalizeSubSessionSpecs(args)
      started = await ctx.subSessions.start(specs)
    } catch (err) {
      return { output: `子会话运行未启动（参数或环境问题，请修正后重试）：${err instanceof Error ? err.message : String(err)}` }
    }
    const isAsync = specs[0].async
    const fork = specs[0].inheritContext
    if (isAsync) {
      const lines = started.map((r) => `- 「${r.name}」 runId: ${r.runId}${r.model ? `（${r.model}）` : ""}${r.agents.length ? ` · 子Agent ${r.agents.join("+")}` : ""}`)
      const tail = fork ? "完成即自动把最终报告合入本会话上下文" : "完成后用 bg_task 取回最终结果"
      return {
        output:
          `[子会话已后台启动] 共 ${started.length} 个并行执行（${fork ? "继承上下文" : "隔离上下文"}），${tail}（过程实时推送到前端）:\n${lines.join("\n")}\n` +
          `（本会话可继续其他工作；子会话内可用 subsession_merge 随时合入阶段性成果；用 bg_task action=status id=${started[0].runId} 查进度、action=wait 等完成、action=stop 终止，action=list 列全部后台任务。）`,
        data: { subsessions: started.map((r) => ({ runId: r.runId, name: r.name, status: r.status, inheritContext: r.inheritContext, rounds: r.rounds, toolCalls: r.toolCalls, merged: r.merged })) },
      }
    }
    // 同步 fan-in：等待全部子会话终态（继承上下文形态的报告在终态时经引擎合入队列入父上下文——
    // 排空点在本轮工具结果之后，父会话下轮模型调用即见全部合并消息，本结果只给概要不重复全文）
    const recs: SubSessionRecord[] = []
    for (const r of started) recs.push((await ctx.subSessions.wait(r.runId, SUBSESSION_SYNC_WAIT_S * 1000)) ?? r)
    const failed = recs.filter((r) => r.status !== "done")
    const lines = recs.map((r) => `- ${subSessionLine(r)}`)
    // 隔离形态：最终结果即交付物，随工具结果返回（继承上下文形态的报告已合入父上下文，不重复）
    const outputs: string[] = []
    if (!fork) {
      for (const r of recs) {
        const res = ctx.subSessions.result(r.runId)
        if (res?.output) outputs.push(recs.length === 1 ? res.output : `【${r.name}】\n${res.output}`)
      }
    }
    const single = recs.length === 1 ? ctx.subSessions.result(recs[0].runId) : undefined
    const head = fork
      ? `[子会话执行完成] 共 ${recs.length} 个子会话${failed.length ? `（${failed.length} 个未正常完成，详情见各状态行）` : "，全部成功"}。各子会话完整报告已作为合并消息追加进本会话上下文（随后的消息，过程存档可回放）:`
      : `[子会话执行完成] 共 ${recs.length} 个子会话${failed.length ? `（${failed.length} 个未正常完成，详情见各状态行）` : "，全部成功"}:`
    const body = `${head}\n${lines.join("\n")}${outputs.length ? `\n\n最终结果:\n${outputs.join("\n\n")}` : ""}`
    const safe = body.length <= TRUNCATE_THRESHOLD ? { output: body } : await truncate(body, "subsession_run", ctx)
    return {
      output: safe.output,
      // 单子会话运行：过程存档挂到本次调用记录（历史回放渲染折叠容器）；多子会话不挂（继承上下文形态的存档随合并消息）
      ...(single && !fork ? { subSessionArchive: single.archive } : {}),
      data: { subsessions: recs.map((r) => ({ runId: r.runId, name: r.name, status: r.status, inheritContext: r.inheritContext, rounds: r.rounds, toolCalls: r.toolCalls, merged: r.merged, ...(single ? { output: single.output } : {}) })) },
    }
  },
}

/** 子会话与父会话双向同步（**仅在异步子会话运行上下文注册**：引擎 runSubSession 装配上下文时注入，父会话/
 *  同步子会话不可见）——子会话唯一协作工具，传 content 即交出、不传即拉取，均返回父会话增量。 */
export const subSessionMergeTool: Tool = {
  name: "subsession_merge",
  description:
    "子会话与父会话双向同步（异步子会话内唯一协作工具，两种用法）：\n" +
    "① **交出**——传 content 把阶段性成果（重要结论/产物清单/对其他并行子会话有用的发现）立即合入父会话并广播其他并行子会话（各自下一轮可见）；本子会话继续执行不受影响、可多次调用（继承上下文形态运行完成时的最终报告仍会自动合入）。\n" +
    "② **拉取**——不传 content 返回父会话自你 fork（或上次同步）以来的全部新消息（父会话用户输入与回复、其他子会话合入的完整内容、父会话工具结果摘要）。\n" +
    "两种用法均返回父会话增量——合入后立刻看到父会话与其他子会话动态。合入通知（【子会话感知】/【父会话进展】）只是摘要，需要完整内容（如依赖兄弟子会话详细发现做决策）时用②拉取，每次只返回上次同步之后的新内容（增量式）。仅异步子会话运行内可用。",
  parameters: schema(
    { content: { type: "string", description: "可选：阶段性成果全文（传入即合入父会话并广播其他子会话——写清结论与对协作方有用的信息）；省略 = 纯拉取父会话增量" } },
    [],
  ),
  async execute(args, ctx) {
    if (!ctx.subSessionMerge) return { output: "当前上下文不支持子会话同步（subsession_merge 仅在异步子会话运行内可用；同步运行不注入合并工具）。" }
    const content = typeof args.content === "string" ? args.content.trim() : ""
    const delta = await ctx.subSessionMerge(content || undefined)
    return content
      ? { output: `已合入父会话：父会话与其他并行子会话下一轮可见（本子会话继续执行）。\n父会话自 fork/上次同步以来的新进展:\n${delta}` }
      : { output: `父会话新进展（自 fork/上次同步以来的增量）:\n${delta}` }
  },
}

/** 后台异步任务统一管理（DESIGN「sh 异步后台任务」「子会话运行」）：两类任务同构管理面，按 id 前缀识别——
 *  命令任务（sh async:true 启动，id 形如 tXXXXXXXX：status/wait 附输出尾部，stop 杀进程树，磁盘落盘跨重启可见）
 *  与子会话运行（subsession_run async:true 启动，id 形如 sXXXXXXXX：继承上下文形态完成即自动合入父会话，
 *  隔离形态 wait 取回结果；status/wait/stop/list 同构管理）。管理动作免审批。 */
export const bgTaskTool: Tool = {
  name: "bg_task",
  description:
    "统一管理后台异步任务（按 id 前缀自动识别两类，无需指定类型）：命令任务（sh async:true 启动，taskId 形如 tXXXXXXXX）与子会话运行（subsession_run async:true 启动，runId 形如 sXXXXXXXX）。" +
    "action=status 立即返回状态——命令任务附输出尾部（stdout+stderr 合并日志，完整日志 tmp/sh-tasks/{id}.log），子会话附进度（已执行轮次/工具调用/最近活动，已结束含最终结果与合入状态）；" +
    "action=wait 阻塞等待完成并取回结果（子会话完成时附完整存档供回放；继承上下文形态的报告已自动合入父会话，wait 仅确认终态与存档）；timeout 秒内未完成返回当前状态（上限 1 分钟——超时后建议用 status 看进度，不宜闭眼等）；" +
    "action=stop 终止（命令任务杀进程树、子会话协作中止，已执行过程保留在存档）；action=list 列出本会话全部后台任务。",
  card: { titleParams: ["action", "id"] },
  parameters: schema(
    {
      action: { type: "string", enum: ["status", "wait", "stop", "list"], description: "操作（必填）" },
      id: { type: "string", description: "任务 id——命令任务 taskId（t 开头）或子会话 runId（s 开头），action=list 可省略" },
      timeout: { type: "number", description: "wait 操作等待秒数（默认/上限 60——最长阻塞 1 分钟，超时返回当前状态与进度，需要继续等再次 wait 或改用 status 看进度）" },
      tail: { type: "number", description: "命令任务返回输出尾部字符数（默认 4000，上限 20000）" },
    },
    ["action"],
  ),
  outputSchema: schema(
    {
      id: { type: "string", description: "任务 id（list 为空）" },
      kind: { type: "string", description: "sh=命令任务 / subsession=子会话运行" },
      status: { type: "string", description: "命令任务：running/done/failed/killed/timed_out/lost；子会话：running/done/failed/cancelled" },
      exitCode: { type: "integer", description: "命令任务退出码（未知为 null）" },
      rounds: { type: "integer", description: "子会话：已执行模型回复轮次" },
      toolCalls: { type: "integer", description: "子会话：已执行工具调用次数" },
      merged: { type: "boolean", description: "子会话：报告是否已合入父会话" },
      output: { type: "string", description: "命令任务输出尾部 / 子会话最终结果文本（done 时）" },
      tasks: { type: "array", description: "list 的任务概要（两类合并，按启动顺序）", items: schema({ id: { type: "string" }, kind: { type: "string", description: "sh/subsession" }, status: { type: "string" }, detail: { type: "string", description: "命令或子会话名" } }, ["id", "kind", "status"]) },
    },
    [],
  ),
  async execute(args, ctx) {
    const action = String(args.action ?? "status")
    if (action === "list") {
      const shList = ctx.shTasks ? await ctx.shTasks.list() : []
      const subList = ctx.subSessions ? ctx.subSessions.list() : []
      const merged = [
        ...shList.map((r) => ({ startedAt: r.startedAt, line: shTaskLine(r), data: { id: r.id, kind: "sh" as const, status: shTaskStatus(r), detail: r.command } })),
        ...subList.map((r) => ({ startedAt: r.startedAt, line: subSessionLine(r), data: { id: r.runId, kind: "subsession" as const, status: r.status, detail: r.name } })),
      ].sort((a, b) => a.startedAt - b.startedAt)
      if (!merged.length) return { output: "本会话暂无后台任务（用 sh async:true 或 subsession_run async:true 启动）。", data: { tasks: [] } }
      return {
        output: `本会话后台任务（${merged.length} 个，按启动顺序——t 开头为命令任务、s 开头为子会话运行）:\n${merged.map((t) => t.line).join("\n")}`,
        data: { tasks: merged.map((t) => t.data) },
      }
    }
    const id = String(args.id ?? "")
    if (!id) return { output: "缺少任务 id（status/wait/stop 需要传启动时返回的 taskId/runId；列清单用 action=list）。" }

    // 命令任务分支（id 前缀 t）：状态/输出尾部/进程树终止，磁盘落盘跨重启可见
    if (id.startsWith("t")) {
      if (!ctx.shTasks) return { output: "当前环境不支持命令后台任务（shTasks 服务未注入）。" }
      const tail = shTaskTailChars(args.tail)
      const rec = action === "wait" ? await ctx.shTasks.wait(id, shTaskWaitMs(args.timeout)) : action === "stop" ? await ctx.shTasks.kill(id) : await ctx.shTasks.refresh(id)
      if (!rec) return { output: `未找到命令后台任务: ${id}（taskId 以 sh async:true 的返回为准；查现有任务用 action=list）。` }
      if (action === "wait" && !rec.endedAt) {
        const out = await ctx.shTasks.readLog(id, tail)
        const text = `${shTaskLine(rec, "")}\n（等待超时仍在运行；可再次 wait、用 status 查询，或 stop 终止）\n已产出输出（尾部 ${Math.min(out.length, tail)} 字符）:\n${out || "（暂无输出）"}`
        return { ...(await truncate(text, "bg_task", ctx)), data: { id, kind: "sh", status: shTaskStatus(rec), exitCode: null, output: out } }
      }
      const out = await ctx.shTasks.readLog(id, tail)
      const text = `${shTaskLine(rec)}${out ? `\n输出（尾部 ${Math.min(out.length, tail)} 字符，完整日志 tmp/sh-tasks/${id}.log）:\n${out}` : "\n（无输出）"}`
      return { ...(await truncate(text, "bg_task", ctx)), data: { id, kind: "sh", status: shTaskStatus(rec), exitCode: rec.exitCode ?? null, output: out } }
    }

    // 子会话运行分支（id 前缀 s）：进度/最终结果与完整存档/协作中止，进程内随服务重启中断
    if (id.startsWith("s")) {
      if (!ctx.subSessions) return { output: "当前环境不支持子会话后台运行（subSessions 服务未注入）。" }
      const runs = ctx.subSessions
      const missing = `未找到子会话运行: ${id}（runId 以 subsession_run 的返回为准；查现有运行用 action=list）。`
      if (action === "stop") {
        const rec = await runs.cancel(id)
        if (!rec) return { output: missing }
        if (rec.status === "running") {
          return { output: `${subSessionLine(rec)}\n（终止指令已下达，执行循环仍在收尾——稍后用 action=status 确认。）`, data: { id, kind: "subsession", status: rec.status } }
        }
        const res = runs.result(id)
        const text = `子会话运行 ${id}「${rec.name}」已终止（终止前 ${rec.rounds} 轮回复、${rec.toolCalls} 次工具调用，过程保留在存档可回放；终止的运行不合入父会话）。`
        return { output: text, subSessionArchive: res?.archive, data: { id, kind: "subsession", status: rec.status, rounds: rec.rounds, toolCalls: rec.toolCalls } }
      }
      if (action === "wait") {
        const rec = await runs.wait(id, shTaskWaitMs(args.timeout))
        if (!rec) return { output: missing }
        if (rec.status === "running") {
          return {
            output: `${subSessionLine(rec)}\n（等待超时仍在运行；可再次 wait、用 status 查询进度，或 stop 终止。）`,
            data: { id, kind: "subsession", status: rec.status, rounds: rec.rounds, toolCalls: rec.toolCalls },
          }
        }
        const res = runs.result(id)
        const text = rec.status === "done"
          ? `子会话运行 ${id}「${rec.name}」已完成（${elapsed(rec)}s，${rec.rounds} 轮回复、${rec.toolCalls} 次工具调用）${rec.inheritContext ? "，报告已自动合入本会话上下文（合并消息可见）" : ""}。\n最终结果:\n${rec.output || "（无输出文本）"}`
          : `子会话运行 ${id}「${rec.name}」${rec.status === "cancelled" ? "已被终止" : "执行失败"}（${elapsed(rec)}s，终止/失败前 ${rec.rounds} 轮回复、${rec.toolCalls} 次工具调用）${rec.error ? `: ${rec.error}` : ""}。`
        return { ...(await truncate(text, "bg_task", ctx)), subSessionArchive: res?.archive, data: { id, kind: "subsession", status: rec.status, output: rec.output, merged: rec.merged } }
      }
      // status
      const rec = runs.get(id)
      if (!rec) return { output: missing }
      const text = rec.status === "done"
        ? `${subSessionLine(rec)}\n最终结果${rec.inheritContext ? "（已自动合入父会话上下文）" : ""}:\n${rec.output || "（无输出文本）"}`
        : rec.status === "running"
          ? `${subSessionLine(rec)}\n（执行中——${rec.inheritContext ? "完成自动合入父会话上下文；" : ""}可 wait 等待、status 跟踪进度或 stop 终止。）`
          : subSessionLine(rec)
      return { ...(await truncate(text, "bg_task", ctx)), data: { id, kind: "subsession", status: rec.status, rounds: rec.rounds, toolCalls: rec.toolCalls, output: rec.output, merged: rec.merged } }
    }

    return { output: `未找到后台任务: ${id}（命令任务 taskId 以 sh async:true 返回为准（t 开头）、子会话 runId 以 subsession_run 返回为准（s 开头）；查现有任务用 action=list）。` }
  },
}

export const globalTools: GlobalToolEntry[] = [
  { name: "agent_load", tool: agentLoadTool },
  { name: "subsession_run", tool: subSessionRunTool },
  { name: "bg_task", tool: bgTaskTool },
]
