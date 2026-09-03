import { uuid } from "./uuid"
import type { ContentBlock, SessionInfo, SubAgentInfo, TodoItem } from "@gebai/sdk"
import "./css/base.css"
import "./css/chat.css"
import "./css/composer.css"
import "./css/overlays.css"
import { restoreToken, bindAuth, showLogin, tryExternalAuth } from "./auth"
import { bindApprovalSkip, applyApprovalSkip } from "./approval-skip"
import { bindMinimalMode, applyMinimalMode, syncMinimalModeFromServer } from "./minimal-mode"
import { autosize, bindComposer, bindInputBehavior, recordInput, syncSendButton, takeInterruptNext } from "./composer"
import { bindSettings } from "./settings"
import { bindMiniTools } from "./mini-tools"
import { bindWheel } from "./wheel"
import { loadLocalEnv } from "./env-local"
import { bindThemePop, initTheme } from "./theme"
import { initThemeFx } from "./theme-fx"
import { initCnyCat } from "./cny-cat"
import { initLowPower } from "./low-power"
import { initTurnTimer } from "./turn-timer"
import { initFileDisplay } from "./file-display"
import { bindSessionActions, enterDraftView, exportSession, hideEmptyState, loadMessages, maybeAutoTitle, refreshSessions, updateSessionCtx } from "./sessions"
import { appendMsg, bindMessagesSessions, sealSegment } from "./messages"
import { sendPending } from "./attachments"
import { loadToolCardMeta } from "./tool-cards"
import { lockToBottom, refreshJumpBottom, scrollIfSticky } from "./jump-bottom"
import { bindMsgNav } from "./msg-nav"
import { enqueueFront, enqueueInput, setQueueExecutor, type QueuedInput } from "./queue"
import { client, compactBtn, composer, exportBtn, getCurrentSession, input, isDraftView, lastSessionId, pendingFiles, runs, setConn, setCurrentSession, setMaxCtxTokens, setSubAgentNames } from "./state"
import { onApprovalRequest, onTodoUpdate, onChoiceRequest, onEnvRequest, onDrawRender, onCaptureRequest, onToolCall, onToolResult, onMessageCompact, touchRunActivity } from "./events"
import { consumeTaskStream } from "./stream"
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
  // 发送即自动命名（不等首答完成）：长任务运行期间侧栏/标题栏即可见会话标题
  void maybeAutoTitle(sessionId)
  // env：浏览器本地环境变量（localStorage），随请求临时注入，仅本次任务生效
  await consumeTaskStream(sessionId, (run) => client.sendPrompt(sessionId, prompt, { attachments, env: loadLocalEnv(), messageId: msgId, signal: run.abort.signal }))
})


/* ---------- 初始化 ---------- */

/** 隐藏启动动画画面（init 完成/失败兜底调用）：淡出后移除。 */
let splashHidden = false
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
      onChoiceRequest({ sessionId: ev.sessionId, prompt: String(ev.payload.prompt ?? ""), options: (Array.isArray(ev.payload.options) ? ev.payload.options : []) as Array<string | Record<string, unknown>>, choiceId: String(ev.payload.choiceId ?? ""), multi: ev.payload.multi === true, plan: ev.payload.plan as { title?: unknown; content?: unknown; path?: unknown } | undefined })
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
