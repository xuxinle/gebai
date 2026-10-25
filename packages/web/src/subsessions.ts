/**
 * 子会话观察面板（右侧抽屉）：把 `subsession_run` 的全部内容从消息流里搬出来集中观察。
 *
 * 为什么不在消息流里就地展开：一次子会话运行的过程是「推理 + 工具卡 + 多轮回复」，展开后
 * 长度可达数屏；它一插进消息流，父会话自己的叙述就被顶散，并发多个 run 时更是几条流互相
 * 交错，既看不出哪个还在跑，也看不出各自跑到哪。消息流只留**一行入口**（`subSessionEntry`），
 * 过程、报告、阶段性合入全部进本面板。
 *
 * 结构：
 * - 左列 = 本次会话的全部运行（运行中在前，按结束时间倒序），每行一个状态点 + 名称 + 状态摘要；
 * - 右列 = 选中运行的详情：任务指令 / 最终报告（markdown）/ 阶段性合入记录（markdown）/
 *   完整执行过程（推理、工具卡、回复——与主循环同构的渲染，实时跟随）。
 *
 * 过程的 DOM 归属：每个 run 的过程区是一棵**常驻 DOM**（`body`）。未选中的 run 的 body 挂在
 * 面板内的隐藏 staging 里而不是被丢弃——`events.ts` 的工具卡与流式渲染要求目标节点在文档中
 * （`isConnected`），丢弃就等于后台 run 的过程静默丢失；切换选中只是把 body 移进详情区。
 */

import type { SubSessionArchive, SubSessionEntry } from "@gebai/sdk"
import { appendMsg, bindSessionScroll } from "./messages"
import { markdownBlock } from "./markdown"
import { el, getCurrentSession } from "./state"
import { formatElapsed, runStateLine, sortRuns, type SubRunStatus } from "./subsessions-core"

/** 一个子会话运行在面板里的记录（含过程区 DOM，跨会话切换保留）。 */
interface RunRecord {
  sessionId: string
  runId: string
  name: string
  model?: string
  agents: string[]
  input: string
  status: SubRunStatus
  startedAt: number
  endedAt?: number
  /** 模型回复轮次（按 messageId 变化计数，不用 start 事件——服务端每轮重推 start）。 */
  rounds: number
  toolCalls: number
  /** 阶段性合入次数（subsession_merge 交出）。 */
  merges: number
  /** 最近一轮回复的 messageId（轮次计数用）。 */
  lastMessageId?: string
  output: string
  /** 阶段性合入全文（按提交顺序，markdown 渲染）。 */
  mergeNotes: string[]
  /** 过程消息区（推理/工具卡/回复）：常驻 DOM，选中时移入详情区。 */
  body: HTMLElement
  /** 历史存档的原始消息（懒渲染进 body）。 */
  archiveMessages?: SubSessionEntry[]
  archiveRendered: boolean
  /** 最近一次写入各状态位的文案（相同则跳过 DOM 写入）。 */
  stateText: string
}

/** 会话 → (runId → 记录)。按会话隔离：切换会话时面板只展示当前会话的运行。 */
const store = new Map<string, Map<string, RunRecord>>()
/** 当前选中的 runId。 */
let selected: string | null = null
/** 面板是否展开（内存态：刷新页面回到收起）。 */
let open = false

function sessionRuns(sessionId: string): Map<string, RunRecord> {
  let m = store.get(sessionId)
  if (!m) {
    m = new Map()
    store.set(sessionId, m)
  }
  return m
}

function overlayEl(): HTMLElement | null {
  return document.getElementById("subsessions-overlay")
}
function listEl(): HTMLElement | null {
  return document.getElementById("subsessions-list")
}
function detailEl(): HTMLElement | null {
  return document.getElementById("subsessions-detail")
}
function stageEl(): HTMLElement | null {
  return document.getElementById("subsessions-stage")
}
function buttonEl(): HTMLElement | null {
  return document.getElementById("subsessions-btn")
}

/** 面板是否展开。 */
export function isSubSessionsOpen(): boolean {
  return open
}

