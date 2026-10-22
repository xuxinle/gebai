/**
 * 插话信箱（Interjection Mailbox）：让**运行中的会话**接纳新事件。
 *
 * ## 为什么需要它
 *
 * 引擎的硬约束是「同一会话不能并发跑两个任务」（`engine.ts:1071` 直接抛错），
 * 人格层此前的做法是**排队等下一轮**。但那不符合"人怎么工作"：
 *
 * > 正在查一件事，同事又补了一句关键信息——人会把这句话听进去，
 * > 不会说"等我忙完这轮再听"。
 *
 * 排队 = 新信息**晚一个完整任务**才被看到，而那时结论可能已经错了。
 * 插入 = 新信息在**下一个轮次边界**就进入上下文。
 *
 * ## 两种投递路径（都要保命）
 *
 * ```
 * 事件到达 ──会话在跑？──否──▶ enqueue（下一轮跑）
 *              │是
 *              ├─ 策略 insert 且 runner 会 drain ──▶ 信箱（轮次边界注入，即时生效）
 *              └─ 否则 ─────────────────────────▶ enqueue（排队，不丢）
 * ```
 *
 * **关键在最后一条**：插话是"尽力而为"的高优先路径，不是可靠投递的替代品。
 * runner 若不支持 drain（如只跑完一轮就返回），信箱里的东西**必须在 run 结束时
 * 重新入队**，否则就是静默丢事件——那比不插话严重得多。
 *
 * ## 顺序保证
 *
 * 信箱是 FIFO：`drain()` 返回自上次调用以来到达的全部插话（按到达顺序）。
 * 这是"对话不串味"的最低要求——乱序插入会让模型看到自相矛盾的上下文。
 */

import type { PersonaEvent } from "./event"

/** 插话信箱（runner 侧只读视图）。 */
export interface PersonaMailbox {
  /**
   * 取出自上次调用以来新到的插话（FIFO，取走即清空）。
   *
   * 调用时机由 runner 决定：通常是**轮次边界**（模型停止调用工具、准备下一轮之前）——
   * 那正是"人听完一句话之后"的位置。
   */
  drain(): PersonaEvent[]
  /** 待取插话数（不消费）。 */
  size(): number
  /** 是否有待取插话。 */
  hasPending(): boolean
}

/** 插话信箱的写入端（调度器持有，不外暴露给 runner）。 */
export interface MailboxWriter {
  push(event: PersonaEvent): void
}

/** 信箱实现（写入端与读取端一体；调度器只把 `PersonaMailbox` 部分交给 runner）。 */
export class InterjectionMailbox implements PersonaMailbox, MailboxWriter {
  private items: PersonaEvent[] = []
  /** 累计推入数（诊断：这次运行期间收到过几次插话）。 */
  private totalPushed = 0
  /** 累计取走数。 */
  private totalDrained = 0

  push(event: PersonaEvent): void {
    // 同 id 不重复入箱（幂等兜底；runtime 层已按「事件×人格」判过，这里防同一事件
    // 在"插话 + 排队"两条路径上各走一次）
    if (this.items.some((e) => e.id === event.id)) return
    this.items.push(event)
    this.totalPushed++
  }

  drain(): PersonaEvent[] {
    if (this.items.length === 0) return []
    const out = this.items
    this.items = []
    this.totalDrained += out.length
    return out
  }

  size(): number {
    return this.items.length
  }

  hasPending(): boolean {
    return this.items.length > 0
  }

  /** 诊断快照（调度器状态展示用）。 */
  stats(): { pending: number; pushed: number; drained: number } {
    return { pending: this.items.length, pushed: this.totalPushed, drained: this.totalDrained }
  }

  /** 取回尚未被 drain 的插话（run 结束时调用：这些要重新入队，绝不能丢）。 */
  takeAll(): PersonaEvent[] {
    return this.drain()
  }
}
