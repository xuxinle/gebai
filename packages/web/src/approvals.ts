import { approvalsEl, attachBtn, client, el, getCurrentSession, input, pendingTools, pendingToolsKey, sendBtn } from "./state"
import { focusInput } from "./state"
import type { KeyBinding } from "./keymap"
import { displayToolName } from "./tool-cards"
import { makeCardFoldable } from "./card-fold"
import { toast } from "./ui"

/* ---------- 审批卡片 ---------- */

/** 每会话待审批卡片数：同一会话全部处理完才解除输入锁定。 */
const pendingBySession = new Map<string, number>()

/** 待决卡片增减通知：会话列表的运行角标要据此变成「⌛ 等待你的输入」（它直接数 DOM 里的卡）。
 *  用事件而不是直接调 sessions（sessions 已导入本模块，反向导入会成环）。 */
function notifyPendingChange(): void {
  document.dispatchEvent(new CustomEvent("gebai:pending-change"))
}

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

/** 该会话是否有等待用户作答的卡片（审批 + 选择/填值）：交互等待不是「流挂起」，
 *  空闲看门狗不得据此取消任务（服务端等待超时后会推来结果事件自行刷新活跃时间）。 */
export function hasPendingInteraction(sessionId: string): boolean {
  const id = CSS.escape(sessionId)
  return !!approvalsEl.querySelector(`.approval[data-session="${id}"], .interaction-card[data-session="${id}"]`)
}

export function addApproval(sessionId: string, toolCallId: string, tool: string) {
  // 同 toolCallId 重复推送（无 seq 服务端的事件重放/重放应答窗口）替换旧卡不堆叠——
  // 同款防御已存在于选择/环境变量卡（choiceId/envId），审批卡此前缺失；堆叠会虚高
  // pendingBySession 计数，处理一张后输入持续锁定
  for (const old of approvalsEl.querySelectorAll<HTMLElement>(`.approval[data-toolcall="${CSS.escape(toolCallId)}"]`)) old.remove()
  const box = el("div", "approval")
  box.dataset.session = sessionId
  box.dataset.toolcall = toolCallId
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
  /** 收卡：移除并解除输入锁定（决策送达或服务端已确认该请求失效时调用）。 */
  const drop = () => {
    box.remove()
    const left = (pendingBySession.get(sessionId) ?? 1) - 1
    if (left > 0) pendingBySession.set(sessionId, left)
    else pendingBySession.delete(sessionId)
    syncLock()
    notifyPendingChange()
  }
  let submitting = false
  // 决策送达才收卡：断线/请求失败时保留卡片并提示——此前无条件移除且吞掉错误，
  // 界面「已通过」而引擎仍在等待（用户与模型状态错位）
  const resolve = async (approve: boolean) => {
    if (submitting) return
    submitting = true
    try {
      // 审批绑定来源会话：即使已切到其他会话，也向正确会话的引擎提交决策
      await client.decideApproval(sessionId, toolCallId, approve)
    } catch (err) {
      submitting = false
      if ((err as Error & { code?: string }).code === "expired") {
        // 服务端已无此等待（等待超时/已在其他页面处理/任务已结束）：卡片已失效，移除并说明
        drop()
        toast("该审批请求已失效（等待超时或已在其他页面处理）。")
        return
      }
      toast(`审批提交失败：${(err as Error).message || "连接不可用"}，请重试。`)
      return
    }
    if (!approve) pendingTools.delete(pendingToolsKey(sessionId, toolCallId)) // 拒绝后该工具不会产生结果，清理配对
    drop()
  }
  yes.onclick = () => void resolve(true)
  no.onclick = () => void resolve(false)
  // 悬浮提示快捷键（键盘 Y/N 全局生效，见文件底部绑定）
  yes.title = "通过 (Y)"
  no.title = "拒绝 (N)"
  actions.append(yes, no)
  // 卡体（图标 + 文案）与操作区分列：卡体是折叠/限高作用的内容区（单行审批卡通常不触发，
  // 内容超出一屏高度时同样可收缩）
  const body = el("div", "approval-body")
  body.append(ico, txt)
  box.append(body, actions)
  // 先入文档再装折叠：折叠控件按真实内容高度决定是否出现
  approvalsEl.appendChild(box)
  makeCardFoldable(box, body)
  pendingBySession.set(sessionId, (pendingBySession.get(sessionId) ?? 0) + 1)
  applyApprovalVisibility()
  syncLock()
  notifyPendingChange()
  // 当前会话弹出审批：锁定页面并聚焦到卡片（键盘焦点落到审批，输入已禁用）
  if (getCurrentSession()?.id === sessionId) {
    box.tabIndex = -1
    box.scrollIntoView({ block: "nearest" })
    box.focus()
  }
}

/* ---------- 键盘快捷键：Y = 通过、N = 拒绝 ---------- */

/**
 * 卡片可见时按 Y/N 直接处理最早等待的审批卡片。
 * 修饰键、长按重复、输入框焦点、输入法组合态都不触发——这些守卫由 keymap 统一负责
 * （此前本模块自己写了一份，与其他模块的写法各有出入）。
 */
export const approvalBindings: KeyBinding[] = [
  {
    id: "main.approval.approve",
    keys: "Y",
    label: "通过最早等待的审批卡片",
    group: "main.approval",
    run: () => clickDecision(true),
  },
  {
    id: "main.approval.reject",
    keys: "N",
    label: "拒绝最早等待的审批卡片",
    group: "main.approval",
    run: () => clickDecision(false),
  },
]

function clickDecision(approve: boolean): void {
  const card = approvalsEl.querySelector<HTMLElement>(".approval:not([hidden])")
  card?.querySelector<HTMLButtonElement>(approve ? ".yes" : ".no")?.click()
}
