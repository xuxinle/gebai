import { GebaiClient } from "@gebai/sdk"
import type { SessionInfo, TodoItem } from "@gebai/sdk"

/* ---------- 客户端与 DOM 引用 ---------- */

export const client = new GebaiClient({ baseUrl: "" })

export const msgEl = document.getElementById("messages")!
export const sessionList = document.getElementById("session-list")!
export const composer = document.getElementById("composer") as HTMLFormElement
export const input = document.getElementById("input") as HTMLTextAreaElement
export const connEl = document.getElementById("conn")!
export const approvalsEl = document.getElementById("approvals")!
export const queueEl = document.getElementById("queue")!
export const newSessionBtn = document.getElementById("new-session") as HTMLButtonElement
export const sidebarToggle = document.getElementById("sidebar-toggle")!
export const aside = document.getElementById("sessions")!
export const attachBtn = document.getElementById("attach-btn") as HTMLButtonElement
export const fileInput = document.getElementById("file-input") as HTMLInputElement
export const attachmentsEl = document.getElementById("attachments")!
export const sendBtn = document.getElementById("send") as HTMLButtonElement
export const themeBtn = document.getElementById("theme-btn")!
export const themePop = document.getElementById("theme-pop")!
export const jumpBottom = document.getElementById("jump-bottom")!
export const msgNav = document.getElementById("msg-nav")!
export const miniToolsBtn = document.getElementById("mini-tools-btn") as HTMLButtonElement
export const exportBtn = document.getElementById("export-btn")!
export const compactBtn = document.getElementById("compact-btn")!
export const settingsBtn = document.getElementById("settings-btn")!
export const logoutBtn = document.getElementById("logout-btn")!
export const loginOverlay = document.getElementById("login-overlay")!
export const loginForm = document.getElementById("login-form") as HTMLFormElement
export const loginUser = document.getElementById("login-user") as HTMLInputElement
export const loginPass = document.getElementById("login-pass") as HTMLInputElement
export const loginPass2 = document.getElementById("login-pass2") as HTMLInputElement
export const loginToggle = document.getElementById("login-toggle") as HTMLButtonElement
export const loginSubmit = document.querySelector<HTMLButtonElement>("#login-form .login-submit")!
export const loginErr = document.getElementById("login-err")!
export const settingsOverlay = document.getElementById("settings-overlay")!
export const settingsTabs = document.getElementById("settings-tabs")!
export const settingsBody = document.getElementById("settings-body")!
export const settingsFoot = document.getElementById("settings-foot")!

/* ---------- 可变状态（跨模块共享） ---------- */

export let currentSession: SessionInfo | null = null
/** 草稿态（空白页、会话未创建）：点击「新会话」进入，首条消息发送时才真正创建会话；
 *  该标志阻止迟到的服务端快照把草稿页覆盖回服务端记忆的当前会话。 */
let draftView = false
/** 是否处于草稿态（当前无会话的空白页）。 */
export function isDraftView(): boolean {
  return draftView
}
/** 记住当前浏览的会话：刷新页面后恢复。 */
const SESSION_KEY = "gebai.ui.session"
export function setCurrentSession(s: SessionInfo | null): void {
  currentSession = s
  draftView = s === null
  if (s) {
    try {
      localStorage.setItem(SESSION_KEY, s.id)
    } catch {
      /* 存储不可用（隐私模式/配额满）时静默忽略，不中断会话切换 */
    }
  }
}
/** 上次浏览的会话 id（刷新恢复用；会话可能已删除，调用方需回退）。 */
export function lastSessionId(): string | null {
  return localStorage.getItem(SESSION_KEY)
}
/** 当前会话 getter（跨模块读取；避免直接引用可变 let）。 */
export function getCurrentSession(): SessionInfo | null {
  return currentSession
}

export let pendingFiles: Array<{ name: string; mime: string; size: number; blob: Blob }> = []
export function setPendingFiles(files: Array<{ name: string; mime: string; size: number; blob: Blob }>): void {
  pendingFiles = files
}

/* ---------- 会话草稿与附件（多会话并行：切换会话保存/恢复，防 A 的草稿/附件串到 B） ---------- */

/** 各会话输入框草稿（切换时保存/恢复）。 */
const drafts = new Map<string, string>()
export function saveDraft(sessionId: string): void {
  drafts.set(sessionId, input.value)
}
export function getDraft(sessionId: string): string | undefined {
  return drafts.get(sessionId)
}
export function clearDraft(sessionId: string): void {
  drafts.delete(sessionId)
}

/** 各会话待发送附件（切换时保存/恢复）；pendingFiles 保持「当前会话附件」语义。 */
export const pendingFilesBySession = new Map<string, Array<{ name: string; mime: string; size: number; blob: Blob }>>()

export let emptyState: HTMLElement | null = null
export function setEmptyState(e: HTMLElement | null): void {
  emptyState = e
}
/** 空状态元素 getter。 */
export function getEmptyState(): HTMLElement | null {
  return emptyState
}

