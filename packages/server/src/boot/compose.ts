/** 组合根（DI 装配）：loadConfig → 各组件构建（store/registry/sandbox/auth/subAgents/engine/
 *  webhooks/tasks/todos/gc/feishuBot/externalAuth/state/app/devReload）→ Composed。网络监听见 serve.ts，
 *  进程入口见 cli.ts / index.ts。自原单文件 index.ts 的 startServer 装配段拆分，行为不变。 */
import { existsSync } from "node:fs"
import { mkdir, readFile, rename, writeFile } from "node:fs/promises"
import { join } from "node:path"
import type { ServerWebSocket } from "bun"
import { isolationConfigConflict, loadConfig } from "../core/base/config"
import { SessionStore } from "../core/session/store"
import { reportInterruptedRuns } from "../core/session/run-marker"
import { ToolRegistry } from "../core/base/registry"
import { createGlobalTools } from "../core/tools"
import { Sandbox } from "../core/security/sandbox"
import { EnvManager, applyEmbeddedEnvDefaults, cleanupLegacyUserEnv } from "../core/session/env"
import { EMBEDDED_ENV_DEFAULTS } from "../core/session/env-embedded.generated"
import { EventBus } from "../core/base/event-bus"
import { AuthService } from "../auth"
import { SubAgentManager } from "../core/agents/subagents"
import { RESERVED_PROJECT_TMP } from "../core/tools/projects"
import { AgentEngine } from "../core/engine/engine"
import { isPromptSectionKey, PROMPT_SECTION_KEYS } from "../core/engine/prompt"
import { WebhookManager } from "../webhooks"
import { GitService } from "../core/git/service"
import { FsAudit } from "../core/fs/audit"
import { TerminalService } from "../core/exec/term-session"
import { PtySessionService } from "../core/exec/pty-session"
import { LspService } from "../core/lsp/service"
import { createExternalAuthProvider } from "../external-auth"
import { defaultCredentialSources, parseCredentialSources } from "../credential-sources"
import { loadCustomCredentialSources } from "../custom-auth"
import { applyModelEnvOverrides, createProvider, parseExtraParams, resolveModelRouteProvider, resolveVisionProvider, type ApiKind, type ProviderConfig } from "../core/llm/llm"
import { setVisionProviderGetter } from "@gebai/agents"
import { scheduleGC } from "../core/session/gc"
import { TaskManager } from "../core/schedule/tasks"
import { UserTodoManager } from "../core/schedule/todos"
import { createPrimaryGate } from "../core/schedule/primary"
import { isFeishuChatId, validateNotifyChannel } from "../core/schedule/notify"
import { createApp, type AppDeps } from "../app"
import { DevReloadManager, webRootOf } from "../dev-reload"
import { WsStateService } from "../ws-state"
import { FeishuBot } from "../feishu-bot/bot"
import { EngineBotAdapter } from "../feishu-bot/adapter"
import { createFeishuApi } from "../feishu-bot/api"
import { makeWsSink } from "./serve"
import { installBrowserFetchProxy } from "../core/browser/fetch-proxy"
import { ensureCorporateTrust } from "../core/trust/corporate"
import { log, setLogLevel } from "@gebai/sdk/node"
import { FeishuConn } from "../feishu-bot/conn"
import { ChannelRegistry, ChannelInputBridge, defaultChannelScanDirs } from "@gebai/channels"
import type { InputCounters, PersonaDispatchPort } from "@gebai/channels"
import type { DriverPersistence as PersonaDriverPersistence, DriverRecord as PersonaDriverRecord } from "@gebai/persona"

/**
 * 未装配人格运行时的输入面落点：把入站事件记入日志并计数。
 *
 * 为何不直接丢弃：渠道能收事件是**可独立验证的能力**（`local` 的 drop/inbox 是联调入口），
 * 不该因为人格层未装配就静默失败。日志 + 统计让「收到了什么」可见，
 * 且接口上就是 `PersonaDispatchPort`——将来接上真运行时时零改动。
 */
class LogOnlyDispatcher implements PersonaDispatchPort {
  private readonly counters = new Map<string, InputCounters>()
  constructor(private readonly opts: { log: { info(msg: string): void; warn(msg: string): void } }) {}

  /** 注册表约定的入口（与 `ChannelInputBridge.deliver` 同形）。 */
  async deliver(
    channel: string,
    _defaultKind: unknown,
    ev: { summary: string; body: string; id?: string; kind?: string; source?: string },
  ): Promise<{ status: "unrouted"; reason: string }> {
    return this.dispatch({
      id: ev.id ?? "",
      kind: ev.kind ?? "message",
      channel,
      body: ev.body,
      context: { source: ev.source ?? channel, summary: ev.summary },
    }) as Promise<{ status: "unrouted"; reason: string }>
  }

  async dispatch(event: {
    id: string
    kind: string
    channel: string
    body: string
    context: { source: string; summary: string }
  }): Promise<{ status: string; reason?: string }> {
    const c = this.counters.get(event.channel) ?? {
      emitted: 0,
      accepted: 0,
      unrouted: 0,
      duplicate: 0,
      overflow: 0,
      unavailable: 0,
    }
    c.emitted++
    c.unrouted++
    this.counters.set(event.channel, c)
    this.opts.log.info(
      `[gebai] 渠道事件（无人格运行时，仅记录）：channel=${event.channel} kind=${event.kind} ` +
        `source=${event.context.source} summary=${event.context.summary}`,
    )
    return { status: "unrouted", reason: "人格运行时尚未装配（GEBAI_PERSONA 未启用），事件已记入日志" }
  }
}

