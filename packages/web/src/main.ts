import { uuid } from "./uuid"
import type { ChatChunk, ContentBlock, DiagramFormat, PendingInteraction, SessionInfo, SubAgentInfo, TodoItem } from "@gebai/sdk"
import "./css/base.css"
import "./css/chat.css"
import "./css/composer.css"
import "./css/overlays.css"
import { restoreToken, bindAuth, showLogin, tryExternalAuth } from "./auth"
import { capturePage } from "./capture"
import { bindApprovalSkip, applyApprovalSkip } from "./approval-skip"
import { bindMinimalMode, applyMinimalMode, syncMinimalModeFromServer } from "./minimal-mode"
import { autosize, bindComposer, bindInputBehavior, recordInput, syncSendButton, takeInterruptNext } from "./composer"
import { bindSettings } from "./settings"
import { bindMiniTools } from "./mini-tools"
import { bindWheel } from "./wheel"
import { loadLocalEnv } from "./env-local"
import { bindThemePop, initTheme } from "./theme"
import { initThemeFx } from "./theme-fx"
import { cnyCatTurnEnd, initCnyCat } from "./cny-cat"
import { initLowPower } from "./low-power"
import { initTurnTimer, isTurnTimerEnabled } from "./turn-timer"
import { initFileDisplay } from "./file-display"
import { bindSessionActions, enterDraftView, exportSession, hideEmptyState, loadMessages, maybeAutoTitle, refreshSessions, setRunningAttach, updateSessionCtx } from "./sessions"
import { addApproval, clearApprovals } from "./approvals"
import {
  addMetaActions,
  appendCompactNotice,
  appendMsg,
  appendPlanCard,
  appendTodoCard,
  appendToolResult,
  assistantContent,
  bindMessagesSessions,
  clearInteractionCards,
  finishSessionRun,
  reasoningBlock,
  renderChoiceCard,
  renderEnvRequestCard,
  scrollSessionSticky,
  sealBlockResultSegment,
  sealSegment,
  sealSessionSegment,
  sessionRunBox,
} from "./messages"
import { sendPending } from "./attachments"
import { isBlockOnly, loadToolCardMeta, shortToolName } from "./tool-cards"
import { lockToBottom, noteIncoming, refreshJumpBottom, scrollIfSticky } from "./jump-bottom"
import { scrollReasoningSticky } from "./reasoning-scroll"
import { bindMsgNav } from "./msg-nav"
import { drainQueue, enqueueFront, enqueueInput, setQueueExecutor, type QueuedInput } from "./queue"
import {
  client,
  compactBtn,
  composer,
  el,
  exportBtn,
  focusInput,
  getCurrentSession,
  headerCtxEl,
  input,
  isDraftView,
  lastSessionId,
  msgEl,
  pendingFiles,
  pendingTools,
  pendingToolsKey,
  clearPendingTools,
  runs,
  setConn,
  setCurrentSession,
  setMaxCtxTokens,
  setSubAgentNames,
  syncConnThinking,
  todoState,
  turnTimerEl,
  type RunState,
  type SessionRunState,
} from "./state"
import { blockText, markdownBlock } from "./markdown"
import { renderDiagramSvg } from "./diagram"
import { bindTooltips, confirmDialog } from "./ui"

/* ---------- 压缩入口 ---------- */

compactBtn.onclick = async () => {
  const cur = getCurrentSession()
  if (!cur) return
  if (!(await confirmDialog({ title: "压缩上下文", text: "压缩当前会话上下文？（最早的历史消息将合并为摘要）", danger: false }))) return
  try {
    await client.compactSession(cur.id)
    await loadMessages(cur.id)
  } catch (err) {
    setConn(`压缩失败: ${(err as Error).message}`, false)
  }
}

/* ---------- 会话导出 ---------- */

exportBtn.onclick = async () => {
  const cur = getCurrentSession()
  if (!cur) return
  try {
    await exportSession(cur.id)
  } catch (err) {
    setConn(`导出失败: ${(err as Error).message}`, false)
  }
}

/* ---------- 主流程：发送消息（WS 通道） ---------- */

/** 交互等待（选择/填值/画图/捕获）期间刷新流活跃时间：这类事件不产生 ChatChunk，
 *  空闲超时兜底只应按「无任何数据」判定，不应误杀等待用户回应的挂起。 */
function touchRunActivity(sessionId: string): void {
  const run = runs.get(sessionId)
  if (run) run.lastActivity = Date.now()
}

