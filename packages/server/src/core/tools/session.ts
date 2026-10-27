/** 会话管控全局工具（session）——跨会话的创建/列表/状态/消息注入/终止/删除。
 *
 * 与 subsession_run 的分工（DESIGN「会话管控」）：subsession_run 是「同步派生执行」（等待/合并回本上下文，
 * 生命周期与本次调用绑定）；本工具是「脱管会话管控」——目标会话与当前任务无父子关系，创建后由会话自身
 * 的事件循环驱动，本工具只做观察与控制平面动作（注入消息/终止/删除）。驻留演进（sticky 任务续跑）、
 * 多会话编排（主控 + 工作会话组）都以此为基础设施。
 *
 * 自保护（不操作自身）：create 返回的新会话与本会话无亲缘，可管控；stop/cancel/注入消息对
 * 当前执行会话（ctx.sessionId）一律拒绝——终止自己会让工具结果无处落盘（半途死锁），
 * 注入消息给自己则与既有任务循环竞态（引擎 tasks.has 检查会拒绝并发 run）。
 * 服务模式（sandboxed）：仅可见/可操作本用户会话（store 按用户过滤，权限由存储层保证）。
 *
 * 异步语义：send 注入即返回（不阻塞等待目标会话跑完——那是目标会话的事件循环的事）；
 * 需要结果用 status 轮询或读消息尾部。run() 的并发拒绝（同会话已有任务在运行）如实透传。 */
import type { SessionControlService, Tool } from "../base/types"
import { truncate } from "../support/truncate"
import { schema, type GlobalToolEntry } from "./shared"

/** 状态行（列表用）：一行一个会话，含运行态与更新时间。 */
function statusLine(s: { id: string; name?: string; updatedAt: number; running?: boolean }, now = Date.now()): string {
  const age = Math.round((now - s.updatedAt) / 60000)
  const ageText = age < 1 ? "刚刚" : age < 60 ? `${age} 分钟前` : `${Math.round(age / 60)} 小时前`
  const running = s.running ? " [运行中]" : ""
  const name = s.name ? `「${s.name}」` : ""
  return `- ${s.id}${name}${running}（更新 ${ageText}）`
}

/** 会话消息尾部摘要（status 用）：role + 文本截断。 */
function messageTailLine(m: { role: string; content?: unknown; name?: string }, max = 120): string {
  let text = ""
  if (typeof m.content === "string") text = m.content
  else if (Array.isArray(m.content)) text = m.content.map((b) => (b && typeof b === "object" && "text" in b ? String((b as { text?: unknown }).text ?? "") : "")).filter(Boolean).join(" ")
  text = text.replace(/\s+/g, " ").trim()
  if (!text) return `  ${m.role}${m.name ? `(${m.name})` : ""}: （无文本）`
  return `  ${m.role}${m.name ? `(${m.name})` : ""}: ${text.slice(0, max)}${text.length > max ? "…" : ""}`
}

