/**
 * 动作执行器：把「模型的一次调用」翻译成「某个渠道的一次动作」，并在中间做**全部裁决**。
 *
 * ## 为什么裁决要集中在这里
 *
 * 渠道插件只回答「怎么把这件事做掉」；「能不能做、要不要问人」必须由核心决定。
 * 一旦让渠道自己判断，就等于把授权能力下放给插件——那是最危险的一类越权
 * （「读告警的插件」顺手往别的群发消息）。
 *
 * ## 裁决顺序（顺序本身就是安全性）
 *
 * ```
 * 解析 target → 渠道存在？ → 已启用？ → 能力已声明？ → 参数齐备？ → 目标在白名单？ → 执行
 *                            （前五步失败一律直接拒绝，不进审批）
 * ```
 *
 * **白名单越界直接拒绝而不是转审批**：发错群是信息泄露，不是「让用户决定」的事。
 * 让人去点一个「是否允许发给无关群」的按钮，等于把泄露的决定权交给一次误点。
 */

import type { ActOutcome, CapabilitySpec, ChannelContext, ObserveOutcome } from "./channel"
import { parseTarget } from "./channel"
import type { ChannelRegistry } from "./registry"

/** 解析结果（内部）：成功带三件套，失败带拒绝理由。 */
type ResolveResult =
  | { ok: true; value: { def: import("./channel").ChannelDef; ctx: ChannelContext; address: string } }
  | { ok: false; kind: "rejected"; reason: string }

/** 执行结果（成功/拒绝/失败三态分明，调用方不必解析文案）。 */
export type ExecResult<T> =
  | { ok: true; outcome: T }
  | { ok: false; reason: string; kind: "rejected" | "failed" }

/** 一次动作请求。 */
export interface ActRequest {
  target: string
  verb: string
  args: Record<string, unknown>
  idempotencyKey?: string
}

/** 一次观测请求。 */
export interface ObserveRequest {
  target: string
  what: string
  args: Record<string, unknown>
}

/**
 * 参数齐备校验：`required: true` 的键必须存在且非空串/非 null。
 *
 * 为何不交给渠道自己校验：模型漏传参数时若直接进渠道，渠道内部会以「路径不存在」
 * 「undefined」之类的远期错误暴露，模型无从学到「你漏了参数」。集中校验才能回传
 * 「缺哪个参数」这种可行动的反馈。
 */
function missingArgs(spec: CapabilitySpec | undefined, args: Record<string, unknown>): string[] {
  const out: string[] = []
  const decl = spec?.args
  if (!decl) return out
  for (const [key, arg] of Object.entries(decl)) {
    if (!arg.required) continue
    const v = args[key]
    if (v === undefined || v === null) out.push(key)
    else if (typeof v === "string" && !v.trim()) out.push(key)
  }
  return out
}

/** 参数清单（错误信息里告诉模型「有哪些可用参数」）。 */
function argNames(spec: CapabilitySpec | undefined): string {
  const decl = spec?.args
  if (!decl || !Object.keys(decl).length) return "（无）"
  return Object.entries(decl)
    .map(([k, a]) => `${k}:${a.type}${a.required ? "（必填）" : ""}`)
    .join("、")
}

/** 未知能力错误：列出可用能力，而不是只说「不支持」。 */
function unknownCapability(face: string, name: string, available: string[]): string {
  return available.length
    ? `渠道没有声明${face}「${name}」（可用：${available.join("、")}）`
    : `该渠道没有任何${face}能力（不能用于此类调用）`
}

/**
 * 动作执行器。
 *
 * 与 `ChannelRegistry` 分开的理由：注册表管「有哪些渠道、怎么加载」，
 * 执行器管「这一次调用能不能做、怎么做」。混在一起会让两者的测试互相纠缠。
 */
export class ActionExecutor {
  constructor(
    private readonly registry: ChannelRegistry,
    private readonly opts: { log?: { info(msg: string): void; warn(msg: string): void } } = {},
  ) {}

  /** 执行一次写动作。 */
  async act(req: ActRequest): Promise<ExecResult<ActOutcome>> {
    const resolved = this.resolve(req.target, "act")
    if (!resolved.ok) return resolved
    const { def, ctx, address } = resolved.value

    const specs = def.verbs ?? {}
    const spec = specs[req.verb]
    if (!spec) return { ok: false, kind: "rejected", reason: unknownCapability("动作", req.verb, Object.keys(specs)) }

    const missing = missingArgs(spec, req.args)
    if (missing.length) {
      return {
        ok: false,
        kind: "rejected",
        reason: `缺少必填参数：${missing.join("、")}（${req.verb} 的参数：${argNames(spec)}）`,
      }
    }

    const guard = await this.checkTargets(def, ctx, address, req.target)
    if (!guard.ok) return guard

    try {
      const outcome = await def.act!(
        { address, target: req.target, verb: req.verb, args: req.args, ...(req.idempotencyKey ? { idempotencyKey: req.idempotencyKey } : {}) },
        ctx,
      )
      this.opts.log?.info(`[channels] act ${req.target} verb=${req.verb} 成功${outcome.ref ? `（ref=${outcome.ref}）` : ""}`)
      return { ok: true, outcome }
    } catch (err) {
      // 渠道实现抛错 = 这一次没做成（与「被拒绝」性质不同：一个是策略，一个是故障）
      const reason = String((err as Error)?.message || err)
      this.opts.log?.warn(`[channels] act ${req.target} verb=${req.verb} 失败: ${reason}`)
      return { ok: false, kind: "failed", reason }
    }
  }