/** 该会话是否已有运行记录（消息流入口与按钮角标用）。 */
export function hasSubSessions(sessionId: string): boolean {
  return (store.get(sessionId)?.size ?? 0) > 0
}

function recordOf(sessionId: string, runId: string): RunRecord | undefined {
  return store.get(sessionId)?.get(runId)
}

/**
 * 建立/更新一个运行记录（幂等：服务端每轮重推 start，同 runId 重复调用只刷新元信息）。
 * 返回记录供调用方取过程区（流式渲染的目标）。
 */
export function startRun(opts: { sessionId: string; runId: string; name?: string; model?: string; agents: string[]; input: string }): RunRecord {
  const runs = sessionRuns(opts.sessionId)
  const existing = runs.get(opts.runId)
  if (existing) {
    // 元信息以最新一次为准（重推携带更完整的 agents/input），状态与过程不动
    if (opts.name) existing.name = opts.name
    if (opts.model) existing.model = opts.model
    if (opts.agents.length) existing.agents = opts.agents
    if (opts.input) existing.input = opts.input
    syncRun(existing)
    return existing
  }
  const body = el("div", "subsession-process")
  body.dataset.runId = opts.runId
  bindSessionScroll(body)
  const rec: RunRecord = {
    sessionId: opts.sessionId,
    runId: opts.runId,
    name: opts.name || "子会话",
    model: opts.model,
    agents: opts.agents,
    input: opts.input,
    status: "running",
    startedAt: Date.now(),
    rounds: 0,
    toolCalls: 0,
    merges: 0,
    output: "",
    mergeNotes: [],
    body,
    archiveRendered: true, // 实时运行的过程由流式渲染直接写入
    stateText: "",
  }
  runs.set(opts.runId, rec)
  // 未选中的过程区先驻留隐藏 staging（保持 isConnected，后台 run 的工具卡照常落 DOM）
  stageEl()?.appendChild(body)
  if (open) renderList()
  syncButton()
  if (!selected) select(opts.runId)
  syncRun(rec)
  return rec
}

/** 记录一轮模型回复（messageId 变化即新一轮）：流式文本到达时调用。 */
export function noteRound(sessionId: string, runId: string, messageId: string): void {
  const rec = recordOf(sessionId, runId)
  if (!rec) return
  if (messageId && rec.lastMessageId !== messageId) {
    rec.lastMessageId = messageId
    if (rec.rounds > 0 || rec.status === "running") rec.rounds += 1
    syncRun(rec)
  }
}

/** 记录一次工具调用（工具卡片落到过程区时调用）。 */
export function noteToolCall(sessionId: string, runId: string): void {
  const rec = recordOf(sessionId, runId)
  if (!rec) return
  rec.toolCalls += 1
  syncRun(rec)
}

/** 是否为**阶段性**合入（而非最终报告合入）：合并消息的内容头由服务端构造，
 *  阶段性为「阶段性合入」（含摘要变体），最终为「已合并」。最终报告已由存档 output 承担
 *  （详情里的「最终报告」区），再计一次合入会让面板重复展示同一份内容。 */
export function isStageMerge(text: string): boolean {
  return /【智体·子会话[^】]*阶段性合入/.test(text)
}

/** 记录一次阶段性合入（subsession_merge 交出 content）：全文留档，面板内 markdown 展示。
 *  最终报告合入不计——它就是本 run 的输出（详情「最终报告」区），两处都排会让同一份内容出现两次。 */
export function noteMerge(sessionId: string, runId: string, text: string): void {
  if (!isStageMerge(text)) return
  const rec = recordOf(sessionId, runId)
  if (!rec) return
  rec.merges += 1
  if (text.trim()) rec.mergeNotes.push(text.trim())
  syncRun(rec)
  if (open && selected === runId) renderDetail()
}

/** 历史回放：从落盘的合入消息认回阶段合入（一个 run 可多次交出阶段性成果）。
 *  实时路径只计数不断内容（内容已随消息落盘），刷新后靠这一条重建——否则合入记录只在实时会话里存在。 */
