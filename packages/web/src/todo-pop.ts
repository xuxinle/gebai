/** 待办弹窗（DESIGN「用户级待办与闲时任务」）：标题栏轮盘「待办」按钮打开的**可拖动**浮层——
 *  新增/多行编辑/删除/拖动排序/勾选完成；标记 ⚡ 的条目为闲时任务（服务端没有运行中的会话时按顺序
 *  自动执行）；「▶ 执行」**新建一条会话**以该待办全文为提示词跑一次；点击条目文本或其「填入」按钮
 *  把内容写进对话输入框。
 *
 *  待办文本即模型提示词（可为多行详细描述）：列表内长文本折叠展示（可展开）、编辑与新增都用多行
 *  文本域（编辑可拖拽调高，Ctrl/Cmd+Enter 保存）。
 *
 *  显隐：**只有点击右上角 ✕ 才隐藏**（点轮盘按钮、点弹窗外、按 Esc 都不关闭，避免编辑长提示词时
 *  误触丢失）；打开状态与位置一起持久化（`gebai.ui.todo.open` / `gebai.ui.todo.pos`），页面刷新
 *  （含 dev-reload）后自动恢复打开与位置。数据源 REST /api/v1/todos；打开期间每 15s 静默同步状态
 *  （列表无变化时不重绘，不打断滚动/编辑）。
 *  拖动范式照 cny-cat.ts（pointerdown + setPointerCapture + 位移钳制 + 丢失捕获兜底）。 */
import type { UserTodo } from "@gebai/sdk"
import { autosize, syncSendButton } from "./composer"
import { refreshSessions } from "./sessions"
import { clampPos, defaultPos, dropTargetIndex, moveItem, parsePos, type PopPos } from "./todo-core"
import { client, el, focusInput, input } from "./state"
import { confirmDialog, toast } from "./ui"

const POS_KEY = "gebai.ui.todo.pos"
const OPEN_KEY = "gebai.ui.todo.open"
const POLL_MS = 15_000
/** 列表内长文本折叠阈值（行数 / 字符数，超过即折叠展示 + 「展开全文」）。 */
const CLAMP_LINES = 8
const CLAMP_CHARS = 420

const ICON = {
  idle: '<svg viewBox="0 0 24 24" width="14" height="14" fill="currentColor" aria-hidden="true"><path d="M13 2L4.5 13H11l-1 9 8.5-11H12l1-9z"/></svg>',
  run: '<svg viewBox="0 0 24 24" width="14" height="14" fill="currentColor" aria-hidden="true"><path d="M8 5l11 7-11 7V5z"/></svg>',
  fill: '<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 19V5"/><path d="M5 12l7-7 7 7"/></svg>',
  edit: '<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 20h4L18 10l-4-4L4 16v4z"/><path d="M14 6l4 4"/></svg>',
  del: '<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13"/></svg>',
} as const

interface TodoPopRefs {
  root: HTMLDivElement
  list: HTMLUListElement
  empty: HTMLDivElement
  count: HTMLSpanElement
  addForm: HTMLFormElement
  addInput: HTMLTextAreaElement
  addIdle: HTMLInputElement
}

let refs: TodoPopRefs | null = null
let todos: UserTodo[] = []
let opened = false
let bound = false
/** 行内编辑中的待办 id（编辑期间跳过自动重绘，避免打断输入）。 */
let editingId: string | null = null
let pollTimer: number | null = null
/** 列表拖动排序的源索引。 */
let dragFromIndex: number | null = null
/** 浮层拖动会话（指针位移起点）。 */
let dragWindow: { dx: number; dy: number; pid: number } | null = null
/** 展开全文的待办 id（长文本折叠状态）。 */
const expandedIds = new Set<string>()
/** 上次渲染的数据签名（无变化时跳过重绘——不打断滚动与编辑）。 */
let lastSig = ""

