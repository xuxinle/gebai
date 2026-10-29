/**
 * `PersonaLookup` 的标准实现：**静态人格定义 + 动态 driver 合流**。
 *
 * ## 为什么要有这一层
 *
 * 人格与 driver 有两个来源，语义不同但消费者只该看到一个视图：
 *
 * | 来源 | 谁写的 | 权威性 |
 * |---|---|---|
 * | `PersonaDef.drivers`（代码式定义） | 人 | **权威**（手写配置优先） |
 * | `DriverRegistry`（运行期动态创建） | 模型自己 | 可下线（模型想出来的分工） |
 *
 * 合流规则：**静态在前、动态在后；同 id 静态胜出**。
 * 这条规则不是随便定的——模型不能靠「创建一个同名 driver」来夺走人手写的分工
 * （那等于让模型改写部署配置）。`DriverRegistry.init()` 里已有同款保护，
 * 这里在视图层再守一道（防御性：两侧语义必须一致）。
 *
 * ## 两级订阅
 *
 * 1. **角色级**：有没有任何一个 driver 关注该事件
 * 2. **driver 级**：角色内具体哪些 driver 关注
 *
 * 第二级是必需的：一个角色可能有「盯告警」和「答支持群」两个分身。
 * 若只有角色级，支持群的消息会把盯告警的分身也唤醒（上下文互相污染、
 * 且长时间任务会挡住另一类事件）。
 *
 * ## 环境变量隔离
 *
 * `env(name)` 按 `PersonaDef.envVars` 声明白名单过滤（与渠道/事件源同口径）：
 * 人格定义只能读自己声明过的变量，防止「读告警的人格」顺手拿到模型密钥。
 */

import type { DriverDef, PersonaIdentity, PersonaLookup } from "./ports"
import type { PersonaDef } from "./persona"
import type { DriverRegistry } from "./driver"
import { coordsOf, matchesAny } from "./subscription"
import type { PersonaEvent } from "./event"

export interface PersonaCatalogOptions {
  /** 静态人格定义（id → def）。 */
  personas: ReadonlyMap<string, PersonaDef>
  /** 动态 driver 注册表（可缺省：只有静态定义时不需要）。 */
  drivers?: DriverRegistry
  /** 角色级并发额度缺省值（driver 未单独配置时继承）。 */
  defaultMaxConcurrent?: number
  log?: { info(msg: string): void; warn(msg: string): void }
}

/** 人格定义替换的差异（热加载日志与调度器同步用）。 */
export interface PersonaCatalogDiff {
  /** 新出现的人格 id。 */
  added: string[]
  /** 被删掉（定义文件已不存在）的人格 id。 */
  removed: string[]
  /** 定义内容有变化的人格 id（身份/关注面/订阅改过）。 */
  changed: string[]
  /** 新增的 `personaId/driverId`（静态 driver）。 */
  driversAdded: string[]
  /** 消失的 `personaId/driverId`。 */
  driversRemoved: string[]
}

/**
 * 两份人格定义是否相同（只比较**影响行为的字段**）。
 *
 * 不比 `description`：它只进面板不进提示词——改一句说明不该被当成“行为变了”而刷屏。
 */
function samePersona(a: PersonaDef, b: PersonaDef): boolean {
  if (a.name !== b.name || a.role !== b.role || a.identity !== b.identity) return false
  const da = a.drivers ?? []
  const db = b.drivers ?? []
  if (da.length !== db.length) return false
  const mapB = new Map(db.map((d) => [d.id, d]))
  for (const d of da) {
    const o = mapB.get(d.id)
    if (!o) return false
    if (d.focus !== o.focus) return false
    if (d.maxConcurrent !== o.maxConcurrent) return false
    if (d.subscriptions.length !== o.subscriptions.length) return false
    if (d.subscriptions.some((s, i) => s !== o.subscriptions[i])) return false
  }
  return true
}

/**
 * 人格目录：`PersonaLookup` 的默认实现。
 *
 * 只读视图——**不提供创建/删除**。人格定义来自代码文件（热加载走 discovery 重扫），
 * driver 的增删走 `DriverRegistry`。把写能力放这里会让「谁是权威」变模糊。
 */
export class PersonaCatalog implements PersonaLookup {
  private personas: ReadonlyMap<string, PersonaDef>
  private readonly registry?: DriverRegistry
  private readonly defaultMaxConcurrent: number

  constructor(opts: PersonaCatalogOptions) {
    this.personas = opts.personas
    this.registry = opts.drivers
    this.defaultMaxConcurrent = Math.max(1, opts.defaultMaxConcurrent ?? 2)
  }