export const sessionTool: Tool = {
  name: "session",
  description:
    "会话管控（跨会话控制平面）：创建新会话、列出/查看会话状态与消息尾部、向会话注入消息（触发其执行）、终止运行中的任务、删除会话。与 subsession_run 不同：本工具创建/操作的会话与当前上下文无父子绑定（脱管），适合驻留工作会话、多会话编排、演进会话观察与干预。send 驱动的轮次**默认免审**（无人值守，需审批工具不阻塞；approve=false 可收紧；interactive=false 同时关闭依赖前端的交互能力）。自保护：不能对当前会话自身执行任何动作（终止自己=结果无处落盘；注入自己=与任务循环竞态），create 的新会话不受此限。",
  parameters: schema(
    {
      action: {
        type: "string",
        enum: ["create", "list", "status", "send", "stop", "delete"],
        description: "create=创建新会话（可用 env 预置会话环境变量）；list=列出本用户会话（含运行态）；status=查单会话详情（消息尾部+待决交互）；send=注入用户消息触发执行（目标须空闲）；stop=终止运行中的任务（软终止，解开全部挂起等待）；delete=删除会话（进 GC 归档，7 天内可 restore）",
      },
      session_id: { type: "string", description: "目标会话 id（create/list 外必填）" },
      name: { type: "string", description: "create：会话名（缺省自动命名）" },
      message: { type: "string", description: "send：注入的用户消息（触发目标会话执行一轮）" },
      approve: {
        type: "boolean",
        description:
          "send：目标会话本轮是否需要审批（缺省 true = 免审——工具派出的活无人值守，需审批工具自动通过、不空等审批超时；显式 false = 需审批工具在无交互通道下直接拒绝，不进入等待）",
      },
      interactive: {
        type: "boolean",
        description: "send：目标会话本轮是否保留交互能力（缺省 true，即 ask/show 预览等可用）；false = 按无交互通道运行（依赖前端的交互工具直接返回不可用，不挂起等超时）",
      },
      env: { type: "object", additionalProperties: { type: "string" }, description: "create：会话环境变量（内存态会话 env，零落盘；服务重启即空——持久配置用任务级 env 或 AUTO_EVOLVE 类变量前缀）" },
      tail: { type: "number", description: "status：消息尾部条数（默认 12，上限 60）" },
    },
    ["action"],
  ),
  outputSchema: schema({
    session: { type: "object" },
    sessions: { type: "array", items: { type: "object" } },
    running: { type: "boolean" },
  }),
  async execute(args, ctx) {
    const action = String(args.action ?? "")
    const svc: SessionControlService | undefined = ctx.sessions
    if (!svc) return { output: "本上下文未注入会话管控服务（子代理契约上下文不可用；请在主会话/子会话运行上下文直接调用）。" }
    const selfId = ctx.sessionId

    // ---- create：创建新会话（脱管——与当前会话无亲缘，可被本工具管控）----
    if (action === "create") {
      const name = args.name ? String(args.name).slice(0, 80) : undefined
      const created = await svc.store.createSession(ctx.user, name)
      let envNote = ""
      const env = (args.env && typeof args.env === "object" && !Array.isArray(args.env) ? args.env : null) as Record<string, string> | null
      if (env && Object.keys(env).length) {
        // 值白名单化（防对象/数组混入）后写入内存态会话 env
        const vars: Record<string, string> = {}
        for (const [k, v] of Object.entries(env)) if (typeof v === "string" && v) vars[k] = v
        if (Object.keys(vars).length) {
          await svc.store.setEnv(created.id, ctx.user, vars)
          envNote = `\n- 已预置会话环境变量 ${Object.keys(vars).length} 项（内存态；服务重启即空）`
        }
      }
      return {
        output: `会话已创建: ${created.id}${created.name ? `「${created.name}」` : ""}（用户 ${created.userId}）。${envNote}\n- 注入消息触发执行：action=send, session_id=${created.id}, message=...\n- 查看状态：action=status, session_id=${created.id}；列表：action=list\n- 终止：action=stop；删除：action=delete\n（新会话与当前会话无父子绑定——subsession_run 的合并/等待语义不适用，结果经 status 轮询交互。）`,
        data: { session: { id: created.id, name: created.name } },
      }
    }

    // ---- list：本用户会话清单（含运行态）----
    if (action === "list") {
      const infos = await svc.store.listSessionInfos(ctx.user)
      const running = new Set(await svc.engine.runningIds(ctx.user))
      if (!infos.length) return { output: "无会话。", data: { sessions: [] } }
      const lines = infos.map((s) => statusLine({ ...s, running: running.has(s.id) })).slice(0, 50)
      const more = infos.length > 50 ? `\n…（共 ${infos.length} 个，仅列前 50）` : ""
      return { output: `本用户会话 ${infos.length} 个（当前会话 ${selfId}）：\n${lines.join("\n")}${more}`, data: { sessions: infos.slice(0, 50) } }
    }

    // 以下动作需要目标会话 id；自保护：不操作当前会话自身
    const sessionId = String(args.session_id ?? "").trim()
    if (!sessionId) return { output: `action=${action} 需要 session_id。` }
    if (sessionId === selfId) {
      return { output: `不能对当前会话自身执行 ${action}（自保护：终止自己=工具结果无处落盘；注入消息=与任务循环竞态）。当前会话 ${selfId} 的生命周期由用户/宿主通道管理。` }
    }

    // 目标存在性（load 兼做权限校验：服务模式仅本用户会话可见）
    const target = await svc.store.load(sessionId, ctx.user)
    if (!target) return { output: `会话不存在或无权访问: ${sessionId}` }

    // ---- status：详情（运行态 + 消息尾部 + 待决交互）----
    if (action === "status") {
      const running = svc.engine.isRunning(sessionId)
      const tailN = Math.min(Math.max(Number(args.tail) || 12, 1), 60)
      const msgs = (target.messages ?? []).filter((m) => m.role !== "system").slice(-tailN)
      const runtime = running ? (await svc.engine.runtimeOf(ctx.user))[sessionId] : undefined
      const pending = runtime && typeof runtime === "object" && "pending" in runtime ? (runtime as { pending: unknown[] }).pending : undefined
      const head = `会话 ${sessionId}${target.name ? `「${target.name}」` : ""} · ${running ? "运行中" : "空闲"} · ${target.messages.length} 消息 · ctx ${target.ctxTokens ?? "?"} tokens`
      const tailBlock = msgs.length ? `\n消息尾部 ${msgs.length} 条：\n${msgs.map((m) => messageTailLine(m)).join("\n")}` : "\n（无消息）"
      const pendingBlock = Array.isArray(pending) && pending.length ? `\n待决交互 ${pending.length} 项（ask 审批/选择等——目标会话在等待决策；无人值守会话会挂起至超时）` : ""
      return { ...(await truncate(`${head}${tailBlock}${pendingBlock}`, "session", ctx)), data: { running, messageCount: target.messages.length } }
    }

    // ---- send：注入用户消息（触发执行；注入即返回）----
    if (action === "send") {
      const message = String(args.message ?? "").trim()
      if (!message) return { output: "send 需要 message（注入的用户消息）。" }
      if (svc.engine.isRunning(sessionId)) {
        return {
          output: `会话 ${sessionId} 有任务在运行——注入被拒绝（同会话并发 run 竞态）。可先 action=stop 终止当前任务再注入；或等它跑完（status 轮询）后重试。`,
          data: { running: true },
        }
      }
      // 审批姿态缺省免审：本工具派出的会话轮次都是无人值守的活，无人守审批卡——按实时审批
      // 运行会让需审批工具挂到超时（每层重活都中道而废），服务模式下更直接被拒。语义与
      // task 型无人值守执行同一授权面（任务经用户审批创建，执行即其授权的免审落地）。
      // 显式 approve=false 收紧为 deny（无交互通道下需审批工具即时报拒绝，不空等超时）。
      const approve = args.approve !== false
      // 交互模式显式给出（缺省 realtime 与引擎缺省同值）：无人值守会话的 ask 类工具若仍按实时通道
      // 运行，会挂起到交互超时（审批已免审，但选择/填值/计划审批仍是阻塞点）。
      const interactionMode = args.interactive === false ? ("none" as const) : ("realtime" as const)
      // 脱管注入：不等待目标会话执行完（那是目标会话事件循环的事）——只同步等待启动早期错误
      //（会话缺失/存储异常等同步段失败，约 2 秒窗口），之后立即放行（目标会话后台自行驱动）。
      // 否则控制会话派发长任务 worker 时会被 run 的 Promise 拖住，控制平面自身被锁死。
      const started = svc.run(sessionId, ctx.user, message, { autoApprove: approve, interactionMode })
      const early = await Promise.race([started.then(() => "ok"), new Promise<string>((r) => setTimeout(() => r("bg"), 2000))])
      if (early === "ok") {
        // run 已完整结束（消息极短任务/目标会话立即出错）——若 throw 则如实上抛
        return {
          output: `消息已注入会话 ${sessionId}${target.name ? `「${target.name}」` : ""}${approve ? "（本轮免审）" : "（本轮按需审批）"}，执行已完成（目标会话本次运行已结束）。用 action=status 查看。`,
          data: { running: false },
        }
      }
      return {
        output: `消息已注入会话 ${sessionId}${target.name ? `「${target.name}」` : ""}${approve ? "（本轮免审：需审批工具自动通过）" : "（本轮按需审批：无交互通道下需审批工具直接拒绝）"}，执行已触发（后台运行——本调用不等待完成）。用 action=status 跟踪进展与消息尾部。`,
        data: { running: true },
      }
    }

    // ---- stop：终止运行中的任务（软终止）----
    if (action === "stop") {
      if (!svc.engine.isRunning(sessionId)) {
        return { output: `会话 ${sessionId} 无运行中任务（无需终止）。`, data: { running: false } }
      }
      svc.engine.cancel(sessionId)
      return {
        output: `终止指令已下达（会话 ${sessionId}）——解开全部挂起等待（审批/选择/渲染），任务循环将在当前工具调用收尾后退出；已产生的消息均已落盘，会话保留可继续注入。用 action=status 确认退出。`,
        data: { running: false },
      }
    }

    // ---- delete：删除（进 GC 归档）----
    if (action === "delete") {
      if (svc.engine.isRunning(sessionId)) {
        return { output: `会话 ${sessionId} 有任务在运行——先 action=stop 终止再删除（删除运行中会话会留下孤儿任务态）。`, data: { running: true } }
      }
      await svc.store.delete(sessionId, ctx.user)
      return { output: `会话 ${sessionId} 已删除（进 GC 归档，7 天内可由管理员 restore；会话数据与 tmp 附件一并归档）。`, data: {} }
    }

    return { output: `未知 action: ${action}（可用：create/list/status/send/stop/delete）` }
  },
}

export const globalTools: GlobalToolEntry[] = [{ name: "session", tool: sessionTool }]
