import { uuid } from "./uuid"
import type { ContentBlock, SessionInfo } from "@gebai/sdk"
import {
  aside,
  autoNamed,
  client,
  copyTextToClipboard,
  el,
  focusInput,
  getCurrentSession,
  getEmptyState,
  input,
  msgEl,
  newSessionBtn,
  pendingFiles,
  pendingFilesBySession,
  pendingTools,
  renderHeaderCtx,
  runs,
  sessionList,
  setCurrentSession,
  setEmptyState,
  setPendingFiles,
  sidebarToggle,
  todoState,
  updateTitle,
  saveDraft,
  getDraft,
  clearDraft,
} from "./state"
import { markdownBlock } from "./markdown"
import { appendMsg, appendTodoCard, beginMsgBatch, finishSessionRun, flushMsgBatch, reasoningBlock, renderLegacySubAgentArchive, renderSessionArchive, sessionRunBox } from "./messages"
import { clearUnread, isAtBottom, lockToBottom, restoreScroll } from "./jump-bottom"
import { applyApprovalSkip } from "./approval-skip"
import { applyMinimalMode } from "./minimal-mode"
import { applyApprovalVisibility } from "./approvals"
import { autosize, firstInputOf, resetHistoryNav, syncSendButton } from "./composer"
import { autoHideScrollbar, confirmDialog, desktopDownloadHint, toast } from "./ui"
import { renderShortcutButtons } from "./shortcuts"
import { clearMsgNav, updateMsgNav } from "./msg-nav"
import { renderAttachments } from "./attachments"
import { clearQueue, renderQueue } from "./queue"

export type LoadMessagesFn = (sessionId: string) => Promise<void>

/* 批量选择删除：侧栏顶部操作条（index.html 提供）；入口为每个会话行的选择框（hover 显示，勾选即进入批量模式） */
const batchBar = document.getElementById("batch-bar")!
const batchCountEl = document.getElementById("batch-count")!
const batchDelBtn = document.getElementById("batch-del") as HTMLButtonElement
const batchCancelBtn = document.getElementById("batch-cancel")!
const searchInputEl = document.getElementById("session-search") as HTMLInputElement

/* ---------- 会话加载与列表 ---------- */

/** 消息加载请求序号守卫：快速切换会话（A→B→A）时丢弃迟到的旧请求结果，
 * 防止旧数据覆盖新视图（DOM 清空/渲染是异步链，迟到的 getSession 响应会串台）。 */
let loadSeq = 0

/** 各会话离开时的滚动位置（-1=粘底/无记忆，下次落底；>=0=恢复该位置，阅读位置跨会话保留）。 */
const scrollMemory = new Map<string, number>()

/** 保存当前会话的草稿/附件/滚动位置（切换会话前调用）。 */
function saveSessionViewState(sessionId: string) {
  saveDraft(sessionId)
  pendingFilesBySession.set(sessionId, pendingFiles)
  scrollMemory.set(sessionId, isAtBottom() ? -1 : msgEl.scrollTop)
}

/** 恢复目标会话的草稿/附件（切换会话后调用）。 */
function restoreSessionViewState(sessionId: string) {
  input.value = getDraft(sessionId) ?? ""
  autosize()
  setPendingFiles(pendingFilesBySession.get(sessionId) ?? [])
  renderAttachments()
}

/** 清空会话草稿（新会话创建/会话删除时调用，防残留串台）。 */
export function clearSessionViewState(sessionId: string) {
  clearDraft(sessionId)
  pendingFilesBySession.delete(sessionId)
  scrollMemory.delete(sessionId)
  // 当前会话自身被清（新建会话）：输入框与附件列表同步清空
  if (getCurrentSession()?.id === sessionId) {
    input.value = ""
    autosize()
    setPendingFiles([])
    renderAttachments()
  }
}