/** 绑定轮盘「待办」按钮（main.ts 初始化时调用一次）；上次退出时弹窗是打开的则自动恢复打开。 */
export function bindTodoPop(): void {
  const btn = document.getElementById("todo-btn")
  if (!btn || bound) return
  bound = true
  // 点轮盘按钮只负责「打开/前置」（**不关闭**——关闭唯一入口是弹窗右上角 ✕）
  btn.addEventListener("click", (e) => {
    e.preventDefault()
    void openTodoPop()
  })
  if (readLocal(OPEN_KEY) === "1") void openTodoPop()
}

export function isTodoPopOpen(): boolean {
  return opened
}

export async function openTodoPop(): Promise<void> {
  if (!refs) refs = buildPop()
  const wasOpen = opened
  opened = true
  writeLocal(OPEN_KEY, "1")
  refs.root.hidden = false
  refs.root.classList.add("show")
  if (!wasOpen) applyPos(clampNow(loadPos()), true)
  await refresh()
  // 首次打开默认聚焦新增输入框，直接敲键盘即可记待办
  if (!wasOpen && !editingId) refs.addInput.focus()
  startPoll()
}

/** 隐藏弹窗（唯一入口：弹窗右上角 ✕）。 */
export function closeTodoPop(): void {
  if (!refs) return
  opened = false
  editingId = null
  writeLocal(OPEN_KEY, "0")
  refs.root.hidden = true
  refs.root.classList.remove("show")
  stopPoll()
}

/* ---------- 数据 ---------- */

/** 数据签名：id/文本/状态任一变化才重绘（定时同步不打断滚动与编辑）。 */
function signature(): string {
  return todos.map((t) => `${t.id}:${t.done ? 1 : 0}:${t.idle ? 1 : 0}:${t.idleState ?? ""}:${t.idleResult ?? ""}:${t.idleError ?? ""}:${t.text}`).join("|")
}

async function refresh(force = false): Promise<void> {
  try {
    todos = await client.listUserTodos()
  } catch (err) {
    toast(`待办加载失败: ${(err as Error).message}`)
    return
  }
  const sig = signature()
  if (force || sig !== lastSig) render()
}

function startPoll(): void {
  stopPoll()
  pollTimer = window.setInterval(() => {
    if (!opened || document.visibilityState !== "visible" || editingId) return
    void refresh()
  }, POLL_MS)
}

function stopPoll(): void {
  if (pollTimer) clearInterval(pollTimer)
  pollTimer = null
}

/* ---------- 渲染 ---------- */

function render(): void {
  const r = refs
  if (!r) return
  lastSig = signature()
  r.list.textContent = ""
  r.count.textContent = summary()
  r.empty.hidden = todos.length > 0
  todos.forEach((t, i) => r.list.appendChild(renderItem(t, i)))
}

function summary(): string {
  if (!todos.length) return "暂无待办"
  const open = todos.filter((t) => !t.done).length
  const idle = todos.filter((t) => t.idle && !t.done).length
  return `未完成 ${open} · 闲时任务 ${idle}`
}

