/** 定时任务域路由（用户级资源，DESIGN「定时任务」）：REST 管理面（第三方集成/脚本管理），
 *  与 cron_* 工具同源同权（写操作不经审批——REST 已有身份认证边界，与 sessions/env 等既有资源管理端点姿态一致）。 */
import type { RouteCtx } from "./context"
import type { Context } from "hono"
import type { AppEnv } from "../app"
import { isValidSessionId } from "../core/base/paths"

export function registerCronRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  // 任务 id 格式白名单（32 位 hex，与生成规则一致）：畸形/穿越形态 400。
  const validateCronId = async (c: Context<AppEnv>, next: () => Promise<void>) => {
    const id = c.req.param("id") ?? ""
    if (!/^[a-f0-9]{32}$/.test(id)) return c.json({ error: `invalid cron task id: ${id}` }, 400)
    await next()
  }
  app.use("/api/v1/cron/:id", validateCronId)
  app.use("/api/v1/cron/:id/*", validateCronId)

  app.get("/api/v1/cron", async (c) => {
    if (!d.cron) return c.json({ error: "cron disabled (GEBAI_CRON_ENABLED=false)" }, 503)
    const user = await userOf(c)
    return c.json(await d.cron.list(user.id))
  })
  app.post("/api/v1/cron", async (c) => {
    if (!d.cron) return c.json({ error: "cron disabled (GEBAI_CRON_ENABLED=false)" }, 503)
    const user = await userOf(c)
    try {
      const body = await c.req.json()
      const task = await d.cron.add(user.id, body, typeof body?.originSessionId === "string" && isValidSessionId(body.originSessionId) ? body.originSessionId : undefined)
      return c.json(task, 201)
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })
  app.patch("/api/v1/cron/:id", async (c) => {
    if (!d.cron) return c.json({ error: "cron disabled (GEBAI_CRON_ENABLED=false)" }, 503)
    const user = await userOf(c)
    try {
      const body = await c.req.json()
      const task = await d.cron.update(user.id, c.req.param("id"), body)
      if (!task) return c.json({ error: "not found" }, 404)
      return c.json(task)
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })
  app.delete("/api/v1/cron/:id", async (c) => {
    if (!d.cron) return c.json({ error: "cron disabled (GEBAI_CRON_ENABLED=false)" }, 503)
    const user = await userOf(c)
    const removed = await d.cron.remove(user.id, c.req.param("id"))
    return removed ? c.json({ ok: true }) : c.json({ error: "not found" }, 404)
  })
  app.post("/api/v1/cron/:id/run", async (c) => {
    if (!d.cron) return c.json({ error: "cron disabled (GEBAI_CRON_ENABLED=false)" }, 503)
    const user = await userOf(c)
    const task = await d.cron.trigger(user.id, c.req.param("id"))
    if (!task) return c.json({ error: "not found" }, 404)
    return c.json(task)
  })
}
