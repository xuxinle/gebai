/** 用户管理域路由（admin）：/users 列表/创建/更新/删除。WS 侧 user.* 同权限，REST 与 WS 双通道一致。 */
import type { RouteCtx } from "./context"

export function registerUserRoutes(rc: RouteCtx): void {
  const { app, d } = rc

  app.get("/api/v1/users", async (c) => {
    const denied = rc.requireAdmin(c)
    if (denied) return denied
    const users = await d.auth.listUsers()
    return c.json(users.map((u) => d.auth.strip(u)))
  })
  app.post("/api/v1/users", async (c) => {
    const denied = rc.requireAdmin(c)
    if (denied) return denied
    const { username, password, role } = await c.req.json<{ username: string; password: string; role?: "user" | "admin" }>()
    const user = await d.auth.createUser(username, password, role)
    return c.json(d.auth.strip(user), 201)
  })
  app.patch("/api/v1/users/:id", async (c) => {
    const denied = rc.requireAdmin(c)
    if (denied) return denied
    const patch = await c.req.json()
    const user = await d.auth.updateUser(c.req.param("id"), patch)
    return c.json(d.auth.strip(user))
  })
  app.delete("/api/v1/users/:id", async (c) => {
    const denied = rc.requireAdmin(c)
    if (denied) return denied
    await d.auth.deleteUser(c.req.param("id"))
    return c.json({ ok: true })
  })
}
