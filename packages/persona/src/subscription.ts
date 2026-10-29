/**
 * 订阅匹配：决定「哪些人格关注这个事件」。
 *
 * ## 为什么单独成模块
 *
 * 订阅规则的语义（通配/优先级/空订阅含义）是**整套广播模型的地基**，
 * 放在各个人格注册表实现里各写一遍，必然分叉——典型后果是
 * 「A 家注册表支持通配、B 家不支持」，同一条事件在两处走不同路径，
 * 而现象是「某个群偶尔没反应」，极难排查。
 *
 * ## 订阅语义（封闭集合，与审批规则同一设计取向）
 *
 * | 写法 | 含义 |
 * |---|---|
 * | `"*"` | 关注**全部**事件 |
 * | `"message"` | 关注该 `kind` 的全部事件 |
 * | `"feishu"` | 关注该 `channel` 的全部事件 |
 * | `"feishu:message"` | 同时限定 channel 与 kind（**且**关系） |
 * | `"message:feishu"` | 同上（顺序无关） |
 * | `"feishu:oc_xxx:message"` | 三段式：channel + **群/会话 id** + kind（且关系；
 *                  顺序仍无关——但 id 段以 `oc_`/数字开头时可辨识，段序自由不影响语义） |
 *
 * **刻意不支持正则/前缀通配**（如 `feishu*`）：一条写错的正则能让某人格
 * 收到全部事件（噪音淹没）或全部收不到（静默失效），两者都难排查。
 *
 * ## 三段式为什么现在才需要
 *
 * channel:kind 只能区分「哪个渠道的哪类事件」；同一渠道里不同群归不同人格
 * （如 A 群给算力审核员、B 群给值班助手）需要第三维。而「人格共性放代码、
 * 具体群放 env」（`GEBAI_PERSONA_<ID>_CHATS`）的部署形态也依赖这一维——
 * 定义里写 `feishu:message` 共性，宿主装配时把 env 群注入订阅面。
 */

import type { PersonaEvent } from "./event"

/** 一条订阅。 */
export type Subscription = string

/** 事件的匹配坐标（只取订阅关心的部分）。 */
export interface EventCoords {
  kind: string
  channel: string
  /** 群/会话 id（如飞书 chat_id；无则空串）。三段式订阅的第三维。 */
  chatId?: string
}

/**
 * 单条订阅是否命中。
 *
 * 允许的分隔符是 `:`（channel:kind / channel:chatId:kind，顺序无关）。
 */
export function matchesSubscription(sub: Subscription, coords: EventCoords): boolean {
  const s = sub.trim().toLowerCase()
  if (!s) return false
  if (s === "*") return true
  const parts = s.split(":").map((p) => p.trim()).filter(Boolean)
  const kind = coords.kind.toLowerCase()
  const channel = coords.channel.toLowerCase()
  const chatId = (coords.chatId ?? "").toLowerCase()
  if (parts.length === 1) {
    // 单段：kind 或 channel 任一命中即算关注
    return parts[0] === kind || parts[0] === channel
  }
  if (parts.length === 2) {
    // 双段：且关系，顺序无关（两段各命中 kind/channel）
    const [a, b] = parts
    return (a === kind && b === channel) || (a === channel && b === kind)
  }
  if (parts.length === 3) {
    // 三段：channel + chatId + kind 且关系，段序无关——
    // 三个段分别等于 kind/channel/chatId 的某个排列（chatId 非空才可能命中）
    if (!chatId) return false
    const [a, b, c] = parts
    const perms: Array<[string, string, string]> = [
      [channel, chatId, kind],
      [channel, kind, chatId],
      [chatId, channel, kind],
      [chatId, kind, channel],
      [kind, channel, chatId],
      [kind, chatId, channel],
    ]
    return perms.some(([x, y, z]) => a === x && b === y && c === z)
  }
  return false
}

/** 任一订阅命中即关注（无人格级"排除"语义——排除用 filters）。 */
export function matchesAny(subs: readonly Subscription[] | undefined, coords: EventCoords): boolean {
  if (!subs || subs.length === 0) return false
  for (const s of subs) {
    if (matchesSubscription(s, coords)) return true
  }
  return false
}

/** 事件的匹配坐标（从 `PersonaEvent` 取）。 */
export function coordsOf(event: Pick<PersonaEvent, "kind" | "channel" | "context">): EventCoords {
  return { kind: event.kind, channel: event.channel, chatId: event.context?.refs?.chatId ?? "" }
}

/**
 * 校验订阅写法（人格定义加载期用；非法即报错，不静默忽略）。
 *
 * 返回错误文案，合法返回 null。规则：`*` 或 1~3 段的 `[a-z0-9_]+`。
 */
export function validateSubscription(sub: unknown): string | null {
  if (typeof sub !== "string") return `订阅必须是字符串（当前：${typeof sub}）`
  const s = sub.trim()
  if (!s) return "订阅不能为空"
  if (s === "*") return null
  const parts = s.split(":")
  if (parts.length > 3) return `订阅「${s}」段数过多（最多 channel:chatId:kind 三段）`
  for (const p of parts) {
    if (!/^[a-z0-9_]+$/.test(p.trim())) {
      return `订阅「${s}」含非法字符（每段须为 [a-z0-9_]+；不支持通配符与正则）`
    }
  }
  return null
}