export function mergeFromMessage(sessionId: string, meta: { runId: string; name?: string; model?: string }, text: string): void {
  if (!meta.runId || !isStageMerge(text)) return
  if (!recordOf(sessionId, meta.runId)) {
    startRun({ sessionId, runId: meta.runId, name: meta.name, model: meta.model, agents: [], input: "" })
    finishRun(sessionId, meta.runId, "")
  }
  noteMerge(sessionId, meta.runId, text)
}

/** 运行结束：定稿输出与耗时（空输出 = 失败/取消/终止，展示为「无返回」）。 */
export function finishRun(sessionId: string, runId: string, output: string): void {
  const rec = recordOf(sessionId, runId)
  if (!rec) return
  rec.status = "done"
  rec.endedAt = Date.now()
  rec.output = output
  if (rec.rounds === 0 && output.trim()) rec.rounds = 1 // 无流式文本（final_only）时至少记一轮
  syncRun(rec)
  if (open) {
    renderList()
    if (selected === runId) renderDetail()
  }
}

/**
 * 登记历史存档（工具调用记录的 `subSessionArchive` / 合并消息携带的存档）：
 * 已存在同名运行（实时跑过或已渲染）时只补存档、不改状态；否则建一条已结束记录，过程按需懒渲染。
 */
export function registerArchive(sessionId: string, archive: SubSessionArchive): RunRecord {
  const runs = sessionRuns(sessionId)
  const existing = runs.get(archive.runId)
  const startedAt = Date.now()
  const rec: RunRecord =
    existing ??
    (() => {
      const body = el("div", "subsession-process")
      body.dataset.runId = archive.runId
      bindSessionScroll(body)
      const created: RunRecord = {
        sessionId,
        runId: archive.runId,
        name: archive.subsession?.name || "子会话",
        model: archive.subsession?.model,
        agents: archive.agents ?? [],
        input: archive.input ?? "",
        status: "done",
        startedAt,
        // 历史存档没有起止时刻：不填 endedAt（否则摘要会显示假的「<1s」耗时）
        rounds: 0,
        toolCalls: 0,
        merges: 0,
        output: archive.output ?? "",
        mergeNotes: [],
        body,
        archiveRendered: false,
        stateText: "",
      }
      runs.set(archive.runId, created)
      stageEl()?.appendChild(body)
      return created
    })()
  // 过程区归属：实时跑过的记录（startRun 建的）已由流式渲染填好，存档**不覆盖**它（等价内容，覆盖只会把实时
  // 卡片换成重算的一份）；仅历史记录（无实时过程）才把存档当过程源，按需懒渲染
  if (!rec.archiveRendered) {
    if (!rec.archiveMessages?.length) rec.archiveMessages = archive.messages ?? []
  } else if (!rec.archiveMessages?.length) {
    rec.archiveMessages = archive.messages ?? [] // 留作嵌套运行回放（不参与本区渲染）
  }
  if (rec.status === "done" && !rec.output) rec.output = archive.output ?? ""
  if (!rec.input) rec.input = archive.input ?? ""
  if (!rec.agents.length && archive.agents?.length) rec.agents = archive.agents
  if (!rec.model && archive.subsession?.model) rec.model = archive.subsession.model
  if (!rec.name && archive.subsession?.name) rec.name = archive.subsession.name
  syncRun(rec)
  if (open) renderList()
  syncButton()
  return rec
}

/** 登记一条历史过程消息（旧版数据：过程消息本身落盘、无存档字段）。 */
export function appendArchiveMessage(sessionId: string, runId: string, meta: { name?: string; agents?: string[]; input?: string }, entry: SubSessionEntry): RunRecord {
  const rec =
    recordOf(sessionId, runId) ??
    registerArchive(sessionId, { runId, agents: meta.agents ?? [], input: meta.input ?? "", output: "", messages: [], ...(meta.name ? { subsession: { name: meta.name } } : {}) })
  ;(rec.archiveMessages ??= []).push(entry)
  rec.archiveRendered = false
  return rec
}

