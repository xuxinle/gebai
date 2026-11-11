/** 会话输入队列的**展示层**（排队条）。
 *
 *  队列真相在服务端（`packages/server/src/core/session/input-queue.ts`）：任务运行期间提交的输入
 *  由服务端排队，当前任务收尾后自动按序执行。前端这里只做三件事：
 *  ① 缓存服务端推来的队列（`applyQueued` / `applyQueuedUpdate`：快照与 `event.queue.update` 两来源）；
 *  ② 渲染当前会话的排队条；
 *  ③ 把用户意图（编辑/撤回/立即执行）转成请求——不改写本地队列，等服务端应答/事件回推。
 *
 *  为何不在前端持队列：本地队列在多标签页、刷新、飞书/定时任务同时发话时与真实运行态打架
 *  （本地消化逻辑看不见别处发起的任务），而「这个会话还有几条待说」本是会话属性、不是页面属性。
 *
 *  **渲染是增量对齐，不是整体重建**：队列数据被推送的频率远高于它真正变化的频率——任何一次
 *  `state.snapshot` 都携带全量 `queues`（页面可见时自检每 3 秒一拍快照，重连/登录另计），
 *  而快照与事件都会走到这里。整体重建会让排队条随会话更新反复闪动：内联编辑框被销毁、
 *  已输入未保存的文字被抹回旧值、焦点与光标丢失、入场动画重放。所以按条目 id 复用节点，
 *  内容没变就一个 DOM 都不写；编辑中的条目只认 textarea 里的草稿，服务端推来的内容不得覆盖它。
 */
import type { QueuedPrompt } from "@gebai/sdk"
import { client, el, getCurrentSession, queueEl } from "./state"
import { tip, toast } from "./ui"

/** 各会话排队输入（服务端推送的镜像，按会话隔离）。 */
const queues = new Map<string, QueuedPrompt[]>()

/** 内联编辑态：被编辑的条目 + 已输入未保存的草稿。
 *  草稿存在这里而不是只留在 DOM：万不得已要整条重排时，输入不会凭空消失。 */
let editing: { sessionId: string; id: string; draft: string } | null = null

/** 单条排队输入的渲染视图（队列更新只改这些引用指向的节点，不重建 chip）。 */
interface ChipView {
  /** chip 容器（`.queue-item`）：进入/退出编辑态时只替换它的内容，节点本身原位不动。 */
  node: HTMLElement
  /** 序号节点：整条队列共用一份引用，条目增删后改文字即可（无需重建）。 */
  ordEl: HTMLElement
  /** 上次渲染的序号文字（没变就不写 DOM）。 */
  ord: string
  /** 上次渲染的正文与附件数：两者都没变就跳过 DOM 写入。 */
  text: string
  files: number
  /** 编辑态 textarea（非空 = 该条正在内联编辑，普通内容更新一律不碰它）。 */
  editor: HTMLTextAreaElement | null
}

/** 已渲染的 chip（条目 id → 视图）。 */
const chips = new Map<string, ChipView>()
/** 排队条当前渲染的会话（切会话整条重建：条目集合、顺序与编辑态都不再适用）。 */
let renderedSessionId: string | null = null
/** 排队条头部（只建一次，之后只改标题文字与条目节点）。 */
let headEl: HTMLElement | null = null
let headTitleEl: HTMLElement | null = null
/** 上次渲染的「已排队 N 条」文字（没变就不写 DOM）。 */
let headTitle = ""

/** 队列全量对齐（快照 `state.snapshot.queues`：未携带即清空——与后端同源，不残留旧值）。 */
export function applyQueued(all: Record<string, QueuedPrompt[]> | undefined): void {
  queues.clear()
  for (const [sessionId, items] of Object.entries(all ?? {})) {
    if (items?.length) queues.set(sessionId, items)
  }
  renderQueue()
}

/** 单会话队列更新（`event.queue.update`：服务端在入队/出队/编辑/撤回/置顶后推送）。 */
export function applyQueuedUpdate(sessionId: string, items: QueuedPrompt[]): void {
  if (items?.length) queues.set(sessionId, items)
  else queues.delete(sessionId)
  renderQueue()
}

/** 某会话当前排队条目（本地镜像只读视图）。 */
export function queuedOf(sessionId: string): QueuedPrompt[] {
  return queues.get(sessionId) ?? []
}

