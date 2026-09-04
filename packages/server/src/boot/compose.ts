/** 组合根（DI 装配）：loadConfig → 各组件构建（store/registry/sandbox/auth/subAgents/engine/
 *  webhooks/cron/gc/feishuBot/externalAuth/state/app/devReload）→ Composed。网络监听见 serve.ts，
 *  进程入口见 cli.ts / index.ts。自原单文件 index.ts 的 startServer 装配段拆分，行为不变。 */
import { existsSync } from "node:fs"
import { rename } from "node:fs/promises"
import { join } from "node:path"
import type { ServerWebSocket } from "bun"
import { loadConfig } from "../core/base/config"
import { SessionStore } from "../core/session/store"
import { ToolRegistry } from "../core/base/registry"
import { createGlobalTools, isGlobalToolExcluded } from "../core/tools"
import { Sandbox } from "../core/security/sandbox"
import { EnvManager, applyEmbeddedEnvDefaults, cleanupLegacyUserEnv } from "../core/session/env"
import { EMBEDDED_ENV_DEFAULTS } from "../core/session/env-embedded.generated"
import { EventBus } from "../core/base/event-bus"
import { AuthService } from "../auth"
import { SubAgentManager } from "../core/agents/subagents"
import { RESERVED_PROJECT_TMP } from "../core/tools/projects"
import { AgentEngine } from "../core/engine/engine"
import { WebhookManager } from "../webhooks"
import { createExternalAuthProvider } from "../external-auth"
import { applyModelEnvOverrides, createProvider, parseExtraParams, resolveModelRouteProvider, resolveVisionProvider, type ApiKind, type ProviderConfig } from "../core/llm/llm"
import { makeVisionTool, setVisionProviderGetter, getVisionProvider } from "../core/tools/vision"
import { scheduleGC } from "../core/session/gc"
import { CronManager } from "../core/schedule/cron"
import { isFeishuChatId, validateNotifyChannel } from "../core/schedule/notify"
import { createApp, type AppDeps } from "../app"
import { DevReloadManager, webRootOf } from "../dev-reload"
import { WsStateService } from "../ws-state"
import { FeishuBot } from "../feishu-bot/bot"
import { EngineBotAdapter } from "../feishu-bot/adapter"
import { createFeishuApi } from "../feishu-bot/api"
import { makeWsSink } from "./serve"
import { installBrowserFetchProxy } from "../core/browser/fetch-proxy"

/** 组合产物：全部组件实例 + 监听所需的依赖包（serve.ts 消费）。 */
export interface Composed {
  config: ReturnType<typeof loadConfig>
  store: SessionStore
  registry: ToolRegistry
  engine: AgentEngine
  subAgents: SubAgentManager
  auth: AuthService
  events: EventBus
  sandbox: Sandbox
  webhooks: WebhookManager
  cron: CronManager | null
  gc: { stop: () => void } | null
  feishuBot: FeishuBot | null
  deps: AppDeps
  /** WS 状态服务（每用户事件日志/连接状态/快照）。 */
  state: WsStateService
  app: ReturnType<typeof createApp>
  devReload: DevReloadManager | null
  /** 热刷新通道连接集合（serve.ts 的 websocket.open 注册，DevReloadManager 广播消费）。 */
  devReloadClients: Set<ServerWebSocket<unknown>>
}

