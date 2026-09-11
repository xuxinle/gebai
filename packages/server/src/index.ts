/** 服务端入口（薄）：startServer = compose（boot/compose.ts 装配）+ serve（boot/serve.ts 监听）。
 *  进程主命令分发（exec / 默认启动）见 boot/cli.ts。各域实现分布：REST routes/、WS 分发 ws-handlers/、
 *  领域核心 core/、扩展定义 @gebai/agents 包（子代理定义域 src/agents/）。 */
import type { SessionStore } from "./core/session/store"
import type { ToolRegistry } from "./core/base/registry"
import type { AgentEngine } from "./core/engine/engine"
import type { EventBus } from "./core/base/event-bus"
import type { AuthService } from "./auth"
import type { SubAgentManager } from "./core/agents/subagents"
import type { CronManager } from "./core/schedule/cron"
import type { UserTodoManager } from "./core/schedule/todos"
import type { DevReloadManager } from "./dev-reload"
import type { FeishuBot } from "./feishu-bot/bot"
import type { loadConfig } from "./core/base/config"
import type { AppDeps, createApp } from "./app"
import { composeServer } from "./boot/compose"
import { serveComposed } from "./boot/serve"
import { runMain } from "./boot/cli"
import { consumeRestartContinuation } from "./core/tools/restart"

export interface ServerHandle {
  server: ReturnType<typeof Bun.serve>
  app: ReturnType<typeof createApp>
  engine: AgentEngine
  store: SessionStore
  registry: ToolRegistry
  subAgents: SubAgentManager
  auth: AuthService
  events: EventBus
  config: ReturnType<typeof loadConfig>
  deps: AppDeps
  /** 数据生命周期 GC 清理任务句柄（GEBAI_GC_DISABLED=1 时为 null）。 */
  gc: { stop: () => void } | null
  /** 定时任务调度器（GEBAI_CRON_ENABLED 默认 true；显式 false 时为 null）。 */
  cron: CronManager | null
  /** 用户级待办管理器（GEBAI_IDLE_TODO_ENABLED 默认 true；显式 false 时为 null）。 */
  todos: UserTodoManager | null
  /** 开发模式热刷新管理器（--reload / GEBAI_DEV_RELOAD=1 时启用，否则 null）。 */
  devReload: DevReloadManager | null
  /** 飞书机器人对话桥接（GEBAI_FEISHU_BOT_ENABLED=true 时启用，否则 null）。 */
  feishuBot: FeishuBot | null
}

export async function startServer(overrides: Partial<Parameters<typeof loadConfig>[0]> = {}): Promise<ServerHandle> {
  const c = await composeServer(overrides)
  const server = serveComposed(c)
  // 重启续跑（restart_server 的 prompt 参数）：本次启动若承接自重启拉起器，把旧进程留下的提示词注入
  // 原会话继续执行（后台任务，不阻塞监听；结论写 {tmpdir}/gebai-restart/continue.result.json）。
  // 仅本地模式（restart_server 本身也只在此形态暴露）；测试进程跳过——本机 tmp 可能残留真实续跑请求，
  // 测试触发引擎运行会造成意外副作用。
  if (c.config.auth === "local" && process.env.NODE_ENV !== "test") {
    void consumeRestartContinuation({
      pid: process.pid,
      sessionExists: async (sessionId, user) => !!(await c.store.load(sessionId, user)),
      run: async (req) => {
        // engine.run 落盘 user 消息（原样提示词）并跑起完整 agent 循环：模型按重启后续跑指令继续工作
        await c.engine.run(req.sessionId, req.user, req.prompt, { role: req.role })
      },
    }).catch((err) => console.error(`[restart] 续跑消费异常: ${String((err as Error).message || err)}`))
  }
  return { server, app: c.app, engine: c.engine, store: c.store, registry: c.registry, subAgents: c.subAgents, auth: c.auth, events: c.events, config: c.config, deps: c.deps, gc: c.gc, cron: c.cron, todos: c.todos, devReload: c.devReload, feishuBot: c.feishuBot }
}

if (import.meta.main) {
  await runMain(startServer)
}
