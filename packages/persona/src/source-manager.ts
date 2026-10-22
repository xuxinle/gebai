/**
 * 事件源管理器：插件生命周期 + 热加载 + 从 `IngressEvent` 到 `PersonaEvent` 的桥接。
 *
 * 职责边界（与 `SubAgentManager` 对齐）：
 * - **管**：扫描注册、启停、热加载、失败清单、启停名单（环境变量白名单/黑名单）
 * - **不管**：不解释人格、不裁决归属、不调度——那是 `PersonaRuntime` 的事
 *
 * 为什么桥接放在这里而不是插件里（关键）：
 * - 幂等键推导、线程键推导、归属冲突检测全是**核心规则**。放插件里意味着每个源
 *   重新实现一遍，必然分叉（子 Agent 的教训：`shApprovalFreeAllowed` 若各工具各写
 *   一遍，「只读」判定就会自相矛盾）。
 * - 插件只回答「发生了什么」（`IngressEvent`），核心回答「归谁、进哪个会话」。
 */

import { buildEvent, eventIdFor } from "./events"
import { discoverEventSources, discoverySignature, validateSourceDef, type DiscoveryResult, type EventSourceScanDirs } from "./discovery"
import type { PersonaEvent } from "./event"
import type { PersonaRuntime } from "./runtime"
import type { EmitResult, EventSourceContext, EventSourceDef, EventSourceLoadError, IngressEvent } from "./source"

export interface EventSourceManagerOptions {
  scanDirs: EventSourceScanDirs
  runtime: PersonaRuntime
  /** 数据根（`ctx.home`）。 */
  home: string
  /**
   * 启停名单（`GEBAI_EVENT_SOURCES_ENABLE` / `DISABLE`）：白名单非空仅启用名单内，
   * 黑名单移除名单内（先白后黑）——与 `GEBAI_TOOL_*` / `GEBAI_SUB_AGENTS_*` 同口径。
   */
  enable?: string[]
  disable?: string[]
  log?: {
    info(msg: string): void
    warn(msg: string): void
    error(msg: string): void
  }
  /** 环境变量读取器（缺省 `process.env`；测试注入）。 */
  envReader?: (name: string) => string | undefined
  /**
   * 宿主注入的能力（经 `ctx.deps` 给插件只读访问）。
   *
   * 用途：需要宿主侧能力的源（如飞书源要复用会话级 user_access_token 存储）不必
   * import 宿主包——那样会把整棵重依赖树拖进内核。注入的键名由源自己约定。
   */
  deps?: Readonly<Record<string, unknown>>
}

/** 单个源的运行状态（面板/诊断可见）。 */
export interface EventSourceState {
  name: string
  channel: string
  description: string
  /** 是否通过启停名单。 */
  enabled: boolean
  /** 是否已启动订阅。 */
  started: boolean
  /** 启动失败原因（若有）。 */
  error?: string
  /** 该源已投递/被接管/被忽略的计数（本进程）。 */
  stats: { emitted: number; accepted: number; unattended: number; duplicate: number; overflow: number; unavailable: number }
}

/**
 * 事件源管理器。
 *
 * 生命周期：`start()`（发现 + 启动全部启用源）→ 运行 → `stop()`（逆序停止）。
 * 热加载：`refreshIfChanged()` 比对目录签名（与 `SubAgentManager` 同成本：一次目录遍历）。
 */
export class EventSourceManager {
  private defs = new Map<string, EventSourceDef>()
  private errors = new Map<string, EventSourceLoadError>()
  private started = new Map<string, EventSourceDef>()
  private startErrors = new Map<string, string>()
  private stats = new Map<string, EventSourceState["stats"]>()
  private signature: string | null = null
  private starting = false

  constructor(private readonly opts: EventSourceManagerOptions) {}

  /** 发现并启动全部启用的源（失败隔离：单源失败不阻断其它）。 */
  async start(): Promise<void> {
    await this.discover()
    await this.startAll()
  }

  /** 停止全部源（逆序；单源异常只记日志）。 */
  async stop(): Promise<void> {
    const entries = [...this.started.entries()].reverse()
    for (const [name, def] of entries) {
      try {
        await def.stop?.()
      } catch (err) {
        this.log("warn", `停止源 ${name} 失败: ${String((err as Error)?.message || err)}`)
      }
    }
    this.started.clear()
  }

  /**
   * 热加载检查（目录签名变化即重扫重启）。
   *
   * 成本：一次目录遍历（~30 次 stat，与子 Agent 同量级），可在每次事件投递前调用。
   * 返回是否发生了重扫。
   */
  async refreshIfChanged(): Promise<boolean> {
    const sig = await discoverySignature(this.opts.scanDirs)
    if (sig === this.signature) return false
    this.log("info", "事件源目录变化，重新扫描")
    await this.stop()
    await this.discover()
    await this.startAll()
    return true
  }

