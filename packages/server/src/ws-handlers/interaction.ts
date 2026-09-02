/** WS 交互决策域消息：审批/选择/环境填值/画图回传/页面捕获（引擎五种阻塞等待的 decide 入口）。 */
import type { WsHandler } from "./context"

export const interactionHandlers: Record<string, WsHandler> = {
  "approval.decide": async ({ d, p, user, reply }) => {
    const sessionId = String(p.id)
    const s = await d.store.load(sessionId, user.id)
    if (!s) return reply(false, undefined, "session not found")
    await d.engine.decideApproval(sessionId, String(p.toolCallId), Boolean(p.approve))
    return reply(true)
  },
  "choice.decide": async ({ d, p, user, reply }) => {
    // 提交用户选择（ask 选项询问分支阻塞等待）；option 单选 / options 数组多选 / refuse=true（或均缺失）拒绝回答
    const sessionId = String(p.id)
    const s = await d.store.load(sessionId, user.id)
    if (!s) return reply(false, undefined, "session not found")
    const multi = Array.isArray(p.options)
    const refuse = p.refuse === true || (!multi && p.option == null)
    await d.engine.decideChoice(sessionId, String(p.choiceId), refuse ? null : multi ? (p.options as unknown[]).map(String) : String(p.option))
    return reply(true)
  },
  "env.decide": async ({ d, p, user, reply }) => {
    // 提交用户填写的环境变量值（ask 填值分支阻塞等待）；value 缺失视为拒绝
    const sessionId = String(p.id)
    const s = await d.store.load(sessionId, user.id)
    if (!s) return reply(false, undefined, "session not found")
    const value = p.value == null ? null : String(p.value)
    await d.engine.decideEnvResult(sessionId, String(p.envId ?? ""), value)
    return reply(true)
  },
  "draw.result": async ({ d, p, user, reply }) => {
    // 提交前端渲染结果（show 图表分支阻塞等待）
    const sessionId = String(p.id)
    const s = await d.store.load(sessionId, user.id)
    if (!s) return reply(false, undefined, "session not found")
    await d.engine.decideDrawResult(sessionId, String(p.renderId), { ok: Boolean(p.ok), error: p.error != null ? String(p.error) : undefined })
    return reply(true)
  },
  "capture.result": async ({ d, p, user, reply }) => {
    // 提交前端页面捕获结果（page_capture 工具阻塞等待）
    const sessionId = String(p.id)
    const s = await d.store.load(sessionId, user.id)
    if (!s) return reply(false, undefined, "session not found")
    await d.engine.decideCaptureResult(sessionId, String(p.captureId), {
      html: String(p.html ?? ""),
      imageBase64: p.imageBase64 != null ? String(p.imageBase64) : undefined,
      error: p.error != null ? String(p.error) : undefined,
    })
    return reply(true)
  },
}