/**
 * 动态 driver 持久化：模型自建的分身落盘到 `{GEBAI_HOME}/persona/drivers.json`。
 *
 * 为何单独一个文件而不是写进会话：driver 是**人格级**的（跨会话生效），
 * 跟某个对话无关。写进会话会导致「换个会话就丢分工」。
 *
 * 写入串行化：并发创建（模型同时造两个分身）时整文件覆写会丢记录。
 */
class PersonaDriverStore implements PersonaDriverPersistence {
  private readonly path: string
  private chain: Promise<unknown> = Promise.resolve()

  constructor(home: string) {
    this.path = join(home, "persona", "drivers.json")
  }

  private serialize<T>(fn: () => Promise<T>): Promise<T> {
    const next = this.chain.then(fn, fn)
    this.chain = next.then(
      () => undefined,
      () => undefined,
    )
    return next
  }

  private async readAll(): Promise<Record<string, PersonaDriverRecord>> {
    const text = await readFile(this.path, "utf8").catch(() => null)
    if (!text) return {}
    try {
      return JSON.parse(text) as Record<string, PersonaDriverRecord>
    } catch {
      return {} // 坏文件不阻断启动（下次写入会重建）
    }
  }

  async load(): Promise<Array<{ personaId: string; record: PersonaDriverRecord }>> {
    const all = await this.readAll()
    return Object.entries(all).map(([key, record]) => ({
      // 键格式：`{personaId}\u0000{driverId}`
      personaId: key.split("\u0000")[0] ?? "",
      record,
    }))
  }

  async save(personaId: string, record: PersonaDriverRecord): Promise<void> {
    return this.serialize(async () => {
      const all = await this.readAll()
      all[`${personaId}\u0000${record.def.id}`] = record
      await mkdir(join(this.path, ".."), { recursive: true })
      await writeFile(this.path, JSON.stringify(all, null, 2), "utf8")
    })
  }

  async remove(personaId: string, driverId: string): Promise<void> {
    return this.serialize(async () => {
      const all = await this.readAll()
      delete all[`${personaId}\u0000${driverId}`]
      await mkdir(join(this.path, ".."), { recursive: true })
      await writeFile(this.path, JSON.stringify(all, null, 2), "utf8")
    })
  }
}

/**
 * 解析 `@gebai/channels` 包根（供 `defaultChannelScanDirs` 推内置域与二开域）。
 *
 * 为何不直接 `import.meta.dirname` 推导：包会被打包进 dist / 单文件二进制，
 * 那时路径与源码树无关——以内置渠道文件的实际存在与否作为判据，都不存在时
 * 返回一个不存在的路径（扫描域不存在 = 正常跳过，不是错误）。
 */
async function resolveChannelsPackageRoot(): Promise<string> {
  for (const candidate of [join(import.meta.dirname, "..", "..", "..", "channels"), join(import.meta.dirname, "..", "..", "..", "..", "packages", "channels")]) {
    if (existsSync(join(candidate, "src", "index.ts"))) return candidate
  }
  // 二进制形态：内置渠道已内联进 bundle，目录扫描不适用（显式注册由 bundle 负责）
  return join(import.meta.dirname, "__no_channels__")
}

/** 解析 `@gebai/persona` 包根（供 `defaultPersonaScanDirs` 推内置域与二开域）。 */
async function resolvePersonaPackageRoot(): Promise<string> {
  for (const candidate of [join(import.meta.dirname, "..", "..", "..", "persona"), join(import.meta.dirname, "..", "..", "..", "..", "packages", "persona")]) {
    if (existsSync(join(candidate, "src", "index.ts"))) return candidate
  }
  return join(import.meta.dirname, "__no_personas__")
}

/** 逗号/空白分隔的名单解析（与既有 `GEBAI_*_ENABLE/DISABLE` 同口径）。 */
function splitList(raw: string | undefined): string[] {
  if (!raw) return []
  return raw
    .split(/[,\s]+/)
    .map((s) => s.trim())
    .filter(Boolean)
}

/** 组合产物：全部组件实例 + 监听所需的依赖包（serve.ts 消费）。 */
export interface Composed {
  config: ReturnType<typeof loadConfig>
  store: SessionStore
  registry: ToolRegistry
  engine: AgentEngine
  subAgents: SubAgentManager
  /** 对外动作渠道注册表（act/observe 工具）。 */
  channels: ChannelRegistry
  auth: AuthService
  events: EventBus
  sandbox: Sandbox
  webhooks: WebhookManager
  tasks: TaskManager | null
  /** 用户级待办管理器（GEBAI_IDLE_TODO_ENABLED=false 时为 null）。 */
  todos: UserTodoManager | null
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
  // 日志级别早于任何装配日志生效（GEBAI_LOG_LEVEL，默认 info；非法值忽略）
  setLogLevel(config.logLevel)
  // 启动裁剪自检（DESIGN「启动裁剪与领域专用模式」）：领域档案与未知段落键在启动期说清楚——
  // 静默忽略的配置项看起来就是「裁剪没生效」，比启动失败难排查得多（档案本身非法已在 loadConfig 抛错）
  if (config.profile) log.info(`[gebai] 领域专用模式档案: ${config.profile}`)
  for (const [key, list] of [
    ["GEBAI_PROMPT_ENABLE", config.promptEnable],
    ["GEBAI_PROMPT_DISABLE", config.promptDisable],
  ] as const) {
    for (const k of list ?? []) {
      if (!isPromptSectionKey(k)) log.warn(`[gebai] ${key} 中的提示词段落不存在: ${k}（可用: ${PROMPT_SECTION_KEYS.join("/")}）`)
    }
  }

