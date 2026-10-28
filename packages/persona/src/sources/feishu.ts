/**
 * 内置事件源：**飞书消息**（应用身份收、用户身份读）。
 *
 * ## 与渠道（`@gebai/channels` 的 feishu）的分工
 *
 * 两者都能收飞书消息，但能力面不同——**不是重复实现**：
 *
 * | | 渠道 | 本源 |
 * |---|---|---|
 * | 收消息 | 应用身份长连接 | 应用身份长连接（复用宿主连接，不建第二条） |
 * | **读群历史** | 不支持 | **用户身份回看**（机器人不在的群也能读） |
 * | 出站 | `act(verb:"send"/"reply")` | 不提供（出站一律走渠道） |
 * | 配置 | `GEBAI_CHANNELS_FEISHU_*`（独立机器人） | `FEISHU_BOT_*`（回落到 `GEBAI_FEISHU_*`） |
 * | 定位 | 出站主路径 + 入站 | **入站增强**：带上下文的消息 |
 *
 * 所以启用本源的价值就一句话：**让模型看到「这条消息之前发生了什么」**。
 * 群里有人说「还是不行」，只有一条正文时模型只能猜；带上前 5 条历史就能答。
 *
 * ## 三条协议事实（决定了形态，不是设计选择）
 *
 * 1. **收消息只能应用身份**：长连接端点只接受 AppID + AppSecret，没有用户 token 入口。
 * 2. **读历史必须用户身份**：用户能读自己参与的所有群（含机器人未加入的）；应用身份只能读
 *    机器人所在群。这就是「有了渠道还需要本源」的全部理由。
 * 3. **同一应用只能一条长连接**：所以本源**不自己建连**，经 `ctx.deps.feishuLongConn` 拿
 *    宿主已有的连接注册面。自己建 = 与应用互踢 = 两边都偶发收不到消息（最难排查的一类）。
 *
 * ## 三条硬约束（与 `docs/event-sources.md` 一致）
 *
 * - `start` 立即 resolve（长驻连接由宿主持有，本源只注册回调）
 * - 状态全在 `start` 闭包里（多实例/热加载/测试并行都不串扰）
 * - 幂等键用飞书消息 id（各源重投是常态）
 */

import type { EventSourceContext, EventSourceDef, IngressEvent } from "../source"

/** 宿主注入的长连接工厂形状（与 channels 侧同口径，不 import 宿主包）。 */
type LongConnFactory = (opts: {
  appId: string
  appSecret: string
  onEvent: (ev: Record<string, unknown>) => void
}) => { start(): Promise<void>; stop(): void }

/** 宿主注入的「按用户身份读历史」能力（拿不到就如实降级，不静默）。 */
type HistoryReader = (input: {
  chatId: string
  limit: number
  /** 用于取用户令牌的上下文（宿主决定怎么定位那个用户）。 */
}) => Promise<{ messages: Array<{ senderName?: string; text: string; createTime?: number }>; readIdentity: "user" | "app"; fallbackReason?: string }>

/** 取一条消息的正文（只 text 类型取 content.text，其余降级为说明）。 */
function messageText(message: Record<string, unknown>): string {
  const type = String(message.message_type ?? "text")
  const raw = String(message.content ?? "")
  if (type !== "text") {
    // 不按 text 处理：图片/文件消息的 content 里可能残留 text 字段，
    // 当成正文会让模型以为看到了完整内容
    return `（${type === "image" ? "图片" : type === "file" ? "文件" : type === "post" ? "富文本" : type}消息，正文未展开；message_id=${String(message.message_id ?? "")}）`
  }
  try {
    const parsed = JSON.parse(raw) as { text?: string }
    return (parsed.text ?? "").trim()
  } catch {
    return raw.trim()
  }
}

/** 从长连接事件里抽消息（非消息事件返回 null）。 */
export function extractMessage(ev: Record<string, unknown>): {
  messageId: string
  chatId: string
  chatType: string
  senderId?: string
  senderType?: string
  text: string
  createTime?: number
} | null {
  const header = (ev.header ?? {}) as { event_type?: string; create_time?: string }
  if (header.event_type !== "im.message.receive_v1") return null
  const event = (ev.event ?? {}) as Record<string, unknown>
  const message = (event.message ?? {}) as Record<string, unknown>
  const messageId = String(message.message_id ?? "")
  const chatId = String(message.chat_id ?? "")
  // 缺 message_id/chat_id 就不能投：幂等键与出站目标都靠它，宁可丢也不乱投
  if (!messageId || !chatId) return null
  const sender = (event.sender ?? {}) as { sender_id?: { open_id?: string }; sender_type?: string }
  const createTime = Number(header.create_time ?? message.create_time ?? 0) || undefined
  return {
    messageId,
    chatId,
    chatType: String(message.chat_type ?? ""),
    ...(sender.sender_id?.open_id ? { senderId: sender.sender_id.open_id } : {}),
    ...(sender.sender_type ? { senderType: sender.sender_type } : {}),
    text: messageText(message),
    ...(createTime ? { createTime } : {}),
  }
}