/** 结构化事件处理（WS onEvent 推送；payload 字段与 event.* 一致）。 */
function onApprovalRequest(ev: { sessionId: string; toolCallId: string; tool: string }) {
  // 审批绑定来源会话：卡片仅随当前会话显示（切走隐藏、切回恢复）；当前会话有审批时锁定输入
  addApproval(ev.sessionId, ev.toolCallId, ev.tool)
  // 审批等待同样刷新活跃时间：服务端审批超时 5 分钟 > 前端空闲看门狗 150s，
  // 不刷新会在用户思考超 150s 时被看门狗误杀任务（选择/填值/画图/捕获均已刷新，此前漏了审批）
  touchRunActivity(ev.sessionId)
}
function onTodoUpdate(ev: { sessionId: string; todos: TodoItem[] }) {
  // 待办状态更新（任意会话，后台也记录）
  todoState.set(ev.sessionId, ev.todos ?? [])
}
function onChoiceRequest(ev: { sessionId: string; prompt: string; options: Array<string | Record<string, unknown>>; choiceId: string; multi?: boolean }) {
  // 选择卡片：渲染到审批容器（随会话显示/隐藏，切走不丢、切回恢复），点击/输入/拒绝提交决策（ask 选项询问分支阻塞等待）
  touchRunActivity(ev.sessionId)
  noteIncoming()
  renderChoiceCard(String(ev.prompt ?? ""), ev.options ?? [], String(ev.choiceId ?? ""), ev.sessionId, ev.multi === true)
}
function onEnvRequest(ev: { sessionId: string; envId: string; name: string; description?: string; secret?: boolean }) {
  // 环境变量填值卡片：渲染到审批容器（随会话显示/隐藏），用户填值提交后保存到浏览器本地并回传引擎（ask 填值分支阻塞等待）
  touchRunActivity(ev.sessionId)
  noteIncoming()
  renderEnvRequestCard(String(ev.name ?? ""), String(ev.description ?? ""), ev.secret === true, String(ev.envId ?? ""), ev.sessionId)
}
function onDrawRender(ev: { sessionId: string; renderId: string; code: string; format?: string }) {
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
function onCaptureRequest(ev: { sessionId: string; captureId: string; fullPage?: boolean; delay?: number }) {
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
function onToolCall(ev: { sessionId: string; toolCallId: string; name: string; arguments?: Record<string, unknown>; sessionRunId?: string }) {
  // 工具事件只渲染当前显示的会话；切走的会话由 loadMessages 兜底（服务端已持久化）
  const cur = getCurrentSession()
  if (ev.sessionId !== cur?.id) return
  const runId = ev.sessionRunId
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
    // 新会话执行过程内的调用：记录卡渲染进该 run 的折叠容器
    if (runId) {
      const sub = runs.get(ev.sessionId)?.sessionRuns?.get(runId)
      if (!sub?.container.isConnected) return
      sealSessionSegment(sub) // 容器内文本分段：问答记录卡处截断当前文本段
      pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId, runId), { session: ev.sessionId, kind: "ask_choice", runId, askArgs })
      scrollSessionSticky(sub.body)
      return
    }
    sealSegment(ev.sessionId) // 文本分段：问答记录卡处截断当前文本段
    pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId), { session: ev.sessionId, kind: "ask_choice", askArgs })
    return
  }
  // ask 计划审批分支：像选项询问一样在消息流中开启计划卡片
  // （展示态，交互作答由审批容器选择卡片承载）；填值分支（name）无专属卡，走通用工具卡（元数据驱动）
  if (short === "ask" && argsObj?.title != null) {
    const args = (argsObj ?? {}) as { title?: unknown; steps?: unknown; content?: unknown }
    const title = String(args.title ?? "").trim()
    if (!title) return
    noteIncoming()
    if (runId) {
      const sub = runs.get(ev.sessionId)?.sessionRuns?.get(runId)
      if (!sub?.container.isConnected) return
      sealSessionSegment(sub) // 容器内文本分段：计划卡片处截断当前文本段
      const wrapper = appendPlanCard(args, sub.body)
      const body = wrapper.querySelector<HTMLElement>(".msg-body")
      if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId, runId), { wrapper, body, session: ev.sessionId, kind: "ask_plan", runId })
      scrollSessionSticky(sub.body)
      return
    }
    sealSegment(ev.sessionId) // 文本分段：计划卡片处截断当前文本段
    const wrapper = appendPlanCard(args)
    const body = wrapper.querySelector<HTMLElement>(".msg-body")
    if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId), { wrapper, body, session: ev.sessionId, kind: "ask_plan" })
    return
  }
  if (isBlockOnly(String(ev.name ?? ""))) return
  noteIncoming()
  // 新会话执行过程内的工具调用：渲染进该 run 的折叠容器（容器缺失=切走场景，由 loadMessages 兜底）
  if (runId) {
    const sub = runs.get(ev.sessionId)?.sessionRuns?.get(runId)
    if (!sub?.container.isConnected) return
    sealSessionSegment(sub) // 容器内文本分段：工具调用处截断当前文本段
    if (short === "todo") {
      const wrapper = appendTodoCard(ev.sessionId, sub.body)
      const body = wrapper.querySelector<HTMLElement>(".msg-body")
      if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId, runId), { wrapper, body, session: ev.sessionId, kind: "todo", runId })
      scrollSessionSticky(sub.body)
      return
    }
    // 调用即建卡（无参数仅头部）：参数先展示、结果到达后追加，不在执行完成后一并出现
    const args = argsObj && Object.keys(argsObj).length > 0 ? JSON.stringify(argsObj, null, 2) : ""
    const wrapper = appendMsg({ id: uuid(), role: "tool", content: args ? `→ ${ev.name} ${args}` : `→ ${ev.name}`, createdAt: Date.now() }, false, sub.body)
    const body = wrapper.querySelector<HTMLElement>(".msg-body")
    if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId, runId), { wrapper, body, session: ev.sessionId, kind: "tool", name: String(ev.name ?? ""), argsText: args, runId })
    scrollSessionSticky(sub.body)
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
function onToolResult(ev: { sessionId: string; toolCallId: string; name: string; output: string; blocks?: ContentBlock[]; sessionRunId?: string }) {
  const cur = getCurrentSession()
  if (ev.sessionId !== cur?.id) {
    // 切走会话时结果不渲染，但配对必须清理：残留会让切回时 loadMessages 按「未完成配对」
    // 重建等待卡，与历史渲染的已完成工具卡重复且永不更新
    if (ev.toolCallId) pendingTools.delete(pendingToolsKey(ev.sessionId, ev.toolCallId, ev.sessionRunId))
    return
  }
  const name = String(ev.name ?? "tool")
  noteIncoming()
  const blocks = ev.blocks as ContentBlock[] | undefined
  if (isBlockOnly(name)) {
    // show/diff 等（card.args="block"）：不显示工具卡片，appendMsg 只渲染内容块（渲染失败/能力受限时显示输出文本）；
    // 追加前封存当前文本段——图表卡片独立展示，画图后的输出另起新卡片（防输出追加到图上方同一张卡片）
    const runId = ev.sessionRunId
    const parent = runId ? runs.get(ev.sessionId)?.sessionRuns?.get(runId)?.body : undefined
    sealBlockResultSegment(ev.sessionId, runId)
    appendMsg({ id: uuid(), role: "tool", name, content: String(ev.output ?? ""), blocks, createdAt: Date.now() }, false, parent)
    return
  }
  const runId = ev.sessionRunId
  const sub = runId ? runs.get(ev.sessionId)?.sessionRuns?.get(runId) : undefined
  const parent = sub?.body
  // ask 选项询问分支：更新消息流中的问答卡片（头部完成态 + 回答；无配对时兜底独立结果消息）
  appendToolResult(ev.sessionId, ev.toolCallId, name, String(ev.output ?? ""), blocks, runId, parent)
  if (sub) scrollSessionSticky(sub.body)
}
function onMessageCompact(ev: { sessionId: string; count: number; summary: string }) {
  // 上下文压缩通知（自动/主动压缩均推送）
  const cur = getCurrentSession()
  if (ev.sessionId !== cur?.id) return
  noteIncoming()
  const count = Number.isFinite(Number(ev.count)) ? Number(ev.count) : 0
  appendCompactNotice(`已压缩 ${count} 条历史消息`, String(ev.summary ?? ""))
}

