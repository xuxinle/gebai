/**
 * Driver 管理工具：让角色**在运行时管理自己的分工**。
 *
 * ## 为什么这是"自我组织"的关键一步
 *
 * 静态配置只能覆盖部署方预见到的分工。真实工作里总有新情况：
 * 「这次专项排查我单开一个盯 TSDB 的活儿」「周报那个先停了吧」。
 * 让人去改配置文件、重启服务，等于把组织的反应速度绑在运维节奏上。
 *
 * 所以把「增删分工」做成工具，交给角色自己判断。
 *
 * ## 三个刻意的约束
 *
 * ### 1. 创建必须给 reason
 * 不是形式主义：`reason` 是事后回答"这条分工是怎么来的"的唯一线索。
 * 没有它，一个月后看到一堆 driver 无法判断哪些还有用。
 *
 * ### 2. 静态 driver 不能删
 * 手写配置是**权威**。工具只能动自己创建的（dynamic），否则模型一次误调用
 * 就能把部署方精心配置的分工干掉，而下次重启又复活——状态来源变得不可推理。
 *
 * ### 3. 创建不弹审批，但**留痕 + 可回滚**
 * 频繁审批会让"自我组织"退化成"每步都要问人"，那就失去意义了。
 * 但每条创建都写盘（`DriverRegistry` 持久化）+ 进日志 + 在状态里可见，
 * 事后可审、可删。风险由"可见 + 可回滚"承担，而不是由"事前逐次批准"承担。
 *
 * 这条取舍需要明确：如果你希望创建 driver 也要审批，把 `requiresApproval` 改成 true 即可
 * （见 `createDriverTools` 的 `opts.requireApproval`）。
 */

import { DriverRegistry, type CreateDriverInput } from "./driver"
import type { DriverDef } from "./ports"

/**
 * 工具定义（与 SDK `Tool` 结构兼容，但**不 import** `@gebai/sdk`）。
 *
 * 为什么不直接依赖 SDK 类型：`@gebai/persona` 是零运行时依赖的内核。
 * 结构兼容即可——宿主的 `ToolRegistry.register()` 按结构消费，无需适配器。
 * 若将来结构变化，宿主侧会立即类型报错（那是好事：说明契约变了两边都得知道）。
 */
export interface PersonaTool {
  name: string
  description: string
  parameters: { type: "object"; properties: Record<string, unknown>; required?: string[] }
  requiresApproval?: boolean | ((args: Record<string, unknown>, ctx: unknown) => boolean)
  execute(args: Record<string, unknown>, ctx: unknown): Promise<{ output: string; data?: unknown }>
}

/** 工具执行时需要的最小上下文（宿主把自己的上下文适配成它）。 */
export interface DriverToolContext {
  /** 当前角色 id（工具自动知道自己属于谁，不需要模型传）。 */
  personaId: string
  /** 当前会话 id（审计用：哪个会话创建了这条分工）。 */
  sessionId?: string
}

export interface CreateDriverToolsOptions {
  registry: DriverRegistry
  /**
   * 从宿主工具上下文里取「当前角色」。
   *
   * 为什么不让模型传 personaId：那等于让模型自己声明"我代表谁"，
   * 一次错误就能让 A 角色创建/删除 B 角色的分工。身份必须由宿主给定。
   */
  resolveContext(ctx: unknown): DriverToolContext | null
  /** 创建/删除是否需审批。默认 false（见文件头「三个刻意的约束」第 3 条）。 */
  requireApproval?: boolean
  /**
   * driver 变更后的回调（宿主用来同步运行时：`runtime.syncDriver`）。
   *
   * 必须有：注册表变了但调度器不知道，新 driver 就收不到事件——
   * 那会让「创建成功」变成一句谎话。
   */
  onChange?(change: { action: "created" | "removed"; personaId: string; driverId: string }): void
}

/**
 * 造两个工具：`driver_create` 与 `driver_retire`。
 *
 * 命名沿用仓库工具名规范（`[a-z][a-z0-9_]*`，蛇形）。
 */