/** 把历史拼成一段可读上下文（放进 `body` 尾部，而不是另设字段——模型只能看 `body`）。 */
export function renderHistory(
  history: Array<{ senderName?: string; text: string; createTime?: number }>,
  readIdentity: "user" | "app",
  fallbackReason?: string,
): string {
  if (history.length === 0) return ""
  const lines = history
    .filter((h) => h.text.trim())
    .map((h) => {
      const who = h.senderName?.trim() || "某人"
      const when = h.createTime ? ` · ${new Date(h.createTime).toLocaleString("zh-CN")}` : ""
      return `- ${who}${when}：${h.text.replace(/\s+/g, " ").slice(0, 300)}`
    })
  if (lines.length === 0) return ""
  const head = readIdentity === "user" ? "【这段对话之前的内容】" : `【这段对话之前的内容（以应用身份读取，可能不全${fallbackReason ? `：${fallbackReason}` : ""}）】`
  return `\n\n${head}\n${lines.join("\n")}`
}

/** 清理器栈（每次 start 压一个闭包；`stop()` 无参数，弹最近一个）——见 docs/event-sources.md §5。 */
const cleanups: Array<() => void> = []

export const source: EventSourceDef = {
  name: "feishu",
  description:
    "飞书消息（带上下文）：应用身份收、用户身份回看历史——让模型看到「这条消息之前发生了什么」。需要宿主注入长连接（deps.feishuLongConn）",
  channel: "feishu",
  envVars: [
    { name: "FEISHU_BOT_APP_ID", description: "飞书应用 App ID（缺省回落 GEBAI_FEISHU_APP_ID）" },
    { name: "FEISHU_BOT_APP_SECRET", description: "飞书应用 App Secret（缺省回落 GEBAI_FEISHU_APP_SECRET）", secret: true },
    {
      name: "GEBAI_PERSONA_FEISHU_CHATS",
      description: "仅处理这些群（逗号分隔的 chat_id；空 = 全部）。入站不做写操作，因此缺省不限群",
    },
    {
      name: "GEBAI_PERSONA_FEISHU_HISTORY",
      description: "回看历史条数（默认 0 = 不回查，只投正文）。这是本源相对渠道渠道的全部附加价值",
    },
    {
      name: "GEBAI_PERSONA_FEISHU_USE_USER",
      description: "读历史时优先用用户身份（默认 on）。off = 一律应用身份（只能读机器人所在群）",
    },
  ],

  async start(ctx: EventSourceContext): Promise<void> {
    const appId = ctx.env("FEISHU_BOT_APP_ID") || ctx.env("GEBAI_FEISHU_APP_ID")
    const appSecret = ctx.env("FEISHU_BOT_APP_SECRET") || ctx.env("GEBAI_FEISHU_APP_SECRET")
    if (!appId || !appSecret) {
      // 抛错 = 本源不可用（记 loadErrors + 告警，不阻断其它源）
      throw new Error(
        "缺少 FEISHU_BOT_APP_ID / FEISHU_BOT_APP_SECRET（也接受 GEBAI_FEISHU_*）。" +
          "该源未启动——其它事件源不受影响。",
      )
    }

    // 状态全在闭包（多实例/热加载/测试并行互不覆盖）
    let stopped = false
    let conn: { start(): Promise<void>; stop(): void } | null = null

    const factory = ctx.deps.feishuLongConn as LongConnFactory | undefined
    if (typeof factory !== "function") {
      // 如实失败而不是静默降级：静默时现象是「配了但收不到消息」，最难排查
      throw new Error(
        "未注入长连接实现（deps.feishuLongConn）——无法订阅飞书消息。\n" +
          "宿主应把已有的飞书长连接以工厂形态注入（本源**不自己建连**：同一飞书应用只能一条长连接，" +
          "两套各自连会互踢，表现为两边都偶发收不到消息）。",
      )
    }

    const historyLimit = Math.max(0, Math.min(50, Number(ctx.env("GEBAI_PERSONA_FEISHU_HISTORY") ?? "0") || 0))
    const useUser = !/^(off|0|false|no)$/i.test(ctx.env("GEBAI_PERSONA_FEISHU_USE_USER") ?? "")
    const allowed = new Set(
      (ctx.env("GEBAI_PERSONA_FEISHU_CHATS") ?? "")
        .split(/[,\s]+/)
        .map((s) => s.trim())
        .filter(Boolean),
    )
    const reader = ctx.deps.feishuHistory as HistoryReader | undefined
    if (historyLimit > 0 && typeof reader !== "function") {
      // 要历史但宿主没给读能力：不阻断（正文仍有价值），但必须说清楚
      ctx.log(
        "warn",
        `GEBAI_PERSONA_FEISHU_HISTORY=${historyLimit} 但宿主未注入 deps.feishuHistory（读历史能力）——` +
          "本条源将只投正文，不带上下文",
      )
    }
    if (!allowed.size) ctx.log("info", "未配置 GEBAI_PERSONA_FEISHU_CHATS：入站不限群")

    conn = factory({
      appId,
      appSecret,
      onEvent: (ev) => {
        if (stopped) return
        const msg = extractMessage(ev)
        if (!msg) return
        // 自发送消息记账（宿主注入，与渠道共享同一实例）：自己发出/回复的消息
        // （含 as_user 名义，sender_type=user 拦不住）按 message_id 精确过滤，防回灌
        const sentMessages = ctx.deps.sentMessages as { hit(id: string): boolean } | undefined
        if (sentMessages?.hit(msg.messageId)) {
          ctx.log("info", `忽略自发送消息 ${msg.messageId}（防回灌）`)
          return
        }
        // 防自触发循环：机器人自己发的消息若被当输入，会无限自我对话
        if (msg.senderType === "app") return
        if (allowed.size && !allowed.has(msg.chatId)) return
        if (!msg.text) {
          ctx.log("info", `忽略空正文消息 ${msg.messageId}（撤回/系统消息）`)
          return
        }
        // 回看历史是**附加**能力：失败不影响本条投递（只是没有上下文）
        void (async () => {
          let history = ""
          if (historyLimit > 0 && typeof reader === "function") {
            try {
              const h = await reader({ chatId: msg.chatId, limit: historyLimit })
              if (h.readIdentity === "app" && useUser) {
                // 静默降级会让人以为「这个群就是没历史」——如实说出来
                ctx.log("warn", `回退到应用身份读历史：${h.fallbackReason ?? "用户授权不可用"}`)
              }
              history = renderHistory(h.messages, h.readIdentity, h.fallbackReason)
            } catch (err) {
              ctx.log("warn", `读历史失败（本条仍投递，只是不带上下文）: ${String((err as Error)?.message || err)}`)
            }
          }
          // 发送者姓名（尽力而为）：优先群成员表（宿主注入了 chatId 上下文），contact 兜底。
          // 查不到（无权限/接口失败）时 body 兜底标「用户」——模型至少知道这条是别人发的。
          const senderName =
            msg.senderId && typeof ctx.deps.feishuUserName === "function"
              ? await (ctx.deps.feishuUserName as (id: string, chatId?: string) => Promise<string | undefined>)(msg.senderId, msg.chatId)
              : undefined
          const who = senderName?.trim() ? `${senderName}：` : "用户："
          const isGroup = msg.chatType !== "p2p"
          const ingress: IngressEvent = {
            kind: "message",
            channel: "feishu",
            source: isGroup ? `飞书群 ${msg.chatId}` : "飞书单聊",
            summary: `${senderName ?? msg.senderId ?? "用户"}：${msg.text.slice(0, 60)}`,
            body: who + msg.text + history,
            // 幂等键 = 飞书消息 id（它会重投同一消息）
            id: `feishu:${msg.messageId}`,
            ...(msg.createTime ? { at: msg.createTime } : {}),
            refs: {
              chatId: msg.chatId,
              messageId: msg.messageId,
              ...(msg.senderId ? { senderId: msg.senderId } : {}),
              ...(senderName ? { senderName } : { senderName: "用户" }),
              ...(history ? { extra: { historyCount: String(historyLimit), readIdentity: history ? (useUser ? "user" : "app") : "none" } } : {}),
            },
          }
          const r = await ctx.emit(ingress)
          if (r.status !== "accepted") {
            ctx.log("warn", `消息 ${msg.messageId} 未被接管（${r.status}${"reason" in r && r.reason ? `：${r.reason}` : ""}）`)
          }
        })().catch((err) => ctx.log("warn", `投递 ${msg.messageId} 失败: ${String((err as Error)?.message || err)}`))
      },
    })

    cleanups.push(() => {
      stopped = true
      try {
        conn?.stop()
      } catch {
        /* 停止异常只影响本实例，无须上抛 */
      }
      conn = null
    })

    await conn.start()
    ctx.log("info", `飞书消息源已启动（历史回看 ${historyLimit} 条${historyLimit > 0 ? `，用户身份优先=${useUser}` : ""}）`)
  },

  async stop(): Promise<void> {
    cleanups.pop()?.()
  },
}