export async function loadMessages(sessionId: string) {
  const seq = ++loadSeq
  applyApprovalVisibility() // 审批卡片跟随会话：仅显示当前会话的待审批，切回恢复
  const session = await client.getSession(sessionId)
  if (seq !== loadSeq) return // 已有更新的加载请求：本次结果作废
  msgEl.innerHTML = ""
  clearMsgNav()
  clearUnread()
  setEmptyState(null)
  resetHistoryNav()
  // 先拉取待办状态：历史 todo 卡片渲染使用真实清单
  try {
    const todos = await client.listTodos(sessionId)
    if (seq !== loadSeq) return
    todoState.set(sessionId, todos)
  } catch {
    /* 忽略 */
  }
  void applyApprovalSkip(sessionId) // 自动审批开启时，确保该会话 env 同步
  void applyMinimalMode(sessionId) // 极简模式开关同理（开启/关闭均同步，避免服务端内存态 env 残留旧值）
  // 空内容消息（无 content/blocks/attachments/reasoning）不显示：按渲染后的实际可见消息数判空
  const visible = (session.messages ?? []).filter((m) => m.content || m.blocks?.length || m.attachments?.length || m.reasoning)
  if (visible.length === 0) showEmptyState()
  else hideEmptyState()
  // 运行中的会话状态（新会话容器恢复/流式累积引用；先于消息循环就位）
  const run = runs.get(sessionId)
  // 批量挂载避免逐条触发滚动/重排
  beginMsgBatch()
  try {
    // 新会话执行过程消息（session 标记）：按 runId 分组渲染进折叠容器（默认折叠，只显示输入与最终返回）。
    // 旧版（agent_call 时代）独立存档消息为 subAgent/subAgentRunId/subAgentMeta 字段，兼容回放
    let subRun: { runId: string; container: HTMLDetailsElement; body: HTMLElement; outputEl: HTMLElement; lastMsg?: import("@gebai/sdk").Message } | null = null
    const closeSubRun = () => {
      if (!subRun) return
      // 结束判定：最后一条消息为无 toolCalls 的 assistant（有最终回复）→ 折叠显示返回；
      // 无最终回复（中断/风暴终止）：任务仍在运行 → 保持执行中态；任务已结束 → 折叠显示「（无返回）」
      const last = subRun.lastMsg
      const hasFinal = last?.role === "assistant" && !last.toolCalls?.length
      if (hasFinal) finishSessionRun(subRun.container, subRun.outputEl, last!.content)
      else if (runs.has(sessionId)) finishSessionRun(subRun.container, subRun.outputEl, undefined)
      else finishSessionRun(subRun.container, subRun.outputEl, "")
      subRun = null
    }
    for (const m of visible) {
      const isLegacy = (m as import("@gebai/sdk").Message).subAgent === true
      if (m.session || isLegacy) {
        const runId = m.sessionRunId ?? (m as import("@gebai/sdk").Message).subAgentRunId
        const agents = m.sessionMeta?.agents ?? ((m as import("@gebai/sdk").Message).subAgentMeta?.agent ? [(m as import("@gebai/sdk").Message).subAgentMeta!.agent] : [])
        const input = m.sessionMeta?.input ?? (m as import("@gebai/sdk").Message).subAgentMeta?.input ?? (m.role === "user" ? m.content : "")
        if (!subRun || runId !== subRun.runId) {
          closeSubRun()
          if (runId) {
            // 任务运行中切回：重建容器并恢复 run.sessionRuns 引用与切走期间累积的流式文本
            const existing = run?.sessionRuns?.get(runId)
            const box = sessionRunBox({ runId, agents, input })
            if (run) {
              const reasoningAcc = existing?.reasoningAcc ?? ""
              const acc = existing?.acc ?? ""
              let streamEl: HTMLElement | null = null
              let reasoningEl: HTMLElement | null = null
              if (acc.trim() || reasoningAcc.trim()) {
                streamEl = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true, box.body)
                const bubble = streamEl.querySelector<HTMLElement>(".msg-body .bubble")
                if (bubble) {
                  if (reasoningAcc.trim()) {
                    reasoningEl = reasoningBlock()
                    const rb = reasoningEl.querySelector<HTMLElement>(".reasoning-body")
                    if (rb) rb.appendChild(markdownBlock(reasoningAcc.trim()))
                    bubble.prepend(reasoningEl)
                  }
                  const textWrap = el("div", "msg-text")
                  textWrap.appendChild(markdownBlock(acc))
                  bubble.appendChild(textWrap)
                }
              }
              run.sessionRuns ??= new Map()
              run.sessionRuns.set(runId, {
                runId,
                agents,
                input,
                container: box.container,
                body: box.body,
                outputEl: box.outputEl,
                acc,
                el: streamEl,
                messageId: existing?.messageId ?? "",
                reasoningAcc,
                reasoningEl,
              })
            }
            subRun = { runId, container: box.container, body: box.body, outputEl: box.outputEl }
          }
        }
        if (!subRun) continue
        if (m.role === "user" && m.sessionMeta) continue // 输入已随容器创建渲染（sessionRunBox）
        if (m.role === "user" && isLegacy && (m as import("@gebai/sdk").Message).subAgentMeta) continue
        subRun.lastMsg = m
        appendMsg(m, false, subRun.body)
      } else {
        closeSubRun()
        // 新会话存档：agent_run 工具调用记录扩展字段（sessionRun）→ 先渲染折叠容器（含嵌套递归），
        // 再渲染工具结果卡片（agent_run 输出为 markdown）；旧版 subAgentRun 字段兼容回放
        if (m.sessionRun) renderSessionArchive(m.sessionRun)
        else if ((m as import("@gebai/sdk").Message).subAgentRun) renderLegacySubAgentArchive((m as import("@gebai/sdk").Message).subAgentRun!)
        appendMsg(m)
      }
    }
    closeSubRun()
  } finally {
    flushMsgBatch()
  }
  // 会话加载完成：锁定粘底并滚动到底（粘底状态不跨会话记忆——上会话阅读历史时 stickToBottom=false，
  // 不重置会导致新会话停在旧的中间滚动位置，不会自动落底）
  lockToBottom()
  // 恢复后台运行中的流式消息（切回时会话仍在作答）
  if (run) {
    if (!run.acc.trim()) {
      run.el = null // 累积内容为空白：不渲染占位气泡，后续实质文本再惰性创建
    } else {
      run.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true)
      const bubble = run.el.querySelector(".msg-body .bubble")
      if (bubble) {
        // 推理恢复：切走期间的推理累积在切回时重建（与正文恢复同构，不丢失）
        if (run.reasoningAcc.trim()) {
          run.reasoningEl = reasoningBlock()
          const rb = run.reasoningEl.querySelector<HTMLElement>(".reasoning-body")
          if (rb) rb.appendChild(markdownBlock(run.reasoningAcc.trim()))
          bubble.prepend(run.reasoningEl)
        } else {
          run.reasoningEl = null
        }
        const textWrap = el("div", "msg-text")
        textWrap.appendChild(markdownBlock(run.acc))
        bubble.appendChild(textWrap)
      }
      lockToBottom()
    }
  }
  // 恢复运行中工具调用的卡片 DOM 引用（切回时重建，后续 tool.result 仍能在同一卡片追加；
  // 切走期间已到达的结果由服务端历史消息兜底，本列表仅覆盖未完成配对；子Agent 容器内调用重建到容器）
  // 历史消息中已有结果的 toolCallId 不再重建（切走期间完成的结果已由历史渲染，重建会重复出卡）
  const doneIds = new Set<string>()
  for (const m of (await client.getSession(sessionId)).messages ?? []) {
    if (m.role === "tool" && m.toolCallId) doneIds.add(m.toolCallId)
  }
  for (const [key, entry] of pendingTools.entries()) {
    if (entry.session !== sessionId) continue
    // key 形如 `{sessionId}:{runId}:{toolCallId}`（各段不含 ":"）
    const toolCallId = key.split(":")[2]
    if (toolCallId && doneIds.has(toolCallId)) {
      pendingTools.delete(key)
      continue
    }
    const parent = entry.runId ? run?.sessionRuns?.get(entry.runId)?.body : undefined
    if (entry.kind === "todo") {
      const w = appendTodoCard(sessionId, parent)
      entry.wrapper = w
      entry.body = w.querySelector<HTMLElement>(".msg-body")!
    } else if (entry.name) {
      const w = appendMsg({ id: uuid(), role: "tool", content: `→ ${entry.name}${entry.argsText ? ` ${entry.argsText}` : ""}`, createdAt: Date.now() }, false, parent)
      entry.wrapper = w
      entry.body = w.querySelector<HTMLElement>(".msg-body")!
    }
  }
  updateTitle()
  focusInput()
  syncSendButton() // 按钮跟随当前会话：切到运行中的会话显示「停止」，空闲会话显示「发送」
  updateMsgNav() // 全部消息挂上后再定位
  // 滚动位置恢复：离开时未粘底（阅读历史中）→ 恢复原位置；否则（或新会话）落底。
  // restoreScroll 同步按落位刷新锁定状态并使未决跟随回调（flushMsgBatch 排期的 rAF /
  // lockToBottom 启动的对齐保持循环）失效——直接赋值 scrollTop 会留下 following=true 的
  // 旧状态，未决回调晚于恢复执行时把刚恢复的历史位置拽到底部
  const mem = scrollMemory.get(sessionId)
  if (mem !== undefined && mem >= 0) {
    restoreScroll(mem)
  } else {
    lockToBottom()
  }
  // 运行中会话附加（DESIGN「运行中会话恢复」）：页面刷新/切换进入运行中会话时恢复在途流与
  // 待决交互卡。渲染完成后再附加（存储基线先上屏，在途文本作为流式消息续接其后）
  void runningAttachHook?.(sessionId)
}