/** 单个流式 chunk 的渲染处理（直接发送与排队输入自动执行共用的流式渲染管道）。 */
function applyStreamChunk(run: RunState, sessionId: string, chunk: ChatChunk): void {
  if (chunk.kind === "resume") {
    // 断线重连后的全量重同步：重置本轮回累积并重建消息元素，防止内容重复渲染
    run.acc = ""
    run.reasoningAcc = ""
    run.messageId = ""
    run.lastTextKind = undefined
    run.lastTextMsgId = undefined
    run.sessionRuns = undefined // 新会话容器随消息元素一并重建（服务端每轮重推 start，容器会重新创建）
    if (run.el) {
      run.el.classList.remove("streaming")
      run.el.remove()
    }
    run.el = null
    run.reasoningEl = null
    return
  }
  if (chunk.kind === "text") {
    // 模型恢复输出：移除模型服务异常瞬时提示
    if (run.modelErrorEl?.isConnected) clearModelErrorNotice(run)
    if (chunk.messageId) run.messageId = chunk.messageId
    const runId = chunk.sessionRunId
    if (runId) {
      // 新会话执行过程文本：渲染进该 run 的折叠容器（执行中展开，与主回复同流显示）
      let sub = run.sessionRuns?.get(runId)
      if (!sub) {
        // 容器缺失（重连全量重同步清空 sessionRuns 后服务端不重推 start——事件已在断线前投递）：
        // 惰性重建容器兜底，否则该 run 后续输出静默丢弃、容器永久停留旧状态（分支标题等元信息
        // 随 sessionRuns 一并丢失，下一轮 start 重推时容器已存在会被忽略——可接受的降级）
        run.sessionRuns ??= new Map()
        const box = sessionRunBox({ runId, agents: [], input: "" })
        sub = { runId, agents: [], input: "", container: box.container, body: box.body, outputEl: box.outputEl, acc: "", el: null, messageId: "", reasoningAcc: "", reasoningEl: null }
        run.sessionRuns.set(runId, sub)
        scrollIfSticky()
        refreshJumpBottom()
      }
      // 切走/重载中（容器脱离 DOM）：先累积文本（切回由 loadMessages 恢复渲染），与主循环累积语义一致
      if (!sub.container.isConnected) {
        sub.acc += chunk.text ?? ""
        return
      }
      // 新会话新一轮回复（messageId 变化）：封存上一段
      if (chunk.messageId && sub.messageId && chunk.messageId !== sub.messageId) sealSessionSegment(sub)
      if (chunk.messageId) sub.messageId = chunk.messageId
      sub.acc += chunk.text ?? ""
      // 空白内容不渲染：工具调用之间的空文本段不产生空气泡
      if (!sub.acc.trim()) return
      if (!sub.el?.isConnected) {
        sub.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true, sub.body)
      }
      scheduleSessionRender(sub)
      return
    }
    // 新会话输出与主回复分段（换行分隔）：来源切换（主↔新会话）或
    // 新会话新一轮回复（messageId 变化）时封存当前文本段，避免内容一直追加成同一条
    const isSub = chunk.session === true
    if (run.lastTextKind !== undefined) {
      const kindChanged = run.lastTextKind !== (isSub ? "sub" : "main")
      const subRoundChanged = isSub && !!run.lastTextMsgId && !!chunk.messageId && run.lastTextMsgId !== chunk.messageId
      if (kindChanged || subRoundChanged) sealSegment(sessionId)
    }
    run.lastTextKind = isSub ? "sub" : "main"
    if (chunk.messageId) run.lastTextMsgId = chunk.messageId
    run.acc += chunk.text ?? ""
    // 推理完成、正文开始：自动折叠推理块（用户可点 summary 重新展开）
    if (run.reasoningEl?.isConnected && (run.reasoningEl as HTMLDetailsElement).open) (run.reasoningEl as HTMLDetailsElement).open = false
    // 空白内容不渲染：工具调用之间的空文本段不产生空气泡
    if (!run.acc.trim()) return
    // 会话守卫：切到其他会话时只累积不触碰 DOM（切回时由 loadMessages 从 run.acc 恢复渲染）
    if (getCurrentSession()?.id !== sessionId) return
    // 工具调用已封段后（run.el 为 null）或元素脱离 DOM：惰性重建消息元素
    if (!run.el?.isConnected) {
      run.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true)
    }
    scheduleStreamRender(run)
  } else if (chunk.kind === "reasoning") {
    const runId = chunk.sessionRunId
    if (runId) {
      // 新会话执行过程推理：渲染进容器内气泡（与主循环同构的折叠推理块）
      const sub = run.sessionRuns?.get(runId)
      if (!sub) return
      if (!sub.container.isConnected) {
        // 切走/重载中：只累积推理（切回由 loadMessages 恢复），与主循环累积语义一致
        sub.reasoningAcc += chunk.text ?? ""
        return
      }
      sub.reasoningAcc += chunk.text ?? ""
      if (!sub.reasoningAcc.trim()) return
      if (!sub.el?.isConnected) {
        sub.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true, sub.body)
      }
      const bubble = sub.el.querySelector(".msg-body .bubble")
      if (bubble && sub.el.isConnected) {
        if (!sub.reasoningEl?.isConnected) {
          sub.reasoningEl = reasoningBlock()
          bubble.prepend(sub.reasoningEl)
        }
        scheduleSessionRender(sub)
        scrollIfSticky()
        refreshJumpBottom()
      }
      return
    }
    run.reasoningAcc += chunk.text ?? ""
    // 空白推理内容不展示（不创建折叠块）
    if (!run.reasoningAcc.trim()) return
    // 会话守卫：切走时只累积（推理内容不持久化，切回由正文恢复；再流式时重建折叠块）
    if (getCurrentSession()?.id !== sessionId) return
    if (!run.el?.isConnected) {
      run.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true)
    }
    const bubble = run.el.querySelector(".msg-body .bubble")
    if (bubble && run.el.isConnected) {
      if (!run.reasoningEl?.isConnected) {
        run.reasoningEl = reasoningBlock()
        bubble.prepend(run.reasoningEl)
      }
      // 推理内容 markdown 渲染：与正文同走 120ms 尾沿节流渲染路径
      scheduleStreamRender(run)
      if (getCurrentSession()?.id === sessionId) {
        scrollIfSticky()
        refreshJumpBottom()
      }
    }
  } else if (chunk.kind === "session_start") {
    // 新会话 run 开始：创建折叠容器（执行中展开并滚动到可见；服务端每轮重推同 runId start，已存在则忽略）
    // 分支运行（branch_run）容器标题带分支名/模型路由（sessionMeta.branch/model）
    const runId = chunk.sessionRunId ?? ""
    if (!runId || getCurrentSession()?.id !== sessionId) return
    sealSegment(sessionId) // 新会话开始：主文本段在此分段
    run.sessionRuns ??= new Map()
    if (run.sessionRuns.get(runId)?.container.isConnected) return
    const branch = chunk.sessionMeta?.branch ? { name: chunk.sessionMeta.branch, model: chunk.sessionMeta.model } : undefined
    const box = sessionRunBox({ runId, agents: chunk.sessionMeta?.agents ?? [], input: chunk.sessionMeta?.input ?? "", branch })
    run.sessionRuns.set(runId, {
      runId,
      agents: chunk.sessionMeta?.agents ?? [],
      input: chunk.sessionMeta?.input ?? "",
      branch,
      container: box.container,
      body: box.body,
      outputEl: box.outputEl,
      acc: "",
      el: null,
      messageId: "",
      reasoningAcc: "",
      reasoningEl: null,
    })
    scrollIfSticky()
    refreshJumpBottom()
  } else if (chunk.kind === "session_done") {
    // 新会话 run 结束：封存流式文本段，写入最终返回摘要并自动折叠容器（只显示输入与最终返回）
    const runId = chunk.sessionRunId ?? ""
    const sub = run.sessionRuns?.get(runId)
    if (sub) {
      if (sub.el?.isConnected) {
        sub.el.classList.remove("streaming")
        const bubble = sub.el.querySelector<HTMLElement>(".msg-body .bubble")
        if (bubble) addMetaActions(sub.el.querySelector<HTMLElement>(".msg-meta") ?? sub.el, sub.el, bubble, { role: "assistant", content: sub.acc, id: sub.messageId }, { noRevoke: true })
      }
      sealSessionSegment(sub)
      finishSessionRun(sub.container, sub.outputEl, chunk.sessionMeta?.output ?? "")
      run.sessionRuns?.delete(runId)
    }
  } else if (chunk.kind === "model_error") {
    // 模型服务异常（引擎自动重试中）：消息流尾部瞬时提示，非终态——文本恢复时移除
    showModelErrorNotice(run, sessionId, chunk)
  }
}