export function createDriverTools(opts: CreateDriverToolsOptions): PersonaTool[] {
  const { registry } = opts
  const ctxOf = (raw: unknown): DriverToolContext | { error: string } => {
    const c = opts.resolveContext(raw)
    if (!c) return { error: "当前通道不支持 driver 管理（无法确定所属角色）" }
    return c
  }

  const create: PersonaTool = {
    name: "driver_create",
    description:
      "给自己开一个新的工作分身（driver）：它有自己的常驻会话与关注范围。用于「这类事以后长期归我管」或「这次专项我单开一个盯着」。\n" +
      "创建后**立即生效**：下一个符合 subscriptions 的事件就会投给它。\n" +
      "注意：id 创建后不可复用；同一个关注范围不要建两个（会造成重复处理与双份成本）。",
    parameters: {
      type: "object",
      properties: {
        id: {
          type: "string",
          description: "分身标识（小写字母/数字/下划线，如 tsdb_watch）。创建后不可改、不可复用。",
        },
        focus: { type: "string", description: "这个分身负责什么（一句话，会进它的提示词）" },
        subscriptions: {
          type: "array",
          items: { type: "string" },
          description:
            '关注的事件类型。写法：`*`（全部）、`message`（按类别）、`feishu`（按渠道）、`feishu:message`（两者都要）。不要留空——不关注任何事件的分身永远不会被唤醒。',
        },
        reason: { type: "string", description: "为什么需要这个分工（必填，事后审计靠它）" },
        max_concurrent: { type: "integer", description: "该分身同时最多跑几个会话（默认继承角色额度，1~16）" },
      },
      required: ["id", "focus", "subscriptions", "reason"],
    },
    ...(opts.requireApproval ? { requiresApproval: true } : {}),
    async execute(args, rawCtx) {
      const c = ctxOf(rawCtx)
      if ("error" in c) return { output: c.error }
      const subs = toStringArray(args.subscriptions)
      if (subs.length === 0) {
        return { output: "创建失败：subscriptions 不能为空——一个不关注任何事件的分身永远不会被唤醒。" }
      }
      const input: CreateDriverInput = {
        personaId: c.personaId,
        id: String(args.id ?? "").trim(),
        focus: String(args.focus ?? "").trim(),
        subscriptions: subs,
        reason: String(args.reason ?? "").trim(),
        ...(args.max_concurrent !== undefined ? { maxConcurrent: Number(args.max_concurrent) } : {}),
      }
      const r = await registry.create(input)
      if (!r.ok) return { output: `创建失败：${r.error}` }
      opts.onChange?.({ action: "created", personaId: c.personaId, driverId: r.def.id })
      return {
        output:
          `已创建分身「${r.def.id}」（${r.def.focus}）。\n` +
          `关注：${r.def.subscriptions.join("、")}——**下一个符合的事件就会投给它**。\n` +
          `原因已记录：${input.reason}\n` +
          `（若这条分工不再需要，用 driver_retire 撤掉它。）`,
        data: { id: r.def.id, focus: r.def.focus, subscriptions: r.def.subscriptions },
      }
    },
  }

  const retire: PersonaTool = {
    name: "driver_retire",
    description:
      "撤销一个自己开的分身（仅限运行时创建的）。静态配置里的分身不能撤——那是部署方的配置，改配置才是对的做法。\n" +
      "撤销后它不再接收任何事件；已排队的事件会被丢弃。",
    parameters: {
      type: "object",
      properties: {
        id: { type: "string", description: "要撤销的分身标识" },
        reason: { type: "string", description: "为什么撤销（建议填写，便于日后回看）" },
      },
      required: ["id"],
    },
    ...(opts.requireApproval ? { requiresApproval: true } : {}),
    async execute(args, rawCtx) {
      const c = ctxOf(rawCtx)
      if ("error" in c) return { output: c.error }
      const id = String(args.id ?? "").trim()
      if (!id) return { output: "撤销失败：缺少 id" }
      const rec = registry.get(c.personaId, id)
      if (!rec) return { output: `撤销失败：没有找到分身「${id}」` }
      const r = await registry.remove(c.personaId, id)
      if (!r.ok) return { output: `撤销失败：${r.error}` }
      opts.onChange?.({ action: "removed", personaId: c.personaId, driverId: id })
      const why = String(args.reason ?? "").trim()
      return {
        output: `已撤销分身「${id}」（原本负责：${rec.def.focus}）。它不再接收任何事件。${why ? `\n撤销原因：${why}` : ""}`,
        data: { id, focus: rec.def.focus },
      }
    },
  }

  return [create, retire]
}

/**
 * 列出当前角色全部分工的工具（只读，**永远可用**）。
 *
 * 为什么单独提供：模型需要先看见"我现在有哪些分身"才能判断该不该新建。
 * 没有这个，它会在每次需要时都建一个新的（重复分工）。
 */
export function createDriverListTool(opts: {
  registry: DriverRegistry
  resolveContext(ctx: unknown): DriverToolContext | null
}): PersonaTool {
  return {
    name: "driver_list",
    description: "查看自己当前的全部工作分身（含各自关注范围、来源、并发额度）。新建前先看这里，避免重复建。",
    parameters: { type: "object", properties: {} },
    async execute(_args, rawCtx) {
      const c = opts.resolveContext(rawCtx)
      if (!c) return { output: "当前通道不支持 driver 管理（无法确定所属角色）" }
      const defs = opts.registry.drivers(c.personaId)
      if (defs.length === 0) return { output: "当前没有工作分身。可以用 driver_create 开一个。" }
      const lines = defs.map((d) => {
        const rec = opts.registry.get(c.personaId, d.id)
        const origin = rec?.origin === "static" ? "静态（部署配置）" : "动态（自己创建）"
        const limit = d.maxConcurrent !== undefined ? `，并发 ${d.maxConcurrent}` : ""
        const why = rec?.reason ? `\n    原因：${rec.reason}` : ""
        return `- ${d.id}｜${d.focus}\n    关注：${d.subscriptions.join("、")}｜${origin}${limit}${why}`
      })
      return {
        output: `当前共 ${defs.length} 个工作分身：\n${lines.join("\n")}`,
        data: { drivers: defs.map((d) => ({ ...d, origin: opts.registry.get(c.personaId, d.id)?.origin })) },
      }
    },
  }
}

/** 把模型给的数组/字符串统一成字符串数组。 */
function toStringArray(v: unknown): string[] {
  if (Array.isArray(v)) return v.map((x) => String(x).trim()).filter(Boolean)
  if (typeof v === "string") {
    return v
      .split(/[,;\s]+/)
      .map((s) => s.trim())
      .filter(Boolean)
  }
  return []
}

/** 供宿主构造 DriverDef 时的便捷类型导出。 */
export type { DriverDef }