  /** 执行一次观测（读面，无副作用）。 */
  async observe(req: ObserveRequest): Promise<ExecResult<ObserveOutcome>> {
    const resolved = this.resolve(req.target, "observe")
    if (!resolved.ok) return resolved
    const { def, ctx, address } = resolved.value

    const specs = def.observes ?? {}
    const spec = specs[req.what]
    if (!spec) return { ok: false, kind: "rejected", reason: unknownCapability("观测", req.what, Object.keys(specs)) }

    const missing = missingArgs(spec, req.args)
    if (missing.length) {
      return {
        ok: false,
        kind: "rejected",
        reason: `缺少必填参数：${missing.join("、")}（${req.what} 的参数：${argNames(spec)}）`,
      }
    }

    const guard = await this.checkTargets(def, ctx, address, req.target)
    if (!guard.ok) return guard

    try {
      return { ok: true, outcome: await def.observe!({ address, target: req.target, what: req.what, args: req.args }, ctx) }
    } catch (err) {
      const reason = String((err as Error)?.message || err)
      this.opts.log?.warn(`[channels] observe ${req.target} what=${req.what} 失败: ${reason}`)
      return { ok: false, kind: "failed", reason }
    }
  }

  /**
   * 是否需要审批（供工具层 `requiresApproval` 调用）。
   *
   * 判定规则：
   * - 渠道/动词不存在 → `false`（执行期直接拒绝；问人白问，且审批卡会泄露参数）
   * - **目标不在白名单 → `false`**（fail-closed 直接拒：“是否允许发给无关群”不该让人点）
   * - `writes: true` → `true`
   *
   * 为何白名单要在这里同步判：工具层的审批发生在执行**之前**；如果这里说“需审批”，
   * 用户会看到一张注定被拒的卡（点批准也无用）。审批卡只该出现在“问了有意义”的场合。
   */
  needsApproval(req: ActRequest): boolean {
    const parsed = parseTarget(req.target)
    if ("error" in parsed) return false
    const def = this.registry.usable(parsed.channel)
    if (!def) return false
    const spec = def.verbs?.[req.verb]
    if (spec?.writes !== true) return false // 读类免审
    // 白名单是同步可得的信息（渠道可通过同步函数或已缓存清单给出）——越界则不问人
    const allowed = this.syncTargets(def)
    if (allowed && !allowed.includes(parsed.address)) return false
    return true
  }

  /**
   * 同步取白名单（同步函数可直接给；异步函数返回 `null` = 未知，此时按需审批处理）。
   *
   * 为何不在这里 await：`requiresApproval` 所在的工具契约允许异步，但引擎在审批点
   * 每工具都调用一次，await 一个可能抛错的 I/O 会把审批链路拖成故障点。渠道若需要
   * 异步白名单（如从远端拉），应在 `start()` 时缓存到闭包里再同步返回。
   */
  private syncTargets(def: import("./channel").ChannelDef): string[] | null | undefined {
    if (typeof def.targets !== "function") return undefined
    try {
      const r = def.targets(this.registry.context(def))
      if (r instanceof Promise) return undefined // 异步：未知，交执行期裁决
      return r
    } catch {
      return undefined
    }
  }

  /** 解析 target 并做「渠道存在 / 已启用」两步裁决。 */
  private resolve(target: string, face: "act" | "observe"): ResolveResult {
    const parsed = parseTarget(target)
    if ("error" in parsed) return { ok: false, kind: "rejected", reason: parsed.error }

    const def = this.registry.get(parsed.channel)
    if (!def) {
      const names = this.registry.list().map((c) => c.name)
      return {
        ok: false,
        kind: "rejected",
        reason: `未知渠道「${parsed.channel}」（已注册：${names.join("、") || "无"}）——用 channels_list 查看可用渠道与能力`,
      }
    }
    const usable = this.registry.usable(parsed.channel)
    if (!usable) {
      const err = this.registry.loadErrors()[parsed.channel]
      return {
        ok: false,
        kind: "rejected",
        reason: `渠道「${parsed.channel}」当前不可用（未启用或启动失败）${err ? `：${err}` : ""}`,
      }
    }
    void face
    return { ok: true, value: { def: usable, ctx: this.registry.context(usable), address: parsed.address } }
  }

  /**
   * 目标白名单校验（fail-closed）。
   *
   * `targets()` 返回 `null` = 不做地址级限制；返回数组 = 只允许清单内。
   * 函数抛错按**拒绝**处理（fail-safe：读不到白名单时不能放行）。
   */
  private async checkTargets(
    def: import("./channel").ChannelDef,
    ctx: ChannelContext,
    address: string,
    raw: string,
  ): Promise<{ ok: true } | { ok: false; kind: "rejected"; reason: string }> {
    if (typeof def.targets !== "function") return { ok: true }
    let allowed: string[] | null
    try {
      allowed = await def.targets(ctx)
    } catch (err) {
      return {
        ok: false,
        kind: "rejected",
        reason: `渠道「${def.name}」的目标白名单不可读（${String((err as Error)?.message || err)}）——拒绝执行（fail-closed）`,
      }
    }
    if (allowed === null) return { ok: true }
    if (allowed.includes(address)) return { ok: true }
    return {
      ok: false,
      kind: "rejected",
      reason: `目标「${raw}」不在渠道「${def.name}」的允许清单内（允许：${allowed.join("、") || "无"}）`,
    }
  }
}