/** 过程区（流式渲染/工具卡目标）：记录不存在返回 null（无 start 的异常事件）。
 *  过程区常驻于面板（不随会话切换/选中切换销毁），因此切回会话时仍可向同一区追加。 */
export function processArea(sessionId: string, runId: string): HTMLElement | null {
  return recordOf(sessionId, runId)?.body ?? null
}

/** 取过程区，记录缺失（start 事件已在断线前投递）时惰性建一条运行中记录。 */
export function ensureProcessArea(sessionId: string, runId: string): HTMLElement {
  return recordOf(sessionId, runId)?.body ?? startRun({ sessionId, runId, agents: [], input: "" }).body
}

/** 状态位同步：消息流入口行与面板列表行（同一 run 可能在多个位置各有入口）。
 *  列表行存在则**原地更新**（避免整列重建导致展开态/滚动位置丢失），新 run 才重建整列。 */
function syncRun(rec: RunRecord): void {
  const text = runStateLine(rec)
  const changed = text !== rec.stateText
  rec.stateText = text
  if (changed) {
    for (const node of entryNodes(rec)) {
      node.dataset.state = rec.status
      const stateEl = node.querySelector<HTMLElement>(".subsession-entry-state")
      if (stateEl) stateEl.textContent = text
    }
  }
  if (!open) return
  const row = listEl()?.querySelector<HTMLElement>(`[data-run-row="${rec.runId}"]`)
  if (row) syncRowState(rec)
  else renderList()
}

/** 消息流入口节点（同 run 可能出现在窗口化块的多个已挂载块里）。 */
function entryNodes(rec: RunRecord): HTMLElement[] {
  const out: HTMLElement[] = []
  for (const node of document.querySelectorAll<HTMLElement>("[data-subsession-entry]")) {
    if (node.dataset.runId === rec.runId && node.dataset.sessionId === rec.sessionId) out.push(node)
  }
  return out
}

/** 列表行的状态文本原地更新（避免整列重建导致展开态/滚动位置丢失）。 */
function syncRowState(rec: RunRecord): void {
  const row = listEl()?.querySelector<HTMLElement>(`[data-run-row="${rec.runId}"]`)
  if (!row) return
  row.dataset.state = rec.status
  const stateEl = row.querySelector<HTMLElement>(".subsessions-row-state")
  if (stateEl) stateEl.textContent = rec.stateText
}

/* ---------- 面板渲染 ---------- */

function currentRuns(): RunRecord[] {
  const id = getCurrentSession()?.id
  if (!id) return []
  return sortRuns([...(store.get(id)?.values() ?? [])])
}

function selectedRecord(): RunRecord | null {
  const id = getCurrentSession()?.id
  if (!id || !selected) return null
  return recordOf(id, selected) ?? null
}

export function openSubSessions(runId?: string): void {
  const overlay = overlayEl()
  if (!overlay) return
  open = true
  overlay.hidden = false
  if (runId) selected = runId
  else if (!selectedRecord()) selected = currentRuns()[0]?.runId ?? null
  renderList()
  renderDetail()
  syncButton()
}

export function closeSubSessions(): void {
  const overlay = overlayEl()
  if (overlay) overlay.hidden = true
  open = false
  syncButton()
}

/** 选中一个运行并展开面板（消息流入口、合入通知的「查看过程」都走这里）。 */
export function focusRun(sessionId: string, runId: string): void {
  if (getCurrentSession()?.id !== sessionId) return // 切走的会话：入口不可点（DOM 已卸载）
  selected = runId
  openSubSessions(runId)
}

function select(runId: string): void {
  selected = runId
  if (open) {
    renderList()
    renderDetail()
  }
}