/** 运行中会话附加钩子（main.ts 注册实现；null 时无附加能力——单测环境等）。 */
let runningAttachHook: ((sessionId: string) => Promise<void>) | null = null
export function setRunningAttach(fn: (sessionId: string) => Promise<void>): void {
  runningAttachHook = fn
}

/* ---------- 批量选择删除 ---------- */

let batchMode = false
const selected = new Set<string>()
const selectedNames = new Map<string, string>()

function toggleSelect(id: string, li: HTMLElement) {
  if (selected.has(id)) {
    selected.delete(id)
    li.classList.remove("selected")
  } else {
    selected.add(id)
    li.classList.add("selected")
  }
  const box = li.querySelector<HTMLInputElement>(".session-check")
  if (box) box.checked = selected.has(id)
  updateBatchCount()
  // 全部取消 → 退出批量模式
  if (batchMode && selected.size === 0) exitBatch()
}

function updateBatchCount() {
  batchCountEl.textContent = `已选 ${selected.size} 项`
  batchDelBtn.disabled = selected.size === 0
}

/** 批量模式切换后的延迟重渲染（展开/恢复分组折叠）：可取消——批量删除后紧随的正式刷新
 *  必须清掉它，否则迟到的旧列表渲染会因 seq 守卫抢占正式刷新（导致删除后列表不刷新）。 */