/** 新会话 run 折叠容器状态（agent_run 执行过程）：同 runId 事件共享，执行中展开、结束后折叠只显示输入与最终返回。 */
export interface SessionRunState {
  runId: string
  /** 预加载进新会话的子Agent 列表。 */
  agents: string[]
  input: string
  /** details.session-run 折叠容器（summary=标题+输入/返回摘要，body=执行过程消息区）。 */
  container: HTMLDetailsElement
  /** 容器内消息追加区（.session-body）。 */
  body: HTMLElement
  /** 折叠摘要行「返回」区（.session-output）：执行中显示占位，结束后显示最终返回。 */
  outputEl: HTMLElement
  /** 当前流式文本累积（本轮）。 */
  acc: string
  /** 当前文本气泡（容器内惰性创建）。 */
  el: HTMLElement | null
  messageId: string
  reasoningAcc: string
  reasoningEl: HTMLElement | null
  /** 低性能模式流式渲染节流定时器。 */
  renderTimer?: ReturnType<typeof setTimeout>
}

/** 会话运行状态：切走会话后 SSE 流继续累积，切回时恢复渲染 */
export interface RunState {
  sessionId: string
  acc: string
  el: HTMLElement | null
  reasoningAcc: string
  reasoningEl: HTMLElement | null
  /** 本轮 assistant 消息的真实 id（delta 事件携带；用于反馈/操作绑定）。 */
  messageId: string
  /** 流活跃时间（毫秒）：流式 chunk 与交互等待事件（选择/填值/画图/捕获）到达时刷新，
   *  空闲超时兜底按此判定——等待用户回应的挂起不算无数据，防误杀。 */
  lastActivity: number
  /** 空闲超时兜底已触发（150s 无数据主动中止）：收尾时据此给出显式提示而非静默结束。 */
  idleTimedOut?: boolean
  /** 流中断控制器：空闲超时兜底中断挂起的 SSE（防止运行态/信号灯残留）。 */
  abort: AbortController
  /** 低性能模式流式渲染节流定时器（跨 chunk 合并渲染，未排期时为 undefined）。 */
  renderTimer?: ReturnType<typeof setTimeout>
  /** 当前文本段来源（主循环/新会话执行）：来源切换时封段换行，避免新会话输出一直追加成同一条。 */
  lastTextKind?: "main" | "sub"
  /** 当前文本段所属消息 id（新会话新一轮回复时封段）。 */
  lastTextMsgId?: string
  /** 新会话 run 折叠容器：runId → 状态（agent_run 执行过程，结束后折叠保留在 DOM）。 */
  sessionRuns?: Map<string, SessionRunState>
}

/** 正在流式运行中的会话（多会话后台运行）。 */
export const runs = new Map<string, RunState>()

/** 工具调用配对：会话隔离 key（toolCallId 由 LLM 生成，不保证跨会话唯一）→ 已渲染的调用卡片
 * （tool.result 到达后在同一卡片追加结果；name/argsText 供切回时重建卡片 DOM 引用；runId 区分子Agent 容器内调用）。 */
export const pendingTools = new Map<string, { wrapper: HTMLElement; body: HTMLElement; session: string; kind: "tool" | "todo" | "ask_user" | "plan"; name?: string; argsText?: string; runId?: string }>()

/** 工具调用配对 key：`{sessionId}:{runId}:{toolCallId}`，跨会话不串台；runId 区分主循环与子Agent 容器内调用。 */
export function pendingToolsKey(sessionId: string, toolCallId: string, runId?: string): string {
  return `${sessionId}:${runId ?? ""}:${toolCallId}`
}

/** 任务结束清理：移除该会话全部工具调用配对（结果已落盘历史，残留配对只占内存）。 */
export function clearPendingTools(sessionId: string): void {
  for (const [key, entry] of pendingTools) {
    if (entry.session === sessionId) pendingTools.delete(key)
  }
}

/** 会话待办状态（event.todo.update 维护，渲染待办卡片用）。 */
export const todoState = new Map<string, TodoItem[]>()

/** 已自动命名过的会话（避免重复改名）。 */
export const autoNamed = new Set<string>()

/** 已注册的子Agent 名（工具名解析用：`{agent}_{tool}` → agent + 短名）。 */
let subAgentNames: string[] = []
export function setSubAgentNames(names: string[]): void {
  subAgentNames = [...names].sort((a, b) => b.length - a.length) // 最长前缀优先
}
export function getSubAgentNames(): string[] {
  return subAgentNames
}

/* ---------- 基础工具函数 ---------- */

export function setConn(text: string, ok = true) {
  // 信号灯居于上下文圆环圆心：正常仅圆点（空闲隐藏/思考闪烁，CSS 渲染）；
  // 断开/异常：红点常亮，具体信息走 data-tip（圆心放不下原文字徽章）
  connEl.textContent = ""
  connEl.classList.toggle("bad", !ok)
  if (ok) delete connEl.dataset.tip
  else connEl.dataset.tip = text
  syncCtxSignal()
}