export async function composeServer(overrides: Partial<Parameters<typeof loadConfig>[0]> = {}): Promise<Composed> {
  // 构建期内置模型配置默认值（发行裁剪构建烘焙的 GEBAI_LLM_*/GEBAI_VISION_*）：仅填充未设置的键，
  // 运行时环境变量与前端/任务级 env 优先；须在读 GEBAI_LLM_* 前应用
  applyEmbeddedEnvDefaults(EMBEDDED_ENV_DEFAULTS)

  const config = loadConfig(overrides)

  // 透明浏览器代理（GEBAI_BROWSER_PROXY=1，重启生效）：服务启动即安装 fetch 垫片——平台级
  // 能力，不依赖任何子Agent 的装载/裁剪（桥接基建在 core/browser，见 DESIGN「透明浏览器代理」）
  installBrowserFetchProxy()

  const store = new SessionStore({ home: config.gebaiHome })

  const apiBase = process.env.GEBAI_LLM_API_BASE || process.env.OPENAI_BASE_URL || ""
  const apiKey = process.env.GEBAI_LLM_API_KEY || process.env.OPENAI_API_KEY || ""
  const model = process.env.GEBAI_LLM_MODEL || process.env.OPENAI_MODEL || "deepseek-chat"
  const apiKind = (process.env.GEBAI_LLM_API_KIND as ApiKind) || "openai"
  // 额外模型接口参数（如推理强度）：JSON 字符串，非法时启动即报错提示
  const extraParams = parseExtraParams(process.env.GEBAI_LLM_EXTRA_PARAMS)
  // 启动主模型配置（Provider 实例随任务级 env 覆盖重建，见 resolveProvider；本配置为缺省基准）
  const mainConfig: ProviderConfig = {
    apiKind,
    apiBase,
    apiKey,
    model,
    maxContextTokens: Number(process.env.GEBAI_LLM_MAX_CONTEXT || 128000),
    // 单次响应输出 token 上限（大文件生成截断防护）：可选，未配置时 openai/responses 用服务端缺省、anthropic 用 8192
    ...(process.env.GEBAI_LLM_MAX_OUTPUT_TOKENS ? { maxOutputTokens: Number(process.env.GEBAI_LLM_MAX_OUTPUT_TOKENS) } : {}),
    // 主模型多模态能力须显式声明（GEBAI_LLM_MULTIMODAL=true）：默认 false，
    // 避免纯文本模型谎报能力导致图片内联被接口拒绝；视觉能力请配置 GEBAI_VISION_* 外挂模型
    multimodal: process.env.GEBAI_LLM_MULTIMODAL === "true",
    extraParams,
  }
  const provider = createProvider(mainConfig)

  // 额外多模态（视觉）模型：GEBAI_VISION_MODEL 配置后启用独立视觉 Provider（vision 工具使用），
  // 接口地址/密钥/类型缺省时继承主模型配置；未配置时 vision 工具回落到主模型（须声明多模态能力）
  const visionConfig: ProviderConfig | null = process.env.GEBAI_VISION_MODEL
    ? {
        apiKind: (process.env.GEBAI_VISION_API_KIND as ApiKind) || apiKind,
        apiBase: process.env.GEBAI_VISION_API_BASE || apiBase,
        apiKey: process.env.GEBAI_VISION_API_KEY || apiKey,
        model: process.env.GEBAI_VISION_MODEL,
        maxContextTokens: Number(process.env.GEBAI_VISION_MAX_CONTEXT || 128000),
        multimodal: true,
        extraParams: parseExtraParams(process.env.GEBAI_VISION_EXTRA_PARAMS),
      }
    : null

  // 安全模式：子Agent 工具按 Tool.safeMode 自主声明过滤（全局风险工具内置降级，不过滤）
  const registry = new ToolRegistry({ safeMode: config.safeMode })
  for (const tool of Object.values(createGlobalTools())) registry.register(tool)
  // 视觉 provider 提供者注册（子Agent 定义如 self_optimize 的 vision 工具经 getVisionProvider 复用同一解析逻辑）；
  // 任务级 env 覆盖生效：会话/前端配置 GEBAI_VISION_*（或 GEBAI_LLM_MULTIMODAL）时按任务重建视觉 Provider
  setVisionProviderGetter((env) => resolveVisionProvider(mainConfig, visionConfig, env))
  // 构建期排除清单（GEBAI_BUILD_EXCLUDE_TOOLS）同规则生效（与全局工具表一致：不注册不暴露）
  if (!isGlobalToolExcluded("vision")) registry.register(makeVisionTool({ vision: getVisionProvider }))
  registry.enableSet(config.toolEnable, config.toolDisable)

  // 沙箱 auto 判定（DESIGN「GEBAI_SANDBOX」）：只看运行形态，不判定监听 IP——
  // 服务模式（多用户公用、远程可达）强制启用沙箱（防普通用户越权读写/操控宿主）；
  // 本地模式（操作者本人）默认不限制。显式 GEBAI_SANDBOX=on/off 仍可覆盖。
  // 防呆：服务模式 + 显式关沙箱 = 多租户下任意用户可越界读写宿主任意路径（files 接口回退 resolve），
  // 启动直接拒绝（安全配置错误在启动期暴露，而非运行期出事后追溯）。
  if (config.sandbox === "off" && config.auth === "server") {
    throw new Error("GEBAI_SANDBOX=off 与服务模式（GEBAI_MODE=server）互斥：多用户隔离要求路径沙箱，请使用 auto/on")
  }
  const sandbox = new Sandbox({
    home: config.gebaiHome,
    enabled: config.sandbox === "on" || (config.sandbox === "auto" && config.auth === "server"),
    // 豁免语义（与 DESIGN 一致）：仅本地模式默认用户（id=admin）豁免路径沙箱——
    // 本地模式是操作者本人机器，不受限；服务模式一律沙箱（admin 也不豁免，多租户边界一致）
    isExempt: (u) => u === "admin" && config.auth === "local",
  })

  const auth = new AuthService(config.gebaiHome, config.auth)
  // 服务模式 admin 引导：GEBAI_ADMIN_PASSWORD_HASH 设置则启用 admin（覆盖哈希），未设置则禁用 admin
  if (config.auth === "server") {
    await auth.applyAdminHash(config.adminPasswordHash)
  }
  // 本地模式用户目录平滑迁移：旧版默认用户目录 users/default/ → users/admin/（仅当 admin 目录不存在）
  if (config.auth === "local") {
    const defaultDir = join(config.gebaiHome, "users", "default")
    const adminDir = join(config.gebaiHome, "users", "admin")
    if (existsSync(defaultDir) && !existsSync(adminDir)) {
      try {
        await rename(defaultDir, adminDir)
        console.log("[gebai] 本地模式用户目录迁移：users/default → users/admin")
      } catch {
        /* 迁移失败不影响启动（后续写入按 admin 目录） */
      }
    }
  }
  // 用户环境变量零留存（只存浏览器本地）：启动时清理历史用户级 env.json（旧版三层结构遗留）
  await cleanupLegacyUserEnv(config.gebaiHome)
  const env = new EnvManager(store)
  const events = new EventBus()
  const subAgents = new SubAgentManager({ registry, preloadOverride: config.preloadSubAgents })
  await subAgents.discover()
  // 定时任务能力开关（GEBAI_CRON_ENABLED，默认 true）：关闭时 cron 子Agent 不注册（agent_list/agent_load/
  // agent_run 均不可见，cron_* 工具不进工具表/schema，与调度器一致完全隐藏）；开启时按需装载、REST /api/v1/cron 可管
  if (!config.cronEnabled) subAgents.unregister("cron")
  // 子Agent 启停名单（GEBAI_SUB_AGENTS_ENABLE 白名单 / GEBAI_SUB_AGENTS_DISABLE 黑名单）：enable 非空仅保留
  // 名单内，disable 移除名单内（先白后黑）；unregister 后 agent_list/装载/新会话执行/系统提示词注入均不可见
  // 且热加载不复活；未知名告警不阻断启动
  subAgents.applyEnableDisable(config.subAgentsEnable, config.subAgentsDisable)
  // 预置项目保留名防呆（DESIGN「项目机制」）：tmp 为会话工作区保留名——{AGENT}_PROJECTS 配了叫 tmp 的
  // 项目在启动期拒绝（静默遮蔽保留名会在设定项目根后无法访问会话文件，难排查）；前端注入的任务级 env
  // 由引擎 presetProjectsFor 同规则兜底。仅校验进程环境变量中已声明的清单（子Agent envVars 声明面）。
  for (const d of subAgents.allDefs()) {
    for (const v of d.envVars ?? []) {
      if (!/_PROJECTS$/.test(v.name)) continue
      const raw = process.env[v.name]
      if (!raw) continue
      try {
        const list = JSON.parse(raw)
        if (Array.isArray(list) && list.some((p) => p && typeof p === "object" && String((p as Record<string, unknown>).name ?? "").trim() === RESERVED_PROJECT_TMP)) {
          throw new Error(`${v.name} 中的预置项目名 "${RESERVED_PROJECT_TMP}" 为保留名（会话工作区），请改名后重启`)
        }
      } catch (err) {
        if (err instanceof SyntaxError) continue // 非法 JSON 由 presetProjectsFor 静默忽略（既有语义），此处只管保留名
        throw err
      }
    }
  }

  const engine = new AgentEngine({
    provider,
    registry,
    store,
    env,
    sandbox,
    events,
    config,
    subAgents,
    authMode: config.auth,
    // 任务级主模型解析：会话/前端 env 配置 GEBAI_LLM_* 时按任务重建 Provider（模型/地址/密钥/类型/上下文预算/多模态声明覆盖启动配置），
    // 无覆盖返回 undefined（引擎沿用 opts.provider 实例）
    resolveProvider: (env) => {
      const cfg = applyModelEnvOverrides(mainConfig, env)
      // 语义比较而非引用比较：applyModelEnvOverrides 有覆盖时返回新对象；但 .env 配置的
      // GEBAI_LLM_* 同时进入 mainConfig 与任务 env，覆盖结果与启动配置逐字段相同——
      // 引用比较会把「无实际覆盖」误判为重建（每次任务新建 Provider + 测试注入的 fake 被绕过）
      return JSON.stringify(cfg) === JSON.stringify(mainConfig) ? undefined : createProvider(cfg)
    },
    // 分支运行模型路由（DESIGN「会话分支运行与合并」多路接口）：GEBAI_LLM_ROUTES 路由名命中走
    // 独立端点/模型，字面模型名按主配置基准覆盖——多分支多路并行分摊单路限流
    resolveModelProvider: (env, name) => resolveModelRouteProvider(mainConfig, env, name),
  })
  // 事件 Webhook（REST /api/v1/webhooks 注册面）先行构建：定时任务通知的 webhookId 引用解析依赖它
  const webhooks = new WebhookManager({ home: config.gebaiHome })
  webhooks.ownerOf = async (sessionId: string) => store.ownerOf(sessionId)
  await webhooks.start(events)
  // 定时任务（GEBAI_CRON_ENABLED，默认开启）：通知通道含飞书应用消息（feishu_chat）——复用全局飞书
  // 应用凭证（GEBAI_FEISHU_APP_ID/SECRET，与机器人桥接/云文档共用）构建发送器；agents 预载名单合法性
  // 由 subAgents.def 探测；webhookId 引用解析——具名注册（userId 记录）仅本人任务可引用，全局注册
  // （admin，userId 未记录=部署方集成通道）人人可引用；关闭时调度器不启动（工具注册见上）
  let cron: CronManager | null = null
  if (config.cronEnabled) {
    const feishuApi = config.feishuAppId && config.feishuAppSecret ? createFeishuApi({ appId: config.feishuAppId, appSecret: config.feishuAppSecret }) : undefined
    // 全局默认通知通道（GEBAI_CRON_NOTIFY_WEBHOOK / GEBAI_CRON_NOTIFY_FEISHU）：任务未配 notify 时兜底推送。
    // 构建期逐条校验（SSRF/域名/chat_id 形态），非法配置告警跳过不阻断启动；chat_id 形态需飞书应用凭证
    const defaultNotify: import("../core/schedule/notify").CronNotifyChannel[] = []
    for (const [label, type, raw] of [
      ["GEBAI_CRON_NOTIFY_WEBHOOK", "webhook", config.cronNotifyWebhook],
      ["GEBAI_CRON_NOTIFY_FEISHU", "feishu", config.cronNotifyFeishu],
    ] as const) {
      if (!raw) continue
      const ch = { type, target: raw } as import("../core/schedule/notify").CronNotifyChannel
      try {
        validateNotifyChannel(ch)
        if (type === "feishu" && isFeishuChatId(raw) && !feishuApi) throw new Error("chat_id 形态需配置 GEBAI_FEISHU_APP_ID/GEBAI_FEISHU_APP_SECRET")
        defaultNotify.push(ch)
      } catch (err) {
        console.warn(`[gebai] 全局定时通知 ${label} 配置无效，已忽略: ${(err as Error).message}`)
      }
    }
    cron = new CronManager({
      home: config.gebaiHome,
      store,
      env,
      sandbox,
      events,
      safeMode: config.safeMode,
      notify: feishuApi
        ? {
            // 飞书应用消息：2.0 markdown 卡片（与对话桥接同款新版本接口）；at 含 "all"（@所有人）时降级
            // text 消息（@所有人 提及通知以 text 正文标签为可靠路径——与群机器人 webhook 同规则）
            feishuSend: async (chatId, msgType, content) => {
              await feishuApi.sendMessage({ receiveId: chatId, receiveIdType: "chat_id", msgType, content })
            },
          }
        : undefined,
      agentExists: (name) => subAgents.def(name) !== undefined,
      resolveWebhook: (id, user) => {
        const cfg = webhooks.of(id)
        if (!cfg) return null
        if (cfg.userId && cfg.userId !== user) return null
        return { url: cfg.url, secret: cfg.secret }
      },
      defaultNotify,
    })
    cron.attach(engine)
    await cron.start()
  }
  // 数据生命周期 GC：启动即跑一次，之后每日周期执行（GEBAI_GC_DISABLED=1 关闭）
  const gc = config.gcDisabled
    ? null
    : scheduleGC(config.gebaiHome, {
        // 归档后失效会话缓存，防止缓存命中后 save() 在 sessions/ 重建已归档目录
        onArchive: (sessionId) => store.evict(sessionId),
      })
  // 飞书机器人对话桥接（GEBAI_FEISHU_BOT_ENABLED=true）：长连接订阅消息事件，回推 Agent 回复
  let feishuBot: FeishuBot | null = null
  if (config.feishuBotEnabled) {
    if (!config.feishuAppId || !config.feishuAppSecret) {
      throw new Error("GEBAI_FEISHU_BOT_ENABLED=true 需要同时配置 GEBAI_FEISHU_APP_ID 与 GEBAI_FEISHU_APP_SECRET")
    }
    feishuBot = new FeishuBot({
      appId: config.feishuAppId,
      appSecret: config.feishuAppSecret,
      authMode: config.auth,
      home: config.gebaiHome,
      store,
      // 接口层：多轮交互 + 仅最终回复桥接（bot 不直接接触引擎/事件总线）
      adapter: new EngineBotAdapter(engine, events),
      auth,
    })
    await feishuBot.start()
  }
  // 外部身份验证器（GEBAI_EXTERNAL_AUTH_SECRET / GEBAI_EXTERNAL_AUTH_URL 配置；两者同设会抛错）
  const externalAuth = createExternalAuthProvider(config)
  // WS 状态服务（MVC 模型层）：每用户事件日志（断线可重放）+ 连接状态持久化 + 快照；
  // state 自持 deps 前身（不递归引用自身）
  const baseDeps: AppDeps = { config, store, env, sandbox, registry, engine, auth, events, subAgents, webhooks, externalAuth, cron }
  const state = new WsStateService(config.gebaiHome, baseDeps)
  const deps: AppDeps = { ...baseDeps, state }
  const app = createApp(deps)

  // 开发模式热刷新（bun run dev --reload / GEBAI_DEV_RELOAD=1）：
  // Web 源码变更 → vite build --watch 自动重建 → 广播 reload → 页面自动刷新
  const devReloadClients = new Set<ServerWebSocket<unknown>>()
  let devReload: DevReloadManager | null = null
  if (config.devReload) {
    devReload = new DevReloadManager(webRootOf(config.gebaiHome), () => {
      for (const ws of devReloadClients) {
        makeWsSink(ws).send(JSON.stringify({ type: "reload" }))
      }
    })
    devReload.start()
  }

  return { config, store, registry, engine, subAgents, auth, events, sandbox, webhooks, cron, gc, feishuBot, deps, state, app, devReload, devReloadClients }
}