/** 模型服务异常瞬时提示（重试期间）：单一元素复用更新（重连重放不堆叠），文本恢复/任务结束时移除。 */
function showModelErrorNotice(run: RunState, sessionId: string, chunk: ChatChunk): void {
  if (getCurrentSession()?.id !== sessionId) return
  const retry = chunk.retry ? (chunk.maxRetry ? `（第 ${chunk.retry}/${chunk.maxRetry} 次重试）` : `（第 ${chunk.retry} 次重试）`) : ""
  const text = `模型服务异常${retry}：${chunk.error ?? ""}，正在自动重试…`
  if (!run.modelErrorEl?.isConnected) {
    run.modelErrorEl = el("div", "model-error-notice")
    msgEl.appendChild(run.modelErrorEl)
    scrollIfSticky()
    refreshJumpBottom()
  }
  run.modelErrorEl.textContent = text
}

/** 移除模型服务异常瞬时提示（模型恢复输出/任务结束时调用）。 */
function clearModelErrorNotice(run: RunState): void {
  run.modelErrorEl?.remove()
  run.modelErrorEl = null
}

/** 单轮耗时展示格式：<1m 整秒；<1h 分+秒；以上时+分。 */
function formatTurnDuration(ms: number): string {
  const s = Math.max(0, Math.floor(ms / 1000))
  if (s < 60) return `${s}s`
  const m = Math.floor(s / 60)
  if (m < 60) return `${m}m ${String(s % 60).padStart(2, "0")}s`
  return `${Math.floor(m / 60)}h ${String(m % 60).padStart(2, "0")}m`
}

/** 单轮计时器：标题栏右侧、上下文占比左侧常驻显示，**随会话视图切换**——当前会话运行中实时走
 *  （图标呼吸），切走不打扰其他会话视图、切回续走；非运行会话显示该会话最后定格时长，无记录隐藏。
 *  运行结束不清除（定格保留），下次运行重启归零。
 *  时长分级变色（<1min 中性 / 1-5min 主题色 / ≥5min 警告色，与上下文圆环分级着色语言一致）；
 *  hover 显示总运行时——该会话每轮净耗时累加（不含轮间空闲），而非距第一条消息的墙钟时间。 */
/** 各会话计时状态：累计净耗时（总运行时 tip）与最后定格时长（切回该会话时恢复显示）。 */
const sessionTimers = new Map<string, { totalMs: number; lastElapsed: number }>()

/** 时长分级（颜色渐进，冷→暖）：<10s 极淡 / 10-30s 中性 / 30s-1m 工具青绿 / 1-3m 主题色 / 3-5m 警告 / ≥5m 危险。 */
function turnDurLevel(ms: number): 0 | 1 | 2 | 3 | 4 | 5 {
  if (ms >= 300_000) return 5
  if (ms >= 180_000) return 4
  if (ms >= 60_000) return 3
  if (ms >= 30_000) return 2
  if (ms >= 10_000) return 1
  return 0
}

/** 刷新计时显示：文本 + 分级色 + 运行态；信号灯思考闪烁周期同步分级（越久越慢——闪得慢=执行得慢）。 */
function renderTurnTimer(elapsedMs: number, running: boolean): void {
  const dur = String(turnDurLevel(elapsedMs))
  turnTimerEl.dataset.dur = dur
  if (headerCtxEl) {
    if (running) headerCtxEl.dataset.dur = dur
    else delete headerCtxEl.dataset.dur // 非运行态：信号灯恢复默认闪烁周期
  }
  turnTimerEl.classList.toggle("running", running)
  ;(turnTimerEl.querySelector<HTMLElement>(".tt-text")!).textContent = formatTurnDuration(elapsedMs)
}

function turnTimerTick(run: RunState): void {
  if (!isTurnTimerEnabled()) {
    if (run.timerInterval) {
      clearInterval(run.timerInterval)
      run.timerInterval = undefined
    }
    return
  }
  // 显示跟随当前会话：后台会话运行不打扰当前视图（interval 空转，切回时接管显示）
  if (getCurrentSession()?.id !== run.sessionId) return
  const elapsed = Date.now() - run.startedAt
  turnTimerEl.hidden = false
  renderTurnTimer(elapsed, true)
  turnTimerEl.dataset.tip = `总运行 ${formatTurnDuration((sessionTimers.get(run.sessionId)?.totalMs ?? 0) + elapsed)}` // 含进行中这轮
}

function startTurnTimer(run: RunState): void {
  if (!isTurnTimerEnabled()) return
  turnTimerTick(run)
  run.timerInterval = setInterval(() => turnTimerTick(run), 250)
}

function stopTurnTimer(run: RunState): void {
  if (run.timerInterval) {
    clearInterval(run.timerInterval)
    run.timerInterval = undefined
  }
  const elapsed = Date.now() - run.startedAt
  const st = sessionTimers.get(run.sessionId) ?? { totalMs: 0, lastElapsed: 0 }
  st.totalMs += elapsed // 每段累加：总运行时 = 该会话各轮净耗时之和（不含轮间空闲）
  st.lastElapsed = elapsed
  sessionTimers.set(run.sessionId, st)
  // 定格仅在当前会话显示（切走会话的定格不覆盖当前视图，切回时经 sessionTimers 恢复）
  if (getCurrentSession()?.id === run.sessionId) {
    renderTurnTimer(elapsed, false) // 结束不清除：定格保留
    turnTimerEl.dataset.tip = `总运行 ${formatTurnDuration(st.totalMs)}`
  }
}

/** 会话视图切换联动（gebai:session-view）：计时显示随会话切换——当前会话运行中续走（interval
 *  到点接管刷新，此处先行渲染一帧），非运行会话恢复该会话定格时长，无记录隐藏。 */
function syncTurnTimerView(): void {
  if (!isTurnTimerEnabled()) return // CSS data-turn-timer="off" 隐藏全部计时元素，无需渲染
  const cur = getCurrentSession()
  if (!cur) {
    turnTimerEl.hidden = true // 草稿页（无会话）：无计时展示
    if (headerCtxEl) delete headerCtxEl.dataset.dur
    return
  }
  const run = runs.get(cur.id)
  if (run) {
    const elapsed = Date.now() - run.startedAt
    turnTimerEl.hidden = false
    renderTurnTimer(elapsed, true)
    turnTimerEl.dataset.tip = `总运行 ${formatTurnDuration((sessionTimers.get(cur.id)?.totalMs ?? 0) + elapsed)}`
    return
  }
  const st = sessionTimers.get(cur.id)
  if (st) {
    turnTimerEl.hidden = false
    renderTurnTimer(st.lastElapsed, false)
    turnTimerEl.dataset.tip = `总运行 ${formatTurnDuration(st.totalMs)}`
  } else {
    turnTimerEl.hidden = true
    if (headerCtxEl) delete headerCtxEl.dataset.dur // 清运行态闪烁分级残留
  }
}
document.addEventListener("gebai:session-view", syncTurnTimerView)

