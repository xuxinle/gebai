/**
 * 用户级偏好路由（DESIGN「用户级偏好」）：`GET/PATCH /api/v1/prefs`。
 *
 * 为什么放在服务端（而不是继续塞浏览器 localStorage）：偏好里有些项**直接影响服务端行为**
 * （如默认闲时执行时间段——调度器要知道"这个用户的闲时任务该在几点跑"），
 * 浏览器存一份服务端读不到；而且用户换设备（手机 / 桌面）要求看到同样的设置。
 * 纯观感偏好（主题、浮层位置等）不走这里——那些没有跨设备一致的需求。
 */
import type { RouteCtx } from "./context"

export function registerPrefsRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  app.get("/api/v1/prefs", async (c) => {
    if (!d.prefs) return c.json({ error: "prefs unavailable" }, 503)
    const user = await userOf(c)
    return c.json(await d.prefs.get(user.id))
  })

  app.patch("/api/v1/prefs", async (c) => {
    if (!d.prefs) return c.json({ error: "prefs unavailable" }, 503)
    const user = await userOf(c)
    try {
      const body = await c.req.json()
      if (!body || typeof body !== "object" || Array.isArray(body)) return c.json({ error: "body 须为对象" }, 400)
      return c.json(await d.prefs.update(user.id, body))
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })
}
