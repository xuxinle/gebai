/**
 * 「这条消息是谁发的」——决定要不要让这个事件进入人格。
 *
 * ## 为什么需要单独一层（而不在事件源里写个 if）
 *
 * 该判定有两个**独立的**数据来源（用户身份 open_id / 人格自己的发送记录），
 * 且两条入站路径（渠道长连接、人格事件源）都必须用同一套口径。判定散在
 * 各处的后果是：改了一处、另一处行为不一致，而现象是「有时回有时不回」——
 * 最难排查的一类。
 *
 * ## 三种发送者，处置不同
 *
 * | 发送者 | 判定依据 | 默认 |
 * |---|---|---|
 * | 人格自己发的（as_user 回灌） | 发送记录命中 `message_id` | **永不回复**（那是它自己的话） |
 * | 用户本人 | `senderId === 用户 openId` | **默认不回复**（调试口径，可配置开启） |
 * | 其他人 | 以上都不是 | 回复 |
 *
 * ## 为什么「用户自己的消息」默认不回复
 *
 * 人格以用户本人名义发言，所以「用户发的话」与「人格代发的话」在群里看起来
 * 是同一个人说的。调试时若每条自己的消息都被回复，会在群里形成自我对话噪音。
 * 默认不回，需要时用 `GEBAI_PERSONA_REPLY_TO_OWNER=true` 开。
 *
 * 注意这与「防回灌」不是一回事：防回灌挡的是**人格代发**的消息（按 message_id
 * 精确匹配）；这里还挡**用户亲手打的**消息（按 senderId 匹配）。
 * 两者缺一：只防回灌的话，用户随口说一句也会被当成待办任务。
 */

/** 发送者判定结果。 */
export type SpeakerKind = "self" | "owner" | "other"

export interface SpeakerVerdict {
  kind: SpeakerKind
  /** 该事件应否进入人格（false = 直接丢弃）。 */
  deliver: boolean
  /** 丢弃原因（日志用，必须能让人一眼看懂为什么没回）。 */
  reason?: string
}

export interface SpeakerInput {
  /** 这条消息的发送者 open_id（事件缺 sender 时为空）。 */
  senderId?: string | undefined
  /** 用户本人（人格扮演者）的飞书 open_id；拿不到时无法判定 owner。 */
  ownerOpenId?: string | undefined
  /** 这条消息是否命中发送记录（= 人格自己发出去的）。由调用方查。 */
  isSelf: boolean
  /** 是否回复用户本人发的消息（`GEBAI_PERSONA_REPLY_TO_OWNER`，缺省 false）。 */
  replyToOwner: boolean
}

/**
 * 判定一条入站消息该不该交给人格。
 *
 * 纯函数：所有外部依赖（发送记录、open_id）都由调用方查好后传进来，
 * 于是这段逻辑能被单测穷举，不需要假长连接与假令牌。
 */
export function classifySpeaker(input: SpeakerInput): SpeakerVerdict {
  // ① 自己发的（as_user 回灌）：永远丢弃。
  // 这不是可配置项——把自己的话当成新指令，是**确定错误**而不只是噪音。
  if (input.isSelf) return { kind: "self", deliver: false, reason: "这是人格自己发出的消息（防回灌）" }

  // ② 用户本人：按配置。
  //
  // 两个「空」都不能当成匹配：
  // - ownerOpenId 为空（拿不到令牌）→ 无法判定，按 other 处理。
  //   猜成 owner 会让所有消息静默消失。
  // - senderId 为空（事件缺 sender）→ 也不匹配。否则缺 sender 的消息
  //   全被当成「用户自己」而集体丢掉。
  const owner = input.ownerOpenId?.trim()
  const sender = input.senderId?.trim()
  if (owner && sender && owner === sender) {
    return input.replyToOwner
      ? { kind: "owner", deliver: true }
      : {
          kind: "owner",
          deliver: false,
          reason: "用户本人发的消息（GEBAI_PERSONA_REPLY_TO_OWNER 未开启，默认不回复自己的话）",
        }
  }

  return { kind: "other", deliver: true }
}

/** 从环境读「是否回复用户本人」。缺省 false（调试友好）。 */
export function replyToOwnerFromEnv(env: Record<string, string | undefined>): boolean {
  const raw = (env.GEBAI_PERSONA_REPLY_TO_OWNER ?? "").trim().toLowerCase()
  if (!raw) return false
  return /^(1|true|on|yes)$/i.test(raw)
}