/** 空闲超时兜底（无数据视为挂起的判定窗口，毫秒）：高于服务端 LLM 读空闲超时（120s）——
 *  模型调用假死先由服务端超时上报明确错误（「模型接口读超时」），前端看门狗只兜底
 *  服务端也检测不到的挂起；此前 60s 先于服务端超时静默取消，慢模型（长思考无流式输出）
 *  被误杀且用户看不到任何原因。 */
const IDLE_TIMEOUT_MS = 150_000

/** 任务无任何可见输出时的收尾说明气泡（静默结束兜底：用户始终能看到「为什么没有回复」）。 */
function appendFinalNotice(sessionId: string, text: string): void {
  if (getCurrentSession()?.id !== sessionId) return
  const wrapper = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() })
  const bubble = wrapper.querySelector<HTMLElement>(".msg-body .bubble")
  if (bubble) {
    bubble.appendChild(blockText(text))
    addMetaActions(wrapper.querySelector<HTMLElement>(".msg-meta") ?? wrapper, wrapper, bubble, { role: "assistant", content: text, id: uuid() }, { noRevoke: true })
  }
}

/**
 * 运行一个任务流并渲染（直接发送与排队输入自动执行共用）：
 * 建立运行态与空闲看门狗，迭代 ChatChunk 流式渲染，收尾统一清理（运行态/审批卡/配对/焦点），
 * 收尾后自动发送下一条排队输入（会话输入队列）。
 * makeSource 以运行态的 abort 信号构造流（信号由空闲超时兜底触发中止）。
 */
async function consumeTaskStream(sessionId: string, makeSource: (run: RunState) => AsyncIterable<ChatChunk>, opts?: { startedAt?: number }): Promise<void> {
  const run: RunState = { sessionId, acc: "", el: null, reasoningAcc: "", reasoningEl: null, messageId: "", lastActivity: Date.now(), startedAt: opts?.startedAt ?? Date.now(), abort: new AbortController() }
  runs.set(sessionId, run)
  startTurnTimer(run) // 单轮计时器：本轮耗时实时显示（外观 tab 可关）
  syncConnThinking() // 运行开始：信号灯闪烁
  syncSendButton() // 不禁用按钮：运行中点击 = 停止（stopping 拦截）
  const source = makeSource(run)
  // 空闲超时兜底：流 IDLE_TIMEOUT_MS 无任何数据视为挂起（服务端/网络异常），中断并清理，
  // 防止运行态/信号灯残留；交互等待（选择/填值/画图/捕获）由 touchRunActivity 刷新活跃时间，
  // 等待用户回应的挂起不算无数据
  const idleTimer = setInterval(() => {
    if (Date.now() - run.lastActivity > IDLE_TIMEOUT_MS && !run.abort.signal.aborted) {
      run.idleTimedOut = true // 标记：收尾时给出显式提示（此前静默取消，用户无从得知原因）
      run.abort.abort()
      void client.cancelTask(sessionId).catch(() => {})
    }
  }, 10_000)
  try {
    // 文本/推理增量与审批/工具调用/结果等结构化事件统一走 WS（sendPrompt 内部订阅 event.*
    // 并转换为 ChatChunk）；WS 断开时迭代抛错（流中断），由 catch 分支渲染错误
    for await (const chunk of source) {
      run.lastActivity = Date.now()
      if (chunk.kind === "error") {
        if (chunk.error === "cancelled") {
          // 任务被取消：手动停止/审批拒绝/中断插入为有意为之，静默收尾；
          // 空闲超时兜底的取消非用户意图且无任何输出时，给出显式说明（有输出则保留内容静默）
          if (run.idleTimedOut && !run.el?.isConnected && !run.reasoningEl?.isConnected && !run.acc.trim()) {
            appendFinalNotice(sessionId, "生成超时：长时间未收到数据（模型响应过慢或连接中断），任务已中止。请重试或检查网络/模型配置。")
          }
          return
        }
        // 服务端任务失败（LLM 接口错误等）：与 catch 分支一致渲染错误气泡
        throw new Error(chunk.error || "任务失败")
      }
      applyStreamChunk(run, sessionId, chunk)
      // 单轮完成庆祝（人民币主题招财猫爆金币，运行越久爆得越多）；错误/取消路径不庆祝
      if (chunk.kind === "done") cnyCatTurnEnd(Date.now() - run.startedAt)
    }
    // 推理后无正文直接结束（如纯工具链）：兜底折叠推理块。
    // 不依赖 run.el 连接状态：最后一次工具调用封段后 run.el 为 null，但推理块仍在 DOM
    if (run.reasoningEl?.isConnected && (run.reasoningEl as HTMLDetailsElement).open) (run.reasoningEl as HTMLDetailsElement).open = false
    if (run.el?.isConnected) {
      run.el.classList.remove("streaming")
      const bubble = run.el.querySelector<HTMLElement>(".msg-body .bubble")
      if (bubble) addMetaActions(run.el.querySelector<HTMLElement>(".msg-meta") ?? run.el, run.el, bubble, { role: "assistant", content: run.acc, id: run.messageId })
    } else if (!run.reasoningEl?.isConnected && !run.acc.trim() && !run.reasoningAcc.trim()) {
      // 任务正常结束却无任何可见输出（接口返回空回复等异常形态）：显式提示，不留「无声无息就结束」的悬念
      appendFinalNotice(sessionId, "任务已结束，但没有收到任何回复内容。请重试；若反复出现请检查模型配置。")
    }
  } catch (err) {
    clearStreamRender(run) // 错误路径：作废低性能节流排期（防补渲覆盖错误气泡）
    if (run.el?.isConnected) {
      run.el.classList.remove("streaming")
      const bubble = run.el.querySelector<HTMLElement>(".msg-body .bubble")
      if (bubble) {
        // 空闲超时主动中断且回答已有内容：内容完整，静默收尾不渲染错误气泡；否则提示超时/错误
        if (!(run.abort.signal.aborted && run.acc.trim())) {
          const msg = run.abort.signal.aborted ? "生成超时，请重试" : `错误: ${(err as Error).message}`
          if (run.acc.trim()) {
            // 已有部分输出后失败：保留已生成内容，错误说明追加其后（不覆盖）
            const notice = el("div", "msg-error-notice")
            notice.appendChild(blockText(msg))
            bubble.appendChild(notice)
          } else {
            bubble.innerHTML = ""
            bubble.appendChild(blockText(msg))
          }
          // 错误路径的部分输出未持久化（无落点消息），不提供撤回
          addMetaActions(run.el.querySelector<HTMLElement>(".msg-meta") ?? run.el, run.el, bubble, { role: "assistant", content: run.acc || msg, id: run.messageId }, { noRevoke: true })
        }
      }
    } else {
      // 无任何输出即失败（首条内容到达前的接口错误/断连）：此前静默结束，补渲染错误气泡说明原因
      const msg = run.abort.signal.aborted ? "生成超时，请重试" : `错误: ${(err as Error).message}`
      appendFinalNotice(sessionId, msg)
    }
  } finally {
    clearModelErrorNotice(run) // 任务结束：模型服务异常瞬时提示随流收尾移除
    stopTurnTimer(run) // 任务结束：单轮计时停表定格
    clearInterval(idleTimer) // 空闲超时兜底定时器随流结束清理
    // 流结束：低性能节流排期未到点则同步补渲最后一帧（防末尾文本丢失）；错误路径已清排期，不重复渲染
    if (run.renderTimer) {
      clearTimeout(run.renderTimer)
      run.renderTimer = undefined
      renderStreamText(run)
    }
    runs.delete(sessionId)
    clearApprovals(sessionId) // 任务结束：该会话残留审批卡片随任务终止失效并解除输入锁定
    clearInteractionCards(sessionId) // 任务结束：选择/环境变量填值卡片随任务终止失效
    clearPendingTools(sessionId) // 任务结束：工具调用配对清理（结果已落盘历史）
    syncConnThinking() // 运行结束：信号灯恢复常亮
    syncSendButton()
    void maybeAutoTitle(sessionId)
    // 队列继续：任务收尾（完成/取消/出错）后自动发送下一条排队输入（运行中为空转）
    drainQueue(sessionId)
    // 焦点守卫：仅当用户仍在发起会话（未切走）时恢复输入焦点，避免后台流结束抢走当前会话光标
    if (getCurrentSession()?.id === sessionId) focusInput()
  }
}

