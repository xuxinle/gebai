import { existsSync, statSync } from "node:fs"
import { rename } from "node:fs/promises"
import { join } from "node:path"
import { loadConfig, isBinaryMode } from "./core/config"
import { walkDir } from "./core/paths"
import { SessionStore } from "./core/store"
import { ToolRegistry } from "./core/registry"
import { createGlobalTools } from "./core/tools"
import { Sandbox } from "./core/sandbox"
import { EnvManager } from "./core/env"
import { EventBus } from "./core/event-bus"
import { AuthService, type AuthUser } from "./auth"
import { SubAgentManager } from "./core/subagents"
import { AgentEngine } from "./core/engine"
import { WebhookManager } from "./webhooks"
import { createExternalAuthProvider } from "./external-auth"
import { applyModelEnvOverrides, createProvider, parseExtraParams, resolveVisionProvider, type ApiKind, type ProviderConfig } from "./core/llm"
import { makeVisionTool, setVisionProviderGetter, getVisionProvider } from "./core/vision"
import { scheduleGC } from "./core/gc"
import { CronManager } from "./core/cron"
import { makeCronTools } from "./core/tools"
import { createApp, SERVICE_USER, type AppDeps } from "./app"
import { DevReloadManager, webRootOf } from "./dev-reload"
import type { ServerWebSocket } from "bun"
import { handleWsMessage, type WsConn, type WsSink } from "./ws"
import { WsStateService } from "./ws-state"
import { FeishuBot } from "./feishu-bot/bot"
import { EngineBotAdapter } from "./feishu-bot/adapter"

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
  /** 定时任务调度器（GEBAI_CRON_ENABLED=true 时启用，否则 null）。 */
  cron: CronManager | null
  /** 开发模式热刷新管理器（--reload / GEBAI_DEV_RELOAD=1 时启用，否则 null）。 */
  devReload: DevReloadManager | null
  /** 飞书机器人对话桥接（GEBAI_FEISHU_BOT_ENABLED=true 时启用，否则 null）。 */
  feishuBot: FeishuBot | null
}

/** 每个 WS 连接的事件总线退订函数（以连接为键，避免与 ws.data 的 user 字段互相覆盖）。 */
const wsSubs = new WeakMap<object, () => void>()
/** 每个 WS 连接的连接上下文（open/message 共享同一实例，保证用户变更回调一致）。 */
const wsConns = new WeakMap<object, WsConn>()
/**
 * 每个 WS 连接的消息处理链（以连接为键）：Bun 对 async `websocket.message` 处理器
 * **不保证串行**（前一条 await 磁盘/网络 I/O 时下一条即并发进入）——认证（auth.login
 * 依赖磁盘读用户注册表）、会话切换等消息的顺序语义会被打乱，导致紧跟在认证后的
 * 请求以未登录态被拒绝（`unauthorized: login required`）。此处按到达顺序串行处理，
 * 对齐 SDK 假设的「服务端顺序处理消息」契约（认证消息先于其后请求生效）。
 */
const wsMsgChains = new WeakMap<object, Promise<void>>()

/** 单连接发送缓冲上限（字节）：Bun WebSocket 发送缓冲超限说明客户端消费速度远低于
 *  推送速率（慢客户端/高频流式），继续发送缓冲无界增长——断开让其走自动重连 + seq 重放收敛。 */
const WS_MAX_BUFFERED = 16 * 1024 * 1024

/** 构造带背压保护的发送 sink：超限断开连接（客户端自动重连后按事件 seq 重放补偿）。 */
function makeWsSink(ws: ServerWebSocket<unknown>): WsSink {
  return {
    send: (data: string) => {
      if (ws.readyState !== WebSocket.OPEN) return
      const buffered = (ws as unknown as { getBufferedAmount?: () => number }).getBufferedAmount?.() ?? 0
      if (buffered > WS_MAX_BUFFERED) {
        console.warn("[ws] 慢客户端发送缓冲超限（16MB），断开连接——客户端将自动重连并按 seq 重放")
        ws.close()
        return
      }
      ws.send(data)
    },
  }
}

/**
 * 构建 WS 连接上下文：连接级用户 + 连接级当前会话。
 * 当前会话为「连接覆盖值 ?? 每用户持久化状态」（WsStateService.connState）——
 * 重连/重启后自动恢复，连接级显式切换（session.switch）优先。
 */