let batchRenderTimer: ReturnType<typeof setTimeout> | null = null
function scheduleBatchRender(): void {
  if (batchRenderTimer) clearTimeout(batchRenderTimer)
  batchRenderTimer = setTimeout(() => {
    batchRenderTimer = null
    void refreshSessions(lastSessions ?? undefined)
  }, 0)
}

function enterBatch() {
  batchMode = true
  sessionList.classList.add("batch-mode")
  batchBar.hidden = false
  searchInputEl.disabled = true
  newSessionBtn.disabled = true
  selected.clear()
  updateBatchCount()
  // 延迟重渲染展开全部折叠分组：先让触发勾选的 toggleSelect 在旧 DOM 上完成选中（同步重渲染会
  // 摘除当前 li，toggleSelect 会因 selected 为空误触发 exitBatch 取消批量模式）
  scheduleBatchRender()
}

function exitBatch() {
  batchMode = false
  sessionList.classList.remove("batch-mode")
  batchBar.hidden = true
  searchInputEl.disabled = false
  newSessionBtn.disabled = false
  selected.clear()
  scheduleBatchRender() // 恢复折叠状态
}

/** 会话列表刷新请求序号守卫：快速搜索/切换时丢弃迟到的旧响应。 */
let refreshSeq = 0
/** 最近一次加载的会话列表（组折叠/批量模式切换重渲染时复用，避免重复请求）。 */
let lastSessions: SessionInfo[] | null = null

/* ---------- 会话列表分组（今天/昨天/近7天/更早，组可折叠，状态本地记忆） ---------- */

const GROUP_ORDER = ["today", "yesterday", "week", "older"] as const
type GroupKey = (typeof GROUP_ORDER)[number]
const GROUP_LABEL: Record<GroupKey, string> = { today: "今天", yesterday: "昨天", week: "近7天", older: "更早" }
const GROUP_KEY = "gebai.ui.sessionsCollapsed"

function sessionGroup(ts: number): GroupKey {
  const now = new Date()
  const startOfDay = (x: Date) => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime()
  const diff = Math.floor((startOfDay(now) - startOfDay(new Date(ts))) / 86400000)
  if (diff <= 0) return "today"
  if (diff === 1) return "yesterday"
  if (diff < 7) return "week"
  return "older"
}

function readCollapsedGroups(): string[] {
  try {
    const raw = JSON.parse(localStorage.getItem(GROUP_KEY) ?? "[]")
    return Array.isArray(raw) ? raw.filter((k) => typeof k === "string") : []
  } catch {
    return []
  }
}

let collapsedGroups = new Set<string>(readCollapsedGroups())

function persistCollapsedGroups(): void {
  try {
    localStorage.setItem(GROUP_KEY, JSON.stringify([...collapsedGroups]))
  } catch {
    /* 存储不可用静默忽略 */
  }
}

/** 组头：点击折叠/展开（折叠分组不渲染成员）；批量模式下点击切换全组选中。 */
function appendGroupHeader(key: GroupKey, count: number): void {
  const li = el("li", "session-group")
  li.dataset.group = key
  const collapsed = !batchMode && collapsedGroups.has(key)
  if (collapsed) li.classList.add("collapsed")
  const chevron = el("span", "sg-chevron", "▾")
  li.append(chevron, el("span", "sg-label", GROUP_LABEL[key]), el("span", "sg-count", String(count)))
  li.onclick = () => {
    if (batchMode) {
      // 全组选中/取消（含组头所在行外的成员 li，data-group 匹配）
      const lis = sessionList.querySelectorAll<HTMLElement>(`li[data-sid][data-group="${key}"]`)
      const all = [...lis].every((x) => selected.has(x.dataset.sid!))
      for (const x of lis) {
        const sid = x.dataset.sid!
        if (all) {
          selected.delete(sid)
          x.classList.remove("selected")
        } else {
          selected.add(sid)
          x.classList.add("selected")
        }
        const box = x.querySelector<HTMLInputElement>(".session-check")
        if (box) box.checked = selected.has(sid)
      }
      updateBatchCount()
      return
    }
    if (collapsedGroups.has(key)) collapsedGroups.delete(key)
    else collapsedGroups.add(key)
    persistCollapsedGroups()
    void refreshSessions(lastSessions ?? undefined)
  }
  sessionList.appendChild(li)
}

/** 会话行渲染（组折叠时仅被折叠的组不调用；搜索态平铺调用，groupKey 为空）。
 *  无行内按钮：右键菜单（多选/重命名/删除）+ 双击重命名。 */
