/** 交互类全局工具（todo/ask/full_mode）——自 core/tools.ts 按域拆分。 */
import { randomUUID } from "node:crypto"
import type { TodoItem } from "@gebai/sdk"
import type { ChoiceOption, Tool } from "../base/types"
import { PLAN_DIR, buildPlanMarkdown, planFileName } from "../support/plan"
import { schema, type GlobalToolEntry } from "./shared"

export function makeTodoTool(): Tool {
  /** 单行待办：状态/标题/进度/预计耗时/id（id 供 update/delete 精确定位，同名待办区分用）。 */
  const line = (t: TodoItem): string => {
    const progress = t.progress !== undefined ? ` (${t.progress}%)` : ""
    const eta = t.etaMin !== undefined ? `（预计 ${t.etaMin} 分钟）` : ""
    return `[${t.status}] ${t.title}${progress}${eta}（id: ${t.id}）`
  }
  /** 完整清单文本（操作后随结果返回，让模型一次掌握最新状态，无需再查）。 */
  const snapshot = (todos: TodoItem[]): string => {
    if (!todos.length) return "当前全部待办（0 项）：（无）"
    return `当前全部待办（${todos.length} 项）：\n${todos.map(line).join("\n")}`
  }
  /**
   * 定位待办：id 优先（精确）；无 id 时按 title 定位——先精确匹配，唯一时命中；
   * 精确多匹配或无精确时唯一包含匹配兜底；仍不唯一/无匹配返回 undefined。
   */
  const matchTodo = (e: Record<string, unknown>, todos: TodoItem[]): TodoItem | undefined => {
    const id = String(e.id ?? "").trim()
    if (id) return todos.find((x) => x.id === id)
    const title = String(e.title ?? "").trim()
    if (!title) return undefined
    const exact = todos.filter((x) => x.title === title)
    if (exact.length === 1) return exact[0]
    const subs = todos.filter((x) => x.title.includes(title))
    if (subs.length === 1) return subs[0]
    return undefined
  }
  const tool: Tool = {
    name: "todo",
    description:
      "待办管理（统一入口）：entries 为操作列表，每项 op=add/update/delete。省略 entries 或传空数组 = 查询（清单含 id）。返回操作摘要与当前全部待办状态。新增待办开启新任务时，及时 delete 清理与当前任务无关的历史残留待办——待办只跟踪当前任务，陈旧条目徒增干扰与 token 浪费。",
    parameters: schema({
      entries: {
        type: "array",
        description: "操作列表（空/省略=查询）。每项：{ op: add|update|delete, 及对应字段 }",
        items: {
          type: "object",
          properties: {
            op: { enum: ["add", "update", "delete"], description: "操作类型" },
            title: { type: "string", description: "add：标题；update/delete：定位目标（无 id 时按标题精确或唯一包含匹配，多条同名用 id；update 有 id 时作为新标题）" },
            id: { type: "string", description: "update/delete：待办 id（清单返回，优先于标题定位）" },
            status: { enum: ["pending", "in_progress", "completed", "failed", "cancelled"], description: "update：目标状态" },
            progress: { type: "number", description: "update：目标进度（0-100）" },
            priority: { enum: ["low", "medium", "high"], description: "add：优先级" },
            note: { type: "string", description: "add：备注" },
            eta: { type: "number", description: "add：预计耗时（分钟）" },
          },
          required: ["op"],
        },
      },
    }),
    outputSchema: schema({
      todos: {
        type: "array",
        description: "操作后的全部待办（查询时即当前清单）",
        items: schema({
          id: { type: "string" }, title: { type: "string" }, status: { type: "string", description: "pending/in_progress/completed/failed/cancelled" },
          priority: { type: "string", description: "low/medium/high" }, progress: { type: "number" }, etaMin: { type: "number", description: "预计耗时（分钟）" }, note: { type: "string" },
        }, ["id", "title", "status", "priority"]),
      },
    }, ["todos"]),
    async execute(args, ctx) {
      const todos = await ctx.getTodos()
      const entries = Array.isArray(args.entries) ? (args.entries as Array<Record<string, unknown>>) : []
      // 空列表 = 查询：不落盘不发布事件
      if (!entries.length) return { output: `查询待办：\n${snapshot(todos)}`, data: { todos } }
      const results: string[] = []
      const failures: string[] = []
      for (const raw of entries) {
        const e = raw ?? {}
        const op = String(e.op ?? "")
        if (op === "add") {
          const title = String(e.title ?? "").trim()
          if (!title) throw new Error("add 操作缺少 title")
          todos.push({
            id: randomUUID().replace(/-/g, ""),
            title,
            status: "pending",
            priority: (e.priority as TodoItem["priority"]) || "medium",
            etaMin: e.eta !== undefined ? Number(e.eta) : undefined,
            note: e.note ? String(e.note) : undefined,
          })
          results.push(`新增: ${title}`)
        } else if (op === "update" || op === "delete") {
          const t = matchTodo(e, todos)
          if (!t) {
            const byId = String(e.id ?? "").trim()
            const byTitle = String(e.title ?? "").trim()
            const key = byId ? `id: ${byId}` : byTitle ? `标题「${byTitle}」` : "（未指定 id/title）"
            const tip = byTitle && todos.filter((x) => x.title === byTitle).length > 1 ? "（标题匹配多个待办，请用 id 指定）" : ""
            failures.push(`${op === "update" ? "更新" : "删除"}未匹配 ${key}${tip}`)
            continue
          }
          if (op === "update") {
            // 有 id 时 title 表示新标题；无 id（按 title 定位）时 title 仅用于定位，不改标题
            if (String(e.id ?? "").trim()) {
              if (e.title !== undefined) t.title = String(e.title)
            } else {
              if (e.status === undefined && e.progress === undefined) {
                failures.push(`更新未变更任何字段（无 id 时 title 仅用于定位，改标题请用 id）: ${t.title}`)
                continue
              }
            }
            if (e.status) t.status = e.status as TodoItem["status"]
            if (e.progress !== undefined) t.progress = Number(e.progress)
            results.push(`更新: ${t.title}`)
          } else {
            const idx = todos.indexOf(t)
            todos.splice(idx, 1)
            results.push(`删除: ${t.title}`)
          }
        } else {
          throw new Error(`未知操作: ${op || "(空)"}（应为 add/update/delete）`)
        }
      }
      await ctx.setTodos(todos)
      ctx.publish("event.todo.update", { todos })
      const head =
        `待办操作完成（${results.length} 成功${failures.length ? `，${failures.length} 失败` : ""}）：\n` +
        results.join("\n") +
        (failures.length ? `\n失败：\n${failures.join("\n")}` : "")
      return { output: `${head}\n${snapshot(todos)}`, data: { todos } }
    },
  }
  return tool
}
/** 规范化 ask 选项询问分支的选项：纯文本 → { title }，复杂选项原样。 */
function normalizeChoiceOption(o: unknown): ChoiceOption {
  if (o && typeof o === "object") {
    const t = String((o as { title?: unknown }).title ?? "")
    if (t) return { title: t, description: (o as { description?: unknown }).description != null ? String((o as { description?: unknown }).description) : undefined }
  }
  return String(o ?? "")
}
/**
 * ask：向用户询问并**阻塞等待回应**——统一入口（原 ask_user/ask_env/plan 三工具合并），分支按专属参数三选一：
 * - 选项询问（options+prompt，原 ask_user）：用户点选/自定义文本/拒绝；multi=true 多选。
 * - 环境变量填值（name，原 ask_env）：前端弹窗填值后注入本次任务环境。
 * - 计划审批（title+steps/content，原 plan）：计划写入会话 tmp/plans/ 展示全文，批准/拒绝（附意见）。
 * 结果文案前缀（「用户选择：」「计划已批准」等）与「请审核计划」prompt 前缀是前端卡片识别契约，勿改。
 */
