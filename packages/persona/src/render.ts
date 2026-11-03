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
 * ## 每条事件都带完整元信息（单条与多条同一形态）
 *
 * 历史上有过两次因「缺信息」而实际踩坑的事故，都记录在这里：
 *
 * 1. **只给 body**（最早的实现）：模型看不到 chatId/messageId，只能猜收件地址——
 *    实测先写 `feishu:default` 被白名单拒，再猜对群 id；话题消息不知道要带 root，
 *    于是飞书另开了一个新话题（用户完全看不到回复落在同一个讨论里）。
 * 2. **单条仍然只给 body**（第二次）：多事件时带来源，单条直通——而绝大多数事件是单条，
 *    等于把一个已修好的缺陷留在了主路径上。
 *
 * 所以现在的规则很简单：**任何事件、任何条数，都渲染成同一种块**，块里必带
 * 「谁 / 从哪来 / 何时 / 怎么回」四类信息（见 `renderEventBlock`）。
 * 模型的职责是判断与措辞，不该承担"猜一个 id"的风险。
 *
 * 元信息与正文分开呈现（`【元信息】` 行 vs `【正文】` 段）：前者是**协议事实**，
 * 后者是**对方说的话**。混写会让模型把 id 当内容复述，也让人读会话记录时分不清哪句是人说的。
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
  /**
   * 是否渲染元信息行。默认 true。
   *
   * 保留关闭开关只为一种场景：**对照排查**（怀疑元信息本身干扰模型时，可 A/B 对比）。
   * 生产不该关——关掉就回到了「让模型猜 id」的老路。
   */
  includeMeta?: boolean
}

/**
 * 把一批事件整编成一段模型输入。
 *
 * 单条与多条走**同一套**渲染：每条一个块（元信息 + 正文 + 回法），多条时块前加序号。
 */
export function renderBatch(events: PersonaEvent[], opts: RenderOptions = {}): string {
  if (events.length === 0) return ""

  const maxBody = opts.maxBodyChars ?? 4000
  const includeHint = opts.includeReplyHint !== false
  const includeMeta = opts.includeMeta !== false

  // 单条：直接给块，不编号、不写"本轮共 N 件"（模型只看一件事，包装是噪音）
  if (events.length === 1) {
    return renderEventBlock(events[0]!, { maxBody, includeHint, includeMeta })
  }

  const includeBrief = opts.includeDriverBrief !== false
  const lines: string[] = []
  if (includeBrief) {
    lines.push(`【这一段共 ${events.length} 件事】可能相关、也可能无关；你先判断：合并处理、分别处理，还是派出去查。`)
  } else {
    lines.push(`【本轮收到 ${events.length} 件事，按到达顺序】`)
  }
  events.forEach((e, i) => {
    lines.push("")
    lines.push(renderEventBlock(e, { maxBody, includeHint, includeMeta, index: i + 1 }))
  })
  return lines.join("\n")
}

/** 单条事件的块（单条/多条共用，保证两处形态一致）。 */
function renderEventBlock(
  e: PersonaEvent,
  opts: { maxBody: number; includeHint: boolean; includeMeta: boolean; index?: number },
): string {
  const lines: string[] = []
  const time = formatDateTime(e.receivedAt)
  const head = `${e.context.source}${time ? ` · ${time}` : ""}`
  const mark = opts.index !== undefined ? `${circleNum(opts.index)} ` : ""
  lines.push(`── ${mark}${head} ${"─".repeat(Math.max(2, 34 - head.length))}`)
  if (opts.includeMeta) {
    const meta = metaLine(e)
    if (meta) lines.push(`【元信息】${meta}`)
  }
  lines.push("【正文】")
  lines.push(truncate(e.body.trim(), opts.maxBody))
  if (opts.includeHint) {
    const hint = replyHint(e)
    if (hint) lines.push(`（${hint}）`)
  }
  return lines.join("\n")
}

/**
 * 元信息行：把「这是什么、从哪来、谁说的、怎么定位」一次给全。
 *
 * 只放**事实**（事件自带的值），不放建议——建议在 `replyHint` 里，两者职责不同：
 * 混在一起时模型分不清"这是消息里的内容"还是"系统的提示"。
 *
 * 空值不渲染（不写 `chatId=undefined`）：一行里塞一堆空字段会稀释真正的信息。
 */
function metaLine(e: PersonaEvent): string {
  const refs = e.context.refs
  const parts: string[] = [`类型=${e.kind}`, `渠道=${e.channel}`]
  parts.push(`事件ID=${e.id}`)
  if (refs?.chatId) parts.push(`群=${refs.chatId}`)
  if (refs?.messageId) parts.push(`消息=${refs.messageId}`)
  if (refs?.threadId) parts.push(`话题=${refs.threadId}`)
  if (refs?.senderName || refs?.senderId) {
    const who = refs.senderName?.trim() || "(未知)"
    parts.push(`发送者=${who}${refs.senderId ? `(${refs.senderId})` : ""}`)
  }
  if (refs?.taskId) parts.push(`任务=${refs.taskId}`)
  if (refs?.extra) {
    for (const [k, v] of Object.entries(refs.extra)) {
      if (v !== undefined && v !== "") parts.push(`${k}=${v}`)
    }
  }
  if (e.attachments?.length) parts.push(`附件=${e.attachments.map((a) => a.name).join("、")}`)
  if (e.threadKey) parts.push(`线程=${e.threadKey}`)
  return parts.join("  ")
}

/**
 * 出站提示：告诉模型"这件事该回到哪"。
 *
 * 由整编层统一生成而非塞进 `body`：`body` 是**事实**（对方说了什么），
 * 回法是**协议**。混写会让同一句话在不同上下文里出现不一致的提示，
 * 且模型可能抄错 id。集中生成 = 只有一处可能错。
 *
 * 这里给出的是**可直接复制执行的完整调用**（含真实 id）：模型不必再拼、
 * 也就不必再猜——猜错一次的代价是一次白发送 + 用户看不到回复。
 *
 * worker 回报没有 `chatId`（它不是某个群的消息）→ 返回 null，不出提示。
 */
export function replyHint(e: PersonaEvent): string | null {
  const refs = e.context.refs
  if (!refs?.chatId) return null
  // 话题消息（refs.threadId 非空，话题群形态）：回原话题 = send 带 root=触发消息 id
  //（飞书自动继承 thread_id 落回原话题，实测验证）；漏了 root 会另开新话题。
  // 注意 root 取 **messageId**（om_）而非 threadId（omt_）——飞书 reply 端点只认消息 id。
  if (refs.threadId && refs.messageId) {
    return `要回复这一件（必须回到原话题内，否则会另开新话题）：act(to="${e.channel}:${refs.chatId}", verb="send", args={text:"…", as_user:true, root:"${refs.messageId}"})`
  }
  const target = refs.messageId
    ? `to="${e.channel}:${refs.chatId}", verb="reply", args={text:"…", reply_to:"${refs.messageId}"}`
    : `to="${e.channel}:${refs.chatId}", verb="send", args={text:"…"}`
  return `要回复这一件：act(${target})`
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

/** 完整日期时间（只给 HH:MM 时，跨天事件看不出是哪天——定时/补投场景很常见）。 */
function formatDateTime(ms: number): string {
  if (!ms) return ""
  const d = new Date(ms)
  const p2 = (n: number) => String(n).padStart(2, "0")
  return `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())} ${p2(d.getHours())}:${p2(d.getMinutes())}`
}

function truncate(text: string, max: number): string {
  if (text.length <= max) return text
  return `${text.slice(0, max)}\n…（已截断，原 ${text.length} 字符）`
}
