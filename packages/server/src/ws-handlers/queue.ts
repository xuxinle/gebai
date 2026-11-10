/** WS 会话输入队列域消息：session.queue.list/edit/remove/run。
 *
 *  队列真相在服务端（`core/session/input-queue`）：任务运行期间提交的输入排队等待，当前任务收尾后
 *  自动按序执行；前端只读展示与转发用户意图（编辑/撤回/立即执行），不再自行持队列。
 *  入队本身走 `session.prompt`（`queue: true`）——与直接发送同一入口，队列只是「稍后执行」的姿态。 */
import type { WsHandler } from "./context"

export const queueHandlers: Record<string, WsHandler> = {
  "session.queue.list": async ({ d, user, p, reply }) => {
    // 单会话排队清单（断线重连/进入会话时对齐用；快照 state.snapshot 已带全量，此接口供单点刷新）
    const sessionId = String(p.id)
    const s = await d.store.load(sessionId, user.id)
    if (!s) return reply(false, { code: "session_not_found" }, "session not found")
    return reply(true, { items: d.inputQueue.listPublic(sessionId, user.id) })
  },
  "session.queue.edit": async ({ d, user, p, reply }) => {
    // 编辑未执行的排队输入（内容替换；空内容拒绝）
    const sessionId = String(p.id)
    const s = await d.store.load(sessionId, user.id)
    if (!s) return reply(false, { code: "session_not_found" }, "session not found")
    const ok = d.inputQueue.edit(sessionId, String(p.itemId ?? ""), String(p.text ?? ""), user.id)
    if (!ok) return reply(false, { code: "queue_item_not_found" }, "排队条目不存在或内容为空")
    return reply(true, { items: d.inputQueue.listPublic(sessionId, user.id) })
  },
  "session.queue.remove": async ({ d, user, p, reply }) => {
    // 撤回（移除）未执行的排队输入
    const sessionId = String(p.id)
    const s = await d.store.load(sessionId, user.id)
    if (!s) return reply(false, { code: "session_not_found" }, "session not found")
    const ok = d.inputQueue.remove(sessionId, String(p.itemId ?? ""), user.id)
    if (!ok) return reply(false, { code: "queue_item_not_found" }, "排队条目不存在")
    return reply(true, { items: d.inputQueue.listPublic(sessionId, user.id) })
  },
  "session.queue.run": async ({ d, user, p, reply }) => {
    // 立即执行（排队条 ⚡）：条目置队首并中断当前任务，其收尾后立即执行该条
    const sessionId = String(p.id)
    const s = await d.store.load(sessionId, user.id)
    if (!s) return reply(false, { code: "session_not_found" }, "session not found")
    const ok = d.inputQueue.promote(sessionId, String(p.itemId ?? ""), user.id)
    if (!ok) return reply(false, { code: "queue_item_not_found" }, "排队条目不存在")
    // 运行中才需要中断：空闲时 promote 已直接派发（队列内部判定）
    if (d.engine.isRunning(sessionId)) d.engine.cancel(sessionId)
    return reply(true, { items: d.inputQueue.listPublic(sessionId, user.id) })
  },
}