function renderList(): void {
  const list = listEl()
  if (!list) return
  list.replaceChildren()
  const runs = currentRuns()
  if (!runs.length) {
    list.appendChild(el("div", "subsessions-empty", "本会话暂无子会话运行"))
    return
  }
  for (const rec of runs) {
    const row = el("button", "subsessions-row")
    row.type = "button"
    row.dataset.runRow = rec.runId
    row.dataset.state = rec.status
    if (rec.runId === selected) row.classList.add("active")
    const head = el("div", "subsessions-row-head")
    head.append(
      el("span", "subsessions-dot"),
      el("span", "subsessions-row-name", `🌿 ${rec.name}${rec.model ? `（${rec.model}）` : ""}`),
    )
    if (rec.status === "running") head.appendChild(el("span", "subsessions-tag", "运行中"))
    if (rec.merges > 0) head.appendChild(el("span", "subsessions-tag merges", `合入 ${rec.merges}`))
    row.appendChild(head)
    row.appendChild(el("div", "subsessions-row-state", rec.stateText))
    if (rec.agents.length) row.appendChild(el("div", "subsessions-row-agents", `⚙ ${rec.agents.join(" + ")}`))
    row.onclick = () => select(rec.runId)
    list.appendChild(row)
  }
}

/** 过程区挂载：选中的 run 的过程区移进详情，其余回到隐藏 staging（不销毁，保持 isConnected）。 */
function mountProcess(rec: RunRecord, host: HTMLElement | null): void {
  if (host) {
    host.appendChild(rec.body)
    return
  }
  const stage = stageEl()
  if (stage && rec.body.parentElement !== stage) stage.appendChild(rec.body)
}

function renderDetail(): void {
  const box = detailEl()
  if (!box) return
  // 上一个选中运行的过程区先退回 staging：留在将被清空的详情链里就脱离了文档
  // （isConnected 变假 → 后台 run 的工具卡/流式渲染静默丢弃）
  for (const node of Array.from(box.querySelectorAll<HTMLElement>(".subsession-process"))) stageEl()?.appendChild(node)
  const rec = selectedRecord()
  box.replaceChildren()
  if (!rec) {
    box.appendChild(el("div", "subsessions-empty", "选择左侧的运行查看过程与报告"))
    return
  }
  const head = el("div", "subsessions-detail-head")
  const statusText = rec.status === "running" ? "运行中" : rec.endedAt !== undefined ? `完成 · ${formatElapsed(rec.endedAt - rec.startedAt)}` : "完成"
  head.append(
    el("span", "subsessions-detail-name", `🌿 ${rec.name}${rec.model ? `（${rec.model}）` : ""}`),
    el("span", `subsessions-status ${rec.status}`, statusText),
  )
  if (rec.agents.length) head.appendChild(el("span", "subsessions-detail-agents", `⚙ ${rec.agents.join(" + ")}`))
  box.appendChild(head)

  if (rec.input.trim()) box.appendChild(section("任务指令", el("div", "subsessions-input", rec.input)))

  if (rec.output.trim()) box.appendChild(section("最终报告", mdBlock(rec.output)))
  else if (rec.status === "done") box.appendChild(section("最终报告", el("div", "subsessions-muted", "（无返回——失败、取消或被终止；过程仍在下方）")))

  for (const [i, note] of rec.mergeNotes.entries()) {
    const wrap = el("div", "subsessions-merge")
    wrap.appendChild(el("div", "subsessions-merge-head", `阶段性合入 ${i + 1}`))
    wrap.appendChild(mdBlock(note))
    box.appendChild(section("阶段性合入", wrap, true))
  }

  // 过程区：历史存档懒渲染（只渲染一次，之后随实时事件继续追加）
  if (!rec.archiveRendered) {
    renderArchive(rec)
    rec.archiveRendered = true
  }
  const processHost = el("div", "subsessions-process-host")
  box.appendChild(section("执行过程", processHost))
  mountProcess(rec, processHost)
}

/** 详情分区（标题 + 内容），needWrap 时内容已在自带容器里。 */
function section(title: string, content: HTMLElement, _needWrap = false): HTMLElement {
  const wrap = el("div", "subsessions-section")
  wrap.appendChild(el("div", "subsessions-section-title", title))
  wrap.appendChild(content)
  return wrap
}

function mdBlock(text: string): HTMLElement {
  const div = el("div", "subsessions-md")
  div.appendChild(markdownBlock(text))
  return div
}

