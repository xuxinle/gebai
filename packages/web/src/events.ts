/** WS 事件处理器（自 main.ts 拆分）：审批/选择/填值/画图/捕获请求、工具调用/结果、待办与压缩事件。 */
import type { ContentBlock, DiagramFormat, TodoItem } from "@gebai/sdk"
import { addApproval } from "./approvals"
import { capturePage } from "./capture"
import { noteIncoming } from "./jump-bottom"
import { appendCompactNotice, compactNoticeTitle, appendMsg, appendToolResult, appendTodoCard, renderChoiceCard, renderEnvRequestCard, scrollSessionSticky, sealBlockResultSegment, sealRunSegment, sealSegment, withPane } from "./messages"
import { isBlockOnly, shortToolName } from "./tool-cards"
import { renderDiagramSvg } from "./diagram"
import { client, getCurrentSession, pendingTools, pendingToolsKey, runs, todoState } from "./state"
import { ensureProcessArea, noteToolCall } from "./subsessions"
import { paneOf, type MsgPane } from "./msg-panes"
import { uuid } from "./uuid"

/**
 * 事件该渲染到哪个会话容器：
 * - `undefined`：不渲染（会话未运行且已切走——它没有容器，切回时由 loadMessages 从历史重建）
 * - `MsgPane`：该会话自己的容器（运行中会话，含后台的那个）
 * - `null`：当前会话但容器尚未建立（极短的启动竞态）——按默认路径渲染
 */
function paneForRender(sessionId: string): MsgPane | null | undefined {
  const pane = paneOf(sessionId)
  if (pane) return pane
  return getCurrentSession()?.id === sessionId ? null : undefined
}

/** 交互等待（选择/填值/画图/捕获）期间刷新流活跃时间：这类事件不产生 ChatChunk，
 *  空闲超时兜底只应按「无任何数据」判定，不应误杀等待用户回应的挂起。 */
export function touchRunActivity(sessionId: string): void {
  const run = runs.get(sessionId)
  if (run) run.lastActivity = Date.now()
}

/** 结构化事件处理（WS onEvent 推送；payload 字段与 event.* 一致）。 */
export function onApprovalRequest(ev: { sessionId: string; toolCallId: string; tool: string }) {
  // 审批绑定来源会话：卡片仅随当前会话显示（切走隐藏、切回恢复）；当前会话有审批时锁定输入
  addApproval(ev.sessionId, ev.toolCallId, ev.tool)
  // 审批等待同样刷新活跃时间：服务端审批超时 5 分钟 > 前端空闲看门狗 150s，
  // 不刷新会在用户思考超 150s 时被看门狗误杀任务（选择/填值/画图/捕获均已刷新，此前漏了审批）
  touchRunActivity(ev.sessionId)
}
export function onTodoUpdate(ev: { sessionId: string; todos: TodoItem[] }) {
  // 待办状态更新（任意会话，后台也记录）
  todoState.set(ev.sessionId, ev.todos ?? [])
}
export function onChoiceRequest(ev: { sessionId: string; prompt: string; options: Array<string | Record<string, unknown>>; choiceId: string; multi?: boolean; plan?: { title?: unknown; content?: unknown; path?: unknown } }) {
  // 选择卡片：渲染到审批容器（随会话显示/隐藏，切走不丢、切回恢复），点击/输入/拒绝提交决策（ask 选项询问分支阻塞等待）；
  // plan（计划审批分支）：选择卡内嵌计划全文——审批时直接可见，不依赖消息流位置与滚动状态
  touchRunActivity(ev.sessionId)
  noteIncoming()
  const plan =
    ev.plan && typeof ev.plan === "object"
      ? { title: String(ev.plan.title ?? ""), content: String(ev.plan.content ?? ""), path: String(ev.plan.path ?? "") }
      : undefined
  renderChoiceCard(String(ev.prompt ?? ""), ev.options ?? [], String(ev.choiceId ?? ""), ev.sessionId, ev.multi === true, plan)
}
export function onEnvRequest(ev: { sessionId: string; envId: string; name: string; description?: string; secret?: boolean }) {
  // 环境变量填值卡片：渲染到审批容器（随会话显示/隐藏），用户填值提交后保存到浏览器本地并回传引擎（ask 填值分支阻塞等待）
  touchRunActivity(ev.sessionId)
  noteIncoming()
  renderEnvRequestCard(String(ev.name ?? ""), String(ev.description ?? ""), ev.secret === true, String(ev.envId ?? ""), ev.sessionId)
}
export function onDrawRender(ev: { sessionId: string; renderId: string; code: string; format?: string }) {
  // show 图表分支执行中：前端按图表语言实时渲染（纯计算，不依赖当前会话视图，后台会话同样执行），成功才回传 ok
  touchRunActivity(ev.sessionId)
  noteIncoming()
  void (async () => {
    try {
      // format 原样透传：未知语言由 renderDiagramSvg 显式报错（此处不得归一为 plantuml——
      // echarts 曾被这里吞掉喂给 PlantUML 引擎，报出「PlantUML 渲染错误」误导排查）
      const format = (ev.format || "plantuml") as DiagramFormat
      await renderDiagramSvg(format, String(ev.code ?? ""))
      await client.submitDrawResult(ev.sessionId, String(ev.renderId ?? ""), true)
    } catch (err) {
      await client.submitDrawResult(ev.sessionId, String(ev.renderId ?? ""), false, (err as Error).message)
    }
  })()
}
export function onCaptureRequest(ev: { sessionId: string; captureId: string; fullPage?: boolean; delay?: number }) {
  // page_capture 工具执行中：前端捕获当前页面（页面级操作，与当前会话视图无关，后台会话同样执行）回传
  touchRunActivity(ev.sessionId)
  noteIncoming()
  void (async () => {
    try {
      const cap = await capturePage({ fullPage: ev.fullPage === true, delayMs: ev.delay ?? 0 })
      await client.submitCaptureResult(ev.sessionId, String(ev.captureId ?? ""), cap)
    } catch (err) {
      await client.submitCaptureResult(ev.sessionId, String(ev.captureId ?? ""), { html: "", error: (err as Error).message })
    }
  })()
}
export function onToolCall(ev: { sessionId: string; toolCallId: string; name: string; arguments?: Record<string, unknown>; subSessionId?: string }) {
  // 工具体事件渲染到该会话**自己的**容器（pane）：运行中会话（包括已切走的）都持续维护 DOM，
  // 切回即所见即所得；非运行会话无 pane（paneForRender 返回 null），保持旧行为（由 loadMessages 兜底）
  const pane = paneForRender(ev.sessionId)
  if (pane === undefined) return
  return withPane(pane ?? null, () => renderToolCall(ev))
}

