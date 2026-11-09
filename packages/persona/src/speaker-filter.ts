/**
 * 「这条消息是谁发的」——决定**要不要让它触发人格回复**。
 *
 * ## 立项前提：进入上下文 ≠ 触发执行
 *
 * 早期实现把这两件事合成了一件：判定为「不该回」就直接 `return`，消息**整条消失**。
 * 后果是上下文出现缺口——人格看不到用户本人说过的话，也看不到自己刚代发过什么，
 * 于是「它不知道群里刚才发生了什么」。而它后面的每一次判断都建立在残缺的上下文上。
 *
 * 现在拆成两个独立决策：
 *
 * 1. **record**：是否写入会话上下文。**一律为 true**（本模块不再有否决权）——
 *    群里的每一条消息都是事实，事实不进上下文，后面所有推理都不可靠。
 * 2. **deliver**：是否拿这条去触发一轮模型执行。
 *
 * 一句话：**全部录入上下文，但不是每条都值得跑一轮。**
 *
 * ## 三种发送者，处置不同
 *
 * | 发送者 | 判定依据 | record | deliver | 为什么 |
 * |---|---|---|---|---|
 * | 人格自己代发的回灌 | 发送记录命中 `message_id` | ✅ | ❌ | 那是它自己刚说的话，不是新指令 |
 * | 用户本人 | `senderId === 用户 openId` | ✅ | ✅ | 本人当场发言，就是要它做事 |
 * | 其他人 | 以上都不是 | ✅ | ✅ | 正常对话 |
 *
 * ## 为什么「用户本人」从默认不回复改成默认回复
 *
 * 旧口径是调试友好：人格以用户名义发言，所以「用户发的话」与「人格代发的话」在群里
 * 是同一个人的头像；调试时每条自己的消息都被回复，群里会变成自我对话。
 *
 * 但那个口径有个更重的代价：**用户在群里说「帮我查一下 X」，人格根本不动**。
 * 而这恰恰是最主要的使用方式。真正需要防的是「自问自答」（自己代发的被当成新指令），
 * 那一类由 `isSelf` 精确挡住（按 message_id 匹配，不是按人匹配）——
 * 用户亲手打的字与它自己发的，靠发送记录分得开，不必牺牲「用户说话它就干活」。
 *
 * ## 为什么区分靠 `message_id` 而不靠 `sender_type`
 *
 * 人格以 `as_user` 发言时，飞书事件里 `sender_type=user`、`open_id` 与用户本人
 * **完全相同**——字段上没有任何区别。唯一可靠的依据是「这条 message_id 是不是
 * 我自己刚发出去的」（`SentMessageRegistry`，落盘 7 天）。
 *
 * ## 为什么不静默丢弃
 *
 * 不触发执行的消息也会写进上下文，且带一行身份说明（见 `speakerNote`）。
 * 模型读到「这是你自己刚发出去的」与「这是张三说的话」，才不会把自己昨天的话
 * 当成用户的新反馈——**它需要知道来历，而不是被蒙在鼓里**。
 */

/** 发送者判定结果。 */
export type SpeakerKind = "self" | "owner" | "other"

export interface SpeakerVerdict {
  kind: SpeakerKind
  /**
   * 是否写入会话上下文。
   *
   * 恒为 `true`：本模块**不再否决任何消息的录入**。保留这个字段是为了让调用点
   * 读起来是「两个决策」而不是「一个决策」，也为了将来若有真需要排除的东西
   * （如系统撤回通知）有地方表达——但当前的每一种发送者都必须进上下文。
   */
  record: boolean
  /** 是否触发一轮模型执行（false = 只记上下文，不跑）。 */
  deliver: boolean
  /** 不触发执行的原因（日志用，必须能让人一眼看懂为什么它没回）。 */
  reason?: string
}

