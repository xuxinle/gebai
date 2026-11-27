/** WS 多人会话（群）域消息：group.list/create/join/get/update_passcode/rename/leave/dissolve/set_profile。
 *
 *  设计要点（DESIGN「多人会话（群）」）：
 *  - **不放在任何用户目录下**：群会话正文落在 `{GEBAI_HOME}/groups/{groupId}/`（`core/session/groups.ts`）；
 *  - **名称与角色不与账号绑定**：进群时自设 `name/role`，同一个账号在不同群里可以是不同身份；
 *  - 邀请码是所有成员的进入凭据，**进入口令**（可选）由群主加设，scrypt 加盐哈希落盘；
 *  - 建群/入群成功后自动把连接级当前会话切到该群会话（用户点了「创建/加入」就是要进这个群说话）。
 */
import type { WsHandler } from "./context"
import { toSessionInfo } from "../core/session/store"

/** 群存储不可用（未装配）时的统一应答。 */
function noGroups(reply: (ok: boolean, payload?: Record<string, unknown>, error?: string) => void) {
  return reply(false, { code: "groups_unavailable" }, "多人会话能力未启用")
}

/**
 * 建群/入群应答：群视图 + 群会话信息。
 *
 * 会话信息的 `group` 字段必须带**请求者在群内的身份**（名称/角色）：前端拿到后就地设为当前会话，
 * 标题栏身份条与会话列表据此渲染——占位空串会让界面先闪一下「（未设群内身份）」。
 */
async function groupCreateReply(d: { store: import("../core/session/store").SessionStore; groups?: import("../core/session/groups").GroupStore }, groupId: string, userId: string) {
  const view = d.groups?.view(groupId, userId) ?? null
  const session = view ? await d.store.load(view.sessionId, userId) : null
  return {
    group: view,
    session: session && view?.me
      ? { ...toSessionInfo(session), group: { id: view.id, name: view.name, memberName: view.me.name, role: view.me.role } }
      : session
        ? toSessionInfo(session)
        : null,
  }
}

export const groupHandlers: Record<string, WsHandler> = {
  "group.list": async ({ d, user, reply }) => {
    if (!d.groups) return noGroups(reply)
    return reply(true, { groups: d.groups.listViews(user.id) })
  },

  "group.create": async ({ d, user, p, conn, reply }) => {
    if (!d.groups) return noGroups(reply)
    try {
      const g = await d.groups.create(user.id, user.username ?? user.id, {
        name: p.name,
        passcode: p.passcode,
        memberName: p.memberName,
        memberRole: p.memberRole,
      })
      // 建群即进入：连接级当前会话切到群会话（前端随后 attach/载入消息）
      conn.setCurrent(g.sessionId)
      return reply(true, await groupCreateReply(d, g.id, user.id))
    } catch (err) {
      return reply(false, undefined, String((err as Error).message || err))
    }
  },

  "group.join": async ({ d, user, p, conn, reply }) => {
    if (!d.groups) return noGroups(reply)
    try {
      const g = await d.groups.join(user.id, user.username ?? user.id, {
        joinCode: p.joinCode,
        passcode: p.passcode,
        memberName: p.memberName,
        memberRole: p.memberRole,
      })
      conn.setCurrent(g.sessionId)
      return reply(true, await groupCreateReply(d, g.id, user.id))
    } catch (err) {
      return reply(false, undefined, String((err as Error).message || err))
    }
  },

  "group.get": async ({ d, user, p, reply }) => {
    if (!d.groups) return noGroups(reply)
    const gid = String(p.id ?? p.groupId ?? "")
    const v = d.groups.view(gid, user.id)
    // 非成员看不到群细节（成员表/邀请码）：与「群不存在」同一应答，不泄露存在性
    if (!v || !v.me) return reply(false, { code: "group_not_found" }, "群不存在或你不是该群成员")
    return reply(true, { group: v })
  },

  "group.set_profile": async ({ d, user, p, reply }) => {
    if (!d.groups) return noGroups(reply)
    try {
      const g = await d.groups.setProfile(user.id, String(p.id ?? p.groupId ?? ""), p.memberName ?? p.name, p.memberRole ?? p.role)
      return reply(true, { group: d.groups.view(g.id, user.id) })
    } catch (err) {
      return reply(false, undefined, String((err as Error).message || err))
    }
  },

  "group.rename": async ({ d, user, p, reply }) => {
    if (!d.groups) return noGroups(reply)
    try {
      const g = await d.groups.rename(user.id, String(p.id ?? p.groupId ?? ""), p.name)
      return reply(true, { group: d.groups.view(g.id, user.id) })
    } catch (err) {
      return reply(false, undefined, String((err as Error).message || err))
    }
  },

  "group.set_passcode": async ({ d, user, p, reply }) => {
    if (!d.groups) return noGroups(reply)
    try {
      // passcode 缺省/null/空串 = 取消口令（开放给持有邀请码的人）
      const g = await d.groups.setPasscode(user.id, String(p.id ?? p.groupId ?? ""), p.passcode ?? null)
      return reply(true, { group: d.groups.view(g.id, user.id) })
    } catch (err) {
      return reply(false, undefined, String((err as Error).message || err))
    }
  },

  "group.leave": async ({ d, user, p, conn, reply }) => {
    if (!d.groups) return noGroups(reply)
    try {
      const gid = String(p.id ?? p.groupId ?? "")
      const r = await d.groups.leave(user.id, gid)
      // 离开的是当前会话：清空连接级当前会话（前端随后回草稿页/会话列表）
      if (conn.getCurrent() === r.sessionId) conn.setCurrent(undefined)
      // 退出不影响会话本体（他人还在用），只撤自己的访问权；会话列表推送由前端刷新拿
      return reply(true)
    } catch (err) {
      return reply(false, undefined, String((err as Error).message || err))
    }
  },

  "group.dissolve": async ({ d, user, conn, p, reply }) => {
    if (!d.groups) return noGroups(reply)
    try {
      const gid = String(p.id ?? p.groupId ?? "")
      const before = d.groups.view(gid, user.id)
      const { sessionId } = await d.groups.dissolve(user.id, gid)
      // 会话本体随之消失：引擎/排队/存储状态一并释放（群目录已被群存储整体删除）
      d.store.unregisterGroupSession(sessionId)
      d.engine.forgetSession(sessionId)
      d.inputQueue.forget(sessionId)
      if (conn.getCurrent() === sessionId) conn.setCurrent(undefined)
      return reply(true, { sessionId, name: before?.name ?? "" })
    } catch (err) {
      return reply(false, undefined, String((err as Error).message || err))
    }
  },
}