  // 透明浏览器代理（GEBAI_BROWSER_PROXY=1，重启生效）：服务启动即安装 fetch 垫片——平台级
  // 能力，不依赖任何子Agent 的装载/裁剪（桥接基建在 core/browser，见 DESIGN「透明浏览器代理」）
  installBrowserFetchProxy()

  // 企业内网 TLS 出口检查的根证书发现（**必须在任何 HTTPS 请求之前**）：
  //
  // Bun/Node 的 fetch 不读 Windows 证书库。公司网里 TLS 被出口设备改写后
  //（本机实测：飞书 accounts 域的证书链被换成 TENANT-RESTRICTION-CA 签发），
  // 浏览器能访问而服务端报 `unable to verify the first certificate`——
  // 表现为「授权页能开、扫码也成功、回调到了服务端才失败」，极难归因。
  //
  // 本探针只在实际需要时才导出（能通就一行日志都不用打）。
  {
    const trust = await ensureCorporateTrust({
      home: config.gebaiHome,
      log: (level, msg) => log[level](msg),
    })
    if (trust.applied) log.warn(`[gebai] ${trust.message}`)
  }

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

  // 额外多模态（视觉）模型：GEBAI_VISION_MODEL 配置后启用独立视觉 Provider（vision 子代理 analyze 使用），
  // 接口地址/密钥/类型缺省时继承主模型配置；未配置时回落到主模型（须声明多模态能力）
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
  // restart_server 仅本地模式注入（操作者本人机器可重启自身；服务模式多用户部署无权重启宿主服务）
  const globalToolFilter = config.auth === "local" ? null : new Set(["restart_server"])
  for (const tool of Object.values(createGlobalTools())) {
    if (globalToolFilter?.has(tool.name)) continue
    registry.register(tool)
  }
  // 视觉 provider 提供者注册（视觉能力统一经 vision 子代理——其 def 的 analyze 工具经 getVisionProvider
  // 复用同一解析逻辑）；任务级 env 覆盖生效：会话/前端配置 GEBAI_VISION_*（或 GEBAI_LLM_MULTIMODAL）时按任务重建视觉 Provider。
  // 全局 vision 工具已移除（架构决策：视觉相关统一走子代理，主会话需视觉时 agent_load 装载或路由自愈，见 DESIGN「视觉工具 vision」）
  setVisionProviderGetter((env) => resolveVisionProvider(mainConfig, visionConfig, env))
  // 启动级工具启停策略（GEBAI_TOOL_ENABLE/DISABLE 与领域档案）：**注册时判定**——子Agent 在运行期
  // 装载/热加载进来的 {agent}_* 工具同样受约束（一次性快照会让白名单在装载后失效）
  registry.setPolicy({ enable: config.toolEnable, disable: config.toolDisable })