/** 附加请求在途标记（防重复附加）。 */
const attaching = new Set<string>()

/** 待决交互卡片重建（attach 快照 → 既有渲染入口；替换式幂等——同 id 重复推送只保留一张）。 */
function renderPendingInteraction(sessionId: string, it: PendingInteraction): void {
  if (it.type === "approval") addApproval(sessionId, it.toolCallId, it.tool)
  else if (it.type === "choice") renderChoiceCard(it.prompt, it.options, it.choiceId, sessionId, it.multi)
  else if (it.type === "env") renderEnvRequestCard(it.name, it.description, it.secret, it.envId, sessionId)
  else if (it.type === "draw") onDrawRender({ sessionId, renderId: it.renderId, code: it.code, format: it.format ?? "" })
  else if (it.type === "capture") onCaptureRequest({ sessionId, captureId: it.captureId, fullPage: it.fullPage, delay: it.delay })
}

/** 附加运行中会话（DESIGN「运行中会话恢复」，loadMessages 尾部钩子调用）：
 *  页面刷新/切换进入运行中会话时——快照（在途流 + 待决交互）→ 待决卡片重建（审批/选择/填值/
 *  画图/捕获的事件已推送过、本页收不到，不重建则任务干等到超时）→ consumeTaskStream 接管
 *  attachStream（在途文本种子 + 实时续流，与发起页同构渲染：流式消息/工具卡/信号灯/停止按钮/
 *  单轮计时）。未运行或本页已接管（发起/附加过）时 no-op。 */
async function attachRunningSession(sessionId: string): Promise<void> {
  if (runs.has(sessionId) || attaching.has(sessionId)) return
  attaching.add(sessionId)
  try {
    const snap = await client.attachSession(sessionId)
    if (!snap?.running) return
    for (const it of snap.pending ?? []) renderPendingInteraction(sessionId, it)
    await consumeTaskStream(sessionId, (run) => client.attachStream(sessionId, { signal: run.abort.signal }), { startedAt: snap.startedAt })
  } catch {
    /* 附加失败（连接抖动等）：视图保持存储渲染，下次进入会话重试 */
  } finally {
    attaching.delete(sessionId)
  }
}
setRunningAttach(attachRunningSession)

/** 排队输入执行器（队列消化入口，init 注册）：渲染用户消息（同直接发送）并走同一 sendPrompt 任务流。 */
setQueueExecutor(async (sessionId, item: QueuedInput) => {  if (getCurrentSession()?.id === sessionId) {
    lockToBottom() // 发送即锁定粘底（与直接发送一致）
    appendMsg({ id: item.messageId, role: "user", content: item.text, attachments: item.files as Array<{ path: string; mime: string; name: string; size: number }>, createdAt: item.createdAt })
    lockToBottom() // 消息上屏后再落底一次：新消息立即可见（与直接发送一致）
    refreshJumpBottom()
    recordInput(sessionId, item.text)
  }
  // env：发送时点取浏览器本地环境变量（localStorage），随请求临时注入，仅本次任务生效
  await consumeTaskStream(sessionId, (run) => client.sendPrompt(sessionId, item.text, { attachments: item.files, env: loadLocalEnv(), messageId: item.messageId, signal: run.abort.signal }))
})

composer.addEventListener("submit", async (e) => {
  e.preventDefault()
  const interrupt = takeInterruptNext() // Ctrl+Enter：中断插入（运行中取消当前循环后立即执行）
  const cur = getCurrentSession()
  const text = input.value.trim()
  if (!text && pendingFiles.length === 0) return
  let sessionId: string
  if (cur) {
    sessionId = cur.id
  } else {
    // 草稿态（空白页）：首条消息发送时才创建会话（新会话懒创建，避免落盘空会话堆积）
    let s
    try {
      s = await client.createSession()
    } catch (err) {
      setConn(`创建会话失败: ${(err as Error).message}`, false)
      return
    }
    setCurrentSession(s)
    sessionId = s.id
    void refreshSessions() // 列表即时出现新会话（不阻塞发送）
  }
  // 自动审批开关同步会话 env：草稿首条消息创建的会话不经过 loadMessages（applyApprovalSkip 的既有同步点），
  // 每次任务启动前幂等补齐——WS 同连接按序处理，env 写入先于任务请求落地（服务端进程重启丢内存 env 时同样恢复）
  void applyApprovalSkip(sessionId)
  void applyMinimalMode(sessionId) // 极简模式开关同样在任务启动前幂等同步
  input.value = ""
  autosize()
  syncSendButton()
  hideEmptyState()
  // 发送即锁定粘底：用户此前滚走阅读历史时，新消息自动恢复跟随滚动到底
  lockToBottom()
  // 用户消息 id 客户端生成并随请求携带（服务端采用同一 id 持久化），
  // 撤回（truncate 按 id 精确匹配）与反馈定位才能对「当前会话刚发的消息」生效
  const msgId = uuid()
  // 运行中：输入不进消息流，入本地会话输入队列（排队条呈现）——当前任务结束后自动按序发送；
  // Ctrl+Enter（interrupt）插队首并取消当前循环，其收尾后立即执行
  if (runs.has(sessionId)) {
    const attachments = await sendPending(sessionId)
    // 纯附件消息补默认提示词，避免空 prompt 交给 LLM
    const prompt = text || (attachments.length ? "请查看我发送的附件并处理。" : "")
    if (!prompt) return
    const item: QueuedInput = { id: uuid(), text: prompt, files: attachments, messageId: msgId, createdAt: Date.now() }
    if (interrupt) enqueueFront(sessionId, item)
    else enqueueInput(sessionId, item)
    recordInput(sessionId, prompt)
    return
  }
  if (text) {
    appendMsg({ id: msgId, role: "user", content: text, createdAt: Date.now() })
    recordInput(sessionId, text)
  }
  // 用户消息上屏后再落底一次：发送即滚动到底立即可见（不依赖观察器 rAF 时序；此前滚走阅读历史时同样恢复跟随）
  lockToBottom()
  refreshJumpBottom()
  const attachments = await sendPending(sessionId)
  // 纯附件消息补默认提示词，避免空 prompt 交给 LLM
  const prompt = text || (attachments.length ? "请查看我发送的附件并处理。" : "")
  if (!prompt) return
  // env：浏览器本地环境变量（localStorage），随请求临时注入，仅本次任务生效
  await consumeTaskStream(sessionId, (run) => client.sendPrompt(sessionId, prompt, { attachments, env: loadLocalEnv(), messageId: msgId, signal: run.abort.signal }))
})

