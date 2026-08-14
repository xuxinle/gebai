import { uuid } from "./uuid"
import type { ContentBlock, DiagramFormat, SessionInfo, SubAgentInfo, TodoItem } from "@gebai/sdk"
import "./css/base.css"
import "./css/chat.css"
import "./css/composer.css"
import "./css/overlays.css"
import { restoreToken, bindAuth, showLogin, tryExternalAuth } from "./auth"
import { capturePage } from "./capture"
import { bindApprovalSkip } from "./approval-skip"
import { autosize, bindComposer, bindInputBehavior, recordInput, syncSendButton } from "./composer"
import { bindSettings } from "./settings"
import { bindMiniTools } from "./mini-tools"
import { loadLocalEnv } from "./env-local"
import { bindThemePop, initTheme } from "./theme"
import { initLowPower } from "./low-power"
import { bindSessionActions, exportSession, hideEmptyState, loadMessages, maybeAutoTitle, refreshSessions, updateSessionCtx } from "./sessions"
import { addApproval, clearApprovals } from "./approvals"
import {
  addMetaActions,
  appendAskUserCard,
  appendCompactNotice,
  appendMsg,
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
import {
  client,
  compactBtn,
  composer,
  el,
  exportBtn,
  focusInput,
  getCurrentSession,
  input,
  lastSessionId,
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
}
function onTodoUpdate(ev: { sessionId: string; todos: TodoItem[] }) {
  // 待办状态更新（任意会话，后台也记录）
  todoState.set(ev.sessionId, ev.todos ?? [])
}
function onChoiceRequest(ev: { sessionId: string; prompt: string; options: Array<string | Record<string, unknown>>; choiceId: string; multi?: boolean }) {
  // 选择卡片：渲染到审批容器（随会话显示/隐藏，切走不丢、切回恢复），点击/输入/拒绝提交决策（ask_user 工具阻塞等待）
  touchRunActivity(ev.sessionId)
  noteIncoming()
  renderChoiceCard(String(ev.prompt ?? ""), ev.options ?? [], String(ev.choiceId ?? ""), ev.sessionId, ev.multi === true)
}
function onEnvRequest(ev: { sessionId: string; envId: string; name: string; description?: string; secret?: boolean }) {
  // 环境变量填值卡片：渲染到审批容器（随会话显示/隐藏），用户填值提交后保存到浏览器本地并回传引擎（ask_env 工具阻塞等待）
  touchRunActivity(ev.sessionId)
  noteIncoming()
  renderEnvRequestCard(String(ev.name ?? ""), String(ev.description ?? ""), ev.secret === true, String(ev.envId ?? ""), ev.sessionId)
}
function onDrawRender(ev: { sessionId: string; renderId: string; code: string; format?: string }) {
  // draw 工具执行中：前端按图表语言实时渲染（纯计算，不依赖当前会话视图，后台会话同样执行），成功才回传 ok
  touchRunActivity(ev.sessionId)
  noteIncoming()
  void (async () => {
    try {
      const format = (ev.format === "mermaid" || ev.format === "d2" ? ev.format : "plantuml") as DiagramFormat
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
  // ask_user：像 draw 一样中断当前文本段并在消息流中开启问答输出卡片（展示态，
  // 交互作答由 event.choice.request 选择卡片承载）；card.args="block" 工具（draw/diff/render_html）：
  // 内容块直接渲染，不显示工具卡片
  if (short === "ask_user") {
    const args = (argsObj ?? {}) as { prompt?: unknown; options?: unknown; multi?: unknown }
    const prompt = String(args.prompt ?? "")
    const options = Array.isArray(args.options) ? (args.options as Array<string | Record<string, unknown>>) : []
    const multi = args.multi === true
    if (!prompt && !options.length) return
    noteIncoming()
    // 新会话执行过程内的调用：渲染进该 run 的折叠容器
    if (runId) {
      const sub = runs.get(ev.sessionId)?.sessionRuns?.get(runId)
      if (!sub?.container.isConnected) return
      sealSessionSegment(sub) // 容器内文本分段：问答卡片处截断当前文本段
      const wrapper = appendAskUserCard(prompt, options, multi, sub.body)
      const body = wrapper.querySelector<HTMLElement>(".msg-body")
      if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId, runId), { wrapper, body, session: ev.sessionId, kind: "ask_user", runId })
      scrollSessionSticky(sub.body)
      return
    }
    sealSegment(ev.sessionId) // 文本分段：问答卡片处截断当前文本段
    const wrapper = appendAskUserCard(prompt, options, multi)
    const body = wrapper.querySelector<HTMLElement>(".msg-body")
    if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId), { wrapper, body, session: ev.sessionId, kind: "ask_user" })
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
    if (argsObj && Object.keys(argsObj).length > 0) {
      const args = JSON.stringify(argsObj, null, 2)
      const wrapper = appendMsg({ id: uuid(), role: "tool", content: `→ ${ev.name} ${args}`, createdAt: Date.now() }, false, sub.body)
      const body = wrapper.querySelector<HTMLElement>(".msg-body")
      if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId, runId), { wrapper, body, session: ev.sessionId, kind: "tool", name: String(ev.name ?? ""), argsText: args, runId })
      scrollSessionSticky(sub.body)
    }
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
  // 无参数的工具调用不创建空卡片；结果到达时由 appendToolResult 兜底单独展示
  if (argsObj && Object.keys(argsObj).length > 0) {
    const args = JSON.stringify(argsObj, null, 2)
    const wrapper = appendMsg({ id: uuid(), role: "tool", content: `→ ${ev.name} ${args}`, createdAt: Date.now() })
    const body = wrapper.querySelector<HTMLElement>(".msg-body")
    if (body) pendingTools.set(pendingToolsKey(ev.sessionId, ev.toolCallId), { wrapper, body, session: ev.sessionId, kind: "tool", name: String(ev.name ?? ""), argsText: args })
  }
}
function onToolResult(ev: { sessionId: string; toolCallId: string; name: string; output: string; blocks?: ContentBlock[]; sessionRunId?: string }) {
  const cur = getCurrentSession()
  if (ev.sessionId !== cur?.id) return
  const name = String(ev.name ?? "tool")
  noteIncoming()
  const blocks = ev.blocks as ContentBlock[] | undefined
  if (isBlockOnly(name)) {
    // draw/diff/render_html 等：不显示工具卡片，appendMsg 只渲染内容块（渲染失败/能力受限时显示输出文本）；
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
  // ask_user：更新消息流中的问答卡片（头部完成态 + 回答；无配对时兜底独立结果消息）
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

composer.addEventListener("submit", async (e) => {
  e.preventDefault()
  const cur = getCurrentSession()
  // 运行中提交（Enter）= 停止：与「停止」按钮语义一致，防同会话双流并发写同一 RunState；
  // 空输入 Enter 仅忽略（不取消任务，避免误触停止）
  if (cur && runs.has(cur.id)) {
    if (input.value.trim() || pendingFiles.length) void client.cancelTask(cur.id).catch(() => {})
    return
  }
  const text = input.value.trim()
  if ((!text && pendingFiles.length === 0) || !cur) return
  input.value = ""
  autosize()
  hideEmptyState()
  // 发送即锁定粘底：用户此前滚走阅读历史时，新消息自动恢复跟随滚动到底
  lockToBottom()
  // 用户消息 id 客户端生成并随请求携带（服务端采用同一 id 持久化），
  // 撤回（truncate 按 id 精确匹配）与反馈定位才能对「当前会话刚发的消息」生效
  const msgId = uuid()
  if (text) {
    appendMsg({ id: msgId, role: "user", content: text, createdAt: Date.now() })
    recordInput(cur.id, text)
  }
  const attachments = await sendPending(cur.id)
  // 纯附件消息补默认提示词，避免空 prompt 交给 LLM
  const prompt = text || (attachments.length ? "请查看我发送的附件并处理。" : "")
  if (!prompt) return

  const sessionId = cur.id
  // 不预创建空占位消息：空内容（无文本/推理）时不显示任何消息，首个实质内容到达时惰性创建
  const run: RunState = { sessionId, acc: "", el: null, reasoningAcc: "", reasoningEl: null, messageId: "", lastActivity: Date.now(), abort: new AbortController() }
  runs.set(sessionId, run)
  syncConnThinking() // 运行开始：信号灯闪烁
  syncSendButton() // 不禁用按钮：运行中点击 = 停止（stopping 拦截）
  // 空闲超时兜底：流 60s 无任何数据视为挂起（服务端/网络异常），中断并清理，防止运行态/信号灯残留；
  // 交互等待（选择/填值/画图/捕获）由 touchRunActivity 刷新活跃时间，等待用户回应的挂起不算无数据
  const idleTimer = setInterval(() => {
    if (Date.now() - run.lastActivity > 60_000 && !run.abort.signal.aborted) {
      run.abort.abort()
      void client.cancelTask(sessionId).catch(() => {})
    }
  }, 10_000)
  try {
    // 文本/推理增量与审批/工具调用/结果等结构化事件统一走 WS（sendPrompt 内部订阅 event.*
    // 并转换为 ChatChunk）；WS 断开时迭代抛错（流中断），由 catch 分支渲染错误
    // env：浏览器本地环境变量（localStorage），随请求临时注入，仅本次任务生效
    for await (const chunk of client.sendPrompt(sessionId, prompt, { attachments, env: loadLocalEnv(), messageId: msgId, signal: run.abort.signal })) {
      run.lastActivity = Date.now()
      if (chunk.kind === "error") {
        // 任务被取消（手动停止 / 审批拒绝）：静默收尾，不渲染错误气泡
        if (chunk.error === "cancelled") return
        // 服务端任务失败（LLM 接口错误等）：与 catch 分支一致渲染错误气泡
        throw new Error(chunk.error || "任务失败")
      }
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
        continue
      }
      if (chunk.kind === "text") {
        if (chunk.messageId) run.messageId = chunk.messageId
        const runId = chunk.sessionRunId
        if (runId) {
          // 新会话执行过程文本：渲染进该 run 的折叠容器（执行中展开，与主回复同流显示）
          const sub = run.sessionRuns?.get(runId)
          if (!sub) continue // 容器从未创建（切走期间未渲染）：由 loadMessages 兜底
          // 切走/重载中（容器脱离 DOM）：先累积文本（切回由 loadMessages 恢复渲染），与主循环累积语义一致
          if (!sub.container.isConnected) {
            sub.acc += chunk.text ?? ""
            continue
          }
          // 新会话新一轮回复（messageId 变化）：封存上一段
          if (chunk.messageId && sub.messageId && chunk.messageId !== sub.messageId) sealSessionSegment(sub)
          if (chunk.messageId) sub.messageId = chunk.messageId
          sub.acc += chunk.text ?? ""
          // 空白内容不渲染：工具调用之间的空文本段不产生空气泡
          if (!sub.acc.trim()) continue
          if (!sub.el?.isConnected) {
            sub.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true, sub.body)
          }
          scheduleSessionRender(sub)
          continue
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
        if (!run.acc.trim()) continue
        // 会话守卫：切到其他会话时只累积不触碰 DOM（切回时由 loadMessages 从 run.acc 恢复渲染）
        if (getCurrentSession()?.id !== sessionId) continue
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
          if (!sub) continue
          if (!sub.container.isConnected) {
            // 切走/重载中：只累积推理（切回由 loadMessages 恢复），与主循环累积语义一致
            sub.reasoningAcc += chunk.text ?? ""
            continue
          }
          sub.reasoningAcc += chunk.text ?? ""
          if (!sub.reasoningAcc.trim()) continue
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
          continue
        }
        run.reasoningAcc += chunk.text ?? ""
        // 空白推理内容不展示（不创建折叠块）
        if (!run.reasoningAcc.trim()) continue
        // 会话守卫：切走时只累积（推理内容不持久化，切回由正文恢复；再流式时重建折叠块）
        if (getCurrentSession()?.id !== sessionId) continue
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
        const runId = chunk.sessionRunId ?? ""
        if (!runId || getCurrentSession()?.id !== sessionId) continue
        sealSegment(sessionId) // 新会话开始：主文本段在此分段
        run.sessionRuns ??= new Map()
        if (run.sessionRuns.get(runId)?.container.isConnected) continue
        const box = sessionRunBox({ runId, agents: chunk.sessionMeta?.agents ?? [], input: chunk.sessionMeta?.input ?? "" })
        run.sessionRuns.set(runId, {
          runId,
          agents: chunk.sessionMeta?.agents ?? [],
          input: chunk.sessionMeta?.input ?? "",
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
            if (bubble) addMetaActions(sub.el.querySelector<HTMLElement>(".msg-meta") ?? sub.el, sub.el, bubble, { role: "assistant", content: sub.acc, id: sub.messageId })
          }
          sealSessionSegment(sub)
          finishSessionRun(sub.container, sub.outputEl, chunk.sessionMeta?.output ?? "")
          run.sessionRuns?.delete(runId)
        }
      }
    }
    // 推理后无正文直接结束（如纯工具链）：兜底折叠推理块。
    // 不依赖 run.el 连接状态：最后一次工具调用封段后 run.el 为 null，但推理块仍在 DOM
    if (run.reasoningEl?.isConnected && (run.reasoningEl as HTMLDetailsElement).open) (run.reasoningEl as HTMLDetailsElement).open = false
    if (run.el?.isConnected) {
      run.el.classList.remove("streaming")
      const bubble = run.el.querySelector<HTMLElement>(".msg-body .bubble")
      if (bubble) addMetaActions(run.el.querySelector<HTMLElement>(".msg-meta") ?? run.el, run.el, bubble, { role: "assistant", content: run.acc, id: run.messageId })
    }
  } catch (err) {
    clearStreamRender(run) // 错误路径：作废低性能节流排期（防补渲覆盖错误气泡）
    if (run.el?.isConnected) {
      run.el.classList.remove("streaming")
      const bubble = run.el.querySelector<HTMLElement>(".msg-body .bubble")
      if (bubble) {
        // 空闲超时主动中断且回答已有内容：内容完整，静默收尾不渲染错误气泡；否则提示超时/错误
        if (!(run.abort.signal.aborted && run.acc.trim())) {
          bubble.innerHTML = ""
          const msg = run.abort.signal.aborted ? "生成超时，请重试" : `错误: ${(err as Error).message}`
          bubble.appendChild(blockText(msg))
          addMetaActions(run.el.querySelector<HTMLElement>(".msg-meta") ?? run.el, run.el, bubble, { role: "assistant", content: run.acc || msg, id: run.messageId })
        }
      }
    }
  } finally {
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
    // 焦点守卫：仅当用户仍在发起会话（未切走）时恢复输入焦点，避免后台流结束抢走当前会话光标
    if (getCurrentSession()?.id === sessionId) focusInput()
  }
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
  initTheme()
  bindTooltips() // 自定义 tooltip（[data-tip] 全局委托）先于面板绑定
  bindThemePop()
  bindApprovalSkip()
  bindMiniTools()
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
    } else if (ev.type === "event.tool.result") {
      onToolResult({ sessionId: ev.sessionId, toolCallId: String(ev.payload.toolCallId ?? ""), name: String(ev.payload.name ?? "tool"), output: String(ev.payload.output ?? ""), blocks: ev.payload.blocks as ContentBlock[] | undefined, sessionRunId: ev.payload.sessionRunId as string | undefined })
    } else if (ev.type === "event.message.compact") {
      onMessageCompact({ sessionId: ev.sessionId, count: Number(ev.payload.count ?? 0), summary: String(ev.payload.summary ?? "") })
    } else if (ev.type === "event.session.ctx") {
      // 运行中上下文大小实时更新（会话列表 k 显示）
      updateSessionCtx(ev.sessionId, Number(ev.payload.ctxTokens ?? 0))
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
    if (!getCurrentSession() && snap.currentSessionId) {
      const s = snap.sessions.find((x) => x.id === snap.currentSessionId)
      if (s) {
        setCurrentSession(s)
        void loadMessages(s.id).catch(() => {})
      }
    }
    // 模型上下文窗口：标题栏占比显示用（snapshot 与 session.list 均携带）
    setMaxCtxTokens(snap.maxContextTokens ?? 0)
    if (getCurrentSession()) void refreshSessions(snap.sessions)
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
    // 恢复上次浏览的会话；没有本地保存或已删除则新建会话
    const saved = lastSessionId()
    const target = saved ? sessions.find((s) => s.id === saved) : undefined
    if (target) setCurrentSession(target)
    else setCurrentSession(await client.createSession())
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
