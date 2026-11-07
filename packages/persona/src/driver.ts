/**
 * Driver 注册表：**静态定义 + 模型运行时创建**。
 *
 * ## 两种来源，同一个池子
 *
 * | 来源 | 谁决定 | 生命周期 |
 * |---|---|---|
 * | **静态定义**（角色配置里的 `drivers[]`） | 部署方 | 随角色定义，配置改了就变 |
 * | **动态创建**（模型调用工具） | 角色自己 | 创建后常驻，除非显式下线 |
 *
 * 为什么两种都要：
 * - 纯静态：部署方得预先想清楚"这个人有哪些分工"。可预测，但对新情况反应慢。
 * - 纯动态：每次都要现场造，重启即失，角色没有稳定的工作结构。
 *
 * 混合才对：**常用分工写死**（告警、支持），**临时分工现场开**
 * （"这次专项排查我单开一个 tsdb 的活儿盯着"）。
 *
 * ## 动态 driver 必须落盘
 *
 * 模型创建的分工如果只在内存里，进程一重启就没了——而"分工"是角色的工作结构，
 * 不是一次性的临时状态。所以注册表带 `persist` 钩子：任何创建都写盘。
 * 这也让"为什么它有这个 driver"事后可查（盘上有记录）。
 *
 * ## 与订阅广播的关系
 *
 * 动态创建的 driver **立即**参与订阅匹配：下一个符合 `subscriptions` 的事件就会投给它。
 * 这是"自我组织"的关键——角色不需要等人改配置才能开始按新分工工作。
 */

import { validateSubscription } from "./subscription"
import type { DriverDef } from "./ports"

/** driver 创建来源（审计用：事后能区分"人配的"与"自己想出来的"）。 */
export type DriverOrigin = "static" | "dynamic"

/** 注册表里的一条记录。 */
export interface DriverRecord {
  def: DriverDef
  origin: DriverOrigin
  createdAt: number
  /** 动态创建时：为什么建它（模型给的理由，审计价值最高）。 */
  reason?: string
}

/** 持久化钩子（由宿主注入：写盘/写库）。 */
export interface DriverPersistence {
  /** 读出全部动态 driver（进程启动时调用一次）。 */
  load(): Promise<Array<{ personaId: string; record: DriverRecord }>>
  /** 新增/更新一条动态 driver。 */
  save(personaId: string, record: DriverRecord): Promise<void>
  /** 移除（下线）。 */
  remove(personaId: string, driverId: string): Promise<void>
}

export interface DriverRegistryOptions {
  /** 静态定义（角色配置里的 drivers）。 */
  statics?: Array<{ personaId: string; def: DriverDef }>
  /** 动态 driver 的持久化（缺省只存内存——重启丢失，仅适用于测试）。 */
  persistence?: DriverPersistence
  log?: { info(msg: string): void; warn(msg: string): void }
  now?: () => number
}

/** 创建 driver 的入参（模型经工具调用时提供）。 */
export interface CreateDriverInput {
  personaId: string
  id: string
  focus: string
  subscriptions: string[]
  /** 为什么需要这个分工（**必填**：这是审计与防滥用的关键信息）。 */
  reason: string
  maxConcurrent?: number
}

const DRIVER_ID_RE = /^[a-z0-9_]+$/
const MAX_DYNAMIC_PER_PERSONA = 16

/** `personaId/driverId` 的展示形式（日志用）。 */
const fmt = (x: { personaId: string; driverId: string }): string => `${x.personaId}/${x.driverId}`

/** 两个 driver 定义是否等价（只比影响行为的字段：输出目标变了也算变）。 */
function sameDriverDef(a: DriverDef, b: DriverDef): boolean {
  if (a.focus !== b.focus) return false
  if (a.maxConcurrent !== b.maxConcurrent) return false
  if (a.subscriptions.length !== b.subscriptions.length) return false
  if (a.subscriptions.some((s, i) => s !== b.subscriptions[i])) return false
  if (a.sessionKeyOf !== b.sessionKeyOf) return false
  const sig = (t?: DriverDef["outputs"]): string =>
    (t ?? []).map((o) => `${o.channel}:${o.address}#${o.thread ?? ""}:${o.default ? 1 : 0}`).join(",")
  return sig(a.outputs) === sig(b.outputs)
}

/**
 * Driver 注册表。
 *
 * 并发安全：Node/Bun 单线程事件循环内，同步方法天然串行；
 * 异步持久化用「先改内存、再 await 落盘」——落盘失败只记日志不回滚
 * （回滚会让"已创建却报错"更混乱；下次 load 会以盘上状态为准）。
 */
export class DriverRegistry {
  /** personaId → (driverId → record) */
  private byPersona = new Map<string, Map<string, DriverRecord>>()
  private readonly persistence?: DriverPersistence
  private readonly log?: DriverRegistryOptions["log"]
  private readonly now: () => number
  private loaded = false