  /** 当前状态（全部已知源，含未启用与加载失败的）。 */
  state(): EventSourceState[] {
    const out: EventSourceState[] = []
    for (const [name, def] of this.defs) {
      const st = this.stats.get(name) ?? emptyStats()
      const enabled = this.isEnabled(name)
      out.push({
        name,
        channel: def.channel,
        description: def.description,
        enabled,
        started: this.started.has(name),
        ...(this.startErrors.has(name) ? { error: this.startErrors.get(name) } : {}),
        stats: st,
      })
    }
    // 加载失败的源也要可见（否则「配了但没反应」无从排查）
    for (const [name, err] of this.errors) {
      if (out.some((x) => x.name === name)) continue
      out.push({
        name,
        channel: "",
        description: "",
        enabled: false,
        started: false,
        error: `[${err.domain}] ${err.error}`,
        stats: emptyStats(),
      })
    }
    return out.sort((a, b) => a.name.localeCompare(b.name))
  }

  /** 已注册的源（含未启用）。 */
  list(): EventSourceDef[] {
    return [...this.defs.values()]
  }

  /**
   * 程序化注册一个源（不经目录扫描）。
   *
   * 用途：内置核心源、测试注入、以及「源需要访问宿主上下文而无法写成独立文件」的场景。
   * 语义与扫描发现的源一致：**同名覆盖**（后注册胜出），且不受启停名单约束——
   * 显式注册即视为部署方意图。
   */
  register(def: EventSourceDef): void {
    const invalid = validateSourceDef(def, def.name)
    if (invalid) {
      this.startErrors.set(def.name, `注册被拒绝：${invalid}`)
      this.log("warn", `源 ${def.name} 注册被拒绝：${invalid}`)
      return
    }
    this.defs.set(def.name, def)
    this.explicit.add(def.name)
  }

  /** 加载错误清单（`{name: [domain] error}`）。 */
  loadErrors(): Record<string, string> {
    const out: Record<string, string> = {}
    for (const [n, e] of this.errors) out[n] = `[${e.domain}] ${e.error}`
    for (const [n, e] of this.startErrors) out[n] = e
    return out
  }

  /** 手动启停单个源（面板/调试用；不改变启停名单）。 */
  async startOne(name: string): Promise<boolean> {
    const def = this.defs.get(name)
    if (!def || this.started.has(name)) return false
    return this.startDef(def)
  }

  async stopOne(name: string): Promise<boolean> {
    const def = this.started.get(name)
    if (!def) return false
    try {
      await def.stop?.()
    } catch (err) {
      this.log("warn", `停止源 ${name} 失败: ${String((err as Error)?.message || err)}`)
    }
    this.started.delete(name)
    return true
  }

  // ── 内部 ──

  private async discover(): Promise<void> {
    if (this.starting) return
    this.starting = true
    try {
      const result: DiscoveryResult = await discoverEventSources(this.opts.scanDirs)
      // 合并而非覆盖：显式 register() 的源不经目录扫描，重扫不能把它们丢掉
      // （否则「注册 → start → 源消失」，且完全无告警）。扫描发现与显式注册同名时，
      // 显式注册胜出（部署方意图优先于目录里碰巧存在的同名文件）。
      for (const [name, def] of result.sources) {
        if (this.explicit.has(name)) continue
        this.defs.set(name, def)
      }
      for (const [name, err] of result.errors) {
        if (this.explicit.has(name)) continue
        this.errors.set(name, err)
      }
      this.signature = result.signature
      // 已消失的目录源从 defs 里摘除（显式注册的保留）
      for (const name of [...this.defs.keys()]) {
        if (this.explicit.has(name)) continue
        if (!result.sources.has(name)) this.defs.delete(name)
      }
      for (const [name, err] of result.errors) {
        this.log("warn", `事件源 ${name} 加载失败 [${err.domain}]: ${err.error}`)
      }
      this.warnUnknownAllowlistNames()
    } finally {
      this.starting = false
    }
  }

  private async startAll(): Promise<void> {
    for (const def of this.defs.values()) {
      if (!this.isEnabled(def.name)) continue
      await this.startDef(def)
    }
  }

  /** 启动单个源（失败隔离：记 startErrors，不影响其它源）。 */
  private async startDef(def: EventSourceDef): Promise<boolean> {
    const ctx = this.makeContext(def)
    try {
      await def.start(ctx)
      this.started.set(def.name, def)
      this.startErrors.delete(def.name)
      this.log("info", `事件源已启动: ${def.name}（channel=${def.channel}）`)
      return true
    } catch (err) {
      const msg = `启动失败: ${String((err as Error)?.message || err)}`
      this.startErrors.set(def.name, msg)
      this.log("warn", `事件源 ${def.name} ${msg}`)
      return false
    }
  }

