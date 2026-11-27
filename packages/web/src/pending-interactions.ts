/** 待决交互卡重建：服务端的审批/选择/填值/画图/捕获请求**只推送一次**（事件已进日志但可能超出重放窗口），
 *  页面刷新、缺口重同步（overrun）后本页收不到——凭服务端待决清单重建卡片，任务才不至于干等到等待超时。
 *  两个入口共用本模块：attach 快照（session.attach 的 pending）与缺口后重同步。
 *
 *  快照同时是**收敛真值**：本页手里的卡若快照里已经没有（交互在别处被作答/超时/任务已在离线期间结束，
 *  而本页错过了 `event.interaction.settled`），就应就地撤掉——留着一张点了才知道失效的死卡只会误导。 */
import type { AttachSnapshot, PendingInteraction } from "@gebai/sdk"
import { addApproval, settleApproval } from "./approvals"
import { onCaptureRequest, onDrawRender, onToolCall } from "./events"
import { renderChoiceCard, renderEnvRequestCard, settleInteractionCard } from "./messages"
import { approvalsEl, client } from "./state"

/** 渲染一条待决交互（同 reqId 重复重建由各卡片自身的替换逻辑去重）。 */
export function renderPendingInteraction(sessionId: string, it: PendingInteraction): void {
  if (it.type === "approval") addApproval(sessionId, it.toolCallId, it.tool)
  else if (it.type === "choice") renderChoiceCard(it.prompt, it.options, it.choiceId, sessionId, it.multi, it.plan ? { title: it.plan.title, content: it.plan.content, path: it.plan.path } : undefined)
  else if (it.type === "env") renderEnvRequestCard(it.name, it.description, it.secret, it.envId, sessionId)
  else if (it.type === "draw") onDrawRender({ sessionId, renderId: it.renderId, code: it.code, format: it.format ?? "" })
  else if (it.type === "capture") onCaptureRequest({ sessionId, captureId: it.captureId, fullPage: it.fullPage, delay: it.delay })
}

/** 页面上该会话现有待决卡的标识（审批卡取 toolCallId，交互卡取 reqId/choiceId）。
 *  **取快照之前先拍这张底**：请求期间新到的卡不在其中，紧接着的收敛就不会把新卡误摘。 */
export function pendingCardKeys(sessionId: string): { approvals: string[]; cards: string[] } {
  const sid = CSS.escape(sessionId)
  const approvals = [...approvalsEl.querySelectorAll<HTMLElement>(`.approval[data-session="${sid}"]`)].map((c) => c.dataset.toolcall ?? "").filter(Boolean)
  const cards = [...approvalsEl.querySelectorAll<HTMLElement>(`.interaction-card[data-session="${sid}"]`)].map((c) => c.dataset.reqId ?? "").filter(Boolean)
  return { approvals, cards }
}

/** 按快照收敛待决卡片：先摘掉「快照里已经没有」且**取快照前就在页面上**的卡（交互已在别处了结，
 *  本页错过了事件），再重建在册的卡与在途工具卡。 */
export function applyPendingSnapshot(
  sessionId: string,
  snap: Pick<AttachSnapshot, "pending" | "tools"> | null | undefined,
  before?: { approvals: string[]; cards: string[] },
): void {
  const pending = snap?.pending ?? []
  if (before) {
    const keepApprovals = new Set(pending.filter((p) => p.type === "approval").map((p) => (p as { toolCallId: string }).toolCallId))
    const keepCards = new Set(
      pending.map((p) => (p.type === "choice" ? p.choiceId : p.type === "env" ? p.envId : "")).filter(Boolean),
    )
    for (const id of before.approvals) if (!keepApprovals.has(id)) settleApproval(sessionId, id)
    for (const id of before.cards) if (!keepCards.has(id)) settleInteractionCard(sessionId, id)
  }
  for (const it of pending) renderPendingInteraction(sessionId, it)
  renderPendingToolCalls(sessionId, snap?.tools)
}

/** 重建在途工具卡（attach 快照/缺口重同步共用）：走实时事件的同一渲染入口（`onToolCall`）——参数展示、
 *  toolCallId 配对与后续结果填充与在线路径完全同构（不重建则「等待中的工具卡」随页面丢失）。 */
export function renderPendingToolCalls(
  sessionId: string,
  tools: Array<{ toolCallId: string; name: string; arguments?: Record<string, unknown>; subSessionId?: string }> | undefined,
): void {
  for (const t of tools ?? []) onToolCall({ sessionId, toolCallId: t.toolCallId, name: t.name, arguments: t.arguments, subSessionId: t.subSessionId })
}

/** 重取待决交互与在途工具调用并重建卡片（日志缺口后的重同步路径；失败静默——下次进入会话/重连由 attach 兜底）。
 *  快照同样用来收敛：本页错过「在别处了结」事件时，快照里已没有的卡会被摘掉。 */
export async function resyncPendingInteractions(sessionId: string): Promise<void> {
  const before = pendingCardKeys(sessionId)
  try {
    const snap = await client.attachSession(sessionId)
    applyPendingSnapshot(sessionId, snap, before)
  } catch {
    /* 连接仍不稳定：交给 attach 路径重试 */
  }
}