  constructor(opts: DriverRegistryOptions = {}) {
    this.persistence = opts.persistence
    this.log = opts.log
    this.now = opts.now ?? Date.now
    for (const s of opts.statics ?? []) this.putStatic(s.personaId, s.def)
  }

  /** 启动时加载动态 driver（幂等；失败只记日志，不阻断启动）。 */
  async init(): Promise<void> {
    if (this.loaded) return
    this.loaded = true
    if (!this.persistence) return
    try {
      for (const { personaId, record } of await this.persistence.load()) {
        const map = this.byPersona.get(personaId) ?? new Map()
        // 静态定义优先：盘上的同 id 记录不覆盖手写配置（手写才是权威）
        if (map.has(record.def.id)) continue
        map.set(record.def.id, record)
        this.byPersona.set(personaId, map)
      }
    } catch (err) {
      this.log?.warn(`[persona:drivers] 加载动态 driver 失败（已跳过）: ${String((err as Error)?.message || err)}`)
    }
  }

  /** 该角色的全部 driver（静态在前，动态按创建时间在后）。 */
  drivers(personaId: string): DriverDef[] {
    const map = this.byPersona.get(personaId)
    if (!map) return []
    return [...map.values()]
      .sort((a, b) => {
        if (a.origin !== b.origin) return a.origin === "static" ? -1 : 1
        return a.createdAt - b.createdAt
      })
      .map((r) => r.def)
  }

  /** 取单个 driver。 */
  get(personaId: string, driverId: string): DriverRecord | null {
    return this.byPersona.get(personaId)?.get(driverId) ?? null
  }

  /** 静态定义（角色配置加载时调用；同名覆盖静态，不覆盖动态的既有状态）。 */
  putStatic(personaId: string, def: DriverDef): void {
    const map = this.byPersona.get(personaId) ?? new Map<string, DriverRecord>()
    map.set(def.id, { def, origin: "static", createdAt: this.now() })
    this.byPersona.set(personaId, map)
  }

  /**
   * **整体替换静态定义**（热加载用）：差量下线被删的、改写变了的。
   *
   * 为何不能只靠 `putStatic` 逐条覆盖：它只能“加/改”，删不掉。
   * 人格定义被修改（删了一个 driver、改了订阅或 focus）后，旧条目会永久留在注册表里：
   * `driver_list` 会列出一个不存在的分身、新建同名动态分身被「已存在」拒绝、
   * 状态面板也会显示已删除的分工。
   *
   * 动态创建的条目**保留**（它们是模型自己的工位结构，不归静态配置管）。
   *
   * @returns 实际发生的变化（供调用方同步运行时调度器）
   */
  syncStatics(statics: ReadonlyArray<{ personaId: string; def: DriverDef }>): {
    added: Array<{ personaId: string; driverId: string }>
    removed: Array<{ personaId: string; driverId: string }>
    changed: Array<{ personaId: string; driverId: string }>
  } {
    const added: Array<{ personaId: string; driverId: string }> = []
    const removed: Array<{ personaId: string; driverId: string }> = []
    const changed: Array<{ personaId: string; driverId: string }> = []
    const wanted = new Map<string, Map<string, DriverDef>>()
    for (const s of statics) {
      const m = wanted.get(s.personaId) ?? new Map<string, DriverDef>()
      m.set(s.def.id, s.def)
      wanted.set(s.personaId, m)
    }
    // 下线：旧静态条目已不在新定义里
    for (const [personaId, map] of this.byPersona) {
      const want = wanted.get(personaId)
      for (const [driverId, rec] of [...map]) {
        if (rec.origin !== "static") continue
        if (!want?.has(driverId)) {
          map.delete(driverId)
          removed.push({ personaId, driverId })
        }
      }
    }
    // 上线/改写
    for (const [personaId, map] of wanted) {
      for (const [driverId, def] of map) {
        const rec = this.byPersona.get(personaId)?.get(driverId)
        if (rec?.origin === "static") {
          if (sameDriverDef(rec.def, def)) continue
          rec.def = def
          changed.push({ personaId, driverId })
          continue
        }
        // 动态条目同名：静态胜出（手写配置才是权威）
        this.putStatic(personaId, def)
        added.push({ personaId, driverId })
      }
    }
    if (added.length || removed.length || changed.length) {
      this.log?.info(
        `[persona:drivers] 静态定义已同步：新增 ${added.map(fmt).join("、") || "-"}；` +
          `下线 ${removed.map(fmt).join("、") || "-"}；修改 ${changed.map(fmt).join("、") || "-"}`,
      )
    }
    return { added, removed, changed }
  }