  /**
   * 构造插件上下文：`emit` 是全流程唯一出口。
   *
   * 归属与幂等的裁决链：`IngressEvent` → `buildEvent`（幂等键/线程键）→
   * `runtime.dispatch`（路由/冲突检测/入队）。插件只能拿到裁决结果。
   */
  private makeContext(def: EventSourceDef): EventSourceContext {
    const declared = new Set((def.envVars ?? []).map((v) => v.name))
    const readEnv = this.opts.envReader ?? ((n: string) => process.env[n])
    const bump = (key: keyof EventSourceState["stats"]) => {
      const st = this.stats.get(def.name) ?? emptyStats()
      st[key]++
      this.stats.set(def.name, st)
    }
    return {
      name: def.name,
      home: this.opts.home,
      deps: this.opts.deps ?? {},
      emit: async (ingress: IngressEvent): Promise<EmitResult> => {
        bump("emitted")
        const event = this.toEvent(def, ingress)
        const res = await this.opts.runtime.dispatch(event)
        const key =
          res.status === "accepted"
            ? "accepted"
            : res.status === "unrouted"
              ? "unattended"
              : res.status === "duplicate"
                ? "duplicate"
                : res.status === "overflow"
                  ? "overflow"
                  : "unavailable"
        bump(key)
        return res.status === "unrouted"
          ? { status: "unattended", ...(res.reason ? { reason: res.reason } : {}) }
          : ({
              status: res.status,
              ...(res.personaId ? { personaId: res.personaId } : {}),
              ...(res.sessionId ? { sessionId: res.sessionId } : {}),
              ...(res.queued !== undefined ? { queued: res.queued } : {}),
              ...(res.reason ? { reason: res.reason } : {}),
              // 广播模型：逐订阅者结果一并交给插件（“到底谁收到了”对源可观测）
              ...(res.deliveries?.length ? { deliveries: res.deliveries } : {}),
            } as EmitResult)
      },
      log: (level, msg) => this.log(level, `[${def.name}] ${msg}`),
      env: (name) => {
        // 白名单过滤：未声明即不可读（防「读告警的插件」顺手拿到模型密钥）
        if (!declared.has(name)) {
          this.log("warn", `源 ${def.name} 读取未声明的环境变量 ${name}（已拒绝；请在 envVars 中声明）`)
          return undefined
        }
        return readEnv(name)
      },
    }
  }

  /**
   * `IngressEvent` → `PersonaEvent`。
   *
   * 幂等键优先级：插件显式 `id` > `eventIdFor` 的按 kind 推导。
   * `personaId` 留空时由 `runtime.dispatch` 的路由裁决（这也是插件通常不该填的原因）。
   */
  private toEvent(def: EventSourceDef, ingress: IngressEvent): PersonaEvent {
    if (ingress.channel && ingress.channel !== def.channel) {
      // 插件声明 channel 与事件自带 channel 冲突：以事件为准但告警（多渠道路由的常见配置错）
      this.log("warn", `源 ${def.name} 的事件 channel「${ingress.channel}」与其声明「${def.channel}」不一致，按事件值处理`)
    }
    const channel = ingress.channel || def.channel
    const built = buildEvent({
      kind: ingress.kind,
      personaId: ingress.personaId ?? "",
      channel,
      body: ingress.body,
      source: ingress.source,
      summary: ingress.summary,
      ...(ingress.refs ? { refs: ingress.refs } : {}),
      ...(ingress.attachments?.length ? { attachments: ingress.attachments } : {}),
      ...(ingress.id ? { id: ingress.id } : {}),
      ...(ingress.at !== undefined ? { at: ingress.at } : {}),
    })
    return built
  }

  /** 显式注册的源（不受启停名单约束，见 register 注释）。 */
  private explicit = new Set<string>()

  /** 启停名单判定（先白后黑，与既有裁剪口径一致）。 */
  private isEnabled(name: string): boolean {
    if (this.explicit.has(name)) return true
    const enable = this.opts.enable ?? []
    const disable = this.opts.disable ?? []
    if (enable.length && !enable.includes(name)) return false
    return !disable.includes(name)
  }

  /** 名单里的未知名告警（拼错名单会让源静默不启用，属最难排查的一类）。 */
  private warnUnknownAllowlistNames(): void {
    for (const name of [...(this.opts.enable ?? []), ...(this.opts.disable ?? [])]) {
      if (!this.defs.has(name) && !this.errors.has(name)) {
        this.log("warn", `启停名单中的事件源「${name}」不存在（拼写错误？已注册：${[...this.defs.keys()].join(", ") || "无"}）`)
      }
    }
  }

  private log(level: "info" | "warn" | "error", msg: string): void {
    this.opts.log?.[level](`[persona:sources] ${msg}`)
  }
}

function emptyStats(): EventSourceState["stats"] {
  return { emitted: 0, accepted: 0, unattended: 0, duplicate: 0, overflow: 0, unavailable: 0 }
}

/** 幂等键推导（供测试与外部诊断复用，不重复实现）。 */
export { eventIdFor }
