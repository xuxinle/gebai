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
 */
import type { QueuedPrompt } from "@gebai/sdk"
import { client, el, getCurrentSession, queueEl } from "./state"
import { tip, toast } from "./ui"

/** 各会话排队输入（服务端推送的镜像，按会话隔离）。 */
const queues = new Map<string, QueuedPrompt[]>()
/** 正在编辑的排队条目 id（chip 内联编辑态）。 */
let editingId: string | null = null

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
function queueChip(sessionId: string, item: QueuedPrompt, ord: number): HTMLElement {
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
  const files = item.attachments?.length ?? 0
  if (files) text.append(el("span", "queue-files", `📎 ${files}`))
  tip(text, item.text)
  chip.appendChild(text)
  const actions = el("div", "queue-actions")
  const run = el("button", "queue-act queue-run", "⚡")
  tip(run, "立即执行（中断当前任务）")
  run.onclick = () => runQueuedInput(sessionId, item.id)
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