/** 流式文本渲染：整段累积文本重新走 markdown 解析（低性能模式下节流合并，降频重解析）。
 * 必须惰性重建消息元素场景由调用方保证（run.el 已建）。 */
function renderStreamText(run: RunState): void {
  // 推理块 markdown 渲染：流式推理内容实时更新（低性能模式下与正文合并节流）
  if (run.reasoningEl?.isConnected) {
    const rb = run.reasoningEl.querySelector<HTMLElement>(".reasoning-body")
    if (rb) {
      rb.textContent = ""
      if (run.reasoningAcc.trim()) rb.appendChild(markdownBlock(run.reasoningAcc.trim()))
      scrollReasoningSticky(rb)
    }
  }
  const bubble = run.el?.querySelector(".msg-body .bubble")
  if (!bubble) return
  let textWrap = bubble.querySelector<HTMLElement>(".msg-text")
  if (!textWrap) {
    textWrap = el("div", "msg-text")
    bubble.appendChild(textWrap)
  }
  textWrap.innerHTML = ""
  if (run.acc) textWrap.appendChild(assistantContent(run.acc))
  if (getCurrentSession()?.id === run.sessionId) {
    scrollIfSticky()
    refreshJumpBottom()
  }
}

/** 流式文本渲染按 120ms 尾沿节流（markdown 全量重解析是流式期间最重的 CPU 开销，
 * 逐 chunk 同步渲染在长回答下 O(n²) 全量重解析——所有模式统一节流，消除性能悬崖）。
 * 计时器挂 run 上：封段/结束时随 run 清理。 */
function scheduleStreamRender(run: RunState): void {
  if (run.renderTimer) return // 已排期：本轮仅累积 run.acc，到点一起渲染
  run.renderTimer = setTimeout(() => {
    run.renderTimer = undefined
    renderStreamText(run)
  }, 120)
}

/** 清空低性能流式渲染排期（封段/重置/结束时调用，防滞留定时器对已封段气泡补渲染）。 */
function clearStreamRender(run: RunState): void {
  if (run.renderTimer) {
    clearTimeout(run.renderTimer)
    run.renderTimer = undefined
  }
}

/** 新会话容器内流式文本渲染：与主循环 renderStreamText 同构（推理块 + 正文 markdown），追加到容器并滚动。 */
function renderSessionStreamText(sub: SessionRunState): void {
  if (sub.reasoningEl?.isConnected) {
    const rb = sub.reasoningEl.querySelector<HTMLElement>(".reasoning-body")
    if (rb) {
      rb.textContent = ""
      if (sub.reasoningAcc.trim()) rb.appendChild(markdownBlock(sub.reasoningAcc.trim()))
      scrollReasoningSticky(rb)
    }
  }
  const bubble = sub.el?.querySelector(".msg-body .bubble")
  if (!bubble) return
  let textWrap = bubble.querySelector<HTMLElement>(".msg-text")
  if (!textWrap) {
    textWrap = el("div", "msg-text")
    bubble.appendChild(textWrap)
  }
  textWrap.innerHTML = ""
  if (sub.acc) textWrap.appendChild(assistantContent(sub.acc))
  scrollSessionSticky(sub.body) // 容器内粘底：用户未上翻时跟随最新内容
  scrollIfSticky()
  refreshJumpBottom()
}

/** 新会话容器内流式文本渲染按 120ms 尾沿节流（与主循环 scheduleStreamRender 同构，所有模式统一）。 */
function scheduleSessionRender(sub: SessionRunState): void {
  if (sub.renderTimer) return
  sub.renderTimer = setTimeout(() => {
    sub.renderTimer = undefined
    renderSessionStreamText(sub)
  }, 120)
}

/* ---------- 初始化 ---------- */

let splashHidden = false
/** 隐藏启动动画画面（init 完成/失败兜底调用）：淡出后移除。 */
function hideSplash(): void {
  if (splashHidden) return
  splashHidden = true
  const splash = document.getElementById("gb-splash")
  if (!splash) return
  splash.classList.add("gb-splash-done")
  window.setTimeout(() => splash.remove(), 400)
}