/** 会话删除：清理本地镜像（服务端队列由 session.delete 一并释放）。 */
export function clearQueue(sessionId: string): void {
  if (editing?.sessionId === sessionId) editing = null
  if (!queues.delete(sessionId)) return
  renderQueue()
}

/** 编辑未执行的排队输入（请求服务端；界面随事件/应答收敛）。 */
function editQueuedInput(sessionId: string, id: string, text: string): void {
  void client.editQueuedPrompt(sessionId, id, text).catch((err: unknown) => toast(`编辑失败：${(err as Error).message}`, "error"))
}

/** 撤回（移除）未执行的排队输入。 */
function removeQueuedInput(sessionId: string, id: string): void {
  void client.removeQueuedPrompt(sessionId, id).catch((err: unknown) => toast(`撤回失败：${(err as Error).message}`, "error"))
}

/** 立即执行（⚡）：置队首并中断当前任务，其收尾后立即执行该条。 */
function runQueuedInput(sessionId: string, id: string): void {
  void client.runQueuedPrompt(sessionId, id).catch((err: unknown) => toast(`执行失败：${(err as Error).message}`, "error"))
}

/** 清空排队条 DOM（切会话、队列见底、会话删除）。 */
function clearDom(): void {
  queueEl.replaceChildren()
  chips.clear()
  headEl = null
  headTitleEl = null
  headTitle = ""
  renderedSessionId = null
}

/** 渲染当前会话的排队条（仅当前会话可见；空队列整条隐藏）。 */
export function renderQueue(): void {
  const cur = getCurrentSession()
  const items = cur ? queues.get(cur.id) ?? [] : []
  if (!cur || !items.length) {
    clearDom()
    editing = null
    queueEl.hidden = true
    return
  }
  if (renderedSessionId !== cur.id) {
    // 会话切换：条目集合与顺序都换了一批，编辑态也不再属于当前视图
    clearDom()
    renderedSessionId = cur.id
    editing = null
  }
  // 编辑目标已不在队列里（已执行/被撤回）：编辑态结束，退回普通 chip
  if (editing && !items.some((x) => x.id === editing!.id)) editing = null
  if (!headEl) {
    headEl = el("div", "queue-head")
    headTitleEl = el("span", "queue-title", "")
    headEl.append(headTitleEl, el("span", "queue-hint", "当前回答结束后自动按序发送；Ctrl+Enter 可中断插入"))
    queueEl.appendChild(headEl)
  }
  const title = `⏳ 已排队 ${items.length} 条`
  if (headTitle !== title) {
    headTitle = title
    headTitleEl!.textContent = title
  }
  queueEl.hidden = false
  const nodes: HTMLElement[] = []
  for (let i = 0; i < items.length; i++) {
    const item = items[i]
    let view = chips.get(item.id)
    if (!view) {
      view = createChip(cur.id, item)
      chips.set(item.id, view)
    } else {
      syncChip(view, cur.id, item)
    }
    const ord = String(i + 1)
    if (view.ord !== ord) {
      view.ord = ord
      view.ordEl.textContent = ord
    }
    nodes.push(view.node)
  }
  // 掉队的 chip（已执行/撤回/换会话）：随即移除
  const alive = new Set(items.map((x) => x.id))
  for (const [id, view] of [...chips]) {
    if (alive.has(id)) continue
    view.node.remove()
    chips.delete(id)
  }
  // 顺序对齐：入队追加在尾部、撤回就地删除，正常路径下 DOM 顺序已与数组一致，这里一次都不动；
  // 真出现错序（别处置顶后重连、快照换序）才整条重排。重排要搬动节点，正在编辑的 textarea
  // 会连同焦点一起被搬走，所以编辑态下不动顺序（序号照常更新，保存/取消后自然对齐）
  if (!editing && !sameOrder(nodes)) {
    const top = queueEl.scrollTop
    queueEl.replaceChildren(headEl!, ...nodes)
    queueEl.scrollTop = top
  }
}

/** 当前 DOM 顺序是否已与条目数组一致（头部占第 0 位）。 */
function sameOrder(nodes: HTMLElement[]): boolean {
  const kids = queueEl.children
  if (kids.length !== nodes.length + 1) return false
  for (let i = 0; i < nodes.length; i++) {
    if (kids[i + 1] !== nodes[i]) return false
  }
  return true
}