function appendSessionLi(s: SessionInfo, groupKey = ""): void {
  selectedNames.set(s.id, s.name)
  const li = document.createElement("li")
  const cur = getCurrentSession()
  li.className = cur?.id === s.id ? "active" : ""
  if (batchMode && selected.has(s.id)) li.classList.add("selected")
  li.dataset.sid = s.id
  if (groupKey) li.dataset.group = groupKey
  li.onclick = async () => {
    if (batchMode) {
      toggleSelect(s.id, li)
      return
    }
    // 点击当前已激活会话：不切换，不做任何动作（不重载消息）
    if (getCurrentSession()?.id === s.id) return
    const prev = getCurrentSession()
    if (prev) saveSessionViewState(prev.id)
    setCurrentSession(s)
    aside.classList.remove("open")
    try {
      await refreshSessions()
      await loadMessages(s.id)
      restoreSessionViewState(s.id)
    } catch (err) {
      // 切换失败（网络抖动等）：回滚当前会话标记并提示，视图保持旧会话，避免状态不一致
      toast(`切换失败: ${(err as Error).message}`, "error")
      if (prev) {
        setCurrentSession(prev)
        restoreSessionViewState(prev.id)
      }
    }
  }
  // 双击重命名（与右键菜单「重命名」等价入口）
  li.ondblclick = (e) => {
    e.stopPropagation()
    if (batchMode) return
    startRename(s, li.querySelector<HTMLElement>(".session-name")!)
  }
  // 右键：会话上下文菜单（多选/重命名/删除），屏蔽浏览器默认菜单
  li.oncontextmenu = (e) => {
    e.preventDefault()
    e.stopPropagation()
    openSessionMenu(e, s, li)
  }
  const ico = el("span", "session-ico", "💬")
  const body = el("div", "session-body")
  // 重命名：双击/右键菜单触发 → 内联编辑（回车/失焦保存，Esc 取消）
  const nameEl = el("span", "session-name", s.name) as HTMLElement
  // 选中按钮（多选勾选）：hover 显示、批量模式常驻；未进入批量模式时点击即进入并勾选
  const box = el("input", "session-check") as HTMLInputElement
  box.type = "checkbox"
  box.checked = batchMode && selected.has(s.id)
  box.onclick = (e) => {
    e.stopPropagation()
    if (!batchMode) enterBatch()
    toggleSelect(s.id, li)
  }
  body.append(nameEl)
  // 选中按钮直接挂行尾（原时间行已移除）
  li.append(ico, body, box)
  sessionList.appendChild(li)
}

/** 运行中上下文大小实时更新（标题栏展示）：更新内存快照；当前会话时刷新标题栏（事件每轮推送）。
 *  注意 currentSession 与 lastSessions 可能是不同对象引用（切换会话后 refreshSessions 重建列表），
 *  两者都需同步，否则 renderHeaderCtx 读到的 currentSession.ctxTokens 停留在切换时的旧值。 */
export function updateSessionCtx(sessionId: string, ctxTokens: number): void {
  const s = (lastSessions ?? []).find((x) => x.id === sessionId)
  if (s) s.ctxTokens = ctxTokens
  const cur = getCurrentSession()
  if (cur?.id === sessionId) {
    cur.ctxTokens = ctxTokens
    renderHeaderCtx()
  }
}

export async function refreshSessions(preloaded?: SessionInfo[]) {
  const seq = ++refreshSeq
  const sessions = preloaded ?? (await client.listSessions())
  if (seq !== refreshSeq) return // 已有更新的刷新请求：本次结果作废
  lastSessions = sessions
  const scrollTop = sessionList.scrollTop // 重渲染保留滚动位置（组折叠切换不跳顶）
  sessionList.innerHTML = ""
  const shown = searchQuery ? sessions.filter((s) => s.name.toLowerCase().includes(searchQuery)) : sessions
  if (searchQuery) {
    // 搜索态：平铺列表（不分组，聚焦过滤结果）
    for (const s of shown) appendSessionLi(s)
  } else {
    // 按更新时间倒序分组（今天/昨天/近7天/更早）；折叠组不渲染成员，批量模式强制全展开
    const sorted = [...shown].sort((a, b) => b.updatedAt - a.updatedAt)
    const groups = new Map<GroupKey, SessionInfo[]>()
    for (const s of sorted) {
      const key = sessionGroup(s.updatedAt)
      const arr = groups.get(key) ?? []
      arr.push(s)
      groups.set(key, arr)
    }
    for (const key of GROUP_ORDER) {
      const gs = groups.get(key)
      if (!gs || gs.length === 0) continue
      appendGroupHeader(key, gs.length)
      if (batchMode || !collapsedGroups.has(key)) {
        for (const s of gs) appendSessionLi(s, key)
      }
    }
  }
  sessionList.scrollTop = scrollTop
}

