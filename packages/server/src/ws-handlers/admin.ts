/** WS 管理面消息：user.*（admin 用户管理）+ feedback.*（反馈提交/查询）+ sub_agent.*（子Agent 装载）。 */
import type { WsHandler } from "./context"
import type { FeedbackInfo, FeedbackInput, UserPatch } from "@gebai/sdk"
import { SERVICE_USER } from "../app"
import { readFeedback, writeFeedback } from "../feedback"
import { isValidSessionId } from "../core/base/paths"

export const userHandlers: Record<string, WsHandler> = {
  "user.list": async ({ d, user, reply }) => {
    if (user.role !== "admin") return reply(false, undefined, "admin only")
    const users = await d.auth.listUsers()
    return reply(true, { users: users.map((u) => d.auth.strip(u)) })
  },
  "user.create": async ({ d, user, p, reply }) => {
    if (user.role !== "admin") return reply(false, undefined, "admin only")
    const u = await d.auth.createUser(String(p.username), String(p.password), p.role as "user" | "admin" | undefined)
    return reply(true, { user: d.auth.strip(u) })
  },
  "user.update": async ({ d, user, p, reply }) => {
    if (user.role !== "admin") return reply(false, undefined, "admin only")
    const u = await d.auth.updateUser(String(p.id), (p.patch as UserPatch) || {})
    return reply(true, { user: d.auth.strip(u) })
  },
  "user.delete": async ({ d, user, p, reply }) => {
    if (user.role !== "admin") return reply(false, undefined, "admin only")
    await d.auth.deleteUser(String(p.id))
    return reply(true)
  },
}

export const feedbackHandlers: Record<string, WsHandler> = {
  "feedback.submit": async ({ d, user, p, reply }) => {
    const fb = p.feedback as FeedbackInput
    const id = await writeFeedback(d.config.gebaiHome, user.id, fb)
    return reply(true, { id })
  },
  "feedback.list": async ({ d, user, p, reply }) => {
    let list: FeedbackInfo[] = []
    if (user.role === "admin") {
      const users = await d.auth.listUsers()
      const targets = [...users, d.auth.defaultUser(), SERVICE_USER]
      const seen = new Set<string>()
      for (const u of targets) {
        if (seen.has(u.id)) continue
        seen.add(u.id)
        list = list.concat(await readFeedback(d.config.gebaiHome, u.id))
      }
    } else {
      list = await readFeedback(d.config.gebaiHome, user.id)
    }
    const filter = (p.filter as Record<string, string>) || {}
    if (filter.messageId) list = list.filter((f) => f.messageId === filter.messageId)
    if (filter.sessionId) list = list.filter((f) => f.sessionId === filter.sessionId)
    if (filter.type) list = list.filter((f) => f.type === filter.type)
    return reply(true, { feedback: list })
  },
}

export const subAgentHandlers: Record<string, WsHandler> = {
  "sub_agent.list": ({ d, reply }) => reply(true, { subAgents: d.subAgents.list() }),
  "sub_agent.load": async ({ d, user, p, reply }) => {
    const name = String(p.name)
    if (p.sessionId) {
      // 会话级装载：注册工具 + 提示词消息写入该会话记录并落盘（恢复历史会话时按记录还原）
      try {
        await d.engine.loadAgentToSession(String(p.sessionId), user.id, name)
        return reply(true)
      } catch (err) {
        return reply(false, undefined, String((err as Error).message || err))
      }
    }
    // 全局装载（无 sessionId）变更所有用户的工具面（REST 同能力仅 admin）：服务模式需 admin
    if (d.config.auth === "server" && user.role !== "admin") return reply(false, undefined, "admin only（全局装载影响所有用户，会话级装载请传 sessionId）")
    await d.subAgents.load(name)
    return reply(true)
  },
  "sub_agent.unload": async ({ d, user, p, reply }) => {
    // 卸载子Agent：带 sessionId 时同步清理该会话内已持久化的提示词消息与装载记录（与 load 对称，
    // 卸载后提示词不再占用上下文；ensureSessionAgents 也不会再按记录重新装载）
    const name = String(p.name)
    if (p.sessionId) {
      const sid = String(p.sessionId)
      if (!isValidSessionId(sid)) return reply(false, undefined, `invalid session id: ${sid}`)
      try {
        await d.engine.unloadAgentFromSession(sid, user.id, name)
        return reply(true)
      } catch (err) {
        return reply(false, undefined, String((err as Error).message || err))
      }
    }
    // 同 sub_agent.load：全局卸载仅 admin（会话级卸载传 sessionId 不受限）
    if (d.config.auth === "server" && user.role !== "admin") return reply(false, undefined, "admin only（全局卸载影响所有用户，会话级卸载请传 sessionId）")
    d.subAgents.unload(name)
    return reply(true)
  },
}
