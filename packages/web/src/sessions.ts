import { uuid } from "./uuid"
import { mainKeymap } from "./keymap-main"
import { nextScopeId, popKeyScope, pushKeyScope, FOCUS_WITH_INPUT } from "./keymap"
import { activatePane, activePane, deactivatePane, paneOf, reapIdlePanes, type MsgPane, type PaneChunks } from "./msg-panes"
import type { ContentBlock, RuntimeSessionInfo, SessionDetail, SessionInfo } from "@gebai/sdk"
import {
  approvalsEl,
  aside,
  autoNamed,
  client,
  copyTextToClipboard,
  el,
  focusInput,
  getCurrentSession,
  getEmptyState,
  input,
  isDraftView,
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
  sidebarBackdropEl,
  sidebarToggle,
  todoState,
  updateTitle,
  saveDraft,
  getDraft,
  clearDraft,
} from "./state"
import { markdownBlock } from "./markdown"
import { batchExportFileName, buildExportFile, detailToImportData, downloadJson, parseImportText, singleExportFileName } from "./session-io"
import { appendMsg, appendTodoCard, beginMsgBatch, engineNoteOf, isEngineNoteMsg, reasoningBlock, subSessionEntry, takeMsgBatch } from "./messages"
import { clearUnread, isFollowing, lockToBottom, restoreScroll, stopFollowing } from "./jump-bottom"
import { applyApprovalSkip } from "./approval-skip"
import { applyApprovalVisibility } from "./approvals"
import { applyInteractionVisibility } from "./messages"
import { appendArchiveMessage, mergeFromMessage, refreshSubSessions, registerArchive } from "./subsessions"
import { autosize, firstInputOf, resetHistoryNav, syncSendButton } from "./composer"
import { autoHideScrollbar, confirmDialog, desktopDownloadHint, toast } from "./ui"
import { renderShortcutButtons } from "./shortcuts"
import { clearMsgNav, setMsgNavJumper, setMsgNavLocator, setMsgNavSegs, updateMsgNav, type MsgNavSeg } from "./msg-nav"
import { isRunMessage, planMessageChunks, runIdOfMessage } from "./history-chunk"
import { DEFAULT_PER_MSG } from "./virtual-window"
import { dismissOnScroll } from "./scroll-dismiss"
import { msgWindow, setBlockRenderer } from "./msg-window"
import { renderAttachments } from "./attachments"
import { clearQueue, renderQueue } from "./queue"
import { rememberTaskLabels } from "./task-labels"
import { rangeSelection, selectionState } from "./session-select-core"

export type LoadMessagesFn = (sessionId: string) => Promise<void>

/* 批量选择删除：侧栏顶部操作条（index.html 提供）；入口为每个会话行的选择框（hover 显示，勾选即进入批量模式） */
const batchBar = document.getElementById("batch-bar")!
const batchCountEl = document.getElementById("batch-count")!
const batchDelBtn = document.getElementById("batch-del") as HTMLButtonElement
const batchExportBtn = document.getElementById("batch-export") as HTMLButtonElement
const batchCancelBtn = document.getElementById("batch-cancel")!
const searchInputEl = document.getElementById("session-search") as HTMLInputElement

/* ---------- 会话加载与列表 ---------- */

/** 消息加载请求序号守卫：快速切换会话（A→B→A）时丢弃迟到的旧请求结果，
 * 防止旧数据覆盖新视图（DOM 清空/渲染是异步链，迟到的 getSession 响应会串台）。 */
let loadSeq = 0

/** 各会话离开时的阅读位置（null=粘底/无记忆，下次落底）：按「块 key + 块内偏移」记忆——
 *  消息列高度含未渲染块的估高，绝对 scrollTop 跨会话不可复现；锚点在目标块挂载后精确落位。 */
const scrollMemory = new Map<string, { key: string; offset: number } | null>()

