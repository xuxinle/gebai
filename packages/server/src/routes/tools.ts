/** 工具与子Agent 能力域路由：/tools 查询与启停、/sub-agents 列表。 */
import type { RouteCtx } from "./context"

export function registerToolRoutes(rc: RouteCtx): void {
  const { app, d } = rc

  app.get("/api/v1/tools", async (c) => {
    return c.json(
      d.registry.list(false).map((rt) => ({
        name: rt.name,
        description: rt.tool.description,
        enabled: rt.enabled,
        group: rt.agent || "global",
        approvalRequired: !!rt.tool.requiresApproval,
      })),
    )
  })
  app.patch("/api/v1/tools", async (c) => {
    // 工具启停为服务端全局状态（所有用户/会话共享）：多用户模式下仅管理员可操作
    const denied = rc.requireAdmin(c)
    if (denied) return denied
    const { name, enabled } = await c.req.json<{ name: string; enabled: boolean }>()
    d.registry.setEnabled(name, enabled)
    return c.json({ ok: true })
  })

  // Sub-agents：响应前惰性热加载检查（目录签名未变时零成本 stat；放置新 native 子代理目录后无需重启即可出现）
  app.get("/api/v1/sub-agents", async (c) => {
    await d.subAgents.refreshIfChanged()
    return c.json(d.subAgents.list())
  })
}