export interface SpeakerInput {
  /** 这条消息的发送者 open_id（事件缺 sender 时为空）。 */
  senderId?: string | undefined
  /** 用户本人（人格扮演者）的飞书 open_id；拿不到时无法判定 owner。 */
  ownerOpenId?: string | undefined
  /** 这条消息是否命中发送记录（= 人格自己发出去的）。由调用方查。 */
  isSelf: boolean
  /**
   * 是否回复用户本人发的消息（`GEBAI_PERSONA_REPLY_TO_OWNER`）。
   *
   * 缺省从 false 改为 **true**：用户亲手说话就该它干活。想回到「自己的话不吵自己」
   * 的老行为时设 false（此时消息仍会记入上下文，只是不触发执行）。
   */
  replyToOwner: boolean
}

/**
 * 判定一条入站消息：该记入上下文吗（总是记）、该触发执行吗。
 *
 * 纯函数：所有外部依赖（发送记录、open_id）都由调用方查好后传进来，
 * 于是这段逻辑能被单测穷举，不需要假长连接与假令牌。
 */
export function classifySpeaker(input: SpeakerInput): SpeakerVerdict {
  // ① 自己代发的回灌：记下来（它得知道群里现在有自己这段话），但不触发执行。
  // 这不是可配置项——把自己刚说的话当成新指令重答一遍，是**确定错误**而不只是噪音。
  if (input.isSelf) {
    return {
      kind: "self",
      record: true,
      deliver: false,
      reason: "人格自己以用户名义发出的消息回灌（记入上下文，不重复作答）",
    }
  }

  // ② 用户本人：按配置决定是否触发执行（缺省 true）。
  //
  // 两个「空」都不能当成匹配：
  // - ownerOpenId 为空（拿不到令牌）→ 无法判定，按 other 处理。
  //   猜成 owner 会让所有消息走错分支。
  // - senderId 为空（事件缺 sender）→ 也不匹配。否则缺 sender 的消息
  //   全被当成「用户自己」。
  const owner = input.ownerOpenId?.trim()
  const sender = input.senderId?.trim()
  if (owner && sender && owner === sender) {
    return input.replyToOwner
      ? { kind: "owner", record: true, deliver: true }
      : {
          kind: "owner",
          record: true,
          deliver: false,
          reason: "用户本人发的消息（GEBAI_PERSONA_REPLY_TO_OWNER=false，只记上下文不回复）",
        }
  }

  return { kind: "other", record: true, deliver: true }
}

/** 从环境读「是否回复用户本人」。缺省 **true**（用户说话就该它干活）。 */
export function replyToOwnerFromEnv(env: Record<string, string | undefined>): boolean {
  const raw = (env.GEBAI_PERSONA_REPLY_TO_OWNER ?? "").trim().toLowerCase()
  // 未配置 = 开。显式写 false 才关。
  if (!raw) return true
  return !/^(0|false|off|no)$/i.test(raw)
}

/**
 * 写进正文的身份说明（**这条消息是什么来历**）。
 *
 * 为何必须写进去而不只是记在日志里：模型看到一段「以用户名义说的话」时，
 * 无法自行分辨那是用户本人打的、还是人格自己上一轮代发的。而这两种情况的
 * 正确反应完全相反——前者是要处理的输入，后者是自己的输出。
 * 一句话交代清楚，比在提示词里写十条通用告诫都管用。
 *
 * 返回空串表示「无需特别说明」（普通成员的消息，姓名前缀已经够）。
 */
export function speakerNote(kind: SpeakerKind, senderLabel?: string): string {
  const who = senderLabel?.trim() || "用户本人"
  if (kind === "self") {
    return `（这是**你自己**刚才以 ${who} 的名义发出去的话，现在从群里回灌进来。它是你的输出，不是用户的新指令：不要重复作答、不要把它当成待办。留在这里是为了让你知道群里此刻有什么。）`
  }
  if (kind === "owner") {
    return `（这是 ${who} **本人亲手打的**，不是你自己发出去的话。它是用户的当场发言：按最高优先级处理；若与你正在做的事冲突，以它为准）`
  }
  return ""
}