/** 会话名内联编辑（独立按钮 / 双击触发）：回车/失焦保存，Esc 取消。 */
function startRename(s: SessionInfo, nameEl: HTMLElement) {
  const inputEl = el("input", "session-rename") as HTMLInputElement
  inputEl.value = s.name
  inputEl.maxLength = 80
  inputEl.onclick = (ev) => ev.stopPropagation()
  inputEl.onmousedown = (ev) => ev.stopPropagation()
  nameEl.replaceWith(inputEl)
  inputEl.focus()
  inputEl.select()
  let done = false
  const finish = async (save: boolean) => {
    if (done) return
    done = true
    const v = inputEl.value.trim()
    inputEl.replaceWith(nameEl)
    if (save && v && v !== s.name) {
      try {
        await client.renameSession(s.id, v)
        nameEl.textContent = v
        const cur2 = getCurrentSession()
        if (cur2?.id === s.id) {
          setCurrentSession({ ...cur2, name: v })
          updateTitle()
        }
        await refreshSessions()
      } catch {
        /* 忽略 */
      }
    }
  }
  inputEl.onkeydown = (ev) => {
    if (ev.key === "Enter") {
      ev.preventDefault()
      void finish(true)
    } else if (ev.key === "Escape") {
      ev.preventDefault()
      void finish(false)
    }
  }
  inputEl.onblur = () => void finish(true)
}

/** 删除会话后，若当前会话被删则切到剩余第一个，没有则进入空白草稿页。 */
async function fallbackCurrentSession(deleted: Set<string>) {
  // 被删会话的草稿/附件/滚动记忆/排队输入状态一并清理（防残留，含后台删除的会话）
  for (const id of deleted) {
    clearSessionViewState(id)
    clearQueue(id)
  }
  const cur = getCurrentSession()
  if (cur && !deleted.has(cur.id)) return
  const remaining = (await client.listSessions()).filter((x) => !deleted.has(x.id))
  if (remaining.length) {
    setCurrentSession(remaining[0])
    await loadMessages(remaining[0].id)
  } else {
    enterDraftView()
  }
}

/** 删除会话确认：自绘 modal（ui.confirmDialog），确认后执行删除。 */
function showDeleteConfirm(sessionId: string, name: string) {
  void (async () => {
    if (!(await confirmDialog({ title: "删除会话", text: `确定删除会话「${name}」吗？此操作不可恢复。`, okLabel: "删除" }))) return
    try {
      await client.deleteSession(sessionId)
      await fallbackCurrentSession(new Set([sessionId]))
      await refreshSessions()
    } catch {
      /* 删除失败静默 */
    }
  })()
}

/** 批量删除确认：同单删 modal 风格，确认后逐个删除选中会话。 */
function showBatchDeleteConfirm() {
  void (async () => {
    const ids = [...selected]
    const names = ids.map((id) => selectedNames.get(id) || id)
    if (!(await confirmDialog({ title: "批量删除会话", okLabel: `删除 ${ids.length} 项`, list: names }))) return
    // 逐个删除，累计真实删除成功的会话（部分失败时仅按已删集合回退当前会话）
    const done = new Set<string>()
    try {
      for (const id of ids) {
        await client.deleteSession(id)
        done.add(id)
      }
    } catch {
      /* 部分失败静默：已删的删除、未删的保留 */
    }
    if (done.size) await fallbackCurrentSession(done)
    exitBatch()
    // 取消批量模式切换的延迟渲染（旧列表），避免其抢占正式刷新（seq 守卫）导致列表不更新
    if (batchRenderTimer) {
      clearTimeout(batchRenderTimer)
      batchRenderTimer = null
    }
    await refreshSessions() // 删除后重建列表（此前缺失：列表不刷新导致已删会话残留）
  })()
}

/* ---------- 会话右键菜单（多选 / 重命名 / 删除；行内按钮已移除） ---------- */

let ctxMenu: HTMLDivElement | null = null

function closeSessionMenu(): void {
  ctxMenu?.remove()
  ctxMenu = null
}

function openSessionMenu(e: MouseEvent, s: SessionInfo, li: HTMLElement): void {
  closeSessionMenu()
  const menu = el("div", "session-ctx-menu") as HTMLDivElement
  const items: Array<{ label: string; danger?: boolean; action: () => void }> = []
  if (batchMode) {
    items.push({ label: selected.has(s.id) ? "取消选择" : "选择", action: () => toggleSelect(s.id, li) })
  } else {
    items.push({ label: "选中", action: () => { enterBatch(); toggleSelect(s.id, li) } })
  }
  items.push(
    // 复制会话 ID（定位问题/反馈用）：任意条目可复制，不限于当前会话
    { label: "复制会话 ID", action: () => { void copyTextToClipboard(s.id).then((ok) => toast(ok ? `已复制会话 ID: ${s.id}` : "复制失败", ok ? "ok" : "error")) } },
    { label: "重命名", action: () => startRename(s, li.querySelector<HTMLElement>(".session-name")!) },
    { label: "删除", danger: true, action: () => showDeleteConfirm(s.id, s.name) },
  )
  for (const it of items) {
    const b = el("button", it.danger ? "ctx-item danger" : "ctx-item", it.label)
    b.onclick = () => {
      closeSessionMenu()
      it.action()
    }
    menu.appendChild(b)
  }
  document.body.appendChild(menu)
  ctxMenu = menu
  // 定位 + 视口边缘翻转（菜单不超出可视区）
  const rect = menu.getBoundingClientRect()
  menu.style.left = `${Math.max(8, Math.min(e.clientX, window.innerWidth - rect.width - 8))}px`
  menu.style.top = `${Math.max(8, Math.min(e.clientY, window.innerHeight - rect.height - 8))}px`
}

