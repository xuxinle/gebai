/** 服务端入口（薄）：startServer = compose（boot/compose.ts 装配）+ serve（boot/serve.ts 监听）。
 *  进程主命令分发（exec / 默认启动）见 boot/cli.ts。各域实现分布：REST routes/、WS 分发 ws-handlers/、
 *  领域核心 core/、扩展定义 sub-agents/。 */
import type { SessionStore } from "./core/session/store"
import type { ToolRegistry } from "./core/base/registry"
import type { AgentEngine } from "./core/engine/engine"
import type { EventBus } from "./core/base/event-bus"
import type { AuthService } from "./auth"
import type { SubAgentManager } from "./core/agents/subagents"
import type { CronManager } from "./core/schedule/cron"
import type { DevReloadManager } from "./dev-reload"
import type { FeishuBot } from "./feishu-bot/bot"
import type { loadConfig } from "./core/base/config"
import type { AppDeps, createApp } from "./app"
import { composeServer } from "./boot/compose"
import { serveComposed } from "./boot/serve"
import { runMain } from "./boot/cli"

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
  /** 开发模式热刷新管理器（--reload / GEBAI_DEV_RELOAD=1 时启用，否则 null）。 */
  devReload: DevReloadManager | null
  /** 飞书机器人对话桥接（GEBAI_FEISHU_BOT_ENABLED=true 时启用，否则 null）。 */
  feishuBot: FeishuBot | null
}

export async function startServer(overrides: Partial<Parameters<typeof loadConfig>[0]> = {}): Promise<ServerHandle> {
  const c = await composeServer(overrides)
  const server = serveComposed(c)
  return { server, app: c.app, engine: c.engine, store: c.store, registry: c.registry, subAgents: c.subAgents, auth: c.auth, events: c.events, config: c.config, deps: c.deps, gc: c.gc, cron: c.cron, devReload: c.devReload, feishuBot: c.feishuBot }
}

if (import.meta.main) {
  await runMain(startServer)
}