/** 新建一条 chip（追加到排队条尾部；编辑中的条目直接建成编辑态）。 */
function createChip(sessionId: string, item: QueuedPrompt): ChipView {
  const node = el("div", "queue-item")
  node.dataset.id = item.id
  const view: ChipView = {
    node,
    ordEl: el("span", "queue-ord"),
    ord: "",
    text: item.text,
    files: item.attachments?.length ?? 0,
    editor: null,
  }
  if (editing && editing.sessionId === sessionId && editing.id === item.id) renderEditor(view, sessionId, item)
  else renderNormal(view, sessionId, item)
  queueEl.appendChild(node)
  return view
}

/** 用服务端最新数据校准一条已渲染的 chip（内容没变则不写 DOM）。 */
function syncChip(view: ChipView, sessionId: string, item: QueuedPrompt): void {
  if (editing && editing.id === item.id) {
    // 编辑中：正文归 textarea（草稿），服务端内容不得覆盖用户正在输入的文字
    if (!view.editor) renderEditor(view, sessionId, item)
    return
  }
  if (view.editor) {
    // 编辑态结束（保存/取消）：退回普通 chip
    renderNormal(view, sessionId, item)
    return
  }
  const files = item.attachments?.length ?? 0
  if (view.text === item.text && view.files === files) return
  renderNormal(view, sessionId, item)
}

/** 普通态内容：序号 + 正文（含附件计数）+ 立即执行/编辑/撤回。 */
function renderNormal(view: ChipView, sessionId: string, item: QueuedPrompt): void {
  view.editor = null
  view.text = item.text
  view.files = item.attachments?.length ?? 0
  const text = el("div", "queue-text")
  text.textContent = item.text
  if (view.files) {
    text.append(el("span", "queue-files", `📎 ${view.files}`))
  }
  tip(text, item.text)
  const run = el("button", "queue-act queue-run", "⚡")
  tip(run, "立即执行（中断当前任务）")
  run.onclick = () => runQueuedInput(sessionId, item.id)
  const edit = el("button", "queue-act", "✎")
  tip(edit, "编辑")
  edit.onclick = () => beginEdit(sessionId, item.id)
  const del = el("button", "queue-act queue-del", "✕")
  tip(del, "撤回")
  del.onclick = () => removeQueuedInput(sessionId, item.id)
  const actions = el("div", "queue-actions")
  actions.append(run, edit, del)
  // 只换 chip 内容：chip 节点原位不动（序号节点复用，不重建）
  view.node.replaceChildren(view.ordEl, text, actions)
}

/** 编辑态内容：序号 + textarea（草稿）+ 保存/取消。 */
function renderEditor(view: ChipView, sessionId: string, item: QueuedPrompt): void {
  const draft = editing && editing.id === item.id ? editing.draft : item.text
  const wrap = el("div", "queue-editor")
  const ta = document.createElement("textarea")
  ta.value = draft
  ta.rows = 2
  ta.oninput = () => {
    if (editing && editing.id === item.id) editing.draft = ta.value
  }
  const save = el("button", "queue-save", "保存")
  save.onclick = () => {
    const val = ta.value.trim()
    if (!val) return
    editing = null
    // 本地镜像先落新正文：服务端应答/事件回到这里之前，chip 不会先退回旧文字再跳成新文字
    // （真值仍以服务端回推为准，下一次队列事件整体覆盖）
    const mirrored = (queues.get(sessionId) ?? []).find((x) => x.id === item.id)
    if (mirrored) mirrored.text = val
    editQueuedInput(sessionId, item.id, val)
    renderQueue()
  }
  const cancel = el("button", "queue-cancel", "取消")
  cancel.onclick = () => {
    editing = null
    renderQueue()
  }
  const actions = el("div", "queue-editor-actions")
  actions.append(save, cancel)
  wrap.append(ta, actions)
  view.node.replaceChildren(view.ordEl, wrap)
  view.editor = ta
  setTimeout(() => {
    ta.focus()
    ta.setSelectionRange(ta.value.length, ta.value.length)
  }, 0)
}

/** 进入内联编辑：草稿先取当前正文，再就地换成编辑态（节点不移动、不改顺序）。
 *  走统一渲染入口：上一条的编辑态也在这一拍退回普通 chip（同一时刻只会有一条在编辑）。 */
function beginEdit(sessionId: string, id: string): void {
  // 正文取自本地镜像而不是渲染时捕获的对象：镜像可能在那之后被新的队列事件整体替换过
  const item = (queues.get(sessionId) ?? []).find((x) => x.id === id)
  if (!item) return
  editing = { sessionId, id, draft: item.text }
  renderQueue()
}