function renderToolCall(ev: { sessionId: string; toolCallId: string; name: string; arguments?: Record<string, unknown>; subSessionId?: string }) {
  const runId = ev.subSessionId
  const short = shortToolName(String(ev.name ?? ""))
  const argsObj = ev.arguments as Record<string, unknown> | undefined
  // ask 选项询问分支：等待期只在审批容器渲染交互选择卡片
  // （event.choice.request 承载），消息流不重复渲染问题预览卡（上下两张同款卡片会被视为重复）；
  // 结果到达时 appendToolResult 落问答记录卡并封段当前文本；
  // card.args="block" 工具（show/diff）：内容块直接渲染，不显示工具卡片
  if (short === "ask" && argsObj?.options != null) {
    const args = (argsObj ?? {}) as { prompt?: unknown; options?: unknown; multi?: unknown }
    const prompt = String(args.prompt ?? "")
    const options = Array.isArray(args.options) ? (args.options as Array<string | Record<string, unknown>>) : []
    const multi = args.multi === true
    if (!prompt && !options.length) return
    noteIncoming()
    const askArgs = { prompt, options, multi }
    // 子会话运行过程内的调用：记录卡渲染进该 run 的过程区（观察面板）
    if (runId) {
      const area = ensureProcessArea(ev.sessionId, runId)
      sealRunSegment(ev.sessionId, runId)
      pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId, runId), { session: ev.sessionId, kind: "ask_choice", runId, askArgs })
      scrollSessionSticky(area)
      return
    }
    sealSegment(ev.sessionId) // 文本分段：问答记录卡处截断当前文本段
    pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId), { session: ev.sessionId, kind: "ask_choice", askArgs })
    return
  }
  // ask 计划审批分支：等待期不渲染消息流计划卡——计划全文由审批容器选择卡内嵌承载
  // （choice.request 携带 plan 载荷；上下两张同款计划卡会被视为重复）；结果到达时
  // appendToolResult 落计划卡（审批结果态，与历史回放同构）；填值分支（name）无专属卡，走通用工具卡（元数据驱动）
  if (short === "ask" && argsObj?.title != null) {
    const args = (argsObj ?? {}) as { title?: unknown; steps?: unknown; content?: unknown }
    const title = String(args.title ?? "").trim()
    if (!title) return
    noteIncoming()
    if (runId) {
      const area = ensureProcessArea(ev.sessionId, runId)
      sealRunSegment(ev.sessionId, runId)
      pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId, runId), { session: ev.sessionId, kind: "ask_plan", runId, planArgs: args })
      scrollSessionSticky(area)
      return
    }
    sealSegment(ev.sessionId) // 文本分段：后续计划卡处截断当前文本段
    pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId), { session: ev.sessionId, kind: "ask_plan", planArgs: args })
    return
  }
  if (isBlockOnly(String(ev.name ?? ""))) return
  noteIncoming()
  // 子会话运行过程内的工具调用：渲染进该 run 的过程区（观察面板）
  if (runId) {
    const area = ensureProcessArea(ev.sessionId, runId)
    sealRunSegment(ev.sessionId, runId)
    noteToolCall(ev.sessionId, runId)
    if (short === "todo") {
      const wrapper = appendTodoCard(ev.sessionId, area)
      const body = wrapper.querySelector<HTMLElement>(".msg-body")
      if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId, runId), { wrapper, body, session: ev.sessionId, kind: "todo", runId })
      scrollSessionSticky(area)
      return
    }
    // 调用即建卡（无参数仅头部）：参数先展示、结果到达后追加，不在执行完成后一并出现
    const args = argsObj && Object.keys(argsObj).length > 0 ? JSON.stringify(argsObj, null, 2) : ""
    const wrapper = appendMsg({ id: uuid(), role: "tool", content: args ? `→ ${ev.name} ${args}` : `→ ${ev.name}`, createdAt: Date.now() }, false, area)
    const body = wrapper.querySelector<HTMLElement>(".msg-body")
    if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId, runId), { wrapper, body, session: ev.sessionId, kind: "tool", name: String(ev.name ?? ""), argsText: args, runId })
    scrollSessionSticky(area)
    return
  }
  sealSegment(ev.sessionId) // 文本分段：工具调用处截断当前文本段
  // todo 工具：渲染待办占位卡片，结果到达后刷新为清单
  if (short === "todo") {
    const wrapper = appendTodoCard(ev.sessionId)
    const body = wrapper.querySelector<HTMLElement>(".msg-body")
    if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId), { wrapper, body, session: ev.sessionId, kind: "todo" })
    return
  }
  // 调用即建卡（无参数仅头部）：参数先展示、结果到达后追加，不在执行完成后一并出现
  const args = argsObj && Object.keys(argsObj).length > 0 ? JSON.stringify(argsObj, null, 2) : ""
  const wrapper = appendMsg({ id: uuid(), role: "tool", content: args ? `→ ${ev.name} ${args}` : `→ ${ev.name}`, createdAt: Date.now() })
  const body = wrapper.querySelector<HTMLElement>(".msg-body")
  if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId), { wrapper, body, session: ev.sessionId, kind: "tool", name: String(ev.name ?? ""), argsText: args })
}
export function onToolResult(ev: { sessionId: string; toolCallId: string; name: string; output: string; blocks?: ContentBlock[]; subSessionId?: string }) {
  const pane = paneForRender(ev.sessionId)
  if (pane === undefined) {
    // 无容器的会话（未运行且已切走）：不渲染，但配对必须清理：残留会让下次 loadMessages
    // 按「未完成配对」重建等待卡，与历史渲染的已完成工具卡重复且永不更新
    if (ev.toolCallId) pendingTools.delete(pendingToolsKey(ev.sessionId, ev.toolCallId, ev.subSessionId))
    return
  }
  return withPane(pane ?? null, () => renderToolResult(ev))
}