  /**
   * 整体替换人格定义（**热加载専用**：由宿主的目录签名监测调用）。
   *
   * 为何是「整体替换」而不是 `set(id, def)`：热加载看到的是**一次扫描的完整快照**，
   * 逐条增删会让「文件被删」这种情形永远漏掉（没人去调 `delete`），于是删了的人格
   * 依旧活着——那是最难排查的一类“配置怎么改都不生效”。
   *
   * 为何不开放给模型：它是**写入**。人格定义是部署方的权威（与 `drivers[].subscriptions`
   * 同一信任面），运行时改写它等于让模型能改自己的身份与关注范围。
   *
   * 返回差异（供调用方打日志/同步调度器）：调用方靠它知道「哪些 driver 是新出现的」。
   */
  replace(personas: ReadonlyMap<string, PersonaDef>): PersonaCatalogDiff {
    const before = this.personas
    this.personas = personas
    const added: string[] = []
    const removed: string[] = []
    const changed: string[] = []
    for (const id of personas.keys()) if (!before.has(id)) added.push(id)
    for (const id of before.keys()) if (!personas.has(id)) removed.push(id)
    for (const [id, def] of personas) {
      const prev = before.get(id)
      if (prev && !samePersona(prev, def)) changed.push(id)
    }
    const driversBefore = new Set([...before.values()].flatMap((p) => (p.drivers ?? []).map((d) => `${p.id}/${d.id}`)))
    const driversAfter = new Set([...personas.values()].flatMap((p) => (p.drivers ?? []).map((d) => `${p.id}/${d.id}`)))
    return {
      added,
      removed,
      changed,
      driversAdded: [...driversAfter].filter((k) => !driversBefore.has(k)),
      driversRemoved: [...driversBefore].filter((k) => !driversAfter.has(k)),
    }
  }

  /** 全部人格 id（诊断/面板用）。 */
  ids(): string[] {
    return [...this.personas.keys()].sort()
  }

  /** 全部人格定义（诊断/面板用）。 */
  all(): PersonaDef[] {
    return this.ids().map((id) => this.personas.get(id)!).filter(Boolean)
  }

  get(personaId: string): PersonaIdentity | null {
    const p = this.personas.get(personaId)
    if (!p) return null
    // 只返回身份三件套：调用方（提示词构建）不该看到 drivers/description 等内部字段
    return { id: p.id, name: p.name, identity: p.identity, role: p.role }
  }

  /**
   * 该人格声明的子代理工具包。
   *
   * 与 `get()` 分开：`get()` 刻意只给身份三件套（它进系统提示词），
   * 而工具包要流到会话装工具与提示词写纪律两个地方——走一条显式的窄接口更清楚。
   */
  agents(personaId: string): string[] {
    return this.personas.get(personaId)?.agents ?? []
  }

  /**
   * 该人格的全部 driver（静态优先、动态在后；同 id 静态胜出）。
   *
   * 静态胜出为什么重要：模型可以通过 `driver_create` 工具造分身。若允许它用同名
   * 覆盖人手写的 `support`（关注面不同），人手写的部署配置就被模型悄悄改掉了。
   */
  drivers(personaId: string): DriverDef[] {
    const p = this.personas.get(personaId)
    const statics = p?.drivers ?? []
    const staticIds = new Set(statics.map((d) => d.id))
    const dynamic = (this.registry?.drivers(personaId) ?? []).filter((d) => !staticIds.has(d.id))
    return [...statics, ...dynamic]
  }

  /**
   * 找出关注该事件的角色与命中的 driver。
   *
   * 返回值里的 `user` 供会话归属用：人格跑在**部署方指定**的用户下
   * （当前实现取 `"admin"`，与本地模式默认用户一致；多用户人格归属需在
   * `PersonaDef` 增加 `owner` 字段，届时只改这里）。
   */
  subscribers(event: Pick<PersonaEvent, "kind" | "channel" | "context">): Array<{ personaId: string; user: string; driverIds: string[] }> {
    // 定向人格：只给它（且仍要过 driver 级订阅——定向不等于无视关注面）
    const explicit = (event as { personaId?: string }).personaId
    const candidates = explicit ? (this.personas.has(explicit) ? [explicit] : []) : this.ids()
    // 定向 driver（worker 回报回到派它的那个分身）
    const explicitDriver = (event as { driverId?: string }).driverId

    const coords = coordsOf(event)
    const out: Array<{ personaId: string; user: string; driverIds: string[] }> = []
    for (const personaId of candidates) {
      if (!this.personas.has(personaId)) continue
      const hit = this.drivers(personaId)
        .filter((d) => (explicitDriver ? d.id === explicitDriver : true))
        .filter((d) => matchesAny(d.subscriptions, coords))
        .map((d) => d.id)
      if (hit.length === 0) continue
      out.push({ personaId, user: this.userOf(personaId), driverIds: hit })
    }
    return out
  }

  maxConcurrent(personaId: string): number {
    void personaId
    return this.defaultMaxConcurrent
  }

  /**
   * 该人格的会话归属用户。
   *
   * 当前所有人格都跑在本地默认用户下。多用户场景需要 `PersonaDef.owner`，
   * 那时改这一处即可（`PersonaLookup.subscribers()` 已经带出 user 字段，
   * 上层无需改动）。
   */
  private userOf(personaId: string): string {
    void personaId
    return "admin"
  }
}
