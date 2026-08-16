import { client, el, getCurrentSession, queueEl, runs } from "./state"
import { tip, toast } from "./ui"

/* ---------- 会话输入队列（纯前端实现：本地队列，任务结束自动发送下一条） ---------- */

/** 排队输入（本地状态，按会话隔离；刷新页面/关闭浏览器丢失——服务端不感知）。 */
export interface QueuedInput {
  id: string
  text: string
  /** 已上传会话 tmp/ 的附件引用（入队时上传，执行时直用）。 */
  files: Array<{ name: string; mime: string; path: string }>
  messageId: string
  createdAt: number
}

/** 各会话排队输入。 */
const queues = new Map<string, QueuedInput[]>()
/** 正在编辑的排队条目 id（chip 内联编辑态）。 */
let editingId: string | null = null
/** 任务结束后的队列消化执行器（main.ts 注入：渲染用户消息 + 发送任务流）。 */
let executor: ((sessionId: string, item: QueuedInput) => Promise<void>) | null = null
export function setQueueExecutor(fn: typeof executor): void {
  executor = fn
}

/**
 * 消化队列（幂等）：会话不在运行且队列非空时取队首执行。
 * 触发点：任务流收尾（consumeTaskStream finally）与入队后（覆盖「运行恰在入队前结束」的竞态窗口）。
 */
export function drainQueue(sessionId: string): void {
  if (runs.has(sessionId)) return
  const q = queues.get(sessionId)
  if (!q?.length) return
  const item = q.shift()!
  if (!q.length) queues.delete(sessionId)
  renderQueue()
  if (executor) {
    void executor(sessionId, item).catch((err) => toast(`排队输入执行失败: ${(err as Error).message}`, "error"))
  }
}

/** 入队（任务运行期间提交的输入）：排队等待当前任务结束后自动执行。 */
export function enqueueInput(sessionId: string, item: QueuedInput): void {
  const q = queues.get(sessionId) ?? []
  q.push(item)
  queues.set(sessionId, q)
  renderQueue()
  drainQueue(sessionId) // 运行中为空转；覆盖运行恰已结束的竞态
}

/** 中断插入（Ctrl+Enter / 排队条⚡）：插队首并取消当前任务循环，其收尾后立即执行队首。 */
export function enqueueFront(sessionId: string, item: QueuedInput): void {
  const q = queues.get(sessionId) ?? []
  q.unshift(item)
  queues.set(sessionId, q)
  renderQueue()
  if (runs.has(sessionId)) void client.cancelTask(sessionId).catch(() => {})
  else drainQueue(sessionId)
}

/** 编辑未执行的排队输入。 */
export function editQueuedInput(sessionId: string, id: string, text: string): boolean {
  const item = (queues.get(sessionId) ?? []).find((x) => x.id === id)
  if (!item) return false
  item.text = text
  renderQueue()
  return true
}

/** 撤回（移除）未执行的排队输入。 */
export function removeQueuedInput(sessionId: string, id: string): boolean {
  const q = queues.get(sessionId) ?? []
  const idx = q.findIndex((x) => x.id === id)
  if (idx < 0) return false
  q.splice(idx, 1)
  if (!q.length) queues.delete(sessionId)
  renderQueue()
  return true
}

/** 会话删除：清理其排队状态。 */
export function clearQueue(sessionId: string): void {
  queues.delete(sessionId)
  renderQueue()
}

/** 渲染当前会话的排队条（仅当前会话可见；空队列整条隐藏）。 */
export function renderQueue(): void {
  const cur = getCurrentSession()
  const items = cur ? queues.get(cur.id) ?? [] : []
  queueEl.textContent = ""
  if (!cur || !items.length) {
    queueEl.hidden = true
    return
  }
  queueEl.hidden = false
  const head = el("div", "queue-head")
  head.append(el("span", "queue-title", `⏳ 已排队 ${items.length} 条`))
  head.append(el("span", "queue-hint", "当前回答结束后自动按序发送；Ctrl+Enter 可中断插入"))
  queueEl.appendChild(head)
  items.forEach((item, idx) => queueEl.appendChild(queueChip(cur.id, item, idx + 1)))
}

/** 单条排队输入 chip：序号 + 内容预览（附件计数）+ 立即执行/编辑/撤回。 */
function queueChip(sessionId: string, item: QueuedInput, ord: number): HTMLElement {
  const chip = el("div", "queue-item")
  chip.dataset.id = item.id
  chip.append(el("span", "queue-ord", String(ord)))
  if (editingId === item.id) {
    // 内联编辑态：textarea + 保存/取消
    const editor = el("div", "queue-editor")
    const ta = document.createElement("textarea")
    ta.value = item.text
    ta.rows = 2
    const actions = el("div", "queue-editor-actions")
    const save = el("button", "queue-save", "保存")
    save.onclick = () => {
      const val = ta.value.trim()
      if (!val) return
      editQueuedInput(sessionId, item.id, val)
      editingId = null
      renderQueue()
    }
    const cancel = el("button", "queue-cancel", "取消")
    cancel.onclick = () => {
      editingId = null
      renderQueue()
    }
    actions.append(save, cancel)
    editor.append(ta, actions)
    chip.appendChild(editor)
    setTimeout(() => {
      ta.focus()
      ta.setSelectionRange(ta.value.length, ta.value.length)
    }, 0)
    return chip
  }
  const text = el("div", "queue-text")
  text.textContent = item.text
  if (item.files.length) text.append(el("span", "queue-files", `📎 ${item.files.length}`))
  tip(text, item.text)
  chip.appendChild(text)
  const actions = el("div", "queue-actions")
  const run = el("button", "queue-act queue-run", "⚡")
  tip(run, "立即执行（中断当前任务）")
  run.onclick = () => {
    const q = queues.get(sessionId) ?? []
    const idx = q.findIndex((x) => x.id === item.id)
    if (idx < 0) return
    q.splice(idx, 1)
    q.unshift(item)
    queues.set(sessionId, q)
    renderQueue()
    if (runs.has(sessionId)) void client.cancelTask(sessionId).catch(() => {})
    else drainQueue(sessionId)
  }
  const edit = el("button", "queue-act", "✎")
  tip(edit, "编辑")
  edit.onclick = () => {
    editingId = item.id
    renderQueue()
  }
  const del = el("button", "queue-act queue-del", "✕")
  tip(del, "撤回")
  del.onclick = () => removeQueuedInput(sessionId, item.id)
  actions.append(run, edit, del)
  chip.appendChild(actions)
  return chip
}
