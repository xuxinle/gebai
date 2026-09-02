/** 反馈 / Webhook / HTML 小工具三组小域路由（各自独立、无共享逻辑，合并一个文件避免碎文件）。 */
import type { RouteCtx } from "./context"
import type { FeedbackInfo, FeedbackInput } from "@gebai/sdk"
import { readFeedback, writeFeedback } from "../feedback"
import { SERVICE_USER } from "../app"
import { deleteMiniTool, getMiniTool, listMiniTools } from "../core/widgets/mini-tools"

export function registerFeedbackRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  app.post("/api/v1/feedback", async (c) => {
    const user = await userOf(c)
    const fb = await c.req.json<FeedbackInput>()
    const id = await writeFeedback(d.config.gebaiHome, user.id, fb)
    return c.json({ ok: true, id })
  })
  app.get("/api/v1/feedback", async (c) => {
    const user = await userOf(c)
    // 管理员可查询全部用户反馈；普通用户仅自己的（DESIGN REST 协议表：查询/导出）
    let list: FeedbackInfo[] = []
    if (user.role === "admin") {
      const users = await d.auth.listUsers()
      // auth=none 的默认用户（default）同样纳入扫描
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
    const q = c.req.query()
    if (q.messageId) list = list.filter((f) => f.messageId === q.messageId)
    if (q.sessionId) list = list.filter((f) => f.sessionId === q.sessionId)
    if (q.type) list = list.filter((f) => f.type === q.type)
    return c.json(list)
  })
}

export function registerWebhookRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  // Webhooks（DESIGN REST 协议表；签名校验与重试见 webhooks.ts）
  app.get("/api/v1/webhooks", async (c) => {
    const user = await userOf(c)
    return c.json(d.webhooks.list(user.role === "admin" ? undefined : user.id))
  })
  app.post("/api/v1/webhooks", async (c) => {
    const user = await userOf(c)
    const body = await c.req.json<{ url: string; events?: string[]; secret?: string }>()
    try {
      const cfg = await d.webhooks.register(body, user.role === "admin" ? undefined : user.id)
      return c.json(cfg, 201)
    } catch (e) {
      return c.json({ error: (e as Error).message }, 400)
    }
  })
  app.delete("/api/v1/webhooks/:id", async (c) => {
    const user = await userOf(c)
    const mine = d.webhooks.list(user.role === "admin" ? undefined : user.id)
    if (!mine.some((w) => w.id === c.req.param("id"))) return c.json({ error: "not found" }, 404)
    await d.webhooks.remove(c.req.param("id"))
    return c.json({ ok: true })
  })
}

export function registerMiniToolRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  // HTML 小工具库（Agent 经 save_tool 保存；列表/读取/删除供 UI 弹窗加载）
  app.get("/api/v1/mini-tools", async (c) => {
    const user = await userOf(c)
    const tools = await listMiniTools(d.config.gebaiHome, user.id)
    return c.json(tools)
  })
  app.get("/api/v1/mini-tools/:name", async (c) => {
    const user = await userOf(c)
    const tool = await getMiniTool(d.config.gebaiHome, user.id, c.req.param("name"))
    if (!tool) return c.json({ error: "tool not found" }, 404)
    return c.json(tool)
  })
  app.delete("/api/v1/mini-tools/:name", async (c) => {
    const user = await userOf(c)
    const scope = c.req.query("scope") === "public" ? "public" : "private"
    // 多用户模式公共工具仅管理员可删（与 save_tool/delete_tool 同规则，防共享资源投毒/破坏）
    const removed = await deleteMiniTool(d.config.gebaiHome, user.id, c.req.param("name"), scope, { mode: d.config.auth, role: user.role })
    if (!removed) return c.json({ error: "tool not found" }, 404)
    return c.json({ ok: true })
  })
}