async function init() {
  initLowPower() // 先于主题：data-low-power 就位后再应用主题（避免切换动画）
  initTurnTimer()
  initFileDisplay() // 文件展示方式（直显/弹窗）跨标签同步；变更时重载当前会话消息
  document.addEventListener("gebai:file-display-change", () => {
    // 渲染是结构性的（文件链接 chip ↔ 内联内容），切换后重载当前会话即时生效
    const cur = getCurrentSession()
    if (cur) void loadMessages(cur.id)
  })
  initTheme()
  initThemeFx() // 各主题画布环境特效（随主题切换/低功耗启停）
  initCnyCat() // 招财猫（cny 主题专属，随主题切换挂载/卸载）
  bindTooltips() // 自定义 tooltip（[data-tip] 全局委托）先于面板绑定
  bindThemePop()
  bindApprovalSkip()
  bindMinimalMode()
  bindMiniTools()
  bindWheel()
  restoreToken()
  await loadToolCardMeta() // 工具卡片展示元数据（titleParams/args 模式），先于历史消息渲染就绪
  // 外部身份扩展点（同源集成）：本地无令牌且服务端启用时，用 URL 参数/宿主 localStorage 的登录态兑换令牌
  await tryExternalAuth()
  bindSettings()
  bindAuth()
  bindComposer()
  bindInputBehavior()
  bindSessionActions()
  bindMessagesSessions(loadMessages)
  bindMsgNav()
  // "跳到最新"按钮的滚动监听已由 jump-bottom.ts 内部绑定，无需重复
  // 先注册事件监听再建连：避免建连期间服务端推送的实时事件（审批/选择/待办等）丢失
  client.onEvent((ev) => {
    if (ev.type === "event.approval.request") {
      onApprovalRequest({ sessionId: ev.sessionId, toolCallId: String(ev.payload.toolCallId ?? ""), tool: String(ev.payload.tool ?? "") })
    } else if (ev.type === "event.todo.update") {
      onTodoUpdate({ sessionId: ev.sessionId, todos: (ev.payload.todos as TodoItem[]) ?? [] })
    } else if (ev.type === "event.choice.request") {
      onChoiceRequest({ sessionId: ev.sessionId, prompt: String(ev.payload.prompt ?? ""), options: (Array.isArray(ev.payload.options) ? ev.payload.options : []) as Array<string | Record<string, unknown>>, choiceId: String(ev.payload.choiceId ?? ""), multi: ev.payload.multi === true })
    } else if (ev.type === "event.env.request") {
      onEnvRequest({ sessionId: ev.sessionId, envId: String(ev.payload.envId ?? ""), name: String(ev.payload.name ?? ""), description: String(ev.payload.description ?? ""), secret: ev.payload.secret === true })
    } else if (ev.type === "event.draw.render") {
      onDrawRender({ sessionId: ev.sessionId, renderId: String(ev.payload.renderId ?? ""), code: String(ev.payload.code ?? ""), format: String(ev.payload.format ?? "") })
    } else if (ev.type === "event.capture.request") {
      onCaptureRequest({ sessionId: ev.sessionId, captureId: String(ev.payload.captureId ?? ""), fullPage: ev.payload.fullPage === true, delay: Number(ev.payload.delay ?? 0) })
    } else if (ev.type === "event.tool.call") {
      onToolCall({ sessionId: ev.sessionId, toolCallId: String(ev.payload.toolCallId ?? ""), name: String(ev.payload.name ?? ""), arguments: ev.payload.arguments as Record<string, unknown> | undefined, sessionRunId: ev.payload.sessionRunId as string | undefined })
    } else if (ev.type === "event.tool.alive") {
      // 长工具执行心跳：阻塞类工具（sh/py 长命令）执行期间无其他数据，据此刷新活跃，防空闲看门狗误取消
      touchRunActivity(ev.sessionId)
    } else if (ev.type === "event.tool.result") {
      onToolResult({ sessionId: ev.sessionId, toolCallId: String(ev.payload.toolCallId ?? ""), name: String(ev.payload.name ?? "tool"), output: String(ev.payload.output ?? ""), blocks: ev.payload.blocks as ContentBlock[] | undefined, sessionRunId: ev.payload.sessionRunId as string | undefined })
    } else if (ev.type === "event.message.compact") {
      onMessageCompact({ sessionId: ev.sessionId, count: Number(ev.payload.count ?? 0), summary: String(ev.payload.summary ?? "") })
    } else if (ev.type === "event.session.ctx") {
      // 运行中上下文大小实时更新（会话列表 k 显示）；缓存命中（接口返回时）随同更新（圆环悬浮展示）
      updateSessionCtx(ev.sessionId, Number(ev.payload.ctxTokens ?? 0), ev.payload.ctxCachedTokens === undefined ? undefined : Number(ev.payload.ctxCachedTokens))
    } else if (ev.type === "event.session.minimal") {
      // 任务中模型经 full_mode 工具（用户批准）切换到完整模式：本地极简开关随之关闭
      if (ev.payload.enabled === false) syncMinimalModeFromServer(false)
    } else if (ev.type === "event.branch.merged") {
      // 分支报告合入主上下文（DESIGN「会话分支运行与合并」）：实时渲染合并气泡（分支过程在折叠容器，
      // 报告气泡即时可见；历史回放由存储中的合并消息承担，含过程存档容器）
      if (getCurrentSession()?.id !== ev.sessionId) return
      sealSegment(ev.sessionId) // 合并消息独立成段（不并入主线在途流式文本）
      appendMsg({
        id: String(ev.payload.messageId ?? uuid()),
        role: "assistant",
        content: String(ev.payload.text ?? ""),
        branchMeta: {
          branchId: String(ev.payload.branchId ?? ""),
          name: String(ev.payload.name ?? ""),
          ...(ev.payload.model ? { model: String(ev.payload.model) } : {}),
        },
        createdAt: Date.now(),
      })
      scrollIfSticky()
      refreshJumpBottom()
    }
  })
  // 连接状态展示 + 自动重连（SDK 内置指数退避；WS 为唯一通道，断开时进行中的流
  // 由 sendPrompt 挂起等待重连恢复（事件日志重放），长断线由空闲超时兜底取消）
  client.onStatusChange((status) => {
    if (status === "connected") setConn("已连接")
    else setConn("已断开，自动重连中…", false)
  })
  // 状态快照（建连/登录/重连后到达）：重连后模型收敛——静默刷新会话列表；
  // 当前会话缺失时恢复服务端当前会话（含 localStorage 记忆，供 init 对齐）
  client.onSnapshot((snap) => {
    // 草稿态（用户主动进入空白页）不被服务端记忆的当前会话覆盖
    if (!getCurrentSession() && !isDraftView() && snap.currentSessionId) {
      const s = snap.sessions.find((x) => x.id === snap.currentSessionId)
      if (s) {
        setCurrentSession(s)
        void loadMessages(s.id).catch(() => {})
      }
    }
    // 模型上下文窗口：标题栏占比显示用（snapshot 与 session.list 均携带）
    setMaxCtxTokens(snap.maxContextTokens ?? 0)
    if (getCurrentSession() || isDraftView()) void refreshSessions(snap.sessions)
  })
  // connect 与 listSubAgents 并行（SDK 内共享同一连接尝试，listSubAgents 复用该连接）
  const connPromise = client.connect()
  const agentsPromise = client.listSubAgents().catch(() => [] as SubAgentInfo[])
  await connPromise
  setConn("已连接")
  // 缓存子Agent 名：工具全名（{agent}_{tool}）解析显示用
  setSubAgentNames((await agentsPromise).map((a) => a.name))
  let sessions: SessionInfo[] = []
  try {
    sessions = await client.listSessions()
    // 恢复上次浏览的会话；没有本地保存或已删除则进入空白草稿页（首条消息发送时才创建，不产生空会话）
    const saved = lastSessionId()
    const target = saved ? sessions.find((s) => s.id === saved) : undefined
    if (target) setCurrentSession(target)
    else enterDraftView()
  } catch (err) {
    // 服务模式未登录 / 无权限：进入登录态
    showLogin((err as Error).message)
    return
  }
  // 会话列表与当前会话消息并行加载（互不依赖；列表复用上面已拉取的结果，避免重复请求）
  const cur = getCurrentSession()
  await Promise.all([refreshSessions(sessions), cur ? loadMessages(cur.id) : Promise.resolve()])
}

init()
  .catch((err) => setConn(`连接失败: ${(err as Error).message}`, false))
  .finally(hideSplash)