/** 思考信号：任意会话运行（流式生成）中信号灯闪烁；运行结束恢复常亮。 */
export function syncConnThinking() {
  connEl.classList.toggle("thinking", runs.size > 0)
  syncCtxSignal()
}

export function filesContent(sessionId: string, path: string): string {
  return `/api/v1/sessions/${sessionId}/files/content?path=${encodeURIComponent(path)}`
}

export function filesDownload(sessionId: string, path: string): string {
  return `/api/v1/sessions/${sessionId}/files/download?path=${encodeURIComponent(path)}`
}

export const ROLE_NAME: Record<string, string> = { user: "我", assistant: "歌白", tool: "工具" }

export function formatTime(ts: number): string {
  if (!ts) return ""
  const d = new Date(ts)
  const now = new Date()
  const pad = (n: number) => String(n).padStart(2, "0")
  const hm = `${pad(d.getHours())}:${pad(d.getMinutes())}`
  const sameDay = d.toDateString() === now.toDateString()
  if (sameDay) return hm
  const yesterday = new Date(now.getTime() - 86400000)
  if (d.toDateString() === yesterday.toDateString()) return `昨天 ${hm}`
  if (d.getFullYear() === now.getFullYear()) return `${d.getMonth() + 1}月${d.getDate()}日`
  return `${d.getFullYear()}/${d.getMonth() + 1}/${d.getDate()}`
}

export function el<K extends keyof HTMLElementTagNameMap>(tag: K, cls?: string, text?: string): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag)
  if (cls) node.className = cls
  if (text !== undefined) node.textContent = text
  return node
}

/** 聚焦输入框并把光标置于末尾（各交互时机自动调用）。 */
export function focusInput() {
  input.focus()
  input.setSelectionRange(input.value.length, input.value.length)
}

const headerTitleEl = document.getElementById("header-title")
const headerCtxEl = document.getElementById("header-ctx")
const ctxFillEl = document.querySelector<SVGCircleElement>("#header-ctx .ctx-fill")

/** 浏览器 tab 标题固定为「歌白」（不拼接会话名）；标题栏居中会话标题跟随当前会话。 */
export function updateTitle() {
  document.title = "歌白"
  if (headerTitleEl) headerTitleEl.textContent = currentSession ? currentSession.name : ""
  renderHeaderCtx()
}

/** 复制文本到剪贴板（非安全上下文 http/file 回退 execCommand）；返回是否成功（会话右键菜单「复制会话 ID」等用）。 */
export async function copyTextToClipboard(text: string): Promise<boolean> {
  try {
    await navigator.clipboard.writeText(text)
    return true
  } catch {
    const ta = document.createElement("textarea")
    ta.value = text
    ta.style.position = "fixed"
    ta.style.opacity = "0"
    document.body.appendChild(ta)
    ta.select()
    let ok = false
    try {
      ok = document.execCommand("copy")
    } catch {
      ok = false
    }
    ta.remove()
    return ok
  }
}

/** 模型上下文窗口（token）：来自服务端快照/session.list（0=未知/未配置）。 */
let maxCtxTokens = 0
export function setMaxCtxTokens(v: number): void {
  maxCtxTokens = v
  renderHeaderCtx()
}

/**
 * 标题栏上下文占比显示：当前会话 ctxTokens / 模型窗口（SVG 圆环弧线），圆心为连接信号灯（#conn），
 * 具体数值 hover 经 data-tip 展示；切换会话时随 updateTitle 联动；运行中由 event.session.ctx 实时更新。
 * 弧线比例分级着色：<50% 主题色、50-80% 警告色、≥80% 危险色；pathLength=100 下 dashoffset 直接用 100-占比。
 */
export function renderHeaderCtx(): void {
  if (!headerCtxEl || !ctxFillEl) return
  const used = currentSession?.ctxTokens ?? 0
  const hasCtx = used > 0 && maxCtxTokens > 0
  const pct = hasCtx ? Math.min(100, Math.round((used / maxCtxTokens) * 100)) : 0
  headerCtxEl.classList.toggle("no-ring", !hasCtx)
  ctxFillEl.style.strokeDashoffset = String(100 - pct)
  headerCtxEl.classList.toggle("warn", hasCtx && pct >= 50 && pct < 80)
  headerCtxEl.classList.toggle("danger", hasCtx && pct >= 80)
  if (hasCtx) headerCtxEl.dataset.tip = `上下文 ${used.toLocaleString()} / ${maxCtxTokens.toLocaleString()} tokens（${pct}%）`
  else delete headerCtxEl.dataset.tip
  syncCtxSignal()
}

/** 圆环容器可见性：有上下文数据出环；无数据时仅当圆心信号灯需要展示（思考闪烁/断开红点）才保留容器。 */
function syncCtxSignal(): void {
  if (!headerCtxEl) return
  const hasRing = !headerCtxEl.classList.contains("no-ring")
  const signal = connEl.classList.contains("thinking") || connEl.classList.contains("bad")
  headerCtxEl.hidden = !hasRing && !signal
}
