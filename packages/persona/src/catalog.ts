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
import { DriverRegistry } from "./driver"
import { renderTemplate, resolveActorFields } from "./identity"
import { coordsOf, matchesAny } from "./subscription"
import type { PersonaEvent } from "./event"

export interface PersonaCatalogOptions {
  /** 静态人格定义（id → def）。 */
  personas: ReadonlyMap<string, PersonaDef>
  /** 动态 driver 注册表（可缺省：只有静态定义时不需要）。 */
  drivers?: DriverRegistry
  /** 角色级并发额度缺省值（driver 未单独配置时继承）。 */
  defaultMaxConcurrent?: number
  /**
   * 身份模板的环境变量读取（白名单由人格自己声明，`catalog` 只负责调用）。
   *
   * 为何注入而不是直读 process.env：测试要能精确断言「某个变量没配时身份如何降级」，
   * 多实例（多租户/多部署）也不该共享同一份环境。
   */
  envReader?: (name: string) => string | undefined
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
 *
 * 比 `agents`：它同时决定「会话装什么工具包」与「提示词写什么纪律」——改了却判“无变化”，
 * 热加载就不会同步，表现是「改了 agents 要重启才生效」（与热加载的承诺相抵）。
 *
 * 同 `envVars`：身份模板引用的变量改变了，渲染结果会变（同一个 identity 文本可能
 * 依赖不同的变量），因此它属于**行为**而不只是声明。
 */
function samePersona(a: PersonaDef, b: PersonaDef): boolean {
  if (a.name !== b.name || a.role !== b.role || a.identity !== b.identity) return false
  if (sameActor(a.actor, b.actor) === false) return false
  if ((a.agents ?? []).join("\u0000") !== (b.agents ?? []).join("\u0000")) return false
  if (envVarsSignature(a) !== envVarsSignature(b)) return false
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
    // 输出目标（部署注入）：改了它就要让新一轮重新读（否则改了目标还要重启）
    const ta = (d.outputs ?? []).map((t) => `${t.channel}:${t.address}#${t.thread ?? ""}:${t.default ? 1 : 0}`).join(",")
    const tb = (o.outputs ?? []).map((t) => `${t.channel}:${t.address}#${t.thread ?? ""}:${t.default ? 1 : 0}`).join(",")
    if (ta !== tb) return false
    // 定时工作面变了也是行为变了（改了时刻/指令必须让新一轮重新读，不能等重启）
    if (schedulesSignature(d.schedules) !== schedulesSignature(o.schedules)) return false
  }
  return true
}

/**
 * 定时声明的签名（catalog 变更检测用）。
 *
 * 为何不整个 `JSON.stringify` 比：组件顺序无关紧要，而逐字段拼能保证
 * “只改一处描述/时刻”也一定被当成行为变了（否则热加载不刷新面板与提示词）。
 */
function schedulesSignature(list?: import("./ports").DriverSchedule[]): string {
  return (list ?? [])
    .map((s) => [s.id, s.cron, s.timezone ?? "", s.misfire ?? "", s.description ?? "", s.prompt].join("\u0001"))
    .join("\u0000")
}

/** 身份（扮演者）是否相同：按**键值表**逐项比较（键顺序无意义）。 */
function sameActor(a: PersonaDef["actor"], b: PersonaDef["actor"]): boolean {
  const sig = (x: PersonaDef["actor"]): string => {
    if (!x) return ""
    const entries = Object.entries(x.fields ?? {}).sort(([k1], [k2]) => k1.localeCompare(k2))
    return JSON.stringify(entries)
  }
  return sig(a) === sig(b)
}

/** envVars 声明的签名（键集合变化即行为变化：它决定身份模板能不能渲染）。 */
function envVarsSignature(p: PersonaDef): string {
  return (p.envVars ?? []).map((v) => v.name).sort().join(",")
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
  private readonly envReader: (name: string) => string | undefined
  private readonly log?: { info(msg: string): void; warn(msg: string): void }

  constructor(opts: PersonaCatalogOptions) {
    this.personas = opts.personas
    this.registry = opts.drivers
    this.envReader = opts.envReader ?? ((n) => process.env[n])
    this.log = opts.log
    // 整数化：非法配置（如 1.5）经 Math.max(1, x) 仍非整数，会在 PersonaScheduler
    // 构造时抛错并连带整次投递失败——在这里就夹到合法形态（防御性，与 driver 侧校验同口径）
    const dmc = opts.defaultMaxConcurrent
    this.defaultMaxConcurrent = dmc !== undefined && Number.isInteger(dmc) && dmc >= 1 ? dmc : 2
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
    // 只返回身份四件套：调用方（提示词构建）不该看到 drivers/description 等内部字段。
    //
    // 身份文本走**模板渲染**（`${VAR}` 由部署环境提供）：这样“这个角色由谁扮演”
    // 是部署事实而不是代码事实；未声明/未配置的变量原样保留（一眼能看出缺什么）。
    const out: PersonaIdentity = {
      id: p.id,
      name: this.render(p.name),
      identity: this.render(p.identity),
      role: this.render(p.role),
    }
    if (p.actor) {
      // 身份字段是一组**自由键值**（键由部署方定：姓名/工号/部门/邮箱…），
      // 这里只做两件事：展开整组注入的 JSON、渲染 `${VAR}`。
      // 不解释键的含义——那是部署方的领域知识，不是框架的猜测。
      const { fields, problems } = resolveActorFields(p.actor.fields, this.envReader)
      if (problems.length) {
        for (const problem of problems) this.log?.warn(`[persona:${p.id}] ${problem}`)
      }
      if (Object.keys(fields).length) out.actor = { fields }
    }
    return out
  }

  /** 按该人格的 envVars 白名单渲染模板（未声明的变量读不到 → 保留原样）。 */
  private render(template: string): string {
    if (!template.includes("${")) return template
    return renderTemplate(template, this.envReader)
  }

  /**
   * 该人格的会话归属用户。
   *
   * 人格可以声明 `user`（支持 `${VAR}` 模板）：它在定义里表达的是“这个角色由谁扮演”，
   * 是一个**部署事实**（也是会话归属/审计归属）。未声明时退回 `"admin"`
   * ——本地形态的默认用户，与既有行为一致。
   */
  userOf(personaId: string): string {
    const p = this.personas.get(personaId)
    const declared = p && p.user !== undefined ? this.render(p.user).trim() : ""
    return declared && !declared.includes("${") ? declared : "admin"
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
    // 保证整数与 >=1：PersonaScheduler 构造对非整数直接抛错，而本值来自外部配置
    const n = this.defaultMaxConcurrent
    return Number.isInteger(n) && n >= 1 ? n : 2
  }
}
