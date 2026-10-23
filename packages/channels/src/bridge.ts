/**
 * 渠道 → 调度内核的桥接：把「渠道收到的入站事件」变成「内核能处理的事件」。
 *
 * ## 为什么桥接在这边而不是在渠道里
 *
 * 渠道只回答「发生了什么」（`IngressEvent`：谁说了什么、回到哪）。
 * **幂等键推导、渠道标识归一、事件类别归一**全是核心规则——放渠道里意味着每个源
 * 重新实现一遍，必然分叉（子 Agent 的教训：`shApprovalFreeAllowed` 若各工具各写一遍，
 * 「只读」判定就会自相矛盾）。
 *
 * 所以这里做三件**只有一处**该做的事：
 *
 * | 职责 | 规则 |
 * |---|---|
 * | 幂等键 | 渠道显式 `id` 优先；缺失时按 `channel + kind + 内容哈希` 兜底（各源都会重投） |
 * | 渠道标识 | 统一用渠道定义名（**渠道不能自己宣布路由键**） |
 * | 事件类别 | 单条事件 `kind` 优先 → 渠道 `defaultKind` → `message` |
 *
 * ## 与 persona 的关系
 *
 * 本模块只依赖一个极小的结构接口（`PersonaDispatchPort`），刻意**不 import
 * `@gebai/persona`**：channels 是能力层，不该反向依赖调度内核（依赖方向一旦反了，
 * 「内核可独立单测」就不复存在）。宿主把 `PersonaRuntime` 按结构传进来即可。
 */

import type { EmitResult, IngressEvent, IngressKind } from "./channel"

/**
 * 调度内核的最小接口（宿主实现：`PersonaRuntime.dispatch` 结构兼容）。
 *
 * `status` 用字符串而非联合类型：内核的裁决结果集会演进（未来可能加状态），
 * 桥接层只关心「接管了没有」，不该跟着内核的枚举一起改。
 */
export interface PersonaDispatchPort {
  dispatch(event: {
    id: string
    kind: IngressKind
    personaId: string
    channel: string
    threadKey?: string
    context: { source: string; summary: string; refs?: Record<string, unknown> }
    body: string
    attachments?: Array<{ name: string; mime?: string; path?: string; data?: Uint8Array }>
    receivedAt: number
  }): Promise<{ status: string; reason?: string; personaId?: string; sessionId?: string; queued?: number }>
}

/** 稳定的 32 位哈希（FNV-1a）：仅用于兜底幂等键，不作安全用途。 */
export function hash32(text: string): string {
  let h = 0x811c9dc5
  for (let i = 0; i < text.length; i++) {
    h ^= text.charCodeAt(i)
    h = Math.imul(h, 0x01000193) >>> 0
  }
  return h.toString(16).padStart(8, "0")
}

/**
 * 入站事件的幂等键推导（**唯一实现**）。
 *
 * 为什么必须集中：每个驱动源都会重投（飞书 ACK 前断连重推、webhook 按自身策略重试、
 * 调度器重启重新触发）。重复事件 = 重复副作用（重复发消息、重复提交单据），
 * 是真实事故形态而非理论风险。
 *
 * 优先级：渠道显式 `id` → `{channel}:{kind}:{内容哈希}`。
 * 内容哈希兜底并不完美（同一个人把同一句话说两次会被去重），但比「无幂等」安全得多——
 * 前者偶发少处理一次，后者会重复执行副作用。**渠道应当总是给显式 `id`。**
 */
export function eventIdFor(channel: string, kind: IngressKind, ev: IngressEvent): string {
  const explicit = ev.id?.trim()
  if (explicit) return explicit
  const refs = ev.refs
  const seed = `${refs?.chatId ?? ""}|${refs?.senderId ?? ""}|${refs?.messageId ?? ""}|${ev.summary}|${ev.body}`
  return `${channel}:${kind}:${hash32(seed)}`
}

/** 投递计数（诊断：`unrouted` 持续增长 = 订阅配置有问题）。 */
export interface InputCounters {
  emitted: number
  accepted: number
  unrouted: number
  duplicate: number
  overflow: number
  unavailable: number
}

export interface ChannelInputBridgeOptions {
  /** 调度内核（`PersonaRuntime` 结构兼容）。 */
  dispatch: PersonaDispatchPort
  log?: { info(msg: string): void; warn(msg: string): void }
}