function makeWsConn(ws: ServerWebSocket<unknown>, d: AppDeps, state: WsStateService | undefined): WsConn {
  const existing = wsConns.get(ws)
  if (existing) return existing
  const get = (): AuthUser =>
    d.config.auth === "local" ? d.auth.defaultUser() : ((ws.data as { user?: AuthUser } | undefined)?.user) || SERVICE_USER
  const userChangeCbs = new Set<() => void>()
  const conn: WsConn = {
    get,
    set: (u: AuthUser) => {
      ws.data = { ...((ws.data as object) || {}), user: u }
      for (const cb of userChangeCbs) cb()
    },
    onUserChange: (cb: () => void) => {
      userChangeCbs.add(cb)
      return () => {
        userChangeCbs.delete(cb)
      }
    },
    getCurrent: () => {
      const local = (ws.data as { currentSessionId?: string } | undefined)?.currentSessionId
      if (local !== undefined) return local
      return state?.connState.getCurrent(get().id)
    },
    setCurrent: (id: string | undefined) => {
      if (id === undefined) {
        const data = ws.data as { currentSessionId?: string } | undefined
        if (data && "currentSessionId" in data) {
          const { currentSessionId: _drop, ...rest } = data
          ws.data = rest
        }
      } else {
        ws.data = { ...((ws.data as object) || {}), currentSessionId: id }
      }
      state?.connState.setCurrent(get().id, id)
    },
  }
  wsConns.set(ws, conn)
  return conn
}

export async function startServer(overrides: Partial<Parameters<typeof loadConfig>[0]> = {}): Promise<ServerHandle> {
  const config = loadConfig(overrides)

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

  const registry = new ToolRegistry()
  for (const tool of Object.values(createGlobalTools())) registry.register(tool)
  // 视觉 provider 提供者注册（子Agent 定义如 self_optimize 的 vision 工具经 getVisionProvider 复用同一解析逻辑）；
  // 任务级 env 覆盖生效：会话/前端配置 GEBAI_VISION_*（或 GEBAI_LLM_MULTIMODAL）时按任务重建视觉 Provider
  setVisionProviderGetter((env) => resolveVisionProvider(mainConfig, visionConfig, env))
  registry.register(makeVisionTool({ vision: getVisionProvider }))
  // 定时任务工具：仅 GEBAI_CRON_ENABLED=true 时注册（关闭时 cron_* 完全不可见，不进工具表/schema）
  if (config.cronEnabled) {
    for (const tool of Object.values(makeCronTools())) registry.register(tool)
  }
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
  const env = new EnvManager(config.gebaiHome, store)
  const events = new EventBus()
  const subAgents = new SubAgentManager({ registry, preloadOverride: config.preloadSubAgents })
  await subAgents.discover()

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
  })
  // 定时任务：GEBAI_CRON_ENABLED=true 时启动调度器（工具注册见上，关闭时完全不注册）
  let cron: CronManager | null = null
  if (config.cronEnabled) {
    cron = new CronManager({ home: config.gebaiHome, store, env, sandbox, events, safeMode: config.safeMode })
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
  const webhooks = new WebhookManager({ home: config.gebaiHome })
  webhooks.ownerOf = async (sessionId: string) => store.ownerOf(sessionId)
  await webhooks.start(events)
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
  const baseDeps: AppDeps = { config, store, env, sandbox, registry, engine, auth, events, subAgents, webhooks, externalAuth }
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

  const server = Bun.serve<unknown>({
    hostname: config.host,
    port: config.port,
    idleTimeout: 240,
    fetch: (req, srv) => {
      const url = new URL(req.url)
      const wsPath = `${config.basePath === "/" ? "" : config.basePath}/ws`
      if (url.pathname === wsPath && srv.upgrade(req, { data: {} })) return
      // 开发热刷新通道（仅 --reload 模式注册）：页面经此接收 reload 广播
      const hotPath = `${config.basePath === "/" ? "" : config.basePath}/__gebai_hot`
      if (config.devReload && url.pathname === hotPath && srv.upgrade(req, { data: { hot: true } })) return
      return app.fetch(req, srv)
    },
    websocket: {
      open(ws) {
        // 热刷新通道连接（无消息语义，仅接收广播）；主 /ws 通道逻辑见下
        if ((ws.data as { hot?: boolean } | undefined)?.hot) {
          devReloadClients.add(ws)
          return
        }
        // 连接级事件推送：订阅该用户的事件日志（在线推送 = 日志条目实时投递，带 seq）。
        // 退订函数存于 WeakMap，避免被 auth.login 的 ws.data 覆盖而泄漏订阅。
        // 发送统一走背压保护 sink（慢客户端超限断开，见 makeWsSink）
        const conn = makeWsConn(ws, deps, state)
        const sink = makeWsSink(ws)
        let unsub = state.subscribe(conn.get().id, (entry) => sink.send(JSON.stringify(entry)))
        // 用户变更（auth.login/logout）：事件订阅重绑到新用户
        conn.onUserChange?.(() => {
          unsub()
          unsub = state.subscribe(conn.get().id, (entry) => sink.send(JSON.stringify(entry)))
        })
        wsSubs.set(ws, () => unsub())
        // 本地模式：建连即推送状态快照（服务模式在 auth.login 后推送）
        if (config.auth === "local") {
          void state.pushSnapshot(sink, conn).catch(() => {})
        }
      },
      async message(ws, raw) {
        let msg: { type: string; payload?: Record<string, unknown>; id?: string }
        try {
          msg = JSON.parse(raw as string)
        } catch {
          makeWsSink(ws).send(JSON.stringify({ type: "error", ok: false, error: "bad json" }))
          return
        }
        const conn = makeWsConn(ws, deps, state)
        // 按到达顺序串行处理（见 wsMsgChains 注释）：前一条消息完成后再处理下一条，
        // 保证 auth.login 的 conn.set(u) 先于后续请求生效（Bun 不保证 async handler 串行）
        const prev = wsMsgChains.get(ws) ?? Promise.resolve()
        const next = prev.then(() => handleWsMessage(deps, makeWsSink(ws), msg, conn)).catch(() => {})
        wsMsgChains.set(ws, next)
        await next
      },
      close(ws) {
        wsSubs.get(ws)?.()
        wsSubs.delete(ws)
        devReloadClients.delete(ws)
      },
    },
  })

  console.log(
    `[gebai] listening on http://${config.host}:${config.port} (GEBAI_HOME=${config.gebaiHome}, auth=${config.auth}, sandbox=${sandbox.enabled}${config.devReload ? ", dev-reload" : ""})`,
  )
  // 进程退出时终止 vite build --watch 子进程（防孤儿）
  process.on("exit", () => devReload?.stop())
  return { server, app, engine, store, registry, subAgents, auth, events, config, deps, gc, cron, devReload, feishuBot }
}