  // 沙箱/脚本隔离 auto 判定（DESIGN「GEBAI_SANDBOX」「GEBAI_SCRIPT_ISOLATION」）：只看运行形态，不判定监听 IP——
  // 服务模式（多用户公用、远程可达）强制启用沙箱与会话目录隔离（防普通用户越权读写/操控宿主）；
  // 本地模式（操作者本人）默认不限制。
  // 防呆共两条（见 isolationConfigConflict）：服务模式 + 显式关沙箱 = 多租户下任意用户可越界读写宿主任意路径；
  // 服务模式 + 显式关脚本隔离 = 脚本环境与缓存落回宿主共享位置。启动直接拒绝
  // （安全配置错误在启动期暴露，而非运行期出事后追溯）。
  const conflict = isolationConfigConflict(config)
  if (conflict) throw new Error(conflict)
  const sandbox = new Sandbox({
    home: config.gebaiHome,
    enabled: config.sandbox === "on" || (config.sandbox === "auto" && config.auth === "server"),
    // 豁免语义（与 DESIGN 一致）：仅本地模式默认用户（id=admin）豁免路径沙箱——
    // 本地模式是操作者本人机器，不受限；服务模式一律沙箱（admin 也不豁免，多租户边界一致）
    isExempt: (u) => u === "admin" && config.auth === "local",
    // 脚本运行根（服务模式）：HOME/TEMP/XDG 收敛到会话目录，bubblewrap 可用时追加文件系统隔离
    scriptIsolation: config.scriptIsolation,
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
        log.info("[gebai] 本地模式用户目录迁移：users/default → users/admin")
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

  // ── 拟人模式的飞书用户凭据（进程级，跨会话常驻）──
  //
  // 必须建在渠道之前：渠道发消息时要用它（`as_user=true` 以用户本人名义发）。
  // 只依赖 config 与环境变量，不依赖 engine，所以可以提前建。
  //
  // **仅 GEBAI_PERSONA=true 时创建**：关闭时连读盘都不做（行为零影响）。
  //
  // 授权提醒走**相期卡片**（用户桌面上的常驻窗口）：需要授权时推一张带链接的卡，
  // 授权完成后自动撤掉。不阻塞启动，也不靠“用户恰好打开网页”才看得到。
  let personaCredentials: import("@gebai/persona").PersonaCredentials | null = null
  /** 已推出去的授权卡 id（授权完成后用它撤卡；未推为 null）。 */
  let personaAuthCardId: string | null = null
  /** 启动后（子Agent 已发现）再推卡：凭据创建时相期还可能没被发现。 */
  let personaAuthCardSync: ((status: import("@gebai/persona").PersonaCredentialStatus) => Promise<void>) | null = null
  if (config.personaEnabled) {
    const { PersonaTokenStore, PersonaOAuth, PersonaCredentials, scopesFromEnv } = await import("@gebai/persona")
    const personaAppId = () => process.env.GEBAI_CHANNELS_FEISHU_APP_ID || process.env.GEBAI_FEISHU_APP_ID
    const personaAppSecret = () => process.env.GEBAI_CHANNELS_FEISHU_APP_SECRET || process.env.GEBAI_FEISHU_APP_SECRET

    /**
     * 按凭据状态维护相期上的授权卡（推/撤）。
     *
     * 写成函数而不是在两处各写一遍：状态变化的入口有多个（启动、授权完成、撤销），
     * 分散判断必然出现“一个地方推了卡、另一个地方忘了撒”。
     *
     * 走**子Agent 定义取工具**（与模型 `agent_load` 同一来源），不直接 import 相期源码——
     * 内置子Agent 是构建期打包的，源码路径不属于 server 的可引用面。
     */
    const syncAuthCard = async (status: import("@gebai/persona").PersonaCredentialStatus): Promise<void> => {
      const def = subAgents.defOf("xiangqi")
      const authorize = def?.tools?.card_authorize
      const dismiss = def?.tools?.card_dismiss
      if (!authorize || !dismiss) return // 相期未注册（被禁或发现失败）：不报错，网页横幅仍会提示
      try {
        if (status.ready) {
          if (personaAuthCardId !== null) {
            await dismiss.execute({ id: personaAuthCardId }, {} as never)
            personaAuthCardId = null
          }
          return
        }
        if (!status.authorizeUrl) return // 未配应用凭据：推了也是废链接
        if (personaAuthCardId !== null) return // 已有卡在，不重复推
        const res = await authorize.execute(
          {
            title: "需要飞书授权",
            url: status.authorizeUrl,
            from: "歌白",
            body:
              `${status.message}\n\n` +
              "授权后我才能以**你本人**的名义在飞书群里回消息（而不是机器人）。\n" +
              "点下面按钮完成授权；完成后这张卡会自动消失。",
            // 链接 10 分钟过期：过期后卡片自动消失，不留一个点不开的按钮
            ttl_ms: 10 * 60 * 1000,
          },
          {} as never,
        )
        const data = res.data as { id?: string } | undefined
        if (data?.id) personaAuthCardId = data.id
      } catch (err) {
        // 相期没在运行/装载失败不能影响启动：日志提示就够了（用户也可从网页顶部横幅看到）
        log.warn(`[gebai] 推送授权卡到相期失败：${String((err as Error)?.message || err)}`)
      }
    }
    personaAuthCardSync = syncAuthCard

    personaCredentials = new PersonaCredentials({
      store: new PersonaTokenStore({
        home: config.gebaiHome,
        appId: personaAppId,
        appSecret: personaAppSecret,
        log: (level, msg) => log[level](msg),
      }),
      oauth: new PersonaOAuth(),
      appId: personaAppId,
      appSecret: personaAppSecret,
      // 申请哪些 scopes：默认是「能跑起来的最小集」（不含 offline_access——
      // 它需应用在飞书后台开通，未开通时授权页报 20027 把人挡在外面）。
      // 部署方在后台开通后可经 GEBAI_PERSONA_SCOPES 追加。
      scopes: scopesFromEnv(),
      // 回调走拟人专用端点（与会话级 /api/v1/oauth/feishu/callback 分开）
      redirectUri: () => {
        const base = (process.env.GEBAI_PUBLIC_URL ?? "").replace(/\/+$/, "")
        return `${base || `http://localhost:${config.port}`}/api/v1/persona/feishu/callback`
      },
      log: (level, msg) => log[level](msg),
      // 授权完成/撤销 → 同步相期上的卡
      onChange: (status) => void syncAuthCard(status),
    })
    // 启动检查（不吐令牌明文，只报状态）
    const cred = await personaCredentials.status()
    if (cred.ready) {
      log.info(`[gebai] 飞书凭据就绪：${cred.message}${cred.remainingMs ? `（剩 ${Math.round(cred.remainingMs / 60000)} 分钟）` : ""}`)
    } else if (cred.authorizeUrl) {
      log.warn(`[gebai] 飞书凭据不可用：${cred.message}`)
      log.warn(`[gebai] 授权链接：${cred.authorizeUrl}`)
      log.warn(`[gebai] 回调地址需已在飞书后台登记：${process.env.GEBAI_PUBLIC_URL || `http://localhost:${config.port}`}/api/v1/persona/feishu/callback`)
    } else {
      log.warn(`[gebai] 飞书凭据不可用：${cred.message}（未配置 GEBAI_CHANNELS_FEISHU_APP_ID / _APP_SECRET，无法发起授权）`)
    }
    // 启动即尝试推卡：相期若已连上立刻可见；没开着也无妨（卡片会缓存，它启动后重推）
    // 这里只尝试一次；子Agent 发现是异步的，若此时还没就绪，下面 discover 之后会再补推。
    void syncAuthCard(cred)

    /**
     * 临近到期主动提醒（常驻检查）。
     *
     * 不这样做的话，可刷新的凭据没什么问题（临期会自动续），但**不可刷新的**
     *（缺 refresh_token —— 例如应用没开 offline_access）到期后就只能重新授权，
     * 而如果只在「下一次要用时」才发现，人格就变成了「在别人等它回话的那一刻
     * 才告诉人它需要授权」——对常驻人格是错的时机。
     *
     * 提前量取 10 分钟：足够用户看到卡、拉起手机扫码，又不至于早到“看着还早就不管了”。
     */
    const WARN_BEFORE_MS = 10 * 60_000
    const personaExpiryTimer = setInterval(
      () => {
        void (async () => {
          try {
            const st = await personaCredentials!.status() // status() 会顺带尝试刷新
            if (st.ready && (st.remainingMs ?? Number.POSITIVE_INFINITY) <= WARN_BEFORE_MS) {
              log.warn(`[gebai] 飞书凭据将在 ${Math.max(1, Math.round((st.remainingMs ?? 0) / 60000))} 分钟后失效`)
            }
            // 状态变了（就绪→需授权）时推卡；syncAuthCard 自身幂等（已推就不重推）
            await syncAuthCard(st)
          } catch {
            // 提醒本身出错不能影响服务（unref 的定时器不持连接）
          }
        })()
      },
      5 * 60_000,
    )
    // unref：只做后台提醒，不该阻止进程退出（测试/短命令跑完就该退）
    personaExpiryTimer.unref?.()
  }
  // 对外动作渠道（act/observe 工具，DESIGN「对外动作：渠道抽象」）：与子Agent/事件源同款
  // 「丢文件即注册」——内置 packages/channels/src/channels/ + 二开 custom/channels/。
  // 发现/启动失败只记 loadErrors 不阻断启动（单渠道坏不能连带整个服务起不来）。
  const channels = new ChannelRegistry({
    scanDirs: defaultChannelScanDirs({ packageRoot: await resolveChannelsPackageRoot() }),
    home: config.gebaiHome,
    enable: splitList(process.env.GEBAI_CHANNELS_ENABLE),
    disable: splitList(process.env.GEBAI_CHANNELS_DISABLE),
    // 宿主注入的能力（channels 包零依赖，不 import 本包）：
    // - feishuLongConn：飞书长连接工厂（渠道自建**独立**连接，与会话机器人互不干扰）
    // - feishuSessionAppId：会话机器人 AppID——渠道据此检测「与自己的 AppID 撞车」并拒绝启动
    //   （同一飞书应用只能一条长连接，同时用会互踢，表现为其中一边偶发收不到消息）
    deps: {
      feishuLongConn: (o: { appId: string; appSecret: string; onEvent: (ev: Record<string, unknown>) => void }) =>
        new FeishuConn({
          appId: o.appId,
          appSecret: o.appSecret,
          onEvent: (ev) => o.onEvent(ev as Record<string, unknown>),
          log: (m) => log.info(`[channels:feishu] ${m}`),
        }),
      ...(config.feishuAppId ? { feishuSessionAppId: config.feishuAppId } : {}),
      // 拟人凭据（feishu 渠道的 `as_user=true` 用它以用户本人名义发消息）；未启用时为 null
      ...(personaCredentials ? { personaCredentials } : {}),
    },
    log: { info: log.info, warn: log.warn, error: log.error },
  })
  // 注：`channels.init()` 在 engine 创建之后调用（见下）——输入面需要把事件投给引擎，
  // 若在此处 init，`startListen` 拿不到可用的 dispatcher，事件会全部落到 unavailable。
  // 客卿（DESIGN「客卿」）：仅本地形态启用——沙箱启用（服务端部署）时注入 null
  // 显式禁用；GEBAI_KEQING=off 同效。发现失败不阻断启动（逐项记 loadErrors）。
  subAgents.setKeqingOpts(sandbox.enabled ? null : {})
  // 客卿发现放后台（deferNative）：侧车握手与编译型客卿的构建引导耗时不可控（实测：受限网络下
  // go 下载 toolchain 的 TCP 超时能把启动拉长 30s），不该让服务启动等它；TS 域扫描（本地）仍同步。
  // 就绪后自动合并进定义视图，需完整清单的操作（agent_load）由 SubAgentManager 内部等它。
  await subAgents.discover({ deferNative: true })
  // 子Agent 已发现 → 补推一次授权卡（上面建凭据时相期可能还未被发现）
  if (personaCredentials && personaAuthCardSync) {
    void personaAuthCardSync(await personaCredentials.status())
  }
  // 任务能力开关（GEBAI_TASKS_ENABLED，默认 true）：关闭时 task 子Agent 不注册（agent_list/agent_load/
  // subsession_run 均不可见，task_* 工具不进工具表/schema，与调度器一致完全隐藏）；开启时按需装载、REST /api/v1/tasks 可管
  if (!config.tasksEnabled) subAgents.unregister("task")
  // 子Agent 启停名单（GEBAI_SUB_AGENTS_ENABLE 白名单 / GEBAI_SUB_AGENTS_DISABLE 黑名单）：enable 非空仅保留
  // 名单内，disable 移除名单内（先白后黑）；unregister 后 agent_list/装载/子会话运行/系统提示词注入均不可见
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
    // 对外动作渠道（act/observe）：进程级单例，父子会话/子代理共用
    channels,
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
    // 子会话运行模型路由（DESIGN「子会话运行」多路接口）：GEBAI_LLM_ROUTES 路由名命中走
    // 独立端点/模型，字面模型名按主配置基准覆盖——多分支多路并行分摊单路限流
    resolveModelProvider: (env, name) => resolveModelRouteProvider(mainConfig, env, name),
  })
  // 拟人模式装配（GEBAI_PERSONA=true）：**开关关闭时以下代码一行不执行**。
  //
  // 「关闭时行为逐字节不变」的具体保证：
  // 1. `PersonaRuntime` / `Catalog` / `Resolver` / `Runner` 均不被实例化（无副作用、无扫描、无文件读写）
  // 2. `engine` 不加任何入参分支（人格身份走已有的 `channelNote` 注记，不新增引擎逻辑）
  // 3. 渠道输入面接 `LogOnlyDispatcher`——只记日志，与实际部署完全一致
  // 4. config 多两个字段（默认 false/true），不参与任何既有分支
  let personaRuntime: import("@gebai/persona").PersonaRuntime | null = null
  let personaCatalog: import("@gebai/persona").PersonaCatalog | null = null
  let personaSessions: import("../core/session/persona-sessions").PersonaSessionResolver | null = null
  if (config.personaEnabled) {
    const { PersonaRuntime, PersonaCatalog, DriverRegistry, defaultPersonaScanDirs, discoverPersonas } = await import("@gebai/persona")
    const { PersonaSessionResolver } = await import("../core/session/persona-sessions")
    const { EnginePersonaRunner } = await import("../core/session/persona-runner")

    const personaDirs = defaultPersonaScanDirs({ packageRoot: await resolvePersonaPackageRoot() })
    const found = await discoverPersonas(personaDirs)
    if (found.personas.size === 0) {
      log.warn(
        `[gebai] GEBAI_PERSONA 已开启但未发现任何人格定义（内置 ${personaDirs.builtin} / 二开 ${personaDirs.custom}）` +
          `——入站事件将无人处理（记日志）。放一个 \`export const persona: PersonaDef\` 文件即可。`,
      )
    } else {
      log.info(`[gebai] 拟人模式已启用，${found.personas.size} 个人格：${[...found.personas.keys()].join("、")}`)
    }
    for (const [id, err] of found.errors) log.warn(`[gebai] 人格 ${id} 加载失败 [${err.domain}]: ${err.error}`)

    // 动态 driver 持久化：模型自建的分身落盘到 {GEBAI_HOME}/persona/drivers.json
    const driverStore = new PersonaDriverStore(config.gebaiHome)
    const driverRegistry = new DriverRegistry({
      statics: [...found.personas.values()].flatMap((p) => (p.drivers ?? []).map((def) => ({ personaId: p.id, def }))),
      persistence: driverStore,
      log: { info: log.info, warn: log.warn },
    })
    await driverRegistry.init()

    personaCatalog = new PersonaCatalog({ personas: found.personas, drivers: driverRegistry })
    personaSessions = new PersonaSessionResolver({ store, log: { info: log.info, warn: log.warn } })
    // 会话名用「人格显示名·分身 id」：侧栏里一眼能认出来
    personaSessions.nameOf = (id) => personaCatalog!.get(id)?.name ?? id

    personaRuntime = new PersonaRuntime({
      lookup: personaCatalog,
      sessions: personaSessions,
      runner: new EnginePersonaRunner({
        engine,
        autoApprove: config.personaAutoApprove,
        log: { info: log.info, warn: log.warn },
      }),
      log: { info: log.info, warn: log.warn },
    })
    personaRuntime.start()

    // 预热会话绑定（重启后第一条事件不必等扫盘）
    void personaSessions.warm("admin").then((n) => {
      if (n > 0) log.info(`[gebai] 已恢复 ${n} 个人格会话绑定`)
    })
  }

  // 渠道初始化（写/读/收三面一起起）+ 输入面桥接。
  //
  // **输入面的落点**：渠道推上来的事件需要一个「归谁、进哪个会话」的裁决者。
  // 拟人模式未开启时退化为**事件日志消费**——这样「渠道能收」可独立验证，
  // 且不会静默吞掉事件（没有人格时至少日志里看得到）。
  channels.attachDispatcher(
    personaRuntime
      ? new ChannelInputBridge({ dispatch: personaRuntime, log: { info: log.info, warn: log.warn } })
      : new LogOnlyDispatcher({ log }),
  )
  await channels.init()
  {
    const chErrors = channels.loadErrors()
    if (Object.keys(chErrors).length) {
      for (const [name, err] of Object.entries(chErrors)) log.warn(`[gebai] 渠道 ${name} 不可用: ${err}`)
    }
  }
  // 服务中断留痕（见 core/session/run-marker）：上一进程在任务中途退出时，会话里只有用户消息、
  // 没有任何助手回应——启动时对残留的在途标记补一条可见说明（历史与界面都能看出那次任务被打断）
  const interrupted = await reportInterruptedRuns(config.gebaiHome, (sessionId, owner, content) =>
    store.appendMessage(sessionId, { id: crypto.randomUUID(), role: "user", content, engineNote: "interrupted", createdAt: Date.now() }, owner),
  )
  if (interrupted.reported || interrupted.failed) {
    log.info(`[gebai] 服务中断补偿：补记中断说明 ${interrupted.reported} 条${interrupted.failed ? `（另有 ${interrupted.failed} 条会话已不可写）` : ""}`)
  }
  // 事件 Webhook（REST /api/v1/webhooks 注册面）先行构建：定时任务通知的 webhookId 引用解析依赖它
  const webhooks = new WebhookManager({ home: config.gebaiHome })
  webhooks.ownerOf = async (sessionId: string) => store.ownerOf(sessionId)
  await webhooks.start(events)
  // 调度主实例门控（GEBAI_SCHEDULER，默认 auto）：调度判定全在进程内（`TaskManager.tick` 只看本进程状态
  // 与 `engine.busyUser()`），同一 GEBAI_HOME 下多实例并存必然各跑一份：闲时任务被重复领走（僵尸/从实例
  // 自己没有会话 → 永远认为「服务端空闲」）、定时任务重复触发。
  // auto：主实例锁决定谁跑调度，未拿到锁的实例只服务（看门狗在主实例退出后接管）；
  // on：强制本实例跑（忽略锁）；off：完全不跑调度。能力全关（无任务也无待办）时不参与锁协商
  const scheduling = config.tasksEnabled || config.idleTodoEnabled
  const primaryGate = scheduling ? createPrimaryGate({ home: config.gebaiHome, port: config.port, mode: config.scheduler }) : null
  const runScheduler = primaryGate ? await primaryGate.acquire() : false
  /** 本实例是否在跑调度（闲时任务自动进场的门控；看门狗接管/退让时同步翻转）。 */
  let schedulingActive = runScheduler
  // 统一任务（GEBAI_TASKS_ENABLED，默认开启）：定时/普通/闲时三类任务共用一条队列（额度
  // GEBAI_TASK_MAX_CONCURRENT）。通知通道含飞书应用消息（feishu_chat）——复用全局飞书
  // 应用凭证（GEBAI_FEISHU_APP_ID/SECRET，与机器人桥接/云文档共用）构建发送器；agents 预载名单合法性
  // 由 subAgents.def 探测；webhookId 引用解析——具名注册（userId 记录）仅本人任务可引用，全局注册
  // （admin，userId 未记录=部署方集成通道）人人可引用；关闭时调度器不启动（工具注册见上）
  let tasks: TaskManager | null = null
  if (config.tasksEnabled) {
    const feishuApi = config.feishuAppId && config.feishuAppSecret ? createFeishuApi({ appId: config.feishuAppId, appSecret: config.feishuAppSecret }) : undefined
    // 全局默认通知通道（GEBAI_TASK_NOTIFY_WEBHOOK / GEBAI_TASK_NOTIFY_FEISHU）：任务未配 notify 时兜底推送。
    // 构建期逐条校验（SSRF/域名/chat_id 形态），非法配置告警跳过不阻断启动；chat_id 形态需飞书应用凭证
    const defaultNotify: import("../core/schedule/notify").TaskNotifyChannel[] = []
    for (const [label, type, raw] of [
      ["GEBAI_TASK_NOTIFY_WEBHOOK", "webhook", config.taskNotifyWebhook],
      ["GEBAI_TASK_NOTIFY_FEISHU", "feishu", config.taskNotifyFeishu],
    ] as const) {
      if (!raw) continue
      const ch = { type, target: raw } as import("../core/schedule/notify").TaskNotifyChannel
      try {
        validateNotifyChannel(ch)
        if (type === "feishu" && isFeishuChatId(raw) && !feishuApi) throw new Error("chat_id 形态需配置 GEBAI_FEISHU_APP_ID/GEBAI_FEISHU_APP_SECRET")
        defaultNotify.push(ch)
      } catch (err) {
        log.warn(`[gebai] 全局任务通知 ${label} 配置无效，已忽略: ${(err as Error).message}`)
      }
    }
    tasks = new TaskManager({
      home: config.gebaiHome,
      store,
      env,
      sandbox,
      events,
      safeMode: config.safeMode,
      maxConcurrent: config.taskMaxConcurrent,
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
      schedulerActive: () => schedulingActive,
    })
    tasks.attach(engine)
    await tasks.start()
    // 从实例：只加载不跑 tick（数据必须加载——REST/工具的读写都以内存镜像为准，空镜像一次写入会把
    // 磁盘上其他条目抹掉；tick 才是会重复执行的部分）
    if (!runScheduler) tasks.stop()
  }
  // 用户级待办（GEBAI_IDLE_TODO_ENABLED，默认 true）：待办清单是用户级资源（users/{user}/todos.json），
  // 与会话解耦；手动执行经统一任务队列入队，开启「闲时自动执行」的待办绑定一个闲时任务由任务调度器
  // 在队列空闲时串行执行。关闭时不启动（REST /api/v1/todos 返回 503「能力未启用」）。
  let todos: UserTodoManager | null = null
  if (config.idleTodoEnabled) {
    todos = new UserTodoManager({ home: config.gebaiHome, store, tasks: tasks ?? undefined })
    // 任务运行结束回写待办（闲时任务/待办手动执行的临时任务）：成功自动勾选、失败计次达上限停用绑定任务
    tasks?.onFinished((task, run) => todos?.recordTaskResult(task, run))
    await todos.start()
  }
  // 看门狗（仅 auto）：从实例在主实例死亡/租约过期后接管调度（补一条日志便于定位「谁在跑」）；
  // 主实例锁被他人接管（本进程长时间阻塞错过续租）则停调度退让，避免双跑
  if (primaryGate && config.scheduler === "auto") {
    primaryGate.start(
      async () => {
        log.info("[gebai] 调度已接管：本实例开始运行任务队列与待办闲时任务")
        schedulingActive = true
        await tasks?.start()
        await todos?.start()
      },
      () => {
        schedulingActive = false
        tasks?.stop()
        todos?.stop()
      },
    )
  }
  // 数据生命周期 GC：启动即跑一次，之后每日周期执行（GEBAI_GC_DISABLED=1 关闭）
  const gc = config.gcDisabled
    ? null
    : scheduleGC(config.gebaiHome, {
        // 归档后失效会话缓存，防止缓存命中后 save() 在 sessions/ 重建已归档目录
        onArchive: (sessionId) => store.markRemoved(sessionId, "archived"),
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
      // 接口层：多轮交互 + 仅最终回复桥接（bot 不直接接触引擎/事件总线）；
      // 通道行为开关（GEBAI_FEISHU_BOT_*）：工具过程/助手中间轮推送 + 自动审批（部署方整体担责）
      adapter: new EngineBotAdapter(engine, events, {
        notifyTools: config.feishuBotNotifyTools,
        notifyAssistant: config.feishuBotNotifyAssistant,
        autoApprove: config.feishuBotAutoApprove,
      }),
      auth,
      notify: { tools: config.feishuBotNotifyTools, assistant: config.feishuBotNotifyAssistant },
    })
    // 长连接建立（网络握手）由 serve.ts 在监听之后异步启动，不阻塞服务可用
  }
  // 外部身份验证器（GEBAI_EXTERNAL_AUTH_SECRET / GEBAI_EXTERNAL_AUTH_URL 配置；两者同设会抛错）
  const externalAuth = createExternalAuthProvider(config)
  // 凭证来源链（DESIGN「凭证来源扩展点」）：数据级（GEBAI_CREDENTIAL_SOURCES）+ 代码级（custom/auth/）
  // 合并——环境变量显式设置即以其为准（代码级前置，便于改写内置行为）；未设置则内置缺省
  // （bearer → basic → cookie）在前、代码级追加在后。二开来源加载失败不阻断启动（写日志）。
  const customSources = await loadCustomCredentialSources()
  for (const e of customSources.errors) console.warn(`[custom/auth] 凭证来源加载失败 —— ${e}`)
  const credentialSources = config.credentialSources
    ? [...customSources.sources, ...parseCredentialSources(config.credentialSources)]
    : [...defaultCredentialSources(), ...customSources.sources]
  // WS 状态服务（MVC 模型层）：每用户事件日志（断线可重放）+ 连接状态持久化 + 快照；
  // state 自持 deps 前身（不递归引用自身）
  const baseDeps: AppDeps = {
    config,
    store,
    env,
    sandbox,
    registry,
    engine,
    auth,
    events,
    subAgents,
    webhooks,
    externalAuth,
    credentialSources,
    tasks,
    todos,
    // 文件工作台（DESIGN「文件工作台」）：Git 服务（宿主 git CLI，写/远程分别受开关约束）
    // 与写操作审计（用户直操文件系统的留痕；与工具审批的事前拦截互补）
    git: new GitService({
      writeEnabled: config.gitWrite !== false,
      remoteEnabled: config.gitRemote !== false,
      // 体量上限（GEBAI_FS_MAX_DIFF / GEBAI_GIT_MAX_FILE）：超限降级为「统计 + 提示」而非卡死
      maxDiffBytes: config.fsMaxDiff,
      maxFileChars: config.gitMaxFile,
    }),
    fsAudit: new FsAudit(config.gebaiHome, config.fsAudit !== false),
    // 终端（DESIGN「文件工作台·终端」）：持久 shell 会话（管道 + 哨兵行判定命令结束），
    // 会话常驻于服务进程内，无状态落盘；可用性由路由层按开关与沙箱判定
    terminal: new TerminalService({
      defaultShell: config.terminalShell,
    }),
    // 终端 PTY（ConPTY）：真伪控制台会话；驱动由系统自带 csc.exe 编译为临时 exe（启动后预热）
      terminalPty: new PtySessionService({}),
  // 语言服务器（DESIGN「文件工作台·语言服务器」）：有则用、没有不影响——按文件语言探测本机服务器
  lsp: new LspService({ overrides: config.lspServers, idleMs: config.lspIdleMs, maxSessions: config.lspMaxSessions }),
}
  const state = new WsStateService(config.gebaiHome, baseDeps)
  const deps: AppDeps = { ...baseDeps, state, personaCredentials }
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

  return { config, store, registry, engine, subAgents, channels, auth, events, sandbox, webhooks, tasks, todos, gc, feishuBot, deps, state, app, devReload, devReloadClients }
}
