/** 运行中会话附加恢复（自 main.ts 拆分）：attach 快照应用与待决交互卡重渲染。 */
import type { PendingInteraction } from "@gebai/sdk"
import { addApproval } from "./approvals"
import { onCaptureRequest, onDrawRender } from "./events"
import { renderChoiceCard, renderEnvRequestCard } from "./messages"
import { setRunningAttach } from "./sessions"
import { attaching, client, runs } from "./state"
import { consumeTaskStream } from "./stream"
function renderPendingInteraction(sessionId: string, it: PendingInteraction): void {
  if (it.type === "approval") addApproval(sessionId, it.toolCallId, it.tool)
  else if (it.type === "choice") renderChoiceCard(it.prompt, it.options, it.choiceId, sessionId, it.multi, it.plan ? { title: it.plan.title, content: it.plan.content, path: it.plan.path } : undefined)
  else if (it.type === "env") renderEnvRequestCard(it.name, it.description, it.secret, it.envId, sessionId)
  else if (it.type === "draw") onDrawRender({ sessionId, renderId: it.renderId, code: it.code, format: it.format ?? "" })
  else if (it.type === "capture") onCaptureRequest({ sessionId, captureId: it.captureId, fullPage: it.fullPage, delay: it.delay })
}

/** 附加运行中会话（DESIGN「运行中会话恢复」，loadMessages 尾部钩子调用）：
 *  页面刷新/切换进入运行中会话时——快照（在途流 + 待决交互）→ 待决卡片重建（审批/选择/填值/
 *  画图/捕获的事件已推送过、本页收不到，不重建则任务干等到超时）→ consumeTaskStream 接管
 *  attachStream（在途文本种子 + 实时续流，与发起页同构渲染：流式消息/工具卡/信号灯/停止按钮/
 *  单轮计时）。未运行或本页已接管（发起/附加过）时 no-op。 */
async function attachRunningSession(sessionId: string): Promise<void> {
  if (runs.has(sessionId) || attaching.has(sessionId)) return
  attaching.add(sessionId)
  try {
    const snap = await client.attachSession(sessionId)
    if (!snap?.running) return
    for (const it of snap.pending ?? []) renderPendingInteraction(sessionId, it)
    await consumeTaskStream(sessionId, (run) => client.attachStream(sessionId, { signal: run.abort.signal }), { startedAt: snap.startedAt })
  } catch {
    /* 附加失败（连接抖动等）：视图保持存储渲染，下次进入会话重试 */
  } finally {
    attaching.delete(sessionId)
  }
}
setRunningAttach(attachRunningSession)
