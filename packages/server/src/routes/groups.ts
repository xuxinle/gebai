/** 多人会话（群）路由：/groups 列表/创建/加入 + 单群资料、群主操作（改名/口令/解散）。
 *  与 WS `group.*` 同源（同一 GroupStore）——REST 供非浏览器接入方（脚本/机器人/CLI）使用。
 *  群的**会话本体**仍走会话路由（/sessions/:id，群会话与普通会话同协议，访问权由成员资格裁定）。 */
import type { RouteCtx } from "./context"
import { toSessionInfo } from "../core/session/store"

export function registerGroupRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf
  const groups = () => d.groups ?? null

  app.get("/api/v1/groups", async (c) => {
    const user = await userOf(c)
    const store = groups()
    if (!store) return c.json({ error: "多人会话能力未启用" }, 503)
    return c.json(store.listViews(user.id))
  })

  // 创建群（可选进入口令）：创建者即群主，按提交的群内名称/角色入群
  app.post("/api/v1/groups", async (c) => {
    const user = await userOf(c)
    const store = groups()
    if (!store) return c.json({ error: "多人会话能力未启用" }, 503)
    const body = (await c.req.json().catch(() => ({}))) as { name?: string; passcode?: string; memberName?: string; memberRole?: string }
    try {
      const g = await store.create(user.id, user.username ?? user.id, body)
      return c.json(store.view(g.id, user.id), 201)
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })

  // 加入群（邀请码 + 可选口令 + 群内名称/角色）：已是成员时按新身份改写
  app.post("/api/v1/groups/join", async (c) => {
    const user = await userOf(c)
    const store = groups()
    if (!store) return c.json({ error: "多人会话能力未启用" }, 503)
    const body = (await c.req.json().catch(() => ({}))) as { joinCode?: string; passcode?: string; memberName?: string; memberRole?: string }
    try {
      const g = await store.join(user.id, user.username ?? user.id, body)
      return c.json(store.view(g.id, user.id))
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })

  app.get("/api/v1/groups/:id", async (c) => {
    const user = await userOf(c)
    const store = groups()
    if (!store) return c.json({ error: "多人会话能力未启用" }, 503)
    const v = store.view(c.req.param("id"), user.id)
    // 非成员与不存在同应答（不泄露群存在性）
    if (!v || !v.me) return c.json({ error: "not found" }, 404)
    return c.json(v)
  })

  // 群内身份（名称/角色）自改：只影响本群，不动账号
  app.patch("/api/v1/groups/:id/me", async (c) => {
    const user = await userOf(c)
    const store = groups()
    if (!store) return c.json({ error: "多人会话能力未启用" }, 503)
    const body = (await c.req.json().catch(() => ({}))) as { name?: string; role?: string; memberName?: string; memberRole?: string }
    try {
      const g = await store.setProfile(user.id, c.req.param("id"), body.memberName ?? body.name, body.memberRole ?? body.role)
      return c.json(store.view(g.id, user.id))
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })

  // 群主：改群名（会话标题同步）
  app.patch("/api/v1/groups/:id", async (c) => {
    const user = await userOf(c)
    const store = groups()
    if (!store) return c.json({ error: "多人会话能力未启用" }, 503)
    const body = (await c.req.json().catch(() => ({}))) as { name?: string; passcode?: string | null }
    try {
      const gid = c.req.param("id")
      if (body.name !== undefined) await store.rename(user.id, gid, body.name)
      if (body.passcode !== undefined) await store.setPasscode(user.id, gid, body.passcode)
      return c.json(store.view(gid, user.id))
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })

  // 退出群（群主需先转让或解散）
  app.post("/api/v1/groups/:id/leave", async (c) => {
    const user = await userOf(c)
    const store = groups()
    if (!store) return c.json({ error: "多人会话能力未启用" }, 503)
    try {
      await store.leave(user.id, c.req.param("id"))
      return c.json({ ok: true })
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })

  // 群主转让
  app.post("/api/v1/groups/:id/transfer", async (c) => {
    const user = await userOf(c)
    const store = groups()
    if (!store) return c.json({ error: "多人会话能力未启用" }, 503)
    const body = (await c.req.json().catch(() => ({}))) as { userId?: string }
    try {
      const g = await store.transferOwner(user.id, c.req.param("id"), String(body.userId ?? ""))
      return c.json(store.view(g.id, user.id))
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })

  // 解散群（群主）：群目录整体删除，会话本体随之消失
  app.delete("/api/v1/groups/:id", async (c) => {
    const user = await userOf(c)
    const store = groups()
    if (!store) return c.json({ error: "多人会话能力未启用" }, 503)
    try {
      const { sessionId } = await store.dissolve(user.id, c.req.param("id"))
      d.store.unregisterGroupSession(sessionId)
      d.engine.forgetSession(sessionId)
      d.inputQueue.forget(sessionId)
      return c.json({ ok: true, sessionId })
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })

  // 群会话的会话信息（与 /sessions/:id 同源，只是先校验成员资格后返回）
  app.get("/api/v1/groups/:id/session", async (c) => {
    const user = await userOf(c)
    const store = groups()
    if (!store) return c.json({ error: "多人会话能力未启用" }, 503)
    const v = store.view(c.req.param("id"), user.id)
    if (!v || !v.me) return c.json({ error: "not found" }, 404)
    const session = await d.store.load(v.sessionId, user.id)
    if (!session) return c.json({ error: "not found" }, 404)
    return c.json(toSessionInfo(session))
  })
}
