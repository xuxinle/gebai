/** 用户级待办域路由（用户级资源，DESIGN「用户级待办与闲时任务」）：REST 管理面（前端待办弹窗
 *  与其他集成共用）。与 cron 域同风格：写操作不经审批（REST 已有身份认证边界，与既有资源管理端点姿态一致）。
 *  注意清单级批量重排（拖动排序）走 `PATCH /api/v1/todos`（body { ids }）——避免与 :id 通配中间件冲突。 */
import type { RouteCtx } from "./context"
import type { Context } from "hono"
import type { AppEnv } from "../app"

export function registerTodoRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  // 待办 id 格式白名单（32 位 hex，与生成规则一致）：畸形/穿越形态 400。
  const validateTodoId = async (c: Context<AppEnv>, next: () => Promise<void>) => {
    const id = c.req.param("id") ?? ""
    if (!/^[a-f0-9]{32}$/.test(id)) return c.json({ error: `invalid todo id: ${id}` }, 400)
    await next()
  }
  app.use("/api/v1/todos/:id", validateTodoId)

  // 能力开关关闭（GEBAI_IDLE_TODO_ENABLED=false）：统一 503（与 cron 域一致）
  const disabled = (c: Context<AppEnv>) => c.json({ error: "todos disabled (GEBAI_IDLE_TODO_ENABLED=false)" }, 503)

  app.get("/api/v1/todos", async (c) => {
    if (!d.todos) return disabled(c)
    const user = await userOf(c)
    return c.json(await d.todos.list(user.id))
  })
  app.post("/api/v1/todos", async (c) => {
    if (!d.todos) return disabled(c)
    const user = await userOf(c)
    try {
      const body = await c.req.json()
      return c.json(await d.todos.add(user.id, { text: body?.text, idle: body?.idle === true }), 201)
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })
  // 清单级批量操作：{ ids: [...] } 按给定顺序重排（拖动排序落库）
  app.patch("/api/v1/todos", async (c) => {
    if (!d.todos) return disabled(c)
    const user = await userOf(c)
    try {
      const body = await c.req.json()
      return c.json(await d.todos.reorder(user.id, body?.ids))
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })
  app.patch("/api/v1/todos/:id", async (c) => {
    if (!d.todos) return disabled(c)
    const user = await userOf(c)
    try {
      const body = await c.req.json()
      const todo = await d.todos.update(user.id, c.req.param("id"), body ?? {})
      if (!todo) return c.json({ error: "not found" }, 404)
      return c.json(todo)
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 400)
    }
  })
  app.delete("/api/v1/todos/:id", async (c) => {
    if (!d.todos) return disabled(c)
    const user = await userOf(c)
    const removed = await d.todos.remove(user.id, c.req.param("id"))
    return removed ? c.json({ ok: true }) : c.json({ error: "not found" }, 404)
  })
}