function renderToolResult(ev: { sessionId: string; toolCallId: string; name: string; output: string; blocks?: ContentBlock[]; subSessionId?: string }) {
  const name = String(ev.name ?? "tool")
  noteIncoming()
  const blocks = ev.blocks as ContentBlock[] | undefined
  if (isBlockOnly(name)) {
    // show/diff 等（card.args="block"）：不显示工具卡片，appendMsg 只渲染内容块（渲染失败/能力受限时显示输出文本）；
    // 追加前封存当前文本段——图表卡片独立展示，画图后的输出另起新卡片（防输出追加到图上方同一张卡片）
    const runId = ev.subSessionId
    const parent = runId ? ensureProcessArea(ev.sessionId, runId) : undefined
    sealBlockResultSegment(ev.sessionId, runId)
    appendMsg({ id: uuid(), role: "tool", name, content: String(ev.output ?? ""), blocks, createdAt: Date.now() }, false, parent)
    return
  }
  const runId = ev.subSessionId
  const area = runId ? ensureProcessArea(ev.sessionId, runId) : undefined
  // ask 选项询问分支：更新消息流中的问答卡片（头部完成态 + 回答；无配对时兜底独立结果消息）
  appendToolResult(ev.sessionId, ev.toolCallId, name, String(ev.output ?? ""), blocks, runId, area)
  if (area) scrollSessionSticky(area)
}
export function onMessageCompact(ev: { sessionId: string; count: number; summary: string; degraded?: string }) {
  // 上下文压缩 / 溢出护栏降级通知（自动/主动压缩与护栏降级共用本事件；degraded 标识护栏降级类型）
  const cur = getCurrentSession()
  if (ev.sessionId !== cur?.id) return
  noteIncoming()
  const count = Number.isFinite(Number(ev.count)) ? Number(ev.count) : 0
  appendCompactNotice(compactNoticeTitle(count, ev.degraded), String(ev.summary ?? ""))
}

/** 单个流式 chunk 的渲染处理（直接发送与排队输入自动执行共用的流式渲染管道）。 */
