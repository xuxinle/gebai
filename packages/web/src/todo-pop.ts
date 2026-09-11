/** 待办弹窗（DESIGN「用户级待办与闲时任务」）：标题栏轮盘「待办」按钮打开的**可拖动**浮层——
 *  用户在弹窗内新增/行内修改/删除/拖动排序/勾选完成，标记 ⚡ 的条目为闲时任务（服务端没有运行中
 *  的会话时按顺序自动执行）；点击条目文本或其「填入」按钮把内容写进对话输入框。
 *
 *  数据源：REST /api/v1/todos（用户级资源，与会话解耦）；弹窗打开期间每 15s 静默刷新一次（闲时任务
 *  可能在后端改了状态），关闭即停。位置持久化在 localStorage `gebai.ui.todo.pos`（脏数据回退默认位）。
 *  拖动范式照 cny-cat.ts（pointerdown + setPointerCapture + 位移钳制 + 丢失捕获兜底）。 */
import type { UserTodo } from "@gebai/sdk"
import { autosize, syncSendButton } from "./composer"
import { clampPos, defaultPos, dropTargetIndex, moveItem, parsePos, type PopPos } from "./todo-core"
import { client, el, focusInput, input } from "./state"
import { confirmDialog, toast } from "./ui"

const POS_KEY = "gebai.ui.todo.pos"
const POLL_MS = 15_000

const ICON = {
  idle: '<svg viewBox="0 0 24 24" width="13" height="13" fill="currentColor" aria-hidden="true"><path d="M13 2L4.5 13H11l-1 9 8.5-11H12l1-9z"/></svg>',
  fill: '<svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 19V5"/><path d="M5 12l7-7 7 7"/></svg>',
  edit: '<svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 20h4L18 10l-4-4L4 16v4z"/><path d="M14 6l4 4"/></svg>',
  del: '<svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13"/></svg>',
} as const