if (import.meta.main) {
  await ensureWebDistBuilt()
  startServer().catch((err) => {
    console.error(err)
    process.exit(1)
  })
}

/**
 * 脚本调试模式下：server 托管 `packages/web/dist` 构建产物，若 web 源码比
 * dist 新（或 dist 缺失），启动前自动执行 web 构建，避免「改了前端代码但
 * 页面仍是旧产物」的坑。二进制模式跳过（产物随二进制分发）。
 */
async function ensureWebDistBuilt(): Promise<void> {
  const config = loadConfig()
  if (isBinaryMode()) return

  const distIndex = join(config.webDist, "index.html")
  const distMtime = existsSync(distIndex) ? statSync(distIndex).mtimeMs : 0
  if (!distMtime) console.log("[gebai] web dist 缺失，自动构建…")

  // 扫描 web 源码最新修改时间（src/ 递归 + 顶层入口/依赖清单）
  const webRoot = join(config.webDist, "..")
  let newestSrc = 0
  const check = (p: string) => {
    try {
      const m = statSync(p).mtimeMs
      if (m > newestSrc) newestSrc = m
    } catch {
      /* 文件不存在忽略 */
    }
  }
  await walkDir(join(webRoot, "src"), 4, async (p) => check(p))
  check(join(webRoot, "index.html"))
  check(join(webRoot, "package.json"))

  if (newestSrc > distMtime + 1000) {
    console.log("[gebai] 检测到 web 源码更新，自动构建（约 1s）…")
    const r = Bun.spawnSync({
      cmd: [process.execPath, "run", "--cwd", webRoot, "build"],
      stdout: "inherit",
      stderr: "inherit",
    })
    if (!r.success) {
      console.warn("[gebai] web 自动构建失败，继续使用现有 dist；可手动执行: bun run --cwd packages/web build")
    } else {
      console.log("[gebai] web 构建完成")
    }
  }
}