  /**
   * 动态创建 driver（模型经工具调用）。
   *
   * 校验从严（与审批规则同一取向：**写错就报错，不静默降级**）——
   * 静默接受一个 `subscriptions` 写错的 driver，结果是它永远收不到事件，
   * 而现象是"我说要盯告警了但它没反应"，极难排查。
   */
  async create(input: CreateDriverInput): Promise<{ ok: true; def: DriverDef } | { ok: false; error: string }> {
    const err = validateCreate(input, this.byPersona.get(input.personaId))
    if (err) return { ok: false, error: err }

    const def: DriverDef = {
      id: input.id,
      focus: input.focus.trim(),
      subscriptions: input.subscriptions.map((s) => s.trim()).filter(Boolean),
      ...(input.maxConcurrent !== undefined ? { maxConcurrent: input.maxConcurrent } : {}),
    }
    const record: DriverRecord = { def, origin: "dynamic", createdAt: this.now(), reason: input.reason.trim() }
    const map = this.byPersona.get(input.personaId) ?? new Map<string, DriverRecord>()
    map.set(def.id, record)
    this.byPersona.set(input.personaId, map)

    if (this.persistence) {
      try {
        await this.persistence.save(input.personaId, record)
      } catch (e) {
        this.log?.warn(`[persona:drivers] 持久化 driver ${input.personaId}/${def.id} 失败: ${String((e as Error)?.message || e)}`)
      }
    }
    this.log?.info(
      `[persona:drivers] 动态创建 ${input.personaId}/${def.id}（订阅 ${def.subscriptions.join(",")}）：${record.reason}`,
    )
    return { ok: true, def }
  }

  /** 下线一个 driver。静态的不能下线（改配置才是对的做法）。 */
  async remove(personaId: string, driverId: string): Promise<{ ok: true } | { ok: false; error: string }> {
    const map = this.byPersona.get(personaId)
    const rec = map?.get(driverId)
    if (!rec) return { ok: false, error: `driver ${driverId} 不存在` }
    if (rec.origin === "static") {
      return { ok: false, error: `driver ${driverId} 是静态定义（写在角色配置里），不能运行时下线；请改配置` }
    }
    map!.delete(driverId)
    if (this.persistence) {
      try {
        await this.persistence.remove(personaId, driverId)
      } catch (e) {
        this.log?.warn(`[persona:drivers] 移除持久化 driver 失败: ${String((e as Error)?.message || e)}`)
      }
    }
    this.log?.info(`[persona:drivers] 已下线 ${personaId}/${driverId}`)
    return { ok: true }
  }
}

/**
 * 创建入参校验。
 * 两类典型错误都在这里挡住：
 * 1. `subscriptions` 写成不存在的类型 → 该 driver 永远收不到事件（静默失效）
 * 2. `id` 与已有 driver 冲突 → 覆盖掉一个正在工作的分工（意外改变行为）
 */
export function validateCreate(input: CreateDriverInput, existing: Map<string, DriverRecord> | undefined): string | null {
  if (!input.personaId?.trim()) return "缺少 personaId"
  if (!input.id?.trim()) return "缺少 driver id"
  if (!DRIVER_ID_RE.test(input.id.trim())) return `driver id 必须是 [a-z0-9_]+（当前：${input.id}）`
  if (existing?.has(input.id.trim())) {
    const rec = existing.get(input.id.trim())!
    return `driver「${input.id}」已存在（${rec.origin === "static" ? "静态定义" : "此前动态创建"}，focus=${rec.def.focus}）——换个 id 或先下线它`
  }
  if (!input.focus?.trim()) return "缺少 focus（这个 driver 负责什么，一句话）"
  if (input.focus.trim().length > 200) return "focus 过长（上限 200 字符）"
  if (!input.reason?.trim()) return "缺少 reason（为什么需要这个分工）——这是审计必需信息"
  if (input.reason.trim().length > 500) return "reason 过长（上限 500 字符）"
  const subs = (input.subscriptions ?? []).map((s) => s.trim()).filter(Boolean)
  if (subs.length === 0) {
    return "subscriptions 不能为空——一个不关注任何事件的 driver 永远不会被唤醒（那样它没有存在意义）"
  }
  if (subs.length > 12) return "subscriptions 过多（上限 12 条）"
  // 订阅语法复用 subscription.ts 的唯一实现（防两处分叉）。
  // 三段式（channel:chatId:kind）对动态 driver 同样开放——与静态定义（persona.ts
  // 校验同一函数）口径一致；此前这里另有一份只允许两段的本地副本，同一套订阅
  // 两套规则已实际分叉（静态可三段、动态不可），收敛于此。
  for (const s of subs) {
    const bad = validateSubscription(s)
    if (bad) return bad
  }
  const dynamicCount = existing ? [...existing.values()].filter((r) => r.origin === "dynamic").length : 0
  if (dynamicCount >= MAX_DYNAMIC_PER_PERSONA) {
    return `动态 driver 已达上限 ${MAX_DYNAMIC_PER_PERSONA} 个，请先下线不用的`
  }
  if (input.maxConcurrent !== undefined) {
    if (!Number.isInteger(input.maxConcurrent) || input.maxConcurrent < 1 || input.maxConcurrent > 16) {
      return `maxConcurrent 必须是 1~16 的整数（当前：${String(input.maxConcurrent)}）`
    }
  }
  return null
}