/* ---------- 空状态 ---------- */

let searchQuery = ""

/** 进入空白草稿页（不创建会话）：清空消息视图与输入，首条消息发送时才真正创建会话，
 *  避免点「新会话」即落盘产生大量空会话。 */
export function enterDraftView(): void {
  setCurrentSession(null)
  aside.classList.remove("open")
  msgEl.innerHTML = ""
  clearMsgNav()
  clearUnread()
  setEmptyState(null)
  showEmptyState()
  input.value = ""
  autosize()
  setPendingFiles([])
  renderAttachments()
  renderQueue() // 排队条随会话视图隐藏
  updateTitle()
  syncSendButton()
  focusInput()
}

function showEmptyState() {
  if (getEmptyState()) {
    getEmptyState()!.hidden = false
    return
  }
  const emptyState = el("div", "empty-state")
  setEmptyState(emptyState)
  emptyState.append(el("div", "es-slogan", "歌未竟 东方白"))
  const tips = el("div", "es-suggestions")
  renderShortcutButtons(tips)
  emptyState.appendChild(tips)
  msgEl.appendChild(emptyState)
}

function hideEmptyState() {
  const e = getEmptyState()
  if (e) e.hidden = true
}
/** 设置面板增删快捷按钮后，空白页即时刷新按钮（仅影响快捷按钮，保留其他入口）。 */
document.addEventListener("gebai:shortcuts-change", () => {
  const e = getEmptyState()
  if (!e || e.hidden) return
  const tips = e.querySelector<HTMLElement>(".es-suggestions")
  if (tips) renderShortcutButtons(tips)
})
/** 供 main 组装（发送消息时隐藏空状态）。 */
export { hideEmptyState }

/* ---------- 自动标题：第一条回答完成后，用第一个输入生成会话标题 ---------- */

export async function maybeAutoTitle(sessionId: string) {
  if (autoNamed.has(sessionId)) return
  // 名称判定优先走快照（避免为取标题全量拉取会话消息）；快照无此会话时才回退 getSession
  const snapInfo = client.getSnapshot().sessions.find((s) => s.id === sessionId)
  if (snapInfo && snapInfo.name !== "新会话") {
    autoNamed.add(sessionId)
    return
  }
  if (!snapInfo) {
    const session = await client.getSession(sessionId).catch(() => null)
    if (!session) return
    if (session.name !== "新会话") {
      // 已自定义标题（如创建时指定），不再自动命名
      autoNamed.add(sessionId)
      return
    }
  }
  const first = firstInputOf(sessionId)
  if (!first) return
  const compact = first.replace(/\s+/g, " ").trim()
  const title = compact.slice(0, 24) + (compact.length > 24 ? "…" : "")
  try {
    await client.renameSession(sessionId, title)
    autoNamed.add(sessionId)
    const cur = getCurrentSession()
    if (cur?.id === sessionId) {
      setCurrentSession({ ...cur, name: title })
      updateTitle()
    }
    await refreshSessions()
  } catch {
    /* 改名失败（如会话已删除）静默忽略 */
  }
}