function renderItem(t: UserTodo, index: number): HTMLElement {
  const li = el("li", "todo-item")
  li.dataset.id = t.id
  li.dataset.index = String(index)
  li.draggable = true
  if (t.done) li.classList.add("done")
  if (t.idle) li.classList.add("idle")
  if (t.idleState === "running") li.classList.add("running")
  if (t.idleState === "failed") li.classList.add("failed")

  const grip = el("span", "todo-grip", "≡")
  grip.title = "拖动排序"
  li.appendChild(grip)

  const check = document.createElement("input")
  check.type = "checkbox"
  check.className = "todo-check"
  check.checked = t.done
  check.title = t.done ? "标记为未完成" : "标记为完成"
  check.addEventListener("change", () => void setDone(t, check.checked))
  li.appendChild(check)

  const body = el("div", "todo-body")
  const text = el("span", "todo-text", t.text)
  text.title = "点击填入输入框"
  text.addEventListener("click", () => fillFromTodo(t))
  body.appendChild(text)
  // 长提示词折叠展示（列表不被打爆），可一键展开/收起
  const lineCount = t.text.split("\n").length
  if (!expandedIds.has(t.id) && (lineCount > CLAMP_LINES || t.text.length > CLAMP_CHARS)) {
    text.classList.add("clamped")
    const more = el("button", "todo-more", `展开全文（${t.text.length} 字 / ${lineCount} 行）`)
    more.type = "button"
    more.addEventListener("click", (e) => {
      e.stopPropagation()
      expandedIds.add(t.id)
      render()
    })
    body.appendChild(more)
  } else if (expandedIds.has(t.id)) {
    const less = el("button", "todo-more", "收起")
    less.type = "button"
    less.addEventListener("click", (e) => {
      e.stopPropagation()
      expandedIds.delete(t.id)
      render()
    })
    body.appendChild(less)
  }
  const meta = idleMeta(t)
  if (meta) body.appendChild(meta)
  li.appendChild(body)

  const actions = el("div", "todo-actions")
  actions.appendChild(actionBtn("run", "执行：新建一条会话，以本待办全文为提示词立即执行", false, () => void runTodo(t)))
  actions.appendChild(actionBtn("idle", t.idle ? "关闭闲时任务" : "标记为闲时任务（服务端空闲时按顺序自动执行）", t.idle, () => void setIdle(t, !t.idle)))
  actions.appendChild(actionBtn("fill", "填入输入框", false, () => fillFromTodo(t)))
  actions.appendChild(actionBtn("edit", "编辑内容（多行提示词）", false, () => startEdit(li, t)))
  actions.appendChild(actionBtn("del", "删除待办", false, () => void removeTodo(t)))
  li.appendChild(actions)

  bindRowDrag(li, index)
  return li
}

function actionBtn(kind: keyof typeof ICON, tip: string, active: boolean, onClick: () => void): HTMLButtonElement {
  const b = el("button", `todo-act todo-act-${kind}`)
  b.type = "button"
  b.dataset.tip = tip
  b.setAttribute("aria-label", tip)
  b.innerHTML = ICON[kind]
  if (active) b.classList.add("on")
  b.addEventListener("click", (e) => {
    e.stopPropagation()
    onClick()
  })
  return b
}

/** 闲时/执行状态行（排队中/执行中/已完成摘要/失败原因）。 */
function idleMeta(t: UserTodo): HTMLElement | null {
  let text = ""
  if (t.idleState === "running") text = "⚡ 正在执行…（新建会话运行中，完成后自动回写结果）"
  else if (t.idleState === "failed") text = `⚡ 已停止自动执行：${t.idleError ?? "多次失败"}`
  else if (t.done && t.idleResult) text = `⚡ 已完成：${t.idleResult}`
  else if (t.idleError) text = `⚡ 上次失败：${t.idleError}`
  else if (t.idle && t.idleState === "pending") text = "⚡ 排队中：服务端无运行中会话时按顺序自动执行"
  if (!text) return null
  const div = el("div", "todo-meta", text)
  div.title = text
  return div
}

/* ---------- 浮层构建与拖动 ---------- */