interface TodoPopRefs {
  root: HTMLDivElement
  list: HTMLUListElement
  empty: HTMLDivElement
  count: HTMLSpanElement
  addForm: HTMLFormElement
  addInput: HTMLInputElement
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

/** 绑定轮盘「待办」按钮（main.ts 初始化时调用一次）。 */
export function bindTodoPop(): void {
  const btn = document.getElementById("todo-btn")
  if (!btn || bound) return
  bound = true
  btn.addEventListener("click", (e) => {
    e.preventDefault()
    toggleTodoPop()
  })
}

export function toggleTodoPop(): void {
  if (opened) closeTodoPop()
  else void openTodoPop()
}

export function isTodoPopOpen(): boolean {
  return opened
}

export async function openTodoPop(): Promise<void> {
  if (!refs) refs = buildPop()
  opened = true
  refs.root.hidden = false
  refs.root.classList.add("show")
  applyPos(clampNow(loadPos()), true)
  await refresh()
  // 首次打开默认聚焦新增输入框，直接敲键盘即可记待办
  refs.addInput.focus()
  startPoll()
}

export function closeTodoPop(): void {
  if (!refs) return
  opened = false
  editingId = null
  refs.root.hidden = true
  refs.root.classList.remove("show")
  stopPoll()
}

/* ---------- 数据 ---------- */

async function refresh(): Promise<void> {
  try {
    todos = await client.listUserTodos()
  } catch (err) {
    toast(`待办加载失败: ${(err as Error).message}`)
    return
  }
  if (!editingId) render()
}

function startPoll(): void {
  stopPoll()
  pollTimer = window.setInterval(() => {
    if (!opened || document.visibilityState !== "visible") return
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
  const meta = idleMeta(t)
  if (meta) body.appendChild(meta)
  li.appendChild(body)

  const actions = el("div", "todo-actions")
  actions.appendChild(actionBtn("idle", t.idle ? "关闭闲时任务" : "标记为闲时任务（服务端空闲时自动执行）", t.idle, () => void setIdle(t, !t.idle)))
  actions.appendChild(actionBtn("fill", "填入输入框", false, () => fillFromTodo(t)))
  actions.appendChild(actionBtn("edit", "编辑内容", false, () => startEdit(li, t, text)))
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

/** 闲时状态行（排队/执行中/已完成摘要/失败原因）。 */
function idleMeta(t: UserTodo): HTMLElement | null {
  if (!t.idle) return null
  let text = ""
  if (t.idleState === "running") text = "⚡ 正在空闲执行…"
  else if (t.idleState === "failed") text = `⚡ 已停止自动执行：${t.idleError ?? "多次失败"}`
  else if (t.done && t.idleResult) text = `⚡ 已完成：${t.idleResult}`
  else if (t.idleError) text = `⚡ 上次失败：${t.idleError}`
  else if (t.idleState === "pending") text = "⚡ 排队中：服务端无运行中会话时按顺序执行"
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
  refreshBtn.addEventListener("click", () => void refresh())
  const closeBtn = el("button", "todo-pop-icon todo-pop-close")
  closeBtn.type = "button"
  closeBtn.dataset.tip = "关闭"
  closeBtn.setAttribute("aria-label", "关闭待办")
  closeBtn.textContent = "✕"
  closeBtn.addEventListener("click", () => closeTodoPop())
  head.append(title, count, refreshBtn, closeBtn)

  const hint = el("div", "todo-pop-hint", "拖动标题栏移动窗口；⚡ 闲时任务在服务端没有运行中的会话时按顺序自动执行")

  const list = el("ul", "todo-list")
  const empty = el("div", "todo-empty", "暂无待办：在下方输入内容后回车添加")

  const addForm = el("form", "todo-add")
  const addInput = document.createElement("input")
  addInput.type = "text"
  addInput.className = "todo-add-input"
  addInput.placeholder = "新增待办…（回车添加）"
  addInput.maxLength = 2000
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
  // 外点关闭（点击轮盘按钮自身除外——由按钮 click 切换）
  document.addEventListener("pointerdown", (e) => {
    if (!opened) return
    const target = e.target as Node
    if (root.contains(target)) return
    if (document.getElementById("todo-btn")?.contains(target)) return
    closeTodoPop()
  })
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && opened) closeTodoPop()
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
  const w = rect.width || 330
  const h = rect.height || 380
  return clampPos(p.x, p.y, w, h, window.innerWidth, window.innerHeight)
}

function loadPos(): PopPos {
  const saved = parsePos(readLocal(POS_KEY))
  if (saved) return saved
  const r = refs
  const w = r ? r.root.getBoundingClientRect().width || 330 : 330
  return defaultPos(w, window.innerWidth)
}

function savePos(): void {
  const r = refs
  if (!r) return
  const p = currentPos()
  try {
    localStorage.setItem(POS_KEY, JSON.stringify(p))
  } catch {
    /* 隐私模式等：位置不持久化不影响使用 */
  }
}

function readLocal(key: string): string | null {
  try {
    return localStorage.getItem(key)
  } catch {
    return null
  }
}

/* ---------- 交互 ---------- */

async function setDone(t: UserTodo, done: boolean): Promise<void> {
  try {
    patchLocal(await client.updateUserTodo(t.id, { done }))
  } catch (err) {
    toast(`修改失败: ${(err as Error).message}`)
    await refresh()
  }
}

async function setIdle(t: UserTodo, idle: boolean): Promise<void> {
  try {
    patchLocal(await client.updateUserTodo(t.id, { idle }))
    if (idle) toast("已标记为闲时任务：服务端没有运行中的会话时自动执行", "ok")
  } catch (err) {
    toast(`修改失败: ${(err as Error).message}`)
    await refresh()
  }
}

async function saveText(t: UserTodo, text: string): Promise<void> {
  try {
    patchLocal(await client.updateUserTodo(t.id, { text }))
  } catch (err) {
    toast(`保存失败: ${(err as Error).message}`)
    await refresh()
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
    await refresh()
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

/** 行内编辑：Enter 保存 / Esc 取消 / 失焦保存。 */
function startEdit(li: HTMLElement, t: UserTodo, textEl: HTMLElement): void {
  if (editingId) return
  editingId = t.id
  li.classList.add("editing")
  const editor = document.createElement("input")
  editor.type = "text"
  editor.className = "todo-edit-input"
  editor.value = t.text
  editor.maxLength = 2000
  textEl.replaceWith(editor)
  editor.focus()
  editor.select()
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
  editor.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      e.preventDefault()
      void finish(true)
    } else if (e.key === "Escape") {
      e.preventDefault()
      void finish(false)
    }
  })
  editor.addEventListener("blur", () => void finish(true))
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
    await refresh()
  }
}