/** 导出当前会话为 Markdown 文件（下载）：消息 + 内容块转可读 Markdown。 */
export async function exportSession(sessionId: string): Promise<void> {
  const session = await client.getSession(sessionId)
  const lines: string[] = [
    `# ${session.name}`,
    "",
    `> 会话 ID: \`${session.id}\`　创建: ${new Date(session.createdAt).toLocaleString()}　更新: ${new Date(session.updatedAt).toLocaleString()}`,
    "",
  ]
  for (const m of session.messages ?? []) {
    const parts: string[] = []
    if (m.content) parts.push(m.content)
    for (const b of m.blocks ?? []) parts.push(blockToMarkdown(b))
    const text = parts.filter(Boolean).join("\n\n")
    if (!text.trim()) continue
    const tag = m.role === "user" ? "🧑 用户" : m.role === "assistant" ? "🤖 助手" : m.role === "system" ? "📋 系统" : `🔧 工具${m.name ? `（${m.name}）` : ""}`
    lines.push(`## ${tag}`, "", text, "")
  }
  const blob = new Blob([lines.join("\n")], { type: "text/markdown;charset=utf-8" })
  const url = URL.createObjectURL(blob)
  const a = document.createElement("a")
  a.href = url
  // 文件名消毒：控制字符/非法字符替换、首尾点空格去除、长度截断（Windows 保存安全）
  const safeName = (session.name || session.id)
    .replace(/[\\/:*?"<>|\x00-\x1f\x7f]/g, "_")
    .trim()
    .replace(/^[.\s]+|[.\s]+$/g, "")
    .slice(0, 80)
  a.download = `${safeName || session.id}.md`
  desktopDownloadHint(`${safeName || session.id}.md`)
  a.click()
  setTimeout(() => URL.revokeObjectURL(url), 1000)
}

/** 内容块 → Markdown 文本（导出用）。 */
function blockToMarkdown(b: ContentBlock): string {
  switch (b.type) {
    case "text":
      return b.text
    case "code":
      return `\`\`\`${b.language ?? ""}\n${b.text}\n\`\`\``
    case "image":
      return `![${b.name ?? "image"}](${b.path})`
    case "file":
      return `📎 ${b.name}（\`${b.path}\`）`
    case "diagram": {
      const ext = { plantuml: "puml", mermaid: "mmd", d2: "d2", echarts: "echarts" }[b.format ?? "plantuml"] ?? "puml"
      const label = { plantuml: "PlantUML", mermaid: "Mermaid", d2: "D2", echarts: "ECharts" }[b.format ?? "plantuml"] ?? "PlantUML"
      return `📊 图表${b.name ? ` ${b.name}` : ""}（${label}）：\n\`\`\`${ext}\n${b.code}\n\`\`\``
    }
    case "diff":
      return `🔀 对比${b.oldName ? ` ${b.oldName}` : ""} → ${b.newName ?? ""}：\n\`\`\`diff\n${b.oldText}\n──────\n${b.newText}\n\`\`\``
    case "html":
      return `🖥️ HTML 页面${b.name ? ` ${b.name}` : ""}：\n\`\`\`html\n${b.html}\n\`\`\``
  }
}

/** 新会话 / 侧栏开关 / 批量删除绑定（供 main 组装）。 */
export function bindSessionActions() {
  autoHideScrollbar(sessionList)
  // 搜索输入防抖（150ms）：快速击键不触发全量列表请求风暴
  let searchTimer: ReturnType<typeof setTimeout> | null = null
  searchInputEl.addEventListener("input", () => {
    searchQuery = searchInputEl.value.trim().toLowerCase()
    if (searchTimer) clearTimeout(searchTimer)
    searchTimer = setTimeout(() => void refreshSessions(), 150)
  })
  batchCancelBtn.onclick = exitBatch
  batchDelBtn.onclick = () => {
    if (!selected.size) return
    showBatchDeleteConfirm()
  }
  // 右键菜单关闭：任意点击 / 新右键 / Esc / 滚动 / 窗口缩放
  document.addEventListener("click", () => closeSessionMenu())
  document.addEventListener("contextmenu", () => closeSessionMenu(), true) // 捕获：新右键先关旧菜单，再走 li 打开新菜单
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") closeSessionMenu()
  })
  document.addEventListener("scroll", () => closeSessionMenu(), true)
  window.addEventListener("resize", () => closeSessionMenu())
  newSessionBtn.onclick = () => {
    // 草稿态：不立即创建会话（避免落盘大量空会话），首条消息发送时才真正创建
    const prev = getCurrentSession()
    if (prev) saveSessionViewState(prev.id)
    enterDraftView()
    void refreshSessions() // 清除列表中旧会话的激活高亮
  }
  // 整栏折叠：桌面端（>860px）折叠隐藏侧栏（主区占满，状态持久化）；窄屏保持滑动抽屉行为
  const SIDEBAR_KEY = "gebai.ui.sidebarCollapsed"
  const narrowScreen = () => window.matchMedia("(max-width: 860px)").matches
  try {
    if (localStorage.getItem(SIDEBAR_KEY) === "1" && !narrowScreen()) document.body.classList.add("sidebar-collapsed")
  } catch {
    /* 存储不可用忽略 */
  }
  // 切换会话列表显隐：桌面端折叠/展开整栏，窄屏切换滑动抽屉
  const toggleSidebar = () => {
    if (narrowScreen()) {
      aside.classList.toggle("open")
    } else {
      const collapsed = document.body.classList.toggle("sidebar-collapsed")
      try {
        localStorage.setItem(SIDEBAR_KEY, collapsed ? "1" : "")
      } catch {
        /* ignore */
      }
    }
  }
  sidebarToggle.onclick = toggleSidebar
  // Ctrl+B 快捷键切换会话列表（拦截浏览器默认书签行为；任意焦点状态可用）
  document.addEventListener("keydown", (e) => {
    if (e.ctrlKey && !e.shiftKey && !e.altKey && e.key.toLowerCase() === "b") {
      e.preventDefault()
      toggleSidebar()
    }
  })
}