function buildPop(): TodoPopRefs {
  const root = el("div", "todo-pop")
  root.hidden = true
  root.setAttribute("role", "dialog")
  root.setAttribute("aria-label", "待办清单")

  const head = el("div", "todo-pop-head")
  const title = el("span", "todo-pop-title", "待办")
  const count = el("span", "todo-pop-count", "")
  const refreshBtn = el("button", "todo-pop-icon todo-pop-refresh")
  refreshBtn.type = "button"
  refreshBtn.dataset.tip = "刷新"
  refreshBtn.setAttribute("aria-label", "刷新")
  refreshBtn.textContent = "↻"
  refreshBtn.addEventListener("click", () => void refresh(true))
  const closeBtn = el("button", "todo-pop-icon todo-pop-close")
  closeBtn.type = "button"
  closeBtn.dataset.tip = "关闭（唯一关闭入口；点弹窗外或按 Esc 不会关闭）"
  closeBtn.setAttribute("aria-label", "关闭待办")
  closeBtn.textContent = "✕"
  closeBtn.addEventListener("click", () => closeTodoPop())
  head.append(title, count, refreshBtn, closeBtn)

  const hint = el(
    "div",
    "todo-pop-hint",
    "拖动标题栏移动窗口；待办全文即模型提示词（可多行详细描述）。▶ 执行 = 新建一条会话立即执行；⚡ = 服务端没有运行中的会话时按顺序自动执行",
  )

  const list = el("ul", "todo-list")
  const empty = el("div", "todo-empty", "暂无待办：在下方输入内容后 Ctrl/Cmd+Enter 添加")

  const addForm = el("form", "todo-add")
  const addInput = document.createElement("textarea")
  addInput.className = "todo-add-input"
  addInput.rows = 2
  addInput.placeholder = "新增待办…（可多行写详细提示词；Ctrl/Cmd+Enter 添加）"
  addInput.maxLength = 2000
  addInput.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
      e.preventDefault()
      addForm.requestSubmit()
    }
  })
  const addIdleLabel = el("label", "todo-add-idle")
  const addIdle = document.createElement("input")
  addIdle.type = "checkbox"
  addIdleLabel.append(addIdle, document.createTextNode("⚡闲时"))
  addIdleLabel.title = "勾选后：服务端没有运行中的会话时按顺序自动执行"
  const addBtn = el("button", "todo-add-btn", "添加")
  addBtn.type = "submit"
  addForm.append(addInput, addIdleLabel, addBtn)

  root.append(head, hint, list, empty, addForm)
  document.body.appendChild(root)

  const r: TodoPopRefs = { root, list, empty, count, addForm, addInput, addIdle }
  bindHeadDrag(r)
  addForm.addEventListener("submit", (e) => {
    e.preventDefault()
    void addTodo()
  })
  window.addEventListener("resize", () => {
    if (opened) applyPos(clampNow(currentPos()), true)
  })
  return r
}

function bindHeadDrag(r: TodoPopRefs): void {
  const head = r.root.querySelector(".todo-pop-head") as HTMLElement
  head.addEventListener("pointerdown", (e) => {
    if ((e.target as HTMLElement).closest("button")) return
    if (e.button !== 0) return
    const rect = r.root.getBoundingClientRect()
    dragWindow = { dx: e.clientX - rect.left, dy: e.clientY - rect.top, pid: e.pointerId }
    r.root.classList.add("dragging")
    head.setPointerCapture(e.pointerId)
    e.preventDefault()
  })
  const move = (e: PointerEvent) => {
    if (!dragWindow || e.pointerId !== dragWindow.pid) return
    const rect = r.root.getBoundingClientRect()
    const p = clampPos(e.clientX - dragWindow.dx, e.clientY - dragWindow.dy, rect.width, rect.height, window.innerWidth, window.innerHeight)
    applyPos(p, false)
  }
  const end = (e: PointerEvent) => {
    if (!dragWindow || e.pointerId !== dragWindow.pid) return
    dragWindow = null
    r.root.classList.remove("dragging")
    savePos()
  }
  head.addEventListener("pointermove", move)
  head.addEventListener("pointerup", end)
  head.addEventListener("pointercancel", end)
  head.addEventListener("lostpointercapture", end)
}

function applyPos(p: PopPos, persist: boolean): void {
  const r = refs
  if (!r) return
  r.root.style.left = `${p.x}px`
  r.root.style.top = `${p.y}px`
  if (persist) savePos()
}

function currentPos(): PopPos {
  const r = refs
  if (!r) return { x: 0, y: 0 }
  const rect = r.root.getBoundingClientRect()
  return { x: rect.left, y: rect.top }
}

