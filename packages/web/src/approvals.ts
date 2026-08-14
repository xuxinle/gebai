import { approvalsEl, attachBtn, client, el, getCurrentSession, input, pendingTools, pendingToolsKey, sendBtn } from "./state"
import { focusInput } from "./state"
import { displayToolName } from "./tool-cards"

/* ---------- 审批卡片 ---------- */

/** 每会话待审批卡片数：同一会话全部处理完才解除输入锁定。 */
const pendingBySession = new Map<string, number>()

/** 输入锁定：当前会话有待审批卡片时禁止输入/发送/附件，全部处理后恢复焦点。 */
function syncLock() {
  const cur = getCurrentSession()
  const locked = !!cur && (pendingBySession.get(cur.id) ?? 0) > 0
  if (!locked && input.disabled) focusInput() // 解锁瞬间恢复输入焦点
  input.disabled = locked
  sendBtn.disabled = locked
  attachBtn.disabled = locked
}

/** 会话切换后调用：只显示当前会话的卡片（审批 + 选择/环境变量填值），其余隐藏（切回恢复）。 */
export function applyApprovalVisibility() {
  const cur = getCurrentSession()
  for (const card of approvalsEl.querySelectorAll<HTMLElement>(".approval, .interaction-card")) {
    card.hidden = !cur || card.dataset.session !== cur.id
  }
  syncLock()
}

/** 任务结束清理：移除该会话全部审批卡片并解除输入锁定（审批随任务终止失效）。 */
export function clearApprovals(sessionId: string) {
  let removed = 0
  for (const card of approvalsEl.querySelectorAll<HTMLElement>(".approval")) {
    if (card.dataset.session === sessionId) {
      card.remove()
      removed++
    }
  }
  if (removed > 0) {
    pendingBySession.delete(sessionId)
    syncLock()
  }
}

export function addApproval(sessionId: string, toolCallId: string, tool: string) {
  const box = el("div", "approval")
  box.dataset.session = sessionId
  const ico = el("span", "approval-ico", "⚠️")
  const txt = el("div", "approval-txt")
  const toolName = displayToolName(tool)
  const toolEl = el("div", "approval-tool", toolName)
  toolEl.title = toolName // 截断后悬浮可见完整工具名
  txt.append(el("div", "approval-title", "等待审批"), toolEl)
  const actions = el("div", "approval-actions")
  const yes = el("button", "yes", "通过")
  const no = el("button", "no", "拒绝")
  // 按钮内可见快捷键提示（矩阵主题自带 [Y]/[N] 前缀，由主题隐藏提示）
  yes.append(el("span", "kbd-hint", "Y"))
  no.append(el("span", "kbd-hint", "N"))
  const resolve = (approve: boolean) => {
    // 审批绑定来源会话：即使已切到其他会话，也向正确会话的引擎提交决策
    void client.decideApproval(sessionId, toolCallId, approve).catch(() => {})
    if (!approve) pendingTools.delete(pendingToolsKey(sessionId, toolCallId)) // 拒绝后该工具不会产生结果，清理配对
    box.remove()
    const left = (pendingBySession.get(sessionId) ?? 1) - 1
    if (left > 0) pendingBySession.set(sessionId, left)
    else pendingBySession.delete(sessionId)
    syncLock()
  }
  yes.onclick = () => resolve(true)
  no.onclick = () => resolve(false)
  // 悬浮提示快捷键（键盘 Y/N 全局生效，见文件底部绑定）
  yes.title = "通过 (Y)"
  no.title = "拒绝 (N)"
  actions.append(yes, no)
  box.append(ico, txt, actions)
  approvalsEl.appendChild(box)
  pendingBySession.set(sessionId, (pendingBySession.get(sessionId) ?? 0) + 1)
  applyApprovalVisibility()
  syncLock()
  // 当前会话弹出审批：锁定页面并聚焦到卡片（键盘焦点落到审批，输入已禁用）
  if (getCurrentSession()?.id === sessionId) {
    box.tabIndex = -1
    box.scrollIntoView({ block: "nearest" })
    box.focus()
  }
}

/* ---------- 键盘快捷键：Y = 通过、N = 拒绝 ---------- */

/** 卡片可见时按 Y/N 直接处理最早等待的审批卡片；带修饰键或长按不触发。 */
document.addEventListener("keydown", (e) => {
  if (e.repeat) return
  const key = e.key
  if (key !== "y" && key !== "Y" && key !== "n" && key !== "N") return
  if (e.ctrlKey || e.metaKey || e.altKey) return
  const card = approvalsEl.querySelector<HTMLElement>(".approval:not([hidden])")
  if (!card) return
  const btn = card.querySelector<HTMLButtonElement>(key === "y" || key === "Y" ? ".yes" : ".no")
  btn?.click()
})