/**
 * 输入桥接器：把一个渠道的 `IngressEvent` 归一后交给调度内核。
 *
 * 一个渠道一个实例（由注册表在启动该渠道时创建），这样渠道名天然可信——
 * 渠道拿到的 sink 已经绑定了自己的名字，**无从伪造路由键**。
 *
 * 用法（由 `ChannelRegistry.attachDispatcher` 自动完成，宿主无需手写）：
 *
 * ```ts
 * const bridge = new ChannelInputBridge({ dispatch: personaRuntime })
 * channels.attachDispatcher(bridge)   // 之后 init() 时每个渠道拿到绑自己名字的 sink
 * ```
 */
export class ChannelInputBridge {
  private readonly counters = new Map<string, InputCounters>()

  constructor(private readonly opts: ChannelInputBridgeOptions) {}

  /** 某渠道的投递计数。 */
  stats(channel: string): InputCounters {
    return this.counters.get(channel) ?? { emitted: 0, accepted: 0, unrouted: 0, duplicate: 0, overflow: 0, unavailable: 0 }
  }

  /** 全部渠道的投递计数。 */
  allStats(): Record<string, InputCounters> {
    return Object.fromEntries(this.counters)
  }

  private bump(channel: string, key: keyof InputCounters): void {
    const c = this.counters.get(channel) ?? { emitted: 0, accepted: 0, unrouted: 0, duplicate: 0, overflow: 0, unavailable: 0 }
    c[key]++
    this.counters.set(channel, c)
  }

  /**
   * 执行一次投递（由注册表给每个渠道构造绑定名字的 sink 时调用）。
   *
   * 公开但**不意味着渠道能直接拿到**：渠道只拿到 `{ emit(ev) }` 形状的 sink，
   * 里面已绑好渠道名。直接调 `deliver` 需要知道注册表内部约定，不属于渠道契约。
   */
  async deliver(channelName: string, defaultKind: IngressKind | undefined, ev: IngressEvent): Promise<EmitResult> {
    this.bump(channelName, "emitted")
    const kind: IngressKind = ev.kind ?? defaultKind ?? "message"
    const id = eventIdFor(channelName, kind, ev)
    let result: { status: string; reason?: string; personaId?: string; sessionId?: string; queued?: number }
    try {
      result = await this.opts.dispatch.dispatch({
        id,
        kind,
        // 空串 = 广播给所有关注者（归属是部署配置（订阅）的事，不是渠道的事）。
        // **硬写空串而不是透传**：渠道即使往事件里塞了 personaId（类型上不可能，但运行时能塞）
        // 也不能指定归属——否则一个读告警的插件就能把事件定向给运维人格。
        personaId: "",
        // 渠道名**由注册表给定**（渠道无法伪造路由键）；同样硬写而非取事件里的值
        channel: channelName,
        threadKey: ev.refs?.chatId ? `${channelName}:${ev.refs.chatId}` : undefined,
        context: {
          source: ev.source ?? channelName,
          summary: ev.summary,
          ...(ev.refs ? { refs: ev.refs as unknown as Record<string, unknown> } : {}),
        },
        body: ev.body,
        ...(ev.attachments?.length ? { attachments: ev.attachments } : {}),
        receivedAt: ev.at ?? Date.now(),
      })
    } catch (err) {
      // 内核抛错（未启动/内部故障）：如实转成 unavailable，渠道据此记日志而不是崩掉轮询循环
      this.bump(channelName, "unavailable")
      const reason = String((err as Error)?.message || err)
      this.opts.log?.warn(`[channels] ${channelName} 投递失败: ${reason}`)
      return { status: "unavailable", reason }
    }

    // 内核裁决结果 → 渠道可见的五态（与 persona 的 dispatch 语义一一对应）
    const s = result.status
    const key: keyof InputCounters =
      s === "accepted" ? "accepted" : s === "unrouted" ? "unrouted" : s === "duplicate" ? "duplicate" : s === "overflow" ? "overflow" : "unavailable"
    this.bump(channelName, key)
    if (s === "unrouted") {
      this.opts.log?.info(`[channels] ${channelName} 的事件无人关注（unrouted）：${ev.summary}`)
    }
    return {
      status: key === "accepted" ? "accepted" : key === "duplicate" ? "duplicate" : key === "overflow" ? "overflow" : key === "unrouted" ? "unrouted" : "unavailable",
      ...(result.reason ? { reason: result.reason } : {}),
      ...(result.personaId ? { personaId: result.personaId } : {}),
      ...(result.sessionId ? { sessionId: result.sessionId } : {}),
      ...(result.queued !== undefined ? { queued: result.queued } : {}),
    } as EmitResult
  }
}