/** 历史存档过程渲染（嵌套子会话递归：内层运行注册成独立记录 + 一行入口）。 */
function renderArchive(rec: RunRecord): void {
  const msgs = rec.archiveMessages ?? []
  if (!msgs.length) {
    if (!rec.body.childNodes.length) rec.body.appendChild(el("div", "subsessions-muted", "（无过程记录）"))
    return
  }
  rec.body.replaceChildren()
  for (const am of msgs) {
    if (am.subSessionArchive) {
      const nested = registerArchive(rec.sessionId, am.subSessionArchive)
      rec.body.appendChild(nestedEntryRow(rec.sessionId, nested))
      continue
    }
    appendMsg(
      { id: `arch-${rec.runId}-${Math.random().toString(36).slice(2, 8)}`, role: am.role, name: am.name, content: am.content, toolCalls: am.toolCalls, arguments: am.arguments, createdAt: Date.now() },
      false,
      rec.body,
    )
  }
}

/** 嵌套运行的一行入口（面板内部用，与消息流入口同款）。 */
function nestedEntryRow(sessionId: string, rec: RunRecord): HTMLElement {
  const row = el("button", "subsession-entry nested")
  row.type = "button"
  row.dataset.subsessionEntry = ""
  row.dataset.runId = rec.runId
  row.dataset.sessionId = sessionId
  const name = el("span", "subsession-entry-name", `🌿 ${rec.name}${rec.model ? `（${rec.model}）` : ""}`)
  const stateEl = el("span", "subsession-entry-state", rec.stateText)
  row.append(name, stateEl)
  row.onclick = () => select(rec.runId)
  return row
}

/* ---------- 入口与事件装配 ---------- */

/** 当前会话运行数角标（标题栏按钮）。 */
function syncButton(): void {
  const btn = buttonEl()
  if (!btn) return
  const running = currentRuns().filter((r) => r.status === "running").length
  const total = currentRuns().length
  btn.classList.toggle("has-running", running > 0)
  btn.classList.toggle("active", open)
  const badge = btn.querySelector<HTMLElement>(".subsessions-badge")
  if (badge) {
    badge.hidden = total === 0
    badge.textContent = running > 0 ? String(running) : String(total)
  }
}

/** 会话切换时刷新面板（列表按当前会话过滤）。 */
export function refreshSubSessions(): void {
  if (open) {
    // selected 是跨会话共享的单变量：切会话后原选中项不属于新会话，
    // 若不动它就会停在旧会话的 runId 上（面板高亮错行、详情空）。
    if (!selectedRecord()) selected = currentRuns()[0]?.runId ?? null
    renderList()
    renderDetail()
  }
  syncButton()
}

export function bindSubSessions(): void {
  const btn = buttonEl()
  const overlay = overlayEl()
  btn?.addEventListener("click", () => {
    if (open) closeSubSessions()
    else openSubSessions()
  })
  document.getElementById("subsessions-close")?.addEventListener("click", () => closeSubSessions())
  overlay?.addEventListener("click", (e) => {
    if (e.target === overlay) closeSubSessions()
  })
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && open) closeSubSessions()
  })
  // 消息流入口点击 → 打开面板并选中该 run（入口在 messages 侧创建，不反向依赖本模块）
  document.addEventListener("gebai:open-subsession", (e) => {
    const d = (e as CustomEvent<{ sessionId?: string; runId?: string }>).detail
    if (!d?.runId) return
    if (d.sessionId) focusRun(d.sessionId, d.runId)
    else openSubSessions(d.runId)
  })
  // 入口落进 DOM 后按记录补一次状态（记录可能早于入口创建，如历史回放）
  document.addEventListener("gebai:subsession-entry", (e) => {
    const d = (e as CustomEvent<{ sessionId?: string; runId?: string }>).detail
    if (!d?.sessionId || !d.runId) return
    const rec = recordOf(d.sessionId, d.runId)
    if (rec) syncRun(rec)
  })
  syncButton()
}
