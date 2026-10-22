/**
 * 事件整编：把到达 driver 的多件事组织成一段连贯输入。
 *
 * ## driver 眼里有两类输入，但它们同构
 *
 * | 来源 | 例子 |
 * |---|---|
 * | 外部事件 | 飞书群消息、定时任务触发、webhook |
 * | worker 回报 | 「查日志」这个活干完了，结论是… |
 *
 * 两者都走**同一套整编**——因为对 driver 来说都是"新信息到了，你判断怎么处理"。
 * 统一形态才能让 driver 用同一套逻辑排优先级，而不是学两套。
 *
 * ## 为什么单事件要直通
 *
 * 单条时直接返回 `body`（不包装编号与回法提示）。这是零回归保证：
 * 没有插入/合流发生时，模型看到的东西与"没有整编层"时逐字节一致。
 */

import type { PersonaEvent } from "./event"

export interface RenderOptions {
  /** 单条正文字符上限（防一件事挤掉其余）。默认 4000。 */
  maxBodyChars?: number
  /** 是否附出站提示。默认 true。 */
  includeReplyHint?: boolean
  /**
   * 是否在开头说明"你是 driver"。
   *
   * 仅在**多事件**时加：单事件时模型只看一件事，不需要额外的角色说明
   * （身份与职责已在系统提示词里，重复只会浪费 token）。
   */
  includeDriverBrief?: boolean
}

/**
 * 把一批事件整编成一段模型输入。
 *
 * 单事件直通；多事件按到达顺序编号，并保留每件事的来源与回法。
 */
export function renderBatch(events: PersonaEvent[], opts: RenderOptions = {}): string {
  if (events.length === 0) return ""
  if (events.length === 1) return events[0].body

  const maxBody = opts.maxBodyChars ?? 4000
  const includeHint = opts.includeReplyHint !== false
  const includeBrief = opts.includeDriverBrief !== false
  const lines: string[] = []

  if (includeBrief) {
    lines.push(`【这一段共 ${events.length} 件事】可能相关、也可能无关；你先判断：合并处理、分别处理，还是派出去查。`)
  } else {
    lines.push(`【本轮收到 ${events.length} 件事，按到达顺序】`)
  }

  events.forEach((e, i) => {
    const n = i + 1
    const time = formatTime(e.receivedAt)
    const head = `${e.context.source}${time ? ` · ${time}` : ""}`
    lines.push("")
    lines.push(`── ${circleNum(n)} ${head} ${"─".repeat(Math.max(2, 34 - head.length))}`)
    lines.push(truncate(e.body.trim(), maxBody))
    if (includeHint) {
      const hint = replyHint(e)
      if (hint) lines.push(`（${hint}）`)
    }
  })

  return lines.join("\n")
}

/**
 * 出站提示：告诉模型"这件事该回到哪"。
 *
 * 由整编层统一生成而非塞进 `body`：`body` 是**事实**（对方说了什么），
 * 回法是**协议**。混写会让同一句话在不同上下文里出现不一致的提示，
 * 且模型可能抄错 id。集中生成 = 只有一处可能错。
 *
 * worker 回报没有 `chatId`（它不是某个群的消息）→ 返回 null，不出提示。
 */
export function replyHint(e: PersonaEvent): string | null {
  const refs = e.context.refs
  if (!refs?.chatId) return null
  const target = refs.messageId ? `chat_id="${refs.chatId}", reply_to="${refs.messageId}"` : `chat_id="${refs.chatId}"`
  return `要回复这一件：send_message(${target}, text="...")`
}

/** 一句话摘要（状态展示用）。 */
export function summarizeBatch(events: PersonaEvent[]): string {
  if (events.length === 0) return "无事件"
  if (events.length === 1) return events[0].context.summary
  return `${events.length} 件事：${events.map((e) => e.context.summary).join("；")}`
}

function circleNum(n: number): string {
  const marks = ["①", "②", "③", "④", "⑤", "⑥", "⑦", "⑧", "⑨", "⑩", "⑪", "⑫", "⑬", "⑭", "⑮", "⑯", "⑰", "⑱", "⑲", "⑳"]
  return marks[n - 1] ?? `(${n})`
}

function formatTime(ms: number): string {
  if (!ms) return ""
  const d = new Date(ms)
  return `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`
}

function truncate(text: string, max: number): string {
  if (text.length <= max) return text
  return `${text.slice(0, max)}\n…（已截断，原 ${text.length} 字符）`
}