export const askTool: Tool = {
  name: "ask",
  description:
    "向用户询问并**阻塞等待回应**（统一入口，按参数三选一）：①选项询问——prompt + options（multi=true 可多选），用户点选/输入自定义文本/拒绝，结果返回后据此继续；适合方案确认、方向决策。②环境变量填值——name（+description 用途说明、secret 敏感掩码），前端弹窗填值后注入本次任务环境并保存浏览器本地；适合工具缺少必需凭证（API 密钥/Token 等）时向用户索取。③计划审批——title + steps（或 content 完整 Markdown），计划写入会话文件并在聊天界面展示全文，批准后严格按计划执行、拒绝可附修改意见修订重提；适合多步骤、有风险、需用户把关的任务（简单任务用 todo 跟踪即可）。",
  card: { args: "none" },
  parameters: schema(
    {
      prompt: { type: "string", description: "选项询问的问题文本（与 options 搭配，用户按此作答）" },
      options: {
        type: "array",
        description: "选项询问的选项清单（触发选项分支）：每项可为纯文本字符串，或复杂选项 { title, description }（UI 按标题+说明展示，返回值为 title）",
        items: {
          anyOf: [
            { type: "string" },
            { type: "object", properties: { title: { type: "string" }, description: { type: "string" } }, required: ["title"] },
          ],
        },
      },
      multi: { type: "boolean", description: "选项询问是否允许多选（默认单选）" },
      name: { type: "string", description: "环境变量填值分支（触发填值分支）：要请求的环境变量名（如 FEISHU_DOCS_APP_ID，仅限字母/数字/下划线）" },
      description: { type: "string", description: "（填值分支）变量用途说明（展示给用户，帮助其填写正确的值）" },
      secret: { type: "boolean", description: "（填值分支）是否敏感值（密钥/Token 等，输入框掩码显示，默认 false）" },
      title: { type: "string", description: "计划审批分支（触发计划分支）：计划标题（简明概括任务目标，如「重构订单模块」）" },
      steps: { type: "array", items: { type: "string" }, description: "（计划分支）执行步骤清单（按顺序，每步一句可执行动作；与 content 二选一）" },
      content: { type: "string", description: "（计划分支）可选：完整计划 Markdown 正文（提供时覆盖 steps 的自动拼装，用于复杂嵌套/表格结构）" },
    },
    [],
  ),
  outputSchema: schema(
    {
      status: { type: "string", description: "（仅计划分支返回）审批结果：approved/rejected/cancelled/timeout" },
      title: { type: "string", description: "（仅计划分支返回）计划标题" },
      path: { type: "string", description: "（仅计划分支返回）计划文档逻辑路径（tmp/plans/ 下，模型可经 read 读取）" },
      feedback: { type: "string", description: "（仅计划分支返回）拒绝时的用户修改意见（无则空）" },
    },
    ["status", "title", "path"],
  ),
  async execute(args, ctx) {
    // 分支分派（专属参数）：options/multi → 选项询问；name → 填值；title/steps/content → 计划审批
    if (args.options != null || args.multi === true) {
      const options = (Array.isArray(args.options) ? args.options : []).map(normalizeChoiceOption)
      if (!options.length) throw new Error("ask 选项询问需要至少一个选项（options）")
      if (!String(args.prompt ?? "").trim()) return { output: "ask 失败：选项询问（options）需同时提供 prompt（问题文本）。" }
      // 无交互模式无人可答：明确报错引导自行决策（不空等 5 分钟超时）
      if (ctx.interactionMode === "none") {
        return { output: "ask 失败：当前通道无交互能力（无交互调用），无法向用户询问。请基于现有信息自行决策，或在回复中说明选项供用户下次指示。" }
      }
      const prompt = String(args.prompt ?? "")
      // 阻塞等待用户回应：事件推送由引擎 waitForChoice 发布（含 choiceId/multi），
      // 用户经 UI/REST/WS 提交选项/自定义文本/拒绝后本工具才返回，模型据此继续
      const choice = await ctx.waitForChoice(prompt, options, args.multi === true)
      if (!choice) return { output: "用户未在时限内做出选择，已取消本次询问。请基于现有信息自行决策或换一种方式征询。" }
      if (choice.kind === "refuse") {
        return { output: "用户拒绝了本次询问。请停止继续询问，基于现有信息自行决策；如信息不足，说明所需信息并请用户另行补充。" }
      }
      if (choice.kind === "multi") return { output: `用户选择：${choice.values.join("、")}` }
      return { output: `用户选择：${choice.value}` }
    }
    if (args.name != null) {
      const name = String(args.name).trim()
      if (!name) return { output: "ask 失败：填值分支需要指定环境变量名（name）。" }
      if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) return { output: `环境变量名非法: ${name}（仅允许字母/数字/下划线，字母或下划线开头）。` }
      // 填值弹窗仅 Web 前端实时会话可用（飞书/无交互无弹窗通道）
      if (ctx.interactionMode && ctx.interactionMode !== "realtime") {
        return { output: "ask 失败：当前通道不支持填值弹窗（仅 Web 前端实时会话可用）。请说明所需配置引导用户在设置面板配置环境变量。" }
      }
      const ok = await ctx.waitForEnv(name, String(args.description ?? ""), args.secret === true)
      if (!ok) return { output: `用户未提供环境变量 ${name}（拒绝或超时）。请说明所需配置，或基于现有信息改用其他方式。` }
      return { output: `环境变量 ${name} 已由用户设置并注入本次任务（后续工具读取立即生效，并已保存到浏览器本地，后续任务自动生效）。` }
    }
    if (args.title != null || args.steps != null || args.content != null) {
      const title = String(args.title ?? "").trim()
      const steps = (Array.isArray(args.steps) ? args.steps : []).map(String).filter(Boolean)
      const content = args.content != null ? String(args.content) : ""
      if (!title) return { output: "ask 失败：计划审批分支需要指定计划标题（title）。" }
      if (!steps.length && !content.trim()) return { output: "ask 失败：计划审批分支需要至少一个执行步骤（steps）或完整计划内容（content）。" }
      if (ctx.interactionMode === "none") {
        return { output: "ask 失败：当前通道无交互能力（无交互调用），无法提交计划审批。请基于现有信息直接执行，并在回复中说明计划要点。" }
      }
      const md = buildPlanMarkdown(title, steps, content || undefined)
      const logical = `tmp/${PLAN_DIR}/${planFileName(title)}`
      const abs = ctx.resolvePath(logical)
      // 写范围守卫（子Agent 声明，引擎注入）：命中则拒绝落盘（计划文档属会话产物，常规不命中）
      const guardMsg = await ctx.writeGuard?.([abs])
      if (guardMsg) return { output: `计划文档未落盘：${guardMsg}` }
      try {
        await ctx.writeFile(abs, md)
      } catch (err) {
        return { output: `计划文档保存失败：${(err as Error).message}。请检查会话目录权限后重试。` }
      }
      // 阻塞等待用户审批：批准 → 模型继续按计划执行；拒绝（含自定义修改意见）→ 修订后重新提交。
      // plan 载荷（标题/正文/文档路径）随事件到达前端——选择卡内嵌计划全文，审批时直接可见
      const choice = await ctx.waitForChoice(
        `请审核计划「${title}」（已保存到会话文件 ${logical}）：批准后将按计划逐步执行；拒绝将返回模型修改；也可直接输入修改意见（视为拒绝）。`,
        ["批准执行", "拒绝执行"],
        false,
        { title, content: md, path: logical },
      )
      const data = { status: "", title, path: logical }
      if (!choice) {
        return { output: `计划审批超时：「${title}」（5 分钟未响应）。请先向用户确认计划是否可执行；若继续执行，说明计划已提交过审批。`, data: { ...data, status: "timeout" } }
      }
      if (choice.kind === "refuse") {
        return { output: `用户拒绝审核计划「${title}」。请停止计划相关操作，基于现有信息直接回答用户或询问其需求。`, data: { ...data, status: "cancelled" } }
      }
      const value = choice.kind === "multi" ? choice.values.join("、") : choice.value
      if (value === "批准执行") {
        return { output: `计划已批准：「${title}」。请严格按计划逐步执行（计划文档：${logical}，可用 read 读取），每完成一步用 todo 更新状态，关键节点向用户汇报。`, data: { ...data, status: "approved" } }
      }
      const feedback = value === "拒绝执行" ? "" : value
      const reviseNote =
        feedback === ""
          ? "用户未附具体修改意见，请自行分析计划可能存在的不足（目标不清/步骤缺失/风险未评估等），修订后重新提交新版本。"
          : `用户修改意见：${feedback}。请按意见修订计划后重新提交新版本。`
      return { output: `计划已拒绝：「${title}」。${reviseNote}`, data: { ...data, status: "rejected", feedback } }
    }
    return { output: "ask 失败：缺少询问内容——prompt+options（选项询问）/ name（环境变量填值）/ title+steps 或 content（计划审批）三选一。" }
  },
}
/** 切换完整模式（DESIGN「极简模式」）：仅极简会话可见可用（引擎 schema 过滤），需用户批准后执行。 */
export const fullModeTool: Tool = {
  name: "full_mode",
  description: "切换到完整模式（仅在极简模式会话中可用）：解锁全部工具（读取文件、编排、子Agent 等）。当前会话处于极简模式（仅 sh/edit）而任务确需其他工具能力时调用本工具，用户批准后本任务后续轮次与后续任务均启用完整工具集与完整说明；被拒绝则继续用 sh/edit 完成。",
  requiresApproval: true,
  card: { titleParams: ["reason"], args: "none" },
  parameters: schema({
    reason: { type: "string", description: "可选：需要完整模式的原因（展示给用户作审批参考）" },
  }, []),
  async execute(_args, ctx) {
    if (!ctx.exitMinimalMode) return { output: "当前环境不支持切换完整模式。" }
    await ctx.exitMinimalMode()
    return { output: "已切换到完整模式：全部工具已解锁、完整说明已生效（本任务后续轮次立即可用）。请继续执行任务。" }
  },
}


export const globalTools: GlobalToolEntry[] = [
  { name: "todo", tool: () => makeTodoTool() },
  { name: "ask", tool: askTool },
  // full_mode 仅极简会话可见可用（引擎按任务白名单过滤 schema）
  { name: "full_mode", tool: fullModeTool },
]