function clampNow(p: PopPos): PopPos {
  const r = refs
  if (!r) return p
  const rect = r.root.getBoundingClientRect()
  const w = rect.width || 640
  const h = rect.height || 560
  return clampPos(p.x, p.y, w, h, window.innerWidth, window.innerHeight)
}

function loadPos(): PopPos {
  const saved = parsePos(readLocal(POS_KEY))
  if (saved) return saved
  const r = refs
  const w = r ? r.root.getBoundingClientRect().width || 640 : 640
  return defaultPos(w, window.innerWidth)
}

function savePos(): void {
  const r = refs
  if (!r) return
  const p = currentPos()
  writeLocal(POS_KEY, JSON.stringify(p))
}

function readLocal(key: string): string | null {
  try {
    return localStorage.getItem(key)
  } catch {
    return null
  }
}

function writeLocal(key: string, value: string): void {
  try {
    localStorage.setItem(key, value)
  } catch {
    /* 隐私模式等：位置/打开态不持久化不影响使用 */
  }
}

/* ---------- 交互 ---------- */

async function setDone(t: UserTodo, done: boolean): Promise<void> {
  try {
    patchLocal(await client.updateUserTodo(t.id, { done }))
  } catch (err) {
    toast(`修改失败: ${(err as Error).message}`)
    await refresh(true)
  }
}

async function setIdle(t: UserTodo, idle: boolean): Promise<void> {
  try {
    patchLocal(await client.updateUserTodo(t.id, { idle }))
    if (idle) toast("已标记为闲时任务：服务端没有运行中的会话时自动执行", "ok")
  } catch (err) {
    toast(`修改失败: ${(err as Error).message}`)
    await refresh(true)
  }
}

async function saveText(t: UserTodo, text: string): Promise<void> {
  try {
    patchLocal(await client.updateUserTodo(t.id, { text }))
  } catch (err) {
    toast(`保存失败: ${(err as Error).message}`)
    await refresh(true)
  }
}

async function removeTodo(t: UserTodo): Promise<void> {
  const ok = await confirmDialog({
    title: "删除待办",
    text: t.text.length > 60 ? `${t.text.slice(0, 60)}…` : t.text,
    okLabel: "删除",
  })
  if (!ok) return
  try {
    await client.deleteUserTodo(t.id)
    todos = todos.filter((x) => x.id !== t.id)
    render()
  } catch (err) {
    toast(`删除失败: ${(err as Error).message}`)
    await refresh(true)
  }
}

/** 立即执行：**新建一条会话**以该待办全文为提示词跑一次（后端不等待执行结束即返回会话 id）。 */
async function runTodo(t: UserTodo): Promise<void> {
  try {
    const res = await client.runUserTodo(t.id)
    patchLocal(res.todo)
    toast(res.sessionId ? "已新建会话执行该待办，可在会话列表查看进度与结果" : "已开始执行", "ok")
    void refreshSessions() // 会话列表即时出现执行会话（不阻塞）
  } catch (err) {
    toast(`执行失败: ${(err as Error).message}`)
    await refresh(true)
  }
}

async function addTodo(): Promise<void> {
  const r = refs
  if (!r) return
  const text = r.addInput.value.trim()
  if (!text) return
  const idle = r.addIdle.checked
  try {
    const created = await client.createUserTodo({ text, idle })
    todos = [...todos, created]
    r.addInput.value = ""
    r.addIdle.checked = false
    render()
    r.addInput.focus()
  } catch (err) {
    toast(`新增失败: ${(err as Error).message}`)
  }
}

/** 填入输入框（点击条目文本或其「填入」按钮）：覆盖写入 + 聚焦（沿用 shortcuts.ts 的既有做法）。 */
function fillFromTodo(t: UserTodo): void {
  input.value = t.text
  autosize()
  syncSendButton()
  focusInput()
  toast("已填入输入框", "ok")
}