/** 保存当前会话的草稿/附件/阅读位置（切换会话前调用）。 */
function saveSessionViewState(sessionId: string) {
  saveDraft(sessionId)
  pendingFilesBySession.set(sessionId, pendingFiles)
  // 阅读位置：由跟随意图决定「下次落底」还是「恢复位置」——几何贴底在运行中会话会被流式增长
  // 反复打破（距底超出阈值），据此存锚点会把「看最新」的会话错记成阅读位置
  scrollMemory.set(sessionId, isFollowing() ? null : msgWindow.anchor())
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

/** 历史消息的可递归形状（Message 与子会话存档条目共有：role/content/subSessionArchive）。 */
interface TaskLabelScanMsg {
  role: string
  content?: unknown
  subSessionArchive?: { messages: TaskLabelScanMsg[] }
}

/** 登记历史消息里的后台任务身份：工具结果文本（启动结果/任务状态行）+ 子会话存档内的过程消息（嵌套存档递归）。 */
function rememberHistoryTaskLabels(msgs: TaskLabelScanMsg[], depth = 0): void {
  if (depth > 4) return // 嵌套存档深度兜底（子会话内再派生子会话，正常不超过两层）
  for (const m of msgs) {
    if (m.role === "tool" && typeof m.content === "string") rememberTaskLabels(m.content)
    const inner = m.subSessionArchive?.messages
    if (inner?.length) rememberHistoryTaskLabels(inner, depth + 1)
  }
}

/* ---------- 会话联动：切换/加载都必须调用的一份清单 ---------- */

/**
 * 使「随当前会话变化」的 UI 跟随当前会话。
 *
 * 切换会话有两条路径（已缓存容器的快速路径 `activatePaneFast`、未缓存的完整加载 `loadMessages`），
 * 以前它们各自手写一份刷新清单，结果是两条路径各漏一半（切换后列表高亮停在旧会话、子会话角标
 * 停在旧会话……）。所以把清单收在**这一个函数**里：任何随会话变化的 UI 刷新都加到这里，
 * 两条路径都调它，不再各写一份。
 *
 * 判定标准：该项显示的如果是「当前会话」的数据，且在新会话里会呈现不同值，就必须进本清单。
 * 事件驱动型的局部刷新（某会话新到审批/附件、按 sessionId 判定的）不在此列——它们自行处理。
 */
function syncSessionScopedUi(): void {
  syncActiveRow(getCurrentSession()?.id ?? null) // 会话列表高亮
  lastActiveId = getCurrentSession()?.id ?? null
  updateTitle() // 标题栏标题
  syncSendButton() // 发送/停止（运行中会话显示停止）
  renderQueue() // 排队条（队列按会话隔离）
  applyApprovalVisibility() // 待审批卡片（仅显示当前会话的）
  applyInteractionVisibility() // 待作答交互卡（同上）
  updateMsgNav() // 消息导航列（段表就位后重算位置）
  refreshSubSessions() // 标题栏子会话按钮与面板（按当前会话过滤/角标）
  // 空白页：它挂在宿主 #messages 上（草稿页形态），而宿主是容器栈——切到
  // 有内容的会话时不清就会与 pane 并存（上方多出一条空白区）。
  // 判据用「活动 pane 里是否已有消息」而不是「有无 pane」：打开一个空会话仍需显示空白页。
  // 不读 chunks——快速路径下它可能尚未就绪；直接看容器里有无消息节点最可靠。
  if (activePane()?.container.querySelector(".msg")) hideEmptyState()
}

export async function loadMessages(sessionId: string) {
  const seq = ++loadSeq
  // 会话消息视图（pane）：每个会话一份独立滚动容器——切会话只切显隐，后台会话的
  // DOM 与在途引用不丢（容器级几何：粘底跟随/窗口化/消息导航各自成立）
  activatePane(sessionId)
  applyApprovalVisibility() // 审批卡片跟随会话：仅显示当前会话的待审批，切回恢复
  const session = await client.getSession(sessionId)
  if (seq !== loadSeq) return // 已有更新的加载请求：本次结果作废
  // 后台任务身份表：先从历史工具结果登记（任务 id → 命令/子会话名），卡片参数区才能补上「在等什么」——
  // 消息窗口化按需渲染，不能依赖「启动卡先渲染、等待卡后渲染」的先后顺序兜底
  rememberHistoryTaskLabels(session.messages ?? [])
  resetMsgWindow()
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
  // 空内容消息（无 content/blocks/attachments/reasoning）不显示：按渲染后的实际可见消息数判空
  const visible = (session.messages ?? []).filter((m) => m.content || m.blocks?.length || m.attachments?.length || m.reasoning)
  // 运行中的会话状态（新会话容器恢复/流式累积引用；先于消息循环就位）
  const run = runs.get(sessionId)
  // 窗口化：消息切成块（未渲染块只有估高），容器重置后落底——首屏只渲染尾部块，
  // 更早历史在滚动到附近时按需渲染（见 renderChunk）。空状态在重置后追加（重置会清空容器）
  const bounds = planMessageChunks(visible, chunkSize(scrollBox().clientHeight))
  setChunks({ msgs: visible, bounds, sessionId, index: buildMsgIndex(visible) })
  msgWindow.reset(chunkSeeds(bounds))
  buildMsgNav(visible)
  if (visible.length === 0) showEmptyState()
  else hideEmptyState()
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
          const think = reasoningBlock()
          run.reasoningEl = think
          const rb = think.querySelector<HTMLElement>(".reasoning-body")
          if (rb) rb.appendChild(markdownBlock(run.reasoningAcc.trim()))
          bubble.prepend(think)
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
  for (const m of session.messages ?? []) {
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
    const parent = entry.runId ? run?.subSessions?.get(entry.runId)?.body : undefined
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
  syncSessionScopedUi()
  // 滚动位置：离开时在阅读历史 → 按锚点恢复（窗口化按需挂载目标块后精确落位）；否则落底。
  // restoreScroll 同步按落位刷新锁定状态并使未决跟随回调（reset 排期的 rAF / lockToBottom
  // 启动的对齐保持循环）失效——直接赋值 scrollTop 会留下 following=true 的旧状态。
  const mem = scrollMemory.get(sessionId)
  if (mem) {
    stopFollowing()
    const top = msgWindow.scrollToAnchor(mem.key, mem.offset)
    if (top !== null) restoreScroll(top)
  } else {
    // 粘底状态不跨会话记忆：上会话阅读历史时 following=false，不重置会让新会话停在旧位置
    lockToBottom()
  }
  // 运行中会话附加（DESIGN「运行中会话恢复」）：页面刷新/切换进入运行中会话时恢复在途流与
  // 待决交互卡。渲染完成后再附加（存储基线先上屏，在途文本作为流式消息续接其后）
  void runningAttachHook?.(sessionId)
}

/** 窗口化块大小（条/块）：按「约一屏半」估算而非固定条数——消息高度差异极大（工具卡/长代码
 *  可能上千像素），固定条数会让单块高度失控（实测可达九屏：一次滚动要渲染几十条、且估高
 *  修正幅度过大）。块是窗口化的最小挂载单位，宜向视口量级靠。 */
function chunkSize(viewportH: number): number {
  const rows = Math.ceil((Math.max(viewportH, 320) * 0.9) / DEFAULT_PER_MSG)
  return Math.min(12, Math.max(3, rows))
}

/**
 * 渲染 [from, to) 区间的消息：普通消息、工具结果卡片、子会话入口行（过程与报告在观察面板）。
 *
 * 子会话相关消息在消息流里只产生一行入口：`subSession` 过程消息（旧版数据逐条落盘）与
 * `subSessionArchive` 存档（新版挂在工具调用记录上）都登记进观察面板（subsessions.ts），
 * 流里只留 `/🌿 子会话 · 名` 一行——历史回放与实时渲染同一形态，不因刷新就变成满屏过程。
 */
function renderMessageRange(
  msgs: Array<import("@gebai/sdk").Message>,
  from: number,
  to: number,
  sessionId: string,
  _liveRun: ReturnType<typeof runs.get>,
): void {
  // 同一区间内同一 runId 的入口只出一行：旧版数据的过程消息会连续多条
  const shown = new Set<string>()
  const entryFor = (runId: string, meta: { name?: string; model?: string; agents: string[]; state: string; status: "running" | "done" }) => {
    if (shown.has(runId)) return
    shown.add(runId)
    subSessionEntry({ sessionId, runId, ...meta })
  }
  for (let i = from; i < to; i++) {
    const m = msgs[i]
    const isLegacy = (m as import("@gebai/sdk").Message).subAgent === true
    if (m.subSession || isLegacy) {
      // 旧版数据：过程消息逐条落盘，登记进面板的过程区（懒渲染）
      const runId = runIdOfMessage(m) ?? ""
      if (!runId) continue
      const legacyMeta = (m as import("@gebai/sdk").Message).subAgentMeta
      const rec = appendArchiveMessage(sessionId, runId, {
        name: legacyMeta?.agent,
        agents: m.subSessionMeta?.agents ?? (legacyMeta?.agent ? [legacyMeta.agent] : []),
        input: m.subSessionMeta?.input ?? legacyMeta?.input,
      }, { role: m.role as "user" | "assistant" | "tool", name: m.name, content: m.content, toolCalls: m.toolCalls, arguments: m.arguments })
      if (m.role === "user" && (m.subSessionMeta || legacyMeta)) continue // 输入已在面板标题区展示
      entryFor(runId, { name: rec.name, model: rec.model, agents: rec.agents, state: rec.stateText, status: rec.status })
      continue
    }
    // 子会话合入消息（role=user + engineNote=subsession + subSessionMerged）：把阶段性合入认回面板记录，
    // 否则刷新后面板就只剩「最终报告」（合入次数与各阶段全文只在实时路径里）
    if (m.subSessionMerged) mergeFromMessage(sessionId, m.subSessionMerged, String(m.content ?? ""))
    // 存档（新版：subsession_run 工具调用记录扩展字段；旧版：subAgentRun）→ 登记进面板 + 一行入口
    const archive = m.subSessionArchive ?? (m as import("@gebai/sdk").Message).subAgentRun
    if (archive) {
      const rec = registerArchive(sessionId, archive as import("@gebai/sdk").SubSessionArchive)
      entryFor(rec.runId, { name: rec.name, model: rec.model, agents: rec.agents, state: rec.stateText, status: rec.status })
    }
    // 工具结果卡片（subsession_run 的输出为 markdown）照常渲染
    if (!(archive && m.role === "tool")) appendMsg(m)
  }
}

/* ---------- 窗口化块表与导航装配 ---------- */

/** 当前会话的消息清单与块表：存在**活动 pane 上**（不只存一份全局）——缓存的容器切回时
 *  靠它重建历史块，否则窗口化拿不到属于该会话的消息（表现为“切回去是空白”）。 */
function chunksOf(pane: MsgPane | null = activePane()): PaneChunks | null {
  return pane?.chunks ?? null
}

/** 写入当前会话的块表（加载完成后调用）。 */
function setChunks(next: PaneChunks | null): void {
  const pane = activePane()
  if (pane) pane.chunks = next
}

/** 清空消息列并丢弃块表（会话切换 / 空白页 / 登出）。 */
export function resetMsgWindow(): void {
  setChunks(null)
  msgWindow.reset([])
}

/** 块 key（滚动锚点与导航定位）：按块序号稳定——块自消息头部起算，新消息只影响末尾块。 */
function chunkKey(index: number): string {
  return `b${index}`
}

/** 窗口化槽位种子：weight = 块内消息条数（未渲染块的估高权重）。 */
function chunkSeeds(bounds: number[]): Array<{ key: string; weight: number }> {
  return bounds.slice(0, -1).map((start, i) => ({ key: chunkKey(i), weight: Math.max(1, bounds[i + 1] - start) }))
}

/** 消息 id → 下标（导航定位用）。 */
function buildMsgIndex(msgs: Array<import("@gebai/sdk").Message>): Map<string, number> {
  const map = new Map<string, number>()
  msgs.forEach((m, i) => {
    if (m.id) map.set(m.id, i)
  })
  return map
}

/** 消息下标所在的块序号（块边界升序二分）。 */
function chunkOfMsg(bounds: number[], index: number): number {
  let lo = 0
  let hi = bounds.length - 2
  let best = 0
  while (lo <= hi) {
    const mid = (lo + hi) >> 1
    if (bounds[mid] <= index) {
      best = mid
      lo = mid + 1
    } else {
      hi = mid - 1
    }
  }
  return best
}

/** 渲染一块消息到 host（窗口化按需调用）。只有末尾块承载在途运行的流式引用——切走期间累积的
 *  文本在切回时重建；历史块不得覆盖 liveRun 的在途引用。 */
function renderChunk(index: number, host: HTMLElement): void {
  const pane = activePane()
  const t = chunksOf(pane)
  if (!t) return
  const from = t.bounds[index]
  const to = t.bounds[index + 1]
  if (from === undefined || to === undefined) return
  const frag = document.createDocumentFragment()
  beginMsgBatch(frag)
  try {
    const liveRun = index === t.bounds.length - 2 ? runs.get(t.sessionId) : undefined
    renderMessageRange(t.msgs, from, to, t.sessionId, liveRun)
  } finally {
    const out = takeMsgBatch()
    if (out) host.appendChild(out)
  }
}

/** 导航段文本（消息正文压平为单行预览，长度截断）。 */
function navText(content: string): string {
  const flat = content
    .replace(/```[\s\S]*?```/g, " ")
    .replace(/!?\[([^\]]*)\]\([^)]*\)/g, "$1")
    .replace(/[#*>`_~|]/g, " ")
    .replace(/\s+/g, " ")
    .trim()
  if (!flat) return "（无内容）"
  return flat.length > 60 ? `${flat.slice(0, 60)}…` : flat
}

/** 建立导航段表：顶层用户消息一条一段（引擎提示与子会话执行过程消息不算——后者渲染进折叠容器）。
 *  数据驱动：尚未渲染的块里的消息同样有段，导航列长度不随渲染进度变化。 */
function buildMsgNav(msgs: Array<import("@gebai/sdk").Message>): void {
  const segs: MsgNavSeg[] = []
  for (const m of msgs) {
    if (m.role !== "user" || isRunMessage(m) || engineNoteOf(m) || !m.id) continue
    segs.push({ key: m.id, text: navText(m.content ?? "") })
  }
  setMsgNavSegs(segs)
}

/** 活动会话的滚动容器（pane）：消息列几何一律以它为准（宿主 #messages 本身不滚动）。
 *  无 pane（草稿页）时回退宿主——其 scrollHeight 为 0，各几何计算自然空转。 */
function scrollBox(): HTMLElement {
  return activePane()?.container ?? msgEl
}

/** 段位置（屏幕坐标）：已挂载消息取实时几何，未挂载的按块坐标 + 块内比例估算——导航列只作示意，
 *  跳转落位仍按目标元素精确对齐（见 jumpToMsg）。 */
function locateMsg(key: string): number | null {
  const escaped = typeof CSS !== "undefined" && CSS.escape ? CSS.escape(key) : key
  const box = scrollBox()
  const node = box.querySelector<HTMLElement>(`[data-msg-id="${escaped}"]`)
  if (node && node.isConnected) return node.getBoundingClientRect().top
  const t = chunksOf()
  if (!t) return null
  const i = t.index.get(key)
  if (i === undefined) return null
  const b = chunkOfMsg(t.bounds, i)
  const blockKey = chunkKey(b)
  const top = msgWindow.posOfKey(blockKey)
  const height = msgWindow.heightOfKey(blockKey)
  if (top === null || height === null) return null
  const ratio = (i - t.bounds[b]) / Math.max(1, t.bounds[b + 1] - t.bounds[b])
  return box.getBoundingClientRect().top + msgWindow.contentTop() + top + ratio * height - box.scrollTop
}

/** 跳转到某条消息：窗口化把其所在块就位（按需挂载），元素挂载后按真实几何精确对齐。
 *  运行期新增的消息（在本会话加载之后到达，挂在尾部活动区、不在块表内）直接对已挂载节点落位。 */
function jumpToMsg(key: string): void {
  const escaped = typeof CSS !== "undefined" && CSS.escape ? CSS.escape(key) : key
  const box = scrollBox()
  const mounted = box.querySelector<HTMLElement>(`[data-msg-id="${escaped}"]`)
  const t = chunksOf()
  const i = t?.index.get(key)
  if (i === undefined || !t) {
    if (mounted && mounted.isConnected) {
      const delta = mounted.getBoundingClientRect().top - box.getBoundingClientRect().top
      box.scrollTop = Math.max(0, box.scrollTop + delta - 12)
    }
    return
  }
  const b = chunkOfMsg(t.bounds, i)
  const ratio = (i - t.bounds[b]) / Math.max(1, t.bounds[b + 1] - t.bounds[b])
  msgWindow.scrollToAnchor(chunkKey(b), (msgWindow.heightOfKey(chunkKey(b)) ?? 0) * ratio)
  if (mounted && mounted.isConnected) {
    const delta = mounted.getBoundingClientRect().top - box.getBoundingClientRect().top
    box.scrollTop = Math.max(0, box.scrollTop + delta - 12)
  }
}

setBlockRenderer((pane, index, host) => {
  // 块渲染用**该 pane 自己的**块表：缓存过的容器重新显示时，历史块要靠它重建
  if (!pane.chunks || pane.chunks.sessionId !== pane.sessionId) return
  renderChunk(index, host)
})
setMsgNavLocator(locateMsg)
setMsgNavJumper(jumpToMsg)

/** 运行中会话附加钩子（main.ts 注册实现；null 时无附加能力——单测环境等）。 */
let runningAttachHook: ((sessionId: string) => Promise<void>) | null = null
export function setRunningAttach(fn: (sessionId: string) => Promise<void>): void {
  runningAttachHook = fn
}

/** 按需附加恢复运行态（事件/快照驱动，与进入会话时的附加同一实现）：服务端在页面空闲期间开始的
 *  任务（重启续跑/飞书桥接/定时任务/其他标签页）经 `event.task.start` 或快照 `running` 清单告知，
 *  前端据此补上运行态（信号灯/停止按钮/单轮计时/在途流）——未运行或本页已接管时 no-op。 */
export function attachRunningIfNeeded(sessionId: string): void {
  void runningAttachHook?.(sessionId)
}

/* ---------- 批量选择删除 ---------- */

let batchMode = false
/** 选中集（按会话 id）：批量模式下每个可见行按它落选中态，重渲染后选中不丢。 */
const selected = new Set<string>()
const selectedNames = new Map<string, string>()
/** Shift 区间的锚点：上一次选中点击落在哪一行（退出批量模式即清空）。 */
let selectAnchor: string | null = null

/** 当前渲染出的会话行 id，按列表展示顺序（分组展开与搜索平铺同一口径）。 */
function renderedSids(): string[] {
  return [...sessionList.querySelectorAll<HTMLElement>("li[data-sid]")].map((x) => x.dataset.sid!)
}

/** 分类头的选中反映（渲染与选中变更共用同一口径）：勾选框状态 + 整组选中时的行高亮，
 *  与普通会话行完全同一个勾选框元素与同一套选中样式。 */
function syncGroupCheck(head: HTMLElement): void {
  const box = head.querySelector<HTMLInputElement>(".session-check")
  const st = selectionState(sidsOfGroup(head.dataset.group!), selected)
  if (box) {
    box.checked = st === "all"
    // 部分选中：半选态（indeterminate 只能经 JS 设置，无法从 HTML 属性表达）
    box.indeterminate = st === "some"
  }
  head.classList.toggle("selected", st === "all")
}

/** 一个分类下的全部会话 id（按数据算，不读 DOM）：折叠分组没有渲染出成员行，但勾选它仍要选中里面的会话。 */
function sidsOfGroup(key: string): string[] {
  const all = lastSessions ?? []
  const shown = searchQuery ? all.filter((s) => s.name.toLowerCase().includes(searchQuery)) : all
  return shown.filter((s) => (s.pinned ? "pinned" : sessionGroup(s.updatedAt)) === key).map((s) => s.id)
}

/** 把选中集落到当前 DOM（不重建列表）：行高亮 + 勾选框 + 分类头三态 + 操作条计数。 */
function paintSelection(): void {
  for (const li of sessionList.querySelectorAll<HTMLElement>("li[data-sid]")) {
    const on = selected.has(li.dataset.sid!)
    li.classList.toggle("selected", on)
    const box = li.querySelector<HTMLInputElement>(".session-check")
    if (box) box.checked = on
  }
  for (const head of sessionList.querySelectorAll<HTMLElement>("li.session-group[data-group]")) syncGroupCheck(head)
  updateBatchCount()
}

/** 落一次选中结果：选中非空即进入批量模式（修饰键多选也是它的入口），全部取消则退出。 */
function commitSelection(): void {
  if (selected.size === 0) {
    if (batchMode) exitBatch()
    else paintSelection()
    return
  }
  if (!batchMode) enterBatch()
  paintSelection()
}

/** 切换一行的选中态（勾选框 / Ctrl 单击 / 批量模式下单击）。 */
function toggleSelect(id: string): void {
  if (selected.has(id)) selected.delete(id)
  else selected.add(id)
  selectAnchor = id
  commitSelection()
}

/**
 * 区间选中（Shift 单击）：从锚点到该行的连续区段，`additive` 为真时并入现有选中（Ctrl+Shift）、
 * 否则替换。锚点只在无锚点/锚点已不在列表（被删、被搜索过滤、所在分组未渲染）时前移到该行，
 * 连续 Shift 单击因此始终以同一起点为基准延伸（与文件管理器同口径）。
 */
function selectRange(sid: string, additive: boolean): void {
  const ids = rangeSelection(renderedSids(), selectAnchor, sid)
  if (!additive) selected.clear()
  for (const id of ids) selected.add(id)
  if (ids.length === 1) selectAnchor = sid
  commitSelection()
}

function updateBatchCount() {
  batchCountEl.textContent = `已选 ${selected.size} 项`
  batchDelBtn.disabled = selected.size === 0
  batchExportBtn.disabled = selected.size === 0
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
  updateBatchCount()
  // 延迟重渲染展开全部折叠分组：选中集与落选中（paintSelection）都已完成，同步重建不影响结果，
  // 延后只是避免在同一次点击里摘掉刚被点击的行
  scheduleBatchRender()
}

function exitBatch() {
  batchMode = false
  sessionList.classList.remove("batch-mode")
  batchBar.hidden = true
  searchInputEl.disabled = false
  newSessionBtn.disabled = false
  selected.clear()
  selectAnchor = null
  paintSelection() // 选中集清空后立即同步行高亮与分类头（不依赖随后的重渲染时序）
  scheduleBatchRender() // 恢复折叠状态
}

/** 会话列表刷新请求序号守卫：快速搜索/切换时丢弃迟到的旧响应。 */
let refreshSeq = 0
/** 最近一次加载的会话列表（组折叠/批量模式切换重渲染时复用，避免重复请求）。 */
let lastSessions: SessionInfo[] | null = null

/** 列表结构签名与激活项记忆（见 refreshSessions：结构未变时跳过整列重建）。 */
let lastListSig = ""
let lastActiveId: string | null = null

/** 服务端运行态明细（快照 runtime）：待决交互/后台任务/子会话概要的权威基线。 */
let runtimeInfo: Record<string, RuntimeSessionInfo> = {}
/** 本页由事件得知的运行中会话（快照之外的增量修正）：后台任务启动/结束无需等下一次快照即可反映。 */
const runningSeen = new Set<string>()

/** 应用快照运行态（重连/登录/建连时到达）：以服务端为准收敛本地标记（断线期间的事件可能没收到）。 */
export function setRuntimeInfo(rt: Record<string, RuntimeSessionInfo>): void {
  runtimeInfo = rt
  runningSeen.clear()
  for (const id of Object.keys(rt)) runningSeen.add(id)
}

/** 事件驱动的运行态增量修正（event.task.start/done/error）：列表角标据此即时变化。 */
export function markSessionRunning(sessionId: string, running: boolean): void {
  if (running) {
    runningSeen.add(sessionId)
  } else {
    // 任务结束事件即该会话运行态的权威结论：一并丢弃快照里的运行态明细，
    // 否则角标要等下一次快照才消失（期间显示「在跑」而实际已结束）
    const had = runningSeen.delete(sessionId) || sessionId in runtimeInfo
    delete runtimeInfo[sessionId]
    if (!had) return
  }
  // 刷新必须拉新数据，不得用 lastSessions 陈旧数组重流：任务结束刷新与自动命名刷新
  // （maybeAutoTitle rename 后的 refreshSessions）并发时，preloaded 路径不发请求、seq 更大，
  // 会把在途的新名列表用 seq 守卫丢弃——列表停留旧名（如「新会话」）直到手动刷新才恢复。
  void refreshSessions()
}

/** 待决卡片增减（审批/交互卡接）：列表角标要从「运行中」变为「⌛ 等待你的输入」再变回去。
 *  事件由 approvals.ts / messages.ts 派发（它们已被本模块导入，不能反向导入）。
 *  只重算角标而不重建整列：结构签名含运行态，签名未变时 refreshSessions 自己会走轻量同步。 */
document.addEventListener("gebai:pending-change", () => {
  void refreshSessions()
})

/** 本页待决卡片数：审批卡（.approval）与交互卡（.interaction-card）都挂在 #approvals 下、带 data-session。
 *  DOM 是它们的**唯一**存放处（审批计数与卡片生命周期同生共死），直接数它比另维一份计数可靠——
 *  不会出现「卡已处理但计数没减」那种不一致。
 *
 *   **不看 hidden**：非当前会话的卡片会被 applyApprovalVisibility 隐藏，但它仍然在等用户作答——
 *   多会话并行时「哪个会话卡在我这里」正是列表角标要回答的问题，用 :not([hidden]) 会把背景会话全漏掉。 */
function localWaitingCount(sessionId: string): number {
  let n = 0
  for (const node of approvalsEl.querySelectorAll<HTMLElement>(".approval, .interaction-card")) {
    if (node.dataset.session === sessionId) n += 1
  }
  return n
}

/** 会话行运行态（无运行态返回 null）：运行中/等待用户输入/在途工具/后台任务数，列表角标与悬浮说明用。
 *
 * 优先用快照 runtime（服务端权威，含后台任务与子会话）；本页自己发起的会话只有本地状态时，
 * 至少把「是否在等用户作答」算准——否则后台任务在等审批时角标只显示「任务运行中」，
 * 用户看不出它正卡在自己这里（多会话并行时这恰是最需要一眼看到的信号）。
 */
function runMarkOf(sessionId: string): { waiting: number; tools: number; bg: number; subs: number } | null {
  const rt = runtimeInfo[sessionId]
  if (rt) return { waiting: rt.pending.length, tools: rt.toolCalls ?? 0, bg: rt.bgTasks.filter((t) => t.status === "running").length, subs: rt.subRuns.length }
  if (!runningSeen.has(sessionId)) return null
  return { waiting: localWaitingCount(sessionId), tools: 0, bg: 0, subs: 0 }
}

/** 结构签名：搜索/批量/折叠态 + 会话行展示字段——任一变化才需要重建列表 DOM。 */
function listSignature(shown: SessionInfo[]): string {
  const rows = shown.map((s) => `${s.id}\u0001${s.name}\u0001${s.pinned ? 1 : 0}\u0001${s.updatedAt}\u0001${signatureOfRun(runMarkOf(s.id))}`).join("\u0002")
  return `${searchQuery}\u0003${batchMode ? 1 : 0}\u0003${[...collapsedGroups].sort().join(",")}\u0003${rows}`
}

function signatureOfRun(m: { waiting: number; tools: number; bg: number; subs: number } | null): string {
  return m ? `${m.waiting}/${m.tools}/${m.bg}/${m.subs}` : ""
}

/** 仅同步激活高亮（不重建列表）：切换会话时列表结构未变，只需换 active 类。 */
function syncActiveRow(activeId: string | null): void {
  for (const li of sessionList.querySelectorAll<HTMLElement>("li[data-sid]")) {
    li.classList.toggle("active", li.dataset.sid === activeId)
  }
}

/* ---------- 会话列表分组（今天/昨天/近7天/更早，组可折叠，状态本地记忆） ---------- */

const GROUP_ORDER = ["pinned", "today", "yesterday", "week", "older"] as const
type GroupKey = (typeof GROUP_ORDER)[number]
const GROUP_LABEL: Record<GroupKey, string> = { pinned: "置顶", today: "今天", yesterday: "昨天", week: "近7天", older: "更早" }
const GROUP_KEY = "gebai.ui.sessionsCollapsed"

/** 列表排序：置顶优先，组内按更新时间倒序（与服务端 listSessions 同口径）。 */
const byPinnedThenUpdated = (a: SessionInfo, b: SessionInfo) =>
  Number(b.pinned ?? false) - Number(a.pinned ?? false) || b.updatedAt - a.updatedAt

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

/** 组头：点击折叠/展开（折叠分组不渲染成员）；行尾勾选框与普通会话行同一套——
 *  点勾选或 Ctrl+点击分类名 = 选中该分类下全部会话，批量模式下点击分类名也是整组切换。 */
function appendGroupHeader(key: GroupKey, count: number): void {
  const li = el("li", "session-group")
  li.dataset.group = key
  const collapsed = !batchMode && collapsedGroups.has(key)
  if (collapsed) li.classList.add("collapsed")
  const chevron = el("span", "sg-chevron", "▾")
  // 勾选框：与普通会话行的 .session-check 同一个类、同一套可见性（默认隐藏、批量模式常驻）
  const box = el("input", "session-check") as HTMLInputElement
  box.type = "checkbox"
  box.dataset.tip = "选中该分类下的全部会话（Ctrl+点击分类名同效果）"
  li.append(chevron, el("span", "sg-label", GROUP_LABEL[key]), el("span", "sg-count", String(count)), box)
  sessionList.appendChild(li)
  // 重渲染后跟着选中集恢复（批量模式下的展开/折叠重渲染不能把分类头的勾选态弄丢）
  syncGroupCheck(li)
}

/** 勾选一个分类下全部会话（已在批量模式或由它进入）；分类全选时变成全取消。 */
function toggleGroupSelection(key: string): void {
  const ids = sidsOfGroup(key)
  const all = selectionState(ids, selected) === "all"
  for (const id of ids) {
    if (all) selected.delete(id)
    else selected.add(id)
  }
  selectAnchor = null
  commitSelection()
}

/** 组头点击（列表委托分发）：批量模式 / Ctrl(+⌘) 点击切换全组选中，否则折叠/展开该组。 */
function toggleGroupHeader(key: GroupKey, forceSelect = false): void {
  if (batchMode || forceSelect) {
    toggleGroupSelection(key)
    return
  }
  if (collapsedGroups.has(key)) collapsedGroups.delete(key)
  else collapsedGroups.add(key)
  persistCollapsedGroups()
  void refreshSessions(lastSessions ?? undefined)
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
  const ico = el("span", "session-ico", "💬")
  const body = el("div", "session-body")
  // 重命名：双击/右键菜单触发 → 内联编辑（回车/失焦保存，Esc 取消）
  const nameEl = el("span", "session-name", s.name) as HTMLElement
  // 选中按钮（多选勾选）：默认隐藏、批量模式常驻；点击切换该行选中（与行的 Ctrl/Shift 点击同一套选中集）
  const box = el("input", "session-check") as HTMLInputElement
  box.type = "checkbox"
  box.checked = batchMode && selected.has(s.id)
  body.append(nameEl)
  // 置顶标识（独立于 .session-name，不受重命名内联编辑替换影响；session-ico 为隐藏的 💬 占位）
  if (s.pinned) li.append(el("span", "session-pin", "📌"))
  // 运行态角标：后台会话也在跑/在等人（快照 runtime 基线 + 事件增量）——不标出则「哪个会话在等我」无从看出
  const mark = runMarkOf(s.id)
  if (mark) {
    const dot = el("span", `session-run${mark.waiting ? " waiting" : ""}`, mark.waiting ? "⌛" : "●")
    dot.title = [mark.waiting ? `等待你的输入（${mark.waiting} 项）` : "任务运行中", mark.tools ? `在途工具 ${mark.tools} 个` : "", mark.bg ? `后台任务 ${mark.bg} 个` : "", mark.subs ? `子会话 ${mark.subs} 个` : ""]
      .filter(Boolean)
      .join(" · ")
    li.append(dot)
  }
  // 选中按钮直接挂行尾（原时间行已移除）
  li.append(ico, body, box)
  sessionList.appendChild(li)
}

/** 事件目标定位到所属 li（会话行或组头）。 */
function liOf(target: EventTarget | null): HTMLElement | null {
  return ((target as HTMLElement | null)?.closest?.("li[data-sid], li.session-group") ?? null) as HTMLElement | null
}

/**
 * 切到已持有容器的会话：**纯显隐切换**（不重拉、不重建 DOM）。
 * 容器里已是这个会话的完整内容（含后台期间到达的工具卡与正文），切回只需：激活显隐 →
 * 恢复该会话的块表与导航段 → 按锚点恢复阅读位置 → 刷新标题/按钮/队列等跟会话联动的 UI。
 * 返回是否走了快速路径（false = 调用方照旧走 loadMessages）。
 */
async function activatePaneFast(sid: string): Promise<boolean> {
  const pane = paneOf(sid)
  if (!pane) return false
  activatePane(sid) // 显隐 + 几何重绑，并成为活动 pane（下面所有转发都指向它）
  restoreSessionViewState(sid)
  // 导航段表是本会话专属的：切走时已被其他会话重建，这里按该 pane 的消息清单重立
  //（没有它则导航列停在上一个会话的位置或不显示）
  const t = pane.chunks
  if (t) buildMsgNav(t.msgs)
  else clearMsgNav()
  const mem = scrollMemory.get(sid)
  if (mem) {
    stopFollowing()
    const top = pane.virtualizer.scrollToAnchor(mem.key, mem.offset)
    if (top !== null) restoreScroll(top)
  } else {
    // 先声明「追最新」意图，再由 sync 完成落底：sync 在 keepBottom 时会把窗口开在末尾、
    // 更新 spacer，并用实测高度把 scrollTop 钉到底（只需一步，不来回掰）。
    // 反过来先 sync 再锁底会把窗口开在旧 scrollTop 上（缓存页残留位置），
    // 随后重挂一轮 spacer 变动→位置被掰动，这就是切回时抖几下的来源。
    lockToBottom()
    pane.virtualizer.sync()
  }
  // 随会话变化的 UI（标题/按钮/排队条/审批卡/导航列/子会话角标/列表高亮）
  // 统一走这一入口：不再每处各自手写一遍（手写就会漏）
  syncSessionScopedUi()
  focusInput()
  // 运行中的会话：容器里已有实时流（本页发起的），无需重新附加；
  // 若本页未接管（别处发起/刷新后）则由 runningAttachHook 补接管
  void runningAttachHook?.(sid)
  return true
}

/** 切换会话（列表委托分发；原逐行 click 主体）。 */
async function activateSession(sid: string): Promise<void> {
  if (batchMode) {
    toggleSelect(sid)
    return
  }
  // 点了就把抽屉收起（窄屏）：已激活的那一条也要收——否则用户点自己所在的会话行后
  // 抽屉摊着不动，看起来像没反应。桌面态无抽屉，这两行是空操作。
  aside.classList.remove("open")
  sidebarBackdropEl.hidden = true
  // 点击当前已激活会话：不切换，不做任何动作（不重载消息）
  if (getCurrentSession()?.id === sid) return
  const s = (lastSessions ?? []).find((x) => x.id === sid)
  if (!s) return
  const prev = getCurrentSession()
  // 切走：记下离场时的视图状态与滚动锚点（下次切回同位置）
  if (prev) saveSessionViewState(prev.id)
  setCurrentSession(s)
  try {
    // 快速路径：目标会话已持有容器（后台运行中，或已停止但在 LRU 缓存里）——直接显隐切换
    if (await activatePaneFast(sid)) {
      // 切换完成后再回收：保留最近用过的若干个已停止容器，超出上限的才释放
      reapIdlePanes()
      return
    }
    // 常规路径（从没打开过、已被 LRU 挤出）：从服务端加载一次
    await refreshSessions()
    await loadMessages(sid)
    restoreSessionViewState(sid)
    reapIdlePanes()
  } catch (err) {
    // 切换失败（网络抖动等）：回滚当前会话标记并提示，视图保持旧会话，避免状态不一致
    toast(`切换失败: ${(err as Error).message}`, "error")
    if (prev) {
      setCurrentSession(prev)
      restoreSessionViewState(prev.id)
    }
  }
}

/** 触屏长按判定阈值（毫秒）：触发后与右键菜单同效。 */
const LONG_PRESS_MS = 480

/** 会话列表事件委托：click/dblclick/contextmenu 各绑一个，行数增长不再线性增加监听器。 */
function bindSessionListDelegation(): void {
  // Shift 单击防拉选：不拦 mousedown 的默认行为，浏览器会把上一次点击到这次点击之间的文字拉成选区
  sessionList.addEventListener("mousedown", (e) => {
    if (e.shiftKey && liOf(e.target)?.dataset.sid) e.preventDefault()
  })
  sessionList.addEventListener("click", (e) => {
    const li = liOf(e.target)
    if (!li) return
    if (li.classList.contains("session-group")) {
      // 分类行：点勾选框或带 Ctrl/Cmd（Ctrl+点击分类名）都是选中该分类下全部会话；
      // 普通点击仍是折叠/展开（批量模式下则全选/全取消该分类）
      const byBox = !!(e.target as HTMLElement).closest(".session-check")
      toggleGroupHeader(li.dataset.group as GroupKey, byBox || e.ctrlKey || e.metaKey)
      return
    }
    const sid = li.dataset.sid
    if (!sid) return
    // 修饰键多选：Shift = 从锚点到该行的区间（Ctrl/Cmd+Shift 并入现有选中），
    // Ctrl/Cmd = 切换该行选中态；勾选框与不带修饰键的点击都落到切换。
    if (e.shiftKey) {
      selectRange(sid, e.ctrlKey || e.metaKey)
      return
    }
    if (e.ctrlKey || e.metaKey) {
      toggleSelect(sid)
      return
    }
    if ((e.target as HTMLElement).closest(".session-check")) toggleSelect(sid)
    else void activateSession(sid)
  })
  sessionList.addEventListener("dblclick", (e) => {
    const li = liOf(e.target)
    const sid = li?.dataset.sid
    if (!li || !sid || batchMode) return
    const s = (lastSessions ?? []).find((x) => x.id === sid)
    const nameEl = li.querySelector<HTMLElement>(".session-name")
    if (s && nameEl) startRename(s, nameEl)
  })
  sessionList.addEventListener("contextmenu", (e) => {
    const li = liOf(e.target)
    const sid = li?.dataset.sid
    if (!li || !sid) return
    const s = (lastSessions ?? []).find((x) => x.id === sid)
    if (!s) return
    e.preventDefault()
    e.stopPropagation()
    openSessionMenu(e, s, li)
  })
  // 触屏长按 = 右键：iOS/Android 都不保证长按会触发 contextmenu（原生菜单又已被全局屏蔽），
  // 不补这条路径的话重命名/删除/多选在手机上不可达。位移超过 8px 视为滚动，取消长按。
  let pressTimer: ReturnType<typeof setTimeout> | null = null
  let pressOrigin: { x: number; y: number } | null = null
  let suppressClick = false
  const cancelPress = () => {
    if (pressTimer) clearTimeout(pressTimer)
    pressTimer = null
    pressOrigin = null
  }
  sessionList.addEventListener("pointerdown", (e) => {
    if (e.pointerType === "mouse") return
    const li = liOf(e.target)
    const sid = li?.dataset.sid
    if (!li || !sid) return
    const s = (lastSessions ?? []).find((x) => x.id === sid)
    if (!s) return
    suppressClick = false
    pressOrigin = { x: e.clientX, y: e.clientY }
    const at = { clientX: e.clientX, clientY: e.clientY }
    pressTimer = setTimeout(() => {
      pressTimer = null
      // 抑制随后的 click（否则松手还会把会话切过去），短时后自动解防
      suppressClick = true
      setTimeout(() => {
        suppressClick = false
      }, 700)
      openSessionMenu(at, s, li)
    }, LONG_PRESS_MS)
  })
  sessionList.addEventListener("pointermove", (e) => {
    if (!pressOrigin) return
    if (Math.abs(e.clientX - pressOrigin.x) > 8 || Math.abs(e.clientY - pressOrigin.y) > 8) cancelPress()
  })
  sessionList.addEventListener("pointerup", cancelPress)
  sessionList.addEventListener("pointercancel", cancelPress)
  // 捕获阶段抢在行点击之前拦下：长按已开菜单时松手不应再切换会话
  sessionList.addEventListener(
    "click",
    (e) => {
      if (!suppressClick) return
      e.preventDefault()
      e.stopPropagation()
    },
    true,
  )
}

/** 运行中上下文大小实时更新（标题栏展示）：更新内存快照；当前会话时刷新标题栏（事件每轮推送）。
 *  ctxCachedTokens（提示词缓存命中，接口返回缓存字段才有值）同点位更新，undefined 清除防陈旧。
 *  注意 currentSession 与 lastSessions 可能是不同对象引用（切换会话后 refreshSessions 重建列表），
 *  两者都需同步，否则 renderHeaderCtx 读到的 currentSession.ctxTokens 停留在切换时的旧值。 */
export function updateSessionCtx(sessionId: string, ctxTokens: number, ctxCachedTokens?: number): void {
  const s = (lastSessions ?? []).find((x) => x.id === sessionId)
  if (s) {
    s.ctxTokens = ctxTokens
    s.ctxCachedTokens = ctxCachedTokens
  }
  const cur = getCurrentSession()
  if (cur?.id === sessionId) {
    cur.ctxTokens = ctxTokens
    cur.ctxCachedTokens = ctxCachedTokens
    renderHeaderCtx()
  }
}

export async function refreshSessions(preloaded?: SessionInfo[]) {
  const seq = ++refreshSeq
  const sessions = preloaded ?? (await client.listSessions())
  if (seq !== refreshSeq) return // 已有更新的刷新请求：本次结果作废
  lastSessions = sessions
  const shown = searchQuery ? sessions.filter((s) => s.name.toLowerCase().includes(searchQuery)) : sessions
  const activeId = getCurrentSession()?.id ?? null
  const sig = listSignature(shown)
  if (sig === lastListSig) {
    // 结构未变（常见于仅切换激活项）：跳过整列重建，只同步高亮
    if (activeId !== lastActiveId) {
      syncActiveRow(activeId)
      lastActiveId = activeId
    }
    return
  }
  lastListSig = sig
  lastActiveId = activeId
  const scrollTop = sessionList.scrollTop // 重渲染保留滚动位置（组折叠切换不跳顶）
  sessionList.innerHTML = ""
  if (searchQuery) {
    // 搜索态：平铺列表（不分组，聚焦过滤结果；置顶项仍置前）
    for (const s of [...shown].sort(byPinnedThenUpdated)) appendSessionLi(s)
  } else {
    // 置顶项脱离时间分组单列「置顶」组；其余按更新时间倒序分组（今天/昨天/近7天/更早）；
    // 折叠组不渲染成员，批量模式强制全展开
    const sorted = [...shown].sort(byPinnedThenUpdated)
    const groups = new Map<GroupKey, SessionInfo[]>()
    for (const s of sorted) {
      const key: GroupKey = s.pinned ? "pinned" : sessionGroup(s.updatedAt)
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

/** 置顶/取消置顶：成功后刷新列表（置顶组置前）；非破坏性操作无确认框，失败静默（与重命名一致）。 */
function togglePin(s: SessionInfo) {
  void (async () => {
    try {
      await client.pinSession(s.id, !s.pinned)
      await refreshSessions()
    } catch {
      /* 忽略 */
    }
  })()
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
/** 右键菜单打开时的键位作用域 id（Esc 只关最上层浮层，不再广播式连关）。 */
let ctxMenuScope: string | null = null
/** 菜单锚点容器（会话列表）：只有它自身（或页面级）滚动才收菜单，见 scroll-dismiss.ts。 */
let ctxMenuHost: HTMLElement | null = null

function closeSessionMenu(): void {
  if (ctxMenuScope) {
    popKeyScope(ctxMenuScope)
    ctxMenuScope = null
  }
  ctxMenu?.remove()
  ctxMenu = null
  ctxMenuHost = null
}

function openSessionMenu(e: { clientX: number; clientY: number }, s: SessionInfo, li: HTMLElement): void {
  closeSessionMenu()
  const menu = el("div", "session-ctx-menu") as HTMLDivElement
  const items: Array<{ label: string; danger?: boolean; action: () => void }> = []
  if (batchMode) {
    items.push({ label: selected.has(s.id) ? "取消选择" : "选择", action: () => toggleSelect(s.id) })
  } else {
    items.push({ label: "选中", action: () => toggleSelect(s.id) })
  }
  items.push(
    // 复制会话 ID（定位问题/反馈用）：任意条目可复制，不限于当前会话
    { label: "复制会话 ID", action: () => { void copyTextToClipboard(s.id).then((ok) => toast(ok ? `已复制会话 ID: ${s.id}` : "复制失败", ok ? "ok" : "error")) } },
    { label: s.pinned ? "取消置顶" : "置顶", action: () => togglePin(s) },
    { label: "重命名", action: () => startRename(s, li.querySelector<HTMLElement>(".session-name")!) },
    // 导出：Markdown 阅读版（原有）+ JSON 往返格式（可导入回歌白，与轮盘导入联动）
    { label: "导出 Markdown", action: () => { void exportSession(s.id).catch((err) => toast(`导出失败: ${(err as Error).message}`, "error")) } },
    { label: "导出会话", action: () => { void exportSessionJson(s.id).then(() => toast("已导出会话文件", "ok")).catch((err) => toast(`导出失败: ${(err as Error).message}`, "error")) } },
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
  ctxMenuHost = sessionList
  // 定位 + 视口边缘翻转（菜单不超出可视区）
  const rect = menu.getBoundingClientRect()
  menu.style.left = `${Math.max(8, Math.min(e.clientX, window.innerWidth - rect.width - 8))}px`
  menu.style.top = `${Math.max(8, Math.min(e.clientY, window.innerHeight - rect.height - 8))}px`
  ctxMenuScope = nextScopeId("main.sessionMenu")
  pushKeyScope({
    id: ctxMenuScope,
    bindings: [{ id: "main.sessionMenu.esc", keys: "Esc", label: "关闭会话菜单", group: "main.overlay", run: () => closeSessionMenu() }],
  })
}

/* ---------- 空状态 ---------- */

let searchQuery = ""

/** 进入空白草稿页（不创建会话）：清空消息视图与输入，首条消息发送时才真正创建会话，
 *  避免点「新会话」即落盘产生大量空会话。 */
export function enterDraftView(): void {
  setCurrentSession(null)
  aside.classList.remove("open")
  sidebarBackdropEl.hidden = true
  deactivatePane() // 草稿页不属于任何会话：隐藏全部会话视图（容器与后台内容保留）
  resetMsgWindow()
  clearMsgNav()
  clearUnread()
  showEmptyState()
  input.value = ""
  autosize()
  setPendingFiles([])
  renderAttachments()
  // 随会话变化的 UI（无当前会话→各项回到空态）统一走同一入口
  syncSessionScopedUi()
  focusInput()
}

/**
 * 显示空白页（「歌未竟 东方白」+ 快捷按钮）。
 *
 * 挂载位置按当前视图分两种：
 * - 活动 pane 存在且该会话无消息：挂到该会话的滚动容器尾部（窗口化的尾部活动区）；
 * - 无 pane（草稿页、登出）：**挂到宿主 #messages**——消息列按会话分容器后，草稿页没有任何
 *   容器，appendTail 会转发到空 pane 而静默丢弃，空状态于是整个不显示（歌未竟 东方白消失就是这么来的）。
 *
 * 始终只维护**一个**空状态节点（见 emptyStateNode）：反复进出草稿页不能堆出多份。
 */
function showEmptyState() {
  const node = emptyStateNode()
  node.hidden = false
  mountEmptyState(node)
}

/** 空状态的单例节点（首次创建时建好内容；不依赖 state 里的引用，避免被置空后重复创建）。 */
let emptyStateEl: HTMLElement | null = null
function emptyStateNode(): HTMLElement {
  if (emptyStateEl) return emptyStateEl
  const node = el("div", "empty-state")
  node.append(el("div", "es-slogan", "歌未竟 东方白"))
  const tips = el("div", "es-suggestions")
  renderShortcutButtons(tips)
  node.appendChild(tips)
  emptyStateEl = node
  setEmptyState(node)
  return node
}

/** 把空状态放到当前视图该在的位置（切会话/进草稿页时重放）。 */
function mountEmptyState(node: HTMLElement): void {
  if (activePane()) msgWindow.appendTail(node)
  else if (node.parentElement !== msgEl) msgEl.appendChild(node)
}

function hideEmptyState() {
  const e = emptyStateEl ?? getEmptyState()
  if (!e) return
  e.hidden = true
  // 挂在宿主 #messages 上时（草稿页形态）还得多一步移出：宿主是容器栈，
  // 残留的隐藏子元素与 pane 共存时会让布局与滚动几何多算一份。
  if (e.parentElement === msgEl) e.remove()
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

/* ---------- 自动标题：首条消息发送即以其输入命名（main 发送时点触发，任务结束兜底重试） ---------- */

export async function maybeAutoTitle(sessionId: string) {
  if (autoNamed.has(sessionId)) return
  // 名称判定优先走快照（避免为取标题全量拉取会话消息）；快照无此会话时才回退 getSession
  const snapInfo = client.getSnapshot().sessions.find((s) => s.id === sessionId)
  let session: SessionDetail | null = null
  if (snapInfo && snapInfo.name !== "新会话") {
    autoNamed.add(sessionId)
    return
  }
  if (!snapInfo) {
    session = await client.getSession(sessionId).catch(() => null)
    if (!session) return
    if (session.name !== "新会话") {
      // 已自定义标题（如创建时指定），不再自动命名
      autoNamed.add(sessionId)
      return
    }
  }
  // 首条输入优先取内存记录（发送时点即有，零额外请求）；缺失（页面刷新后补命名）时回退历史首条用户消息（子会话执行存档不算）
  const first =
    firstInputOf(sessionId) ??
    (session ?? (await client.getSession(sessionId).catch(() => null)))?.messages?.find((m) => m.role === "user" && !m.subSession && !isEngineNoteMsg(m) && m.content?.trim())?.content
  if (!first) return
  const compact = first.replace(/\s+/g, " ").trim()
  // 落盘标题截 50 字符（超出省略号）：侧栏/标题栏按容器宽度自行省略，此处只防超长输入整段入库
  const title = compact.slice(0, 50) + (compact.length > 50 ? "…" : "")
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
    /* 改名失败（如会话已删除）静默忽略；不标记 autoNamed，任务结束兜底重试 */
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
    // 引擎提示按来源标注（与服务端 engineNote 取值一一对应）；其余按角色
    const note = engineNoteOf(m)
    const tag = note
      ? note === "task"
        ? "🗂️ 任务"
        : note === "cron"
          ? "⏰ 定时任务"
          : note === "subsession"
            ? "🌿 分支合入"
            : note === "interrupted"
              ? "⚠️ 任务中断"
              : "⚙️ 引擎提示"
      : m.role === "user"
        ? "🧑 用户"
        : m.role === "assistant"
          ? "🤖 助手"
          : m.role === "system"
            ? "📋 系统"
            : `🔧 工具${m.name ? `（${m.name}）` : ""}`
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

/* ---------- 会话导出（JSON 往返格式）与导入：纯逻辑在 session-io.ts，此处为 DOM 流程 ---------- */

/** 导出会话为 JSON 导出文件（单会话「批量包」；与 Markdown 导出并存：给人看的 vs 给歌白吃的）。
 *  文件名以**会话 ID** 命名（标题作文件名不可靠：超长/特殊字符/重名/Windows 保留名）。 */
export async function exportSessionJson(sessionId: string): Promise<void> {
  const detail = await client.getSession(sessionId)
  downloadJson(singleExportFileName(sessionId), buildExportFile([detailToImportData(detail)]))
}

/** 批量导出选中会话（单个 JSON「批量包」文件；勾选清单由调用方给，逐个拉取正文）。
 *  批量无单一 ID 可依，文件名按「条数 + 日期」（选中项全为会话名时用户不需要从文件名认它们）。 */
export async function exportSessionsJson(ids: string[]): Promise<void> {
  if (!ids.length) return
  const details = await Promise.all(ids.map((id) => client.getSession(id)))
  downloadJson(batchExportFileName(details.length), buildExportFile(details.map(detailToImportData)))
}

/** 导入会话文件（轮盘入口）：多文件 + 单文件多会话统一为载荷数组 → 确认 → 逐个导入。
 *  成功后刷新列表并切到最后一个导入成功的会话（导入即达：不需再去找刚导入的那条）。
 *  返回导入结果（成功数/失败数/最后成功的会话 id），供调用方 toast。 */
export async function importSessionFiles(files: File[]): Promise<{ ok: number; failed: number; lastId: string | null }> {
  // 解析阶段：任一文件损坏即整体报错中止（部分导入后才发现坏文件，用户难以分辨哪些已导入）
  const payloads: import("@gebai/sdk").SessionImportData[] = []
  for (const f of files) {
    payloads.push(...parseImportText(await f.text()))
  }
  const names = payloads.map((s) => s.name)
  if (!(await confirmDialog({ title: `导入 ${payloads.length} 个会话`, okLabel: "导入", list: names }))) {
    return { ok: 0, failed: 0, lastId: null }
  }
  let ok = 0
  let failed = 0
  let lastId: string | null = null
  for (const data of payloads) {
    try {
      const s = await client.importSession(data)
      ok++
      lastId = s.id
    } catch {
      failed++
    }
  }
  if (ok) {
    await refreshSessions()
    if (lastId) {
      const s = (await client.listSessions()).find((x) => x.id === lastId) ?? null
      if (s) {
        const prev = getCurrentSession()
        if (prev) saveSessionViewState(prev.id)
        setCurrentSession(s)
        await loadMessages(s.id)
      }
    }
  }
  return { ok, failed, lastId }
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
  bindSessionListDelegation()
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
  // 批量导出（JSON 批量包，与右键「导出会话」/轮盘导入同一格式）：逐个拉正文打包单文件下载
  batchExportBtn.onclick = () => {
    const ids = [...selected]
    if (!ids.length) return
    batchExportBtn.disabled = true
    void exportSessionsJson(ids)
      .then(() => toast(`已导出 ${ids.length} 个会话`, "ok"))
      .catch((err) => toast(`导出失败: ${(err as Error).message}`, "error"))
      .finally(() => {
        batchExportBtn.disabled = selected.size === 0
      })
  }
  // 导入会话（轮盘入口也触发这里）：隐藏 input[type=file]，选文件后走统一导入流程
  const importInput = document.getElementById("session-import-input") as HTMLInputElement | null
  const importBtn = document.getElementById("import-session-btn")
  const runImport = (files: File[]) => {
    void importSessionFiles(files)
      .then((r) => {
        if (r.ok && !r.failed) toast(`已导入 ${r.ok} 个会话`, "ok")
        else if (r.ok && r.failed) toast(`已导入 ${r.ok} 个、失败 ${r.failed} 个`, "error")
        else if (r.failed) toast(`导入失败（${r.failed} 个）`, "error")
      })
      .catch((err) => toast(`导入失败: ${(err as Error).message}`, "error"))
  }
  if (importBtn) {
    importBtn.addEventListener("click", () => importInput?.click())
  }
  if (importInput) {
    importInput.addEventListener("change", () => {
      const files = [...importInput.files ?? []]
      importInput.value = "" // 允许重复选同一文件（同名再选也触发 change）
      if (files.length) runImport(files)
    })
  }
  // 右键菜单关闭：任意点击 / 新右键 / 锚点容器（会话列表）滚动 / 窗口缩放（Esc 见 openSessionMenu 的作用域绑定）。
  // 滚动不按「任意容器一律关」：生成中的消息流为咬住底部每帧写 scrollTop，那是另一个容器、
  // 菜单并不会跟着漂移，一律关会让菜单在运行中会话里刚打开就被冲掉（见 scroll-dismiss.ts）
  document.addEventListener("click", () => closeSessionMenu())
  document.addEventListener("contextmenu", () => closeSessionMenu(), true) // 捕获：新右键先关旧菜单，再走 li 打开新菜单
  document.addEventListener("scroll", (e) => { if (dismissOnScroll(e.target, ctxMenuHost)) closeSessionMenu() }, true)
  window.addEventListener("resize", () => closeSessionMenu())
  // 新会话：进入空白草稿页（不立即创建会话——避免落盘大量空会话，首条消息发送时才真正创建）；
  // 已处于草稿页时无操作（防误触快捷键清掉正在输入的草稿）
  const newSessionView = () => {
    if (isDraftView()) return
    const prev = getCurrentSession()
    if (prev) saveSessionViewState(prev.id)
    enterDraftView()
    void refreshSessions() // 清除列表中旧会话的激活高亮
  }
  newSessionBtn.onclick = newSessionView
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
      setDrawerOpen(!aside.classList.contains("open"))
    } else {
      const collapsed = document.body.classList.toggle("sidebar-collapsed")
      try {
        localStorage.setItem(SIDEBAR_KEY, collapsed ? "1" : "")
      } catch {
        /* ignore */
      }
    }
  }
  // 窄屏抽屉：展开时铺遮罩（点遮罩收起），收起时卸遮罩——遮罩挡住主区，
  // 否则抽屉展开后点主区仍会命中下面的消息流/输入框
  const setDrawerOpen = (open: boolean) => {
    aside.classList.toggle("open", open)
    sidebarBackdropEl.hidden = !open
  }
  sidebarBackdropEl.onclick = () => setDrawerOpen(false)
  sidebarToggle.onclick = toggleSidebar
  // 窗口拉宽到桌面态：抽屉态（含遮罩）自动解除，避免桌面下残留一个固定定位的侧栏
  window.addEventListener("resize", () => {
    if (!narrowScreen() && !sidebarBackdropEl.hidden) setDrawerOpen(false)
  })
  // 全局快捷键（任意焦点可用，均拦截默认行为；键位族与守卫规则见 keymap.ts）：
  // Ctrl+B 切换会话列表（接管浏览器书签行为）、Alt+N 进入空白草稿页（批量模式下新会话按钮禁用，快捷键随按钮态失效）。
  // 为什么不是 Ctrl+N：那是 Chromium 的保留命令（浏览器自己开新窗口，页面收不到按键、preventDefault 无效），
  // 用它等于做出一套「浏览器形态按不动」的快捷键——键位只有一套，故取浏览器里同样可达的 Alt+N。
  // Alt 组合在上述与工作台里一律走**捕获阶段**：浏览器菜单栏/输入法可能先碰它，捕获才接得住。
  mainKeymap.addAll([
    {
      id: "main.session.toggleSidebar",
      keys: "Ctrl+B",
      label: "折叠 / 展开会话列表",
      group: "main.session",
      browser: "override",
      focus: FOCUS_WITH_INPUT,
      run: () => toggleSidebar(),
    },
    {
      id: "main.session.new",
      keys: "Alt+N",
      label: "新建会话（进入草稿页）",
      group: "main.session",
      phase: "capture",
      focus: FOCUS_WITH_INPUT,
      when: () => !newSessionBtn.disabled,
      run: () => newSessionView(),
    },
  ])
}
