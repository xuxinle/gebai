/**
 * 事件构造助手：把各驱动源的原始形态收敛成 `PersonaEvent`。
 *
 * 为什么集中在这里而不是各适配器自己拼：`id`（幂等键）与 `threadKey`（路由键）的正确性
 * 直接决定「会不会重复副作用」与「上下文会不会断裂」，这两条必须只有一份实现。
 */

import { type PersonaAttachment, type PersonaEvent, type PersonaEventContext, type PersonaEventKind, type PersonaEventRefs } from "./event"
import { renderBatch } from "./render"

/** 构造入参（各源填自己有的）。 */
export interface BuildEventInput {
  kind: PersonaEventKind
  personaId: string
  channel: string
  body: string
  source: string
  summary: string
  refs?: PersonaEventRefs
  threadKey?: string
  attachments?: PersonaAttachment[]
  /** 覆盖幂等键（缺省按 kind 推导）。 */
  id?: string
  /** 覆盖时间（缺省 now）。 */
  at?: number
}

/**
 * 幂等键推导规则（每源必有一条，缺了就重复副作用）：
 * - message：渠道消息 id（飞书 `om_xxx`）——飞书会重推同一 event_id，但消息 id 更稳定
 * - schedule：`{taskId}@{触发时刻}`——同一时刻只该触发一次
 * - webhook：投递方 id（缺失则退回内容哈希）
 * - desktop / idle：调用方显式给（无稳定 id 时按时刻 + 序号兜底）
 */
export function eventIdFor(input: BuildEventInput): string {
  if (input.id && input.id.trim()) return input.id.trim()
  const refs = input.refs
  switch (input.kind) {
    case "message": {
      const mid = refs?.messageId
      if (mid) return `msg:${mid}`
      // 无消息 id（极端情况）：用来源 + 正文哈希，避免同一消息被重复处理
      return `msg:${input.channel}:${hash32(`${refs?.chatId ?? ""}|${refs?.senderId ?? ""}|${input.body}`)}`
    }
    case "schedule": {
      const taskId = refs?.taskId ?? refs?.extra?.taskId ?? ""
      const at = input.at ?? Date.now()
      return `task:${taskId}@${at}`
    }
    case "webhook": {
      const wid = refs?.extra?.deliveryId
      if (wid) return `hook:${wid}`
      return `hook:${input.channel}:${hash32(input.body)}`
    }
    default:
      return `${input.kind}:${input.at ?? Date.now()}:${hash32(input.body).slice(0, 8)}`
  }
}

/** 构造统一事件（幂等键按 §eventIdFor 推导）。 */
export function buildEvent(input: BuildEventInput): PersonaEvent {
  const refs = input.refs
  const context: PersonaEventContext = {
    source: input.source,
    summary: input.summary,
    ...(refs ? { refs } : {}),
  }
  return {
    id: eventIdFor(input),
    kind: input.kind,
    personaId: input.personaId,
    channel: input.channel,
    ...(input.threadKey ? { threadKey: input.threadKey } : {}),
    context,
    body: input.body,
    ...(input.attachments?.length ? { attachments: input.attachments } : {}),
    receivedAt: input.at ?? Date.now(),
  }
}

/**
 * 入站上下文文本（单条事件的便捷入口）。
 *
 * **委托 `renderBatch`**：历史上这里曾是一份独立实现（自带一套「来源/发起者/出站纪律」），
 * 与 `render.ts` 的渲染并行存在。两份必然漂移——它们已经在出站纪律文案上分叉过：
 * 改了一处，另一处还是旧说法，模型收到两套矛盾的指令。所以现在只保留一份。
 *
 * 刻意**不含**"回复会自动发出"这类暗示——恰恰相反，它明确要求显式出站。
 * 这段文案是「人格沉默」的首要防线（模型必须知道没人替它转发）。
 */
export function renderEventContext(e: PersonaEvent, opts: { canReply: boolean } = { canReply: true }): string {
  if (opts.canReply) return renderBatch([e])
  // 无出站通路：正文照常给，但把出站提示换成「本轮无可用通道」。
  // 不能只改提示而不改正文——模型仍需要知道发生了什么。
  const body = renderBatch([e], { includeReplyHint: false })
  return `${body}\n（【出站纪律】本轮无可用出站通道；你的输出仅写入工作记录。）`
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