/** 本地替换单条（乐观更新；失败路径由调用方 refresh 回滚为服务端真值）。 */
function patchLocal(updated: UserTodo): void {
  todos = todos.map((x) => (x.id === updated.id ? updated : x))
  if (!editingId) render()
}

/** 行内编辑（多行文本域，可拖拽调高）：Ctrl/Cmd+Enter 或「保存」保存，Esc 或「取消」放弃；
 *  点击别处不自动保存（避免写长提示词时误触丢失/误存）。 */
function startEdit(li: HTMLElement, t: UserTodo): void {
  if (editingId) return
  editingId = t.id
  li.classList.add("editing")
  const body = li.querySelector(".todo-body") as HTMLElement
  body.textContent = ""
  const editor = document.createElement("textarea")
  editor.className = "todo-edit-input"
  editor.value = t.text
  editor.maxLength = 2000
  editor.rows = 6
  const actions = el("div", "todo-edit-actions")
  const saveBtn = el("button", "todo-add-btn", "保存")
  saveBtn.type = "button"
  const cancelBtn = el("button", "todo-add-btn ghost", "取消")
  cancelBtn.type = "button"
  const tipText = el("span", "todo-edit-tip", "Ctrl/Cmd+Enter 保存 · Esc 取消")
  actions.append(tipText, saveBtn, cancelBtn)
  body.append(editor, actions)
  li.querySelector(".todo-actions")?.setAttribute("hidden", "")
  editor.focus()
  editor.setSelectionRange(editor.value.length, editor.value.length)

  let settled = false
  const finish = async (save: boolean) => {
    if (settled) return
    settled = true
    editingId = null
    li.classList.remove("editing")
    const value = editor.value.trim()
    if (save && value && value !== t.text) await saveText(t, value)
    else render()
  }
  saveBtn.addEventListener("click", () => void finish(true))
  cancelBtn.addEventListener("click", () => void finish(false))
  editor.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
      e.preventDefault()
      void finish(true)
    } else if (e.key === "Escape") {
      e.preventDefault()
      e.stopPropagation()
      void finish(false)
    }
  })
}

/* ---------- 列表拖动排序（HTML5 drag；样式提示落点） ---------- */

function bindRowDrag(li: HTMLElement, index: number): void {
  li.addEventListener("dragstart", (e) => {
    dragFromIndex = index
    li.classList.add("dragging-row")
    e.dataTransfer?.setData("text/plain", String(index))
    if (e.dataTransfer) e.dataTransfer.effectAllowed = "move"
  })
  li.addEventListener("dragend", () => {
    dragFromIndex = null
    li.classList.remove("dragging-row")
    clearDropMarks()
  })
  li.addEventListener("dragover", (e) => {
    if (dragFromIndex === null) return
    e.preventDefault()
    if (e.dataTransfer) e.dataTransfer.dropEffect = "move"
    const rect = li.getBoundingClientRect()
    const before = e.clientY < rect.top + rect.height / 2
    clearDropMarks()
    li.classList.add(before ? "drop-before" : "drop-after")
  })
  li.addEventListener("drop", (e) => {
    if (dragFromIndex === null) return
    e.preventDefault()
    const rect = li.getBoundingClientRect()
    const before = e.clientY < rect.top + rect.height / 2
    const from = dragFromIndex
    const to = dropTargetIndex(from, index, before)
    dragFromIndex = null
    clearDropMarks()
    if (from === to) return
    void applyOrder(moveItem(todos, from, to))
  })
}

function clearDropMarks(): void {
  refs?.root.querySelectorAll(".drop-before, .drop-after").forEach((n) => n.classList.remove("drop-before", "drop-after"))
}

/** 拖动排序落库（先乐观更新；失败回滚为服务端真值）。 */
async function applyOrder(next: UserTodo[]): Promise<void> {
  todos = next
  render()
  try {
    todos = await client.reorderUserTodos(todos.map((t) => t.id))
    render()
  } catch (err) {
    toast(`排序保存失败: ${(err as Error).message}`)
    await refresh(true)
  }
}
